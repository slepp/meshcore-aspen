import binascii
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import zipfile

import ble_field
import prepare

HERE = Path(__file__).resolve().parents[1]


class FieldUpdateTests(unittest.TestCase):
    def test_window_auth_deadline_and_retained_time_expiry(self):
        with tempfile.TemporaryDirectory(dir=HERE / ".build") as directory:
            executable = Path(directory) / "field-update"
            subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra",
                            str(HERE / "tests/field_update.cpp"), "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True)

    def test_trust_expiry_runs_without_lua_jobs_and_is_shared_with_companion(self):
        main = (HERE / "main.cpp").read_text().split("void loop() {", 1)[1]
        self.assertLess(main.index("nrfmast::pollLuaTime();"), main.index("if (luaStarted"))
        self.assertLess(main.index("nrfmast::pollLuaTime();"), main.index("fieldUpdatePending()"))
        platform = (HERE / "PineRuntimePlatform.cpp").read_text()
        self.assertIn("const bool fresh = trustedTime.fresh(now);", platform)
        self.assertIn("trustedTime.poll(millis());", platform)
        self.assertIn("trustedTime.poll(now);", platform.split("bool clockSnapshot(", 1)[1])
        self.assertNotIn("trustedAt", platform)
        self.assertIn("companionClockValue(current, utc, fresh)", platform)
        self.assertIn("trustedSource.store(source", platform)
        companion = (HERE / "CompanionInterface.cpp").read_text()
        self.assertIn("trustLuaTime(read32(input + 1), LuaTimeSource::BleCompanion)", companion)

    def test_staged_service_preserves_adafruit_handover_and_denies_before_it(self):
        with tempfile.TemporaryDirectory(dir=HERE / ".build") as directory:
            output = Path(directory)
            (output / "examples/nrfmast").mkdir(parents=True)
            prepare.stage_field_dfu(output)
            source = (output / "examples/nrfmast/PineDfuService.cpp").read_text()
            self.assertLess(source.index("!nrfmast::authorizeFieldDfu"),
                            source.index("reply.params.write.gatt_status = BLE_GATT_STATUS_SUCCESS"))
            for native in ("conn->loadBondKey(&bkeys)", "0x20007F80UL", "DFU_OTA_MAGIC = 0xB1",
                           "bootloader_util_app_start(NRF_UICR->NRFFW[0])"):
                self.assertIn(native, source)
            self.assertNotIn("BLEDfu::", source)

    def test_app_only_package_crc_bounds_and_destructive_images(self):
        with tempfile.TemporaryDirectory(dir=HERE / ".build") as directory:
            path = Path(directory) / "firmware.zip"
            binary = struct.pack("<II", 0x20040000, 0x27009) + bytes(16)
            crc = binascii.crc_hqx(binary, 0xFFFF)
            data = dict(device_type=0x52, device_revision=0xFFFF, application_version=0xFFFFFFFF,
                        softdevice_req=[0x123], firmware_crc16=crc)
            manifest = {"application": dict(bin_file="firmware.bin", dat_file="firmware.dat",
                                            init_packet_data=data), "dfu_version": 0.5}
            init = struct.pack("<HHIHHH", 0x52, 0xFFFF, 0xFFFFFFFF, 1, 0x123, crc)
            def write(image=binary, packet=init, extra=None):
                with zipfile.ZipFile(path, "w") as z:
                    z.writestr("manifest.json", json.dumps({"manifest": manifest}))
                    z.writestr("firmware.bin", image)
                    z.writestr("firmware.dat", packet)
                    if extra:
                        z.writestr(extra, b"")
            write()
            self.assertFalse(ble_field.validate_package(path)["signed"])
            for image in (b"", binary[:-1], binary + bytes(811008),
                          struct.pack("<II", 0x20040000, 0xF4001) + bytes(16)):
                write(image)
                with self.assertRaises(ValueError):
                    ble_field.validate_package(path)
            write(packet=init[:-1] + b"\x00")
            with self.assertRaises(ValueError):
                ble_field.validate_package(path)
            write(extra="bootloader.bin")
            with self.assertRaises(ValueError):
                ble_field.validate_package(path)
            manifest["softdevice_bootloader"] = {}
            write()
            with self.assertRaises(ValueError):
                ble_field.validate_package(path)

    def test_standard_companion_time_frame_and_valid_range(self):
        self.assertEqual(ble_field.time_frame(1800000000), struct.pack("<BI", 6, 1800000000))
        for invalid in (0, 1715770350, 4102444801):
            with self.assertRaises(ValueError):
                ble_field.time_frame(invalid)

    def test_upstream_unsafe_commands_are_fenced(self):
        source = (HERE / "main.cpp").read_text()
        self.assertLess(source.index("fieldUpdateCommand(senderTimestamp"),
                        source.index("MyMesh::handleCommand(senderTimestamp"))
        self.assertIn('!strncmp(body, "poweroff", 8)', source)
        self.assertIn('!strncmp(body, "shutdown", 8)', source)
        self.assertIn("existing record cannot be read", source)
        self.assertIn("InternalFS.Adafruit_LittleFS::begin()", source)
        self.assertIn("while (!radio_init())", source)
        self.assertIn("activeBlePin != runtimeConfig.ble.getPin()", source)
        self.assertIn('!strncmp(output, "OK - clock set:", 15)', source)


if __name__ == "__main__":
    unittest.main()
