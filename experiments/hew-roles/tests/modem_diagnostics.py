"""Differentiate modem failures without changing deadlines or retry behavior."""
import os
import re
import socket
import struct
import subprocess
import time
import unittest
from unittest.mock import patch

from service_demo import Emulator, RunningService, ROOT, PROFILE


class ControlledModem(Emulator):
    def __init__(self):
        self.profile_fault = ""
        super().__init__()

    def handle(self, frame, connection, epoch):
        if frame[:2] == b"\x06\x22" and self.profile_fault:
            fault = self.profile_fault
            self.profile_fault = ""
            if fault == "drop":
                self.send(0, 6, b"\xf9\x12\xb7", connection)
                return
            if fault == "short":
                self.send(0, 6, b"\xa2\x01\x00", connection)
                return
        super().handle(frame, connection, epoch)


class ModemDiagnostics(unittest.TestCase):
    def exercise(self, cause):
        began = time.time()
        with patch("service_demo.Emulator", ControlledModem):
            with RunningService() as service:
                service.wait("TX port=2 generation=103 job=1 state=2")
                modem = service.emulator
                if cause == "eof":
                    modem.disconnect()
                elif cause == "reset":
                    modem.connection.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                                                struct.pack("ii", 1, 0))
                    modem.connection.close()
                elif cause == "mismatch":
                    modem.drift = True
                else:
                    modem.profile_fault = cause
                service.wait("ONLINE epoch=2", timeout=15)
                self.assertEqual(len([line for line in service.logs
                                      if line.startswith("BOT_PROCESS")]), 1)
                failures = [line for line in service.logs if line.startswith((
                    "MODEM_READ_END", "MODEM_READ_FAILED", "MODEM_CONTROL_FAILED",
                    "MODEM_SESSION_FAILED"))]
                self.assertTrue(failures, service.logs)
                for line in failures:
                    stamp = re.search(r"\bat_ms=(\d+)\b", line)
                    self.assertIsNotNone(stamp, line)
                    self.assertGreaterEqual(int(stamp[1]) / 1000, began - 1)
                    self.assertLessEqual(int(stamp[1]) / 1000, time.time() + 1)
                if cause == "eof":
                    self.assertTrue(any("MODEM_READ_END" in line and "reason=eof" in line
                                        for line in failures), failures)
                elif cause == "reset":
                    self.assertTrue(any(re.search(r"errno=(?:32|104)\b", line)
                                        for line in failures), failures)
                elif cause == "drop":
                    deadlines = [line for line in failures if "reason=deadline" in line]
                    self.assertEqual(len(deadlines), 1, failures)
                    self.assertIn("stage=CONFIG_GET", deadlines[0])
                    self.assertIn("expected_opcode=a2 timeout_ms=2000", deadlines[0])
                    self.assertIn("overdue_ms=", deadlines[0])
                    self.assertFalse(any("MODEM_READ_" in line for line in failures), failures)
                else:
                    rejected = [line for line in failures if "reason=invalid-readback" in line]
                    self.assertEqual(len(rejected), 1, failures)
                    self.assertIn("profile=" + PROFILE.hex(), rejected[0])
                    if cause == "short":
                        self.assertIn("actual=06a20100", rejected[0])
                self.assertTrue(any(line.startswith("OFFLINE epoch=1 ") and "at_ms=" in line
                                    for line in service.logs), service.logs)

    def test_remote_eof(self): self.exercise("eof")
    def test_remote_reset_errno(self): self.exercise("reset")
    def test_profile_deadline_with_unrelated_metadata(self): self.exercise("drop")
    def test_profile_mismatch(self): self.exercise("mismatch")
    def test_truncated_profile(self): self.exercise("short")

    def test_session_write_and_outcome_failure_are_bounded(self):
        binary = os.environ.get("MESHCORE_HEW_SESSION_CHECKS", str(ROOT / "build/session-checks"))
        result = subprocess.run([binary], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        failures = [line for line in result.stdout.splitlines()
                    if line.startswith("MODEM_SESSION_FAILED")]
        self.assertEqual(len(failures), 2, result.stdout)
        self.assertIn("stage=TX_SUBMIT", failures[0])
        self.assertIn("committed=0", failures[0])
        self.assertIn("stage=TX_OUTCOME", failures[1])
        self.assertIn("job=7", failures[1])
        self.assertTrue(all("at_ms=" in line for line in failures))


if __name__ == "__main__":
    unittest.main(verbosity=2)
