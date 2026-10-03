# SPDX-License-Identifier: Apache-2.0
import struct
import unittest
import zlib
from pathlib import Path
import shutil
import uuid
import sys
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
from tools.hardware.esp32_slot import app_slot, live_app_slot

def partition(kind, subtype, address, size):
    return struct.pack("<HBBII16sI", 0x50aa, kind, subtype, address, size, b"test", 0)
def selection(seq, state=2):
    return struct.pack("<I20sII", seq, b"\xff" * 20, state,
                       zlib.crc32(struct.pack("<I", seq), 0xffffffff))

class Esp32Slot(unittest.TestCase):
    table = (partition(1, 0, 0xe000, 0x2000) +
             partition(0, 0x10, 0x10000, 0x330000) +
             partition(0, 0x11, 0x340000, 0x330000))

    def test_active_slot(self):
        ota = bytearray(b"\xff" * 8192)
        self.assertEqual(app_slot(self.table, ota), (0x10000, 0x330000))
        ota[:32] = selection(1)
        ota[4096:4128] = selection(2)
        self.assertEqual(app_slot(self.table, ota), (0x340000, 0x330000))
        ota[:32] = selection(3)
        self.assertEqual(app_slot(self.table, ota), (0x10000, 0x330000))
        ota[:32] = selection(3, 3)
        self.assertEqual(app_slot(self.table, ota), (0x340000, 0x330000))

    def test_unsafe_selection(self):
        for record in (selection(1, 1), selection(1, 0), b"\0" * 32):
            ota = record + b"\xff" * (8192 - 32)
            with self.assertRaises(ValueError):
                app_slot(self.table, ota)

    def test_live_usb_selection_reads_only_boot_metadata_and_cleans_snapshots(self):
        root = Path(__file__).resolve().parents[3]
        directory = root / ".tmp" / ("onchip-esp-slot-test-" + uuid.uuid4().hex)
        directory.mkdir()
        ota = bytearray(b"\xff" * 8192)
        ota[:32], ota[4096:4128] = selection(1), selection(2)
        calls = []

        def read_flash(command, **kwargs):
            calls.append(command)
            self.assertEqual(command[-4], "read_flash")
            address = int(command[-3], 16)
            self.assertIn(address, (0x8000, 0xe000))
            data = self.table + b"\xff" * (4096 - len(self.table)) if address == 0x8000 else ota
            Path(command[-1]).write_bytes(data)

        try:
            with patch("tools.hardware.esp32_slot.subprocess.run", side_effect=read_flash):
                self.assertEqual(live_app_slot("esptool.py", "test-port", directory, self.table),
                                 (0x340000, 0x330000))
                self.assertEqual(len(calls), 2)
                self.assertEqual(list(directory.iterdir()), [])
                with self.assertRaises(ValueError):
                    live_app_slot("esptool.py", "test-port", directory, b"wrong-table")
                self.assertEqual(list(directory.iterdir()), [])
        finally:
            shutil.rmtree(directory)
