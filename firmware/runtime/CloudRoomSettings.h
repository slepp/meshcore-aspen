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

// Version 2 appends role, trust and lifecycle fields to every alias. The writer emits
// version 1 whenever the settings can be expressed there, so a node that never links
// through the lifecycle verbs keeps a record the 0.1.19 loader reads.
struct CloudRoomAliasSettingsV2 {
  CloudRoomAliasSettings base;
  uint8_t role, caMode, flags, reserved;
};
struct CloudRoomSettingsV2 {
  uint8_t magic[4], enabled, count, reserved[2];
  CloudRoomAliasSettingsV2 aliases[2];
  uint8_t digest[32];
};
static_assert(sizeof(CloudRoomAliasSettingsV2) == 4632, "Cloud room v2 alias layout changed");
static_assert(sizeof(CloudRoomSettingsV2) == 9304, "Cloud room v2 settings layout changed");
constexpr uint8_t CloudRoleFrontend = 0, CloudRoleOwner = 1;
constexpr uint8_t CloudCaSystem = 0, CloudCaPinned = 1;
constexpr uint8_t CloudFlagAdvertOnce = 1, CloudFlagConverted = 2;

namespace cloud_room_detail {
inline bool zeros(const void *data, size_t size) {
  const auto *bytes = static_cast<const uint8_t *>(data);
  for (size_t i = 0; i < size; ++i) if (bytes[i]) return false;
  return true;
}
inline bool text(const char *value, size_t capacity, bool required, bool pem = false) {
  const char *end = static_cast<const char *>(memchr(value, 0, capacity));
  if (!end || (required && end == value) ||
      !zeros(end, capacity - size_t(end - value))) return false;
  for (const char *p = value; p != end; ++p)
    if ((uint8_t(*p) < 32 || uint8_t(*p) > 126) &&
        !(pem && (*p == '\n' || *p == '\r'))) return false;
  return true;
}
inline bool identifier(const char *value) {
  if (!text(value, 65, true)) return false;
  for (; *value; ++value)
    if (!((*value >= 'a' && *value <= 'z') || (*value >= 'A' && *value <= 'Z') ||
          (*value >= '0' && *value <= '9') || *value == '_' || *value == '-')) return false;
  return true;
}
// Fields shared by both record versions. `systemRoots` aliases carry no CA text.
inline bool alias(const CloudRoomAliasSettings &alias, bool systemRoots) {
  if (!text(alias.host, sizeof(alias.host), true) ||
      !text(alias.address, sizeof(alias.address), false) ||
      !text(alias.ca, sizeof(alias.ca), !systemRoots, true) ||
      !text(alias.token, sizeof(alias.token), true) ||
      !identifier(alias.id) || !text(alias.name, sizeof(alias.name), true) ||
      zeros(alias.publicKey, 32) || (systemRoots && !zeros(alias.ca, sizeof(alias.ca)))) return false;
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
  if (!systemRoots) {
    constexpr char begin[] = "-----BEGIN CERTIFICATE-----";
    constexpr char end[] = "-----END CERTIFICATE-----";
    if (strncmp(alias.ca, begin, sizeof(begin) - 1) || !strstr(alias.ca, end)) return false;
  }
  return true;
}
inline bool distinct(const CloudRoomAliasSettings &a, const CloudRoomAliasSettings &b) {
  return memcmp(a.publicKey, b.publicKey, 32) &&
         (strcasecmp(a.host, b.host) || strcmp(a.id, b.id));
}
} // namespace cloud_room_detail

// Version 1 field rules, unchanged since 0.1.19.
inline bool cloudRoomSettingsFields(const CloudRoomSettings &settings) {
  using namespace cloud_room_detail;
  if (memcmp(settings.magic, "CRC\1", 4) || settings.enabled > 1 ||
      settings.count < 1 || settings.count > 2 || !zeros(settings.reserved, 2)) return false;
  for (unsigned i = 0; i < 2; ++i) {
    const auto &entry = settings.aliases[i];
    if (i >= settings.count) {
      if (!zeros(&entry, sizeof(entry))) return false;
      continue;
    }
    if (!alias(entry, false)) return false;
    for (unsigned previous = 0; previous < i; ++previous)
      if (!distinct(settings.aliases[previous], entry)) return false;
  }
  return true;
}

// Version 2 field rules: the version 1 checks plus role, trust mode and flags.
inline bool cloudRoomSettingsFieldsV2(const CloudRoomSettingsV2 &settings) {
  using namespace cloud_room_detail;
  if (memcmp(settings.magic, "CRC\2", 4) || settings.enabled > 1 ||
      settings.count > 2 || !zeros(settings.reserved, 2)) return false;
  for (unsigned i = 0; i < 2; ++i) {
    const auto &entry = settings.aliases[i];
    if (i >= settings.count) {
      if (!zeros(&entry, sizeof(entry))) return false;
      continue;
    }
    if (entry.role > CloudRoleOwner || entry.caMode > CloudCaPinned || entry.reserved ||
        (entry.flags & ~(CloudFlagAdvertOnce | CloudFlagConverted)) ||
        ((entry.flags & CloudFlagConverted) && entry.role != CloudRoleOwner) ||
        entry.base.publicKey[0] == 0 || entry.base.publicKey[0] == 255 ||
        !alias(entry.base, entry.caMode == CloudCaSystem)) return false;
    for (unsigned previous = 0; previous < i; ++previous)
      if (!distinct(settings.aliases[previous].base, entry.base)) return false;
  }
  return true;
}

// True when the settings fit a version 1 record, which older firmware also loads.
inline bool cloudRoomFitsV1(const CloudRoomSettingsV2 &settings) {
  if (settings.count < 1) return false;
  for (unsigned i = 0; i < settings.count; ++i) {
    const auto &entry = settings.aliases[i];
    if (entry.role != CloudRoleFrontend || entry.caMode != CloudCaPinned || entry.flags ||
        entry.reserved) return false;
  }
  return true;
}
// Widens a validated version 1 record in place; `settings` must span sizeof(V2) bytes.
// The digest field keeps the version 1 value until the next encode.
inline void cloudRoomWiden(CloudRoomSettingsV2 &settings) {
  auto *bytes = reinterpret_cast<uint8_t *>(&settings);
  constexpr size_t head = offsetof(CloudRoomSettings, aliases), one = sizeof(CloudRoomAliasSettings);
  constexpr size_t wide = sizeof(CloudRoomAliasSettingsV2);
  memmove(bytes + head + 2 * wide, bytes + head + 2 * one, 32);
  memmove(bytes + head + wide, bytes + head + one, one);
  memset(bytes + head + one, 0, wide - one);
  memset(bytes + head + wide + one, 0, wide - one);
  memcpy(settings.magic, "CRC\2", 4);
  for (unsigned i = 0; i < settings.count; ++i) settings.aliases[i].caMode = CloudCaPinned;
}
// Copies version 2 settings that fit version 1 into `out`; the caller computes the digest.
inline void cloudRoomNarrow(const CloudRoomSettingsV2 &settings, CloudRoomSettings &out) {
  memset(&out, 0, sizeof(out));
  memcpy(out.magic, "CRC\1", 4);
  out.enabled = settings.enabled; out.count = settings.count;
  for (unsigned i = 0; i < settings.count; ++i) out.aliases[i] = settings.aliases[i].base;
}
} // namespace onchip
