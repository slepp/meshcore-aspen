// SPDX-License-Identifier: Apache-2.0
#ifndef MESHCORE_ADAPTIVE_ADMISSION_H
#define MESHCORE_ADAPTIVE_ADMISSION_H
#include <stdint.h>
#include <string.h>

namespace onchip {
// Local radio load and bot admission only; the physical scheduler remains authoritative.
class AdaptiveAdmission {
public:
  enum Reason : uint8_t { Allowed, MetricsUnavailable, Congestion, CallerShare, CallersFull, ReservationsFull };
  static const char *code(Reason reason) {
    switch (reason) {
      case Allowed: return "allowed";
      case MetricsUnavailable: return "no-radio-metrics";
      case Congestion: return "congestion";
      case CallerShare: return "caller-share";
      case CallersFull: return "caller-slots-full";
      case ReservationsFull: return "reservations-full";
    }
    return "error";
  }
  static const char *name(Reason reason) {
    switch (reason) {
      case Allowed: return "allowed";
      case MetricsUnavailable: return "adaptive radio measurements unavailable";
      case Congestion: return "adaptive radio congestion";
      case CallerShare: return "adaptive caller airtime share";
      case CallersFull: return "adaptive caller slots full";
      case ReservationsFull: return "adaptive TX reservations full";
    }
    return "adaptive admission error";
  }
  static constexpr unsigned CallerLimit = 8, PendingLimit = 4;
private:
  struct Caller {
    uint8_t key[32]{};
    uint32_t seen = 0, debt = 0;
    bool used = false;
  } callers_[CallerLimit + 1]{};
  struct Pending {
    const void *packet = nullptr;
    uint32_t estimate = 0;
    uint8_t caller = 0;
    bool accepted = false;
    Pending(const void *p = nullptr, uint32_t cost = 0, uint8_t owner = 0, bool admitted = false)
        : packet(p), estimate(cost), caller(owner), accepted(admitted) {}
  } pending_[PendingLimit]{};
  bool enabled_ = false, baseline_ = false, congested_ = false;
  uint8_t quiet_ = 0, refillCursor_ = 0;
  uint16_t load_ = 0, scale_ = 1000;
  uint32_t sampleAt_ = 0, tx_ = 0, rx_ = 0, generation_ = 0, decayAt_ = 0;
  uint32_t limit_ = 360, denied_ = 0, refillFraction_ = 0;
  Reason last_ = Allowed;
  static uint32_t minimum(uint32_t a, uint32_t b) { return a < b ? a : b; }
  bool pinned(unsigned index) const {
    for (const auto &entry : pending_) if (entry.packet && entry.caller == index) return true;
    return false;
  }
  void age(uint32_t now) {
    const uint32_t elapsed = minimum(uint32_t(now - decayAt_), 60000);
    decayAt_ = now;
    const uint64_t credit = uint64_t(elapsed) * limit_ * scale_ + refillFraction_;
    uint32_t refill = credit / 60000000ull;
    refillFraction_ = credit % 60000000ull;
    // Drain the full aggregate allowance among debtors, redistributing any
    // unused share. Waiting callers without debt must not consume refill.
    while (refill) {
      unsigned debtors = 0;
      for (const auto &entry : callers_) if (entry.debt) ++debtors;
      if (!debtors) break;
      const uint32_t share = refill / debtors;
      if (share) {
        for (auto &entry : callers_) {
          const uint32_t amount = minimum(entry.debt, share);
          entry.debt -= amount; refill -= amount;
        }
      } else {
        // Rotate indivisible millisecond credits instead of favoring slot 0.
        for (unsigned i = 0; i <= CallerLimit && refill; ++i) {
          const unsigned index = refillCursor_;
          refillCursor_ = (refillCursor_ + 1) % (CallerLimit + 1);
          if (callers_[index].debt) { --callers_[index].debt; --refill; }
        }
        break;
      }
    }
    for (unsigned i = 0; i <= CallerLimit; ++i)
      if (callers_[i].used && uint32_t(now - callers_[i].seen) >= 60000 && !pinned(i))
        callers_[i] = {};
    bool debt = false;
    for (const auto &entry : callers_) debt = debt || entry.debt;
    if (!debt) refillFraction_ = 0; // Idle time does not bank future debt relief.
  }
  int caller(uint32_t now, const uint8_t key[32]) {
    age(now);
    int free = -1;
    for (unsigned i = 0; i < CallerLimit; ++i) {
      if (callers_[i].used && !memcmp(callers_[i].key, key, 32)) {
        callers_[i].seen = now;
        return i;
      }
      if (!callers_[i].used && free < 0) free = i;
    }
    if (free >= 0) {
      callers_[free].used = true; callers_[free].seen = now;
      memcpy(callers_[free].key, key, 32);
    }
    // Low-load traffic beyond the bounded identity table shares one accounting
    // bucket; it is not rejected until congestion requires individual shares.
    if (free < 0 && !congested_) {
      callers_[CallerLimit].used = true;
      callers_[CallerLimit].seen = now;
      return CallerLimit;
    }
    return free;
  }
  uint32_t used(unsigned index) const {
    uint32_t result = callers_[index].debt;
    for (const auto &entry : pending_)
      if (entry.packet && entry.caller == index) result += entry.estimate;
    return result;
  }
  Reason deny(Reason reason) { last_ = reason; ++denied_; return reason; }
public:
  void configure(bool enabled, uint32_t limit, uint32_t now) {
    *this = {};
    enabled_ = enabled; limit_ = limit; decayAt_ = now;
  }
  bool enabled() const { return enabled_; }
  bool congested() const { return congested_; }
  uint16_t loadPermille() const { return load_; }
  uint16_t scalePermille() const { return scale_; }
  uint32_t denied() const { return denied_; }
  Reason last() const { return last_; }
  Reason recordDenial(Reason reason) { return deny(reason); }
  unsigned callers() const {
    unsigned count = 0;
    for (const auto &entry : callers_) if (entry.used) ++count;
    return count;
  }
  unsigned pending() const {
    unsigned count = 0;
    for (const auto &entry : pending_) if (entry.packet) ++count;
    return count;
  }
  void sample(uint32_t now, uint32_t generation, uint32_t tx, uint32_t rx, uint8_t queued) {
    if (!enabled_) return;
    age(now);
    if (!baseline_ || generation != generation_) {
      baseline_ = true; generation_ = generation;
      sampleAt_ = now; tx_ = tx; rx_ = rx;
      load_ = 0; quiet_ = 0; congested_ = queued >= 4;
      scale_ = congested_ ? 500 : 1000;
      return;
    }
    const uint32_t elapsed = uint32_t(now - sampleAt_);
    if (queued >= 4) { congested_ = true; quiet_ = 0; if (scale_ > 500) scale_ = 500; }
    if (elapsed < 1000) return;
    const uint64_t busy = uint64_t(uint32_t(tx - tx_)) + uint32_t(rx - rx_);
    const uint32_t measured = busy >= elapsed ? 1000 : uint32_t(busy * 1000 / elapsed);
    load_ = (3u * load_ + measured) / 4;
    sampleAt_ = now; tx_ = tx; rx_ = rx;
    if (load_ >= 600 || queued >= 4) { congested_ = true; quiet_ = 0; }
    else if (load_ <= 300 && queued <= 1) {
      if (quiet_ < 3) ++quiet_;
      if (quiet_ == 3) congested_ = false;
    } else quiet_ = 0;
    scale_ = congested_ ? minimum(500, 1000 - load_) : 1000;
    if (scale_ < 250) scale_ = 250;
  }
  Reason work(uint32_t now, const uint8_t key[32], bool metrics, unsigned active,
              unsigned own, uint32_t quantum = 0) {
    if (!enabled_) return Allowed;
    if (!metrics) return deny(MetricsUnavailable);
    const int index = caller(now, key);
    if (index < 0) return deny(CallersFull);
    if (!congested_) return Allowed;
    const unsigned count = callers();
    const unsigned slots = scale_ >= 500 ? 2 : 1;
    const uint32_t allowance = uint64_t(limit_) * scale_ / 1000;
    uint32_t total = 0;
    for (unsigned i = 0; i <= CallerLimit; ++i) total += used(i);
    if (quantum > allowance || total > allowance - quantum) return deny(Congestion);
    const uint32_t share = allowance / count;
    const uint32_t debt = used(index);
    if (debt && (debt >= share || quantum > share || debt > share - quantum))
      return deny(CallerShare);
    if (own >= (count > 1 ? 1u : slots)) return deny(CallerShare);
    if (active >= slots) return deny(Congestion);
    return Allowed;
  }
  Reason reserve(uint32_t now, const uint8_t key[32], const void *packet,
                 uint32_t estimate, bool metrics) {
    if (!enabled_) return Allowed;
    if (!metrics) return deny(MetricsUnavailable);
    const int index = caller(now, key);
    if (index < 0) return deny(CallersFull);
    uint32_t total = 0;
    for (unsigned i = 0; i <= CallerLimit; ++i) total += used(i);
    if (congested_) {
      const uint32_t allowance = uint64_t(limit_) * scale_ / 1000;
      if (estimate > allowance || total > allowance - estimate) return deny(Congestion);
      // A caller can spend one packet quantum even when its equal share is
      // smaller than that packet. It cannot borrow a second quantum.
      const uint32_t share = allowance / callers();
      const uint32_t own = used(index);
      if (own && (estimate > share || own > share - estimate)) return deny(CallerShare);
    }
    for (auto &entry : pending_)
      if (!entry.packet) {
        entry = {packet, minimum(estimate, 60000), uint8_t(index), false};
        return Allowed;
      }
    return deny(ReservationsFull);
  }
  void finish(uint32_t now, const void *packet, bool known, uint32_t rf) {
    if (!enabled_) return;
    age(now);
    for (auto &entry : pending_)
      if (entry.packet == packet) {
        auto &owner = callers_[entry.caller];
        const uint32_t charge = known ? minimum(rf, 60000) : entry.estimate;
        owner.debt = minimum(60000, owner.debt + charge);
        owner.seen = now;
        entry = {};
        return;
      }
  }
  void accepted(const void *packet) {
    for (auto &entry : pending_) if (entry.packet == packet) entry.accepted = true;
  }
  void released(uint32_t now, const void *packet) {
    for (const auto &entry : pending_)
      if (entry.packet == packet) {
        const bool known = !entry.accepted;
        finish(now, packet, known, 0);
        return;
      }
  }
};
} // namespace onchip
#endif
