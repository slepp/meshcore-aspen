"""Isolated clock/GPS RF, native restart and current Go location differential."""
import copy
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey

from parity import ROOT, Host, body, packet, parse, public, secret, seal, text

sys.path.insert(0, str(ROOT))
from reconcile_go import snapshot
from migrate_go import role_state

GO_SOURCE = Path(os.environ.get("MESHCORE_GO_SOURCE", ROOT.parents[1]))


def owner(host, command, now=1_000_000):
    return host.command(f"owner {now} {command.encode().hex()}")[-1]


def rf(host, command, stamp, now=1_000_000):
    frames = host.packet(text(2, stamp, command.encode(), 4), now=now)
    replies = [body(frame, 2) for frame in frames if parse(frame)[0] == 2]
    assert len(replies) == 1, frames
    return replies[0][5:].rstrip(b"\0").decode(), frames


class ClockLocation(unittest.TestCase):
    def test_current_go_location_command_and_signed_advert_differential(self):
        commands = ["gps advert", "get lat", "get lon", "set lat 53.546123456789",
            "set lon -113.493987654321", "get lat", "get lon", "gps advert prefs",
            "gps advert", "set lat -90", "set lon 180", "set lat 90.0000001",
            "set lon -180.0000001", "set lat NaN", "set lon +Inf", "set lat broken",
            "gps advert none", "gps advert", "set lat -0", "get lat"]
        with tempfile.TemporaryDirectory(dir=ROOT/"build") as name:
            directory = Path(name)
            source, destination, overlay = [directory/file for file in ("input", "output", "overlay")]
            source.write_text(json.dumps(commands))
            overlay.write_text(json.dumps({"Replace": {
                str(GO_SOURCE/"internal/roles/willow_location_oracle_test.go"):
                    str(ROOT/"tests/location_oracle_test.go.in")}}))
            result = subprocess.run(["go", "test", "-buildvcs=false", "-overlay", str(overlay),
                    "./internal/roles", "-run", "^TestWillowLocationOracle$", "-count=1"],
                cwd=GO_SOURCE, env=os.environ|{"MESHCORE_LOCATION_INPUT": str(source),
                    "MESHCORE_LOCATION_OUTPUT": str(destination)},
                capture_output=True, text=True, timeout=90)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
            expected = json.loads(destination.read_text())
            for binary in ("roles", "roles-release"):
                with Host(role=2, binary=ROOT/"build"/binary) as host:
                    for command, wanted in zip(commands, expected):
                        with self.subTest(binary=binary, command=command):
                            self.assertEqual(owner(host, command), wanted["Reply"])
                            path = directory/"state"
                            self.assertEqual(host.command("save "+str(path)), ["true"])
                            current = snapshot(path.read_bytes(), public(1), 2, "location")
                            settings = current["settings"] or {"Latitude": 0, "Longitude": 0, "AdvertLocation": 0}
                            self.assertEqual(settings["Latitude"], wanted["Latitude"])
                            self.assertEqual(settings["Longitude"], wanted["Longitude"])
                            self.assertEqual(settings["AdvertLocation"], wanted["Location"])
                            fixed = bytearray(path.read_bytes())
                            struct.pack_into("<I", fixed, 36, 4294967200)
                            path.write_bytes(fixed)
                            self.assertEqual(host.command("load "+str(path)), ["true"])
                            advert = parse(bytes.fromhex(host.command("advert 4294967201")[0].split()[1]))[4]
                            go = bytes.fromhex(wanted["Advert"])
                            self.assertEqual(advert, go)
                            Ed25519PublicKey.from_public_bytes(public(1)).verify(
                                advert[36:100], advert[:36]+advert[100:])
                            if settings["AdvertLocation"]:
                                self.assertEqual(struct.unpack("<ii", advert[101:109]),
                                    (int(settings["Latitude"]*1e6), int(settings["Longitude"]*1e6)))

    def test_rf_clock_gps_persistence_and_timers(self):
        for binary in ("roles", "roles-release"):
            with self.subTest(binary=binary), tempfile.TemporaryDirectory(dir=ROOT/"build") as directory:
                path = Path(directory)/"role.state"
                with Host(role=2, binary=ROOT/"build"/binary) as host:
                    logged_in = packet(7, public(1)[:1]+public(2)+seal(secret(2, public(1)),
                        struct.pack("<I", 1)+b"admin\0"))
                    self.assertTrue(host.packet(logged_in))
                    for command in ("set lat 53.546123456789", "set lon -113.493987654321", "gps advert prefs"):
                        self.assertIn(owner(host, command), ("OK", "ok"))
                    self.assertEqual(owner(host, "clock"), "00:16 - 1/1/1970 UTC")
                    self.assertEqual(owner(host, "clock sync"), "ERR: clock cannot go backwards")
                    self.assertEqual(rf(host, "clock sync", 2000)[0], "OK - clock set: 00:33 - 1/1/1970 UTC")
                    self.assertEqual(owner(host, "clock"), "00:33 - 1/1/1970 UTC")
                    for stamp, command in enumerate(("time 2001", "time -1", "time NaN",
                            "time 4294967296", "time 1_000", "time ++3000"), 2002):
                        reply, _ = rf(host, command, stamp)
                        self.assertEqual(reply, "ERR: clock cannot go backwards" if command == "time 2001"
                                         else "Error, invalid time")
                    reply, frames = rf(host, "time +3000", 2010)
                    self.assertEqual(reply, "OK - clock set: 00:50 - 1/1/1970 UTC")
                    self.assertGreaterEqual(int.from_bytes(body(frames[-1], 2)[:4], "little"), 3000)
                    # Manual adverts retain GPS coordinates and use the advanced clock.
                    reply, frames = rf(host, "advert", 2011)
                    self.assertEqual(reply, "OK - Advert sent")
                    advert = next(parse(frame)[4] for frame in frames if parse(frame)[0] == 4)
                    self.assertEqual(advert[100], 0x92)
                    self.assertGreaterEqual(int.from_bytes(advert[32:36], "little"), 3000)
                    self.assertEqual(struct.unpack("<ii", advert[101:109]), (53546123, -113493987))
                    self.assertEqual(owner(host, "set advert.interval 60"), "OK")
                    host.command("advert 1000")
                    self.assertEqual(host.command("tick 4599999"), [])
                    emitted = host.command("tick 4600000")
                    self.assertEqual(len(emitted), 1)
                    self.assertEqual(parse(emitted[0].split()[2])[4][100], 0x92)
                    # Anonymous replies use RTC time, not timer wall time.
                    query = packet(7, public(1)[:1]+public(2)+seal(secret(2, public(1)),
                        struct.pack("<IBB", 123, 1, 0)))
                    answers = host.packet(query)
                    self.assertTrue(answers)
                    self.assertEqual(int.from_bytes(body(answers[-1], 2)[4:8], "little"), 3000)
                    self.assertEqual(host.command("save "+str(path)), ["true"])
                with Host(role=2, binary=ROOT/"build"/binary) as host:
                    self.assertEqual(host.command("load "+str(path)), ["true"])
                    self.assertEqual(owner(host, "clock", 1_005_000), "00:50 - 1/1/1970 UTC")
                    current = snapshot(path.read_bytes(), public(1), 2, "restart")["settings"]
                    self.assertEqual(current["RTCOffset"], 2000)
                    self.assertEqual(current["Latitude"], 53.546123456789)
                    self.assertEqual(current["Longitude"], -113.493987654321)
                    self.assertEqual(current["AdvertLocation"], 2)
                    self.assertEqual(owner(host, "gps advert none"), "ok")
                    advert = parse(host.command("advert 1006")[0].split()[1])[4]
                    self.assertEqual(advert[100], 0x82)
                    self.assertEqual(advert[101:], b"Hew Room")

    def test_offline_go_clock_and_location_roundtrip_and_legacy(self):
        prefs = {"version": 1, "path_hash_mode": 2, "repeat": False, "loop": 0,
            "local_advert_seconds": 0, "flood_advert_seconds": 0, "regions": []}
        state = {"Version": 2, "Identity": public(1).hex(), "Room": False, "Retention": "durable-replay",
            "Preferences": prefs, "Clock": 0, "Posted": 0, "Pushed": 0, "Members": {}, "MemberOrder": [],
            "Latitude": 53.546123456789, "Longitude": -113.493987654321, "AdvertLocation": 2, "RTCOffset": -86401}
        config = {"relay.name": "Imported", "password": "guest", "admin": "admin"}
        files = {"repeater/identity.seed": bytes([1])*32, "repeater/state.json": json.dumps(state).encode()}
        native, _ = role_state(files, "repeater", "relay", 2, public(1), config)
        decoded = snapshot(native, public(1), 2, "migration")["settings"]
        for key in ("Latitude", "Longitude", "AdvertLocation", "RTCOffset"):
            self.assertEqual(decoded[key], state[key])
        with tempfile.TemporaryDirectory(dir=ROOT/"build") as directory:
            path = Path(directory)/"imported"
            path.write_bytes(native)
            path.chmod(0o600)
            for binary in ("roles", "roles-release"):
                with Host(role=2, binary=ROOT/"build"/binary) as host:
                    self.assertEqual(host.command("load "+str(path)), ["true"])
                    self.assertEqual(host.command("save "+str(path)), ["true"])
                    returned = snapshot(path.read_bytes(), public(1), 2, "restart")["settings"]
                    for key in ("Latitude", "Longitude", "AdvertLocation", "RTCOffset"):
                        self.assertEqual(returned[key], state[key])
        for key, value in (("Latitude", 91), ("Longitude", -181), ("AdvertLocation", 1), ("RTCOffset", 1 << 63)):
            bad = copy.deepcopy(state)
            bad[key] = value
            with self.assertRaises(ValueError):
                role_state(files|{"repeater/state.json": json.dumps(bad).encode()},
                    "repeater", "relay", 2, public(1), dict(config))


if __name__ == "__main__":
    unittest.main(verbosity=2)
