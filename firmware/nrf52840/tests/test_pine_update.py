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

    def retained(self, slot=0, generation=7, digest="a" * 64):
        responses = {command: prefix + "saved" for command, prefix in pine_update.QUERIES.items()}
        responses["source status"] = f"gen={generation} active={slot} prev=3 size=4096 upload=none next=0/0; source ready"
        responses["source hash"] = f"SHA256 {digest} gen={generation}"
        return responses

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
        with patch.object(hardware, "connection", return_value=MagicMock()) as connect, \
             patch.object(hardware, "bootloader") as handover, \
             patch.object(pine_update, "read_command", side_effect=query), \
             patch.object(pine_update.time, "sleep"):
            with self.assertRaisesRegex(ValueError, "bootloader"):
                pine_update.flash_snapshot(self.data, args)
            handover.assert_not_called()
            connect.assert_called_once_with(str(args.port))

    def test_upload_timeout_is_uncertain_and_not_retried(self):
        import hardware
        args = SimpleNamespace(board=self.board, port=self.directory / "device",
                               current_sha="0" * 64, zip=self.package,
                               record=self.directory / "update.json")
        def query(link, command, prefix):
            if command == "get application":
                return "application sha256=" + args.current_sha
            if command == "get update":
                return "BLE-DFU=1 boot=0x000601 boot-timeout=none"
            return self.retained()[command]
        endpoint = SimpleNamespace(vid=0x2886, serial_number="PINE", pid=0x44,
                                   device="selected-bootloader")
        with patch.object(hardware, "connection", return_value=MagicMock()), \
             patch.object(hardware, "bootloader"), \
             patch.object(hardware, "usb_serial", return_value="PINE"), \
             patch.object(hardware, "require_bootloader"), \
             patch("serial.tools.list_ports.comports", return_value=[endpoint]), \
             patch.object(pine_update, "read_command", side_effect=query), \
             patch.object(pine_update.time, "sleep"), \
             patch.object(pine_update.subprocess, "run",
                          side_effect=subprocess.TimeoutExpired("uploader", 240)) as upload:
            with self.assertRaisesRegex(RuntimeError, "completion uncertain"):
                pine_update.flash_snapshot(self.data, args)
            self.assertEqual(upload.call_count, 1)
        record = json.loads(args.record.read_text())
        self.assertEqual(record["manifest"], self.data)
        self.assertEqual(record["usb_serial"], "PINE")
        self.assertEqual(record["previous_sha256"], args.current_sha)
        self.assertEqual(args.record.stat().st_mode & 0o777, 0o600)

    def recovery(self):
        record = {"format": "pine-application-update-v1", "usb_serial": "PINE",
                  "application_port": str(self.directory / "application"),
                  "previous_sha256": "0" * 64, "bootloader": "0.6.1",
                  "manifest": self.data,
                  "retained": self.retained()}
        args = SimpleNamespace(board=self.board, port=self.directory / "bootloader",
                               record=self.directory / "update.json", zip=self.package,
                               operation="recover")
        args.record.write_text(json.dumps(record))
        args.record.chmod(0o600)
        return args, record

    def test_check_record_completes_readbacks_without_reset_or_upload(self):
        import hardware
        args, record = self.recovery()
        original = args.record.read_bytes()
        with patch.object(hardware, "usb_serial", return_value="PINE"), \
                patch.object(hardware, "bootloader") as handover, \
                patch.object(pine_update, "upload_snapshot") as upload, \
                patch.object(pine_update, "check_application") as check:
            pine_update.check_record(self.data, args)
            check.assert_called_once_with(self.data, args.port, record["retained"])
            upload.assert_not_called()
            handover.assert_not_called()
        self.assertEqual(args.record.read_bytes(), original)

    def test_check_record_rejects_another_image_before_device_access(self):
        import hardware
        args, record = self.recovery()
        record["manifest"]["application"]["sha256"] = "f" * 64
        args.record.write_text(json.dumps(record))
        with patch.object(hardware, "usb_serial", return_value="PINE"), \
                patch.object(pine_update, "check_application") as check:
            with self.assertRaisesRegex(ValueError, "record differs"):
                pine_update.check_record(self.verify(), args)
            check.assert_not_called()

    def test_existing_record_stops_before_handover(self):
        import hardware
        args, _ = self.recovery()
        args.current_sha = "0" * 64
        original = args.record.read_bytes()
        def query(link, command, prefix):
            if command == "get application":
                return "application sha256=" + args.current_sha
            if command == "get update":
                return "BLE-DFU=1 boot=0x000601 boot-timeout=none"
            return self.retained()[command]
        with patch.object(hardware, "connection", return_value=MagicMock()), \
                patch.object(hardware, "usb_serial", return_value="PINE"), \
                patch.object(hardware, "bootloader") as handover, \
                patch.object(pine_update, "read_command", side_effect=query), \
                patch.object(pine_update.time, "sleep"):
            with self.assertRaises(FileExistsError):
                pine_update.flash_snapshot(self.data, args)
            handover.assert_not_called()
        self.assertEqual(args.record.read_bytes(), original)

    def test_explicit_recovery_keeps_signed_snapshot_and_original_state(self):
        import hardware
        args, record = self.recovery()
        endpoint = SimpleNamespace(device="identified-bootloader")
        with patch.object(hardware, "device", return_value=endpoint), \
                patch.object(hardware, "require_bootloader") as selected, \
                patch.object(hardware, "usb_serial", return_value="PINE"), \
                patch.object(hardware, "bootloader") as handover, \
                patch.object(pine_update, "upload_snapshot") as upload, \
                patch.object(pine_update, "check_application") as check:
            pine_update.flash(self.data, args)
            selected.assert_called_once_with(str(args.port))
            handover.assert_not_called()
            upload.assert_called_once()
            self.assertNotEqual(upload.call_args.args[0], self.package)
            self.assertEqual(upload.call_args.args[1], endpoint.device)
            check.assert_called_once_with(self.data, Path(record["application_port"]), record["retained"])

    def test_recovery_refuses_other_image_device_record_and_running_app(self):
        import hardware
        args, record = self.recovery()
        for field, value in (("usb_serial", "OTHER"), ("bootloader", "0.7.0"),
                             ("manifest", dict(self.data, board="sensecap-solar")),
                             ("retained", {"ids": "ids saved"})):
            with self.subTest(field=field):
                args.record.write_text(json.dumps(dict(record, **{field: value})))
                with patch.object(hardware, "usb_serial", return_value="PINE"), \
                        patch.object(pine_update, "upload_snapshot") as upload:
                    with self.assertRaisesRegex(ValueError, "differs"):
                        pine_update.recover_snapshot(self.data, args)
                    upload.assert_not_called()
        args.record.write_text(json.dumps(record))
        with patch.object(hardware, "usb_serial", return_value="PINE"), \
                patch.object(hardware, "device"), \
                patch.object(hardware, "require_bootloader", side_effect=ValueError("Enter serial DFU")), \
                patch.object(pine_update, "upload_snapshot") as upload:
            with self.assertRaisesRegex(ValueError, "serial DFU"):
                pine_update.recover_snapshot(self.data, args)
            upload.assert_not_called()
        args.record.chmod(0o644)
        with patch.object(pine_update, "upload_snapshot") as upload:
            with self.assertRaisesRegex(ValueError, "owner-only"):
                pine_update.recover_snapshot(self.data, args)
            upload.assert_not_called()
        args.record.chmod(0o600)
        args.record.with_suffix(".linked").hardlink_to(args.record)
        with patch.object(pine_update, "upload_snapshot") as upload:
            with self.assertRaisesRegex(ValueError, "one link"):
                pine_update.recover_snapshot(self.data, args)
            upload.assert_not_called()

    def test_post_update_and_recovery_check_running_hash_and_retained_state(self):
        import hardware
        args, record = self.recovery()
        port = Path(record["application_port"])
        port.touch()
        running = f'application start=0x27000 bytes={self.data["application"]["bytes"]} sha256={self.data["application"]["sha256"]}'
        responses = dict(record["retained"], **{"ver": "vpine-0.1.1 date", "get application": running})
        with patch.object(hardware, "connection", return_value=MagicMock()), \
                patch.object(pine_update, "read_command", side_effect=lambda _, command, prefix: responses[command]), \
                patch.object(pine_update.time, "sleep"):
            pine_update.check_application(self.data, port, record["retained"])
            responses["get application"] = running.replace("sha256=", "sha256=0")
            with self.assertRaisesRegex(RuntimeError, "running hash differs"):
                pine_update.check_application(self.data, port, record["retained"])
            responses["get application"] = running
            responses["ids"] = "ids replaced"
            with self.assertRaisesRegex(RuntimeError, "retained state differs: ids"):
                pine_update.check_application(self.data, port, record["retained"])

    def test_normal_and_recovery_preserve_custom_source_allow_new_bundled_handlers(self):
        import hardware
        running = f'application start=0x27000 bytes={self.data["application"]["bytes"]} sha256={self.data["application"]["sha256"]}'
        cases = ((3, 3, "b" * 64, 7, True), (0, 0, "b" * 64, 7, False),
                 (0, 0, "a" * 64, 7, True), (3, 0, "a" * 64, 7, False),
                 (0, 0, "a" * 64, 8, False))
        for index, (old_slot, new_slot, digest, generation, accepted) in enumerate(cases):
            with self.subTest(slot=old_slot, new_slot=new_slot, digest=digest[0], generation=generation):
                port = self.directory / f"application-{index}"
                port.touch()
                args = SimpleNamespace(board=self.board, port=port, current_sha="0" * 64,
                                       zip=self.package, record=self.directory / f"update-{index}.json")
                before = self.retained(slot=old_slot)
                after = self.retained(slot=new_slot, generation=generation, digest=digest)
                after["source status"] = after["source status"].replace("size=4096", "size=3999")
                booted = False
                def handover(_):
                    nonlocal booted
                    booted = True
                def query(link, command, prefix):
                    if command == "get application":
                        return running if booted else "application sha256=" + args.current_sha
                    if command == "get update":
                        return "BLE-DFU=1 boot=0x000601 boot-timeout=none"
                    if command == "ver":
                        return "vpine-0.1.1 date"
                    return (after if booted else before)[command]
                endpoint = SimpleNamespace(vid=0x2886, serial_number="PINE", pid=0x44,
                                           device="identified-bootloader")
                with patch.object(hardware, "connection", return_value=MagicMock()), \
                        patch.object(hardware, "usb_serial", return_value="PINE"), \
                        patch.object(hardware, "bootloader", side_effect=handover), \
                        patch.object(hardware, "device", return_value=endpoint), \
                        patch.object(hardware, "require_bootloader"), \
                        patch("serial.tools.list_ports.comports", return_value=[endpoint]), \
                        patch.object(pine_update, "upload_snapshot") as upload, \
                        patch.object(pine_update, "read_command", side_effect=query), \
                        patch.object(pine_update.time, "sleep"):
                    if accepted:
                        pine_update.flash_snapshot(self.data, args)
                    else:
                        with self.assertRaisesRegex(RuntimeError, "retained source selection/hash differs"):
                            pine_update.flash_snapshot(self.data, args)
                    self.assertEqual(json.loads(args.record.read_text())["retained"], before)
                    args.port = self.directory / "bootloader"
                    if accepted:
                        pine_update.recover_snapshot(self.data, args)
                    else:
                        with self.assertRaisesRegex(RuntimeError, "retained source selection/hash differs"):
                            pine_update.recover_snapshot(self.data, args)
                    self.assertEqual(upload.call_count, 2)
