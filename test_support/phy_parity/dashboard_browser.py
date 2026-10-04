"""Render the actual offline page against a local serialized snapshot, not a radio."""
import html
import base64
import hashlib
import http.server
import json
import pathlib
import re
import shutil
import subprocess
import socket
import struct
import sys
import tempfile
import threading

build = pathlib.Path(sys.argv[1]).resolve()
source = pathlib.Path(sys.argv[2]).read_text()
page = source.split('R"HTML(', 1)[1].rsplit(')HTML"', 1)[0].encode()
status = (build / "dashboard.json").read_bytes()
expected = json.loads(status)
chrome = shutil.which("google-chrome") or shutil.which("chromium")
if not chrome:
    raise SystemExit("Chrome/Chromium is required for the optional dashboard-browser target")


class Fixture(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/api/live":
            self.server.stream_calls += 1
            if self.server.fail_after_first and self.server.stream_calls > 1:
                self.send_error(503)
                return
            key = self.headers.get("Sec-WebSocket-Key", "")
            accept = base64.b64encode(hashlib.sha1(
                (key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
            self.send_response(101)
            self.send_header("Upgrade", "websocket")
            self.send_header("Connection", "Upgrade")
            self.send_header("Sec-WebSocket-Accept", accept)
            self.end_headers()
            try:
                for offset in range(0, len(status), 1024):
                    fragment = status[offset:offset + 1024]
                    opcode = 1 if offset == 0 else 0
                    if offset + len(fragment) == len(status):
                        opcode |= 0x80
                    length = bytes([len(fragment)]) if len(fragment) < 126 else b"\x7e" + struct.pack("!H", len(fragment))
                    self.wfile.write(bytes([opcode]) + length + fragment)
                self.wfile.flush()
                if self.server.fail_after_first:
                    self.wfile.write(b"\x88\x02\x03\xe9")
                    self.wfile.flush()
                    return
                self.connection.settimeout(5)
                while self.connection.recv(256):
                    pass
            except (ConnectionError, socket.timeout):
                pass
            return
        if self.path not in ("/", "/api/status"):
            self.send_error(404)
            return
        if self.path == "/api/status":
            self.server.api_calls += 1
        body = page if self.path == "/" else status
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8" if self.path == "/" else "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Content-Security-Policy",
                         "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; "
                         "connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'none'")
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_):
        pass


server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Fixture)
thread = threading.Thread(target=server.serve_forever)
thread.start()
try:
    for name, dimensions in (("desktop", "1280,1600"), ("mobile", "390,1900"), ("stale", "1280,1000")):
        server.api_calls = 0
        server.stream_calls = 0
        server.fail_after_first = name == "stale"
        with tempfile.TemporaryDirectory(prefix="dashboard-browser-", dir=build) as profile:
            result = subprocess.run([
                chrome, "--headless=new", "--disable-gpu", "--disable-background-networking",
                "--disable-component-update", "--disable-sync", "--no-first-run",
                "--no-default-browser-check", "--host-resolver-rules=MAP * ~NOTFOUND, EXCLUDE 127.0.0.1",
                "--user-data-dir=" + profile, "--window-size=" + dimensions,
                "--virtual-time-budget=5000", "--dump-dom",
                "--screenshot=" + str(build / ("dashboard-" + name + ".png")),
                "http://127.0.0.1:" + str(server.server_port) + "/",
            ], capture_output=True, text=True, timeout=45, check=True)
            dom = result.stdout
            (build / ("dashboard-" + name + ".html")).write_text(dom)
            heading = re.search(r'<h1 id="device-name">(.*?)</h1>', dom, re.S)
            assert heading and html.unescape(heading[1]) == expected["device_name"]
            if name == "stale":
                assert 'id="connection" class="pill warning">Stale</span>' in dom
                assert "Connection interrupted. Reconnecting..." in dom
                assert "Last updated " in dom
            else:
                assert 'id="connection" class="pill good">Live</span>' in dom
            assert "912.525 MHz" in dom and "250 kHz / SF7 / CR 4/5 / 2 dBm" in dom
            assert "Unconfirmed / RF timeout" in dom and '>Sent over RF</td>' in dom
            assert dom.count("<script>") == 1
            assert len(re.findall(r'<tbody id="events">.*?</tbody>', dom, re.S)) == 1
            assert server.stream_calls >= 1
            assert server.api_calls == 0, "browser must not poll the diagnostics endpoint"
            print("Dashboard " + name + " rendered data safely" +
                  (" and retained an explicitly stale snapshot" if name == "stale" else ""))
finally:
    server.shutdown()
    thread.join()
    server.server_close()
