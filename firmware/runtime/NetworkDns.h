// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/dns.h>
#include <lwip/tcpip.h>
#include <atomic>
#include <mutex>
#include <cstdio>
#include <cstring>

namespace onchip {
namespace network_dns {
// lwIP owns DNS caching and honours record TTLs. Callback storage must outlive
// a caller's timeout: lwIP can still complete that lookup afterwards.
struct Lookup {
  std::mutex mutex;
  std::atomic<bool> done{true};
  bool pending = false, success = false, resetCache = false;
  char host[254]{};
  uint32_t address = 0, local = 0, gateway = 0, server = 0;
};
inline Lookup &lookup() { static Lookup state; return state; }
inline void complete(const char *, const ip_addr_t *address, void *context) {
  auto &state = *static_cast<Lookup *>(context);
  state.address = address && IP_IS_V4(address) ? ip4_addr_get_u32(ip_2_ip4(address)) : 0;
  state.success = state.address != 0;
  state.done.store(true, std::memory_order_release);
}
inline void start(void *context) {
  auto &state = *static_cast<Lookup *>(context);
  if (state.resetCache) dns_clear_cache();
  ip_addr_t address{};
  const auto result = dns_gethostbyname_addrtype(state.host, &address, complete, context, LWIP_DNS_ADDRTYPE_IPV4);
  if (result != ERR_INPROGRESS) complete(nullptr, result == ERR_OK ? &address : nullptr, context);
}
} // namespace network_dns

// Called from network workers, never the radio loop or lwIP thread. Failed
// lookups return within three seconds; late callbacks never touch a stack frame.
inline bool resolveNetworkHost(const char *host, IPAddress &address, char *error, size_t capacity) {
  if (!host || !*host || strnlen(host, 254) == 254) {
    snprintf(error, capacity, "DNS hostname is missing or too long"); return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    snprintf(error, capacity, "DNS unavailable while WiFi is offline"); return false;
  }
  auto &state = network_dns::lookup();
  const uint32_t started = millis();
  std::unique_lock<std::mutex> guard(state.mutex, std::defer_lock);
  // HTTPS and diagnostics share lwIP callback storage. Let a short lookup
  // finish instead of treating normal contention as a DNS outage. The same
  // deadline covers waiting for ownership, a late callback and our own query.
  while (!guard.try_lock()) {
    if (uint32_t(millis() - started) >= 3000) {
      snprintf(error, capacity, "DNS lookup timed out waiting for another query"); return false;
    }
    vTaskDelay(1);
  }
  while (state.pending && !state.done.load(std::memory_order_acquire)) {
    if (uint32_t(millis() - started) >= 3000) {
      snprintf(error, capacity, "DNS lookup timed out waiting for a late reply"); return false;
    }
    vTaskDelay(1);
  }
  const uint32_t local = WiFi.localIP(), gateway = WiFi.gatewayIP(), server = WiFi.dnsIP();
  state.resetCache = local != state.local || gateway != state.gateway || server != state.server;
  state.local = local; state.gateway = gateway; state.server = server;
  strcpy(state.host, host);
  state.success = false;
  state.done.store(false, std::memory_order_release);
  state.pending = true;
  if (tcpip_try_callback(network_dns::start, &state) != ERR_OK) {
    state.pending = false;
    state.local = state.gateway = state.server = 0;
    state.done.store(true, std::memory_order_release);
    snprintf(error, capacity, "DNS request queue is full"); return false;
  }
  while (!state.done.load(std::memory_order_acquire)) {
    if (uint32_t(millis() - started) >= 3000) {
      snprintf(error, capacity, "DNS lookup timed out"); return false;
    }
    vTaskDelay(1);
  }
  state.pending = false;
  if (!state.success) {
    snprintf(error, capacity, "DNS lookup returned no IPv4 address"); return false;
  }
  address = IPAddress(state.address);
  if (!address[0] || address[0] >= 224 || address[3] == 255) {
    snprintf(error, capacity, "DNS lookup returned an unusable IPv4 address"); return false;
  }
  return true;
}
} // namespace onchip
