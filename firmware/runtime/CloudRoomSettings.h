// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

namespace onchip {
struct CloudRoomAliasSettings {
  char host[128], address[16], ca[4097], token[257], id[65], name[33];
  uint8_t publicKey[32];
};
struct CloudRoomSettings {
  uint8_t magic[4], enabled, count, reserved[2];
  CloudRoomAliasSettings aliases[2];
  uint8_t digest[32];
};
static_assert(sizeof(CloudRoomAliasSettings) == 4628, "Cloud room alias layout changed");
static_assert(sizeof(CloudRoomSettings) == 9296, "Cloud room settings layout changed");

inline bool cloudRoomSettingsFields(const CloudRoomSettings &settings) {
  const auto zeros = [](const void *data, size_t size) {
    const auto *bytes = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < size; ++i) if (bytes[i]) return false;
    return true;
  };
  const auto text = [&](const char *value, size_t capacity, bool required, bool pem = false) {
    const char *end = static_cast<const char *>(memchr(value, 0, capacity));
    if (!end || (required && end == value) ||
        !zeros(end, capacity - size_t(end - value))) return false;
    for (const char *p = value; p != end; ++p)
      if ((uint8_t(*p) < 32 || uint8_t(*p) > 126) &&
          !(pem && (*p == '\n' || *p == '\r'))) return false;
    return true;
  };
  const auto identifier = [&](const char *value) {
    if (!text(value, 65, true)) return false;
    for (; *value; ++value)
      if (!((*value >= 'a' && *value <= 'z') || (*value >= 'A' && *value <= 'Z') ||
            (*value >= '0' && *value <= '9') || *value == '_' || *value == '-')) return false;
    return true;
  };
  if (memcmp(settings.magic, "CRC\1", 4) || settings.enabled > 1 ||
      settings.count < 1 || settings.count > 2 || !zeros(settings.reserved, 2)) return false;
  for (unsigned i = 0; i < 2; ++i) {
    const auto &alias = settings.aliases[i];
    if (i >= settings.count) {
      if (!zeros(&alias, sizeof(alias))) return false;
      continue;
    }
    if (!text(alias.host, sizeof(alias.host), true) ||
        !text(alias.address, sizeof(alias.address), false) ||
        !text(alias.ca, sizeof(alias.ca), true, true) ||
        !text(alias.token, sizeof(alias.token), true) ||
        !identifier(alias.id) || !text(alias.name, sizeof(alias.name), true) ||
        zeros(alias.publicKey, 32)) return false;
    bool allff = true;
    for (uint8_t b : alias.publicKey) allff &= b == 255;
    if (allff) return false;
    const char *host = alias.host;
    size_t label = 0;
    if (*host == '-') return false;
    for (size_t p = 0; host[p]; ++p) {
      const char c = host[p];
      if (c == '.') {
        if (!label || label > 63 || host[p - 1] == '-') return false;
        label = 0;
      } else {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || (c == '-' && label))) return false;
        ++label;
      }
    }
    if (!label || label > 63 || host[strlen(host) - 1] == '-') return false;
    if (alias.address[0]) {
      const char *p = alias.address;
      for (unsigned part = 0; part < 4; ++part) {
        const char *start = p;
        unsigned value = 0, digits = 0;
        while (*p >= '0' && *p <= '9') {
          value = value * 10 + unsigned(*p++ - '0');
          if (++digits > 3 || value > 255) return false;
        }
        if (!digits || (digits > 1 && *start == '0') ||
            (part < 3 ? *p++ != '.' : *p != 0)) return false;
      }
    }
    for (const char *p = alias.token; *p; ++p) if (*p == ' ') return false;
    constexpr char begin[] = "-----BEGIN CERTIFICATE-----";
    constexpr char end[] = "-----END CERTIFICATE-----";
    if (strncmp(alias.ca, begin, sizeof(begin) - 1) ||
        !strstr(alias.ca, end)) return false;
    for (unsigned previous = 0; previous < i; ++previous) {
      const auto &other = settings.aliases[previous];
      if (!memcmp(other.publicKey, alias.publicKey, 32) ||
          (!strcasecmp(other.host, alias.host) && !strcmp(other.id, alias.id))) return false;
    }
  }
  return true;
}
} // namespace onchip
