// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#ifdef ARDUINO_ARCH_ESP32
#include <esp_heap_caps.h>
#endif
#ifdef ONCHIP_BOT_HEAP_MODEL
extern void onchipBotVmHeapModel(size_t oldSize, size_t newSize);
#endif

struct lua_State;
namespace onchip {
struct VmHeap {
  enum AllocationFailure { NoAllocationFailure, HeapLimit, AllocatorFailure };
  size_t live = 0;
  AllocationFailure allocationFailure = NoAllocationFailure;
  void (*parserStep)(lua_State *, VmHeap &) = nullptr;
#ifdef NRF52_PLATFORM
  size_t failedOldSize = 0, failedSize = 0;
  unsigned failedFree = 0;
#endif
  template<class Size>
  void *resize(void *pointer, size_t oldSize, size_t size, size_t limit, Size &peak) {
    if (!pointer) oldSize = 0;
    if (!size) {
#ifdef ONCHIP_BOT_HEAP_MODEL
      if (pointer) onchipBotVmHeapModel(oldSize, 0);
#endif
#ifdef ARDUINO_ARCH_ESP32
      heap_caps_free(pointer);
#else
      free(pointer);
#endif
      live -= oldSize;
      return nullptr;
    }
    if (size > limit || live - oldSize > limit - size) {
      allocationFailure = HeapLimit; return nullptr;
    }
#ifdef NRF52_PLATFORM
    const auto freeBytes = dbgHeapFree();
    const unsigned available = freeBytes > 0 ? unsigned(freeBytes) : 0;
    if (size > oldSize && size - oldSize + 8192u > available) {
      allocationFailure = AllocatorFailure;
      failedOldSize = oldSize; failedSize = size; failedFree = available;
      return nullptr;
    }
#endif
#ifdef ARDUINO_ARCH_ESP32
    void *replacement = heap_caps_realloc(pointer, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    void *replacement = realloc(pointer, size);
#endif
    if (replacement) {
#ifdef ONCHIP_BOT_HEAP_MODEL
      onchipBotVmHeapModel(oldSize, size);
#endif
      live = live - oldSize + size;
      if (live > peak) peak = Size(live);
    } else {
      allocationFailure = AllocatorFailure;
#ifdef NRF52_PLATFORM
      failedOldSize = oldSize; failedSize = size;
      const auto remaining = dbgHeapFree();
      failedFree = remaining > 0 ? unsigned(remaining) : 0;
#endif
    }
    return replacement;
  }
};
} // namespace onchip
