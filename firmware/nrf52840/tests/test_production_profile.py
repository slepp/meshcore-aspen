# SPDX-License-Identifier: Apache-2.0
import configparser
from pathlib import Path
import re
import unittest


class ProductionProfileTest(unittest.TestCase):
    def test_room_budgets_and_sources_in_production_profiles(self):
        config = configparser.ConfigParser(interpolation=None)
        config.read(Path(__file__).resolve().parents[1] / "platformio.ini")
        for name in ("env:nrfmast_fleet_lua", "env:nrfmast_solar_lua"):
            for flag in ("PINE_ROOM_CLIENTS=4", "PINE_ROOM_POSTS=8",
                         "PINE_ROOM_REGIONS=8", "PINE_ROOM_PACKETS=6"):
                self.assertIn("-D " + flag, config[name]["build_flags"])
            self.assertIn("+<../examples/nrfmast/room/*.cpp>", config[name]["build_src_filter"])

    def test_lua_companion_restore_waits_for_selected_source(self):
        main = (Path(__file__).resolve().parents[1] / "main.cpp").read_text()
        setup = main.split("void setup() {", 1)[1].split("void loop() {", 1)[0]
        lua_setup = setup.split("} else if (!packetSelected && !nrfmast::roomSelected()", 1)[1]
        self.assertNotIn("companionStore.begin", lua_setup)
        loop = main.split("void loop() {", 1)[1]
        restore = loop.index("companionStore.begin")
        self.assertLess(loop.index("commandBotService().sourceReady()"), restore)
        self.assertIn("companionRestoreDone || !companionRestoreDeadline", loop[restore:])
        self.assertIn("millis() + 5000", setup)

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

    def test_rak_native_profiles_preserve_board_layout_and_select_ethernet(self):
        config = configparser.ConfigParser(interpolation=None)
        config.read(Path(__file__).resolve().parents[1] / "platformio.ini")
        native = config["env:nrfmast_rak4631_rx"]
        self.assertEqual(native["extends"], "rak4631")
        self.assertIn("${rak4631.build_flags}", native["build_flags"])
        self.assertIn("NRFMAST_QSPI_NOTES=0", native["build_flags"])
        self.assertIn("PINE_APPLICATION_START=0x26000", native["build_flags"])
        self.assertNotIn("NRFMAST_PRODUCTION_LUA=1", native["build_flags"])
        ethernet = config["env:nrfmast_rak4631_rak13800"]
        self.assertIn("ETHERNET_TCP_PORT=5000", ethernet["build_flags"])
        self.assertIn("RAK13800EthernetInterface.cpp", ethernet["build_src_filter"])
        self.assertNotIn("EthernetCLI", ethernet["build_src_filter"])


    def test_sscanf_formats_stay_within_the_integer_scanf_engine(self):
        # PineLibc.cpp routes sscanf to newlib's integer-only engine: no %e, %f or %g, and no wide %ls or %lc.
        root = Path(__file__).resolve().parents[3]
        sources = list((root / "firmware/nrf52840").glob("*.cpp")) + list((root / "firmware/runtime").glob("*.cpp"))
        formats = []
        for source in sources:
            formats += re.findall(r'\bsscanf\s*\([^;"]*"((?:[^"\\]|\\.)*)"', source.read_text())
        self.assertGreater(len(formats), 10)
        for text in formats:
            self.assertIsNone(re.search(r"%[*0-9]*[lL]?[aAeEfFgG]", text), text)
            self.assertIsNone(re.search(r"%[*0-9]*l[sc\[]", text), text)
