#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Make-owned, loopback-only TLS fixture; no package installs, RF or hardware."""
import argparse
from datetime import datetime, timedelta, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import secrets
import socket
import subprocess
import tempfile
import threading
import time
import ssl
from urllib.parse import parse_qs, urlsplit


class WeatherFixture(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        assert "Authorization" not in self.headers
        url = urlsplit(self.path)
        query = parse_qs(url.query)
        status = 200
        if url.path == "/geocode":
            place = query["name"][0]
            if place == "Offline":
                status, result = 503, {}
            elif place == "Timeout":
                status, result = 408, {}
            elif place == "Malformed":
                result = "{"
            elif place == "Oversize":
                result = "x" * 16385
            else:
                result = {"results": [{"name": "Fixture City", "country": "Testland",
                                       "latitude": 2 if place == "Stale" else 1, "longitude": 1}]}
        elif url.path == "/forecast":
            age = 180 if query["latitude"] == ["2"] else 15
            observed = datetime.now(timezone.utc) - timedelta(minutes=age)
            result = {"current_units": {"temperature_2m": "\u00b0C"},
                      "current": {"time": observed.strftime("%Y-%m-%dT%H:%M"),
                                  "temperature_2m": 12.5, "weather_code": 3}}
        else:
            status, result = 404, {}
        body = (result if isinstance(result, str) else json.dumps(result)).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--service", type=Path, required=True)
    parser.add_argument("--client", type=Path, required=True)
    parser.add_argument("--faults", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    with tempfile.TemporaryDirectory(prefix="onchip-https-tls-", dir=root / ".tmp") as temporary:
        directory = Path(temporary)
        for name in ("server", "untrusted"):
            subprocess.run([
                "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost",
                "-keyout", str(directory / (name + ".key")),
                "-out", str(directory / (name + ".crt")),
            ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            port = reservation.getsockname()[1]
        environment = os.environ.copy()
        environment["MESHCORE_BOT_SERVICE_TOKEN"] = secrets.token_hex(16)
        with ThreadingHTTPServer(("127.0.0.1", 0), WeatherFixture) as provider:
            thread = threading.Thread(target=provider.serve_forever)
            thread.start()
            try:
                for weather in (False, True):
                    fixture = f"http://127.0.0.1:{provider.server_port}"
                    with (directory / "service.log").open("w+") as log:
                        process = subprocess.Popen([
                            str(args.service), "-listen", f"127.0.0.1:{port}",
                            "-tls-cert", str(directory / "server.crt"), "-tls-key", str(directory / "server.key"),
                            f"-weather={str(weather).lower()}",
                            "-compute",
                            "-geocode-url", fixture + "/geocode", "-forecast-url", fixture + "/forecast",
                        ], env=environment, stdout=log, stderr=subprocess.STDOUT)
                        try:
                            deadline = time.monotonic() + 5
                            while True:
                                if process.poll() is not None:
                                    log.seek(0)
                                    raise RuntimeError("Loopback service failed: " + log.read(4096))
                                try:
                                    with socket.create_connection(("127.0.0.1", port), timeout=.2):
                                        break
                                except OSError:
                                    if time.monotonic() >= deadline:
                                        raise TimeoutError("Loopback service did not become ready") from None
                                    time.sleep(.05)
                            subprocess.run([str(args.client), str(port), str(directory / "server.crt"),
                                            str(directory / "untrusted.crt"), str(int(weather))],
                                           env=environment, check=True, timeout=60)
                        finally:
                            process.terminate()
                            try:
                                process.wait(timeout=5)
                            except subprocess.TimeoutExpired:
                                process.kill()
                                process.wait()
            finally:
                provider.shutdown()
                thread.join()
        if args.faults:
            release = threading.Event()
            class EffectFixture(BaseHTTPRequestHandler):
                protocol_version = "HTTP/1.1"
                def log_message(self, *args):
                    pass
                def do_POST(self):
                    assert self.headers.get("Authorization") == "Bearer " + environment["MESHCORE_BOT_SERVICE_TOKEN"]
                    assert json.loads(self.rfile.read(int(self.headers["Content-Length"]))) == {"value": 1}
                    self.server.effects += 1
                    if self.path == "/effect/unknown":
                        self.close_connection = True
                        return
                    assert self.path == "/effect/cancel"
                    self.send_response(200)
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Content-Length", "100")
                    self.end_headers()
                    self.wfile.flush()
                    release.wait(5)
                    self.close_connection = True
            with ThreadingHTTPServer(("127.0.0.1", 0), EffectFixture) as effects:
                context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
                context.minimum_version = ssl.TLSVersion.TLSv1_2
                context.load_cert_chain(directory / "server.crt", directory / "server.key")
                effects.socket = context.wrap_socket(effects.socket, server_side=True)
                effects.effects = 0
                serving = threading.Thread(target=effects.serve_forever)
                serving.start()
                try:
                    for expected, mode in enumerate(("unknown", "cancel"), 1):
                        subprocess.run([str(args.client), str(effects.server_port),
                                        str(directory / "server.crt"), str(directory / "untrusted.crt"),
                                        mode], env=environment, check=True, timeout=10)
                        assert effects.effects == expected, "POST was replayed or never accepted"
                finally:
                    release.set()
                    effects.shutdown()
                    serving.join()


if __name__ == "__main__":
    main()
