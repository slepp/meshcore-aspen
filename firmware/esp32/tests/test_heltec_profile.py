import configparser
from pathlib import Path
import unittest

HERE = Path(__file__).resolve().parents[1]


class HeltecProfileTests(unittest.TestCase):
    def test_modem_inherits_radio_without_psram_or_native_roles(self):
        config = configparser.ConfigParser(interpolation=None)
        config.read(HERE / "platformio.heltec-modem.ini")
        self.assertEqual(config.sections(), ["env:Heltec_v3_kiss_wifi_build"])
        profile = config["env:Heltec_v3_kiss_wifi_build"]
        self.assertEqual(profile["extends"], "Heltec_lora32_v3")
        self.assertEqual(profile["board_upload.flash_size"], "8MB")
        self.assertEqual(profile["board_build.partitions"], "default_8MB.csv")
        self.assertEqual(int(profile["board_upload.maximum_size"]), 0x330000)
        flags = profile["build_flags"]
        self.assertIn("${Heltec_lora32_v3.build_flags}", flags)
        self.assertIn('-D WIFI_SSID=\'""\'', flags)
        self.assertIn('-D WIFI_PWD=\'""\'', flags)
        self.assertIn("-D KISS_MAX_TCP_CLIENTS=4", flags)
        for override in ("MESHCORE_ONCHIP", "BOARD_HAS_PSRAM", "P_LORA_", "SX126X_"):
            self.assertNotIn(override, flags)
        self.assertEqual(profile["build_src_filter"].splitlines(), [
            "${Heltec_lora32_v3.build_src_filter}", "+<../examples/kiss_modem/>"])
