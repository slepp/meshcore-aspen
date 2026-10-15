// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>

namespace onchip {
struct SpiffsWriteFailure {
  int32_t error = 0, length = 0;
  int16_t file = 0;
  char path[32]{}, openedBy[16]{}, closedBy[16]{};
  bool open = false;
  int32_t closeResult = 0;
  uint16_t deletedObject = 0, deletedSpan = 0, deletedPage = 0;
  char deletedBy[16]{};
};
#if defined(ARDUINO_ARCH_ESP32) && ONCHIP_SPIFFS_WRITE_TRACE
SpiffsWriteFailure spiffsWriteFailure();
void resetSpiffsWriteFailure();
#else
inline SpiffsWriteFailure spiffsWriteFailure() { return {}; }
inline void resetSpiffsWriteFailure() {}
#endif
}
