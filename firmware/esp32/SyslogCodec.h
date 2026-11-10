// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "Syslog.h"
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>

namespace onchip {
struct LegacySyslogConfig {
  uint8_t version = 1, enabled = 0, address[4]{};
  uint16_t port = 514;
  bool valid() const {
    return version == 1 && enabled <= 1 && port &&
           (!enabled || (address[0] && address[0] < 224));
  }
};
static_assert(sizeof(LegacySyslogConfig) == 8, "Legacy syslog NVS layout changed");
struct SyslogConfig {
  uint8_t version = 2, enabled = 0;
  uint16_t port = 514;
  char host[254]{};
  bool valid() const {
    if (version != 2 || enabled > 1 || !port || !memchr(host, 0, sizeof(host))) return false;
    if (!enabled) return true;
    const size_t n = strlen(host);
    if (!n || host[0] == '-' || host[n - 1] == '-') return false;
    size_t label = 0;
    for (size_t i = 0; i < n; ++i) {
      const char c = host[i];
      if (c == '.') {
        if (!label || host[i - 1] == '-') return false;
        label = 0;
      } else {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || (c == '-' && label)) || ++label > 63) return false;
      }
    }
    if (!label) return false;
    // Numeric destinations remain usable when reading an older installation.
    bool numeric = true;
    for (const char *p = host; *p; ++p) numeric &= (*p >= '0' && *p <= '9') || *p == '.';
    if (numeric) {
      unsigned octets[4]; char extra;
      if (sscanf(host, "%u.%u.%u.%u%c", &octets[0], &octets[1], &octets[2], &octets[3], &extra) != 4 ||
          !octets[0] || octets[0] >= 224 || octets[1] > 255 || octets[2] > 255 || octets[3] >= 255) return false;
    }
    return true;
  }
};
inline bool parseSyslogDestination(const char *text, SyslogConfig &config) {
  SyslogConfig next;
  if (!strcmp(text, "off")) { config = next; return true; }
  const char *colon = strchr(text, ':');
  const size_t n = colon ? size_t(colon - text) : strlen(text);
  if (!n || n >= sizeof(next.host)) return false;
  memcpy(next.host, text, n);
  if (colon) {
    const char *p = colon + 1;
    unsigned value = 0, digits = 0;
    while (*p >= '0' && *p <= '9') {
      value = value * 10 + unsigned(*p++ - '0');
      if (++digits > 5 || value > 65535) return false;
    }
    if (*p || !digits || !value) return false;
    next.port = uint16_t(value);
  }
  next.enabled = 1;
  if (!next.valid()) return false;
  config = next;
  return true;
}
inline bool upgradeSyslog(const LegacySyslogConfig &old, SyslogConfig &config) {
  if (!old.valid()) return false;
  config = {};
  config.enabled = old.enabled;
  config.port = old.port;
  if (old.enabled) snprintf(config.host, sizeof(config.host), "%u.%u.%u.%u",
                           old.address[0], old.address[1], old.address[2], old.address[3]);
  return config.valid();
}

inline const char *syslogSubsystemName(DiagnosticSubsystem subsystem) {
  static const char *const names[] = {"system", "wifi", "clock", "companion", "bot",
                                    "bot-vm", "telemetry", "packet", "backup"};
  const unsigned index = unsigned(subsystem);
  return index < sizeof(names) / sizeof(*names) ? names[index] : nullptr;
}

inline size_t formatSyslog(const char *host, DiagnosticSubsystem subsystem,
                           uint32_t utc, uint64_t uptimeMs, const char *message,
                           char *output, size_t capacity) {
  const auto *tag = syslogSubsystemName(subsystem);
  if (!output || !capacity || !tag) return 0;
  if (!host || !*host || strlen(host) > 32 || !message || !*message) return 0;
  for (const unsigned char *p = reinterpret_cast<const unsigned char *>(host); *p; ++p)
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
          (*p >= '0' && *p <= '9') || *p == '-')) return 0;
  char timestamp[16]{};
  if (utc) {
    const time_t seconds = utc;
    struct tm time{};
    if (!gmtime_r(&seconds, &time) || time.tm_mon < 0 || time.tm_mon > 11) return 0;
    static const char *const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                       "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    snprintf(timestamp, sizeof(timestamp), "%s %2d %02d:%02d:%02d",
             months[time.tm_mon], time.tm_mday, time.tm_hour, time.tm_min, time.tm_sec);
  }
  // Before a trusted clock is available, the receiver supplies the BSD header.
  const int header = utc ?
      snprintf(output, capacity, "<134>%s %s %s: uptime_ms=%" PRIu64 " utc=%u ",
               timestamp, host, tag, uptimeMs, utc) :
      snprintf(output, capacity, "<134>%s: host=%s uptime_ms=%" PRIu64 " clock=unsynced ",
               tag, host, uptimeMs);
  if (header < 0 || size_t(header) >= capacity) return 0;
  size_t used = size_t(header);
  for (const unsigned char *p = reinterpret_cast<const unsigned char *>(message); *p; ++p) {
    if (used + 1 >= capacity) return 0;
    output[used++] = *p >= 32 && *p <= 126 ? char(*p) : ' ';
  }
  while (used > size_t(header) && output[used - 1] == ' ') --used;
  output[used] = 0;
  return used > size_t(header) ? used : 0;
}

class SyslogRateLimit {
public:
  bool admit(uint32_t now) {
    if (uint32_t(now - window_) >= 1000) { window_ = now; used_ = 0; }
    if (used_ == 8) return false;
    ++used_;
    return true;
  }
private:
  uint32_t window_ = 0;
  uint8_t used_ = 0;
};
} // namespace onchip
