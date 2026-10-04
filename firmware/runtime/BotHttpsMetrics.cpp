// SPDX-License-Identifier: Apache-2.0
#include "BotHttpsMetrics.h"
#if ONCHIP_BOT_HTTPS_METRICS && defined(ARDUINO_ARCH_ESP32)
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/sockets.h>
#include <errno.h>

namespace onchip {
namespace {
portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
BotHttpsMeasurement measurement;
bool active = false;
uint32_t epoch = 0;

BotHttpsSample capture() {
  BotHttpsSample sample;
  sample.at = millis();
  constexpr unsigned internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  constexpr unsigned dma = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT;
  sample.internal = heap_caps_get_free_size(internal);
  sample.internalLargest = heap_caps_get_largest_free_block(internal);
  sample.internalGlobalMin = heap_caps_get_minimum_free_size(internal);
  sample.dma = heap_caps_get_free_size(dma);
  sample.dmaLargest = heap_caps_get_largest_free_block(dma);
  sample.dmaGlobalMin = heap_caps_get_minimum_free_size(dma);
  for (int fd = LWIP_SOCKET_OFFSET; fd < LWIP_SOCKET_OFFSET + CONFIG_LWIP_MAX_SOCKETS; ++fd) {
    int type = 0;
    socklen_t length = sizeof(type);
    if (!lwip_getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &length)) ++sample.sockets;
    else if (errno != EBADF && errno != ENOTSOCK) ++sample.socketErrors;
  }
  return sample;
}
}
void startBotHttpsMetrics() {
  const auto sample = capture();
  portENTER_CRITICAL(&lock);
  measurement = {};
  measurement.record(sample);
  ++epoch;
  active = true;
  portEXIT_CRITICAL(&lock);
}
void sampleBotHttpsMetrics() {
  const auto now = millis();
  portENTER_CRITICAL(&lock);
  const bool due = active && uint32_t(now - measurement.after.at) >= 10;
  const uint32_t current = epoch;
  portEXIT_CRITICAL(&lock);
  if (!due) return;
  const auto sample = capture();
  portENTER_CRITICAL(&lock);
  if (active && epoch == current && int32_t(sample.at - measurement.after.at) >= 0)
    measurement.record(sample);
  portEXIT_CRITICAL(&lock);
}
void finishBotHttpsMetrics() {
  const auto sample = capture();
  portENTER_CRITICAL(&lock);
  measurement.record(sample);
  active = false;
  const auto result = measurement;
  portEXIT_CRITICAL(&lock);
  Serial.printf("HTTPS samples: start=%u end=%u count=%u max-gap-ms=%u sockets=%u/%u/%u socket-errors=%u\n",
      unsigned(result.before.at), unsigned(result.after.at), result.samples, unsigned(result.maxGapMs),
      result.before.sockets, result.peakSockets, result.after.sockets, result.socketErrors);
  Serial.printf("HTTPS internal: before=%u sampled-min=%u after=%u largest-min=%u global-min=%u/%u\n",
      unsigned(result.before.internal), unsigned(result.minimum.internal), unsigned(result.after.internal),
      unsigned(result.minimum.internalLargest), unsigned(result.before.internalGlobalMin),
      unsigned(result.after.internalGlobalMin));
  Serial.printf("HTTPS DMA: before=%u sampled-min=%u after=%u largest-min=%u global-min=%u/%u\n",
      unsigned(result.before.dma), unsigned(result.minimum.dma), unsigned(result.after.dma),
      unsigned(result.minimum.dmaLargest), unsigned(result.before.dmaGlobalMin),
      unsigned(result.after.dmaGlobalMin));
}
void phaseBotHttpsMetrics(const char *phase) {
  multi_heap_info_t internal{}, dma{};
  heap_caps_get_info(&internal, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  heap_caps_get_info(&dma, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
  Serial.printf("HTTPS phase %s: internal-free=%u largest=%u allocated=%u blocks=%u free-blocks=%u "
                "dma-free=%u largest=%u allocated=%u blocks=%u stack=%u\n",
      phase, unsigned(internal.total_free_bytes), unsigned(internal.largest_free_block),
      unsigned(internal.total_allocated_bytes), unsigned(internal.allocated_blocks), unsigned(internal.free_blocks),
      unsigned(dma.total_free_bytes), unsigned(dma.largest_free_block),
      unsigned(dma.total_allocated_bytes), unsigned(dma.allocated_blocks),
      unsigned(uxTaskGetStackHighWaterMark(nullptr)));
}
} // namespace onchip
#endif
