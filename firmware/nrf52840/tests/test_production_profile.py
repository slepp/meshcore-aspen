# SPDX-License-Identifier: Apache-2.0
import configparser
from pathlib import Path
import unittest


class ProductionProfileTest(unittest.TestCase):
    def test_companion_channels_only_in_production_profiles(self):
        config = configparser.ConfigParser(interpolation=None)
        config.read(Path(__file__).resolve().parents[1] / "platformio.ini")
        for name in ("env:nrfmast_fleet_lua", "env:nrfmast_solar_lua"):
            self.assertIn("-D MAX_GROUP_CHANNELS=4", config[name]["build_flags"])
            self.assertIn("densaugeo/base64 @ ~1.4.0", config[name]["lib_deps"])
        for name in ("env:nrfmast_rx", "env:nrfmast", "env:nrfmast_fleet"):
            self.assertNotIn("MAX_GROUP_CHANNELS", config[name]["build_flags"])

    def test_lua_uses_full_c_and_software_crypto_without_changing_native(self):
        config = configparser.ConfigParser(interpolation=None)
        config.read(Path(__file__).resolve().parents[1] / "platformio.ini")
        removed = config["env:nrfmast_fleet_lua"]["build_unflags"].splitlines()
        self.assertIn("--specs=nano.specs", removed)
        self.assertIn("-D USE_CC310_HW_CRYPTO=1", removed)
        for name in ("env:nrfmast_rx", "env:nrfmast"):
            self.assertNotIn("--specs=nano.specs", config[name]["build_unflags"])
            self.assertNotIn("USE_CC310_HW_CRYPTO", config[name]["build_unflags"])

    def test_solar_preserves_gps_sensors_and_board_wiring(self):
        config = configparser.ConfigParser(interpolation=None)
        config.read(Path(__file__).resolve().parents[1] / "platformio.ini")
        solar = config["env:nrfmast_solar_lua"]
        self.assertEqual(solar["extends"], "SenseCap_Solar")
        self.assertIn("${SenseCap_Solar.build_flags}", solar["build_flags"])
        self.assertIn("${SenseCap_Solar.build_src_filter}", solar["build_src_filter"])
        self.assertIn("${SenseCap_Solar.lib_deps}", solar["lib_deps"])
        self.assertNotIn("${sensor_base.build_flags}", solar["build_unflags"])
        self.assertIn("ENV_SKIP_GPS_DETECT=1", solar["build_flags"])
        self.assertIn("NRFMAST_PRODUCTION_LUA=1", solar["build_flags"])
