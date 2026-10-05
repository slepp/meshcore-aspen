# SPDX-License-Identifier: Apache-2.0
import json
from pathlib import Path
import subprocess
import unittest


ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / ".tmp/public-esp-birch-identity"
RELEASE = json.loads((ROOT / "release/products.json").read_text())


class FirmwareIdentityTests(unittest.TestCase):
    def test_linked_profile_and_dashboard_json(self):
        BUILD.mkdir(parents=True, exist_ok=True)
        profiles = (
            ("birch", [], "birch-" + RELEASE["products"]["birch"]["version"]),
            ("aspen", ["-DMESHCORE_ONCHIP=1"], "aspen-" + RELEASE["products"]["aspen"]["version"]),
            ("pine", ["-DMESHCORE_ONCHIP=1", "-DNRF52_PLATFORM=1"], "1.17.1-slp-pine"),
            ("onchip-override", ["-DMESHCORE_ONCHIP=1",
                                 '-DONCHIP_FIRMWARE_VERSION="operator-profile"'], "operator-profile"),
        )
        for name, flags, expected in profiles:
            with self.subTest(profile=name):
                binary = BUILD / name
                subprocess.run([
                    "g++", "-std=c++11", "-Wall", "-Wextra", "-Werror", *flags,
                    "-I", str(ROOT / "firmware/shared"), "-I", str(ROOT / "firmware/esp32"),
                    str(ROOT / "firmware/shared/RadioDashboard.cpp"),
                    str(ROOT / "test_support/phy_parity/firmware_identity.cpp"),
                    "-o", str(binary),
                ], check=True)
                constant, body = subprocess.check_output([str(binary)], text=True).splitlines()
                status = json.loads(body)
                self.assertEqual(constant, expected)
                self.assertEqual(status["firmware_version"], expected)
                self.assertEqual(status["upstream_tag"], RELEASE["upstream"]["tag"])
                self.assertEqual(status["upstream_commit"], RELEASE["upstream"]["commit"])
                self.assertEqual(status["api_version"], 1)
                self.assertEqual(status["device_name"], "operator-selected-name")

    def test_actual_http_status_handler(self):
        BUILD.mkdir(parents=True, exist_ok=True)
        for name, flags in (("birch", []),
                            ("aspen", ["-DMESHCORE_ONCHIP=1", "-DKISS_MAX_TCP_CLIENTS=4"])):
            with self.subTest(profile=name):
                binary = BUILD / (name + "-http")
                subprocess.run([
                    "g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    "-DARDUINO_ARCH_ESP32", *flags,
                    "-I", str(ROOT / "test_support/phy_parity/seams/http"),
                    "-I", str(ROOT / "test_support/phy_parity/seams"),
                    "-I", str(ROOT / "firmware/shared"), "-I", str(ROOT / "firmware/esp32"), "-Wl,--wrap=send",
                    str(ROOT / "test_support/phy_parity/dashboard_http.cpp"),
                    "-o", str(binary),
                ], check=True)
                subprocess.run([str(binary)], check=True, stdout=subprocess.PIPE)


if __name__ == "__main__":
    unittest.main()
