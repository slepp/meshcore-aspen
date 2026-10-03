// SPDX-License-Identifier: Apache-2.0
#include "BotHttpsProbe.h"
#if ONCHIP_BOT_HTTPS_SELF_TEST && defined(ARDUINO_ARCH_ESP32)
#include "Clock.h"
#include "BotHttpsMetrics.h"
#include <Arduino.h>
#include <WiFi.h>
#include <esp_system.h>
#include <freertos/task.h>
#include <cstring>

namespace onchip {
namespace {
std::atomic<bool> passed{false};
bool rejectedDate(const char *error) {
  return strstr(error, "certificate chain expired") || strstr(error, "certificate dates") ||
         strstr(error, "CA dates");
}
}
bool botHttpsProbeReady() { return passed.load(); }
bool runBotHttpsProbe(BotHttpsTransport &transport, const BotHttpsConfig &configured,
                     const uint8_t identity[32], const std::atomic<bool> &stopping) {
  uint8_t mac[6]{};
  const uint8_t expected[] = {0x10, 0xb4, 0x1d, 0xe9, 0x31, 0x90};
  if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK || memcmp(mac, expected, sizeof(mac))) {
    Serial.println("HTTPS PROBE FAIL: spare31:90 hardware guard");
    return false;
  }
  char error[128]{};
  if (botHttpsClockTrusted() || transport.validate(error, sizeof(error)) || !strstr(error, "SNTP")) {
    Serial.println("HTTPS PROBE FAIL: did not observe untrusted boot clock");
    return false;
  }
  Serial.println("HTTPS PROBE PASS: actual untrusted boot clock denied without a connection");
  const uint32_t start = millis();
  uint32_t reported = start;
  while (!botHttpsClockTrusted() && !stopping && uint32_t(millis() - start) < 90000) {
    if (uint32_t(millis() - reported) >= 10000) {
      reported = millis();
      Serial.printf("HTTPS PROBE WAIT: WiFi=%u IP=%s heap=%u\n", unsigned(WiFi.status()),
          WiFi.localIP().toString().c_str(), ESP.getFreeHeap());
    }
    vTaskDelay(10);
  }
  if (!botHttpsClockTrusted() || stopping) {
    Serial.println("HTTPS PROBE FAIL: fresh SNTP unavailable");
    return false;
  }
  // Allow the Make-owned harness to attach two native TCP clients after boot.
  const uint32_t ready = millis();
  while (!stopping && uint32_t(millis() - ready) < 30000) vTaskDelay(10);
  if (stopping) return false;
  std::atomic<uint32_t> generation{1}, grant{1};
  std::atomic<bool> enabled{true};
  BotHttps provider(configured, transport);
  bool servicesPassed = true;
  for (const char *operation : {"health", "echo"}) {
    Serial.printf("HTTPS PROBE START: service-%s\n", operation);
    BotIoRequest request;
    request.kind = BotIoRequest::Rpc;
    request.token = {1, 1, 1};
    if (!strcmp(operation, "echo")) request.token.operation = 2;
    request.grant = 1; request.deadline = millis() + BotHttpsDeadlineMs;
    memcpy(request.principal, identity, 32);
    strcpy(request.key, operation);
    if (!strcmp(operation, "echo")) strcpy(request.value, "esp32-native-https-probe");
    BotIoResult result;
    provider.perform(request, result, generation, grant, enabled, stopping);
    const bool ok = result.ok && result.httpStatus == 200 &&
        !strcmp(result.value, !strcmp(operation, "health") ? "ok" : request.value);
    Serial.printf("HTTPS PROBE %s: service-%s status=%u%s%s stack=%u\n",
        ok ? "PASS" : "FAIL", operation, unsigned(result.httpStatus),
        result.error[0] ? " -- " : "", result.error, unsigned(uxTaskGetStackHighWaterMark(nullptr)));
    servicesPassed = servicesPassed && ok;
  }
  const uint16_t ports[] = {8788, 8789, 8790, 8791};
  const char *names[] = {"expired-leaf", "future-leaf", "expired-intermediate", "future-intermediate"};
  bool certificatesPassed = true;
  for (unsigned index = 0; index < 4 && !stopping; ++index) {
    auto config = configured;
    config.port = ports[index];
    error[0] = 0;
    Serial.printf("HTTPS PROBE START: %s\n", names[index]);
    const bool opened = transport.open(config, error, sizeof(error));
    transport.close();
    const bool ok = !opened && rejectedDate(error);
    Serial.printf("HTTPS PROBE %s: %s%s%s\n", ok ? "PASS" : "FAIL", names[index],
                  error[0] ? " -- " : "", error);
    certificatesPassed = certificatesPassed && ok;
  }
  if (stopping || !certificatesPassed || !servicesPassed) {
    Serial.println("HTTPS PROBE COMPLETE: certificate suite failed; grant remains unavailable");
    return false;
  }
  passed = true;
  Serial.println("HTTPS PROBE PASS: native certificate/time and reference service complete; Lua grant unchanged");
  return true;
}
}
#endif
