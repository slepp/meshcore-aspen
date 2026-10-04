"""Native HTTP lifecycle with the real Hew host and isolated companion health."""
from concurrent.futures import ThreadPoolExecutor
import http.client
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import threading
import time
import unittest

from parity import ROOT, public
from service_demo import RunningService


class CompanionHealth:
    def __init__(self, role):
        self.role = role
        self.ready = True
        self.invalid = False
        fixture = self

        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):
                if fixture.invalid:
                    body = b"invalid health JSON"
                    code = 200
                elif self.path == "/status":
                    body = json.dumps({fixture.role: {"public_key": public(9).hex(),
                        "state": "running", "radio_connected": True, "listener_active": True}}).encode()
                    code = 200
                else:
                    body = json.dumps({"ready": fixture.ready,
                        "not_ready": {} if fixture.ready else {fixture.role: "radio_disconnected"},
                        "mqtt_connection_checked": False}).encode()
                    code = 200 if fixture.ready else 503
                self.send_response(code)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *_):
                pass

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=3)


class Dashboard(unittest.TestCase):
    def until(self, callback, timeout=6):
        end = time.monotonic()+timeout
        while time.monotonic() < end:
            value = callback()
            if value:
                return value
            time.sleep(.03)
        self.fail("native dashboard condition timed out")

    def test_unknown_write_is_not_replayed_and_health_remains_available(self):
        for binary in ("hew-dashboard", "hew-dashboard-release"):
            with self.subTest(binary=binary), tempfile.TemporaryDirectory(dir=ROOT/"build") as name:
                root = Path(name)
                configuration = root/"dashboard.conf"
                configuration.write_text(f"port=0\nstate={root}\n"
                    f"page={ROOT.parents[1]/'internal/app/admin_page.html'}\n"
                    "base_port=0\nbot_companion_port=0\ntoken_env=MESHCORE_DASHBOARD_FIXTURE_TOKEN\n")
                configuration.chmod(0o600)
                listener = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
                listener.bind(str(root/"admin.sock"))
                (root/"admin.sock").chmod(0o600)
                listener.listen(8)
                listener.settimeout(3)
                environment = os.environ|{"MESHCORE_DASHBOARD_FIXTURE_TOKEN": "fixture-token-"+32*"a"}
                environment.pop("MESHCORE_HOST_ADMIN_HTTP", None)
                process = subprocess.Popen([str(ROOT/"build"/binary), str(configuration)],
                    env=environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                accepted = None
                try:
                    line = process.stdout.readline().strip()
                    self.assertIn("DASHBOARD_LISTEN", line)
                    port = int(line.split("port=")[1].split()[0])

                    def request(path, command=b"", method="POST"):
                        client = http.client.HTTPConnection("127.0.0.1", port, timeout=12)
                        try:
                            client.request(method, path, command, {
                                "X-Host-Intent": "admin-v1",
                                "X-Host-Admin": environment["MESHCORE_DASHBOARD_FIXTURE_TOKEN"]})
                            response = client.getresponse()
                            return response.status, response.read()
                        finally:
                            client.close()

                    with ThreadPoolExecutor(max_workers=1) as pool:
                        pending = pool.submit(request, "/admin/command", b"bot name Uncertain Fixture")
                        accepted, _ = listener.accept()
                        accepted.settimeout(3)
                        self.assertEqual(accepted.recv(4096), b"\x01\x04bot name Uncertain Fixture")
                        started = time.monotonic()
                        self.assertEqual(request("/status", method="GET")[0], 200)
                        code, body = request("/readyz", method="GET")
                        self.assertEqual(code, 503)
                        self.assertFalse(json.loads(body)["ready"])
                        self.assertLess(time.monotonic()-started, .5)
                        self.assertFalse(pending.done())
                        code, body = pending.result(timeout=11)
                        self.assertEqual(code, 504)
                        self.assertIn(b"outcome unknown", body)
                    accepted.settimeout(.3)
                    self.assertEqual(accepted.recv(4096), b"")
                    listener.settimeout(.3)
                    with self.assertRaises(socket.timeout):
                        listener.accept()
                    process.terminate()
                    stdout, stderr = process.communicate(timeout=6)
                    self.assertEqual(process.returncode, 0, stderr)
                    self.assertNotIn("Uncertain Fixture", line+stdout+stderr)
                    self.assertNotIn(environment["MESHCORE_DASHBOARD_FIXTURE_TOKEN"], line+stdout+stderr)
                finally:
                    if accepted is not None:
                        accepted.close()
                    listener.close()
                    if process.poll() is None:
                        process.terminate()
                        process.communicate(timeout=6)

    def test_http_owner_health_and_restart(self):
        for binary in ("hew-dashboard", "hew-dashboard-release"):
            with self.subTest(binary=binary), tempfile.TemporaryDirectory(dir=ROOT/"build") as name:
                root = Path(name)
                service = RunningService(root=root/"host")
                base = CompanionHealth("companion")
                secondary = CompanionHealth("bot_companion")
                process = None
                token = ""
                try:
                    configuration = root/"dashboard.conf"
                    configuration.write_text(f"port=0\nstate={service.root}\n"
                        f"page={ROOT.parents[1]/'internal/app/admin_page.html'}\n"
                        f"base_port={base.server.server_port}\n"
                        f"bot_companion_port={secondary.server.server_port}\n"
                        "password_env=MESHCORE_DASHBOARD_FIXTURE_PASSWORD\n")
                    configuration.chmod(0o600)
                    process = subprocess.Popen([str(ROOT/"build"/binary), str(configuration)],
                        env=os.environ|{"MESHCORE_HOST_ADMIN_HTTP": "1",
                            "MESHCORE_DASHBOARD_FIXTURE_PASSWORD": "private-fixture-password"},
                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                    line = process.stdout.readline().strip()
                    self.assertIn("DASHBOARD_LISTEN", line)
                    port = int(line.split("port=")[1].split()[0])

                    def request(path, body=b"", method="POST", headers=None):
                        connection = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
                        try:
                            connection.request(method, path, body,
                                headers or {"X-Host-Intent": "admin-v1", "X-Host-Admin": token})
                            response = connection.getresponse()
                            return response.status, response.read(), dict(response.getheaders())
                        finally:
                            connection.close()

                    code, body, headers = request("/admin", method="GET")
                    self.assertEqual(code, 200)
                    self.assertIn(b"MeshCore", body)
                    self.assertEqual(headers["Cache-Control"], "no-store")
                    self.assertIn("frame-ancestors 'none'", headers["Content-Security-Policy"])
                    self.assertEqual(request("/admin/status")[0], 401)
                    self.assertEqual(request("/admin/login", b"private-fixture-password",
                        headers={"Origin": "http://another-host", "X-Host-Intent": "admin-v1"})[0], 403)
                    self.assertEqual(request("/admin/login", b"private-fixture-password", headers={"X-Host-Intent": "wrong"})[0], 403)
                    code, body, _ = request("/admin/login", b"private-fixture-password")
                    self.assertEqual(code, 200)
                    token = body.decode()
                    self.assertEqual(len(token), 64)
                    self.assertEqual(request("/admin/status", headers={"X-Host-Admin": token})[0], 403)
                    self.until(lambda: (root/"host/admin.sock").exists())

                    def healthy():
                        _, body, _ = request("/status", method="GET")
                        value = json.loads(body)
                        return value if value["bot"]["radio_connected"] and value["companion"].get("listener_active") else None

                    status = self.until(healthy)
                    self.assertEqual(set(status), {"bot", "repeater", "room", "observer", "companion", "bot_companion"})
                    self.assertEqual(status["bot"]["public_key"], public(5).hex())
                    self.assertEqual(status["bot_companion"]["public_key"], public(9).hex())
                    self.assertGreater(len(json.dumps(status)), 4096)
                    code, body, _ = request("/admin/status")
                    self.assertEqual(code, 200)
                    self.assertTrue(json.loads(body)["capabilities"]["native_owner"])
                    self.assertEqual(set(json.loads(body)["capabilities"]["role_owner_commands"]), {"room", "repeater"})
                    code, body, _ = request("/readyz", method="GET")
                    self.assertEqual(code, 503)
                    self.until(lambda: json.loads(request("/readyz", method="GET")[1])["not_ready"] ==
                               {"observer": "health_connection_unavailable"})
                    for command in (b"bot status", b"bot key", b"bot name"):
                        self.assertEqual(request("/admin/command", command)[0], 200)
                    self.assertEqual(request("/admin/command", b"bot name Dashboard Fixture")[1],
                                     b"Saved and applied bot name; identity unchanged")
                    self.assertEqual(request("/admin/role/room", b"set name Dashboard Room")[1], b"OK")
                    self.assertEqual(request("/admin/role/room", b"get name")[1], b"> Dashboard Room")
                    self.assertEqual(request("/admin/role/room", b"region def can ab edm")[1],
                                     b"*^ F\n can F\n  ab F\n   edm F\n")
                    self.assertEqual(request("/admin/role/room", b"region home ab")[1], b" home is now ab")
                    self.assertEqual(request("/admin/role/room", b"region default ab")[1], b" default scope is now ab")
                    self.assertEqual(request("/admin/role/room", b"region put can edm")[0], 409)
                    before = (service.root/"admin.clock").read_bytes()
                    for path, command in (("/admin/command", b"bot password secret"),
                                          ("/admin/role/room", b"set tx 2"),
                                          ("/admin/role/companion", b"get name")):
                        self.assertIn(request(path, command)[0], (400, 404))
                    self.assertEqual((service.root/"admin.clock").read_bytes(), before)
                    self.assertEqual(request("/admin/role/room", b"set advert.interval 999")[0], 409)
                    with socket.create_connection(("127.0.0.1", port), timeout=3) as malformed:
                        malformed.sendall(b"POST /admin/command HTTP/1.1\r\nHost: localhost\r\n"
                            b"Content-Length: 6\r\nContent-Length: 6\r\n\r\nreboot")
                        self.assertIn(b" 400 ", malformed.recv(4096))
                    with socket.create_connection(("127.0.0.1", port), timeout=3) as slow:
                        slow.sendall(b"POST /admin/login HTTP/1.1\r\nHost:")
                        started = time.monotonic()
                        self.assertEqual(request("/status", method="GET")[0], 200)
                        self.assertLess(time.monotonic()-started, .5)
                    base.ready = False
                    self.until(lambda: json.loads(request("/readyz", method="GET")[1])["not_ready"].get("companion") == "radio_disconnected")
                    base.invalid = True
                    self.until(lambda: "health_error" in json.loads(request("/status", method="GET")[1])["companion"])
                    self.assertEqual(json.loads(request("/readyz", method="GET")[1])["not_ready"]["companion"], "health_connection_unavailable")
                    base.invalid = False
                    base.ready = True
                    service.stop_process()
                    self.until(lambda: "health_error" in json.loads(request("/status", method="GET")[1])["bot"])
                    service.start_process(True)
                    self.until(healthy, timeout=12)
                    self.assertEqual(request("/admin/command", b"bot name")[1], b"Name: Dashboard Fixture")
                    self.assertEqual(request("/admin/role/room", b"get name")[1], b"> Dashboard Room")
                    self.assertEqual(request("/admin/role/room", b"region home")[1], b" home is ab")
                    self.assertEqual(request("/admin/role/room", b"region default")[1], b" default scope is ab")
                    self.assertEqual(request("/admin/logout")[0], 200)
                    self.assertEqual(request("/admin/status")[0], 401)
                    process.terminate()
                    stdout, stderr = process.communicate(timeout=6)
                    self.assertEqual(process.returncode, 0, stderr)
                    self.assertNotIn("private-fixture-password", line+stdout+stderr)
                    self.assertFalse(any("Dashboard Fixture" in item for item in service.logs+service.errors))
                finally:
                    if process is not None and process.poll() is None:
                        process.terminate()
                        process.communicate(timeout=6)
                    service.close()
                    base.close()
                    secondary.close()


if __name__ == "__main__":
    os.environ.setdefault("MESHCORE_HEW_HOST", str(ROOT/"build/hew-host-release"))
    unittest.main(verbosity=2)
