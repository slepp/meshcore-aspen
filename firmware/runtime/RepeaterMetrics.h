// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>

namespace onchip {
struct BotRepeaterStats {
  uint16_t batteryMv = 0, queued = 0, errors = 0, directDuplicates = 0, floodDuplicates = 0;
  int16_t noise = 0, rssi = 0, snrQuarterDb = 0;
  uint32_t received = 0, sent = 0, txSeconds = 0, uptimeSeconds = 0;
  uint32_t sentFlood = 0, sentDirect = 0, receivedFlood = 0, receivedDirect = 0;
  uint32_t rxSeconds = 0, receiveErrors = 0;
};
enum class BotRepeaterError : uint8_t {
  None, Unavailable, Disabled, NotDue, Busy, Clock, Capacity, Timeout,
  Malformed, Cancelled, Permission, Transmission, Frequency
};
struct BotRepeaterSnapshot {
  char alias[17]{};
  bool configured = false, available = false, fresh = false;
  BotRepeaterError error = BotRepeaterError::Unavailable;
  uint32_t ageSeconds = 0, attempts = 0, failures = 0;
  BotRepeaterStats stats{};
};
} // namespace onchip
