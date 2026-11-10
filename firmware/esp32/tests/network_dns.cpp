// SPDX-License-Identifier: Apache-2.0
#include "NetworkDns.h"
#include <cassert>

static uint32_t now;
unsigned long millis() { return now; }
void vTaskDelay(unsigned ticks) { now += ticks; }
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
  dns_test::deferred = true;
  assert(!resolveNetworkHost("slow.example", address, error, sizeof(error)));
  assert(now == 3000 && strstr(error, "timed out"));
  assert(!resolveNetworkHost("rooms.example", address, error, sizeof(error)));
  assert(now == 3000 && strstr(error, "still pending"));
  // The caller has returned, but the callback's storage is still alive.
  dns_test::finish();
  dns_test::deferred = false;
  assert(resolveNetworkHost("rooms.example", address, error, sizeof(error)));
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
  puts("DNS: address changes, network cache reset, timeout, late completion, lookup failure and recovery passed");
}
