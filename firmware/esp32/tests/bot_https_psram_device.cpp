// SPDX-License-Identifier: Apache-2.0
#include "BotHttps.h"
#include "Clock.h"
#include <WiFiClientSecure.h>
#include <mbedtls/ssl_internal.h>
#include <cassert>
#include <cstring>
#include <memory>

using namespace onchip;
namespace sdk = device_tls_test;
static uint32_t now;
static BotHttpsDiagnostics diagnostics;
static bool failOutputProbe;
static void (*notification)(struct timeval *);
bool onchipClockTestBusy() { return false; }
unsigned long millis() { return now; }
void delay(unsigned long ms) { now += ms; }
void vTaskDelay(unsigned ticks) {
  now += ticks;
  if (failOutputProbe) psram_test::failAfter = 1;
}
void sntp_set_time_sync_notification_cb(void (*callback)(struct timeval *)) { notification = callback; }
void sntp_set_sync_interval(uint32_t) {}
void configTime(long offset, int daylight, const char *server) {
  assert(!offset && !daylight && server[0]);
}
static void reset() {
  assert(device_heap_test::allocations.empty() && psram_test::allocations.empty());
  now = 0;
  failOutputProbe = false;
  sdk::reset();
  device_heap_test::free = 60000;
  device_heap_test::largest = 24000;
  device_heap_test::minimum = SIZE_MAX;
  device_heap_test::failAllocation = -1;
  device_heap_test::recoverAt = 0;
  device_heap_test::layout.clear();
  device_heap_test::remaining.clear();
  device_heap_test::externalFree = 8 * 1024 * 1024;
  device_heap_test::externalLargest = 4 * 1024 * 1024;
  psram_test::failAfter = -1;
  sdk::clientBytes = BotHttpsClientBudgetBytes;
  sdk::parseBytes = 2048;
  sdk::connectionBytes[0] = MBEDTLS_SSL_IN_BUFFER_LEN;
  sdk::connectionBytes[1] = MBEDTLS_SSL_OUT_BUFFER_LEN;
  sdk::connectionBytes[2] = 24000;
  beginClocks();
  beginNetworkClock(true);
  timeval sample{};
  sample.tv_sec = 1767225600;
  assert(notification);
  notification(&sample);
  loopClocks();
}
static BotIoResult query() {
  BotHttpsConfig config{"192.0.2.1", "home.example",
      "-----BEGIN CERTIFICATE-----\nfixture\n-----END CERTIFICATE-----",
      "fixture-fixture-fixture-fixture-fixture", 443, 1};
  std::unique_ptr<BotHttpsTransport> transport(createBotHttpsTransport());
  assert(transport);
  BotHttps provider(config, *transport);
  BotIoRequest request;
  request.kind = BotIoRequest::Rpc;
  request.token = {1, 1, 1};
  request.deadline = now + BotHttpsDeadlineMs;
  request.grant = 1;
  request.principal[0] = 1;
  strcpy(request.key, "health");
  std::atomic<uint32_t> generation{1}, grant{1};
  std::atomic<bool> enabled{true}, stopping{false};
  BotIoResult result;
  provider.perform(request, result, generation, grant, enabled, stopping);
  diagnostics = transport->diagnostics();
  return result;
}
static void clean() {
  assert(!device_heap_test::used && device_heap_test::allocations.empty());
  assert(psram_test::allocations.empty() && sdk::parses == sdk::frees);
  assert(!sdk::clients);
}
int main() {
  static_assert(ONCHIP_TLS_PSRAM, "Test the deployed PSRAM allocation policy");
  reset();
  assert(beginBotHttpsMemory() && device_tls_memory::setups == 1);
  void *old = heap_caps_malloc(128, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  assert(old);
  mbedtls_free(old);
  sdk::afterConnect = [] {
    assert(device_heap_test::used == device_heap_test::charge(BotHttpsClientBudgetBytes));
    assert(psram_test::allocations.size() >= 5);
  };
  const auto success = query();
  assert(success.ok && success.httpStatus == 200 && !strcmp(success.value, "ok"));
  assert(device_heap_test::minimum >= BotHttpsRadioReserveBytes);
  assert(sdk::connections == 1 && sdk::writes && sdk::stops == 1);
  clean();
  assert(device_tls_memory::setups == 1);

  reset();
  device_heap_test::free = BotHttpsRadioReserveBytes + BotHttpsClientBudgetBytes - 1;
  auto result = query();
  assert(!result.ok && !strcmp(result.rpcCode, "heap_unavailable"));
  assert(diagnostics.point == BotHttpsFailurePoint::AdmissionTotal && !sdk::connections);
  clean();

  reset();
  device_heap_test::largest = sizeof(sslclient_context) - 1;
  result = query();
  assert(!result.ok && diagnostics.point == BotHttpsFailurePoint::AdmissionBlock && !sdk::clients);
  clean();

  reset();
  device_heap_test::externalFree = BotHttpsWorkBudgetBytes - 1;
  result = query();
  assert(!result.ok && diagnostics.point == BotHttpsFailurePoint::AdmissionTotal && !sdk::connections);
  clean();

  reset();
  device_heap_test::externalLargest = MBEDTLS_SSL_IN_BUFFER_LEN - 1;
  result = query();
  assert(!result.ok && diagnostics.point == BotHttpsFailurePoint::AdmissionBlock && !sdk::connections);
  clean();

  reset();
  failOutputProbe = true;
  psram_test::failAfter = 2;
  result = query();
  assert(!result.ok && diagnostics.point == BotHttpsFailurePoint::AdmissionOutput && !sdk::connections);
  clean();

  reset();
  sdk::afterConnect = [] {
    device_heap_test::free = BotHttpsRadioReserveBytes - 1;
  };
  result = query();
  assert(!result.ok && diagnostics.point == BotHttpsFailurePoint::ConnectedReserve);
  assert(!sdk::writes && sdk::stops == 1);
  clean();

  reset();
  sdk::onRead = [] {
    device_heap_test::free = BotHttpsRadioReserveBytes - 1;
  };
  result = query();
  assert(!result.ok && diagnostics.point == BotHttpsFailurePoint::IoReserve && sdk::stops == 1);
  clean();
  assert(device_tls_memory::setups == 1);
}
