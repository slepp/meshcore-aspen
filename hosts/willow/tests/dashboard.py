"""Native HTTP lifecycle with the real Hew host and isolated companion health."""
from concurrent.futures import ThreadPoolExecutor
import http.client
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import threading
import time
import unittest

from parity import ROOT, public


class CompanionHealth:
    def __init__(self, role):
        self.role = role
        self.ready = True
        self.invalid = False
        self.resume = threading.Event()
        self.resume.set()
        self.blocked = threading.Event()
        self.requests = 0
        fixture = self

        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):
                fixture.requests += 1
                if not fixture.resume.is_set():
                    fixture.blocked.set()
                    fixture.resume.wait(8)
                if fixture.invalid:
                    body = b"invalid health JSON"
                    code = 200
                elif self.path == "/status":
                    body = json.dumps({fixture.role: {"public_key": public(9).hex(),
                        "state": "running", "radio_connected": True, "listener_active": True,
                        "role_tx": {"available": True, "accepted": 7, "succeeded": 6, "unknown": 1, "reported_rf_airtime_ns": 250000000}}}).encode()
                    code = 200
                else:
                    body = json.dumps({"ready": fixture.ready,
                        "not_ready": {} if fixture.ready else {fixture.role: "radio_disconnected"},
                        "mqtt_connection_checked": False}).encode()
                    code = 200 if fixture.ready else 503
                self.send_response(code)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                try:
                    self.wfile.write(body)
                except (BrokenPipeError, ConnectionResetError):
                    pass

            def log_message(self, *_):
                pass

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def close(self):
        self.resume.set()
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=3)


class OwnerHealth:
    def __init__(self, root, observer_enabled=None):
        self.health_requests = 0
        self.stop = threading.Event()
        self.listeners = []
        self.threads = []
        roles = {name: {"state": "running", "radio_connected": True}
                 for name in ("bot", "repeater", "room", "observer")}
        roles["bot"]["public_key"] = public(9).hex()
        roles["bot"]["packet_reports"] = [{"slot": 0, "name": "repeater_snr",
            "fields": ["mean_db", "rssi_mean_dbm", "samples"],
            "rows": [{"label": "D12600", "values": [-2.25, -109.5, 12]},
                     {"label": 'quote"slash\\', "values": [1.5, -98, 3]}]}]
        self.roles = roles
        roles["observer"]["observer_stats"] = {"observed": 14, "published": 11, "dropped": 1, "queued": 1, "pending": True, "dirty": True, "saving": False, "save_failures": 2, "save_error": -1, "restored": 4, "expired": 3}
        roles["observer"]["mqtt_connected"] = True
        for name in roles:
            roles[name]["telemetry"] = {"has_counters": True,
                "counters": {"PacketsRecv": 42, "PacketsSent": 12, "PacketsErrors": 1},
                "has_mcu_temperature": True, "mcu_temperature_c": 31.5,
                "has_battery": False, "battery_mv": 0}
        ready = {"ready": True, "not_ready": {}, "mqtt_connection_checked": True}
        if observer_enabled is not None:
            ready["observer_enabled"] = observer_enabled
        if observer_enabled is False:
            del roles["observer"]
        for name in ("owner.sock", "admin.sock"):
            listener = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
            listener.bind(str(root/name))
            (root/name).chmod(0o600)
            listener.listen(8)
            listener.settimeout(.1)
            self.listeners.append(listener)

            def serve(listener=listener, health=name == "owner.sock"):
                while not self.stop.is_set():
                    try:
                        connection, _ = listener.accept()
                    except socket.timeout:
                        continue
                    except OSError:
                        break
                    with connection:
                        connection.settimeout(3)
                        request = connection.recv(4096)
                        if not request:
                            continue
                        if health:
                            self.health_requests += 1
                            body = json.dumps(roles if request[2] == 0 else ready).encode()
                        else:
                            body = b"Name: Fixture"
                        connection.sendall(b"\x01\x00"+body)

            thread = threading.Thread(target=serve, daemon=True)
            thread.start()
            self.threads.append(thread)

    def close(self):
        self.stop.set()
        for listener in self.listeners:
            listener.close()
        for thread in self.threads:
            thread.join(timeout=3)


class Dashboard(unittest.TestCase):
    def until(self, callback, timeout=6):
        end = time.monotonic()+timeout
        while time.monotonic() < end:
            value = callback()
            if value:
                return value
            time.sleep(.03)
        self.fail("native dashboard condition timed out")

    def test_ipv6_wildcard_listener(self):
        try:
            with socket.socket(socket.AF_INET6) as probe:
                probe.bind(("::1", 0))
        except OSError:
            self.skipTest("IPv6 loopback is unavailable")
        for binary in ("hew-dashboard", "hew-dashboard-release"):
            with self.subTest(binary=binary), tempfile.TemporaryDirectory(dir=ROOT.parents[1], prefix=".dash-") as name:
                root = Path(name)
                configuration = root/"dashboard.conf"
                configuration.write_text(f"address=::\nport=0\nstate={root}\n"
                    f"page={ROOT.parents[1]/'internal/app/admin_page.html'}\n"
                    "base_port=0\nbot_companion_port=0\n")
                configuration.chmod(0o600)
                environment = os.environ.copy()
                environment.pop("MESHCORE_HOST_ADMIN_HTTP", None)
                process = subprocess.Popen([str(ROOT/"build"/binary), str(configuration)],
                    env=environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                try:
                    line = process.stdout.readline().strip()
                    self.assertIn("DASHBOARD_LISTEN address=:: ", line)
                    port = int(line.split("port=")[1].split()[0])
                    for address in ("::1", "127.0.0.1"):
                        connection = http.client.HTTPConnection(address, port, timeout=4)
                        try:
                            for path in ("/", "/admin"):
                                connection.request("GET", path)
                                response = connection.getresponse()
                                self.assertEqual(response.status, 404)
                                self.assertIn(b"Host route not found", response.read())
                        finally:
                            connection.close()
                finally:
                    process.terminate()
                    process.communicate(timeout=6)

    def test_unknown_write_is_not_replayed_and_health_remains_available(self):
        for binary in ("hew-dashboard", "hew-dashboard-release"):
            with self.subTest(binary=binary), tempfile.TemporaryDirectory(dir=ROOT.parents[1], prefix=".dash-") as name:
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

    def test_health_refresh_is_demand_driven_shared_and_bounded(self):
        for binary in ("hew-dashboard", "hew-dashboard-release"):
            with self.subTest(binary=binary), tempfile.TemporaryDirectory(dir=ROOT.parents[1], prefix=".dash-") as name:
                root = Path(name)
                owner = OwnerHealth(root)
                base = CompanionHealth("companion")
                secondary = CompanionHealth("bot_companion")
                process = None
                credential = "fixture-token-"+32*"a"
                try:
                    configuration = root/"dashboard.conf"
                    configuration.write_text(f"port=0\nstate={root}\nbase_port={base.server.server_port}\n"
                        f"page={ROOT.parents[1]/'internal/app/admin_page.html'}\n"
                        f"bot_companion_port={secondary.server.server_port}\n"
                        "token_env=MESHCORE_DASHBOARD_FIXTURE_TOKEN\n")
                    configuration.chmod(0o600)
                    process = subprocess.Popen([str(ROOT/"build"/binary), str(configuration)],
                        env=os.environ|{"HEW_WORKERS": "4", "MESHCORE_DASHBOARD_FIXTURE_TOKEN": credential},
                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                    line = process.stdout.readline().strip()
                    self.assertIn("DASHBOARD_LISTEN", line)
                    port = int(line.split("port=")[1].split()[0])

                    def request(path, body=b"", authorized=True):
                        connection = http.client.HTTPConnection("127.0.0.1", port, timeout=4)
                        try:
                            headers = {"X-Host-Intent": "admin-v1"}
                            if authorized:
                                headers["X-Host-Admin"] = credential
                            connection.request("POST" if path.startswith("/admin/") else "GET", path, body, headers)
                            response = connection.getresponse()
                            return response.status, response.read()
                        finally:
                            connection.close()

                    def counts():
                        return owner.health_requests, base.requests, secondary.requests

                    time.sleep(.7)
                    self.assertEqual(counts(), (0, 0, 0))
                    self.assertEqual(request("/admin/status", authorized=False)[0], 401)
                    self.assertEqual(counts(), (0, 0, 0))
                    base.resume.clear()
                    with ThreadPoolExecutor(max_workers=16) as pool:
                        pending = [pool.submit(request, path) for path in
                                   ("/status", "/readyz", "/admin/status", "/metrics")*4]
                        self.until(base.blocked.is_set)
                        self.until(lambda: owner.health_requests == 2 and secondary.requests == 2)
                        self.assertFalse(any(item.done() for item in pending))
                        base.resume.set()
                        results = [item.result(timeout=4) for item in pending]
                    self.assertTrue(all(code == 200 for code, _ in results))
                    self.assertTrue(json.loads(results[1][1])["ready"])
                    self.assertEqual(counts(), (2, 2, 2))
                    code, metrics = request("/metrics", authorized=False)
                    self.assertEqual(code, 200)
                    self.assertIn(b'willow_role_health_available{role="companion"} 1', metrics)
                    self.assertIn(b'willow_observer_observed_total{role="observer"} 14', metrics)
                    self.assertIn(b'willow_role_tx_accepted_total{role="companion"} 7', metrics)
                    self.assertIn(b'willow_role_tx_reported_airtime_seconds_total{role="companion"} 0.25', metrics)
                    self.assertIn(b'willow_observer_pending{role="observer"} 1', metrics)
                    self.assertIn(b'willow_observer_dirty{role="observer"} 1', metrics)
                    self.assertIn(b'willow_observer_save_error{role="observer"} -1', metrics)
                    self.assertIn(b'willow_observer_expired_total{role="observer"} 3', metrics)
                    self.assertNotIn(b'public_key', metrics)
                    self.assertNotIn(b'fixture-token', metrics)
                    self.assertNotIn(b'\nwillow_modem_battery_volts ', metrics)
                    self.assertEqual(metrics.count(b'\nwillow_modem_received_packets_total '), 1)
                    self.assertIn(b'\nwillow_modem_received_packets_total 42', metrics)
                    self.assertIn(b'\nwillow_modem_temperature_celsius 31.5', metrics)
                    packet_labels = f'{{device="willow_{public(9).hex()[:16]}",slot="0",report="repeater_snr",label="D12600"}}'.encode()
                    self.assertIn(b'meshcore_packet_mean_db'+packet_labels+b' -2.25', metrics)
                    self.assertIn(b'meshcore_packet_rssi_mean_dbm'+packet_labels+b' -109.5', metrics)
                    self.assertIn(b'meshcore_packet_samples'+packet_labels+b' 12', metrics)
                    self.assertIn(b'label="quote\\"slash\\\\"', metrics)
                    request("/status")
                    request("/readyz")
                    self.assertEqual(counts(), (2, 2, 2))
                    time.sleep(.7)
                    self.assertEqual(counts(), (2, 2, 2))
                    owner.roles["bot"]["packet_reports"] = []
                    self.assertNotIn(b'\nmeshcore_packet_', request("/metrics")[1])
                    # A complete HTTP request remains valid after write-side EOF.
                    with socket.create_connection(("127.0.0.1", port), timeout=4) as half_closed:
                        half_closed.sendall(b"GET /readyz HTTP/1.1\r\nHost: localhost\r\n\r\n")
                        half_closed.shutdown(socket.SHUT_WR)
                        response = http.client.HTTPResponse(half_closed)
                        response.begin()
                        self.assertEqual(response.status, 200)
                        self.assertTrue(json.loads(response.read())["ready"])
                        response.close()
                    time.sleep(.6)

                    base.blocked.clear()
                    base.resume.clear()
                    with ThreadPoolExecutor(max_workers=1) as pool:
                        started = time.monotonic()
                        pending = pool.submit(request, "/readyz")
                        self.until(base.blocked.is_set)
                        self.assertFalse(pending.done())
                        command_at = time.monotonic()
                        self.assertEqual(request("/admin/command", b"bot name"), (200, b"Name: Fixture"))
                        self.assertLess(time.monotonic()-command_at, .5)
                        code, body = pending.result(timeout=4)
                        self.assertGreater(time.monotonic()-started, 1.5)
                        self.assertLess(time.monotonic()-started, 3)
                    self.assertEqual(code, 503)
                    self.assertEqual(json.loads(body)["not_ready"]["companion"], "health_connection_unavailable")
                    completed = counts()
                    self.assertEqual(request("/readyz")[0], 503)
                    self.assertEqual(counts(), completed)
                    code, metrics = request("/metrics", authorized=False)
                    self.assertEqual(code, 200)
                    self.assertIn(b'willow_role_health_available{role="companion"} 0', metrics)
                    self.assertNotIn(b'willow_companion_listener_active{role="companion"}', metrics)
                    self.assertNotIn(b'willow_role_tx_accepted_total{role="companion"}', metrics)
                    self.assertEqual(counts(), completed)
                    # Resetting the last waiter cancels its refresh; quiet time must
                    # not start retries or turn the expired success into a reply.
                    time.sleep(.6)
                    base.blocked.clear()
                    with socket.create_connection(("127.0.0.1", port), timeout=3) as cancelled:
                        cancelled.sendall(b"GET /status HTTP/1.1\r\nHost: localhost\r\n\r\n")
                        self.until(base.blocked.is_set)
                        cancelled.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
                    time.sleep(.2)
                    completed = counts()
                    time.sleep(.7)
                    self.assertEqual(counts(), completed)
                    base.resume.set()
                    code, body = request("/readyz")
                    self.assertEqual(code, 200)
                    self.assertTrue(json.loads(body)["ready"])
                    process.terminate()
                    _, stderr = process.communicate(timeout=6)
                    self.assertEqual(process.returncode, 0, stderr)
                finally:
                    base.resume.set()
                    if process is not None and process.poll() is None:
                        process.terminate()
                        process.communicate(timeout=6)
                    owner.close()
                    base.close()
                    secondary.close()

    def test_disabled_observer_is_not_required_for_readiness(self):
        for binary in ("hew-dashboard", "hew-dashboard-release"):
            # None models a host that predates the field: the observer stays required.
            for enabled, observer_missing in ((False, True), (True, True), (None, True), (True, False)):
                with self.subTest(binary=binary, observer_enabled=enabled, observer_missing=observer_missing), \
                        tempfile.TemporaryDirectory(dir=ROOT.parents[1], prefix=".dash-") as name:
                    root = Path(name)
                    owner = OwnerHealth(root, observer_enabled=enabled)
                    if enabled is not False and observer_missing:
                        del owner.roles["observer"]
                    base = CompanionHealth("companion")
                    secondary = CompanionHealth("bot_companion")
                    process = None
                    try:
                        base.resume.set()
                        secondary.resume.set()
                        configuration = root/"dashboard.conf"
                        configuration.write_text(f"port=0\nstate={root}\nbase_port={base.server.server_port}\n"
                            f"page={ROOT.parents[1]/'internal/app/admin_page.html'}\n"
                            f"bot_companion_port={secondary.server.server_port}\n"
                            "token_env=MESHCORE_DASHBOARD_FIXTURE_TOKEN\n")
                        configuration.chmod(0o600)
                        process = subprocess.Popen([str(ROOT/"build"/binary), str(configuration)],
                            env=os.environ|{"HEW_WORKERS": "4", "MESHCORE_DASHBOARD_FIXTURE_TOKEN": "fixture-token-"+32*"a"},
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                        port = int(process.stdout.readline().strip().split("port=")[1].split()[0])

                        def get(path):
                            connection = http.client.HTTPConnection("127.0.0.1", port, timeout=4)
                            try:
                                connection.request("GET", path)
                                response = connection.getresponse()
                                return response.status, response.read()
                            finally:
                                connection.close()

                        code, body = get("/readyz")
                        _, metrics = get("/metrics")
                        status = json.loads(get("/status")[1])
                        if enabled is False:
                            self.assertEqual((code, json.loads(body)["not_ready"]), (200, {}))
                            self.assertEqual(status["observer"]["state"], "disabled")
                            self.assertIn(b'willow_role_health_available{role="observer"} 1', metrics)
                            self.assertNotIn(b'willow_role_ready{role="observer"}', metrics)
                        elif observer_missing:
                            self.assertEqual(code, 503)
                            self.assertEqual(json.loads(body)["not_ready"],
                                             {"observer": "health_connection_unavailable"})
                        else:
                            self.assertEqual(code, 200)
                            self.assertIn(b'willow_role_ready{role="observer"} 1', metrics)
                        process.terminate()
                        process.communicate(timeout=6)
                    finally:
                        if process is not None and process.poll() is None:
                            process.terminate()
                            process.communicate(timeout=6)
                        owner.close()
                        base.close()
                        secondary.close()

    def test_http_owner_health_and_restart(self):
        from service_demo import RunningService
        for binary in ("hew-dashboard", "hew-dashboard-release"):
            with self.subTest(binary=binary), tempfile.TemporaryDirectory(dir=ROOT.parents[1], prefix=".dash-") as name:
                root = Path(name)
                service = RunningService(root=root/"host")
                base = CompanionHealth("companion")
                secondary = CompanionHealth("bot_companion")
                process = None
                token = ""
                try:
                    configuration = root/"dashboard.conf"
                    configuration.write_text(f"address=0.0.0.0\nport=0\nstate={service.root}\n"
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
                    self.assertIn("DASHBOARD_LISTEN address=0.0.0.0 ", line)
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
                    root_code, root_body, root_headers = request("/", method="GET")
                    self.assertEqual((root_code, root_body, root_headers), (code, body, headers))
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
                    # The service runs with observer.enabled=0, which readiness does not require.
                    self.until(lambda: request("/readyz", method="GET")[0] == 200)
                    self.assertEqual(json.loads(request("/status", method="GET")[1])["observer"]["state"], "disabled")
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
