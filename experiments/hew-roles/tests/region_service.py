"""Named regions over authenticated RF, with host restart and Go reconciliation."""
import json
import os
import shutil
import struct
import time
import unittest

import migration
from parity import ROOT, public, packet, parse, secret, seal
from service_demo import RunningService, Emulator, addressed, body, scoped, scope_code
from migrate_go import migrate, hashes, inventory
from reconcile_go import reconcile, snapshot


class RegionService(unittest.TestCase):
    setUpClass = classmethod(migration.Migration.setUpClass.__func__)
    write_config = classmethod(migration.Migration.write_config.__func__)

    def test_authenticated_rotation_load_restart_and_go_roundtrip(self):
        source = self.root/"region-source"
        shutil.copytree(self.source, source)
        path = source/"room/state.json"
        state = json.loads(path.read_text())
        state["Preferences"]["regions"].append({"id": 3, "parent": 1, "flags": 128,
            "name": "$rotated", "keys": [list(bytes([byte])*16) for byte in (1, 2, 3, 4)]})
        for index in range(4, 33):
            state["Preferences"]["regions"].append({"id": index, "parent": 0, "flags": 2, "name": f"#n{index}"})
        state.update(HomeRegion=3, NextRegionID=33, DefaultRegion=3,
                     ManagedDefaultRegion=True, DiscoveryModified=1234)
        state["Preferences"]["wildcard_flags"] = 128
        state["Preferences"]["default_scope"] = {"name": "$rotated", "key": [1]*16}
        for member in state["Members"].values():
            member.update(KnownPath=False, PathLength=0, Path=None)
        path.write_text(json.dumps(state))
        before = hashes(inventory(source))
        service = RunningService.__new__(RunningService)
        service.root = self.root/"region-live"
        service.emulator = Emulator()
        service.emulator.identity_root = service.root
        service.logs, service.errors = [], []
        configuration = self.root/"region-config"
        self.write_config(configuration, service.emulator.port)
        migrate(source, configuration, service.root)
        initial = snapshot((service.root/"room.state").read_bytes(), public(4), 3, "initial region fixture")
        self.assertEqual(len(initial["region_table"]["entries"]), 32)
        self.assertEqual(initial["region_table"]["discovery"], 1234)
        timestamp = int(time.time())-100
        first, second = bytes([1])*16, bytes([2])*16

        def next_stamp():
            nonlocal timestamp
            timestamp += 1
            return timestamp

        def response(peer, kind):
            return service.emulator.receive(lambda item: item["port"] == 1 and parse(item["raw"])[0] == kind
                and parse(item["raw"])[4][0] == public(peer)[0]
                and (kind != 2 or body(item["raw"], peer, 4)[4] >> 2 == 1))

        def login(peer):
            raw = packet(7, public(4)[:1]+public(peer)+seal(secret(peer, public(4)),
                struct.pack("<II", next_stamp(), 0)+b"admin\0"))
            service.emulator.send(1, 0, raw)
            response(peer, 1)

        def command(text, expected, peer=2, transport_key=None):
            plain = struct.pack("<IB", next_stamp(), 4)+text.encode()
            raw = addressed(peer, 4, 2, plain)
            if transport_key is not None:
                raw = scoped(raw, transport_key)
            service.emulator.send(1, 0, raw)
            reply = response(peer, 2)["raw"]
            self.assertEqual(body(reply, peer, 4)[5:].rstrip(b"\0"), expected.encode())
            if transport_key is not None:
                parsed = parse(reply)
                self.assertEqual(parsed[1], 0)
                self.assertEqual(int.from_bytes(reply[1:3], "little"), scope_code(first, reply))
                self.assertEqual(reply[1:3], reply[3:5])
            return reply

        try:
            service.start_process(True, runner=True)
            login(2)
            command("region get $rotated", " $rotated (#fixture-a) F", transport_key=second)
            command("region home", " home is $rotated")
            command("region put overflow", "Err - unable to put")
            command("region load", "")
            login(7)
            command(" north F", "", transport_key=second)
            command(" north F", "Err - region load owned by another administrator", peer=7)
            loading = snapshot((service.root/"room.state").read_bytes(), public(4), 3, "uncommitted regions")
            self.assertEqual(len(loading["region_table"]["entries"]), 32)
            service.stop_process()
            service.start_process(True, runner=True)
            command("region home", " home is $rotated")
            command("region load", "")
            command(" north F", "")
            command("  $rotated F", "")
            command("", "OK - loaded 2 regions")
            command("region home north", " home is now north")
            command("region default north", " default scope is now north")
            command("region save", "OK")
            modified = snapshot((service.root/"room.state").read_bytes(), public(4), 3, "committed regions")
            table = modified["region_table"]
            self.assertEqual(len(table["entries"]), 2)
            self.assertGreaterEqual(table["discovery"], int(time.time())-10)
            private = next(entry for entry in table["entries"] if entry["name"] == "$rotated")
            self.assertEqual(private["keys"], [bytes([byte])*16 for byte in (1, 2, 3, 4)])
            self.assertEqual(private["flags"], 128)
            self.assertEqual(table["home"], 33)
            self.assertEqual(table["default_id"], 33)
            self.assertEqual(table["next"], 34)
            service.stop_process()
            service.start_process(True, runner=True)
            command("region home", " home is north")
            command("region get $rotated", " $rotated (north) F")
            command("region default", " default scope is north")
            service.stop_process()
            destination = self.root/"region-go-return"
            reconcile(service.root, destination)
            returned = json.loads((destination/"room/state.json").read_text())
            self.assertEqual(returned["HomeRegion"], 33)
            self.assertEqual(returned["DefaultRegion"], 33)
            self.assertEqual(returned["NextRegionID"], 34)
            self.assertEqual(returned["DiscoveryModified"], table["discovery"])
            entries = returned["Preferences"]["regions"]
            self.assertEqual(len(entries), 2)
            private = next(entry for entry in entries if entry["name"] == "$rotated")
            self.assertEqual(private["parent"], 33)
            self.assertEqual(private["keys"], [list(bytes([byte])*16) for byte in (1, 2, 3, 4)])
            self.assertEqual(private["flags"], 128)
            again = self.root/"region-native-return"
            migrate(destination, configuration, again)
            restored = snapshot((again/"room.state").read_bytes(), public(4), 3, "returned region fixture")
            self.assertEqual(restored["region_table"], table)
            self.assertEqual(hashes(inventory(source)), before)
        finally:
            service.close()


if __name__ == "__main__":
    os.environ.setdefault("MESHCORE_HEW_HOST", str(ROOT/"build/hew-host-release"))
    unittest.main(verbosity=2)
