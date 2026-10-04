"""Receive-score arithmetic, bounded flood holds, restart and Go rollback."""
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import time
import unittest

from parity import ROOT, Host, packet, parse, public, tx, seal, secret
from clock_location import owner
from service_demo import RunningService, PROFILE, addressed, body as service_body

sys.path.insert(0, str(ROOT))
from migrate_go import role_state
from reconcile_go import snapshot
from willow import owner_rpc

GO_SOURCE = Path(os.environ.get("MESHCORE_GO_SOURCE", ROOT.parents[1]))
TABLE = struct.pack("<256I", 0, *([100]*255))


def setup(host, airtime=TABLE):
    host.command("profile "+PROFILE.hex())
    assert host.command("airtime "+airtime.hex()) == ["true"]
    host.command("measurement "+struct.pack("<ffB", -73, -7.5, 0).hex())


class ReceivePolicy(unittest.TestCase):
    def test_current_go_arithmetic_scores_and_commands(self):
        cases = {
            "Delays": [{"Base": base, "Score": score, "Airtime": airtime}
                for base in (0, .1, 1, 2, 10, 20) for score in (0, .25, .85, 1)
                for airtime in (1, 100, 32000, 3_600_000, 4_294_967_295)],
            "Scores": [{"SNR": snr, "SF": sf, "Length": size}
                for snr in (-32, -20, -7.5, -7.25, 0, 12.5, 31.75)
                for sf in range(5, 13) for size in (1, 32, 128, 255, 256)],
            "Commands": ["get rxdelay", "set rxdelay 20", "get rxdelay", "set rxdelay -1",
                "set rxdelay 20.1", "set rxdelay NaN", "set rxdelay +Inf", "set rxdelay bad",
                "set rxdelay 0.2", "get rxdelay", "set rxdelay 0", "get rxdelay"],
        }
        with tempfile.TemporaryDirectory(dir=ROOT/"build") as name:
            directory = Path(name)
            source, destination, overlay = [directory/file for file in ("input", "output", "overlay")]
            source.write_text(json.dumps(cases))
            overlay.write_text(json.dumps({"Replace": {
                str(GO_SOURCE/"internal/roles/willow_rx_policy_oracle_test.go"):
                    str(ROOT/"tests/rx_policy_oracle_test.go.in")}}))
            result = subprocess.run(["go", "test", "-buildvcs=false", "-overlay", str(overlay),
                "./internal/roles", "-run", "^TestWillowReceivePolicyOracle$", "-count=1"],
                cwd=GO_SOURCE, env=os.environ|{"MESHCORE_RX_INPUT": str(source),
                    "MESHCORE_RX_OUTPUT": str(destination)}, capture_output=True, text=True, timeout=90)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
            expected = json.loads(destination.read_text())
            for binary in ("roles", "roles-release"):
                with Host(role=2, binary=ROOT/"build"/binary) as host:
                    setup(host)
                    for test, wanted in zip(cases["Delays"], expected["Delays"]):
                        command = f"rxdelay {struct.pack('<f', test['Base']).hex()} {struct.pack('<f', test['Score']).hex()} {test['Airtime']}"
                        self.assertEqual(host.command(command), [wanted], (binary, test))
                    for test, wanted in zip(cases["Scores"], expected["Scores"]):
                        command = f"score {struct.pack('<f', test['SNR']).hex()} {test['SF']} {test['Length']}"
                        self.assertEqual(host.command(command), [wanted], (binary, test))
                    for command, wanted in zip(cases["Commands"], expected["Commands"]):
                        self.assertEqual(owner(host, command), wanted, (binary, command))

    def test_exact_hold_bounds_metadata_bypass_queue_and_restart(self):
        for binary in ("roles", "roles-release"):
            with self.subTest(binary=binary), tempfile.TemporaryDirectory(dir=ROOT/"build") as name:
                path = Path(name)/"state"
                with Host(role=2, binary=ROOT/"build"/binary) as host:
                    self.assertIn("airtime estimator", owner(host, "set rxdelay 2"))
                    setup(host)
                    self.assertEqual(owner(host, "set rxdelay 2"), "OK")
                    raw = packet(5, b"held", route=1)
                    # float32(pow(2,float32(.85)))-1, multiplied by 100, truncates to 80ms.
                    self.assertEqual(host.packet(raw, now=1000), [])
                    self.assertEqual(host.command("tick 1079"), [])
                    self.assertEqual(len(tx(host.command("tick 1080"))), 1)
                    self.assertEqual(host.command("tick 1081"), [])
                    self.assertEqual(host.packet(raw, now=1100), [])
                    self.assertEqual(host.command("tick 1180"), [])
                    direct = packet(5, b"direct", path=public(1)[:1]+b"x")
                    self.assertEqual(len(host.packet(direct, now=1200)), 1)
                    self.assertEqual(host.packet(packet(5, b"local", route=1), now=1300, local=True), [])
                    self.assertEqual(host.command("tick 1380"), [])
                    # A score failure allows delivery but cannot authorize relay.
                    host.command("measurement "+struct.pack("<ffB", 0, 0, 2).hex())
                    self.assertEqual(host.packet(packet(5, b"missing-signal", route=1), now=1400), [])
                    self.assertEqual(host.command("tick 1500"), [])
                    setup(host, struct.pack("<256I", 0, *([3_600_000]*255)))
                    self.assertEqual(owner(host, "set rxdelay 20"), "OK")
                    self.assertEqual(host.packet(packet(5, b"capped", route=1), now=2000), [])
                    self.assertEqual(host.command("tick 33999"), [])
                    self.assertEqual(len(tx(host.command("tick 34000"))), 1)
                    # Fill exactly 64 slots; the next packet is explicitly dropped.
                    setup(host)
                    self.assertEqual(owner(host, "set rxdelay 2"), "OK")
                    for index in range(64):
                        self.assertEqual(host.packet(packet(5, bytes([index]), route=1), now=40000), [])
                    overflow = host.command("packet 40000 0 "+packet(5, b"overflow", route=1).hex())
                    self.assertTrue(any("ROLE_RX_QUEUE_FULL" in line for line in overflow), overflow)
                    self.assertEqual(len(tx(host.command("tick 40080"))), 64)
                    self.assertEqual(host.command("tick 40081"), [])
                    self.assertEqual(host.packet(packet(5, b"unfinished", route=1), now=50000), [])
                    self.assertEqual(host.command("save "+str(path)), ["true"])
                    self.assertEqual(host.command("load "+str(path)), ["true"])
                    self.assertEqual(host.command("tick 50080"), [])
                    saved = snapshot(path.read_bytes(), public(1), 2, "rxdelay")["settings"]["Preferences"]
                    self.assertEqual(saved["rxdelay"], 2)
                    self.assertEqual(saved["airtime_factor"], 1)
                    setup(host)
                    self.assertEqual(owner(host, "get rxdelay"), "> 2.0")
                    self.assertEqual(owner(host, "set rxdelay 0.1"), "OK")
                    # Signed negative delay is not a hold.
                    self.assertEqual(len(host.packet(packet(5, b"negative", route=1), now=51000)), 1)
                    self.assertEqual(owner(host, "set rxdelay 2"), "OK")
                    setup(host, struct.pack("<256I", 0, *([1]*255)))
                    # A positive duration below 50ms is not a hold.
                    self.assertEqual(len(host.packet(packet(5, b"short", route=1), now=52000)), 1)

    def test_actual_native_service_hold_and_healthy_other_roles(self):
        with RunningService(root=ROOT.parents[1]/f".rx-policy-{os.getpid()}-{time.monotonic_ns():x}") as service:
            modem = service.emulator
            modem.send(0, 0, packet(7, public(1)[:1]+public(2)+seal(secret(2, public(1)),
                struct.pack("<I", 10)+b"admin\0")))
            modem.receive(lambda item: item["port"] == 0 and parse(item["raw"])[0] == 1)
            stamp = 10
            def command(text):
                nonlocal stamp
                stamp += 1
                modem.send(0, 0, addressed(2, 1, 2, struct.pack("<IB", stamp, 4)+text.encode()))
                reply = modem.receive(lambda item: item["port"] == 0 and parse(item["raw"])[0] == 2)
                return service_body(reply["raw"], 2, 1)[5:].rstrip(b"\0").decode()
            self.assertEqual(command("set rxdelay 20"), "OK")
            self.assertEqual(command("get rxdelay"), "> 20.0")
            self.assertEqual(owner_rpc(service.root, bytes((1, 5, 2))+b"get rxdelay").decode(), "> 20.0")
            saved = snapshot((service.root/"relay.state").read_bytes(), public(1), 2, "service rx")
            self.assertEqual(saved["settings"]["Preferences"]["rxdelay"], 20)
            raw = packet(5, b"physical flood hold", route=1)
            started = time.monotonic()
            modem.send(0, 0, raw, metadata=b"\xf9\xe2\xb7")  # SNR=-7.5dB.
            direct = packet(5, b"direct bypass", path=public(1)[:1]+b"x")
            modem.send(0, 0, direct)
            immediate = modem.receive(lambda item: item["port"] == 0 and parse(item["raw"])[4] == b"direct bypass")
            self.assertLess(time.monotonic()-started, .30, immediate)
            held = modem.receive(lambda item: item["port"] == 0 and parse(item["raw"])[4] == b"physical flood hold")
            duration = time.monotonic()-started
            size = len(raw)
            exponent = struct.unpack("<f", struct.pack("<f", .85))[0]
            power = struct.unpack("<f", struct.pack("<f", 20**exponent))[0]
            expected = int((power-1)*(10+size))/1000
            self.assertGreaterEqual(duration, expected-.02)
            self.assertLess(duration, expected+.35)
            self.assertEqual(parse(held["raw"])[4], b"physical flood hold")
            service.stop_process()
            service.start_process(True)
            self.assertEqual(command("get rxdelay"), "> 20.0")
            self.assertEqual(sum(item["raw"] == held["raw"] for item in modem.all_submissions), 1)
            self.assertFalse(any("ROLE_OFFLINE" in line for line in service.logs), service.logs)

    def test_go_native_go_receive_setting_preserved(self):
        prefs = {"version": 1, "path_hash_mode": 2, "repeat": False, "loop": 0,
            "local_advert_seconds": 0, "flood_advert_seconds": 0, "regions": [], "rxdelay": .25}
        state = {"Version": 2, "Identity": public(1).hex(), "Room": False, "Retention": "durable-replay",
            "Preferences": prefs, "Clock": 0, "Posted": 0, "Pushed": 0, "Members": {}, "MemberOrder": []}
        config = {"relay.name": "Imported", "password": "room", "admin": "admin"}
        files = {"repeater/identity.seed": bytes([1])*32, "repeater/state.json": json.dumps(state).encode()}
        native, _ = role_state(files, "repeater", "relay", 2, public(1), config)
        with tempfile.TemporaryDirectory(dir=ROOT/"build") as name:
            path = Path(name)/"state"
            path.write_bytes(native); path.chmod(0o600)
            for binary in ("roles", "roles-release"):
                with Host(role=2, binary=ROOT/"build"/binary) as host:
                    self.assertEqual(host.command("load "+str(path)), ["true"])
                    self.assertEqual(owner(host, "get rxdelay"), "> 0.25")
                    self.assertEqual(host.command("save "+str(path)), ["true"])
                    returned = snapshot(path.read_bytes(), public(1), 2, "rx roundtrip")["settings"]["Preferences"]
                    self.assertEqual(returned["rxdelay"], .25)


if __name__ == "__main__":
    unittest.main(verbosity=2)
