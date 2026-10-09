// SPDX-License-Identifier: Apache-2.0
#include "../SyslogCodec.h"
#include <assert.h>
#include <limits.h>

int main() {
  using namespace onchip;
  SyslogConfig config;
  assert(config.valid() && !config.enabled);
  assert(parseSyslogDestination("10.16.1.35", config) && config.port == 514);
  assert(config.enabled && config.address[3] == 35);
  assert(parseSyslogDestination("10.16.1.35:65535", config) && config.port == 65535);
  const auto before = config;
  const char *invalid[] = {"", "off ", "host:514", "10.1.2", "10.1.2.3.4", "10.1.2.256",
    "10.1.2.-1", "10.1.2.3:", "10.1.2.3:0", "10.1.2.3:65536", "10.1.2.3:514x",
    " 10.1.2.3", "10.1.2.3\n", "10.1.2.3:99999999999999", "0.0.0.0", "224.1.2.3", "255.255.255.255"};
  for (const char *text : invalid) {
    assert(!parseSyslogDestination(text, config));
    assert(!memcmp(&before, &config, sizeof(config)));
  }
  assert(parseSyslogDestination("off", config) && !config.enabled && config.valid());
  config.version = 2;
  assert(!config.valid());
  config = SyslogConfig{};
  config.enabled = 2;
  assert(!config.valid());
  char packet[512];
  const auto format = [&](const char *host, const char *message, size_t capacity = sizeof(packet)) {
    return formatSyslog(host, DiagnosticSubsystem::Wifi, 1791563896, 12345, message, packet, capacity);
  };
  const auto size = format("aspen-123", "WiFi lost\nreason=3\r\001");
  assert(size == strlen(packet));
  assert(!strcmp(packet, "<134>Oct  9 16:38:16 aspen-123 wifi: uptime_ms=12345 utc=1791563896 WiFi lost reason=3"));
  assert(!format("bad host", "test"));
  assert(!format("aspen", ""));
  assert(!format("aspen", "test", 4));
  char message[DiagnosticMessageCapacity];
  memset(message, 'x', sizeof(message) - 1);
  message[sizeof(message) - 1] = 0;
  assert(format("aspen", message));
  assert(formatSyslog("aspen", DiagnosticSubsystem::System, 0, UINT64_MAX,
                      "Boot roles=7", packet, sizeof(packet)));
  assert(!strcmp(packet, "<134>system: host=aspen uptime_ms=18446744073709551615 clock=unsynced Boot roles=7"));
  assert(formatSyslog("aspen", DiagnosticSubsystem::Bot, 1767225600, 0,
                      "Bot audit phase=started", packet, sizeof(packet)));
  assert(strstr(packet, "Jan  1 00:00:00 aspen bot:"));
  assert(!formatSyslog("aspen", DiagnosticSubsystem(255), 0, 0, "test", packet, sizeof(packet)));
  const char *tags[] = {"system", "wifi", "clock", "companion", "bot", "bot-vm",
                        "telemetry", "packet", "backup"};
  for (unsigned i = 0; i < sizeof(tags) / sizeof(*tags); ++i)
    assert(!strcmp(syslogSubsystemName(DiagnosticSubsystem(i)), tags[i]));
  SyslogRateLimit rate;
  for (unsigned i = 0; i < 8; ++i) assert(rate.admit(10));
  assert(!rate.admit(999));
  assert(rate.admit(1000));
  assert(rate.admit(UINT32_MAX - 10));
  assert(rate.admit(1000));
  puts("Syslog: retained IPv4 record, single-line BSD tags, UTC/unsynced timestamps, bounded audit records and rate limit passed");
}
