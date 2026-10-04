// SPDX-License-Identifier: Apache-2.0
#if NRFMAST_PRODUCTION_LUA
#include <Arduino.h>
#ifdef USE_TINYUSB
#include <Adafruit_TinyUSB.h>
#endif

void initVariant() __attribute__((weak));
void initVariant() {}
static void dispatch(void *) {
#ifdef USE_TINYUSB
  TinyUSB_Device_Init(0);
#endif
  setup();
  while (true) {
    loop();
    yield();
    vTaskDelay(1);
    if (serialEvent && serialEventRun) serialEventRun();
  }
}
int main() {
  init();
  initVariant();
  // nRF FreeRTOS stack arguments are words, unlike the ESP32 port's bytes.
  if (xTaskCreate(dispatch, "loop", 2560, nullptr, TASK_PRIO_NORMAL, nullptr) != pdPASS)
    NVIC_SystemReset();
  ada_callback_init(768);
  vTaskStartScheduler();
  NVIC_SystemReset();
}
#endif
