// SPDX-License-Identifier: Apache-2.0
#include "Syslog.h"
#if defined(ARDUINO_ARCH_ESP32) && defined(MESHCORE_MAST_ADMIN) && MESHCORE_MAST_ADMIN
#include "SyslogCodec.h"
#include "NetworkDns.h"
#include <nvs.h>
#include "Runtime.h"
#include "RadioNetwork.h"
#include <Arduino.h>
#include <WiFi.h>
#include <atomic>
#include <lwip/udp.h>
#include <lwip/priv/tcpip_priv.h>

namespace onchip {
bool mastRecord(const char *, void *, size_t, bool, bool &);
namespace {
SyslogConfig liveConfig;
bool configReady = false;
portMUX_TYPE configMux = portMUX_INITIALIZER_UNLOCKED;
std::atomic<uint32_t> submitted{0}, failed{0}, limited{0}, offline{0}, dnsFailed{0};
SyslogRateLimit rate;
struct UdpSend {
  tcpip_api_call_data call;
  uint32_t address;
  uint16_t port;
  const char *message;
  size_t size;
};
err_t udpSend(tcpip_api_call_data *call) {
  auto &send = *reinterpret_cast<UdpSend *>(call);
  auto *pcb = udp_new();
  if (!pcb) return ERR_MEM;
  auto *buffer = pbuf_alloc(PBUF_TRANSPORT, send.size, PBUF_RAM);
  err_t result = ERR_MEM;
  if (buffer) {
    result = pbuf_take(buffer, send.message, send.size);
    if (result == ERR_OK) {
      ip_addr_t address;
      ip_addr_set_ip4_u32(&address, send.address);
      result = udp_sendto(pcb, buffer, &address, send.port);
    }
    pbuf_free(buffer);
  }
  udp_remove(pcb);
  return result;
}
void publishConfig(const SyslogConfig &config, bool ready) {
  portENTER_CRITICAL(&configMux);
  liveConfig = config;
  configReady = ready;
  portEXIT_CRITICAL(&configMux);
}
bool snapshot(SyslogConfig &config) {
  portENTER_CRITICAL(&configMux);
  config = liveConfig;
  const bool ready = configReady;
  portEXIT_CRITICAL(&configMux);
  return ready;
}
} // namespace
bool beginSyslog() {
  SyslogConfig config;
  nvs_handle_t handle;
  const auto opened = nvs_open("mc-mast-admin", NVS_READONLY, &handle);
  bool ready = opened == ESP_ERR_NVS_NOT_FOUND;
  if (opened == ESP_OK) {
    size_t size = 0;
    const auto result = nvs_get_blob(handle, "syslog", nullptr, &size);
    ready = result == ESP_ERR_NVS_NOT_FOUND;
    if (result == ESP_OK && size == sizeof(config))
      ready = nvs_get_blob(handle, "syslog", &config, &size) == ESP_OK && config.valid();
    else if (result == ESP_OK && size == sizeof(LegacySyslogConfig)) {
      LegacySyslogConfig old;
      ready = nvs_get_blob(handle, "syslog", &old, &size) == ESP_OK && upgradeSyslog(old, config);
    }
    nvs_close(handle);
  }
  publishConfig(ready ? config : SyslogConfig{}, ready);
  if (!ready) Serial.println("Syslog settings unavailable; use set syslog HOST:PORT|off to repair");
  return ready;
}
bool syslogCommand(const char *command, char *reply, size_t capacity) {
  if (!strcmp(command, "get syslog")) {
    SyslogConfig config;
    if (!snapshot(config)) snprintf(reply, capacity, "Error: syslog settings unavailable; use set syslog HOST:PORT|off");
    else if (!config.enabled) snprintf(reply, capacity, "Syslog off");
    else snprintf(reply, capacity, "Syslog UDP %s:%u", config.host, config.port);
  } else if (!strcmp(command, "get syslog.stats")) {
    snprintf(reply, capacity, "UDP submitted=%u failed=%u limited=%u offline=%u dns-failed=%u; since boot",
             submitted.load(), failed.load(), limited.load(), offline.load(), dnsFailed.load());
  } else if (!strncmp(command, "set syslog ", 11)) {
    SyslogConfig config, actual;
    bool present = false;
    if (!parseSyslogDestination(command + 11, config)) {
      snprintf(reply, capacity, "Error: use set syslog HOST[:PORT]|off; hostname or unicast IPv4; port 1..65535");
    } else if (!mastRecord("syslog", &config, sizeof(config), true, present) ||
               !mastRecord("syslog", &actual, sizeof(actual), false, present) ||
               !present || memcmp(&config, &actual, sizeof(config))) {
      snprintf(reply, capacity, "Error: syslog save/readback failed; inspect get syslog and retry");
    } else {
      publishConfig(config, true);
      diagnosticEvent(config.enabled ? "Syslog enabled" : "Syslog disabled");
      snprintf(reply, capacity, "OK - syslog saved and applied");
    }
  } else if (!strcmp(command, "syslog test")) {
    SyslogConfig config;
    if (!snapshot(config) || !config.enabled)
      snprintf(reply, capacity, "Error: syslog is off or unavailable; use set syslog HOST:PORT");
    else if (!diagnosticEvent("Syslog test"))
      snprintf(reply, capacity, "Error: diagnostics queue full or unavailable; inspect get diagnostics");
    else snprintf(reply, capacity, "Syslog test queued");
  } else return false;
  return true;
}
void sendSyslog(const char *message, DiagnosticSubsystem subsystem, uint32_t utc, uint64_t uptimeMs) {
  SyslogConfig config;
  if (!snapshot(config) || !config.enabled) return;
  if (WiFi.status() != WL_CONNECTED) { ++offline; return; }
  if (!rate.admit(millis())) { ++limited; return; }
  char packet[512];
  const size_t size = formatSyslog(radio_network::hostname(), subsystem, utc, uptimeMs,
                                 message, packet, sizeof(packet));
  if (!size) { ++failed; return; }
  static uint32_t retryAt = 0;
  if (retryAt && int32_t(millis() - retryAt) < 0) { ++failed; return; }
  IPAddress destination;
  char error[96];
  if (!resolveNetworkHost(config.host, destination, error, sizeof(error))) {
    retryAt = millis() + 30000; ++dnsFailed; ++failed; return;
  }
  retryAt = 0;
  UdpSend send{};
  send.address = uint32_t(destination);
  send.port = config.port;
  send.message = packet;
  send.size = size;
  const auto result = tcpip_api_call(udpSend, &send.call);
  if (result == ERR_OK) ++submitted;
  else ++failed;
}
} // namespace onchip
#endif
