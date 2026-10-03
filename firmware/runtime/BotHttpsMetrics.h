// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <algorithm>
#include <stdint.h>

#ifndef ONCHIP_BOT_HTTPS_METRICS
#define ONCHIP_BOT_HTTPS_METRICS 0
#endif

namespace onchip {
struct BotHttpsSample {
  uint32_t at = 0, internal = 0, internalLargest = 0, internalGlobalMin = 0;
  uint32_t dma = 0, dmaLargest = 0, dmaGlobalMin = 0;
  unsigned sockets = 0, socketErrors = 0;
};
struct BotHttpsMeasurement {
  BotHttpsSample before{}, minimum{}, after{};
  unsigned samples = 0, peakSockets = 0, socketErrors = 0;
  uint32_t maxGapMs = 0;
  void record(const BotHttpsSample &sample) {
    if (!samples) before = minimum = sample;
    else maxGapMs = std::max(maxGapMs, uint32_t(sample.at - after.at));
    minimum.internal = std::min(minimum.internal, sample.internal);
    minimum.internalLargest = std::min(minimum.internalLargest, sample.internalLargest);
    minimum.dma = std::min(minimum.dma, sample.dma);
    minimum.dmaLargest = std::min(minimum.dmaLargest, sample.dmaLargest);
    peakSockets = std::max(peakSockets, sample.sockets);
    socketErrors += sample.socketErrors;
    after = sample;
    ++samples;
  }
};

#if ONCHIP_BOT_HTTPS_METRICS && defined(ARDUINO_ARCH_ESP32)
void startBotHttpsMetrics();
void sampleBotHttpsMetrics();
void finishBotHttpsMetrics();
void phaseBotHttpsMetrics(const char *phase);
#else
inline void startBotHttpsMetrics() {}
inline void sampleBotHttpsMetrics() {}
inline void finishBotHttpsMetrics() {}
inline void phaseBotHttpsMetrics(const char *) {}
#endif
} // namespace onchip
