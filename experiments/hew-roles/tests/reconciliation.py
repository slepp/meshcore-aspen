"""One Go -> running Willow -> new Go application lifecycle, on localhost only."""
import base64
import copy
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import time
import unittest

from migration import Migration
from migrate_go import hashes, inventory, migrate, write_private
from reconcile_go import merge_state, reconcile, snapshot
from parity import ROOT, public, packet, parse, seal, secret
from service_demo import RunningService, Emulator, advert, addressed, body

ADMIN = "isolated-runtime-admin-credential"


def login(peer, recipient, stamp, password=ADMIN, since=0):
    return packet(7, public(recipient)[:1]+public(peer)+seal(secret(peer, public(recipient)),
                  struct.pack("<I", stamp)+(struct.pack("<I", since) if recipient == 4 else b"")+password.encode()+b"\0"))


def command(recipient, stamp, text, peer=2, flags=4):
    return addressed(peer, recipient, 2, struct.pack("<IB", stamp, flags)+text.encode())


class Reconciliation(Migration):
    def test_lifecycle(self):
        source = self.root/"reconcile-source"
        shutil.copytree(self.source, source)
        room_original = json.loads((source/"room/state.json").read_text())
        seed = bytes([4])*32
        expanded = bytearray(hashlib.sha512(seed).digest())
        expanded[0] &= 248
        expanded[31] = (expanded[31]&63)|64
        write_private(source/"room/identity-state.json", json.dumps({
            "version": 1, "identity": base64.b64encode(expanded).decode(),
            "document": "state.json", "state": room_original}).encode())
        (source/"room/state.json").write_bytes(b'{"stale":"keep me non-authoritative"}')
        write_private(source/"companion/opaque-extension.json", b'{"principals":{"base":"retained"}}')
        source_before = hashes(inventory(source))
        service = RunningService.__new__(RunningService)
        service.root = self.root/"reconcile-willow"
        service.emulator = Emulator()
        service.emulator.identity_root = service.root
        service.logs, service.errors = [], []
        config = self.root/"reconcile-config"
        self.write_config(config, service.emulator.port)
        config.write_text(config.read_text().replace("admin=admin\n", "admin="+ADMIN+"\n"))
        migrate(source, config, service.root)
        untouched = self.root/"untouched-go"
        reconcile(service.root, untouched)
        untouched_state = json.loads((untouched/"room/identity-state.json").read_text())["state"]
        for key in ("Members", "MemberOrder", "History", "Clock", "Posted", "Pushed", "HomeRegion"):
            self.assertEqual(untouched_state[key], room_original[key], key)
        self.assertEqual((untouched/"room/state.json").read_bytes(), (source/"room/state.json").read_bytes())
        stamp = int(time.time())-30
        try:
            service.start_process(True, runner=True)
            modem = service.emulator
            with self.assertRaisesRegex(ValueError, "locked"):
                reconcile(service.root, self.root/"must-not-exist")
            modem.send(2, 0, packet(4, parse(advert(2))[4], route=2, width=3))
            time.sleep(.1)
            for offset, text, expected in (
                    (1, "!remember migration updated native data", b"committed"),
                    (2, "!remember handover added native note", b"committed")):
                modem.send(2, 0, command(5, stamp+offset, text, flags=0))
                reply = modem.receive(lambda item: item["port"] == 2 and parse(item["raw"])[0] == 2)
                self.assertIn(expected, body(reply["raw"], 2, 5))
            for port, recipient in ((0, 1), (1, 4)):
                modem.send(port, 0, login(2, recipient, stamp+3, since=102))
                modem.receive(lambda item: item["port"] == port and parse(item["raw"])[0] == 1)
                updates = ["set owner.info returned|operator", "set loop.detect moderate"]
                if port == 1:
                    updates += ["set name Returned-Room", "set multi.acks 1"]
                for offset, text in enumerate(updates, 4):
                    modem.send(port, 0, command(recipient, stamp+offset, text))
                    reply = modem.receive(lambda item: item["port"] == port and parse(item["raw"])[0] == 2)
                    self.assertEqual(body(reply["raw"], 2, recipient)[5:].rstrip(b"\0"), b"OK")
            modem.send(1, 0, addressed(2, 4, 8, b"\x81\xaa\xbb\xcc\x0f"))
            modem.send(1, 0, login(7, 4, stamp+8, password="room", since=102))
            modem.receive(lambda item: item["port"] == 1 and parse(item["raw"])[0] == 1)
            modem.send(1, 0, addressed(7, 4, 8, b"\x80\x0f"))
            modem.send(1, 0, command(4, stamp+9, "post accepted during Willow handover", flags=0))
            modem.receive(lambda item: item["port"] == 1 and parse(item["raw"])[0] == 3)
            delivered = modem.receive(lambda item: item["port"] == 1 and parse(item["raw"])[0] == 2
                                     and parse(item["raw"])[4][0] == public(7)[0], timeout=15)
            plain = body(delivered["raw"], 7, 4).rstrip(b"\0")
            self.assertTrue(plain.endswith(b"post accepted during Willow handover"), plain)
            post_stamp = int.from_bytes(plain[:4], "little")
            modem.send(1, 0, packet(3, hashlib.sha256(plain+public(7)).digest()[:4]))
            end = time.monotonic()+3
            while time.monotonic() < end:
                current = snapshot((service.root/"room.state").read_bytes(), public(4), 3, "room")
                if current["Members"][public(7).hex()]["SyncSince"] == post_stamp:
                    break
                time.sleep(.02)
            else:
                self.fail("room history ACK was not durably committed")
            for offset, text, expected in (
                    (19, "log start", b"   logging on"),
                    (20, "set lat 53.5", b"OK"),
                    (21, "set lon -113.25", b"OK"),
                    (22, "gps advert prefs", b"ok"),
                    (23, "time "+str(int(time.time())+60), b"OK - clock set:"),
                    (24, "set rxdelay 0.25", b"OK"),
                    (25, "set af 2", b"OK")):
                modem.send(1, 0, command(4, stamp+offset, text))
                reply = modem.receive(lambda item: item["port"] == 1 and parse(item["raw"])[0] == 2)
                self.assertTrue(body(reply["raw"], 2, 4)[5:].rstrip(b"\0").startswith(expected))
            deadline = time.monotonic()+5
            while time.monotonic() < deadline:
                captured = (service.root/"room.state.packet.log").read_bytes().splitlines(keepends=True)
                if any(json.loads(line)["direction"] == "TX" for line in captured if line.endswith(b"\n")):
                    break
                time.sleep(.02)
            else:
                self.fail("room packet log did not record a confirmed transmission")
            service.stop_process()
            # Explicit config edits override the migrated regional baseline,
            # while unchanged config must preserve later owner changes.
            config_path = service.root/"config"
            cfg = dict(line.split("=", 1) for line in config_path.read_text().splitlines())
            key = hashlib.sha256(b"#fixture-b").digest()[:16].hex()
            cfg["room.home"], cfg["room.default_scope"] = key, key
            cfg["relay.wildcard"] = "0"
            config_path.write_text("".join(k+"="+v+"\n" for k, v in cfg.items()))
            service.start_process(True, runner=True)
            service.stop_process()
            latest_go = self.root/"independent-go"
            shutil.copytree(source, latest_go)
            (latest_go/"companion/opaque-extension.json").write_bytes(b'{"principals":{"base":"newer independent state"}}')
            frozen = hashes(inventory(service.root, retained=("rollback-go",)))
            destination = ROOT.parents[1]/f".reconciled-{os.getpid()}"
            self.addCleanup(lambda: shutil.rmtree(destination) if destination.exists() else None)
            report = reconcile(service.root, destination, latest_go)
            self.assertEqual(report["required_go_runtime"], {"admin_password": ADMIN})
            self.assertEqual(hashes(inventory(service.root, retained=("rollback-go",))), frozen)
            self.assertEqual(hashes(inventory(source)), source_before)
            self.assertEqual((destination/"companion/opaque-extension.json").read_bytes(),
                             (latest_go/"companion/opaque-extension.json").read_bytes())
            returned = json.loads((destination/"room/identity-state.json").read_text())["state"]
            current = snapshot((service.root/"room.state").read_bytes(), public(4), 3, "room")
            for name in ("Clock", "Posted", "Pushed", "Members", "MemberOrder", "History"):
                self.assertEqual(returned[name], current[name], name)
            self.assertEqual(returned["HomeRegion"], 2)
            self.assertEqual(returned["DefaultRegion"], 2)
            self.assertEqual(returned["Name"], "Returned-Room")
            self.assertEqual(returned["Preferences"]["owner_info"], "returned\noperator")
            self.assertEqual(returned["Preferences"]["loop"], 2)
            self.assertEqual(returned["Preferences"]["rxdelay"], .25)
            self.assertEqual(returned["Preferences"]["airtime_factor"], 2)
            self.assertEqual(returned["Latitude"], 53.5)
            self.assertEqual(returned["Longitude"], -113.25)
            self.assertEqual(returned["AdvertLocation"], 2)
            self.assertNotIn("Logging", returned)
            retained_log = (service.root/"room.state.packet.log").read_bytes()
            self.assertEqual((destination/"room/packet.log").read_bytes(), retained_log)
            records = [json.loads(line) for line in retained_log.splitlines()]
            self.assertTrue(any(entry["direction"] == "RX" for entry in records))
            self.assertTrue(any(entry["direction"] == "TX" for entry in records))
            self.assertGreaterEqual(returned["RTCOffset"], 59)
            self.assertLessEqual(returned["RTCOffset"], 60)
            self.assertEqual(returned["Members"][public(2).hex()]["Path"], base64.b64encode(b"\xaa\xbb\xcc").decode())
            self.assertEqual(hashes(inventory(destination/"bot/native")), hashes(inventory(service.root/"native")))
            transcript = self.root/"go-reconcile-requests.json"
            requests = [{"Raw": login(2, recipient, stamp+90+recipient, since=post_stamp).hex(),
                         "Want": "", "Seed": recipient} for recipient in (1, 4)]
            for recipient, text, wanted in (
                    (5, "!recall migration", "updated native data"),
                    (5, "!recall handover", "added native note"),
                    (1, "get owner.info", "> returned|operator"),
                    (4, "get name", "> Returned-Room"),
                    (4, "get owner.info", "> returned|operator"),
                    (4, "get lat", "> 53.5"),
                    (4, "get lon", "> -113.25"),
                    (4, "get rxdelay", "> 0.25"),
                    (4, "get af", "> 2.0"),
                    (4, "get dutycycle", "> 33.3%"),
                    (4, "gps advert", "> prefs")):
                requests.append({"Raw": command(recipient, stamp+100+len(requests), text,
                                                flags=0 if recipient == 5 else 4).hex(),
                                 "Want": wanted, "Seed": recipient})
            transcript.write_text(json.dumps({"Root": str(destination), "Worker": cfg["worker"],
                "AdminPassword": report["required_go_runtime"]["admin_password"],
                "Advert": packet(4, parse(advert(2))[4], route=2, width=3).hex(), "Requests": requests}))
            result = subprocess.run(["go", "test", "../../internal/app", "-run",
                                     "^TestWillowReconciledApplication$", "-count=1", "-v"],
                cwd=ROOT, env=os.environ|{"TMPDIR": str(ROOT/"build"),
                                         "MESHCORE_WILLOW_RECONCILED": str(transcript)},
                capture_output=True, text=True, timeout=70)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
            loaded = json.loads((destination/"room/identity-state.json").read_text())["state"]
            self.assertEqual(loaded["History"], returned["History"])
            for key in ("Latitude", "Longitude", "AdvertLocation", "RTCOffset"):
                self.assertEqual(loaded[key], returned[key])
            self.assertEqual(loaded["Preferences"]["rxdelay"], .25)
            self.assertEqual(loaded["Preferences"]["airtime_factor"], 2)
            self.assertEqual((destination/"room/packet.log").read_bytes(), retained_log)
            self.assertEqual(loaded["Members"][public(7).hex()], returned["Members"][public(7).hex()])
            self.assertGreaterEqual(loaded["Members"][public(2).hex()]["LastTimestamp"],
                                    returned["Members"][public(2).hex()]["LastTimestamp"])
            extension = copy.deepcopy(room_original)
            extension["future_principal_policy"] = {"opaque": [1, 2, 3]}
            extension["Members"][public(2).hex()]["future_grant"] = {"retain": True}
            merged = merge_state(extension, current, cfg, "room", {})
            self.assertEqual(merged["future_principal_policy"], extension["future_principal_policy"])
            self.assertEqual(merged["Members"][public(2).hex()]["future_grant"], {"retain": True})
            (ROOT/"build/reconciliation-results.json").write_text(json.dumps(
                {"room_history": len(returned["History"]), "room_members": len(returned["Members"]),
                 "room_sync_cursor": returned["Members"][public(7).hex()]["SyncSince"],
                 "new_and_updated_notes_read_by_go": True, "source_unchanged": True,
                 "independent_base_preserved": True, "go_application": result.stdout}, indent=2)+"\n")
            broken = self.root/"broken-willow"
            shutil.copytree(service.root, broken)
            with (broken/"room.state").open("ab") as out:
                out.write(b"unknown")
            with self.assertRaisesRegex(ValueError, "trailing|preference"):
                reconcile(broken, self.root/"refused-candidate")
            self.assertFalse((self.root/"refused-candidate").exists())
            with (latest_go/"room/state.json").open("ab") as out:
                out.write(b" ")
            with self.assertRaisesRegex(ValueError, "competing writes"):
                reconcile(service.root, self.root/"competing-candidate", latest_go)
            self.assertFalse((self.root/"competing-candidate").exists())
        finally:
            (ROOT/"build/reconciliation-service.log").write_text("\n".join(service.logs+service.errors)+"\n")
            service.close()


if __name__ == "__main__":
    unittest.main(defaultTest="Reconciliation.test_lifecycle", verbosity=2)
