// SPDX-License-Identifier: Apache-2.0
#if defined(MESHCORE_ONCHIP_BOT) && MESHCORE_ONCHIP_BOT && ONCHIP_BOT_WASM
#include "WamrRuntime.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef ONCHIP_BOT_VM_TEST
extern uint64_t onchipBotVmTestClock();
#endif
#ifdef ARDUINO_ARCH_ESP32
#include <esp_heap_caps.h>
#include <esp_timer.h>
#endif

namespace onchip::wamr {
uint64_t nowUs() {
#ifdef ONCHIP_BOT_VM_TEST
  return onchipBotVmTestClock();
#elif defined(ARDUINO_ARCH_ESP32)
  return esp_timer_get_time();
#else
  return std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}
#ifdef ARDUINO_ARCH_ESP32
namespace {
void *allocatePool(size_t bytes) { return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
void releasePool(void *pool) { heap_caps_free(pool); }
}
#endif
bool ensure(const char *module, NativeSymbol *symbols, uint32_t count, char *error, size_t capacity) {
  hwamr::Config config;
  config.poolBytes = PoolBytes;
#ifdef ARDUINO_ARCH_ESP32
  config.allocate = allocatePool; config.release = releasePool;
#endif
  return hwamr::configure(config, error, capacity) && hwamr::ensure(module, symbols, count, error, capacity);
}
} // namespace onchip::wamr
#endif
