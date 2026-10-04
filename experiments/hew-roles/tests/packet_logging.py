"""Private JSONL logging, bounded history, terminal outcomes and native restart."""
import datetime
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import struct
import unittest

from parity import ROOT, Host, body, login, parse, public, secret, seal, packet, text, tx
from clock_location import owner
from service_demo import RunningService, addressed, body as service_body

sys.path.insert(0, str(ROOT))
from reconcile_go import snapshot
from willow import packet_log, owner_rpc


class PacketLogging(unittest.TestCase):
    def test_native_service_terminal_metrics_disconnect_and_no_replay(self):
        with RunningService(root=ROOT.parents[1]/f".packet-log-{os.getpid()}-{time.monotonic_ns():x}") as service:
            modem = service.emulator
            log = service.root/"room.state.packet.log"
            startup = next(item for item in modem.all_submissions if item["port"] == 1 and parse(item["raw"])[0] == 4)
            saved = snapshot((service.root/"room.state").read_bytes(), public(4), 3, "startup advert")
            self.assertEqual(saved["Clock"], int.from_bytes(parse(startup["raw"])[4][32:36], "little"))
            before = datetime.datetime.now(datetime.timezone.utc)
            clock = owner_rpc(service.root, bytes((1, 5, 3))+b"clock").decode()
            after = datetime.datetime.now(datetime.timezone.utc)
            possible = {f"{value:%H:%M} - {value.day}/{value.month}/{value.year} UTC" for value in (before, after)}
            self.assertIn(clock, possible)
            modem.send(1, 0, packet(7, public(4)[:1]+public(2)+seal(secret(2, public(4)),
                struct.pack("<II", 10, 0)+b"admin\0")))
            modem.receive(lambda item: item["port"] == 1 and parse(item["raw"])[0] == 1)
            modem.send(1, 0, addressed(2, 4, 2, struct.pack("<IB", 11, 4)+b"log start"))
            started = modem.receive(lambda item: item["port"] == 1 and parse(item["raw"])[0] == 2)
            self.assertEqual(service_body(started["raw"], 2, 4)[5:].rstrip(b"\0"), b"   logging on")

            def records():
                return [json.loads(line) for line in log.read_bytes().splitlines(keepends=True) if line.endswith(b"\n")]

            def outcome(job, state):
                deadline = time.monotonic()+5
                while time.monotonic() < deadline:
                    found = [record for record in records() if record.get("tx_result", {}).get("JobID") == job
                             and record["tx_result"]["State"] == state]
                    if found: return found
                    time.sleep(.02)
                self.fail(f"terminal log missing job={job} state={state}\n"+"\n".join(service.logs[-30:]))

            modem.hold = True
            modem.send(1, 0, addressed(2, 4, 2, struct.pack("<IB", 12, 4)+b"get name"))
            failed = modem.receive(lambda item: item["port"] == 1 and parse(item["raw"])[0] == 2)
            terminal = b"\xfa\1"+struct.pack("<II", failed["generation"], failed["job"])+b"\3\10"+struct.pack("<III", 17, 31, 200)
            modem.send(1, 6, terminal)
            seen = outcome(failed["job"], 3)
            self.assertEqual(len(seen), 1)
            self.assertEqual(seen[0]["raw_packet_hex"], failed["raw"].hex())
            self.assertEqual(seen[0]["tx_result"], {"Generation": failed["generation"], "JobID": failed["job"],
                "State": 3, "Reason": 8, "QueueWait": 17_000_000, "RFAirtime": 31_000_000, "EstimatedAirtime": 200_000_000})
            modem.send(1, 6, terminal)
            modem.hold = True
            modem.send(1, 0, addressed(2, 4, 2, struct.pack("<IB", 13, 4)+b"get owner.info"))
            uncertain = modem.receive(lambda item: item["port"] == 1 and parse(item["raw"])[0] == 2)
            modem.disconnect()
            unknown = outcome(uncertain["job"], 4)
            self.assertEqual(len(unknown), 1)
            self.assertEqual(unknown[0]["raw_packet_hex"], uncertain["raw"].hex())
            self.assertEqual(unknown[0]["tx_result"]["EstimatedAirtime"], (10+len(uncertain["raw"]))*1_000_000)
            self.assertEqual(unknown[0]["tx_result"]["Reason"], 9)
            service.wait("ONLINE epoch=2", 10)
            self.assertEqual(sum(record.get("tx_result", {}).get("JobID") == failed["job"] and
                record["tx_result"]["State"] == 3 for record in records()), 1)
            self.assertEqual(sum(item["raw"] == uncertain["raw"] for item in modem.all_submissions), 1)
            self.assertEqual(packet_log(service.root, "room"), log.read_bytes())

    def test_capture_rf_reflection_tx_outcomes_and_restart(self):
        for binary in ("roles", "roles-release"):
            with self.subTest(binary=binary), tempfile.TemporaryDirectory(dir=ROOT/"build") as directory:
                root = Path(directory)
                log, state = root/"room.state.packet.log", root/"room.state"
                with Host(binary=ROOT/"build"/binary) as host:
                    self.assertEqual(host.command(f"logfile {log} 4194304"), ["true"])
                    self.assertEqual(host.command("state "+str(state)), ["LOCKED"])
                    self.assertTrue(host.packet(login(2, 10, "admin")))
                    self.assertEqual(owner(host, "log start"), "logging on")
                    host.command("measurement 000092c20000904000")
                    measured, reflected = text(2, 11, b"get name", 4), text(2, 12, b"clock", 4)
                    sent = host.packet(measured)
                    host.command("confirmed "+sent[0])
                    host.packet(reflected, local=True)
                    for physical in range(5):
                        host.command(f"outcome 1000000 7 {100+physical} {physical} 3 2000 120 200 {measured.hex()}")
                    records = [json.loads(line) for line in log.read_bytes().splitlines()]
                    self.assertEqual(log.stat().st_mode&0o777, 0o600)
                    rf = next(record for record in records if record["direction"] == "RX" and record["raw_packet_hex"] == measured.hex())
                    local = next(record for record in records if record["direction"] == "RX" and record["raw_packet_hex"] == reflected.hex())
                    self.assertEqual((rf["local_loopback"], rf["snr"], rf["rssi"]), (False, 4.5, -73))
                    self.assertEqual((local["local_loopback"], local["snr"], local["rssi"]), (True, None, None))
                    self.assertEqual(sum(record["direction"] == "TX" for record in records), 1)
                    terminals = [record for record in records if record["direction"] == "TX_RESULT"]
                    self.assertEqual([record["tx_result"]["State"] for record in terminals], [0, 3, 4])
                    for record in terminals:
                        result = record["tx_result"]
                        self.assertEqual((record["local_loopback"], record["snr"], record["rssi"]), (False, None, None))
                        self.assertEqual(result["Generation"], 7)
                        self.assertEqual(result["JobID"], 100+result["State"])
                        self.assertEqual(result["QueueWait"], 2_000_000_000)
                        self.assertEqual(result["RFAirtime"], 120_000_000)
                        self.assertEqual(result["EstimatedAirtime"], 200_000_000)
                    for record in records:
                        self.assertIsNotNone(datetime.datetime.fromisoformat(record["time"].replace("Z", "+00:00")).tzinfo)
                    self.assertNotIn("Logging", snapshot(state.read_bytes(), state.read_bytes()[4:36], 3, "log")["settings"] or {})
                old = log.read_bytes()
                with Host(binary=ROOT/"build"/binary) as host:
                    self.assertEqual(host.command(f"logfile {log} 4194304"), ["true"])
                    self.assertEqual(host.command("state "+str(state)), ["LOCKED"])
                    host.packet(text(2, 13, b"get name", 4))
                    self.assertEqual(log.read_bytes(), old)
                    self.assertEqual(owner(host, "log start"), "logging on")
                    host.packet(text(2, 14, b"get name", 4))
                    self.assertTrue(log.read_bytes().startswith(old))
                    self.assertGreater(len(log.read_bytes()), len(old))
                    self.assertEqual(owner(host, "log stop"), "logging off")
                    stopped = log.read_bytes()
                    host.packet(text(2, 15, b"get name", 4))
                    self.assertEqual(log.read_bytes(), stopped)
                    self.assertEqual(packet_log(root, "room"), stopped)
                    self.assertEqual(owner(host, "log erase"), "log erased")
                    self.assertFalse(log.exists())
                    self.assertNotIn("Logging", snapshot(state.read_bytes(), state.read_bytes()[4:36], 3, "log")["settings"] or {})
                self.assertEqual(packet_log(root, "room"), b"")

    def test_log_limit_is_exact_and_never_overwrites(self):
        for binary in ("roles", "roles-release"):
            with self.subTest(binary=binary), tempfile.TemporaryDirectory(dir=ROOT/"build") as directory:
                root = Path(directory)
                log, state = root/"room.state.packet.log", root/"room.state"
                retained = b"x"*1023
                log.write_bytes(retained); log.chmod(0o600)
                with Host(binary=ROOT/"build"/binary) as host:
                    self.assertEqual(host.command(f"logfile {log} 1024"), ["true"])
                    self.assertEqual(host.command("state "+str(state)), ["LOCKED"])
                    host.packet(login(2, 10, "admin"))
                    self.assertEqual(owner(host, "log start"), "logging on")
                    lines = host.command("packet 1000000 0 "+text(2, 11, b"get name", 4).hex())
                    self.assertTrue(any("PACKET_LOG_STOPPED" in line and "limit-reached" in line for line in lines))
                    self.assertEqual(body(tx(lines)[0], 2)[5:].rstrip(b"\0"), b"> Hew Room")
                    self.assertEqual(log.read_bytes(), retained)
                    host.packet(text(2, 12, b"get name", 4))
                    self.assertEqual(log.read_bytes(), retained)
                    log.write_bytes(b"x"*1024)
                    reply = owner(host, "log start")
                    self.assertTrue(reply.startswith("Error, packet log unavailable"))
                    self.assertEqual(log.read_bytes(), b"x"*1024)

    def test_private_regular_file_guards_and_local_reader(self):
        with tempfile.TemporaryDirectory(dir=ROOT/"build") as directory:
            root = Path(directory)
            target = root/"retained"
            target.write_bytes(b"never erase or overwrite")
            target.chmod(0o600)
            log = root/"room.state.packet.log"
            for kind in ("symlink", "hardlink", "public", "directory", "fifo"):
                with self.subTest(kind=kind):
                    if kind == "symlink": log.symlink_to(target)
                    elif kind == "hardlink": os.link(target, log)
                    elif kind == "public": log.write_bytes(b"public"); log.chmod(0o644)
                    elif kind == "directory": log.mkdir()
                    else: os.mkfifo(log, 0o600)
                    with Host() as host:
                        self.assertEqual(host.command(f"logfile {log} 4194304"), ["true"])
                        self.assertTrue(owner(host, "log start").startswith("Error, packet log unavailable"))
                    with self.assertRaises((ValueError, OSError)):
                        packet_log(root, "room")
                    self.assertEqual(target.read_bytes(), b"never erase or overwrite")
                    if log.is_dir(): log.rmdir()
                    else: log.unlink()
            log.write_bytes(b"x"*4194305); log.chmod(0o600)
            with self.assertRaises(ValueError):
                packet_log(root, "room")
            log.write_bytes(b"retained\n")
            result = subprocess.run([sys.executable, "-B", str(ROOT/"willow.py"), "packet-log",
                "--state", str(root), "--role", "room"], capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr.decode())
            self.assertEqual(result.stdout, b"retained\n")
            with self.assertRaises(ValueError):
                packet_log(root, "bot")

    def test_erase_waits_for_durable_role_commit(self):
        with tempfile.TemporaryDirectory(dir=ROOT/"build") as directory:
            root = Path(directory)
            log, state = root/"room.state.packet.log", root/"room.state"
            retained = b"retained log history\n"
            log.write_bytes(retained); log.chmod(0o600)
            with Host() as host:
                self.assertEqual(host.command(f"logfile {log} 4194304"), ["true"])
                self.assertEqual(host.command("state "+str(state)), ["LOCKED"])
                pending = root/"room.state.pending"
                pending.write_bytes(b"occupied transaction slot"); pending.chmod(0o600)
                lines = host.command("owner 1000000 "+b"log erase".hex())
                self.assertIn("ERROR state commit; role stopped", lines)
                self.assertEqual(log.read_bytes(), retained)


if __name__ == "__main__":
    unittest.main(verbosity=2)
