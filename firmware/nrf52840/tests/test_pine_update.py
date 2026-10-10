# SPDX-License-Identifier: Apache-2.0
import binascii
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import unittest
from unittest.mock import patch, MagicMock
from types import SimpleNamespace
import zipfile

HERE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(HERE))
import pine_update
import peer_setup
from nacl.bindings import crypto_scalarmult_ed25519_base_noclamp


class PineUpdateTests(unittest.TestCase):
    def setUp(self):
        self.directory = HERE / ".build" / f"test-pine-update-{os.getpid()}"
        self.directory.mkdir(mode=0o700)
        self.key = self.directory / "signer"
        subprocess.run(["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-f", str(self.key)], check=True)
        self.package = self.directory / "application.zip"
        binary = struct.pack("<II", 0x20030000, 0x27001)
        crc = binascii.crc_hqx(binary, 0xFFFF)
        init = struct.pack("<HHIHHH", 0x52, 0, 1, 1, 0x123, crc)
        metadata = {"manifest": {"dfu_version": 0.5, "application": {
            "bin_file": "firmware.bin", "dat_file": "firmware.dat",
            "init_packet_data": {"device_type": 0x52, "device_revision": 0,
                                 "application_version": 1, "softdevice_req": [0x123], "firmware_crc16": crc}}}}
        with zipfile.ZipFile(self.package, "w") as archive:
            archive.writestr("firmware.bin", binary)
            archive.writestr("firmware.dat", init)
            archive.writestr("manifest.json", json.dumps(metadata))
        self.board = "xiao-nrf52840-wio-sx1262"
        self.data = pine_update.create_manifest(self.package, self.board, "a" * 40, "0.1.1")
        self.manifest = self.directory / "pine.json"
        self.signature = self.directory / "pine.json.sig"
        self.sign()

    def tearDown(self):
        shutil.rmtree(self.directory)

    def sign(self):
        self.manifest.write_text(json.dumps(self.data))
        self.signature.unlink(missing_ok=True)
        subprocess.run(["ssh-keygen", "-Y", "sign", "-f", str(self.key), "-n", "pine-update",
                        str(self.manifest)], check=True, capture_output=True)

    def verify(self, board=None):
        return pine_update.verify(self.manifest, self.signature, self.package,
                                  board or self.board, self.key.with_suffix(".pub"))

    def test_valid_signature_and_application_layout(self):
        self.assertEqual(self.verify(), self.data)
        self.assertTrue(self.data["update_only"])
        self.assertEqual(self.data["layout"]["application_start"], 0x27000)
        self.assertEqual(self.data["application"]["bytes"], 8)

    def test_signature_hardware_layout_and_hash_fail_closed(self):
        with self.assertRaisesRegex(ValueError, "hardware"):
            self.verify("sensecap-solar")
        self.manifest.write_text(self.manifest.read_text() + " ")
        with self.assertRaisesRegex(ValueError, "signature"):
            self.verify()
        self.data["layout"]["application_start"] = 0
        self.sign()
        with self.assertRaisesRegex(ValueError, "layout"):
            self.verify()
        self.data["layout"]["application_start"] = 0x27000
        self.data["application"]["sha256"] = "0" * 64
        self.sign()
        with self.assertRaisesRegex(ValueError, "application"):
            self.verify()

    def test_wrong_product_and_package_digest(self):
        self.data["product"] = "aspen"
        self.sign()
        with self.assertRaisesRegex(ValueError, "product"):
            self.verify()
        self.data["product"] = "pine"
        self.data["package"]["sha256"] = "0" * 64
        self.sign()
        with self.assertRaisesRegex(ValueError, "package hash"):
            self.verify()

    def test_generated_virtual_identity_matches_native_scalar(self):
        data = peer_setup.generate()
        public = crypto_scalarmult_ed25519_base_noclamp(bytes.fromhex(data["private"])[:32])
        self.assertEqual(public.hex(), data["public"])

    def test_package_snapshot_is_verified_and_removed(self):
        args = SimpleNamespace(zip=self.package)
        def upload(data, selected):
            self.assertNotEqual(selected.zip, self.package)
            self.assertEqual(pine_update.sha(selected.zip), data["package"]["sha256"])
            self.assertEqual(selected.zip.stat().st_mode & 0o777, 0o400)
        with patch.object(pine_update, "flash_snapshot", side_effect=upload):
            pine_update.flash(self.data, args)
        self.assertEqual(args.zip, self.package)
        self.assertFalse((HERE / ".build" / f"pine-update-{os.getpid()}.zip").exists())
        self.package.write_bytes(b"changed")
        with patch.object(pine_update, "flash_snapshot") as uploader:
            with self.assertRaisesRegex(ValueError, "changed"):
                pine_update.flash(self.data, args)
            uploader.assert_not_called()

    def test_wrong_bootloader_stops_before_handover(self):
        import hardware
        args = SimpleNamespace(board=self.board, port=self.directory / "device",
                               current_sha="0" * 64)
        def query(link, command, prefix):
            if command == "get application":
                return "application sha256=" + args.current_sha
            if command == "get update":
                return "BLE-DFU=1 boot=0x000700 boot-timeout=none"
            raise AssertionError(command)
        with patch.object(hardware, "connection", return_value=MagicMock()), \
             patch.object(hardware, "bootloader") as handover, \
             patch.object(pine_update, "read_command", side_effect=query), \
             patch.object(pine_update.time, "sleep"):
            with self.assertRaisesRegex(ValueError, "bootloader"):
                pine_update.flash_snapshot(self.data, args)
            handover.assert_not_called()
