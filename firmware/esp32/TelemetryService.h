// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stddef.h>
#if defined(ARDUINO_ARCH_ESP32) && defined(ONCHIP_BOT_HTTPS) && ONCHIP_BOT_HTTPS
#include "RadioDashboard.h"
class WifiKissMultiplexer;
namespace mesh { class MainBoard; }
namespace onchip {
void telemetryLoop(RadioDashboard &dashboard, const RadioDashboard::RadioStatus &radio,
                   WifiKissMultiplexer &mux, mesh::MainBoard &board);
void telemetryCommand(const char *text, char *reply, size_t capacity);
}
#else
#include <stdio.h>
namespace onchip {
inline void telemetryCommand(const char *, char *reply, size_t capacity) {
  snprintf(reply, capacity, "Error: telemetry requires the ESP32 native HTTPS profile");
}
}
#endif
