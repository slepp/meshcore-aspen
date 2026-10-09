# SPDX-License-Identifier: Apache-2.0
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class NativeServicesTests(unittest.TestCase):
    def compile_run(self, source, *sources):
        with tempfile.TemporaryDirectory(dir=ROOT / ".tmp") as directory:
            binary = Path(directory) / "native-services"
            subprocess.run(
                [os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra",
                 "-Werror", "-pthread", "-I", str(ROOT / "firmware/runtime"),
                 "-I", str(ROOT / "firmware/esp32"),
                 "-I", str(ROOT / "firmware/shared"),
                 "-x", "c++", "-", *map(str, sources), "-o", str(binary)],
                input=source, text=True, check=True, capture_output=True,
            )
            subprocess.run([str(binary)], check=True)

    def test_independent_host_and_disabled_room_adapter(self):
        source = r'''
#include "CloudRoomService.h"
#include <cassert>
#include <type_traits>
class WifiKissMultiplexer {};
struct Host final : onchip::NativeNetworkHost {
  unsigned calls = 0;
  bool ensureNativeHttps() override { ++calls; return true; }
  onchip::NativeServiceRegistration attachNetworkService(
      onchip::NativeNetworkService &, const onchip::NativeServiceBudget &) override {
    ++calls;
    return onchip::NativeServiceRegistration::Full;
  }
};
struct Service final : onchip::NativeNetworkService {
  unsigned polls = 0, closes = 0;
  void poll(unsigned work) override { polls += work; }
  void close() override { ++closes; }
};
int main() {
  static_assert(std::is_same<onchip::BotNetworkService,
                            onchip::NativeNetworkService>::value);
  Host host;
  Service service;
  onchip::NativeNetworkHost &network = host;
  assert(network.ensureNativeHttps());
  assert(network.attachNetworkService(service, {"test", 0, 1}) ==
         onchip::NativeServiceRegistration::Full);
  onchip::NativeNetworkService &hooks = service;
  hooks.poll(1); hooks.close();
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
        self.compile_run(source, ROOT / "firmware/esp32/CloudRoomService.cpp")

    def test_registry_admission_budgets_fairness_and_shutdown(self):
        self.compile_run(r'''
#include "NativeServices.h"
#include <cassert>
#include <vector>
using namespace onchip;
struct Service final : NativeNetworkService {
  std::vector<unsigned> &calls;
  unsigned id, polls = 0, closes = 0, pending = 100;
  bool initialized = true;
  Service(std::vector<unsigned> &log, unsigned number) : calls(log), id(number) {}
  void poll(unsigned work) override {
    assert(initialized && !closes);
    calls.push_back(id);
    while (work-- && pending) { --pending; ++polls; }
  }
  void close() override { assert(!closes++); calls.push_back(10 + id); }
};
int main() {
  static_assert(NativeServiceRegistry::Capacity == 2);
  std::vector<unsigned> calls;
  Service first(calls, 1), second(calls, 2), rejected(calls, 3);
  NativeServiceRegistry registry(2);
  registry.poll();
  for (const auto &budget : {NativeServiceBudget{},
       NativeServiceBudget{"test", 0, 0},
       NativeServiceBudget{"bad name", 0, 1},
       NativeServiceBudget{"abcdefghijklmnopqrstuvwxyz123456", 0, 1},
       NativeServiceBudget{"test", 0, unsigned(UINT16_MAX) + 1}})
    assert(registry.attach(first, budget) == NativeServiceRegistration::InvalidBudget);
  assert(!registry.size());
  assert(registry.attach(first, {"first", 1, 3}) == NativeServiceRegistration::Attached);
  assert(registry.attach(first, {"different", 0, 1}) == NativeServiceRegistration::Duplicate);
  assert(registry.attach(second, {"first", 0, 1}) == NativeServiceRegistration::Duplicate);
  assert(registry.attach(second, {"second", 2, 1}) == NativeServiceRegistration::SocketBudget);
  assert(registry.size() == 1);
  assert(registry.attach(second, {"second", 1, 5}) == NativeServiceRegistration::Attached);
  assert(registry.attach(rejected, {"third", 0, 1}) == NativeServiceRegistration::Full);
  rejected.initialized = false;
  registry.poll(); registry.poll();
  assert((calls == std::vector<unsigned>{1, 2, 2, 1}));
  assert(first.polls == 6 && second.polls == 10 && rejected.polls == 0);
  registry.seal();
  assert(registry.attach(rejected, {"third", 0, 1}) == NativeServiceRegistration::Stopping);
  registry.poll(); registry.close(); registry.close(); registry.poll();
  assert((calls == std::vector<unsigned>{1, 2, 2, 1, 12, 11}));
  assert(first.closes == 1 && second.closes == 1 && rejected.closes == 0);
  NativeServiceRegistry disabled(0);
  assert(disabled.attach(rejected, {"third", 1, 1}) == NativeServiceRegistration::SocketBudget);
  disabled.close();
}
''')

    def test_atomic_registration_versus_network_shutdown(self):
        self.compile_run(r'''
#include "NativeServices.h"
#include <atomic>
#include <cassert>
#include <thread>
using namespace onchip;
struct Service final : NativeNetworkService {
  unsigned polls = 0, closes = 0;
  void poll(unsigned work) override { assert(work == 7 && !closes); ++polls; }
  void close() override { assert(!closes++); }
};
int main() {
  for (unsigned round = 0; round < 500; ++round) {
    Service service;
    NativeServiceRegistry registry(1);
    std::atomic<bool> start{false};
    std::thread network([&] {
      while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
      registry.poll(); registry.close(); registry.poll();
    });
    start.store(true, std::memory_order_release);
    const auto result = registry.attach(service, {"race", 1, 7});
    network.join();
    assert(result == NativeServiceRegistration::Attached ||
           result == NativeServiceRegistration::Stopping);
    assert(service.closes == (result == NativeServiceRegistration::Attached ? 1u : 0u));
    assert(registry.attach(service, {"race", 1, 7}) == NativeServiceRegistration::Stopping);
  }
}
''')
