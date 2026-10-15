// SPDX-License-Identifier: Apache-2.0
#include "SpiffsWriteTrace.h"
#if defined(ARDUINO_ARCH_ESP32) && ONCHIP_SPIFFS_WRITE_TRACE
#include <mutex>
#include <cstdio>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// ESP-IDF's SPIFFS ABI; the filesystem stays opaque. Capture the raw negative
// return before the VFS maps it to errno and clears the filesystem error.
extern "C" {
struct spiffs_t;
int32_t __real_SPIFFS_write(spiffs_t *, int16_t, void *, int32_t);
int16_t __real_SPIFFS_open(spiffs_t *, const char *, uint16_t, uint16_t);
int32_t __real_SPIFFS_close(spiffs_t *, int16_t);
int32_t __real_spiffs_fd_return(spiffs_t *, int16_t);
int32_t __real_SPIFFS_remove(spiffs_t *, const char *);
void __real_spiffs_cb_object_event(spiffs_t *, void *, int, uint16_t, uint16_t, uint16_t, uint32_t);
}
namespace {
std::mutex traceMutex;
onchip::SpiffsWriteFailure firstFailure;
struct Descriptor {
  spiffs_t *fs = nullptr;
  char path[32]{}, openedBy[16]{}, closedBy[16]{};
  bool open = false;
  int32_t closeResult = 0;
  uintptr_t returnedBy = 0;
} descriptors[10];
char removedPath[32]{}, removedBy[16]{};
struct Deletion {
  uint16_t object = 0, span = 0, page = 0;
  char task[16]{};
} lastDeletion;
void taskName(char (&name)[16]) {
  snprintf(name, sizeof(name), "%s", pcTaskGetName(nullptr));
}
Descriptor *descriptor(spiffs_t *fs, int16_t file) {
  if (file < 1 || file > 10) return nullptr;
  auto &entry = descriptors[file - 1];
  return entry.fs == fs ? &entry : nullptr;
}
}
extern "C" int16_t __wrap_SPIFFS_open(spiffs_t *fs, const char *path, uint16_t flags, uint16_t mode) {
  const int16_t result = __real_SPIFFS_open(fs, path, flags, mode);
  if (result >= 1 && result <= 10) {
    std::lock_guard<std::mutex> lock(traceMutex);
    auto &entry = descriptors[result - 1];
    entry = {}; entry.fs = fs; entry.open = true;
    snprintf(entry.path, sizeof(entry.path), "%s", path);
    taskName(entry.openedBy);
  }
  return result;
}
extern "C" int32_t __wrap_SPIFFS_close(spiffs_t *fs, int16_t file) {
  const int32_t result = __real_SPIFFS_close(fs, file);
  std::lock_guard<std::mutex> lock(traceMutex);
  if (auto *entry = descriptor(fs, file)) {
    taskName(entry->closedBy);
    entry->closeResult = result;
    if (!result) entry->open = false;
  }
  return result;
}
extern "C" int32_t __wrap_spiffs_fd_return(spiffs_t *fs, int16_t file) {
  const int32_t result = __real_spiffs_fd_return(fs, file);
  std::lock_guard<std::mutex> lock(traceMutex);
  if (auto *entry = descriptor(fs, file)) {
    taskName(entry->closedBy);
    entry->returnedBy = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    entry->closeResult = result;
    if (!result) entry->open = false;
  }
  return result;
}
extern "C" int32_t __wrap_SPIFFS_remove(spiffs_t *fs, const char *path) {
  {
    std::lock_guard<std::mutex> lock(traceMutex);
    snprintf(removedPath, sizeof(removedPath), "%s", path);
    taskName(removedBy);
  }
  return __real_SPIFFS_remove(fs, path);
}
// Cross-object calls include garbage collection; nucleus-local calls are not wrapped.
extern "C" void __wrap_spiffs_cb_object_event(spiffs_t *fs, void *index, int event,
    uint16_t object, uint16_t span, uint16_t page, uint32_t size) {
  if (event == 2 && span == 0) {
    std::lock_guard<std::mutex> lock(traceMutex);
    lastDeletion = {object, span, page, {}};
    taskName(lastDeletion.task);
  }
  __real_spiffs_cb_object_event(fs, index, event, object, span, page, size);
}
extern "C" int32_t __wrap_SPIFFS_write(spiffs_t *fs, int16_t file, void *bytes, int32_t length) {
  const int32_t result = __real_SPIFFS_write(fs, file, bytes, length);
  if (result < 0) {
    std::lock_guard<std::mutex> lock(traceMutex);
    if (!firstFailure.error) {
      firstFailure = {}; firstFailure.error = result;
      firstFailure.length = length; firstFailure.file = file;
      if (auto *entry = descriptor(fs, file)) {
        snprintf(firstFailure.path, sizeof(firstFailure.path), "%s", entry->path);
        snprintf(firstFailure.openedBy, sizeof(firstFailure.openedBy), "%s", entry->openedBy);
        snprintf(firstFailure.closedBy, sizeof(firstFailure.closedBy), "%s", entry->closedBy);
        firstFailure.open = entry->open; firstFailure.closeResult = entry->closeResult;
        firstFailure.returnedBy = entry->returnedBy;
      }
      snprintf(firstFailure.removedPath, sizeof(firstFailure.removedPath), "%s", removedPath);
      snprintf(firstFailure.removedBy, sizeof(firstFailure.removedBy), "%s", removedBy);
      firstFailure.deletedObject = lastDeletion.object;
      firstFailure.deletedSpan = lastDeletion.span;
      firstFailure.deletedPage = lastDeletion.page;
      snprintf(firstFailure.deletedBy, sizeof(firstFailure.deletedBy), "%s", lastDeletion.task);
    }
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
  lastDeletion = {};
  removedPath[0] = removedBy[0] = 0;
}
}
#endif
