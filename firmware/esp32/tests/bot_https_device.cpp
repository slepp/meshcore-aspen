// SPDX-License-Identifier: Apache-2.0
#include "BotHttps.h"
#include "TelemetryEndpoint.h"
#include "Clock.h"
#include "Config.h"
#include <WiFiClientSecure.h>
#include <mbedtls/pem.h>
#include <mbedtls/ssl_internal.h>
#include <esp_heap_caps.h>
#include <sys/time.h>
#include <cassert>
#include <cstring>
#include <memory>

#ifdef MBEDTLS_HAVE_TIME_DATE
#error This harness must not rely on SDK date verification
#endif
using namespace onchip;
namespace sdk = device_tls_test;
static uint32_t now;
static constexpr size_t RecordRequests = MBEDTLS_SSL_IN_BUFFER_LEN + MBEDTLS_SSL_OUT_BUFFER_LEN;
static constexpr size_t RecordOverhead =
    device_heap_test::charge(MBEDTLS_SSL_IN_BUFFER_LEN) +
    device_heap_test::charge(MBEDTLS_SSL_OUT_BUFFER_LEN) - RecordRequests;
static constexpr size_t RemainingWork = BotHttpsWorkBudgetBytes - RecordRequests;
static constexpr size_t LargestRecord = std::max(MBEDTLS_SSL_IN_BUFFER_LEN, MBEDTLS_SSL_OUT_BUFFER_LEN);
static unsigned busySnapshots;
bool onchipClockTestBusy() {
  if (!busySnapshots) return false;
  --busySnapshots;
  return true;
}
static void (*notification)(struct timeval *);
unsigned long millis() { return now; }
void delay(unsigned long ms) { now += ms; }
void vTaskDelay(unsigned ticks) { now += ticks; }
void sntp_set_time_sync_notification_cb(void (*callback)(struct timeval *)) { notification = callback; }
void sntp_set_sync_interval(uint32_t) {}
void configTime(long offset, int daylight, const char *server) {
  assert(!offset && !daylight && server[0]);
}
static void reset(bool trusted = true) {
  now = 0;
  busySnapshots = 0;
  sdk::reset();
  assert(device_heap_test::allocations.empty() && !device_heap_test::used);
  device_heap_test::free = 200000;
  device_heap_test::largest = 100000; device_heap_test::recoverAt = 0;
  device_heap_test::layout.clear(); device_heap_test::remaining.clear();
  device_heap_test::minimum = SIZE_MAX; device_heap_test::failAllocation = -1;
  beginClocks();
  beginNetworkClock(true);
  if (trusted) {
    timeval sample{};
    sample.tv_sec = 1767225600; // 2026-01-01T00:00:00Z.
    assert(notification);
    notification(&sample);
    loopClocks();
  }
}
static BotIoResult query() {
  BotHttpsConfig config{"192.0.2.1", "home.example",
      "-----BEGIN CERTIFICATE-----\nfixture\n-----END CERTIFICATE-----",
      "fixture-fixture-fixture-fixture-fixture", 443, 1};
  std::unique_ptr<BotHttpsTransport> transport(createBotHttpsTransport());
  assert(transport);
  BotHttps provider(config, *transport);
  BotIoRequest request;
  request.kind = BotIoRequest::Rpc; request.token = {1, 1, 1};
  request.deadline = now + BotHttpsDeadlineMs; request.grant = 1; request.principal[0] = 1;
  strcpy(request.key, "health");
  std::atomic<uint32_t> generation{1}, grant{1};
  std::atomic<bool> enabled{true}, stopping{false};
  BotIoResult result;
  provider.perform(request, result, generation, grant, enabled, stopping);
  assert(sdk::parses == sdk::frees);
  assert(device_heap_test::allocations.empty() && !device_heap_test::used);
  return result;
}
static void rejectedBeforeCredentials(bool beforeConnect = false) {
  const auto result = query();
  assert(!result.ok && result.error[0] && result.rpcCode[0]);
  assert(sdk::connections == unsigned(!beforeConnect) && sdk::writes == 0 &&
         sdk::stops == unsigned(!beforeConnect));
}
static void telemetryReuseAndHeapPressure() {
  reset();
  TelemetryEndpoint endpoint;
  strcpy(endpoint.address, "192.0.2.1");
  strcpy(endpoint.host, "home.example");
  strcpy(endpoint.path, "/write");
  strcpy(endpoint.ca, "-----BEGIN CERTIFICATE-----\nfixture\n-----END CERTIFICATE-----");
  std::unique_ptr<BotHttpsTransport> transport(createBotHttpsTransport());
  assert(transport);
  std::atomic<bool> cancelled{false}, stopping{false};
  const char body[] = "meshcore_device,device=native-test heap_free_bytes=111764i\n";
  const auto post = [&] {
    TelemetryCompletion result;
    performTelemetryPost(*transport, endpoint, body, sizeof(body) - 1,
                         now + BotHttpsDeadlineMs, cancelled, stopping, result);
    assert(sdk::parses == sdk::frees);
    assert(device_heap_test::allocations.empty() && !device_heap_test::used);
    return result;
  };
  for (unsigned i = 1; i <= 4; ++i) {
    const auto result = post();
    assert(result.ok && result.httpStatus == 200);
    assert(result.tls.point == BotHttpsFailurePoint::None && result.tls.sdk == 0);
    assert(result.tls.before == 200000 && result.tls.after == 200000);
    assert(result.tls.cipher == 0xc02f && result.tls.peerCount == 2 && result.tls.peerBytes == 2);
    assert(result.tls.caCached == (i > 1) && sdk::parses == 1 && sdk::frees == 1);
    assert(sdk::connections == i && sdk::stops == i);
  }

  // Both documented SDK record buffers fit; remaining work is an aggregate budget.
  device_heap_test::free = 111764;
  device_heap_test::largest = 53236;
  device_heap_test::layout = {53236, 50000, 8528};
  for (unsigned i = 0; i < 3; ++i) {
    const auto result = post();
    assert(result.ok && result.httpStatus == 200 && now == 0);
  }
  assert(device_heap_test::minimum >= BotHttpsRadioReserveBytes);
  device_heap_test::layout = {53236, 16000, 16000, 16000, 10528};
  for (unsigned i = 0; i < 3; ++i) {
    const auto result = post();
    assert(result.ok && result.httpStatus == 200 && now == 0);
  }
  assert(device_heap_test::minimum >= RemainingWork + BotHttpsRadioReserveBytes);
  const unsigned connections = sdk::connections, writes = sdk::writes, stops = sdk::stops;
  // The first buffer fits in 33000 bytes, but no fragment can hold the second.
  device_heap_test::largest = 33000;
  device_heap_test::layout = {33000, 16000, 16000, 16000, 16000, 14764};
  for (unsigned i = 0; i < 3; ++i) {
    const auto result = post();
    assert(!result.ok && !result.httpStatus && result.error == TelemetryError::Heap);
    assert(result.tls.point == BotHttpsFailurePoint::AdmissionOutput && !result.tls.sdk);
    assert(sdk::connections == connections && sdk::writes == writes && sdk::stops == stops);
    loopClocks();
  }

  // Recovery uses this same provider without a reboot or configuration edit.
  device_heap_test::layout.clear();
  for (auto point : {BotHttpsFailurePoint::AdmissionTotal, BotHttpsFailurePoint::AdmissionBlock,
                     BotHttpsFailurePoint::AdmissionInput, BotHttpsFailurePoint::AdmissionReserve}) {
    device_heap_test::free = point == BotHttpsFailurePoint::AdmissionTotal ?
        BotHttpsWorkBudgetBytes + BotHttpsRadioReserveBytes - 1 :
        point == BotHttpsFailurePoint::AdmissionReserve ?
        BotHttpsWorkBudgetBytes + BotHttpsRadioReserveBytes + RecordOverhead - 1 : 111764;
    device_heap_test::largest = point == BotHttpsFailurePoint::AdmissionBlock ?
                               LargestRecord - 1 : 100000;
    device_heap_test::failAllocation = point == BotHttpsFailurePoint::AdmissionInput ? 0 : -1;
    const auto result = post();
    assert(!result.ok && result.error == TelemetryError::Heap && result.tls.point == point);
    assert(!result.tls.sdk && !result.tls.connected && result.tls.failure == device_heap_test::free);
    assert(sdk::connections == connections && sdk::writes == writes && sdk::stops == stops);
    loopClocks();
  }
  device_heap_test::failAllocation = -1;
  device_heap_test::free = 111764;
  device_heap_test::largest = 102388;
  assert(post().ok && sdk::connections == connections + 1 && sdk::stops == stops + 1);
  sdk::connectResult = false;
  assert(!post().ok && sdk::connections == connections + 2 && sdk::stops == stops + 2);
  sdk::connectResult = true;
  assert(post().ok && sdk::connections == connections + 3 && sdk::stops == stops + 3);
  transport->close();
  transport->close();
  assert(sdk::stops == stops + 3);
  for (int code : {MBEDTLS_ERR_SSL_ALLOC_FAILED, MBEDTLS_ERR_X509_ALLOC_FAILED,
                   MBEDTLS_ERR_PK_ALLOC_FAILED, MBEDTLS_ERR_PEM_ALLOC_FAILED,
                   MBEDTLS_ERR_ECP_ALLOC_FAILED, MBEDTLS_ERR_DHM_ALLOC_FAILED,
                   MBEDTLS_ERR_MD_ALLOC_FAILED, MBEDTLS_ERR_CIPHER_ALLOC_FAILED,
                   MBEDTLS_ERR_MPI_ALLOC_FAILED, MBEDTLS_ERR_ASN1_ALLOC_FAILED,
                   -0x2180 + MBEDTLS_ERR_ASN1_ALLOC_FAILED,
                   -0x4280 + MBEDTLS_ERR_MPI_ALLOC_FAILED}) {
    sdk::connectResult = false; sdk::connectError = code;
    const unsigned beforeWrites = sdk::writes, beforeStops = sdk::stops;
    const auto result = post();
    assert(!result.ok && result.error == TelemetryError::Heap && !result.httpStatus);
    assert(result.tls.point == BotHttpsFailurePoint::Connect && result.tls.sdk == code);
    assert(sdk::writes == beforeWrites && sdk::stops == beforeStops + 1);
  }
  sdk::connectResult = true;
  sdk::parseResult = MBEDTLS_ERR_X509_ALLOC_FAILED;
  // Same endpoint storage address, different bundle bytes must invalidate the cache.
  strcat(endpoint.ca, "\n");
  const auto beforeCaConnections = sdk::connections;
  const auto caAllocation = post();
  assert(!caAllocation.ok && caAllocation.error == TelemetryError::Heap);
  assert(!caAllocation.tls.caCached && sdk::connections == beforeCaConnections);
  assert(caAllocation.tls.point == BotHttpsFailurePoint::CaParse &&
         caAllocation.tls.sdk == MBEDTLS_ERR_X509_ALLOC_FAILED);
  sdk::parseResult = 0;
  assert(post().ok);
  const unsigned beforeReserveWrites = sdk::writes;
  sdk::afterConnect = [] { device_heap_test::free = BotHttpsRadioReserveBytes - 1; };
  const auto reserve = post();
  assert(!reserve.ok && reserve.error == TelemetryError::Heap && !reserve.httpStatus);
  assert(reserve.tls.point == BotHttpsFailurePoint::ConnectedReserve &&
         reserve.tls.failure == BotHttpsRadioReserveBytes - 1);
  assert(reserve.tls.cipher == 0xc02f && reserve.tls.peerCount == 2);
  assert(sdk::writes == beforeReserveWrites);
  sdk::afterConnect = nullptr;
  device_heap_test::free = 200000;
  sdk::onRead = [] { device_heap_test::free = BotHttpsRadioReserveBytes - 1; };
  const auto ioReserve = post();
  assert(!ioReserve.ok && ioReserve.error == TelemetryError::Heap &&
         ioReserve.tls.point == BotHttpsFailurePoint::IoReserve);
  sdk::onRead = nullptr;
  device_heap_test::free = 200000;
  const auto recovered = post();
  assert(recovered.ok && recovered.tls.point == BotHttpsFailurePoint::None &&
         !recovered.tls.sdk && !recovered.tls.failure && recovered.tls.connected == 200000);
  // Cached trust-anchor dates must still be rechecked against a fresh clock.
  uint32_t earliest, latest;
  loopClocks();
  assert(trustedNetworkTime(earliest, latest));
  sdk::root.valid_to = {2026, 1, 1, 0, 0, int(latest - 1767225600 + 2)};
  strcat(endpoint.ca, "\n");
  assert(post().ok);
  now += 4000; loopClocks();
  const auto expired = post();
  assert(!expired.ok && expired.tls.caCached &&
         expired.tls.point == BotHttpsFailurePoint::CertificateTime && !expired.tls.connected);
  cancelled = true;
  const auto cancelledResult = post();
  assert(cancelledResult.error == TelemetryError::Cancelled && !cancelledResult.tls.connected &&
         !cancelledResult.tls.cipher && !cancelledResult.tls.peerCount);
  puts("PASS native telemetry SDK pattern: {53236,16000,16000,16000,10528} repeatedly admitted; "
       "unavailable second record rejected; aggregate workspace/radio reserve, actual SDK OOM typing and cleanup");
}
static void caWorkspaceLifetime() {
  reset();
  device_heap_test::free = 111764;
  device_heap_test::largest = 53236;
  device_heap_test::layout = {53236, 50000, 8528};
  sdk::parseBytes = 6000;
  sdk::connectionBytes[0] = MBEDTLS_SSL_IN_BUFFER_LEN;
  sdk::connectionBytes[1] = MBEDTLS_SSL_OUT_BUFFER_LEN;
  sdk::connectionBytes[2] = 43000;
  assert(query().ok);
  const size_t connectedFree = 111764 - device_heap_test::charge(sdk::connectionBytes[0]) -
      device_heap_test::charge(sdk::connectionBytes[1]) - device_heap_test::charge(sdk::connectionBytes[2]);
  assert(device_heap_test::minimum == connectedFree && connectedFree >= BotHttpsRadioReserveBytes);
  assert(connectedFree - device_heap_test::charge(sdk::parseBytes) < BotHttpsRadioReserveBytes);
  assert(psram_test::allocations.empty());
  reset();
  // Cache allocation is optional; do not convert PSRAM cache pressure into a TLS failure.
  psram_test::failAfter = 1; // BotHttps workspace succeeds; CA cache cannot be retained.
  assert(query().ok && sdk::parses == 1 && sdk::frees == 1);
  psram_test::failAfter = -1;
  assert(psram_test::allocations.empty());
  reset();
  sdk::root.next = &sdk::root;
  rejectedBeforeCredentials(true);
  reset();
  sdk::root.raw.len = 16385;
  rejectedBeforeCredentials(true);
  reset();
  sdk::root.raw.p = nullptr;
  rejectedBeforeCredentials(true);
  puts("PASS CA date workspace precedes TLS context/records; cached dates stay clock-checked, "
       "changed CA invalidates cache, PSRAM cache pressure falls back to an uncached date parse");
}
int main() {
  telemetryReuseAndHeapPressure();
  caWorkspaceLifetime();
  reset(false);
  assert(!botHttpsClockTrusted());
  companionClock().setCurrentTime(1790600400);
  auto result = query();
  assert(!result.ok && !strcmp(result.rpcCode, "tls_policy") && !sdk::connections && !sdk::writes);

  reset();
  assert(botHttpsClockTrusted());
  result = query();
  assert(result.ok && sdk::connections == 1 && sdk::writes && !strcmp(result.value, "ok"));
  reset(); sdk::connectResult = false; sdk::connectError = MBEDTLS_ERR_SSL_ALLOC_FAILED;
  result = query();
  assert(!result.ok && !strcmp(result.rpcCode, "heap_unavailable") &&
         strstr(result.error, "SDK -32512") && !sdk::writes && sdk::stops == 1);
  reset(); sdk::connectResult = false; sdk::connectError = -0x2700;
  result = query();
  assert(!result.ok && !strcmp(result.rpcCode, "transport_error") &&
         strstr(result.error, "SDK -9984") && !sdk::writes && sdk::stops == 1);
  for (unsigned target = 0; target < 3; ++target) {
    reset();
    auto &certificate = target == 0 ? sdk::leaf : target == 1 ? sdk::intermediate : sdk::root;
    certificate.valid_to = {2025, 12, 31, 23, 59, 59};
    rejectedBeforeCredentials(target == 2);
    reset();
    auto &future = target == 0 ? sdk::leaf : target == 1 ? sdk::intermediate : sdk::root;
    future.valid_from = {2026, 1, 2, 0, 0, 0};
    rejectedBeforeCredentials(target == 2);
  }
  reset(); sdk::peer = nullptr;
  rejectedBeforeCredentials();
  reset(); sdk::intermediate.next = &sdk::leaf;
  rejectedBeforeCredentials();
  reset(); sdk::leaf.valid_to = {2026, 2, 29, 0, 0, 0};
  rejectedBeforeCredentials();
  reset(); sdk::root.valid_from = {2028, 1, 1, 0, 0, 0};
  rejectedBeforeCredentials(true);
  reset(); sdk::parseResult = 1;
  rejectedBeforeCredentials(true);
  reset(); sdk::leaf.valid_to = {2026, 1, 1, 0, 0, 0};
  rejectedBeforeCredentials();
  reset(); sdk::leaf.valid_from = {2026, 1, 1, 0, 0, 1};
  rejectedBeforeCredentials();

  reset(); now = 3001;
  result = query();
  assert(!result.ok && !strcmp(result.rpcCode, "tls_policy") && !sdk::connections);
  reset(); now = ONCHIP_SNTP_INTERVAL_SECONDS * 2000u + 1; loopClocks();
  result = query();
  assert(!result.ok && !strcmp(result.rpcCode, "tls_policy") && !sdk::connections);
  reset();
  sdk::afterConnect = [] { now = 3001; };
  rejectedBeforeCredentials();

  reset();
  sdk::leaf.valid_to = {2026, 1, 1, 0, 0, 2};
  sdk::onRead = [] { sdk::onRead = nullptr; now += 4000; loopClocks(); };
  result = query();
  assert(!result.ok && !strcmp(result.rpcCode, "unknown") && sdk::writes > 0);
  assert(strstr(result.error, "expired"));
  reset();
  sdk::onRead = [] { sdk::onRead = nullptr; now += 3001; };
  result = query();
  assert(!result.ok && !strcmp(result.rpcCode, "unknown") && sdk::writes > 0);
  assert(strstr(result.error, "SNTP"));

  reset();
  sdk::leaf.valid_from = {2024, 2, 29, 0, 0, 0};
  assert(query().ok);
  reset(); busySnapshots = 1;
  assert(query().ok && now == 1);
  reset(); busySnapshots = 4;
  result = query();
  assert(!result.ok && strstr(result.error, "publication busy") && !sdk::connections && now == 3);
  reset();
  device_heap_test::largest = LargestRecord - 1; device_heap_test::recoverAt = 300;
  assert(query().ok && now == 300 && sdk::connections == 1);
  reset();
  device_heap_test::largest = 61428;
  result = query();
  assert(result.ok && sdk::connections == 1 && now == 0);
  reset();
  device_heap_test::free = 111764;
  device_heap_test::largest = device_heap_test::charge(LargestRecord);
  assert(query().ok && now == 0 && sdk::connections == 1);
  reset();
  device_heap_test::free = BotHttpsWorkBudgetBytes + BotHttpsRadioReserveBytes +
                          RecordOverhead;
  device_heap_test::largest = 64 * 1024;
  assert(query().ok && now == 0 && sdk::connections == 1);
  assert(device_heap_test::minimum == RemainingWork + BotHttpsRadioReserveBytes);
  reset();
  device_heap_test::free = BotHttpsWorkBudgetBytes + BotHttpsRadioReserveBytes - 1;
  result = query();
  assert(!result.ok && !strcmp(result.rpcCode, "heap_unavailable") && !sdk::connections && now == 1000);
  reset();
  device_heap_test::largest = LargestRecord - 1;
  result = query();
  assert(!result.ok && !strcmp(result.rpcCode, "heap_unavailable") && !sdk::connections && now == 1000);
  for (int allocation : {0, 1}) {
    reset();
    device_heap_test::failAllocation = allocation;
    result = query();
    assert(!result.ok && !strcmp(result.rpcCode, "heap_unavailable") && !sdk::connections && now == 1000);
    assert(device_heap_test::allocations.empty() && !device_heap_test::used);
  }
  reset();
  device_heap_test::failAllocation = 2;
  result = query();
  assert(!result.ok && !strcmp(result.rpcCode, "heap_unavailable") && !sdk::connections && now == 1000);
  reset();
  device_heap_test::free = BotHttpsWorkBudgetBytes + BotHttpsRadioReserveBytes +
                          RecordOverhead - 1;
  result = query();
  assert(!result.ok && !strcmp(result.rpcCode, "heap_unavailable") && !sdk::connections && now == 1000);
  reset();
  device_heap_test::free = BotHttpsWorkBudgetBytes + BotHttpsRadioReserveBytes - 1;
  device_heap_test::recoverAt = 300;
  assert(query().ok && now == 300 && sdk::connections == 1);
  reset(); now = 2501;
  device_heap_test::largest = LargestRecord - 1;
  result = query();
  assert(!result.ok && strstr(result.error, "publication stale") && !sdk::connections && now == 3001);
  assert(psram_test::allocations.empty());
  puts("PASS production ESP SecureTransport path: SNTP-only clock, expired/future leaf/intermediate/CA, "
       "missing/invalid chain, mid-request validity loss, bounded snapshot/admission recovery; "
       "SDK boundary shim, not hardware acceptance");
}
