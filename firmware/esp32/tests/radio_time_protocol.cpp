// SPDX-License-Identifier: Apache-2.0
#include "../../shared/RadioTimeProtocol.h"
#include "../../shared/SntpConfig.h"
#include "../../nrf52840/PineTrustedTime.h"
#include <cassert>
#include <cstdio>
#include <initializer_list>

int main() {
  radio_time::SntpConfig config;
  assert(radio_time::validConfig(config));
  for (auto invalid : {"", "a b", ".host", "host.", "a..b", "http://ntp", "-ntp"})
    assert(!radio_time::validServer(invalid));
  assert(radio_time::editConfig(config, "set sntp.interval 86400"));
  assert(!radio_time::editConfig(config, "set sntp.interval 86401"));
  assert(!radio_time::editConfig(config, "set sntp.interval -60"));
  assert(!radio_time::editConfig(config, "set sntp.interval 60junk"));
  uint8_t provider[32]{}, wrong[32]{}, nonce[8]{}, request[16]{}, reply[32]{};
  provider[0] = wrong[0] = 9; provider[31] = 1; wrong[31] = 2; nonce[7] = 77;
  radio_time::put32(request, 100);
  request[4] = radio_time::RequestType; request[5] = radio_time::ProtocolVersion;
  memcpy(request + 6, nonce, 8);
  assert(radio_time::request(request, 14) && radio_time::request(request, 16));
  request[15] = 1; assert(!radio_time::request(request, 16)); request[15] = 0;
  assert(!radio_time::request(request, 13));
  radio_time::response(request, reply, 1, 1800000000, 1800000001, 10, 3600000);
  radio_time::PendingTimeRequest pending;
  uint32_t lower, upper, ttl;
  pending.begin(provider, nonce, 100, 1000);
  assert(!pending.accept(wrong, reply, 32, true, 1200, lower, upper, ttl));
  assert(!pending.accept(provider, reply, 32, false, 1200, lower, upper, ttl));
  reply[0] ^= 1;
  assert(!pending.accept(provider, reply, 32, true, 1200, lower, upper, ttl));
  reply[0] ^= 1;
  reply[5] = 2;
  assert(!pending.accept(provider, reply, 32, true, 1200, lower, upper, ttl));
  reply[5] = 1;
  reply[8] ^= 1;
  assert(!pending.accept(provider, reply, 32, true, 1200, lower, upper, ttl));
  reply[8] ^= 1;
  assert(pending.accept(provider, reply, 32, true, 5900, lower, upper, ttl));
  assert(lower == 1800000000 && upper == 1800000006 && ttl == 3595100);
  assert(!pending.accept(provider, reply, 32, true, 5901, lower, upper, ttl));
  pending.begin(provider, nonce, 100, 1000);
  assert(!pending.accept(provider, reply, 32, true, 6000, lower, upper, ttl));
  pending.begin(provider, nonce, 100, UINT32_MAX - 100);
  assert(pending.accept(provider, reply, 32, true, 100, lower, upper, ttl));
  pending.begin(provider, nonce, 100, 1000);
  radio_time::response(request, reply, 0, 0, 0, 0, 0);
  assert(!pending.accept(provider, reply, 32, true, 1100, lower, upper, ttl));
  pending.begin(provider, nonce, 100, 1000);
  radio_time::response(request, reply, 1, 1800000000, 1800000040, 0, 3600000);
  assert(!pending.accept(provider, reply, 32, true, 1100, lower, upper, ttl));
  assert(!pending.pending(1101));
  pending.begin(provider, nonce, 100, 1000);
  radio_time::response(request, reply, 2, 1800000000, 1800000002, 3600, 3600000);
  assert(!pending.accept(provider, reply, 32, true, 1100, lower, upper, ttl));
  nrfmast::TrustedTimeSample clock;
  assert(clock.refreshBounds(1800000000, 1800000006, UINT32_MAX - 100, 1000));
  assert(clock.bounds(100, lower, upper) && lower == 1799999999 && upper == 1800000008);
  clock.poll(900);
  assert(!clock.fresh(900));
  assert(!clock.fresh(UINT32_MAX - 100));
  assert(clock.refreshBounds(1800000000, 1800000006, 1000, 3600000));
  clock.revoke();
  assert(!clock.fresh(1001));
  puts("PASS RF time full-key/tag/nonce/path correlation, deadline, uncertainty, errors, rollover, expiry and configuration limits");
}
