"""Commit-first role source factors, modem confirmation, isolation and rollback."""
import copy
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import queue
import struct
import subprocess
import sys
import tempfile
import time
import unittest

from parity import ROOT, Host, packet, parse, public, seal, secret
from clock_location import owner
from rx_policy import GO_SOURCE, setup
from service_demo import RunningService, addressed, body

sys.path.insert(0, str(ROOT))
from migrate_go import role_state
from reconcile_go import snapshot
from willow import owner_rpc


class RoleClient:
    def __init__(self, service, port):
        self.service, self.port = service, port
        self.seed = (1, 4)[port]
        self.stamp = 10
        self.login()

    def login(self):
        self.stamp += 1
        plain = struct.pack("<I", self.stamp)+(bytes(4) if self.port == 1 else b"")+b"admin\0"
        self.service.emulator.send(self.port, 0, packet(7, public(self.seed)[:1]+public(2)+seal(secret(2, public(self.seed)), plain)))
        self.service.emulator.receive(lambda item: item["port"] == self.port and parse(item["raw"])[0] == 1)

    def send(self, command):
        self.stamp += 1
        self.service.emulator.send(self.port, 0, addressed(2, self.seed, 2,
            struct.pack("<IB", self.stamp, 4)+command.encode()))

    def reply(self):
        reply = self.service.emulator.receive(lambda item: item["port"] == self.port and parse(item["raw"])[0] == 2)
        return body(reply["raw"], 2, self.seed)[5:].rstrip(b"\0").decode()

    def command(self, command):
        self.send(command)
        return self.reply()


def saved(service, port):
    name, seed = (("relay", 1), ("room", 4))[port]
    return snapshot((service.root/(name+".state")).read_bytes(), public(seed), port+2, "source policy")["settings"]


class SourcePolicy(unittest.TestCase):
    def test_current_go_full_binary32_domain_and_duty_commands(self):
        commands = ["get af", "get dutycycle", "set af 0", "get af", "get dutycycle",
            "set dutycycle 1", "get af", "get dutycycle", "set dutycycle 50",
            "set dutycycle 100", "get af", "get dutycycle", "set dutycycle 33.3",
            "get af", "get dutycycle", "set af 3.4028234663852886e38", "get af", "get dutycycle",
            "set af 1e-45", "get af", "get dutycycle", "set af -0", "get af",
            "set af -1", "set af NaN", "set af +Inf", "set af 1e39", "set af bad",
            "set dutycycle .9", "set dutycycle 100.1", "set dutycycle -1",
            "set dutycycle NaN", "set dutycycle +Inf", "set dutycycle bad", "set af 2"]
        with tempfile.TemporaryDirectory(dir=ROOT/"build") as name:
            directory = Path(name)
            source, destination, overlay = [directory/file for file in ("input", "output", "overlay")]
            source.write_text(json.dumps({"Commands": commands}))
            overlay.write_text(json.dumps({"Replace": {
                str(GO_SOURCE/"internal/roles/willow_rx_policy_oracle_test.go"):
                    str(ROOT/"tests/rx_policy_oracle_test.go.in")}}))
            result = subprocess.run(["go", "test", "-buildvcs=false", "-overlay", str(overlay),
                "./internal/roles", "-run", "^(TestWillowReceivePolicyOracle|TestRuntimeAirtimeDomainAndPreferenceReloadProfiles|TestSourcePolicyCommitsBeforeDeviceApplicationAndFailsClosed)$", "-count=1"],
                cwd=GO_SOURCE, env=os.environ|{"MESHCORE_RX_INPUT": str(source),
                    "MESHCORE_RX_OUTPUT": str(destination)}, capture_output=True, text=True, timeout=90)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
            expected = json.loads(destination.read_text())["Commands"]
            for binary in ("roles", "roles-release"):
                with Host(role=2, binary=ROOT/"build"/binary) as host:
                    setup(host)
                    self.assertIn("not connected", owner(host, "set af 2"))
                    host.command("source-policy")
                    for command, wanted in zip(commands, expected):
                        self.assertEqual(owner(host, command), wanted, (binary, command))

    def test_native_durable_boot_profiles_and_go_roundtrip(self):
        for profile, boot in (("native-preferences", 9), ("durable-host-preferences", 99)):
            prefs = {"version": 1, "path_hash_mode": 2, "repeat": False, "loop": 0,
                "local_advert_seconds": 0, "flood_advert_seconds": 0, "regions": [], "airtime_factor": 99,
                "txdelay": .12345678, "direct_txdelay": .98765432}
            state = {"Version": 2, "Identity": public(1).hex(), "Room": False, "Retention": "durable-replay",
                "PreferenceProfile": profile, "Preferences": prefs, "Clock": 0, "Posted": 0, "Pushed": 0,
                "Members": {}, "MemberOrder": []}
            config = {"relay.name": "Imported", "password": "room", "admin": "admin"}
            files = {"repeater/identity.seed": bytes([1])*32, "repeater/state.json": json.dumps(state).encode()}
            native, _ = role_state(files, "repeater", "relay", 2, public(1), config)
            self.assertEqual(snapshot(native, public(1), 2, "source import")["settings"]["Preferences"]["airtime_factor"], 99)
            with tempfile.TemporaryDirectory(dir=ROOT/"build") as name:
                path = Path(name)/"state"
                for binary in ("roles", "roles-release"):
                    path.write_bytes(native); path.chmod(0o600)
                    with Host(role=2, binary=ROOT/"build"/binary) as host:
                        setup(host); host.command("source-policy")
                        self.assertEqual(host.command("load "+str(path)), ["true"])
                        self.assertEqual(owner(host, "get af"), f"> {boot}.0")
                        self.assertEqual(host.command("save "+str(path)), ["true"])
                        result = snapshot(path.read_bytes(), public(1), 2, "source boot")["settings"]
                        self.assertEqual(result["Preferences"]["airtime_factor"], boot)
                        self.assertEqual(result["PreferenceProfile"], profile)
                        for key in ("txdelay", "direct_txdelay"):
                            self.assertEqual(result["Preferences"][key], struct.unpack("<f", struct.pack("<f", prefs[key]))[0])
                        self.assertEqual(owner(host, "set af 99"), "OK")
                        self.assertEqual(owner(host, "get af"), "> 99.0")
                        self.assertEqual(host.command("save "+str(path)), ["true"])
                        self.assertEqual(snapshot(path.read_bytes(), public(1), 2, "runtime")["settings"]["Preferences"]["airtime_factor"], 99)
                        self.assertEqual(host.command("load "+str(path)), ["true"])
                        self.assertEqual(owner(host, "get af"), f"> {boot}.0")
            for factor in (-1, float("nan"), float("inf"), 1e39):
                bad = copy.deepcopy(state); bad["Preferences"]["airtime_factor"] = factor
                with self.assertRaises(ValueError):
                    role_state(files|{"repeater/state.json": json.dumps(bad).encode()},
                        "repeater", "relay", 2, public(1), dict(config))

    def test_commit_before_apply_pending_confirmation_and_isolation(self):
        with RunningService() as service:
            modem = service.emulator
            room, relay = RoleClient(service, 1), RoleClient(service, 0)
            modem.policy_before_apply = lambda port, factor: self.assertEqual(saved(service, port)["Preferences"]["airtime_factor"], factor)
            modem.policy_mode = "hold"
            room.send("set af 2")
            request = modem.policy_requests.get(timeout=3)
            self.assertEqual((request["port"], request["factor"]), (1, 2))
            self.assertEqual(saved(service, 1)["Preferences"]["airtime_factor"], 2)
            before = len([item for item in modem.all_submissions if item["port"] == 1])
            # A pending policy pauses only that role; no success is sent before acknowledgement.
            room.send("get af")
            started = time.monotonic()
            self.assertIn("Willow-Relay", relay.command("get name"))
            self.assertLess(time.monotonic()-started, .5)
            started = time.monotonic()
            owner_rpc(service.root, bytes((1, 6, 0)))
            self.assertLess(time.monotonic()-started, .5)
            self.assertEqual(len([item for item in modem.all_submissions if item["port"] == 1]), before)
            with ThreadPoolExecutor(max_workers=1) as waiting:
                queried = waiting.submit(owner_rpc, service.root, bytes((1, 5, 3))+b"get af")
                relay.command("get name")
                self.assertFalse(queried.done())
                health = json.loads(owner_rpc(service.root, bytes((1, 6, 0))))
                self.assertEqual(health["room"]["state"], "running", health)
                modem.policy_mode = "ok"
                modem.send(1, 6, b"\xa3\1\0"+request["bits"])
                self.assertEqual(room.reply(), "OK")
                self.assertEqual(room.reply(), "> 2.0")
                self.assertEqual(queried.result(timeout=5), b"> 2.0")
            self.assertEqual(room.command("set af 2"), "OK")
            with self.assertRaises(queue.Empty): modem.policy_requests.get_nowait()
            self.assertEqual(room.command("set dutycycle 1"), "OK - 1.0%")
            request = modem.policy_requests.get(timeout=3)
            self.assertEqual(request["factor"], 99)
            self.assertEqual(room.command("get af"), "> 99.0")
            service.stop_process()
            service.start_process(True)
            request = modem.policy_requests.get(timeout=3)
            self.assertEqual(request["factor"], 9)
            room.login()
            self.assertEqual(room.command("get af"), "> 9.0")
            modem.disconnect()
            service.wait("ONLINE epoch=2", 10)
            request = modem.policy_requests.get(timeout=3)
            self.assertEqual(request["factor"], 9)
            self.assertEqual(room.command("get af"), "> 9.0")
            self.assertFalse(any("ROLE_OFFLINE" in line for line in service.logs), service.logs)

    def test_failure_suppresses_success_and_keeps_other_role_healthy(self):
        for mode in ("malformed", "reject", "hold", "disconnect"):
            with self.subTest(mode=mode), RunningService() as service:
                modem = service.emulator
                room, relay = RoleClient(service, 1), RoleClient(service, 0)
                modem.policy_mode = "hold" if mode == "disconnect" else mode
                before = len([item for item in modem.all_submissions if item["port"] == 1])
                room.send("set af 3")
                request = modem.policy_requests.get(timeout=3)
                if mode == "disconnect":
                    modem.disconnect()
                service.wait("source airtime policy unconfirmed", 7)
                self.assertEqual(saved(service, 1)["Preferences"]["airtime_factor"], 3)
                self.assertEqual(len([item for item in modem.all_submissions if item["port"] == 1]), before)
                if mode == "disconnect": service.wait("ONLINE epoch=2", 10)
                self.assertIn("Willow-Relay", relay.command("get name"))
                # A late matching response cannot reopen the failed role or replay its command.
                modem.send(1, 6, b"\xa3\1\0"+request["bits"])
                relay.command("get name")
                self.assertEqual(len([item for item in modem.all_submissions if item["port"] == 1]), before)
                self.assertEqual(len([item for item in modem.controls if item[2] == 35 and item[3].endswith(request["bits"].hex())]), 1)

    def test_failed_snapshot_never_applies_source_policy(self):
        with RunningService() as service:
            modem = service.emulator
            room, relay = RoleClient(service, 1), RoleClient(service, 0)
            retained = (service.root/"room.state").read_bytes()
            pending = service.root/"room.state.pending"
            pending.write_bytes(b"occupied transaction slot"); pending.chmod(0o600)
            room.send("set af 4")
            service.wait("role snapshot commit failed", 5)
            self.assertEqual((service.root/"room.state").read_bytes(), retained)
            with self.assertRaises(queue.Empty): modem.policy_requests.get_nowait()
            self.assertIn("Willow-Relay", relay.command("get name"))
            pending.unlink()

    def test_reconnect_policy_failure_retries_without_role_quarantine(self):
        with RunningService() as service:
            modem = service.emulator
            room = RoleClient(service, 1)
            self.assertEqual(room.command("set af 2"), "OK")
            modem.policy_requests.get(timeout=3)
            retained_clock = saved(service, 1)
            clock_bytes = (service.root/"room.state").read_bytes()[36:40]
            flood_reservation = (service.root/"advert-times").read_bytes()
            before = len(service.logs)
            modem.policy_mode = "hold"
            modem.disconnect()
            first = modem.policy_requests.get(timeout=5)
            self.assertEqual(first["factor"], 2)
            self.assertEqual((service.root/"room.state").read_bytes()[36:40], clock_bytes)
            self.assertEqual((service.root/"advert-times").read_bytes(), flood_reservation)
            # A source restore belongs to connection negotiation, not the runtime CLI transaction.
            modem.disconnect()
            second = modem.policy_requests.get(timeout=5)
            self.assertEqual(second["factor"], 2)
            self.assertEqual((service.root/"room.state").read_bytes()[36:40], clock_bytes)
            self.assertEqual((service.root/"advert-times").read_bytes(), flood_reservation)
            modem.policy_mode = "ok"
            modem.send(1, 6, b"\xa3\1\0"+second["bits"])
            service.wait("ONLINE epoch=2", 10, after=before)
            self.assertEqual(room.command("get af"), "> 2.0")
            self.assertFalse(any("ROLE_OFFLINE" in line for line in service.logs), service.logs)
            self.assertEqual(saved(service, 1)["Preferences"], retained_clock["Preferences"])

    def test_runtime_ack_after_two_seconds_is_still_accepted(self):
        with RunningService() as service:
            modem = service.emulator
            room, relay = RoleClient(service, 1), RoleClient(service, 0)
            modem.policy_mode = "hold"
            room.send("set af 4")
            request = modem.policy_requests.get(timeout=3)
            time.sleep(2.5)
            self.assertIn("Willow-Relay", relay.command("get name"))
            modem.policy_mode = "ok"
            modem.send(1, 6, b"\xa3\1\0"+request["bits"])
            self.assertEqual(room.reply(), "OK")
            self.assertEqual(room.command("get af"), "> 4.0")
            self.assertFalse(any("ROLE_OFFLINE" in line for line in service.logs), service.logs)


if __name__ == "__main__":
    unittest.main(verbosity=2)
