import asyncio
from pathlib import Path
import tempfile
import types
import unittest
from unittest.mock import AsyncMock, MagicMock, patch

import ble_dfu

HERE = Path(__file__).resolve().parents[1]


class BleDfuTests(unittest.TestCase):
    def args(self, **changes):
        values = dict(zip=Path("firmware.zip"), adapter="hci1", app_address="AA:BB:CC:DD:EE:01",
                      boot_address="AA:BB:CC:DD:EE:02", boot_only=False, tool_dir=Path("unused"))
        values.update(changes)
        return types.SimpleNamespace(**values)

    def fixtures(self, revision=b"\x08\x00"):
        dfu = MagicMock(upload_mode=4)
        dfu.jump_to_bootloader = AsyncMock()
        dfu._setup_mtu = AsyncMock(return_value=23)
        dfu.client = AsyncMock()
        dfu.client.read_gatt_char.return_value = revision
        dfu.image_sent = False
        async def transfer(*args, **kwargs):
            await dfu._setup_mtu()
            dfu.image_sent = True
        dfu.perform_update = AsyncMock(side_effect=transfer)
        library = MagicMock()
        library.NordicLegacyDFU.return_value = dfu
        library.find_device_by_name_or_address = AsyncMock(
            side_effect=lambda address, **kw: types.SimpleNamespace(address=address))
        return library, dfu

    def test_exact_app_and_boot_addresses_no_uuid_selection(self):
        library, dfu = self.fixtures()
        with patch("ble_dfu.validate_package") as validate, \
                patch("ble_dfu.asyncio.sleep", new_callable=AsyncMock):
            asyncio.run(ble_dfu.update(self.args(), library))
        validate.assert_called_once()
        self.assertEqual(library.find_device_by_name_or_address.call_count, 2)
        for call in library.find_device_by_name_or_address.call_args_list:
            self.assertEqual(call.kwargs, dict(force_scan=True, adapter="hci1"))
        self.assertEqual(dfu.jump_to_bootloader.await_args.args[0].address, self.args().app_address)
        self.assertEqual(dfu.perform_update.await_args.args[0].address, self.args().boot_address)
        self.assertEqual(dfu.perform_update.await_args.kwargs, dict(max_retries=1))
        self.assertFalse(library.NordicLegacyDFU.call_args.kwargs["high_mtu"])
        self.assertTrue(dfu.image_sent)

    def test_retry_skips_application_handover(self):
        library, dfu = self.fixtures()
        with patch("ble_dfu.validate_package"):
            asyncio.run(ble_dfu.update(self.args(boot_only=True, app_address=None), library))
        dfu.jump_to_bootloader.assert_not_awaited()
        dfu.perform_update.assert_awaited_once()
        self.assertEqual(library.find_device_by_name_or_address.call_count, 1)

    def test_app_revision_or_other_address_never_receives_image(self):
        for revision, address in ((b"\x01\x00", None), (b"\x08\x00", "OTHER")):
            library, dfu = self.fixtures(revision)
            if address:
                library.find_device_by_name_or_address.return_value = types.SimpleNamespace(address=address)
                library.find_device_by_name_or_address.side_effect = None
            with patch("ble_dfu.validate_package"), self.assertRaises(ValueError):
                asyncio.run(ble_dfu.update(self.args(boot_only=True), library))
            self.assertFalse(dfu.image_sent)

    def test_destructive_package_and_non_app_library_mode_fail_before_jump(self):
        library, dfu = self.fixtures()
        with patch("ble_dfu.validate_package", side_effect=ValueError("unsafe package")), \
                self.assertRaises(ValueError):
            asyncio.run(ble_dfu.update(self.args(), library))
        library.NordicLegacyDFU.assert_not_called()
        with patch("ble_dfu.validate_package"), self.assertRaises(ValueError):
            dfu.upload_mode = 3
            asyncio.run(ble_dfu.update(self.args(), library))
        dfu.jump_to_bootloader.assert_not_awaited()
        dfu.perform_update.assert_not_awaited()

    def test_missing_or_modified_tool_is_not_imported(self):
        with tempfile.TemporaryDirectory(dir=HERE / ".build") as directory:
            with self.assertRaisesRegex(ValueError, "Missing"):
                ble_dfu.load_tool(directory)
            path = Path(directory) / "dfu_lib.py"
            path.write_text("raise AssertionError('must not execute')\n")
            with self.assertRaisesRegex(ValueError, "reviewed revision"):
                ble_dfu.load_tool(directory)
