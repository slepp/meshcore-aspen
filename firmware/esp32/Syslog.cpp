// SPDX-License-Identifier: Apache-2.0
#include "Syslog.h"
#if defined(ARDUINO_ARCH_ESP32) && defined(MESHCORE_MAST_ADMIN) && MESHCORE_MAST_ADMIN
#include "SyslogCodec.h"
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
std::atomic<uint32_t> submitted{0}, failed{0}, limited{0}, offline{0};
SyslogRateLimit rate;
struct UdpSend {
  tcpip_api_call_data call;
  SyslogConfig config;
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
      IP_ADDR4(&address, send.config.address[0], send.config.address[1],
               send.config.address[2], send.config.address[3]);
      result = udp_sendto(pcb, buffer, &address, send.config.port);
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
  bool present = false;
  const bool ready = mastRecord("syslog", &config, sizeof(config), false, present) && config.valid();
  publishConfig(ready ? config : SyslogConfig{}, ready);
  if (!ready) Serial.println("Syslog settings unavailable; use set syslog IP:PORT|off to repair");
  return ready;
}
bool syslogCommand(const char *command, char *reply, size_t capacity) {
  if (!strcmp(command, "get syslog")) {
    SyslogConfig config;
    if (!snapshot(config)) snprintf(reply, capacity, "Error: syslog settings unavailable; use set syslog IP:PORT|off");
    else if (!config.enabled) snprintf(reply, capacity, "Syslog off");
    else snprintf(reply, capacity, "Syslog UDP %u.%u.%u.%u:%u",
                  config.address[0], config.address[1], config.address[2], config.address[3], config.port);
  } else if (!strcmp(command, "get syslog.stats")) {
    snprintf(reply, capacity, "UDP submitted=%u failed=%u limited=%u offline=%u; since boot",
             submitted.load(), failed.load(), limited.load(), offline.load());
  } else if (!strncmp(command, "set syslog ", 11)) {
    SyslogConfig config, actual;
    bool present = false;
    if (!parseSyslogDestination(command + 11, config)) {
      snprintf(reply, capacity, "Error: use set syslog IP[:PORT]|off; unicast IPv4; port 1..65535");
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
      snprintf(reply, capacity, "Error: syslog is off or unavailable; use set syslog IP:PORT");
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
  const size_t size = formatSyslog(radio_network::hostname, subsystem, utc, uptimeMs,
                                 message, packet, sizeof(packet));
  if (!size) { ++failed; return; }
  UdpSend send{};
  send.config = config;
  send.message = packet;
  send.size = size;
  const auto result = tcpip_api_call(udpSend, &send.call);
  if (result == ERR_OK) ++submitted;
  else ++failed;
}
} // namespace onchip
#endif
