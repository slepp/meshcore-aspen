// SPDX-License-Identifier: Apache-2.0
#include "AdaptiveAdmission.h"
#include <assert.h>
#include <stdio.h>

using onchip::AdaptiveAdmission;
using Gate = AdaptiveAdmission;
static uint8_t a[32] = {1}, b[32] = {2};
static int packets[8];

static void busy(Gate &gate, uint32_t start = 0, uint32_t initial = 0) {
  gate.sample(start, 1, initial, initial, 0);
  for (unsigned i = 1; i <= 6; ++i)
    // TX from all roles, plus estimated physical RX, are additive once each.
    gate.sample(start + i * 1000, 1, initial + i * 600, initial + i * 400, 0);
  assert(gate.congested() && gate.loadPermille() >= 600);
}
static unsigned cleanCallerWait(unsigned count) {
  Gate gate;
  gate.configure(true, 360, 0);
  gate.sample(0, 1, 0, 0, 4); // 180 ms/min aggregate allowance.
  uint8_t keys[Gate::CallerLimit][32]{};
  for (unsigned i = 0; i < count; ++i) {
    keys[i][0] = i + 1;
    assert(gate.work(0, keys[i], true, 0, 0) == Gate::Allowed);
  }
  assert(gate.reserve(0, keys[0], &packets[0], 170, true) == Gate::Allowed);
  gate.finish(0, &packets[0], true, 170);
  for (unsigned second = 1; second <= 20; ++second) {
    gate.sample(second * 1000, 1, 0, 0, 4);
    bool admitted = false;
    for (unsigned i = 1; i < count; ++i)
      admitted = gate.work(second * 1000, keys[i], true, 0, 0, 60) == Gate::Allowed || admitted;
    if (admitted) return second;
  }
  assert(false && "zero-debt retries must not waste aggregate refill");
  return 0;
}
int main() {
  assert(!strcmp(Gate::code(Gate::MetricsUnavailable), "no-radio-metrics"));
  assert(!strcmp(Gate::code(Gate::ReservationsFull), "reservations-full"));
  assert(cleanCallerWait(2) == 17);
  assert(cleanCallerWait(8) == 17);
  {
    Gate gate;
    const uint32_t start = UINT32_MAX - 1000;
    gate.configure(true, 360, start);
    gate.sample(start, 1, 0, 0, 4);
    assert(gate.reserve(start, a, &packets[0], 170, true) == Gate::Allowed);
    gate.finish(start, &packets[0], true, 170);
    for (unsigned ms = 10; ms <= 16660; ms += 10)
      gate.sample(start + ms, 1, 0, 0, 4);
    assert(gate.work(start + 16666, b, true, 0, 0, 60) == Gate::Congestion);
    assert(gate.work(start + 16667, b, true, 0, 0, 60) == Gate::Allowed);
  }
  {
    Gate gate;
    uint8_t c[32] = {3};
    gate.configure(true, 360, 0);
    gate.sample(0, 1, 0, 0, 4);
    assert(gate.reserve(0, a, &packets[0], 1, true) == Gate::Allowed);
    gate.finish(0, &packets[0], true, 1);
    assert(gate.reserve(0, b, &packets[1], 169, true) == Gate::Allowed);
    gate.finish(0, &packets[1], true, 169);
    gate.sample(1000, 1, 0, 0, 4);
    assert(gate.work(1000, c, true, 0, 0, 14) == Gate::Congestion);
    assert(gate.work(1000, c, true, 0, 0, 13) == Gate::Allowed);
  }
  {
    Gate gate;
    uint8_t keys[3][32] = {{1}, {2}, {3}};
    gate.configure(true, 360, 0);
    gate.sample(0, 1, 0, 0, 4);
    for (unsigned i = 0; i < 3; ++i) {
      assert(gate.reserve(0, keys[i], &packets[i], 7, true) == Gate::Allowed);
      gate.finish(0, &packets[i], true, 7);
    }
    const uint32_t times[] = {334, 668, 1000};
    for (const auto now : times) gate.sample(now, 1, 0, 0, 4);
    for (const auto &key : keys)
      assert(gate.work(1000, key, true, 0, 0, 54) == Gate::Allowed);
  }
  {
    Gate gate;
    gate.configure(false, 360, 0);
    gate.sample(0, 1, 0, 0, 8);
    gate.sample(1000, 1, UINT32_MAX, UINT32_MAX, 8);
    assert(!gate.congested() && gate.pending() == 0);
    assert(gate.work(1000, a, false, 4, 4) == Gate::Allowed);
    assert(gate.reserve(1000, a, &packets[0], 60000, false) == Gate::Allowed);
    assert(gate.pending() == 0);
  }
  {
    Gate gate;
    gate.configure(true, 3600, 0);
    gate.sample(0, 1, 0, 0, 0);
    gate.sample(1000, 1, 100, 100, 0);
    assert(!gate.congested());
    assert(gate.work(1000, a, true, 3, 3) == Gate::Allowed);
    assert(gate.reserve(1000, a, &packets[0], 3600, true) == Gate::Allowed);
    gate.finish(1000, &packets[0], true, 0);
    assert(gate.pending() == 0);
    assert(gate.reserve(1000, a, &packets[0], 3600, true) == Gate::Allowed);
    gate.released(1000, &packets[0]); // Rejected before physical admission.
    assert(gate.pending() == 0);
    assert(gate.work(1000, a, false, 0, 0) == Gate::MetricsUnavailable);
  }
  {
    Gate gate;
    gate.configure(true, 3600, 0);
    busy(gate);
    assert(gate.work(6000, a, true, 0, 0) == Gate::Allowed);
    assert(gate.work(6000, b, true, 0, 0) == Gate::Allowed);
    assert(gate.reserve(6000, a, &packets[0], 400, true) == Gate::Allowed);
    gate.finish(6000, &packets[0], true, 400);
    assert(gate.reserve(6000, a, &packets[0], 400, true) == Gate::CallerShare);
    assert(gate.reserve(6000, b, &packets[1], 400, true) == Gate::Allowed);
    assert(gate.reserve(6000, b, &packets[2], 400, true) != Gate::Allowed);
    assert(gate.work(6000, a, true, 1, 0) == Gate::Congestion);
    assert(gate.work(6000, a, true, 1, 1) == Gate::CallerShare);
    gate.finish(6000, &packets[1], false, 0); // Unknown is charged, never replayed.
    assert(gate.pending() == 0);
    assert(gate.reserve(6000, b, &packets[1], 400, true) != Gate::Allowed);
    // Falling local load recovers only after smoothing and three quiet samples.
    for (unsigned i = 7; i <= 30; ++i) gate.sample(i * 1000, 1, 3600, 2400, 0);
    assert(!gate.congested() && gate.scalePermille() == 1000);
    assert(gate.work(30000, a, true, 3, 3) == Gate::Allowed);
    assert(gate.reserve(30000, a, &packets[0], 1000, true) == Gate::Allowed);
  }
  {
    Gate gate;
    gate.configure(true, 3600, 0);
    gate.sample(0, 1, 0, 0, 4); // Immediate half-allowance from queue pressure.
    assert(gate.work(0, a, true, 0, 0, 1801) == Gate::Congestion);
    assert(gate.work(0, a, true, 0, 0, 1800) == Gate::Allowed);
    assert(gate.work(0, b, true, 0, 0, 100) == Gate::Allowed);
    assert(gate.reserve(0, a, &packets[0], 900, true) == Gate::Allowed);
    gate.finish(0, &packets[0], true, 900);
    assert(gate.work(0, a, true, 0, 0, 1) == Gate::CallerShare);
    assert(gate.work(0, b, true, 0, 0, 901) == Gate::Congestion);
    assert(gate.work(0, b, true, 0, 0, 900) == Gate::Allowed);
  }
  {
    Gate gate;
    const uint32_t start = UINT32_MAX - 3000, initial = UINT32_MAX - 1000;
    gate.configure(true, 3600, start);
    busy(gate, start, initial); // Clock and both airtime counters wrap.
    assert(gate.loadPermille() < 1000);
    gate.sample(start + 7000, 2, 0, 0, 0); // New source generation rebases counters.
    assert(!gate.congested() && gate.loadPermille() == 0);
    gate.sample(start + 8000, 2, 0, 0, 4);
    assert(gate.congested());
    for (unsigned i = 9; i <= 11; ++i) gate.sample(start + i * 1000, 2, 0, 0, 0);
    assert(!gate.congested());
  }
  {
    Gate gate;
    gate.configure(true, 3600, 0);
    gate.sample(0, 1, 0, 0, 0);
    gate.sample(1000, 1, UINT32_MAX, 1, 0);
    assert(gate.loadPermille() == 250); // Saturate before narrowing a 2^32-ms sum.
  }
  {
    Gate gate;
    gate.configure(true, 3600, 0);
    gate.sample(0, 1, 0, 0, 4);
    assert(gate.reserve(0, a, &packets[0], 500, true) == Gate::Allowed);
    gate.accepted(&packets[0]);
    gate.released(0, &packets[0]); // Loss/cancel after admission is conservative.
    assert(gate.pending() == 0);
    gate.finish(0, &packets[0], true, 0); // Duplicate terminal cannot refund twice.
    assert(gate.reserve(0, a, &packets[1], 1500, true) == Gate::Congestion);
    gate.sample(61000, 1, 0, 0, 4);
    assert(gate.reserve(61000, b, &packets[1], 500, true) == Gate::Allowed);
    gate.finish(61000, &packets[1], true, 0); // Known failure before RF refunds.
    assert(gate.reserve(61000, b, &packets[1], 500, true) == Gate::Allowed);
    gate.finish(61000, &packets[1], true, 700); // Measured duration, not queue wait.
    assert(gate.pending() == 0);
  }
  {
    Gate gate;
    gate.configure(true, 3600, 0);
    gate.sample(0, 1, 0, 0, 0);
    for (unsigned i = 0; i < Gate::CallerLimit; ++i) {
      uint8_t key[32] = {uint8_t(i + 1)};
      assert(gate.work(0, key, true, 0, 0) == Gate::Allowed);
    }
    uint8_t extra[32] = {99};
    assert(gate.work(0, extra, true, 0, 0) == Gate::Allowed);
    gate.sample(1000, 1, 0, 0, 4);
    assert(gate.work(1000, extra, true, 0, 0) == Gate::CallersFull);
    assert(gate.callers() == Gate::CallerLimit + 1);
    assert(gate.work(60000, extra, true, 0, 0) == Gate::Allowed);
    for (unsigned i = 0; i < Gate::PendingLimit; ++i)
      assert(gate.reserve(60000, extra, &packets[i], 10, true) == Gate::Allowed);
    assert(gate.reserve(60000, extra, &packets[4], 10, true) == Gate::ReservationsFull);
    for (unsigned i = 0; i < Gate::PendingLimit; ++i) gate.released(60000, &packets[i]);
    assert(gate.pending() == 0);
  }
  puts("adaptive admission: disabled, low/high load, caller fairness, role load, ceilings, recovery, rollover, generations, loss/cancel/failure PASS");
}
