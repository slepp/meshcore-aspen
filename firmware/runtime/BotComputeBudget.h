// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <algorithm>
#include <stdint.h>

namespace onchip {
// One worker shares a quarter of elapsed time, with 100 ms of burst credit.
// Entries are cooperative: an entry may borrow credit; later entries wait.
class BotComputeBudget {
  static constexpr int64_t Burst = 4 * 100000;
  int64_t credit_ = Burst;
  uint32_t sampled_ = 0;
  bool initialized_ = false;
  void refill(uint32_t now) {
    if (initialized_) credit_ = std::min(Burst, credit_ + uint32_t(now - sampled_));
    sampled_ = now; initialized_ = true;
  }
public:
  bool ready(uint32_t now) { refill(now); return credit_ > 0; }
  uint32_t waitUs(uint32_t now) {
    refill(now);
    return credit_ > 0 ? 0 : uint32_t(1 - credit_);
  }
  void charge(uint32_t started, uint32_t ended) {
    refill(ended);
    credit_ -= int64_t(uint32_t(ended - started)) * 4;
  }
};
} // namespace onchip
