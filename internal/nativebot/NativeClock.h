// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>

namespace onchip {
constexpr uint64_t NativeSchedulerErrorLimitUs = 2000000;
struct NativeClockSample {
  uint64_t earliestUtcMs = 0, latestUtcMs = 0;
  uint64_t sampledMonotonicNs = 0;
  uint64_t epoch = 0;
  uint64_t errorBoundUs = 0;
  bool httpsTrusted = false;
  const char *reason = "kernel-untrusted";
};
// Returns scheduler usability; HTTPS synchronization does not require its error budget.
bool nativeBotClockSample(NativeClockSample &sample);
}
