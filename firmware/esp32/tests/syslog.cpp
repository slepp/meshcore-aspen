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
  char packet[256];
  const auto size = formatSyslog("aspen-123", "WiFi lost\nreason=3\r\001", packet, sizeof(packet));
  assert(size == strlen(packet));
  assert(!strcmp(packet, "<134>1 - aspen-123 meshcore - - - WiFi lost reason=3"));
  assert(!formatSyslog("bad host", "test", packet, sizeof(packet)));
  assert(!formatSyslog("aspen", "", packet, sizeof(packet)));
  assert(!formatSyslog("aspen", "test", packet, 4));
  char message[160];
  memset(message, 'x', sizeof(message) - 1);
  message[159] = 0;
  assert(formatSyslog("aspen", message, packet, sizeof(packet)));
  SyslogRateLimit rate;
  for (unsigned i = 0; i < 8; ++i) assert(rate.admit(10));
  assert(!rate.admit(999));
  assert(rate.admit(1000));
  assert(rate.admit(UINT32_MAX - 10));
  assert(rate.admit(1000));
  puts("Syslog: bounded IPv4 config, record validation, single-line RFC5424 and rate limit passed");
}
