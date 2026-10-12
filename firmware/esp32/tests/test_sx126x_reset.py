# SPDX-License-Identifier: Apache-2.0
"""Compile the staged SX126x reset against its checked RadioLib boundary."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
UPSTREAM = Path(os.environ.get("MESHCORE_UPSTREAM", ROOT / ".tmp/onchip-upstream"))
FILES = ("CustomSX1262Wrapper.h", "RadioLibWrappers.cpp", "SX126xReset.h")

RADIO = r'''
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#define RADIOLIB_ERR_NONE 0
struct SerialSink {
  std::vector<std::string> messages;
  void printf(const char* format, int status) {
    char line[128]; std::snprintf(line, sizeof(line), format, status);
    messages.emplace_back(line);
  }
};
inline SerialSink Serial;
struct SX126x {
  std::vector<int> calls;
  int16_t reset_status = 0, gain_status = 0, read_status = 0;
  bool gain = true;
  uint8_t written = 0;
  int16_t resetAGC() { calls.push_back(1); return reset_status; }
  int16_t setRxBoostedGainMode(bool value) { calls.push_back(2); gain=value; return gain_status; }
  int16_t readRegister(int, uint8_t* data, int) { calls.push_back(3); *data=0x10; return read_status; }
  int16_t writeRegister(int, uint8_t* data, int) { calls.push_back(4); written=*data; return 0; }
};
'''

CHECK = r'''
#include <cassert>
#include "SX126xReset.h"
int main() {
  for (bool gain : {false, true}) {
    SX126x radio; Serial.messages.clear(); sx126xResetAGC(&radio, gain);
#ifdef SX126X_RX_BOOSTED_GAIN
    assert(radio.gain == gain);
    if (!gain) assert(radio.calls[1] == 2);
#endif
#ifdef SX126X_REGISTER_PATCH
    assert(radio.written == 0x11);
#endif
    assert(radio.calls.front() == 1 && Serial.messages.empty());
  }
  SX126x failed; failed.reset_status=-705; Serial.messages.clear();
  sx126xResetAGC(&failed, false);
  assert(failed.calls == std::vector<int>{1});
  assert(Serial.messages.size() == 1 && Serial.messages[0].find("-705") != std::string::npos);
#ifdef SX126X_RX_BOOSTED_GAIN
  SX126x gain_failed; gain_failed.gain_status=-707; Serial.messages.clear();
  sx126xResetAGC(&gain_failed, false);
  assert((gain_failed.calls == std::vector<int>{1,2}));
  assert(Serial.messages.size() == 1);
#endif
#ifdef SX126X_REGISTER_PATCH
  SX126x read_failed; read_failed.read_status=-706; Serial.messages.clear();
  sx126xResetAGC(&read_failed, false);
  assert(read_failed.calls.back() == 3 && read_failed.written == 0);
  assert(Serial.messages.size() == 1);
#endif
}
'''


class Reset(unittest.TestCase):
    def test_checked_reset_stops_optional_writes_on_error_and_preserves_gain(self):
        with tempfile.TemporaryDirectory(prefix="sx126x-reset-") as directory:
            root = Path(directory)
            for name in FILES:
                path = root / "src/helpers/radiolib" / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(subprocess.check_output(
                    ["git", "-C", str(UPSTREAM), "show", "HEAD:src/helpers/radiolib/" + name]))
            patch = ROOT / "firmware/shared/radio-reconfigure.patch"
            subprocess.run(["git", "apply", str(patch)], cwd=root, check=True)
            subprocess.run(["git", "apply", "--reverse", "--check", str(patch)], cwd=root, check=True)
            (root / "RadioLib.h").write_text(RADIO)
            (root / "MeshCore.h").write_text("#define MESH_DEBUG_PRINTLN(format, ...) Serial.printf(format, __VA_ARGS__)\n")
            (root / "check.cpp").write_text(CHECK)
            for flags in ([], ["-DSX126X_RX_BOOSTED_GAIN=1"],
                          ["-DSX126X_RX_BOOSTED_GAIN=1", "-DSX126X_REGISTER_PATCH=1"]):
                with self.subTest(flags=flags):
                    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                                    "-I", str(root), "-I", str(root / "src/helpers/radiolib"), *flags,
                                    str(root / "check.cpp"), "-o", str(root / "check")], check=True, timeout=30)
                    subprocess.run([str(root / "check")], check=True, timeout=3)


if __name__ == "__main__":
    unittest.main()
