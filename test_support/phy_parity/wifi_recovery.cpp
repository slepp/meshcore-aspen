#include "RadioNetwork.h"
#include <cassert>
#include <cstdio>
#include <initializer_list>

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
  Recovery flapping;
  flapping.begin(0, true);
  flapping.update(1000, true, false, 0, 0);
  for (uint32_t now = 2000; now < 121000; now += 1000)
    assert(!flapping.update(now, (now / 1000) % 2, false, 0, now).retry);
  assert(flapping.update(121000, true, false, 0, 121).retry);
  Recovery failed;
  failed.begin(0, true);
  assert(failed.update(30000, false, false, 0, 1).retry);
  assert(failed.retryIn(30000) == 30000 && failed.attempts() == 1);
  assert(failed.update(60000, false, false, 0, 2).retry);
  assert(failed.retryIn(60000) == 60000 && failed.attempts() == 2);
  assert(!failed.update(119999, false, false, 0, 3).retry);
  auto restart = failed.update(120000, false, false, 0, 4);
  assert(restart.retry && restart.restart && failed.retryIn(120000) == 120000);
  assert(failed.update(240000, false, false, 0, 5).restart);
  assert(failed.update(240001, true, true, 123, 5).up);
  assert(failed.attempts() == 0 && failed.retryIn(240001) == 0);
  assert(failed.update(240002, false, false, 0, 6).down);
  assert(!failed.update(270001, false, false, 0, 6).retry);
  assert(!failed.update(270002, false, false, 0, 6).restart);
  Recovery overdue;
  overdue.begin(0, true);
  // A late main-loop pass still performs recovery, even if association
  // appeared in the meantime. Only one attempt is issued, not a catch-up burst.
  assert(overdue.update(1000000, true, false, 0, 0).retry);
  assert(!overdue.update(1000000, true, false, 0, 0).retry);
  Recovery disabled;
  disabled.begin(0, true);
  assert(disabled.update(1, true, true, 123, 0).up);
  disabled.begin(2, false);
  assert(disabled.update(2, true, true, 123, 0).down);
  assert(!disabled.ready() && !disabled.enabled());
  assert(!disabled.update(1000000, true, true, 123, 0).retry);
  Recovery backoffWrap;
  const uint32_t boot = UINT32_MAX - 15000;
  backoffWrap.begin(boot, true);
  for (uint32_t elapsed : {30000u, 60000u, 120000u, 240000u}) {
    auto action = backoffWrap.update(boot + elapsed, false, false, 0, 0);
    assert(action.retry && action.restart == (elapsed >= 120000));
  }
  std::puts("WiFi recovery: loss, DHCP/flapping, backoff/station restart, late dispatch, disable/rearm and wrap passed");
}
