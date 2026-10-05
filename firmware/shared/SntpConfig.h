// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <cstring>
#include <cstdlib>
#include <cstdio>

#ifndef ONCHIP_SNTP_SERVER
#define ONCHIP_SNTP_SERVER "pool.ntp.org"
#endif
#ifndef ONCHIP_SNTP_INTERVAL_SECONDS
#define ONCHIP_SNTP_INTERVAL_SECONDS 3600
#endif
static_assert(ONCHIP_SNTP_INTERVAL_SECONDS >= 60 && ONCHIP_SNTP_INTERVAL_SECONDS <= 86400,
              "Initial SNTP interval must be 60..86400 seconds");

namespace radio_time {
struct SntpConfig {
  uint32_t interval = ONCHIP_SNTP_INTERVAL_SECONDS;
  char server[64] = ONCHIP_SNTP_SERVER;
};
inline bool validServer(const char *server) {
  const size_t size = strlen(server);
  if (!size || size >= sizeof(SntpConfig::server)) return false;
  for (size_t i = 0; i < size; ++i) {
    const char c = server[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '.')) return false;
    if (c == '.' && (!i || i + 1 == size || server[i - 1] == '.')) return false;
  }
  return server[0] != '-' && server[size - 1] != '-';
}
inline bool validConfig(const SntpConfig &config) {
  return memchr(config.server, 0, sizeof(config.server)) &&
      (!config.server[0] || validServer(config.server)) &&
      config.interval >= 60 && config.interval <= 86400;
}
inline bool editConfig(SntpConfig &config, const char *command) {
  if (!strncmp(command, "set sntp.server ", 16)) {
    const char *value = command + 16;
    if (!strcmp(value, "off")) config.server[0] = 0;
    else {
      if (!validServer(value)) return false;
      memset(config.server, 0, sizeof(config.server));
      strcpy(config.server, value);
    }
    return true;
  }
  if (!strncmp(command, "set sntp.interval ", 18)) {
    const char *value = command + 18;
    if (!*value) return false;
    for (const char *p = value; *p; ++p)
      if (*p < '0' || *p > '9') return false;
    if (strlen(value) > 5) return false;
    const uint32_t seconds = strtoul(value, nullptr, 10);
    if (seconds < 60 || seconds > 86400) return false;
    config.interval = seconds;
    return true;
  }
  return false;
}
} // namespace radio_time
