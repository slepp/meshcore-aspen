"""Role-only authenticated restart and owner-admin actor recovery."""
import json
import os
import re
import struct
import sys
import time
import unittest

from parity import ROOT, packet, parse, public, seal, secret
from service_demo import RunningService, addressed, body, text
from source_policy import RoleClient

sys.path.insert(0, str(ROOT))
from reconcile_go import snapshot
from willow import owner_rpc


def fixture():
    return RunningService(root=ROOT.parents[1]/f".lifecycle-{os.getpid()}-{time.monotonic_ns():x}",
                          extra_config="room.retention=durable-replay\n")


class RoleLifecycle(unittest.TestCase):
    def test_restart_retires_only_old_role_jobs_and_ignores_late_success(self):
        with RunningService(native=False,
                root=ROOT.parents[1]/f".lifecycle-jobs-{os.getpid()}-{time.monotonic_ns():x}") as service:
            modem = service.emulator
            relay, room = RoleClient(service, 0), RoleClient(service, 1)
            modem.hold_all = True
            self.assertIn("Willow-Relay", relay.command("get name"))
            relay_job = next(item for item in reversed(modem.all_submissions)
                             if item["port"] == 0 and parse(item["raw"])[0] == 2)
            room_before = room.command("get stats")
            room_job = next(item for item in reversed(modem.all_submissions)
                            if item["port"] == 1 and parse(item["raw"])[0] == 2)
            old_sent = int(re.search(r"sent=(\d+)", room_before).group(1))
            for item in (relay_job, room_job):
                service.wait(f"TX port={item['port']} generation={item['generation']} job={item['job']} state=1")
            after = len(service.logs)
            relay.send("reboot")
            service.wait(f"TX port=0 generation={relay_job['generation']} job={relay_job['job']} state=4", after=after)
            self.assertFalse(any(f"TX port=1 generation={room_job['generation']} job={room_job['job']} state=4" in line
                                 for line in service.logs[after:]))
            modem.event(0, relay_job["generation"], relay_job["job"], 2, modem.connection)
            self.assertIn("sent=0 ", relay.command("get stats"))
            modem.event(1, room_job["generation"], room_job["job"], 2, modem.connection)
            room_after = room.command("get stats")
            self.assertEqual(int(re.search(r"sent=(\d+)", room_after).group(1)), old_sent+1)
            self.assertFalse(any(f"TX port=0 generation={relay_job['generation']} job={relay_job['job']} state=2" in line
                                 for line in service.logs[after:]))
            self.assertEqual(modem.epoch, 1)

    def test_authenticated_restart_preserves_replay_and_other_roles(self):
        for port, legacy in ((0, False), (1, False), (0, True)):
            with self.subTest(port=port, legacy=legacy), fixture() as service:
                modem = service.emulator
                target, other = RoleClient(service, port), RoleClient(service, 1-port)
                name = ("relay", "room")[port]
                seed = (1, 4)[port]
                state = service.root/(name+".state")
                keys = {file.name: file.read_bytes() for file in service.root.glob("*.seed")}
                self.assertEqual(target.command("set name Retained"), "OK")
                self.assertEqual(target.command("log start"), "   logging on")
                if port == 1:
                    target.stamp += 1
                    modem.send(port, 0, text(2, seed, target.stamp, "retained history"))
                    modem.receive(lambda item: item["port"] == port and parse(item["raw"])[0] == 3)
                before = snapshot(state.read_bytes(), public(seed), port+2, "before restart")
                start = len(modem.all_submissions)
                after = len(service.logs)
                target.stamp += 1
                request = addressed(2, seed, 2, struct.pack("<IB", target.stamp, 0 if legacy else 4)+b"reboot")
                modem.send(port, 0, request)
                service.wait("ROLE_RESTARTED state="+str(state), after=after)
                service.wait("ROLE_RESTORED state="+str(state)+" generation=2", after=after)
                log = service.root/(name+".state.packet.log")
                retained_log = log.read_bytes()
                self.assertTrue(retained_log)
                if legacy:
                    ack = modem.receive(lambda item: item["port"] == port and parse(item["raw"])[0] == 3)
                    self.assertEqual(ack["delay"], 200)
                self.assertIn("Willow-", other.command("get name"))
                self.assertEqual(target.command("get name"), "> Retained")
                self.assertEqual(log.read_bytes(), retained_log)
                committed = snapshot(state.read_bytes(), public(seed), port+2, "after restart")
                self.assertEqual(before["settings"], committed["settings"])
                self.assertEqual(before["History"], committed["History"])
                self.assertGreater(committed["Members"][public(2).hex()]["LastTimestamp"],
                                   before["Members"][public(2).hex()]["LastTimestamp"])
                self.assertEqual(keys, {file.name: file.read_bytes() for file in service.root.glob("*.seed")})
                self.assertFalse(any(item["port"] == port and parse(item["raw"])[0] == 2
                                     and b"reboot" in body(item["raw"], 2, seed).lower()
                                     for item in modem.all_submissions[start:]))
                after = len(service.logs)
                modem.send(port, 0, request)
                self.assertIn("Willow-", other.command("get name"))
                self.assertEqual(target.command("get name"), "> Retained")
                self.assertFalse(any("ROLE_RESTARTED" in line for line in service.logs[after:]))
                self.assertEqual(modem.epoch, 1)
                self.assertEqual(sum("BOT_PROCESS" in line for line in service.logs), 1)

    def test_restart_reconfirms_native_or_durable_source_factor(self):
        for profile, expected in (("native-preferences", 9), ("durable-host-preferences", 99)):
            with self.subTest(profile=profile), RunningService(
                    root=ROOT.parents[1]/f".lifecycle-af-{os.getpid()}-{time.monotonic_ns():x}",
                    extra_config=f"room.preference_profile={profile}\n") as service:
                room, relay = RoleClient(service, 1), RoleClient(service, 0)
                self.assertEqual(room.command("set af 99"), "OK")
                first = service.emulator.policy_requests.get(timeout=3)
                self.assertEqual(first["factor"], 99)
                after = len(service.logs)
                room.send("reboot")
                service.wait("ROLE_RESTARTED", after=after)
                self.assertEqual(room.command("get af"), f"> {expected}.0")
                if expected != 99:
                    restored = service.emulator.policy_requests.get(timeout=3)
                    self.assertEqual((restored["port"], restored["factor"]), (1, expected))
                self.assertIn("Willow-Relay", relay.command("get name"))
                self.assertEqual(service.emulator.epoch, 1)

    def test_unauthorized_and_room_text_do_not_restart(self):
        with fixture() as service:
            modem = service.emulator
            for port, seed in ((0, 1), (1, 4)):
                stamp = 10
                plain = struct.pack("<I", stamp)+(bytes(4) if port else b"")+b"room\0"
                modem.send(port, 0, packet(7, public(seed)[:1]+public(3)+seal(secret(3, public(seed)), plain)))
                modem.receive(lambda item: item["port"] == port and parse(item["raw"])[0] == 1)
                modem.send(port, 0, addressed(3, seed, 2, struct.pack("<IB", stamp+1, 4)+b"reboot"))
            modem.send(1, 0, text(3, 4, 12, "reboot"))
            modem.receive(lambda item: item["port"] == 1 and parse(item["raw"])[0] == 3)
            self.assertFalse(any("ROLE_RESTARTED" in line for line in service.logs))
            self.assertEqual(modem.epoch, 1)

    def test_failed_commit_does_not_restart(self):
        with fixture() as service:
            target, other = RoleClient(service, 0), RoleClient(service, 1)
            pending = service.root/"relay.state.pending"
            pending.mkdir()
            after = len(service.logs)
            target.send("reboot")
            service.wait("role snapshot commit failed", after=after)
            self.assertFalse(any("ROLE_RESTARTED" in line for line in service.logs[after:]))
            self.assertIn("Willow-Room", other.command("get name"))
            self.assertEqual(service.emulator.epoch, 1)

    def test_admin_recovery_restores_management_without_worker_restart(self):
        with fixture() as service:
            self.assertTrue(owner_rpc(service.root, b"\x01\x04bot name").startswith(b"Name: "))
            deadline = time.monotonic()+6
            while not json.loads(owner_rpc(service.root, b"\x01\x06\x01"))["ready"]:
                self.assertLess(time.monotonic(), deadline, "initial modem telemetry not ready")
                time.sleep(.05)
            old_clock = (service.root/"admin.clock").read_bytes()
            after = len(service.logs)
            fault = service.root/"admin.inject"
            fault.write_bytes(b"\x01")
            fault.chmod(0o600)
            service.wait("OWNER_ADMIN_OFFLINE", after=after)
            deadline = time.monotonic()+4
            while True:
                readiness = json.loads(owner_rpc(service.root, b"\x01\x06\x01"))
                self.assertTrue(readiness["ready"], readiness)
                try:
                    reply = owner_rpc(service.root, b"\x01\x04bot name")
                    break
                except (ValueError, OSError) as error:
                    self.assertLess(time.monotonic(), deadline, str(error))
                    time.sleep(.05)
            self.assertTrue(reply.startswith(b"Name: "))
            self.assertGreater(int.from_bytes((service.root/"admin.clock").read_bytes()[4:], "little"),
                               int.from_bytes(old_clock[4:], "little"))
            self.assertEqual(service.emulator.epoch, 1)
            self.assertEqual(sum("BOT_PROCESS" in line for line in service.logs), 1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
