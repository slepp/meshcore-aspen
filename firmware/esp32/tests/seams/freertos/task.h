#pragma once
#include "FreeRTOS.h"
inline int xTaskCreate(void (*)(void *), const char *, uint32_t, void *,
                       unsigned, void *) {
  return pdPASS;
}
