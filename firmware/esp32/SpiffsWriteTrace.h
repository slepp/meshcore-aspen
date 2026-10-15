// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>

namespace onchip {
struct SpiffsWriteFailure {
  int32_t error = 0, length = 0;
  int16_t file = 0;
};
#if defined(ARDUINO_ARCH_ESP32) && ONCHIP_SPIFFS_WRITE_TRACE
SpiffsWriteFailure spiffsWriteFailure();
void resetSpiffsWriteFailure();
#else
inline SpiffsWriteFailure spiffsWriteFailure() { return {}; }
inline void resetSpiffsWriteFailure() {}
#endif
}
