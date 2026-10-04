# SPDX-License-Identifier: Apache-2.0
import pathlib
import struct
import unittest
from unittest.mock import patch

import memory


class MemoryTest(unittest.TestCase):
    def test_arm_role_constants_are_not_reported_as_free_heap(self):
        symbols = (
            "20010000 B __HeapBase\n2003f800 B __HeapLimit\n"
            "2003f800 B __StackLimit\n20040000 B __StackTop\n"
            "00027010 T _ZN6onchip10BotSession12StorageBytesE\n"
            "00027014 T _ZN6onchip9BotWorker12StorageBytesE\n"
            "00027018 T _ZN6onchip10CommandBot12StorageBytesE\n"
        )
        image = b"\0" * 16 + struct.pack("<III", 12456, 24312, 23368)
        with patch.object(memory.subprocess, "check_output", side_effect=[
            ".text 28 159744\n.ARM.extab 272 159772\n.bss 8192 536895488\n", symbols
        ]), patch.object(pathlib.Path, "exists", return_value=True), \
                patch.object(pathlib.Path, "read_bytes", return_value=image), \
                patch.object(pathlib.Path, "stat") as stat:
            stat.return_value.st_size = len(image)
            result = memory.measure(pathlib.Path("fixture.elf"), "size", "nm")
        self.assertEqual(result["runtime_role_storage_bytes"],
                         {"session": 12456, "worker": 24312, "command_bot": 23368})
        self.assertIsNone(result["runtime_heap_free_bytes"])
        self.assertEqual(result["application_flash_bytes"], 300)

    def test_exception_tables_count_toward_application_limit(self):
        with patch.object(memory.subprocess, "check_output", side_effect=[
            ".text 811004 159744\n.ARM.extab 8 970748\n", ""
        ]):
            with self.assertRaisesRegex(ValueError, "811012 > 811008"):
                memory.measure(pathlib.Path("fixture.elf"), "size", "nm")
