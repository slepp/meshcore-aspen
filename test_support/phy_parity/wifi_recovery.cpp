#include "RadioNetwork.h"
#include <cassert>
#include <cstdio>

using Recovery = radio_network::WifiRecovery;

int main() {
  Recovery wifi;
  wifi.begin(0, true);
  assert(!wifi.update(29999, false, false, 0, 0).retry);
  assert(wifi.update(30000, false, false, 0, 0).retry);
  assert(!wifi.update(30001, false, false, 0, 0).retry);
  // Association is not readiness. Allow DHCP its own bounded interval.
  assert(!wifi.update(31000, true, false, 0, 0).up);
  assert(!wifi.update(150999, true, false, 0, 0).retry);
  assert(wifi.update(151000, true, false, 0, 0).retry);
  assert(wifi.update(151001, true, true, 123, 0).up);
  assert(wifi.ready());
  assert(!wifi.update(200000, true, true, 123, 0).retry);
  // IP loss with association intact starts a new DHCP deadline.
  auto lost = wifi.update(200001, true, false, 123, 1);
  assert(lost.down && !lost.up && !wifi.ready());
  assert(!wifi.update(320000, true, false, 123, 1).retry);
  assert(wifi.update(320001, true, false, 123, 1).retry);
  assert(wifi.update(320002, true, true, 456, 1).up);
  // A rapid loss/rejoin between dispatch passes must still retire old sockets.
  auto rapid = wifi.update(320003, true, true, 456, 2);
  assert(rapid.down && rapid.up);
  auto moved = wifi.update(320004, true, true, 789, 2);
  assert(moved.down && moved.up);
  assert(wifi.update(320005, false, false, 0, 3).down);
  assert(!wifi.update(350004, false, false, 0, 3).retry);
  assert(wifi.update(350005, false, false, 0, 3).retry);
  // Missing IP even with the SDK's HAS_IP flag must not expose readiness.
  assert(!wifi.update(350006, true, true, 0, 3).up);
  Recovery invalid;
  invalid.begin(0, false);
  assert(!invalid.update(1000000, false, false, 0, 0).retry);
  // Native wifi apply explicitly rearms retries without changing identity/PHY.
  invalid.begin(1000000, true);
  assert(invalid.update(1030000, false, false, 0, 0).retry);
  Recovery wrapped;
  wrapped.begin(UINT32_MAX - 15000, true);
  assert(!wrapped.update(14998, false, false, 0, 0).retry);
  assert(wrapped.update(14999, false, false, 0, 0).retry);
  assert(!wrapped.update(15000, false, false, 0, 0).retry);
  std::puts("WiFi recovery: AP/IP loss, DHCP, rapid rejoin, retry rearm and millis wrap passed");
}
