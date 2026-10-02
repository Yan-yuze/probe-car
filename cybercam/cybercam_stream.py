#!/usr/bin/env python3
"""CyberCAM camera -> direct MJPEG over the PC hotspot (no relay board)."""

from __future__ import annotations

import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import logging
import signal
import socket
import threading
import time

import cv2
from walnutpi import Sensor


LOG = logging.getLogger("cybercam_stream")


class LatestJpeg:
    def __init__(self) -> None:
        self.condition = threading.Condition()
        self.jpeg: bytes | None = None
        self.sequence = 0
        self.last_at = 0.0

    def publish(self, jpeg: bytes) -> None:
        with self.condition:
            self.jpeg = jpeg
            self.sequence += 1
            self.last_at = time.monotonic()
            self.condition.notify_all()

    def snapshot(self) -> tuple[int, bytes | None, int]:
        with self.condition:
            age_ms = -1 if self.last_at == 0 else int((time.monotonic() - self.last_at) * 1000)
            return self.sequence, self.jpeg, age_ms

    def wait_after(self, sequence: int, stop: threading.Event) -> tuple[int, bytes | None]:
        with self.condition:
            if self.sequence <= sequence and not stop.is_set():
                self.condition.wait(timeout=1.0)
            return self.sequence, self.jpeg


def handler_factory(frames: LatestJpeg, stop: threading.Event) -> type[BaseHTTPRequestHandler]:
    class Handler(BaseHTTPRequestHandler):
        server_version = "CyberCAMDirect/1.0"

        def do_GET(self) -> None:
            path = self.path.split("?", 1)[0]
            if path == "/health":
                sequence, _, age_ms = frames.snapshot()
                body = json.dumps({
                    "ok": sequence > 0 and 0 <= age_ms < 2000,
                    "frames": sequence,
                    "age_ms": age_ms,
                }).encode("ascii")
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Cache-Control", "no-store")
                self.end_headers()
                self.wfile.write(body)
                return
            if path != "/stream.mjpg":
                self.send_error(404)
                return

            try:
                self.connection.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 32 * 1024)
                self.connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                self.connection.settimeout(1.5)
                self.send_response(200)
                self.send_header("Content-Type", "multipart/x-mixed-replace; boundary=frame")
                self.send_header("Cache-Control", "no-store")
                self.end_headers()
                sequence = -1
                while not stop.is_set():
                    newer, jpeg = frames.wait_after(sequence, stop)
                    if jpeg is None or newer == sequence:
                        continue
                    sequence = newer
                    header = (
                        b"--frame\r\nContent-Type: image/jpeg\r\n"
                        + f"Content-Length: {len(jpeg)}\r\n\r\n".encode("ascii")
                    )
                    self.wfile.write(header + jpeg + b"\r\n")
            except (BrokenPipeError, ConnectionResetError, OSError):
                pass

        def log_message(self, format: str, *args: object) -> None:
            LOG.debug(format, *args)

    return Handler


def main() -> None:
    parser = argparse.ArgumentParser(description="CyberCAM direct MJPEG streamer")
    parser.add_argument("--http-host", default="0.0.0.0")
    parser.add_argument("--http-port", type=int, default=8081)
    parser.add_argument("--width", type=int, default=320)
    parser.add_argument("--height", type=int, default=240)
    parser.add_argument("--fps", type=float, default=8.0)
    parser.add_argument("--jpeg-quality", type=int, default=50)
    args = parser.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")

    stop = threading.Event()
    signal.signal(signal.SIGINT, lambda _sig, _frame: stop.set())
    signal.signal(signal.SIGTERM, lambda _sig, _frame: stop.set())
    camera = Sensor.Sensor(args.width, args.height)
    if not camera.isOpened():
        raise RuntimeError("CyberCAM camera failed to open")

    frames = LatestJpeg()
    server = ThreadingHTTPServer((args.http_host, args.http_port), handler_factory(frames, stop))
    server.daemon_threads = True
    server_thread = threading.Thread(target=server.serve_forever, name="mjpeg-http", daemon=True)
    server_thread.start()
    LOG.info("stream: http://%s:%d/stream.mjpg", args.http_host, args.http_port)

    period = 1.0 / max(1.0, args.fps)
    next_frame = time.monotonic()
    reported_size = False
    try:
        for _ in range(10):
            camera.read()
        while not stop.is_set():
            ok, image = camera.read()
            if not ok or image is None:
                stop.wait(0.05)
                continue
            if not reported_size:
                LOG.info("camera frame: %dx%d", image.shape[1], image.shape[0])
                reported_size = True
            if image.shape[1] != args.width or image.shape[0] != args.height:
                image = cv2.resize(image, (args.width, args.height), interpolation=cv2.INTER_AREA)
            encoded_ok, encoded = cv2.imencode(
                ".jpg", image, [cv2.IMWRITE_JPEG_QUALITY, args.jpeg_quality]
            )
            if encoded_ok:
                frames.publish(encoded.tobytes())
            next_frame += period
            remaining = next_frame - time.monotonic()
            if remaining > 0:
                stop.wait(remaining)
            else:
                next_frame = time.monotonic()
    finally:
        stop.set()
        with frames.condition:
            frames.condition.notify_all()
        server.shutdown()
        server.server_close()
        server_thread.join(timeout=2)
        release = getattr(camera, "release", None)
        if callable(release):
            release()


if __name__ == "__main__":
    main()
