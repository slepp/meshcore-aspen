// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "BotHttps.h"
#ifndef ONCHIP_BOT_HTTPS_SELF_TEST
#define ONCHIP_BOT_HTTPS_SELF_TEST 0
#endif
namespace onchip {
#if ONCHIP_BOT_HTTPS_SELF_TEST && defined(ARDUINO_ARCH_ESP32)
bool botHttpsProbeReady();
bool runBotHttpsProbe(BotHttpsTransport &, const BotHttpsConfig &, const uint8_t[32],
                     const std::atomic<bool> &);
#else
inline bool botHttpsProbeReady() { return true; }
#endif
}
