// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <Arduino.h>
#ifdef NRF52_PLATFORM
#include <cstdlib>
#else
#include <esp_heap_caps.h>
#endif
#include <new>
#include <utility>

namespace onchip {
// Dispatch-owned application state only. Never use this for ISR/DMA buffers,
// task stacks, synchronization primitives or cache-disabled callbacks.
template <class T, class... Args>
T *allocateRoleStorage(const char *role, Args &&...args) {
#ifdef NRF52_PLATFORM
  void *memory = calloc(1, sizeof(T));
#else
  void *memory = heap_caps_calloc(1, sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
  if (!memory) {
#ifdef NRF52_PLATFORM
    Serial.printf("On-chip %s storage allocation failed (%u bytes)\n",
                  role, unsigned(sizeof(T)));
#else
    Serial.printf("On-chip %s PSRAM allocation failed (%u bytes); no internal fallback\n",
                  role, unsigned(sizeof(T)));
#endif
    return nullptr;
  }
#ifdef NRF52_PLATFORM
  Serial.printf("On-chip %s storage: %u bytes\n", role,
                unsigned(sizeof(T)));
#else
  Serial.printf("On-chip %s storage: %u bytes in PSRAM\n", role,
                unsigned(sizeof(T)));
#endif
  return new (memory) T(std::forward<Args>(args)...);
}

template <class T> void releaseRoleStorage(T *&storage) {
  if (!storage)
    return;
  storage->~T();
  // Native identity, ACL secrets and decrypted packets share this lifetime.
  auto *bytes = reinterpret_cast<volatile uint8_t *>(storage);
  for (size_t i = 0; i < sizeof(T); ++i)
    bytes[i] = 0;
#ifdef NRF52_PLATFORM
  free(storage);
#else
  heap_caps_free(storage);
#endif
  storage = nullptr;
}
} // namespace onchip
