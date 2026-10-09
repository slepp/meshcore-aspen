# SPDX-License-Identifier: Apache-2.0
import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ONCHIP = Path(__file__).resolve().parents[1]
ROOT = ONCHIP.parents[1]
spec = importlib.util.spec_from_file_location("onchip_prepare", ONCHIP / "prepare.py")
prepare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prepare)


class BuildIsolation(unittest.TestCase):
    def test_native_owner_reply_bounds_refresh_without_duplicate_hooks(self):
        original = "start\n      if (reply_len == 0) return; // invalid command\nend\n"
        bounded = prepare.bound_owner_info_replies(original)
        self.assertEqual(prepare.bound_owner_info_replies(bounded), bounded)
        changed = bounded.replace("MAX_PACKET_PAYLOAD - 2 - CIPHER_BLOCK_SIZE - 5",
                                  "MAX_PACKET_PAYLOAD - 2 - CIPHER_BLOCK_SIZE - 4")
        self.assertEqual(prepare.bound_owner_info_replies(changed), bounded)
        self.assertEqual(bounded.count("const size_t owner_info_capacity"), 1)

    def test_contact_capacity_fits_wire_byte(self):
        for contacts, valid in ((0, False), (-2, False), (64, True), (255, False),
                                (256, True), (510, True), (512, False)):
            with self.subTest(contacts=contacts):
                result = subprocess.run(
                    [os.environ.get("CXX", "c++"), "-std=c++17", "-fsyntax-only",
                     "-x", "c++", "-", "-I", str(ONCHIP), "-DMESHCORE_ONCHIP=1",
                     f"-DMAX_CONTACTS={contacts}"], input='#include "Config.h"\n',
                    capture_output=True, text=True, check=False,
                )
                self.assertEqual(result.returncode == 0, valid, result.stderr)
                if not valid:
                    self.assertIn("half-count byte", result.stderr)

    def test_only_dedicated_build_tree_is_accepted(self):
        upstream = ROOT / ".tmp/parity-upstream"
        prepare.check_target(upstream, ROOT / ".tmp/onchip-MeshCore")
        for target in (ROOT, ROOT / ".tmp", upstream, ROOT / ".tmp/MeshCore",
                       ROOT / ".tmp/phyless-MeshCore", ROOT / ".tmp/onchip-upstream"):
            with self.subTest(target=target), self.assertRaises(ValueError):
                prepare.check_target(upstream, target)

    def test_symlink_cannot_alias_production(self):
        with tempfile.TemporaryDirectory(prefix="onchip-guard-", dir=ROOT / ".tmp") as directory:
            link = Path(directory) / "onchip-link"
            link.symlink_to(ROOT / ".tmp/MeshCore", target_is_directory=True)
            with self.assertRaises(ValueError):
                prepare.check_target(ROOT / ".tmp/parity-upstream", link)

    def test_make_rejects_production_before_extracting(self):
        result = subprocess.run(
            ["make", "-C", str(ONCHIP), "prepare", f"BUILD={ROOT / '.tmp/MeshCore'}"],
            capture_output=True, text=True, check=False,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("dedicated .tmp/onchip-*", result.stderr)
        self.assertNotIn("archive HEAD", result.stdout)

    def test_combined_capacity_uses_stock_socket_limit(self):
        for kiss, companion, sockets, https, valid in (
                (4, 2, 16, 0, True), (8, 2, 16, 0, False),
                (4, 4, 16, 0, False), (4, 2, 15, 0, False),
                (3, 2, 16, 1, True), (4, 2, 16, 1, False),
                (3, 2, 15, 1, False), (3, 2, 16, 0, False)):
            with self.subTest(kiss=kiss, companion=companion, sockets=sockets, https=https):
                result = subprocess.run(
                    [os.environ.get("CXX", "c++"), "-std=c++17", "-fsyntax-only",
                     "-x", "c++", "-", "-I", str(ONCHIP), "-I", str(ROOT / "firmware/shared"),
                     "-I", str(ROOT / "firmware/runtime"),
                     f"-DKISS_MAX_TCP_CLIENTS={kiss}",
                     f"-DONCHIP_COMPANION_MAX_CLIENTS={companion}",
                     f"-DONCHIP_BOT_HTTPS={https}",
                     f"-DCONFIG_LWIP_MAX_SOCKETS={sockets}"],
                    input='#include "Capacity.h"\n',
                    capture_output=True, text=True, check=False,
                )
                if valid:
                    self.assertEqual(result.returncode, 0, result.stderr)
                else:
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("static assertion failed", result.stderr)

    def test_native_service_socket_reservations_include_room_and_preserve_sdk_limit(self):
        for kiss, reserved, room, room_sockets, https, valid in (
                (3, 0, 0, 1, 1, True),
                (2, 1, 0, 1, 1, True),
                (1, 2, 0, 1, 1, True),
                (2, 1, 1, 1, 1, True),
                (1, 2, 1, 1, 1, True),
                (1, 2, 1, 2, 1, True),
                (3, 0, 1, 1, 1, False),
                (2, 1, 1, 2, 1, False),
                (0, 3, 0, 1, 1, False),
                (3, 1, 0, 1, 1, False),
                (3, 1, 0, 1, 0, False)):
            with self.subTest(kiss=kiss, reserved=reserved, room=room,
                              room_sockets=room_sockets, https=https):
                result = subprocess.run(
                    [os.environ.get("CXX", "c++"), "-std=c++17", "-fsyntax-only",
                     "-x", "c++", "-", "-I", str(ONCHIP),
                     "-I", str(ROOT / "firmware/shared"), "-I", str(ROOT / "firmware/runtime"),
                     f"-DKISS_MAX_TCP_CLIENTS={kiss}", "-DONCHIP_COMPANION_MAX_CLIENTS=2",
                     f"-DONCHIP_BOT_HTTPS={https}", "-DCONFIG_LWIP_MAX_SOCKETS=16",
                     f"-DMESHCORE_CLOUD_ROOM={room}",
                     f"-DONCHIP_CLOUD_ROOM_CONNECTIONS={room_sockets}",
                     f"-DONCHIP_NATIVE_SERVICE_SOCKETS={reserved}"],
                    input='#include "Capacity.h"\n', capture_output=True, text=True,
                )
                self.assertEqual(result.returncode == 0, valid, result.stderr)
                if not valid:
                    self.assertIn("static assertion failed", result.stderr)

    def test_phy_requires_explicit_matching_native_defaults(self):
        profile = {
            "ONCHIP_RADIO_FREQ_MHZ": "912.525", "ONCHIP_RADIO_BW_KHZ": "250.0",
            "ONCHIP_RADIO_SF": "7", "ONCHIP_RADIO_CR": "5",
            "ONCHIP_RADIO_TX_POWER": "2",
            "LORA_FREQ": "ONCHIP_RADIO_FREQ_MHZ",
            "LORA_BW": "ONCHIP_RADIO_BW_KHZ",
            "LORA_SF": "ONCHIP_RADIO_SF", "LORA_CR": "ONCHIP_RADIO_CR",
            "LORA_TX_POWER": "ONCHIP_RADIO_TX_POWER",
        }
        for change, valid in (({}, True), ({"ONCHIP_RADIO_CR": None}, False),
                              ({"LORA_FREQ": "868.0"}, False),
                              ({"ONCHIP_RADIO_SF": "13"}, False),
                              ({"ONCHIP_RADIO_TX_POWER": "30"}, False)):
            with self.subTest(change=change):
                flags = profile | change
                result = subprocess.run(
                    [os.environ.get("CXX", "c++"), "-std=c++17", "-fsyntax-only",
                     "-x", "c++", "-", "-I", str(ONCHIP)] +
                    [f"-D{key}={value}" for key, value in flags.items() if value is not None],
                    input='#include "PhyConfig.h"\n', capture_output=True,
                    text=True, check=False,
                )
                self.assertEqual(result.returncode == 0, valid, result.stderr)

    def test_prepare_preserves_private_config_and_stages_blank_filesystem(self):
        with tempfile.TemporaryDirectory(prefix="onchip-private-", dir=ROOT / ".tmp") as directory:
            build = Path(directory)
            build.chmod(0o755)
            config = build / "platformio.local.ini"
            config.write_text("private-test-profile\n")
            config.chmod(0o600)
            result = subprocess.run(
                ["make", "-C", str(ONCHIP), "prepare", f"BUILD={build}",
                 f"UPSTREAM={ROOT / '.tmp/onchip-upstream'}", "CONFIG="],
                capture_output=True, text=True, check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(config.read_text(), "private-test-profile\n")
            self.assertEqual(config.stat().st_mode & 0o777, 0o600)
            self.assertEqual(build.stat().st_mode & 0o777, 0o700)
            self.assertEqual((build / "onchip-data/onchip-layout").read_text(),
                             "meshcore-onchip-fs-v1\n")
            revision = subprocess.check_output(
                ["git", "-C", str(ROOT / ".tmp/onchip-upstream"), "rev-parse", "HEAD"],
                text=True).strip()[:12]
            self.assertIn(f'#define ONCHIP_NATIVE_REVISION "{revision}"',
                          (build / "examples/kiss_modem/onchip/BuildClock.h").read_text())
            native = build / "examples/kiss_modem/onchip"
            self.assertEqual((native / "NativeServices.h").read_bytes(),
                             (ROOT / "firmware/runtime/NativeServices.h").read_bytes())
            for unit in ("BotVm", "BotWorker", "CommandBot", "MastSource"):
                self.assertEqual(
                    (native / f"{unit}.cpp").read_bytes(),
                    (ROOT / f"firmware/runtime/{unit}.cpp").read_bytes(),
                )
            for unit in ("MastAdmin", "Management"):
                for suffix in ("h", "cpp"):
                    self.assertEqual(
                        (native / f"{unit}.{suffix}").read_bytes(),
                        (ONCHIP / f"{unit}.{suffix}").read_bytes(),
                    )
            for role in ("repeater", "room", "companion"):
                text = (native / role / f"Onchip{role.title()}.cpp").read_text()
                self.assertIn("getTotalAirTimeSeconds()", text)
                self.assertIn("ONCHIP_FIRMWARE_VERSION", text)
                self.assertIn("::onchipAdvertise(bool zeroHop)", text)
                self.assertIn("if (zeroHop) sendZeroHop(packet);", text)
                self.assertIn("sendFloodScoped(default_scope, packet, 0", text)
                wrapper = (native / f"{role.title()}.cpp").read_text()
                self.assertIn(f"bool {role}Advertise(bool zeroHop)", wrapper)
                self.assertIn("meshInstance->onchipAdvertise(zeroHop)", wrapper)
                self.assertNotIn("FIRMWARE_VERSION,", text.replace("ONCHIP_FIRMWARE_VERSION,", ""))
                if role == "repeater":
                    self.assertIn("const size_t owner_info_capacity", text)
                if role != "companion":
                    self.assertIn("tryGetTotalAirTime(tx_air_time_ms)", text)
                    self.assertIn("getReceiveAirTime(), tx_air_time_valid)", text)
                    for timer in ("next_local_advert", "next_flood_advert"):
                        self.assertIn(f"if (onchip::automaticAdvertsEnabled() && {timer}", text)
                    wrapper = (native / f"{role.title()}.cpp").read_text()
                    self.assertIn("if (automaticAdvertsEnabled()) mesh.sendSelfAdvertisement(16000, false);", wrapper)
            obsolete = native / "OnchipStatsFormatHelper.h"
            obsolete.write_text("stale generated header\n")
            prepare.generate(ROOT / ".tmp/onchip-upstream", build)
            self.assertFalse(obsolete.exists())

    def test_provisioning_requires_explicit_erasure_confirmation(self):
        with tempfile.TemporaryDirectory(prefix="onchip-provision-", dir=ROOT / ".tmp") as directory:
            marker = Path(directory) / "pio-called"
            pio = Path(directory) / "pio"
            pio.write_text(f"#!/bin/sh\ntouch '{marker}'\nexit 99\n")
            pio.chmod(0o700)
            result = subprocess.run(
                ["make", "-C", str(ONCHIP), "provision-fs",
                 "CONFIRM_FS_ERASE=", "UPLOAD_PORT=", f"BUILD={directory}"],
                env=os.environ | {"PATH": f"{directory}:{os.environ['PATH']}"},
                capture_output=True, text=True, check=False,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Provisioning erases all SPIFFS role data", result.stderr)
            self.assertFalse(marker.exists())


if __name__ == "__main__":
    unittest.main()
