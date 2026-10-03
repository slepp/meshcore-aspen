#include "../PineFieldUpdate.h"
#include "../PineTrustedTime.h"
#include <cassert>
#include <cstdio>

static void retainedTrustExpiry() {
  const uint32_t utc = 1800000000u, future = 4000000000u;
  uint32_t earliest, latest;
  nrfmast::TrustedTimeSample sample;
  auto bounds = [&](uint32_t now) {
    sample.poll(now);
    return nrfmast::trustedTimeBounds(sample.epoch(), sample.ageMs(now), earliest, latest);
  };
  auto companion = [&](uint32_t now, uint32_t current, uint32_t candidate) {
    const bool fresh = sample.fresh(now);
    if (!nrfmast::companionTimeStepAllowed(current, candidate, fresh)) return false;
    return sample.refresh(nrfmast::companionClockValue(current, candidate, fresh), now);
  };
  assert(!sample.fresh(0) && !bounds(0) && earliest == 0 && latest == 0);
  assert(!sample.refresh(1715770350u, 0) && !sample.refresh(4102444801u, 0));
  assert(companion(1000, future, utc));  // Boot has no trusted correction guard.
  assert(bounds(1000) && earliest == utc - 2 && latest == utc + 2);
  assert(!companion(1001, utc, utc + 301) && sample.ageMs(1001) == 1);
  assert(!companion(1002, utc, utc - 3) && sample.epoch() == utc);
  assert(companion(1003, utc, utc + 300));
  assert(companion(1004, utc + 301, utc + 299) && sample.epoch() == utc + 301);
  assert(!sample.refresh(0, 1005) && sample.epoch() == utc + 301 && sample.ageMs(1005) == 1);

  assert(sample.refresh(future, 1000));
  assert(!companion(3600999, future + 3599, utc + 3599));
  assert(sample.fresh(3600999));
  sample.poll(3601000);  // Main-loop expiry runs even with no jobs or time queries.
  const uint64_t outage = (uint64_t(1) << 32) + 1000;
  const uint32_t wrappedNow = uint32_t(uint64_t(1000) + outage);
  assert(!bounds(wrappedNow) && earliest == 0 && latest == 0 && !sample.fresh(wrappedNow));
  std::printf("Pine expired UTC: outage_ms=%llu wrapped_age_ms=%u trusted=%u bounds=%u..%u companion_guard_fresh=%u\n",
              static_cast<unsigned long long>(outage), uint32_t(wrappedNow - 1000),
              sample.epoch() != 0, earliest, latest, sample.fresh(wrappedNow));
  assert(companion(wrappedNow, future + uint32_t(outage / 1000), utc + uint32_t(outage / 1000)));
  assert(bounds(wrappedNow) && sample.ageMs(wrappedNow) == 0);
  assert(sample.refresh(future, 1000));  // Explicit bot time can always correct a fresh clock.
  assert(sample.refresh(utc, 1001) && bounds(1001));

  assert(sample.refresh(utc, 1000));
  sample.poll(UINT32_MAX - 100);  // A large missed-poll interval also latches expiry.
  assert(!sample.fresh(1000) && !bounds(1000));
  assert(companion(1000, future, utc));

  const uint32_t start = UINT32_MAX - 1000;
  assert(sample.refresh(utc, start) && sample.fresh(start));
  const uint32_t acrossWrap = start + 1800000u;
  assert(bounds(acrossWrap) && sample.ageMs(acrossWrap) == 1800000u &&
         earliest <= utc + 1800 && latest >= utc + 1800);
  assert(companion(acrossWrap, utc + 1800, utc + 1799) && sample.epoch() == utc + 1800);
  assert(bounds(acrossWrap + 3599999u));
  sample.poll(acrossWrap + 3600000u);
  assert(!bounds(acrossWrap) && !sample.fresh(start));  // Queries cannot revive revoked samples.
  assert(companion(start, future, utc) && bounds(start));
  for (unsigned i = 1; i <= 100; ++i) {
    const uint32_t now = start + i * 1800000u;
    assert(bounds(now) && sample.ageMs(now) == 1800000u);
    assert(sample.refresh(utc + i * 1800u, now) && bounds(now));
  }

  nrfmast::TrustedTimeSample reboot;
  assert(!reboot.fresh(start) && reboot.epoch() == 0);
  assert(reboot.refresh(4102444800u, start) && !reboot.fresh(start));
  std::puts("Pine UTC: >49.71-day outage stays revoked after millis wrap; inactive-job housekeeping, missed polls, companion correction after expiry, refresh across wrap, fresh +300s/-2s guard, explicit admin correction, bounds and reboot passed");
}
int main() {
  retainedTrustExpiry();
  assert(nrfmast::authenticatedBleBond(true, true, true, 1, 3));
  assert(nrfmast::authenticatedBleBond(true, true, true, 1, 4));
  assert(!nrfmast::authenticatedBleBond(false, true, true, 1, 3));
  assert(!nrfmast::authenticatedBleBond(true, false, true, 1, 3));
  assert(!nrfmast::authenticatedBleBond(true, true, false, 1, 3));
  assert(!nrfmast::authenticatedBleBond(true, true, true, 1, 2));
  assert(!nrfmast::authenticatedBleBond(true, true, true, 2, 3));
  nrfmast::FieldUpdateWindow window;
  const uint8_t start[] = {1, 4}, fullflash[] = {1, 3}, invalid[] = {2};
  assert(!window.authorize(0, true, start, 2));
  assert(!window.arm(0, false, true, true));
  assert(!window.arm(0, true, false, true));
  assert(!window.arm(0, true, true, false));
  const uint32_t armed = UINT32_MAX - 1000;
  assert(window.arm(armed, true, true, true));
  assert(!window.arm(armed + 1, true, true, true));
  assert(window.remaining(armed + 1) == 119999);
  assert(!window.authorize(armed + 1, false, start, 2));
  assert(!window.authorize(armed + 1, true, nullptr, 1));
  assert(!window.authorize(armed + 1, true, start, 0));
  assert(!window.authorize(armed + 1, true, start, 3));
  assert(!window.authorize(armed + 1, true, fullflash, 2));
  assert(!window.authorize(armed + 1, true, invalid, 1));
  assert(window.authorize(armed + 1, true, start, 1));
  assert(window.authorize(armed + 119999, true, start, 2));
  assert(!window.authorize(armed + 120000, true, start, 2));
  assert(window.expired(armed + 120000));
  assert(window.remaining(armed + 120000) == 0);
  window.revoke();
  assert(!window.active(armed));
  assert(!window.authorize(armed, true, start, 2));
  uint32_t earliest, latest;
  assert(!nrfmast::trustedTimeBounds(0, 0, earliest, latest));
  assert(earliest == 0 && latest == 0);
  assert(nrfmast::trustedTimeBounds(1800000000, 0, earliest, latest));
  assert(earliest == 1799999998 && latest == 1800000002);
  assert(nrfmast::trustedTimeBounds(1800000000, 3599999, earliest, latest));
  assert(earliest == 1800003592 && latest == 1800003606);
  assert(!nrfmast::trustedTimeBounds(1800000000, 3600000, earliest, latest));
  assert(earliest == 0 && latest == 0);
  assert(!nrfmast::trustedTimeBounds(1800000000, 3600001, earliest, latest));
  assert(!nrfmast::trustedTimeBounds(4102444800, 0, earliest, latest));
  assert(!nrfmast::trustedTimeBounds(1715770350, 0, earliest, latest));
  assert(nrfmast::companionTimeStepAllowed(1715770351, 1800000000, false));
  assert(nrfmast::companionTimeStepAllowed(1800000000, 1800000000, true));
  assert(nrfmast::companionTimeStepAllowed(1800000000, 1800000300, true));
  assert(!nrfmast::companionTimeStepAllowed(1800000000, 1800000301, true));
  assert(nrfmast::companionTimeStepAllowed(1800000000, 1799999999, true));
  assert(nrfmast::companionTimeStepAllowed(1800000000, 1799999998, true));
  assert(!nrfmast::companionTimeStepAllowed(1800000000, 1799999997, true));
  const uint32_t roundedHostUtc = 1800000000, slightlyAheadClock = roundedHostUtc + 1;
  assert(nrfmast::companionTimeStepAllowed(slightlyAheadClock, roundedHostUtc, true));
  const uint32_t renewed = nrfmast::companionClockValue(slightlyAheadClock, roundedHostUtc, true);
  assert(renewed == slightlyAheadClock);
  assert(nrfmast::trustedTimeBounds(renewed, 0, earliest, latest));
  assert(earliest <= roundedHostUtc && latest >= slightlyAheadClock);
  assert(!nrfmast::companionTimeStepAllowed(1800000000, 4102444801u, false));
  const uint32_t futureUtc = 4000000000u, correctUtc = 1800000000u;
  assert(nrfmast::companionTimeStepAllowed(1715770351, futureUtc, false));
  assert(nrfmast::trustedTimeBounds(futureUtc, 0, earliest, latest));
  assert(!nrfmast::companionTimeStepAllowed(futureUtc, correctUtc, true));
  assert(nrfmast::trustedTimeBounds(futureUtc, 3599999, earliest, latest));
  assert(!nrfmast::companionTimeStepAllowed(futureUtc + 3599, correctUtc + 3599, true));
  assert(!nrfmast::trustedTimeBounds(futureUtc, 3600000, earliest, latest));
  assert(nrfmast::companionTimeStepAllowed(futureUtc + 3600, correctUtc + 3600, false));
  assert(nrfmast::trustedTimeBounds(correctUtc + 3600, 0, earliest, latest));
}
