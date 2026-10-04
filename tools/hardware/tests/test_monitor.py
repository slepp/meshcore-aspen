# SPDX-License-Identifier: Apache-2.0
import copy
import json
import tempfile
import unittest
from unittest.mock import patch

import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
from tools.hardware import monitor as field_monitor


from test_support.operator_inventory import configure_inventory

class FieldMonitorTests(unittest.TestCase):
    def setUp(self):
        configure_inventory(self)

    def reading(self):
        return {
            "uptime_ms": 1000,
            "profile": dict(zip(field_monitor.PHY_FIELDS, field_monitor.active_profile()),
                            committed=True, fault=False),
            "memory": {name: 20000 for name in field_monitor.MEMORY_FIELDS},
            "totals": {name: 0 for name in field_monitor.ERROR_COUNTERS},
            "scheduler": {"queued": 0, "transmitting": False},
            "roles": [{"role": name, "state": "running", "ready": True,
                       "public_key": name, "fault": None} for name in field_monitor.REQUIRED_ROLES],
        }

    def test_exact_full_hour_with_start_end_and_memory_changes(self):
        clock = [0]
        calls = [0]

        def fetch():
            value = self.reading()
            value["uptime_ms"] += int(clock[0] * 1000)
            value["memory"]["free_bytes"] -= calls[0] * 10
            calls[0] += 1
            return value

        with tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parents[3] / ".tmp") as directory:
            with patch.object(field_monitor, "DIRECTORY", Path(directory)), \
                    patch("tools.hardware.mast_checks.DIRECTORY", Path(directory)), \
                    patch.object(field_monitor, "dashboard", side_effect=fetch), \
                    patch.object(field_monitor.time, "monotonic", side_effect=lambda: clock[0]), \
                    patch.object(field_monitor.time, "sleep",
                                 side_effect=lambda seconds: clock.__setitem__(0, clock[0] + seconds)):
                result = field_monitor.monitor()
            self.assertTrue(result["completed"])
            self.assertEqual(result["duration_seconds"], 3600)
            self.assertEqual(result["samples"], 121)
            self.assertEqual(calls[0], 121)
            self.assertEqual(result["error_count"], 0)
            self.assertEqual(result["memory_delta_bytes"]["free_bytes"], -1200)
            self.assertEqual(result["minimum_observed_memory"]["free_bytes"], 18800)
            lines = Path(result["sample_log"]).read_text().splitlines()
            self.assertEqual(len(lines), 121)
            self.assertEqual(json.loads(lines[0])["elapsed_seconds"], 0)
            self.assertEqual(json.loads(lines[-1])["elapsed_seconds"], 3600)

    def test_errors_identity_restart_and_real_phy_guard(self):
        initial = field_monitor.snapshot(self.reading())
        changed = copy.deepcopy(initial)
        changed["uptime_ms"] = 0
        changed["totals"]["tx_failed"] = 1
        changed["roles"]["command-bot"]["public_key"] = "another"
        changed["roles"]["room"]["ready"] = False
        errors = field_monitor.problems(changed, initial, initial)
        self.assertEqual(len(errors), 4)
        wrong_phy = self.reading()
        wrong_phy["profile"]["frequency_hz"] = 910525000
        with patch.object(field_monitor, "dashboard", return_value=wrong_phy):
            with self.assertRaisesRegex(ValueError, "before.*912.525"):
                field_monitor.monitor()
        with self.assertRaisesRegex(ValueError, "Incomplete"):
            field_monitor.snapshot({})

    def test_last_fetch_failure_is_not_a_successful_end_reading(self):
        clock = [0]
        readings = [self.reading(), OSError("endpoint unreachable")]
        with tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parents[3] / ".tmp") as directory:
            with patch.object(field_monitor, "DIRECTORY", Path(directory)), \
                    patch("tools.hardware.mast_checks.DIRECTORY", Path(directory)), \
                    patch.object(field_monitor, "INTERVAL", 3600), \
                    patch.object(field_monitor, "dashboard", side_effect=readings), \
                    patch.object(field_monitor.time, "monotonic", side_effect=lambda: clock[0]), \
                    patch.object(field_monitor.time, "sleep",
                                 side_effect=lambda seconds: clock.__setitem__(0, clock[0] + seconds)):
                result = field_monitor.monitor()
            self.assertTrue(result["completed"])
            self.assertIsNone(result["end_reading"])
            self.assertIsNone(result["memory_delta_bytes"])
            self.assertEqual(result["unavailable_samples"], 1)
            self.assertEqual(result["error_count"], 1)


if __name__ == "__main__":
    unittest.main()
