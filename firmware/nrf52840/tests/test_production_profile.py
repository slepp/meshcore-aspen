# SPDX-License-Identifier: Apache-2.0
import configparser
from pathlib import Path
import unittest


class ProductionProfileTest(unittest.TestCase):
    def test_lua_uses_full_c_and_software_crypto_without_changing_native(self):
        config = configparser.ConfigParser(interpolation=None)
        config.read(Path(__file__).resolve().parents[1] / "platformio.ini")
        removed = config["env:nrfmast_fleet_lua"]["build_unflags"].splitlines()
        self.assertIn("--specs=nano.specs", removed)
        self.assertIn("-D USE_CC310_HW_CRYPTO=1", removed)
        for name in ("env:nrfmast_rx", "env:nrfmast"):
            self.assertNotIn("--specs=nano.specs", config[name]["build_unflags"])
            self.assertNotIn("USE_CC310_HW_CRYPTO", config[name]["build_unflags"])
