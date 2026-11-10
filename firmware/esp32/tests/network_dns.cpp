// SPDX-License-Identifier: Apache-2.0
#include "NetworkDns.h"
#include <cassert>
#include <thread>

static uint32_t now;
static bool finishOnDelay;
static bool releaseOnDelay;
static std::atomic<bool> ownerReady{false}, releaseOwner{false}, ownerReleased{false};
unsigned long millis() { return now; }
void vTaskDelay(unsigned ticks) {
  now += ticks;
  if (finishOnDelay) { finishOnDelay = false; dns_test::finish(); }
  if (releaseOnDelay) {
    releaseOnDelay = false; releaseOwner = true;
    while (!ownerReleased.load()) std::this_thread::yield();
  }
}
int main() {
  using namespace onchip;
  IPAddress address;
  char error[128];
  assert(resolveNetworkHost("metrics.example", address, error, sizeof(error)));
  assert(uint32_t(address) == 0x010200c0 && dns_test::clears == 1);
  dns_test::answer = 0x020200c0;
  assert(resolveNetworkHost("metrics.example", address, error, sizeof(error)));
  assert(uint32_t(address) == 0x020200c0 && dns_test::clears == 1);
  ++TestWiFi::server;
  assert(resolveNetworkHost("rooms.example", address, error, sizeof(error)));
  assert(dns_test::clears == 2);
  std::thread owner([] {
    auto &mutex = network_dns::lookup().mutex;
    mutex.lock(); ownerReady = true;
    while (!releaseOwner.load()) std::this_thread::yield();
    mutex.unlock(); ownerReleased = true;
  });
  while (!ownerReady.load()) std::this_thread::yield();
  releaseOnDelay = true;
  assert(resolveNetworkHost("syslog.example", address, error, sizeof(error)));
  owner.join();
  assert(now == 1);
  now = 0;
  dns_test::deferred = true;
  assert(!resolveNetworkHost("slow.example", address, error, sizeof(error)));
  assert(now == 3000 && strstr(error, "timed out"));
  assert(!resolveNetworkHost("rooms.example", address, error, sizeof(error)));
  assert(now == 6000 && strstr(error, "late reply"));
  // The caller has returned, but the callback's storage is still alive.
  dns_test::deferred = false;
  finishOnDelay = true;
  assert(resolveNetworkHost("rooms.example", address, error, sizeof(error)));
  assert(now == 6001);
  dns_test::failure = true;
  assert(!resolveNetworkHost("missing.example", address, error, sizeof(error)));
  assert(strstr(error, "no IPv4"));
  dns_test::failure = false;
  assert(resolveNetworkHost("metrics.example", address, error, sizeof(error)));
  dns_test::queueFull = true;
  assert(!resolveNetworkHost("rooms.example", address, error, sizeof(error)));
  dns_test::queueFull = false;
  assert(resolveNetworkHost("rooms.example", address, error, sizeof(error)));
  dns_test::answer = 0xffffffff;
  assert(!resolveNetworkHost("bad.example", address, error, sizeof(error)));
  puts("DNS: contention, address changes, network cache reset, timeout, late completion, lookup failure and recovery passed");
}
