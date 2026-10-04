import pathlib
import json
import struct
import tempfile
import types
import unittest
from unittest.mock import MagicMock, patch

import hardware


import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
from test_support.operator_inventory import configure_inventory

class HardwareGuards(unittest.TestCase):
    def setUp(self):
        configure_inventory(self)

    def test_runtime_name_check_preserves_keys_and_shared_profile(self):
        identities = ("01" * 32, "02" * 32)
        before = {"get name": "> Pine-Relay", "get radio": "> 912.5250244,250,7,5",
                  "get tx": "> 2", "get repeat": "> on", "get path.hash.mode": "> 2"}
        after = dict(before)
        names = iter(("> Pine-Bot-Check", "> Pine-Bot"))
        replies = {
            "get capabilities": "key-import=USB-only; channel-keys=unsupported",
            "get pub.key": f"active={identities[0]} saved={identities[0]} reboot=0",
            "get bot.pub.key": f"active={identities[1]} saved={identities[1]} reboot=0",
        }

        def response(_, command, **kwargs):
            if command == "get bot.name":
                return next(names)
            return replies.get(command, "OK saved; name applies to next advert")

        with patch("hardware.snapshot", side_effect=[
                (before, {}, identities), (after, {}, identities)]), \
                patch("hardware.connection"), patch("hardware.save"), \
                patch("hardware.hardware_check") as reboot, \
                patch("hardware.runtime_rf_check") as rf, \
                patch("hardware.serial_command", side_effect=response) as command:
            hardware.runtime_check("test", None)
            reboot.assert_called_once_with("test", None, True)
            rf.assert_called_once_with("test", None, identities)
            self.assertNotIn("set prv.key", " ".join(call.args[1] for call in command.call_args_list))
        replies["get bot.pub.key"] = replies["get bot.pub.key"].replace("reboot=0", "reboot=1")
        names = iter(("> Pine-Bot-Check",))
        with patch("hardware.snapshot", return_value=(before, {}, identities)), \
                patch("hardware.connection"), patch("hardware.save"), \
                patch("hardware.hardware_check") as reboot, \
                patch("hardware.serial_command", side_effect=response) as command:
            with self.assertRaisesRegex(RuntimeError, "Pending or changed identity"):
                hardware.runtime_check("test", None)
            reboot.assert_not_called()
            self.assertFalse(any(call.args[1].startswith("set ") for call in command.call_args_list))

    def test_native_name_change_preserves_radio_and_both_keys(self):
        identities = ("01" * 32, "02" * 32)
        before = {"get name": "> old", "get radio": "> 912.5250244,250,7,5",
                  "get tx": "> 2", "get repeat": "> on", "get path.hash.mode": "> 2"}
        after = dict(before, **{"get name": "> Pine-Relay"})
        with patch("hardware.connection"), \
                patch("hardware.serial_command", return_value="OK\r\n") as command, \
                patch("hardware.snapshot", side_effect=[
                    (before, {}, identities), (after, {}, identities)]):
            hardware.fleet_name("test", None)
            self.assertEqual(command.call_args.args[1], "set name Pine-Relay")
        after["get tx"] = "> 3"
        with patch("hardware.connection"), patch("hardware.serial_command", return_value="OK"), \
                patch("hardware.snapshot", side_effect=[
                    (before, {}, identities), (after, {}, identities)]):
            with self.assertRaisesRegex(RuntimeError, "preserve"):
                hardware.fleet_name("test", None)

    def test_fleet_profile_preserves_keys_and_checks_lab_readback(self):
        identities = ("01" * 32, "02" * 32)
        ids = f"ids repeater={identities[0]} bot={identities[1]}"
        replies = {"get name": "> Pine-Relay", "get radio": "> 912.5250244,250,7,5",
                   "get path.hash.mode": "> 2", "ids": ids}
        with patch("hardware.snapshot", return_value=({"ids": ids + "\n"}, {}, identities)), \
             patch("hardware.connection"), patch("hardware.save"), \
             patch("hardware.serial_command", side_effect=lambda _, cmd, **kw: replies.get(cmd, "OK")):
            hardware.fleet_profile("test", None)
            for key, value in (("ids", ids.replace("01", "03")),
                               ("get radio", "> 910.5250244,62.5,7,5"),
                               ("get path.hash.mode", "> 0")):
                original = replies[key]
                replies[key] = value
                with self.assertRaisesRegex(RuntimeError, "readback mismatch"):
                    hardware.fleet_profile("test", None)
                replies[key] = original

    def test_snapshot_checks_expected_rf_mode(self):
        for rf_enabled in (False, True):
            with self.subTest(rf_enabled=rf_enabled):
                link = MagicMock()
                link.__enter__.return_value.read.return_value = b""
                replies = {
                    "mem": f"mem heap_total=100 heap_used=20 heap_free=80 sampled_low=80 "
                           f"stack_free=40 rf={int(rf_enabled)} tx={int(rf_enabled)}",
                    "ids": "ids repeater=" + "01" * 32 + " bot=" + "02" * 32,
                    "stats-packets": json.dumps(dict.fromkeys(
                        ("sent", "flood_tx", "direct_tx"), int(rf_enabled))),
                }
                with patch("hardware.connection", return_value=link), \
                     patch("hardware.time.sleep"), patch("hardware.save"), \
                     patch("hardware.serial_command", side_effect=lambda _, cmd: replies.get(cmd, "OK")):
                    hardware.snapshot("test", None, "record", rf_enabled)
                    with self.assertRaisesRegex(RuntimeError, "RF mode"):
                        hardware.snapshot("test", None, "record", not rf_enabled)
                    if not rf_enabled:
                        replies["stats-packets"] = '{"sent":1,"flood_tx":0,"direct_tx":0}'
                        with self.assertRaisesRegex(RuntimeError, "transmission"):
                            hardware.snapshot("test", None, "record")

    def test_stats_keep_expected_background_diagnostics(self):
        text = '{"sent":0}\r\nrepeater: transmit failed (RF is disabled in nrfmast_rx)\r\n'
        stats, diagnostics = hardware.packet_stats(text)
        self.assertEqual(stats["sent"], 0)
        self.assertEqual(len(diagnostics), 1)
        with self.assertRaisesRegex(RuntimeError, "Unexpected background"):
            hardware.packet_stats('{"sent":0}\nradio hardware failed\n')
        with self.assertRaisesRegex(RuntimeError, "exactly one"):
            hardware.packet_stats('{"sent":0}\n{"sent":1}\n')

    def test_explicit_xiao_device_only(self):
        xiao = types.SimpleNamespace(device="/dev/nrf-test", vid=0x2886, pid=0x8044,
                                     serial_number=hardware.usb_serial())
        other = types.SimpleNamespace(device="/dev/nrf-test", vid=0x303A, pid=0x1001)
        with patch("hardware.serial.tools.list_ports.comports", return_value=[xiao]):
            self.assertIs(hardware.device("/dev/nrf-test"), xiao)
            with self.assertRaisesRegex(ValueError, "Enter serial DFU"):
                hardware.require_bootloader("/dev/nrf-test")
        with patch("hardware.serial.tools.list_ports.comports", return_value=[other]):
            with self.assertRaisesRegex(ValueError, "XIAO"):
                hardware.device("/dev/nrf-test")

    def test_unrelated_nrf_devices_are_never_opened(self):
        for serial_number in ("1122334455667788", "8877665544332211", None):
            unrelated = types.SimpleNamespace(device="/dev/nrf-test", vid=0x2886, pid=0x8044,
                                               serial_number=serial_number)
            with patch("hardware.serial.tools.list_ports.comports", return_value=[unrelated]), \
                    patch("hardware.serial.Serial") as opened:
                with self.assertRaisesRegex(ValueError, "operator inventory; connection refused"):
                    hardware.connection("/dev/nrf-test")
                opened.assert_not_called()

    def test_private_records_stay_in_repository(self):
        with self.assertRaisesRegex(ValueError, "repository"):
            hardware.private_directory("/tmp/nrfmast-not-allowed")
        build = pathlib.Path(__file__).resolve().parents[1] / ".build"
        with tempfile.TemporaryDirectory(dir=build) as temporary:
            directory = hardware.private_directory(temporary)
            hardware.save(directory, "record.bin", b"generated-test-record")
            self.assertEqual(directory.stat().st_mode & 0o777, 0o700)
            self.assertEqual((directory / "record.bin").stat().st_mode & 0o777, 0o600)
            with self.assertRaises(FileExistsError):
                hardware.save(directory, "record.bin", b"replacement")

    def test_uf2_backup_records_actual_coverage(self):
        build = pathlib.Path(__file__).resolve().parents[1] / ".build"
        with tempfile.TemporaryDirectory(dir=build) as temporary:
            root = pathlib.Path(temporary)
            volume = root / "volume"
            volume.mkdir()
            (volume / "INFO_UF2.TXT").write_text("Board-ID: Seeed-XIAO-nRF52840")
            block = bytearray(512)
            struct.pack_into("<8I", block, 0, 0x0A324655, 0x9E5D5157, 0x2000,
                             0x27000, 256, 0, 1, 0xADA52840)
            struct.pack_into("<I", block, 508, 0x0AB16F30)
            (volume / "CURRENT.UF2").write_bytes(block)
            records = hardware.private_directory(root / "records")
            hardware.uf2_backup(volume, records)
            manifest = json.loads((records / "uf2-manifest.json").read_text())
            self.assertTrue(manifest["repeat_read_verified"])
            self.assertFalse(manifest["internalfs_included"])
            self.assertFalse(manifest["external_qspi_included"])
            self.assertEqual(manifest["flash_start"], "0x27000")
            self.assertEqual((records / "flash-base-00027000.bin").stat().st_size, 256)

    def test_usb_disconnect_requires_verified_dfu(self):
        application = types.SimpleNamespace(device="/dev/nrf-test", vid=0x2886, pid=0x8044, serial_number="generated-usb-id")
        bootloader = types.SimpleNamespace(device="/dev/nrf-dfu", vid=0x2886, pid=0x45, serial_number="generated-usb-id")
        with patch("hardware.device", return_value=application), \
             patch("hardware.connection", side_effect=OSError(hardware.errno.ESHUTDOWN, "USB reset")), \
             patch("hardware.serial.tools.list_ports.comports", return_value=[bootloader]):
            hardware.bootloader("/dev/nrf-test")
