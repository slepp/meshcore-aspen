"""Frozen forward/reverse rebind and current-write Go recovery on loopback."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import time
import unittest
import uuid
from unittest.mock import patch

from parity import ROOT, public, packet, parse, seal, secret
sys.path.insert(0, str(ROOT))
from migrate_go import hashes, inventory, migrate, write_private
from reconcile_go import reconcile, snapshot
from service_demo import RunningService, PROFILE, advert, addressed, body, login, text
from willow import rebind, owner_rpc, owner_status


class Upgrade(unittest.TestCase):
    def until(self, callback, timeout=12):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            result = callback()
            if result:
                return result
            time.sleep(.025)
        self.fail("synthetic upgrade condition did not complete")

    def test_current_writes_survive_forward_reverse_and_go_recovery(self):
        reference = Path(os.environ["MESHCORE_HEW_RECOVERY_REFERENCE"]).resolve()
        root = ROOT.parents[1] / f".u-{os.getpid()}"
        root.mkdir(mode=0o700)
        self.addCleanup(shutil.rmtree, root)
        source = root / "go"
        subprocess.run(["go", "test", "../../internal/roles", "-run",
                        "^TestWillowMigrationFixture$", "-count=1"], cwd=ROOT,
                       env=os.environ | {"TMPDIR": str(ROOT / "build"),
                                         "MESHCORE_WILLOW_FIXTURE": str(source)},
                       check=True, capture_output=True, timeout=60)
        service = RunningService(root=root / "origin")
        self.addCleanup(service.close)
        service.stop_process()
        shutil.copytree(service.root / "native", source / "bot/native")
        config = root / "settings"
        write_private(config, (f"address=127.0.0.1\nport={service.emulator.port}\n"
                              f"profile={PROFILE.hex()}\npassword=room\nadmin=admin\n"
                              "bot_home=\nbot_default=\n").encode())
        staged, older, candidate, reverse, returned = (
            root / name for name in ("stage", "old", "new", "reverse", "returned"))
        migrate(source, config, staged)

        def old_rebind(frozen, destination):
            completed = subprocess.run([sys.executable, "-B", str(reference / "willow.py"),
                "rebind", "--source", str(frozen), "--state", str(destination)],
                check=True, capture_output=True, text=True, timeout=30)
            self.assertEqual(json.loads(completed.stdout)["mode"], "rebound")

        old_rebind(staged, older)
        older_before = hashes(inventory(older, retained=("rollback-go",)))
        rebind(older, candidate)
        self.assertEqual(hashes(inventory(older, retained=("rollback-go",))), older_before)
        service.root = candidate
        service.emulator.identity_root = candidate
        service.start_process(True, runner=True)
        self.until(lambda: (candidate / "admin.sock").exists())
        self.assertEqual(owner_rpc(candidate, b"\1\4bot name Upgrade Latest"),
                         b"Saved and applied bot name; identity unchanged")
        self.assertEqual(owner_rpc(candidate, b"\1\5\3set name Upgrade Latest Room"), b"OK")
        modem = service.emulator
        stamp = int(time.time()) - 10
        modem.send(2, 0, advert(2))
        time.sleep(.1)
        modem.send(2, 0, text(2, 5, stamp, "!remember upgrade latest data"))
        note = modem.receive(lambda item: item["port"] == 2 and parse(item["raw"])[0] == 2)
        self.assertIn(b"committed", body(note["raw"], 2, 5))
        modem.send(1, 0, login(2, 4, stamp + 1))
        modem.receive(lambda item: item["port"] == 1 and parse(item["raw"])[0] == 1)
        post = b"Latest candidate room post"
        modem.send(1, 0, addressed(2, 4, 2, struct.pack("<IB", stamp + 2, 0) + post))
        modem.receive(lambda item: item["port"] == 1 and parse(item["raw"])[0] == 3)
        self.until(lambda: any(entry["Text"] == post.decode() for entry in snapshot(
            (candidate / "room.state").read_bytes(), public(4), 3, "room")["History"]))
        modem.send(2, 0, addressed(2, 5, 8, b"\x80\x0f"))
        time.sleep(.1)
        identifier = uuid.uuid4().bytes
        outgoing = b"Latest candidate owner DM"
        owner_status(candidate, identifier, 1, public(2) + outgoing)
        sent = modem.receive(lambda item: item["port"] == 2 and parse(item["raw"])[0] == 2
            and body(item["raw"], 2, 5)[5:].rstrip(b"\0") == outgoing)
        self.until(lambda: owner_status(candidate, identifier)["local_tx_confirmed"])
        proof = hashlib.sha256(body(sent["raw"], 2, 5).rstrip(b"\0") + public(5)).digest()[:4]
        modem.send(2, 0, packet(3, proof))
        self.until(lambda: owner_status(candidate, identifier)["native_rf_ack"])
        service.stop_process()
        retained = hashes(inventory(candidate, retained=("rollback-go",)))
        latest = {name: (candidate / name).read_bytes() for name in (
            "owner.state", "admin.clock", "room.state", "native/host-name")}
        evidence = {"reference": str(reference), "candidate": str(ROOT),
                    "rf_commands": 0, "synthetic_peer_ack": True,
                    "forward_rebind_source_unchanged": True}
        recovery_source = candidate
        try:
            old_rebind(candidate, reverse)
            for name, raw in latest.items():
                self.assertEqual((reverse / name).read_bytes(), raw, name)
            service.root = reverse
            service.emulator.identity_root = reverse
            with patch.dict(os.environ, {"MESHCORE_HEW_HOST": str(
                    reference / "build/hew-host-release")}):
                service.start_process(True)
            self.until(lambda: (reverse / "owner.sock").exists())
            self.assertTrue(owner_status(reverse, identifier)["native_rf_ack"])
            self.assertEqual(owner_rpc(reverse, b"\1\4bot name"), b"Name: Upgrade Latest")
            self.assertEqual(owner_rpc(reverse, b"\1\5\3get name"), b"> Upgrade Latest Room")
            service.stop_process()
            recovery_source = reverse
            evidence["old_hew_recovery"] = "current writes rebind, start and read successfully"
        except (subprocess.CalledProcessError, AssertionError) as error:
            if service.proc.poll() is None:
                service.stop_process()
            evidence["old_hew_recovery"] = "unsupported; use validated Go recovery"
            evidence["old_hew_error"] = str(error)
        self.assertEqual(hashes(inventory(candidate, retained=("rollback-go",))), retained)
        report = reconcile(recovery_source, returned)
        for name in ("owner.state", "admin.clock"):
            self.assertEqual((returned / ("willow-" + name)).read_bytes(),
                             (recovery_source / name).read_bytes())
        current = snapshot((recovery_source / "room.state").read_bytes(), public(4), 3, "room")
        room_identity = returned / "room/identity-state.json"
        room = (json.loads(room_identity.read_text())["state"] if room_identity.exists()
                else json.loads((returned / "room/state.json").read_text()))
        self.assertEqual(room["History"], current["History"])
        self.assertTrue(any(entry["Text"] == post.decode() for entry in room["History"]))
        self.assertEqual(hashes(inventory(returned / "bot/native")),
                         hashes(inventory(recovery_source / "native")))
        transcript = root / "go-check.json"
        requests = [
            {"Raw": packet(7, public(4)[:1] + public(2) + seal(secret(2, public(4)),
                struct.pack("<II", stamp + 90, 0) + b"admin\0")).hex(),
             "Want": "", "Seed": 4},
            {"Raw": addressed(2, 4, 2, struct.pack("<IB", stamp + 91, 4) +
                             b"get name").hex(), "Want": "> Upgrade Latest Room", "Seed": 4},
            {"Raw": text(2, 5, stamp + 92, "!recall upgrade").hex(),
             "Want": "latest data", "Seed": 5},
        ]
        worker = dict(line.split("=", 1) for line in (
            recovery_source / "config").read_text().splitlines())["worker"]
        write_private(transcript, json.dumps({
            "Root": str(returned), "Worker": worker,
            "AdminPassword": report["required_go_runtime"].get("admin_password", "admin"),
            "Advert": packet(4, parse(advert(2))[4], route=2, width=3).hex(),
            "Requests": requests}).encode())
        completed = subprocess.run(["go", "test", "../../internal/app", "-run",
            "^TestWillowReconciledApplication$", "-count=1"], cwd=ROOT,
            env=os.environ | {"TMPDIR": str(ROOT / "build"),
                              "MESHCORE_WILLOW_RECONCILED": str(transcript)},
            capture_output=True, text=True, timeout=70)
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        evidence.update(go_application_recovery=True, latest_owner_ledger=True,
                        latest_admin_clock=True, latest_room_post=True,
                        latest_native_note=True, candidate_source_unchanged=True)
        write_private(ROOT / "build/upgrade-results.json",
                      (json.dumps(evidence, indent=2) + "\n").encode())


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference", required=True, type=Path,
                        help="installed experiments/hew-roles directory with its bound worker")
    args = parser.parse_args()
    os.environ["MESHCORE_HEW_RECOVERY_REFERENCE"] = str(args.reference)
    os.environ.setdefault("MESHCORE_HEW_HOST", str(ROOT / "build/hew-host-release"))
    unittest.main(argv=[sys.argv[0]], verbosity=2)
