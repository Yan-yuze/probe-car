import json
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.error import HTTPError
from urllib.request import Request, urlopen

from app import make_config, make_server


MJPEG = b"--frame\r\nContent-Type: image/jpeg\r\n\r\n\xff\xd8demo\xff\xd9\r\n"


class DeviceHandler(BaseHTTPRequestHandler):
    commands = []
    servo_commands = []

    def do_GET(self):
        if self.path == "/api/status":
            self.reply_json({"command": "STOP", "stm32_online": True, "stm32_version": "V80SG90",
                             "servo_supported": True, "servo_angle": 90, "servo_target_angle": 90})
        elif self.path == "/health":
            self.reply_json({"ok": True, "frames": 9, "age_ms": 20})
        elif self.path == "/stream.mjpg":
            self.send_response(200)
            self.send_header("Content-Type", "multipart/x-mixed-replace; boundary=frame")
            self.send_header("Content-Length", str(len(MJPEG)))
            self.end_headers()
            self.wfile.write(MJPEG)
        else:
            self.send_error(404)

    def do_POST(self):
        if self.path not in {"/api/command", "/api/servo"}:
            self.send_error(404)
            return
        raw = self.rfile.read(int(self.headers["Content-Length"]))
        if self.path == "/api/servo":
            self.servo_commands.append(raw.decode("ascii"))
            self.reply_json({"command": "STOP", "stm32_online": True, "servo_supported": True,
                             "servo_target_angle": int(raw.decode("ascii").split("=", 1)[1])})
            return
        self.commands.append(raw.decode("ascii"))
        self.reply_json({"command": raw.decode("ascii").split("=", 1)[1], "stm32_online": True})

    def reply_json(self, data):
        raw = json.dumps(data).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def log_message(self, *_args):
        pass


class AppTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.device = ThreadingHTTPServer(("127.0.0.1", 0), DeviceHandler)
        cls.device_thread = threading.Thread(target=cls.device.serve_forever, daemon=True)
        cls.device_thread.start()
        device_url = f"http://127.0.0.1:{cls.device.server_port}"
        cls.app = make_server("127.0.0.1", 0, make_config(device_url, device_url + "/stream.mjpg"))
        cls.app_thread = threading.Thread(target=cls.app.serve_forever, daemon=True)
        cls.app_thread.start()
        cls.url = f"http://127.0.0.1:{cls.app.server_port}"

    @classmethod
    def tearDownClass(cls):
        cls.app.shutdown()
        cls.app.server_close()
        cls.device.shutdown()
        cls.device.server_close()
        cls.app_thread.join(timeout=2)
        cls.device_thread.join(timeout=2)

    def setUp(self):
        DeviceHandler.commands.clear()
        DeviceHandler.servo_commands.clear()

    def send_command(self, command, sequence):
        body = json.dumps({"command": command, "client_id": "test-client", "sequence": sequence}).encode()
        request = Request(
            self.url + "/api/command",
            data=body,
            headers={"Content-Type": "application/json", "X-Car-UI": "1"},
            method="POST",
        )
        with urlopen(request) as response:
            return json.load(response)

    def test_status_and_video_are_forwarded(self):
        with urlopen(self.url + "/api/status") as response:
            status = json.load(response)
        self.assertTrue(status["car"]["stm32_online"])
        self.assertTrue(status["video"]["ok"])
        with urlopen(self.url + "/video.mjpg") as response:
            self.assertIn("multipart/x-mixed-replace", response.headers["Content-Type"])
            self.assertEqual(response.read(), MJPEG)

    def test_commands_use_existing_esp32_http_protocol_and_skip_late_motion(self):
        self.send_command("FORWARD", 100)
        self.send_command("STOP", 102)
        late = self.send_command("FORWARD", 101)
        self.assertTrue(late["skipped"])
        self.assertEqual(DeviceHandler.commands, ["cmd=FORWARD", "cmd=STOP"])

    def test_invalid_command_is_rejected(self):
        with self.assertRaises(HTTPError) as failure:
            self.send_command("FLY", 200)
        self.assertEqual(failure.exception.code, 400)
        self.assertEqual(DeviceHandler.commands, [])

    def send_servo(self, angle, sequence, client_id="servo-test"):
        body = json.dumps({"angle": angle, "client_id": client_id, "sequence": sequence}).encode()
        request = Request(self.url + "/api/servo", data=body,
                          headers={"Content-Type": "application/json", "X-Car-UI": "1"})
        with urlopen(request) as response:
            return json.load(response)

    def test_servo_forwarding_and_stale_angle_guard(self):
        self.send_servo(30, 1)
        self.send_servo(150, 3)
        self.assertTrue(self.send_servo(90, 2)["skipped"])
        self.assertEqual(DeviceHandler.servo_commands, ["angle=30", "angle=150"])
        self.assertEqual(DeviceHandler.commands, [])

    def test_servo_does_not_supersede_motor_stop(self):
        self.send_servo(100, 999, client_id="test-client")
        self.send_command("STOP", 500)
        self.assertEqual(DeviceHandler.servo_commands, ["angle=100"])
        self.assertEqual(DeviceHandler.commands, ["cmd=STOP"])

    def test_invalid_servo_angles_never_reach_device(self):
        for angle in (29, 151, True, 90.5, "90", None):
            with self.subTest(angle=angle), self.assertRaises(HTTPError) as failure:
                self.send_servo(angle, 1000)
            self.assertEqual(failure.exception.code, 400)
        self.assertEqual(DeviceHandler.servo_commands, [])


if __name__ == "__main__":
    unittest.main()
