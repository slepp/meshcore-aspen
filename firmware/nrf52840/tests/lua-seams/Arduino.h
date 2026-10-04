// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../seams/Arduino.h"
#include <chrono>
#include <thread>
#include <mutex>
#include <cstdint>
struct StaticSemaphore_t { std::recursive_mutex mutex; };
using SemaphoreHandle_t = StaticSemaphore_t *;
constexpr unsigned portMAX_DELAY = UINT32_MAX;
inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *value) { return value; }
inline bool xSemaphoreTake(SemaphoreHandle_t value, unsigned) { value->mutex.lock(); return true; }
inline void xSemaphoreGive(SemaphoreHandle_t value) { value->mutex.unlock(); }
inline void taskENTER_CRITICAL() {}
inline void taskEXIT_CRITICAL() {}
inline uint32_t millis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}
inline void delay(unsigned value) { std::this_thread::sleep_for(std::chrono::milliseconds(value)); }
