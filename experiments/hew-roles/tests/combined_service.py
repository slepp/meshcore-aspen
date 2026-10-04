"""Native broker, observer, companions and dashboard on isolated modem fixtures."""
from http.client import HTTPConnection
import json
import os
from pathlib import Path
import select
import socket
import struct
import subprocess
import tempfile
import time
import unittest

from base_service import SharedModem, Client as CompanionClient, saved_state, addressed, advert, PROFILE
from base_service import transfer, load_rollback_in_go
from broker import Client as MQTTClient, frame
from observer_service import ObserverService
from parity import ROOT, public


def unused_port():
    with socket.socket() as candidate:
        candidate.bind(("127.0.0.1", 0))
        return candidate.getsockname()[1]


class CombinedService(unittest.TestCase):
    def until(self, predicate, timeout=12):
        end = time.monotonic()+timeout
        while time.monotonic() < end:
            result = predicate()
            if result:
                return result
            time.sleep(.05)
        self.fail("combined service condition did not become ready")

    def test_native_endpoints_observations_restarts_and_latest_rollback(self):
        processes, clients = [], []
        service = None
        modem = SharedModem()
        with tempfile.TemporaryDirectory(prefix=".combined-", dir=ROOT.parents[1]) as name:
            root = Path(name)

            def launch(binary, config, marker, environment=None):
                process = subprocess.Popen([str(ROOT/"build"/binary), str(config)],
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                    env=os.environ | (environment or {}))
                processes.append(process)
                self.assertTrue(select.select([process.stdout], [], [], 20)[0], binary+" startup timeout")
                line = process.stdout.readline().strip()
                self.assertTrue(line.startswith(marker), (binary, line, process.poll()))
                return process, line

            def stop(process, marker=""):
                process.terminate()
                stdout, stderr = process.communicate(timeout=15)
                self.assertEqual(process.returncode, 0, stdout+stderr)
                if marker:
                    self.assertIn(marker, stdout)
                self.assertFalse(stderr, stderr)

            def private(path, data):
                path.write_bytes(data)
                path.chmod(0o600)

            try:
                mqtt_port = unused_port()
                broker_config = root/"broker.conf"
                private(broker_config, f"host=127.0.0.1\nport={mqtt_port}\n".encode())
                broker, _ = launch("hew-broker-release", broker_config, "MQTT_BROKER_READY ")
                subscriber = MQTTClient(mqtt_port, "combined-observer", keepalive=0)
                clients.append(subscriber)
                subscriber.subscribe([("combined/#", 1)])
                service = ObserverService(root=root/"host", extra_config=(
                    f"observer.enabled=1\nobserver.url=tcp://127.0.0.1:{mqtt_port}\n"
                    "observer.format=internal-v1\nobserver.topic_prefix=combined\n"))
                health_ports, configurations, directories, companion_processes = [], [], [], []
                ports = []
                for index, seed in enumerate((17, 18)):
                    frozen = root/f"frozen-{index}"
                    frozen.mkdir(mode=0o700)
                    document = saved_state()
                    document["PublicKey"] = list(public(seed))
                    document["Name"] = ("Synthetic Base", "Synthetic Secondary")[index]
                    if index:
                        document["Contacts"] = document["Contacts"][:3]
                        document["Messages"] = []
                        document["Sequence"] = 0
                    document["Preferences"]["flood_advert_seconds"] = 0
                    private(frozen/"identity.seed", bytes([seed])*32)
                    private(frozen/"companion.json", json.dumps(document).encode())
                    directory = root/f"native-{index}"
                    transfer(frozen, directory, "stage")
                    directories.append(directory)
                    health_port = unused_port()
                    health_ports.append(health_port)
                    config = root/f"companion-{index}.json"
                    private(config, json.dumps({"state_dir": str(directory),
                        "radio_address": f"127.0.0.1:{modem.port}",
                        "companion_listen": "127.0.0.1:0",
                        "status_listen": f"127.0.0.1:{health_port}",
                        "status_role": ("companion", "bot_companion")[index],
                        "required_profile": PROFILE.hex(),
                        "enable_factory_reset": False, "enable_key_export": False,
                        "enable_key_import": False}).encode())
                    configurations.append(config)
                    process, line = launch("hew-base-release", config, "BASE_LISTEN ")
                    companion_processes.append(process)
                    ports.append(int(line.split("port=")[1].split()[0]))
                base, second, secondary = [CompanionClient(port) for port in (ports[0], ports[0], ports[1])]
                clients.extend((base, second, secondary))
                for client, seed in ((base, 17), (second, 17), (secondary, 18)):
                    self.assertEqual(client.command(b"\x16\3", 13)[0], 13)
                    self.assertEqual(client.command(bytes([1])+bytes(7), 5)[4:36], public(seed))
                for client in (base, second):
                    for _ in range(256):
                        self.assertEqual(client.command(b"\x0a", 16)[0], 16)
                    self.assertEqual(client.command(b"\x0a"), b"\x0a")
                self.assertEqual(secondary.command(b"\x0a"), b"\x0a")
                dashboard_config = root/"dashboard.conf"
                private(dashboard_config, (f"port=0\nstate={service.root}\n"
                    f"page={ROOT.parents[1]/'internal/app/admin_page.html'}\n"
                    f"base_port={health_ports[0]}\nbot_companion_port={health_ports[1]}\n"
                    "password_env=MESHCORE_COMBINED_FIXTURE_PASSWORD\n").encode())
                dashboard, line = launch("hew-dashboard-release", dashboard_config, "DASHBOARD_LISTEN ",
                    {"MESHCORE_HOST_ADMIN_HTTP": "1", "MESHCORE_COMBINED_FIXTURE_PASSWORD": "synthetic-password"})
                dashboard_port = int(line.split("port=")[1].split()[0])
                token = ""

                def http(path, data=b"", method="GET"):
                    connection = HTTPConnection("127.0.0.1", dashboard_port, timeout=10)
                    try:
                        connection.request(method, path, data, {"X-Host-Intent": "admin-v1", "X-Host-Admin": token})
                        response = connection.getresponse()
                        return response.status, response.read()
                    finally:
                        connection.close()

                self.until(lambda: http("/readyz")[0] == 200)
                status = json.loads(http("/status")[1])
                self.assertEqual(set(status), {"repeater", "room", "bot", "observer", "companion", "bot_companion"})
                for role, seed in (("repeater", 1), ("room", 4), ("bot", 5), ("observer", 3),
                                   ("companion", 17), ("bot_companion", 18)):
                    self.assertEqual(status[role]["public_key"], public(seed).hex())
                self.assertTrue(json.loads(http("/readyz")[1])["mqtt_connection_checked"])
                code, value = http("/admin/login", b"synthetic-password", "POST")
                self.assertEqual(code, 200)
                token = value.decode()
                self.assertEqual(http("/admin/command", b"bot name Combined Retained", "POST")[1],
                                 b"Saved and applied bot name; identity unchanged")
                self.assertEqual(http("/admin/role/room", b"set name Combined Room", "POST")[1], b"OK")

                raw = advert(2, "Authenticated RF source", int(time.time()))
                modem.receive(raw)
                service.emulator.send(3, 0, raw)
                subscriber.conn.settimeout(8)
                while True:
                    header, topic, payload, identifier = subscriber.read_publication()
                    if ((header >> 1) & 3) == 1:
                        subscriber.conn.sendall(frame(64, identifier.to_bytes(2, "big")))
                    if topic.endswith(b"/status"):
                        self.assertIn(payload, (b"online", b"offline"))
                        continue
                    observed = json.loads(payload)
                    if observed.get("raw_packet_hex") == raw.hex():
                        break
                self.assertEqual(topic, ("combined/"+public(3).hex()+"/packets").encode())
                self.assertFalse(observed["local_loopback"])
                stamp = int(time.time())
                message = addressed(2, 2, struct.pack("<IB", stamp, 0)+b"new Base message", recipient=17)
                modem.receive(message)
                for client in (base, second):
                    client.wait_rx(message)
                    self.assertTrue(client.command(b"\x0a", 16).endswith(b"new Base message"))
                self.assertEqual(secondary.command(b"\x0a"), b"\x0a")
                message = addressed(2, 2, struct.pack("<IB", stamp+1, 0)+b"new secondary message", recipient=18)
                modem.receive(message)
                secondary.wait_rx(message)
                self.assertTrue(secondary.command(b"\x0a", 16).endswith(b"new secondary message"))
                self.assertEqual(base.command(b"\x0a"), b"\x0a")

                stop(broker, "MQTT_BROKER_STOPPED")
                subscriber.close()
                clients.remove(subscriber)
                self.until(lambda: json.loads(http("/readyz")[1])["not_ready"].get("observer") == "mqtt_disconnected")
                self.assertEqual(http("/admin/role/room", b"get name", "POST")[1], b"> Combined Room")
                self.assertEqual(base.command(bytes([1])+bytes(7), 5)[4:36], public(17))
                broker, _ = launch("hew-broker-release", broker_config, "MQTT_BROKER_READY ")
                self.until(lambda: http("/readyz")[0] == 200)
                subscriber = MQTTClient(mqtt_port, "combined-after-broker-restart", keepalive=0)
                clients.append(subscriber)
                subscriber.subscribe([("combined/#", 1)])
                service.stop_process()
                self.until(lambda: http("/readyz")[0] == 503)
                service.start_process(True)
                self.until(lambda: http("/readyz")[0] == 200)
                self.assertEqual(http("/admin/command", b"bot name", "POST")[1], b"Name: Combined Retained")
                self.assertEqual(http("/admin/role/room", b"get name", "POST")[1], b"> Combined Room")

                for index, process in enumerate(companion_processes):
                    stop(process, "state_saved=true")
                    self.until(lambda: json.loads(http("/readyz")[1])["not_ready"].get(
                        ("companion", "bot_companion")[index]) == "health_connection_unavailable")
                    rollback = root/f"rollback-{index}"
                    transfer(directories[index], rollback, "rollback")
                    load_rollback_in_go(rollback, public((17, 18)[index]))
                    document = json.loads((rollback/"companion.json").read_bytes())
                    self.assertEqual(document["Sequence"], (295, 1)[index])
                    self.assertEqual(len(document["Messages"]), (256, 1)[index])
                    self.assertEqual(document["Preferences"]["future_preference"], {"enabled": True, "value": 99})
                    process, _ = launch("hew-base-release", configurations[index], "BASE_LISTEN ")
                    self.until(lambda: http("/readyz")[0] == 200)
                stop(dashboard)
            finally:
                for client in clients:
                    client.close()
                for process in reversed(processes):
                    if process.poll() is None:
                        process.terminate()
                        process.communicate(timeout=15)
                if service is not None:
                    service.close()
                modem.close()


if __name__ == "__main__":
    unittest.main(verbosity=2)
