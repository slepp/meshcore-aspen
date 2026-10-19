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
            ".text 798716 159744\n.ARM.extab 8 958460\n", ""
        ]):
            with self.assertRaisesRegex(ValueError, "798724 > 798720"):
                memory.measure(pathlib.Path("fixture.elf"), "size", "nm")

    def test_rak_layout_uses_its_softdevice_boundary(self):
        with patch.object(memory.subprocess, "check_output", side_effect=[
            ".text 802816 155648\n", "20010000 B __HeapBase\n2003f800 B __HeapLimit\n2003f800 B __StackLimit\n20040000 B __StackTop\n"
        ]):
            result = memory.measure(pathlib.Path("fixture.elf"), "size", "nm")
        self.assertEqual(result["application_start"], 0x26000)
        self.assertEqual(result["application_flash_bytes"], 802816)
        with patch.object(memory.subprocess, "check_output", side_effect=[".text 24 151552\n", ""]):
            with self.assertRaisesRegex(ValueError, "application start"):
                memory.measure(pathlib.Path("fixture.elf"), "size", "nm")


class ProfileGuardTest(unittest.TestCase):
    SECTIONS = ".text 700000 159744\n"
    SYMBOLS = ("20010000 B __HeapBase\n2003f800 B __HeapLimit\n2003f800 B __StackLimit\n"
               "20040000 B __StackTop\n00040000 T __ssvfscanf_r\n")

    def measure(self, **options):
        with patch.object(memory.subprocess, "check_output", side_effect=[self.SECTIONS, self.SYMBOLS]):
            return memory.measure(pathlib.Path("fixture.elf"), "size", "nm", **options)

    def test_reserve_fails_before_the_application_limit(self):
        self.assertEqual(self.measure(reserve=98720)["application_flash_bytes"], 700000)
        with self.assertRaisesRegex(ValueError, "leaves 98720 bytes.*at least 98721"):
            self.measure(reserve=98721)

    def test_forbidden_library_code_is_named(self):
        with self.assertRaisesRegex(ValueError, "__ssvfscanf_r"):
            self.measure(forbid=["_jp2uc_l", "__ssvfscanf_r"])
        self.assertEqual(self.measure(forbid=["_jp2uc_l"])["application_flash_bytes"], 700000)
