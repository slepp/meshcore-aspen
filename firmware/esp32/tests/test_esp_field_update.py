# SPDX-License-Identifier: Apache-2.0
import struct
import unittest
import shutil
import uuid
import hashlib
import sys
import os
import shlex
import subprocess
import tempfile
from pathlib import Path
from unittest.mock import patch

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from esp_update_manifest import canonical, sign
from prepare import stage_field_network

class ESPFieldUpdate(unittest.TestCase):
    def test_shared_storage_failure_never_formats_or_replaces_identity(self):
        root = Path(__file__).resolve().parents[3]
        source = (root / "firmware/esp32/wifi_kiss_main.cpp").read_text()
        function = source[source.index("void loadOrCreateIdentity() {"):source.index("\nvoid onSetRadio(")]
        fixture = r'''
#include <cassert>
#define MESHCORE_ONCHIP 1
struct Halted {};
void halt() { throw Halted{}; }
struct { void println(const char *) {} } Serial;
struct {
  bool available = true, formatted = false;
  bool begin(bool format) { formatted |= format; return available; }
} SPIFFS;
int identity = 0;
namespace onchip {
bool valid = true;
bool loadIdentity(const char *, int &key) { if (!valid) return false; key = 42; return true; }
}
''' + function + r'''
int main() {
  loadOrCreateIdentity();
  assert(identity == 42 && !SPIFFS.formatted);
  for (bool mounted : {false, true}) {
    SPIFFS.available = mounted; onchip::valid = false;
    bool halted = false;
    try { loadOrCreateIdentity(); } catch (Halted) { halted = true; }
    assert(halted && identity == 42 && !SPIFFS.formatted);
  }
}
'''
        with tempfile.TemporaryDirectory(dir=root / ".tmp") as directory:
            cpp, binary = Path(directory) / "storage.cpp", Path(directory) / "storage"
            cpp.write_text("#include <initializer_list>\n" + fixture)
            subprocess.run([*shlex.split(os.environ.get("CXX", "c++")), "-std=c++17", "-Wall", "-Wextra",
                            "-Werror", str(cpp), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


    def test_signature_contract(self):
        image = bytearray(24)
        image[0], image[1], image[12], image[23] = 0xe9, 1, 9, 1
        marker = b"meshcore-esp-target-v1:xiao-esp32s3:"
        image += struct.pack("<II", 0x3c020000, len(marker)) + marker
        checksum = 0xef
        for value in marker:
            checksum ^= value
        image += bytes(((len(image) + 16) & ~15) - len(image) - 1) + bytes([checksum])
        image += hashlib.sha256(image).digest()
        manifest = sign(image, "xiao-esp32s3", bytes(32))
        Ed25519PrivateKey.from_private_bytes(bytes(32)).public_key().verify(
            bytes.fromhex(manifest["signature"]),
            canonical(manifest["target"], manifest["size"], manifest["sha256"]))
        self.assertEqual(canonical("xiao-esp32s3", 48, manifest["sha256"]),
                         f'meshcore-esp-update-v1\nesp32s3\nxiao-esp32s3\n48\n{manifest["sha256"]}\n'.encode())
        with self.assertRaises(ValueError):
            sign(image, "another-board", bytes(32))
        with self.assertRaises(ValueError):
            sign(image + b"extra", "xiao-esp32s3", bytes(32))
        corrupt = bytearray(image)
        corrupt[-1] ^= 1
        with self.assertRaises(ValueError):
            sign(corrupt, "xiao-esp32s3", bytes(32))
        image[12] = 0
        with self.assertRaises(ValueError):
            sign(image, "xiao-esp32s3", bytes(32))

    def test_target_validation(self):
        for target in ("bad\nname", "", "a" * 64, "BAD"):
            with self.assertRaises(ValueError):
                canonical(target, 48, "0" * 64)


    def test_field_network_refresh_retains_clock_and_role_wrappers(self):
        root = Path(__file__).resolve().parents[3]
        build = root / ".tmp" / ("onchip-esp-network-test-" + uuid.uuid4().hex)
        native = build / "examples/kiss_modem/onchip"
        native.mkdir(parents=True)
        try:
            clock = native / "BuildClock.h"
            wrapper = native / "Companion.cpp"
            clock.write_text("retained clock\n")
            wrapper.write_text("retained role wrapper\n")
            with patch.object(sys, "path", [str(root / "firmware/shared"), *sys.path]):
                with patch("gps_time.stage_gps_time") as stage_gps:
                    stage_field_network(build)
            stage_gps.assert_called_once_with(build)
            self.assertEqual(clock.read_text(), "retained clock\n")
            self.assertEqual(wrapper.read_text(), "retained role wrapper\n")
        finally:
            shutil.rmtree(build)
