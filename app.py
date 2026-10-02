#!/usr/bin/env python3
"""Serve the CyberCAM viewer and forward car commands to the existing ESP32 API."""

from __future__ import annotations

import argparse
import json
import threading
from dataclasses import dataclass
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.error import HTTPError, URLError
from urllib.parse import urlencode, urlsplit, urlunsplit
from urllib.request import Request, urlopen


WEB_DIR = Path(__file__).resolve().parent / "web"
COMMANDS = {"FORWARD", "BACKWARD", "LEFT", "RIGHT", "STOP"}
SERVO_MIN_ANGLE = 30
SERVO_MAX_ANGLE = 150


@dataclass(frozen=True)
class Config:
    car_url: str
    video_url: str
    video_health_url: str


def normalize_url(value: str) -> str:
    parts = urlsplit(value.strip())
    if parts.scheme != "http" or not parts.hostname or parts.username or parts.password:
        raise ValueError("设备地址必须是 http://主机[:端口] 格式")
    return urlunsplit((parts.scheme, parts.netloc, parts.path, parts.query, ""))


def make_config(car_url: str, video_url: str) -> Config:
    car = normalize_url(car_url).rstrip("/")
    car_parts = urlsplit(car)
    if car_parts.path or car_parts.query:
        raise ValueError("ESP32 控制地址请只填写 http://主机[:端口]")
    video = normalize_url(video_url)
    parts = urlsplit(video)
    health = urlunsplit((parts.scheme, parts.netloc, "/health", "", ""))
    return Config(car_url=car, video_url=video, video_health_url=health)


def fetch_json(url: str, timeout: float = 1.0) -> dict:
    request = Request(url, headers={"Cache-Control": "no-cache"})
    with urlopen(request, timeout=timeout) as response:
        data = response.read(64 * 1024)
    result = json.loads(data)
    if not isinstance(result, dict):
        raise ValueError("设备返回的 JSON 不是对象")
    return result


def handler_factory(config: Config) -> type[BaseHTTPRequestHandler]:
    control_lock = threading.Lock()
    last_sequence: dict[str, int] = {}
    servo_lock = threading.Lock()
    last_servo_sequence: dict[str, int] = {}

    class Handler(BaseHTTPRequestHandler):
        server_version = "CyberCAMCarUI/1.0"

        def send_bytes(self, status: int, body: bytes, content_type: str) -> None:
            try:
                self.send_response(status)
                self.send_header("Content-Type", content_type)
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Cache-Control", "no-store")
                self.send_header("X-Content-Type-Options", "nosniff")
                self.end_headers()
                self.wfile.write(body)
            except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
                pass  # The browser closed the request while the device was unreachable.

        def send_json(self, status: int, value: dict) -> None:
            self.send_bytes(
                status,
                json.dumps(value, ensure_ascii=False).encode("utf-8"),
                "application/json; charset=utf-8",
            )

        def do_GET(self) -> None:
            path = self.path.split("?", 1)[0]
            static = {
                "/": ("index.html", "text/html; charset=utf-8"),
                "/app.js": ("app.js", "text/javascript; charset=utf-8"),
                "/style.css": ("style.css", "text/css; charset=utf-8"),
            }
            if path in static:
                filename, content_type = static[path]
                self.send_bytes(200, (WEB_DIR / filename).read_bytes(), content_type)
            elif path == "/api/status":
                self.handle_status()
            elif path == "/video.mjpg":
                self.handle_video()
            else:
                self.send_json(404, {"ok": False, "error": "未找到页面"})

        def handle_status(self) -> None:
            result: dict = {"car": {"ok": False}, "video": {"ok": False}}
            try:
                result["car"] = {"ok": True, **fetch_json(config.car_url + "/api/status")}
            except (HTTPError, URLError, TimeoutError, OSError, ValueError, json.JSONDecodeError) as exc:
                result["car"]["error"] = str(exc)
            try:
                health = fetch_json(config.video_health_url)
                age_ms = health.get("age_ms")
                is_fresh = isinstance(age_ms, (int, float)) and 0 <= age_ms < 2000
                # CyberCAM's own /health uses ok; the older USB relay uses usb_connected.
                source_ok = health.get("usb_connected", health.get("ok", False))
                result["video"] = {
                    **health,
                    "ok": bool(source_ok and is_fresh),
                }
            except (HTTPError, URLError, TimeoutError, OSError, ValueError, json.JSONDecodeError) as exc:
                result["video"]["error"] = str(exc)
            self.send_json(200, result)

        def handle_video(self) -> None:
            headers_sent = False
            try:
                request = Request(config.video_url, headers={"Cache-Control": "no-cache"})
                with urlopen(request, timeout=3.0) as source:
                    content_type = source.headers.get("Content-Type", "")
                    if not content_type.lower().startswith("multipart/x-mixed-replace"):
                        self.send_json(502, {"ok": False, "error": "视频源不是 MJPEG 流"})
                        return
                    self.send_response(200)
                    self.send_header("Content-Type", content_type)
                    self.send_header("Cache-Control", "no-store")
                    self.send_header("X-Content-Type-Options", "nosniff")
                    self.end_headers()
                    headers_sent = True
                    while True:
                        # read1 returns available bytes without waiting for a full 16 KiB.
                        chunk = source.read1(16 * 1024)
                        if not chunk:
                            break
                        self.wfile.write(chunk)
            except (BrokenPipeError, ConnectionResetError):
                pass  # Browser closed this stream.
            except (HTTPError, URLError, TimeoutError, OSError) as exc:
                if not headers_sent:
                    try:
                        self.send_json(502, {"ok": False, "error": str(exc)})
                    except (BrokenPipeError, ConnectionResetError, OSError):
                        pass

        def do_POST(self) -> None:
            if self.path not in {"/api/command", "/api/servo"}:
                self.send_json(404, {"ok": False, "error": "未找到接口"})
                return
            if self.headers.get("Content-Type", "").split(";", 1)[0].strip() != "application/json":
                self.send_json(415, {"ok": False, "error": "请使用 application/json"})
                return
            if self.headers.get("X-Car-UI") != "1":
                self.send_json(403, {"ok": False, "error": "缺少网页控制标记"})
                return
            try:
                size = int(self.headers.get("Content-Length", "0"))
                if not 1 <= size <= 128:
                    raise ValueError("请求长度无效")
                payload = json.loads(self.rfile.read(size))
                if not isinstance(payload, dict):
                    raise ValueError("请求必须是对象")
                is_servo = self.path == "/api/servo"
                command = payload.get("command") if isinstance(payload, dict) else None
                angle = payload.get("angle")
                if is_servo:
                    if type(angle) is not int or not SERVO_MIN_ANGLE <= angle <= SERVO_MAX_ANGLE:
                        raise ValueError("舵机角度无效")
                elif not isinstance(command, str) or command not in COMMANDS:
                    raise ValueError("未知控制命令")
                client_id = payload.get("client_id")
                sequence = payload.get("sequence")
                if not isinstance(client_id, str) or not 1 <= len(client_id) <= 64 or not client_id.isascii():
                    raise ValueError("客户端标识无效")
                if type(sequence) is not int or not 1 <= sequence <= 9007199254740991:
                    raise ValueError("命令序号无效")
            except (ValueError, json.JSONDecodeError):
                self.send_json(400, {"ok": False, "error": "控制命令无效"})
                return

            request = Request(
                config.car_url + self.path,
                data=urlencode({"angle": angle} if is_servo else {"cmd": command}).encode("ascii"),
                headers={"Content-Type": "application/x-www-form-urlencoded"},
                method="POST",
            )
            # Servo requests cannot supersede a late motor STOP request.
            lock = servo_lock if is_servo else control_lock
            sequence_map = last_servo_sequence if is_servo else last_sequence
            with lock:
                if sequence <= sequence_map.get(client_id, 0):
                    self.send_json(200, {"ok": True, "skipped": True})
                    return
                sequence_map[client_id] = sequence
                try:
                    with urlopen(request, timeout=0.8) as response:
                        status = json.loads(response.read(64 * 1024))
                    self.send_json(200, {"ok": True, "car": status})
                except (HTTPError, URLError, TimeoutError, OSError, ValueError, json.JSONDecodeError) as exc:
                    self.send_json(502, {"ok": False, "error": "ESP32 控制失败：" + str(exc)})

        def log_message(self, format: str, *args: object) -> None:
            # Keep video-frame connection churn out of the console.
            if self.path != "/video.mjpg":
                super().log_message(format, *args)

    return Handler


def make_server(host: str, port: int, config: Config) -> ThreadingHTTPServer:
    server = ThreadingHTTPServer((host, port), handler_factory(config))
    server.daemon_threads = True
    return server


def main() -> None:
    parser = argparse.ArgumentParser(description="CyberCAM 图像 + ESP32 小车网页控制台")
    parser.add_argument("--host", default="127.0.0.1", help="网页监听地址")
    parser.add_argument("--port", type=int, default=8765, help="网页监听端口")
    parser.add_argument("--car-url", default="http://192.168.137.127", help="ESP32 控制地址")
    parser.add_argument(
        "--video-url",
        default="http://192.168.137.67:8081/stream.mjpg",
        help="CyberCAM MJPEG 地址",
    )
    args = parser.parse_args()
    try:
        config = make_config(args.car_url, args.video_url)
    except ValueError as exc:
        parser.error(str(exc))
    with make_server(args.host, args.port, config) as server:
        print(f"网页：http://{args.host}:{server.server_port}/", flush=True)
        print(f"视频源：{config.video_url}", flush=True)
        print(f"小车控制：{config.car_url}", flush=True)
        try:
            server.serve_forever(poll_interval=0.2)
        except KeyboardInterrupt:
            print("\n已停止网页服务", flush=True)


if __name__ == "__main__":
    main()
