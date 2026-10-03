#pragma once
#include <cstdint>
using BaseType_t = int;
using TickType_t = uint32_t;
constexpr int pdTRUE = 1, pdFALSE = 0, pdPASS = 1;
constexpr TickType_t portMAX_DELAY = UINT32_MAX;
inline TickType_t pdMS_TO_TICKS(uint32_t value) { return value; }
