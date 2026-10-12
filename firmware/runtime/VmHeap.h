// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <hlua_core.hpp>
#ifdef ARDUINO_ARCH_ESP32
#include <esp_heap_caps.h>
#endif
#ifdef ONCHIP_BOT_HEAP_MODEL
extern void onchipBotVmHeapModel(size_t oldSize, size_t newSize);
#endif

struct lua_State;
namespace onchip {
struct VmHeap : hlua::Heap {
#ifdef NRF52_PLATFORM
  size_t failedOldSize = 0, failedSize = 0;
  unsigned failedFree = 0;
#endif
  VmHeap() { reallocate = platformReallocate; }
  static void *platformReallocate(hlua::Heap &owner, void *pointer, size_t oldSize, size_t size) {
#ifdef NRF52_PLATFORM
    auto &heap = static_cast<VmHeap &>(owner);
    const auto freeBytes = dbgHeapFree();
    const unsigned available = freeBytes > 0 ? unsigned(freeBytes) : 0;
    if (size > oldSize && size - oldSize + 8192u > available) {
      heap.failedOldSize = oldSize; heap.failedSize = size; heap.failedFree = available;
      return nullptr;
    }
#endif
#ifdef ARDUINO_ARCH_ESP32
    void *replacement = size ? heap_caps_realloc(pointer, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : nullptr;
    if (!size) heap_caps_free(pointer);
#else
    void *replacement = size ? realloc(pointer, size) : nullptr;
    if (!size) free(pointer);
#endif
#ifdef ONCHIP_BOT_HEAP_MODEL
    if (replacement || (!size && pointer)) onchipBotVmHeapModel(oldSize, size);
#endif
#ifdef NRF52_PLATFORM
    if (size && !replacement) {
      heap.failedOldSize = oldSize; heap.failedSize = size;
      const auto remaining = dbgHeapFree();
      heap.failedFree = remaining > 0 ? unsigned(remaining) : 0;
    }
#endif
    return replacement;
  }
};
} // namespace onchip
