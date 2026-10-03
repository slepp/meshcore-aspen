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
public:
  bool refresh(uint32_t epoch, uint32_t now) {
    if (epoch < 1715770351u || epoch > 4102444800u) return false;
    epoch_ = epoch;
    sampledAt_ = now;
    return true;
  }
  void poll(uint32_t now) {
    uint32_t earliest, latest;
    // Expiry is irreversible until refresh, including after millis() rolls over.
    if (!trustedTimeBounds(epoch_, uint32_t(now - sampledAt_), earliest, latest)) epoch_ = 0;
  }
  bool fresh(uint32_t now) {
    poll(now);
    return epoch_ != 0;
  }
  uint32_t epoch() const { return epoch_; }
  uint32_t ageMs(uint32_t now) const { return epoch_ ? uint32_t(now - sampledAt_) : 0; }
};
}
