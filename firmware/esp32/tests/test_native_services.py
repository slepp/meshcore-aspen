# SPDX-License-Identifier: Apache-2.0
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class NativeServicesTests(unittest.TestCase):
    def test_independent_host_and_disabled_room_adapter(self):
        source = r'''
#include "CloudRoomService.h"
#include <cassert>
#include <type_traits>
class WifiKissMultiplexer {};
struct Host final : onchip::NativeNetworkHost {
  unsigned calls = 0;
  bool ensureNativeHttps() override { ++calls; return true; }
  bool attachNetworkService(onchip::NativeNetworkService &) override {
    ++calls;
    return false;
  }
};
struct Service final : onchip::NativeNetworkService {
  unsigned polls = 0, closes = 0;
  void poll() override { ++polls; }
  void close() override { ++closes; }
};
int main() {
  static_assert(std::is_same<onchip::BotNetworkService,
                            onchip::NativeNetworkService>::value);
  Host host;
  Service service;
  onchip::NativeNetworkHost &network = host;
  assert(network.ensureNativeHttps());
  assert(!network.attachNetworkService(service));
  onchip::NativeNetworkService &hooks = service;
  hooks.poll(); hooks.close();
  assert(service.polls == 1 && service.closes == 1);
  WifiKissMultiplexer mux;
  assert(!onchip::beginCloudRoom(mux, host));
  assert(host.calls == 2);
  onchip::loopCloudRoom();
  assert(onchip::cloudRoomAliases() == 0);
  assert(onchip::cloudRoomPublicKey(0) == nullptr);
  assert(!onchip::requestCloudRoomAdvertisement(0));
}
'''
        with tempfile.TemporaryDirectory(dir=ROOT / ".tmp") as directory:
            binary = Path(directory) / "native-services"
            subprocess.run(
                [os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra",
                 "-Werror", "-I", str(ROOT / "firmware/runtime"),
                 "-I", str(ROOT / "firmware/esp32"),
                 "-I", str(ROOT / "firmware/shared"),
                 "-x", "c++", "-", str(ROOT / "firmware/esp32/CloudRoomService.cpp"),
                 "-o", str(binary)],
                input=source, text=True, check=True, capture_output=True,
            )
            subprocess.run([str(binary)], check=True)
