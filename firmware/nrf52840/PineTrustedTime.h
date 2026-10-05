#pragma once
#include <stdint.h>

namespace nrfmast {
inline bool companionTimeStepAllowed(uint32_t current, uint32_t candidate, bool freshTrusted) {
  return candidate >= 1715770351u && candidate <= 4102444800u &&
         (!freshTrusted || (candidate < current ? current - candidate <= 2u :
                            candidate - current <= 300u));
}
inline uint32_t companionClockValue(uint32_t current, uint32_t candidate, bool freshTrusted) {
  return freshTrusted && candidate < current ? current : candidate;
}
inline bool trustedTimeBounds(uint32_t epoch, uint32_t ageMs, uint32_t& earliest, uint32_t& latest) {
  earliest = latest = 0;
  if (epoch < 1715770351u || epoch > 4102444800u || ageMs >= 3600000u) return false;
  const uint64_t current = uint64_t(epoch) + ageMs / 1000;
  const uint32_t uncertainty = 2 + ageMs / 600000;
  if (current + uncertainty > 4102444800u) return false;
  earliest = current - uncertainty;
  latest = current + uncertainty;
  return true;
}
class TrustedTimeSample {
  uint32_t epoch_ = 0, sampledAt_ = 0;
  uint32_t upper_ = 0, lifetime_ = 3600000;
public:
  bool refresh(uint32_t epoch, uint32_t now) {
    if (epoch < 1715770351u || epoch > 4102444800u) return false;
    epoch_ = epoch;
    upper_ = 0; lifetime_ = 3600000;
    sampledAt_ = now;
    return true;
  }
  void poll(uint32_t now) {
    uint32_t earliest, latest;
    // Expiry is irreversible until refresh, including after millis() rolls over.
    if (!bounds(now, earliest, latest)) epoch_ = 0;
  }
  bool refreshBounds(uint32_t lower, uint32_t upper, uint32_t now, uint32_t lifetime) {
    if (lower < 1715770351u || upper < lower || upper > 4102444800u ||
        upper - lower > 32 || !lifetime || lifetime > 3600000u) return false;
    epoch_ = lower; upper_ = upper; sampledAt_ = now; lifetime_ = lifetime;
    return true;
  }
  bool bounds(uint32_t now, uint32_t &lower, uint32_t &upper) const {
    lower = upper = 0;
    const uint32_t age = uint32_t(now - sampledAt_);
    if (age >= lifetime_) return false;
    if (!upper_) return trustedTimeBounds(epoch_, age, lower, upper);
    const uint32_t drift = (age + 599999u) / 600000;
    const uint64_t high = uint64_t(upper_) + (uint64_t(age) + 999) / 1000 + drift;
    if (!epoch_ || high > 4102444800u) return false;
    lower = epoch_ + age / 1000 - drift;
    upper = high;
    return true;
  }
  void revoke() { epoch_ = upper_ = 0; }
  bool fresh(uint32_t now) {
    poll(now);
    return epoch_ != 0;
  }
  uint32_t epoch() const { return epoch_; }
  uint32_t ageMs(uint32_t now) const { return epoch_ ? uint32_t(now - sampledAt_) : 0; }
};
}
