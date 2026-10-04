"""One private owner -> native radio -> TX/ACK/inbox/restart lifecycle."""
import hashlib
import json
import os
from pathlib import Path
import signal
import shutil
import stat
import subprocess
import sys
import time
import unittest
import uuid

from parity import ROOT, public, packet, parse
from service_demo import RunningService, advert, addressed, body, PROFILE
from willow import owner_inbox, owner_status, owner_rpc, rebind, check
from migrate_go import migrate, inventory, hashes, write_private
from reconcile_go import reconcile


class OwnerService(unittest.TestCase):
    def until(self, callback, timeout=12):
        end = time.monotonic()+timeout
        while time.monotonic() < end:
            value = callback()
            if value:
                return value
            time.sleep(.025)
        self.fail("owner lifecycle condition timed out")

    def test_send_receive_restart(self):
        service = RunningService(root=ROOT.parents[1]/f".owner-{os.getpid()}")
        evidence = {}
        try:
            self.until(lambda: (service.root/"owner.sock").exists())
            self.until(lambda: (service.root/"admin.sock").exists())
            self.assertEqual(stat.S_IMODE((service.root/"owner.sock").stat().st_mode), 0o600)
            self.assertEqual(stat.S_IMODE((service.root/"admin.sock").stat().st_mode), 0o600)
            def admin(command, role=None):
                request = b"\x01\x04" if role is None else bytes((1, 5, role))
                return owner_rpc(service.root, request+command.encode()).decode()
            self.assertIn("Native bot ready=", admin("bot status"))
            self.assertEqual(admin("bot key"), "KEY "+public(5).hex())
            self.assertEqual(admin("bot name Private Owner Bot"),
                             "Saved and applied bot name; identity unchanged")
            self.assertEqual(admin("bot name"), "Name: Private Owner Bot")
            cli = subprocess.run([sys.executable, "-B", str(ROOT/"willow.py"), "command",
                "--state", str(service.root), "--role", "bot"], input="bot name\n",
                text=True, capture_output=True, check=True, timeout=12)
            self.assertEqual(json.loads(cli.stdout), {"role": "bot", "reply": "Name: Private Owner Bot"})
            self.assertEqual(admin("set name Private Owner Room", 3), "OK")
            self.assertEqual(admin("get name", 3), "> Private Owner Room")
            clock_before_reject = (service.root/"admin.clock").read_bytes()
            for request in (b"\x01\x04reboot", b"\x01\x05\x03set tx 2"):
                with self.assertRaisesRegex(ValueError, "malformed-owner-request"):
                    owner_rpc(service.root, request)
            self.assertEqual((service.root/"admin.clock").read_bytes(), clock_before_reject)
            evidence["scoped_native_and_committed_role_commands"] = True
            def health(selector):
                return json.loads(owner_rpc(service.root, bytes((1, 6, selector))))
            readiness = self.until(lambda: (value if (value := health(1))["ready"] else None))
            self.assertEqual(readiness["not_ready"], {})
            self.assertFalse(readiness["mqtt_connection_checked"])
            snapshot = health(0)
            self.assertEqual(set(snapshot), {"bot", "repeater", "room"})
            for name, key in (("bot", public(5)), ("repeater", public(1)), ("room", public(4))):
                self.assertEqual(snapshot[name]["public_key"], key.hex())
                self.assertTrue(snapshot[name]["radio_connected"])
                self.assertGreater(snapshot[name]["phy_configuration_generation"], 0)
                self.assertEqual(len(snapshot[name]["airtime_ms"]), 256)
                self.assertEqual(snapshot[name]["airtime_ms"][0], 0)
                self.assertIsInstance(snapshot[name]["telemetry"], dict)
            evidence["actual_actor_modem_health_readiness"] = True
            cli = subprocess.run([sys.executable, "-B", str(ROOT/"willow.py"), "health",
                "--state", str(service.root)], text=True, capture_output=True, check=True, timeout=12)
            self.assertTrue(json.loads(cli.stdout)["readiness"]["ready"])
            for binary in ("owner-client-checks", "owner-client-checks-release"):
                result = subprocess.run([str(ROOT/"build"/binary), str(service.root/"admin.sock"), "0104626f7420737461747573"],
                    capture_output=True, text=True, timeout=12, check=True)
                reply = bytes.fromhex(result.stdout.strip())
                self.assertEqual(reply[:2], b"\x01\x00")
                self.assertIn(b"Native bot ready=", reply[2:])
                result = subprocess.run([str(ROOT/"build"/binary), str(service.root/"owner.sock"), "010600"],
                    capture_output=True, text=True, timeout=6, check=True)
                reply = bytes.fromhex(result.stdout.strip())
                self.assertEqual(reply[:2], b"\x01\x00")
                self.assertEqual(set(json.loads(reply[2:])), set(snapshot))
                self.assertGreater(len(reply), 4096)
            evidence["native_private_packet_client_debug_release_large_health"] = True
            first_cursor = owner_inbox(service.root)["cursor"]
            modem = service.emulator

            def contact():
                modem.send(2, 0, advert(2))
                time.sleep(.1)
                modem.send(2, 0, addressed(2, 5, 8, b"\x80\x0f"))
                time.sleep(.1)

            def phase(identifier, wanted):
                result = owner_status(service.root, identifier)
                return result if result["phase"] == wanted else None

            def outgoing(identifier, text):
                start = len(modem.all_submissions)
                owner_status(service.root, identifier, 1, public(2)+text)
                item = modem.receive(lambda j: j["port"] == 2 and parse(j["raw"])[0] == 2
                    and body(j["raw"], 2, 5)[5:].rstrip(b"\0") == text)
                self.until(lambda: phase(identifier, "local-tx-confirmed"))
                return item, start

            contact()
            identifier = uuid.uuid4().bytes
            message = b"Owner-only outbound fixture"
            message_file = service.root/"owner-message"
            message_file.write_bytes(message); message_file.chmod(0o600)
            result = subprocess.run([sys.executable, "-B", str(ROOT/"willow.py"), "send",
                "--state", str(service.root), "--request-id", identifier.hex(),
                "--to", public(2).hex(), "--message-file", str(message_file)],
                capture_output=True, text=True, timeout=6)
            message_file.unlink()
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(json.loads(result.stdout)["request_id"], identifier.hex())
            tx = modem.receive(lambda j: j["port"] == 2 and parse(j["raw"])[0] == 2
                and body(j["raw"], 2, 5)[5:].rstrip(b"\0") == message)
            sent = self.until(lambda: phase(identifier, "local-tx-confirmed"))
            self.assertTrue(sent["admitted"] and sent["local_tx_confirmed"])
            self.assertFalse(sent["native_rf_ack"])
            proof = hashlib.sha256(body(tx["raw"], 2, 5).rstrip(b"\0")+public(5)).digest()[:4]
            modem.send(2, 0, packet(3, bytes(value ^ 0xff for value in proof)))
            modem.send(2, 0, packet(3, proof), metadata=b"\xf9\x80\x7f")
            time.sleep(.15)
            self.assertFalse(owner_status(service.root, identifier)["native_rf_ack"])
            modem.send(2, 0, packet(3, proof))
            acked = self.until(lambda: phase(identifier, "native-rf-ack"))
            self.assertTrue(acked["local_tx_confirmed"] and acked["native_rf_ack"])
            evidence["acknowledged_send"] = acked

            stamp = int(time.time())
            text = b"Owner-only incoming fixture"
            plain = stamp.to_bytes(4, "little")+b"\0"+text
            for flags in (0, 0, 1):
                modem.send(2, 0, addressed(2, 5, 2, plain[:4]+bytes([flags])+text))
            received = self.until(lambda: owner_inbox(service.root, first_cursor)["messages"])
            self.assertEqual(len(received), 1)
            self.assertEqual(received[0]["sender"], public(2).hex())
            self.assertEqual((received[0]["timestamp"], received[0]["message"]), (stamp, text.decode()))
            time.sleep(.15)
            self.assertEqual(len(owner_inbox(service.root, first_cursor)["messages"]), 1)
            evidence["authenticated_inbox_duplicate_count"] = 1

            bad = uuid.uuid4().bytes
            owner_status(service.root, bad, 1, public(7)+b"No contact")
            failure = self.until(lambda: phase(bad, "failed"))
            self.assertEqual(failure["reason"], "fresh-direct-contact-required")
            with self.assertRaisesRegex(ValueError, "content-conflict"):
                owner_status(service.root, identifier, 1, public(2)+b"Changed request")

            pending = uuid.uuid4().bytes
            outgoing(pending, b"Uncertain across worker fault")
            before = len(service.logs)
            pid = int(next(line for line in reversed(service.logs) if line.startswith("BOT_PROCESS pid=")).split("=")[1])
            os.kill(pid, signal.SIGSEGV)
            self.until(lambda: phase(pending, "unknown"))
            service.wait("BOT_READY", 15, after=before)
            self.assertEqual(admin("bot name"), "Name: Private Owner Bot")
            frozen = len(modem.all_submissions)
            self.assertEqual(owner_status(service.root, pending, 1, public(2)+b"Uncertain across worker fault")["phase"], "unknown")
            time.sleep(.15)
            self.assertEqual(len(modem.all_submissions), frozen)

            contact()
            restart_id = uuid.uuid4().bytes
            outgoing(restart_id, b"Uncertain across host restart")
            service.stop_process()
            self.assertFalse((service.root/"owner.sock").exists())
            self.assertFalse((service.root/"admin.sock").exists())
            service.start_process(True)
            self.until(lambda: (service.root/"owner.sock").exists())
            self.assertEqual(owner_status(service.root, restart_id)["phase"], "unknown")
            self.assertTrue(owner_status(service.root, restart_id)["local_tx_confirmed"])
            self.assertEqual(owner_status(service.root, identifier)["phase"], "native-rf-ack")
            with self.assertRaisesRegex(ValueError, "generation-changed"):
                owner_inbox(service.root, first_cursor)
            self.assertEqual(owner_inbox(service.root)["messages"], [])
            self.assertEqual(admin("get name", 3), "> Private Owner Room")
            self.assertEqual(admin("bot name"), "Name: Private Owner Bot")
            self.assertGreater(int.from_bytes((service.root/"admin.clock").read_bytes()[4:], "little"),
                               int.from_bytes(clock_before_reject[4:], "little"))
            evidence["worker_and_host_restart_unknown_without_replay"] = True
            self.assertFalse(any(text.decode() in line or message.decode() in line for line in service.logs+service.errors))
            evidence["plaintext_absent_from_journal"] = True

            with self.assertRaisesRegex(ValueError, "locked"):
                rebind(service.root, service.root.parent/f".owner-busy-{os.getpid()}")
            service.stop_process()
            source = ROOT/"build"/f"owner-go-{os.getpid()}"
            self.addCleanup(lambda: shutil.rmtree(source) if source.exists() else None)
            subprocess.run(["go", "test", "../../internal/roles", "-run", "^TestWillowMigrationFixture$", "-count=1"],
                cwd=ROOT, env=os.environ|{"TMPDIR": str(ROOT/"build"), "MESHCORE_WILLOW_FIXTURE": str(source)},
                check=True, capture_output=True, timeout=60)
            shutil.copytree(service.root/"native", source/"bot/native")
            write_private(source/"willow-owner.state", (service.root/"owner.state").read_bytes())
            write_private(source/"willow-admin.clock", (service.root/"admin.clock").read_bytes())
            candidates = [service.root.parent/f".{name}-{os.getpid()}" for name in ("owner-stage", "owner-rebind", "owner-go-back", "owner-return")]
            for path in candidates:
                self.addCleanup(lambda p=path: shutil.rmtree(p) if p.exists() else None)
            configuration = service.root/"migration-config"
            write_private(configuration, (f"address=127.0.0.1\nport={service.emulator.port}\n"
                f"profile={PROFILE.hex()}\npassword=room\nadmin=admin\n"
                "bot_home=\nbot_default=\nbot_name=Private Owner Bot\n").encode())
            staged, rebound, returned_go, returned_willow = candidates
            migrate(source, configuration, staged)
            # A stopped previous installation is not required to remain loadable.
            old_config = staged/"config"
            lines = old_config.read_text().splitlines()
            old_config.write_text("\n".join("worker=/retired-version/native-worker" if line.startswith("worker=") else line for line in lines)+"\n")
            record = json.loads((staged/"migration.json").read_text())
            record["output_files"]["config"] = hashlib.sha256(old_config.read_bytes()).hexdigest()
            (staged/"migration.json").write_text(json.dumps(record))
            before = hashes(inventory(staged, retained=("rollback-go",)))
            rebound_result = rebind(staged, rebound)
            self.assertEqual(rebound_result["mode"], "rebound")
            check(rebound)
            self.assertEqual(hashes(inventory(staged, retained=("rollback-go",))), before)
            for name, digest in before.items():
                if name not in ("config", "migration.json"):
                    self.assertEqual(hashlib.sha256((rebound/name).read_bytes()).hexdigest(), digest, name)
            old_root = service.root
            self.addCleanup(lambda: shutil.rmtree(old_root) if old_root.exists() else None)
            service.root = rebound
            service.emulator.identity_root = rebound
            service.start_process(True, runner=True)
            self.until(lambda: (rebound/"owner.sock").exists())
            self.assertEqual(owner_status(rebound, identifier)["phase"], "native-rf-ack")
            self.assertEqual(owner_status(rebound, restart_id)["phase"], "unknown")
            self.assertEqual(admin("bot name"), "Name: Private Owner Bot")
            service.stop_process()
            reconcile(rebound, returned_go)
            self.assertEqual((returned_go/"willow-owner.state").read_bytes(), (rebound/"owner.state").read_bytes())
            self.assertEqual((returned_go/"willow-admin.clock").read_bytes(), (rebound/"admin.clock").read_bytes())
            self.assertEqual(hashes(inventory(returned_go/"bot/native")), hashes(inventory(rebound/"native")))
            migrate(returned_go, configuration, returned_willow)
            self.assertEqual((returned_willow/"owner.state").read_bytes(), (rebound/"owner.state").read_bytes())
            self.assertEqual((returned_willow/"admin.clock").read_bytes(), (rebound/"admin.clock").read_bytes())
            self.assertEqual((returned_willow/"native/host-name").read_bytes(), b"HNM1Private Owner Bot")
            evidence["frozen_rebind_and_go_roundtrip_preserve_native_state_and_request_ids"] = True
            (ROOT/"build/owner-service-results.json").write_text(json.dumps(evidence, indent=2)+"\n")
        finally:
            (ROOT/"build/owner-service.log").write_text("\n".join(service.logs+service.errors)+"\n")
            service.close()


if __name__ == "__main__":
    os.environ.setdefault("MESHCORE_HEW_HOST", str(ROOT/"build/hew-host-release"))
    unittest.main(verbosity=2)
