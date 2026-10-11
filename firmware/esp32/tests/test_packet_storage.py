# SPDX-License-Identifier: Apache-2.0
"""Exercise packet staging against Arduino's cached pre-open file size."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class PacketStorageTests(unittest.TestCase):
    def test_reused_staging_file_is_empty_after_truncation(self):
        source = (ROOT / 'firmware/esp32/EspPacketPrograms.cpp').read_text()
        start = source.index('  bool truncate(')
        stop = source.index('  bool append(', start)
        method = source[start:stop].replace(' override', '')
        fixture = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
struct File {
  unsigned cached; bool valid = true;
  explicit operator bool() const { return valid; }
  unsigned size() const { return cached; }
  void flush() {} // Arduino flush does not invalidate the size cache.
  void close() { valid = false; }
};
struct Filesystem {
  unsigned bytes = 2062;
  bool available = true;
  File open(const char *, const char *mode) {
    const unsigned prior = bytes;
    if (*mode == 'w') bytes = 0;
    return {prior, available}; // stat occurs before fopen("w").
  }
} SPIFFS;
struct Store {
  static void path(uint8_t, uint8_t, char (&out)[24]) { strcpy(out, "/packet-0.stage"); }
''' + method + r'''
};
int main() {
  Store store;
  assert(store.truncate(0, 3) && SPIFFS.bytes == 0);
  SPIFFS.bytes = 4096;
  assert(store.truncate(0, 3) && SPIFFS.bytes == 0);
  SPIFFS.available = false;
  assert(!store.truncate(0, 3));
}
'''
        with tempfile.TemporaryDirectory(dir=ROOT / '.tmp') as temporary:
            cpp = Path(temporary) / 'storage.cpp'
            binary = Path(temporary) / 'storage'
            cpp.write_text(fixture)
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', str(cpp), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
