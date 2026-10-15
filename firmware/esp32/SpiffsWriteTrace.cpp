// SPDX-License-Identifier: Apache-2.0
#include "SpiffsWriteTrace.h"
#if defined(ARDUINO_ARCH_ESP32) && ONCHIP_SPIFFS_WRITE_TRACE
#include <mutex>

// ESP-IDF's SPIFFS ABI; the filesystem stays opaque. Capture the raw negative
// return before the VFS maps it to errno and clears the filesystem error.
extern "C" {
struct spiffs_t;
int32_t __real_SPIFFS_write(spiffs_t *, int16_t, void *, int32_t);
}
namespace {
std::mutex traceMutex;
onchip::SpiffsWriteFailure firstFailure;
}
extern "C" int32_t __wrap_SPIFFS_write(spiffs_t *fs, int16_t file, void *bytes, int32_t length) {
  const int32_t result = __real_SPIFFS_write(fs, file, bytes, length);
  if (result < 0) {
    std::lock_guard<std::mutex> lock(traceMutex);
    if (!firstFailure.error) firstFailure = {result, length, file};
  }
  return result;
}
namespace onchip {
SpiffsWriteFailure spiffsWriteFailure() {
  std::lock_guard<std::mutex> lock(traceMutex);
  return firstFailure;
}
void resetSpiffsWriteFailure() {
  std::lock_guard<std::mutex> lock(traceMutex);
  firstFailure = {};
}
}
#endif
