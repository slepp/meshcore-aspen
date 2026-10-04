"""Check repository-owned firmware paths without touching radios or build configs."""
import configparser
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]


class SourceLayoutTests(unittest.TestCase):
    def test_remote_role_profiles_are_esp32_tcp_only(self):
        config = configparser.ConfigParser(interpolation=None)
        config.read(ROOT / "firmware/remote-radio/esp32/platformio.ini.example")
        self.assertEqual(set(config.sections()), {
            "Phyless_Xiao_S3", "env:Phyless_Xiao_S3_repeater",
            "env:Phyless_Xiao_S3_room", "env:Phyless_Xiao_S3_client",
        })
        self.assertIn("KISS_MODEM_HOST", config["Phyless_Xiao_S3"]["build_flags"])
        self.assertNotIn("KISS_UART_BAUD", config["Phyless_Xiao_S3"]["build_flags"])
        self.assertTrue((ROOT / "firmware/remote-radio/esp32/target.cpp").is_file())
        self.assertTrue((ROOT / "firmware/shared/RemoteKissRadio.cpp").is_file())
        self.assertTrue((ROOT / "firmware/remote-radio/esp32/Esp32TcpKissLink.h").is_file())
        self.assertFalse((ROOT / "firmware/shared/Esp32TcpKissLink.h").exists())

    def test_platform_stagers_use_same_portable_runtime_sources(self):
        esp = (ROOT / "firmware/esp32/prepare.py").read_text()
        pine = (ROOT / "firmware/nrf52840/prepare.py").read_text()
        self.assertIn('source.parent / "runtime"', esp)
        self.assertIn('HERE.parent / "runtime" / (unit + ".cpp")', pine)
        for unit in ("BotVm", "BotWorker", "CommandBot", "MastSource"):
            self.assertTrue((ROOT / f"firmware/runtime/{unit}.cpp").is_file())
            self.assertFalse((ROOT / f"firmware/esp32/{unit}.cpp").exists())

    def test_pine_runtime_destination_matches_build_consumers(self):
        stager = (ROOT / "firmware/nrf52840/prepare.py").read_text()
        self.assertIn('shared = dest / "onchip"', stager)
        self.assertNotIn('shared = dest / "esp32"', stager)
        config = configparser.ConfigParser(interpolation=None)
        config.read(ROOT / "firmware/nrf52840/platformio.ini")
        for profile in ("env:nrfmast_fleet_lua", "env:nrfmast_solar_lua"):
            self.assertIn("-I examples/nrfmast/onchip", config[profile]["build_flags"])
            self.assertIn("+<../examples/nrfmast/onchip/*.cpp>",
                          config[profile]["build_src_filter"])
        for name in ("main.cpp", "CommandBot.cpp"):
            self.assertIn('#include "onchip/CommandBot.h"',
                          (ROOT / "firmware/nrf52840" / name).read_text())

    def test_operator_aliases_retain_established_commands(self):
        makefile = (ROOT / "Makefile").read_text()
        for alias, original in (
                ("aspen-config", "onchip-config"), ("aspen-firmware", "onchip-firmware"),
                ("aspen-upload", "onchip-upload"), ("aspen-test", "onchip-test"),
                ("remote-radio-config", "phyless-config"),
                ("remote-radio-firmware", "phyless-firmware"),
                ("remote-radio-upload", "phyless-upload")):
            self.assertIn(f"\n{alias}: {original}\n", makefile)
        for config in ("platformio.local.ini", "platformio.onchip.ini",
                       "platformio.phyless.ini", "platformio.nrf52.ini"):
            self.assertIn(f"firmware/{config}", makefile)

    def test_moved_hardware_cli_resolves_monitor_import(self):
        result = subprocess.run(
            ["python3", str(ROOT / "test_support/hardware/live_multiclient.py"), "--help"],
            cwd=ROOT, capture_output=True, text=True, check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("--receive-only", result.stdout)

    def test_hardware_tools_and_tests_are_separate_from_firmware(self):
        for name in (
                "esp32_device", "companion_device", "mast_checks", "esp32_update",
                "radio_checks", "wifi_checks", "https_checks", "identity_install",
                "monitor", "companion_checks", "management_rf_checks",
                "companion_cli", "role_checks", "bot_checks", "lua_checks",
                "rf", "admin", "esp32_slot", "inventory"):
            self.assertTrue((ROOT / f"tools/hardware/{name}.py").is_file())
        self.assertTrue((ROOT / "firmware/esp32/https_profile.py").is_file())
        self.assertFalse((ROOT / "firmware/esp32/mast_cli.py").exists())
        self.assertFalse((ROOT / "firmware/shared/operator_inventory.py").exists())
        makefile = (ROOT / "firmware/esp32/Makefile").read_text()
        self.assertIn("discover -s tests -p 'test_*.py'", makefile)
        self.assertIn("discover -s $(ROOT)/tools/hardware/tests -p 'test_*.py'", makefile)


if __name__ == "__main__":
    unittest.main()
