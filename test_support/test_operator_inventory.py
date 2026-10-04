# SPDX-License-Identifier: Apache-2.0
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from tools.hardware import inventory


class OperatorInventoryTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.path = self.root / "inventory.json"
        environment = patch.dict(os.environ, {"MESHCORE_OPERATOR_CONFIG": str(self.path)})
        environment.start()
        self.addCleanup(environment.stop)
        inventory._read.cache_clear()
        self.addCleanup(inventory._read.cache_clear)

    def write(self, **values):
        self.path.write_text(json.dumps({"format": "meshcore-operator-inventory-v1", **values}))
        self.path.chmod(0o600)

    def test_missing_inventory_never_selects_a_default_device(self):
        with self.assertRaisesRegex(ValueError, "set MESHCORE_OPERATOR_CONFIG"):
            inventory.value("mast_mac")

    def test_permissions_symlinks_and_nonregular_files_are_rejected(self):
        self.write(mast_mac="02:00:00:00:00:01")
        self.path.chmod(0o644)
        with self.assertRaisesRegex(ValueError, "owner-only"):
            inventory.value("mast_mac")
        self.path.chmod(0o600)
        link = self.root / "link.json"
        link.symlink_to(self.path)
        with patch.dict(os.environ, {"MESHCORE_OPERATOR_CONFIG": str(link)}):
            with self.assertRaisesRegex(ValueError, "unavailable"):
                inventory.value("mast_mac")
        with patch.dict(os.environ, {"MESHCORE_OPERATOR_CONFIG": str(self.root)}):
            with self.assertRaisesRegex(ValueError, "owner-only"):
                inventory.value("mast_mac")

    def test_missing_or_invalid_selection_is_explicit(self):
        cases = ({}, {"mast_mac": None}, {"mast_mac": ""},
                 {"mast_mac": 1}, {"mast_mac": "not-a-hardware-mac"})
        for record in cases:
            with self.subTest(record=record):
                self.write(**record)
                inventory._read.cache_clear()
                with self.assertRaisesRegex(ValueError, "mast_mac"):
                    inventory.value("mast_mac")

    def test_radio_profile_and_role_records_are_typed(self):
        for key, value in (("active_profile", [912525000, 250000, 7, 5, True]),
                           ("active_profile", "912.525,250,7,5,2"),
                           ("peer_roles", {"device": ["name"]}),
                           ("telemetry_policy", {"host": "192.0.2.1"})):
            with self.subTest(key=key, value=value):
                self.write(**{key: value})
                inventory._read.cache_clear()
                with self.assertRaisesRegex(ValueError, key):
                    inventory.value(key)
        self.write(mast_mac="02:00:00:00:00:01", nrf_serial="EXAMPLE-NRF-001",
                   active_profile=[912525000, 250000, 7, 5, 2])
        inventory._read.cache_clear()
        self.assertEqual(inventory.value("mast_mac"), "02:00:00:00:00:01")
        self.assertEqual(inventory.value("nrf_serial"), "EXAMPLE-NRF-001")

    def test_invalid_json_format_and_oversized_inventory_are_rejected(self):
        for raw in ("null", '{"format":"unknown"}', "{", " " * 16385):
            with self.subTest(raw=raw[:40]):
                self.path.write_text(raw)
                self.path.chmod(0o600)
                inventory._read.cache_clear()
                with self.assertRaises(ValueError):
                    inventory.value("mast_mac")
