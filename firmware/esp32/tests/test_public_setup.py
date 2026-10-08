# SPDX-License-Identifier: Apache-2.0
import hashlib
import json
from pathlib import Path
import stat
import struct
import tempfile
import unittest
from unittest.mock import patch

import public_setup as setup


def profile():
    return {
        "schema": 1, "roles": 7, "path_width": 3,
        "frequency_hz": 912525000, "bandwidth_hz": 250000,
        "sf": 7, "cr": 5, "tx_dbm": 2,
        "admin_password": "test-admin", "room_password": "", "mast_password": "test-mast",
        "operator_public_key": bytes(range(32)).hex(),
        "trusted_companion_public_key": "",
        "wifi_ssid": "", "wifi_password": "", "wifi_enabled": False,
    }


def partitions():
    result = bytearray()
    for label, kind, subtype, start, size in (
        ("nvs", 1, 2, 0x9000, 0x5000), ("otadata", 1, 0, 0xe000, 0x2000),
        ("app0", 0, 16, 0x10000, 0x330000), ("app1", 0, 17, 0x340000, 0x330000),
        ("spiffs", 1, 130, 0x670000, 0x180000), ("coredump", 1, 3, 0x7f0000, 0x10000),
    ):
        result += struct.pack("<HBBII16sI", 0x50aa, kind, subtype, start, size, label.encode(), 0)
    return bytes(result).ljust(0xc00, b"\xff")


class PublicSetup(unittest.TestCase):
    def test_exact_record_and_digest(self):
        record = setup.encode_profile(json.dumps(profile()))
        self.assertEqual(len(record), 336)
        self.assertEqual(record[:8], b"MCP\1\7\3\0\0")
        self.assertEqual(struct.unpack_from("<II", record, 8), (912525000, 250000))
        self.assertEqual(record[304:], hashlib.sha256(record[:304]).digest())

    def test_narrow_bandwidth_aliases(self):
        for bandwidth in (7800, 7810, 10400, 10420, 15600, 15630, 20800, 20830):
            with self.subTest(bandwidth=bandwidth):
                record = setup.encode_profile(json.dumps(profile() | {"bandwidth_hz": bandwidth}))
                self.assertEqual(struct.unpack_from("<I", record, 12)[0], bandwidth)
                self.assertEqual(record[304:], hashlib.sha256(record[:304]).digest())

    def test_unknown_missing_duplicate_and_malformed_fields(self):
        for value in (
            profile() | {"seed": "private"}, {k: v for k, v in profile().items() if k != "roles"},
            [], {"schema": 1}, profile() | {"roles": True}, profile() | {"sf": 7.0},
            profile() | {"wifi_enabled": 1},
        ):
            with self.subTest(value=value), self.assertRaises(ValueError):
                setup.encode_profile(json.dumps(value))
        for value in ('{"schema":1,"schema":1}', '{"schema":', b"\xff",
                      " " * (setup.MAX_PROFILE + 1), "[" * 1500 + "]" * 1500):
            with self.assertRaises(ValueError):
                setup.encode_profile(value)

    def test_limits_and_unsafe_strings(self):
        for name, value in (
            ("frequency_hz", 100000000), ("frequency_hz", 960000001),
            ("bandwidth_hz", 200000), ("sf", 13), ("cr", 4), ("tx_dbm", 23),
            ("roles", 16), ("path_width", 0), ("path_width", 4),
            ("admin_password", ""), ("admin_password", "x" * 16),
            ("mast_password", "secret\n"), ("room_password", "\0"),
            ("operator_public_key", "0" * 64), ("operator_public_key", "f" * 64),
            ("operator_public_key", "1" * 128), ("trusted_companion_public_key", "G" * 64),
            ("wifi_ssid", "x" * 33), ("wifi_password", "short"),
        ):
            with self.subTest(name=name, value=value), self.assertRaises(ValueError):
                setup.encode_profile(json.dumps(profile() | {name: value}))
        with self.assertRaises(ValueError):
            setup.encode_profile(json.dumps(profile() | {"wifi_enabled": True}))

    def test_example_requires_owner_authority(self):
        with self.assertRaises(ValueError):
            setup.encode_profile((setup.ROOT / "firmware/esp32/public-profile.example.json").read_bytes())

    def test_private_files_no_symlinks_no_overwrite(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            file = setup.create_private(root / "profile.json", b"private")
            self.assertEqual(stat.S_IMODE(file.stat().st_mode), 0o600)
            self.assertEqual(setup.read_private(file, 7), b"private")
            with self.assertRaises(ValueError):
                setup.read_private(file, 6)
            with self.assertRaises(FileExistsError):
                setup.create_private(file, b"replacement")
            file.chmod(0o644)
            with self.assertRaises(ValueError):
                setup.read_private(file, 10)
            link = root / "link"
            link.symlink_to(file)
            with self.assertRaises(ValueError):
                setup.read_private(link, 10)
            linked_parent = root / "parent"
            linked_parent.symlink_to(root, target_is_directory=True)
            with self.assertRaises(ValueError):
                setup.create_private(linked_parent / "new", b"secret")
            with self.assertRaises(ValueError):
                setup.create_private(setup.ROOT / "leak", b"secret")
            with self.assertRaises(ValueError):
                setup.read_private(root, 10)

    def test_public_permissions_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            root.chmod(0o755)
            with self.assertRaises(ValueError):
                setup.create_private(root / "secret", b"secret")

    def test_exact_existing_partition_layout(self):
        self.assertEqual(setup.storage_partitions(partitions())["spiffs"][2:], (0x670000, 0x180000))
        for index in (8, 2 * 32 + 4, 3 * 32 + 8, 4 * 32 + 4):
            content = bytearray(partitions())
            content[index] ^= 1
            with self.assertRaises(ValueError):
                setup.storage_partitions(content)

    def test_real_offline_spiffs_image(self):
        executable = Path.home() / ".platformio/packages/tool-mkspiffs/mkspiffs_espressif32_arduino"
        if not executable.is_file():
            self.skipTest("PlatformIO mkspiffs is not installed")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            table = root / "partitions.bin"
            table.write_bytes(partitions())
            source = setup.create_private(root / "profile.json", json.dumps(profile()).encode())
            output = root / "private-spiffs.bin"
            setup.build_image(source, output, table, executable)
            self.assertEqual(output.stat().st_size, 0x180000)
            self.assertEqual(stat.S_IMODE(output.stat().st_mode), 0o600)
            extracted = root / "extracted"
            extracted.mkdir(mode=0o700)
            setup.run([str(executable), "-u", str(extracted), "-b", "4096", "-p", "256",
                       "-s", str(0x180000), str(output)])
            self.assertEqual({p.name for p in extracted.iterdir()}, {"onchip-layout", "public-setup.bin"})
            self.assertEqual((extracted / "onchip-layout").read_bytes(), b"meshcore-onchip-fs-v1\n")
            self.assertEqual((extracted / "public-setup.bin").read_bytes(),
                             setup.encode_profile(json.dumps(profile())))

    def test_usb_blank_existing_and_backup_confirmation(self):
        class Loader:
            CHIP_NAME = "ESP32-S3"
            nvs = b"\xff" * 0x5000
            filesystem = b"\xff" * 0x180000
            table = partitions()
            _port = type("Port", (), {"close": lambda self: None})()
            def run_stub(self):
                return self
            def read_flash(self, start, size):
                return {0x8000: self.table, 0x9000: self.nvs, 0x670000: self.filesystem}[start]
        loader = Loader()
        module = type("Esptool", (), {"__version__": "4.5.1", "FatalError": RuntimeError,
                                     "detect_chip": lambda **kwargs: loader})
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            table = root / "partitions.bin"
            table.write_bytes(partitions())
            image = setup.create_private(root / "spiffs.bin", b"\1" * 0x180000)
            with patch.object(setup.importlib, "import_module", return_value=module), \
                    patch.object(setup, "run") as run:
                setup.install_config(image, table, root / "blank-backup", "esptool.py", "USB")
                self.assertEqual(run.call_count, 2)
                self.assertEqual(run.call_args_list[0].args[0][-2], "0x670000")
                self.assertEqual((root / "blank-backup").read_bytes(), loader.filesystem)
                self.assertFalse(any(path.name.endswith("nvs") for path in root.iterdir()))
                loader.nvs = b"\0" * 0x5000
                run.reset_mock()
                with self.assertRaisesRegex(ValueError, "Existing NVS/SPIFFS"):
                    setup.install_config(image, table, root / "existing-backup", "esptool.py", "USB")
                run.assert_not_called()
                self.assertTrue((root / "existing-backup").exists())
                setup.install_config(image, table, root / "confirmed-backup", "esptool.py", "USB", True)
                self.assertEqual(run.call_count, 2)
                loader.table = b"\0" * 0xc00
                run.reset_mock()
                with self.assertRaisesRegex(ValueError, "partition table"):
                    setup.install_config(image, table, root / "wrong-table", "esptool.py", "USB", True)
                run.assert_not_called()
                self.assertFalse((root / "wrong-table").exists())


if __name__ == "__main__":
    unittest.main()
