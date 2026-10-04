// SPDX-License-Identifier: Apache-2.0
#include "RoleProfile.h"
#include "Config.h"
#include <Arduino.h>
#include <nvs.h>
#include <string.h>

namespace onchip {
namespace {
constexpr char Namespace[] = "mc-onchip";
constexpr char Key[] = "role-profile";
constexpr uint8_t Magic[] = {'M', 'C', 'R', 'P'};
constexpr uint8_t Version = 1, JournalVersion = 2;
constexpr size_t RecordSize = 12, JournalSize = 28;

uint32_t checksum(const uint8_t *bytes, size_t length) {
  uint32_t value = 2166136261u;
  for (size_t i = 0; i < length; ++i)
    value = (value ^ bytes[i]) * 16777619u;
  return value;
}
void put64(uint8_t *out, uint64_t value) {
  for (unsigned i = 0; i < 8; ++i)
    out[i] = uint8_t(value >> (8 * i));
}
uint64_t get64(const uint8_t *in) {
  uint64_t value = 0;
  for (unsigned i = 0; i < 8; ++i)
    value |= uint64_t(in[i]) << (8 * i);
  return value;
}
void encode(uint8_t *record, size_t size, const ProfileJournal &journal) {
  memcpy(record, Magic, sizeof(Magic));
  record[4] = size == RecordSize ? Version : JournalVersion;
  record[5] = journal.profile.enabled;
  record[6] = record[7] = 0;
  if (size == JournalSize) {
    put64(record + 8, journal.generation);
    put64(record + 16, journal.nonce);
  }
  const size_t end = size - 4;
  const uint32_t check = checksum(record, end);
  for (unsigned i = 0; i < 4; ++i)
    record[end + i] = uint8_t(check >> (8 * i));
}
bool valid(const uint8_t *record, size_t size) {
  if (size != RecordSize && size != JournalSize)
    return false;
  const size_t end = size - 4;
  uint32_t check = 0;
  for (unsigned i = 0; i < 4; ++i)
    check |= uint32_t(record[end + i]) << (8 * i);
  return !memcmp(record, Magic, sizeof(Magic)) &&
         record[4] == (size == RecordSize ? Version : JournalVersion) &&
         RoleProfile{record[5]}.valid() && !record[6] && !record[7] &&
         (size == RecordSize || (get64(record + 8) && get64(record + 16))) &&
         check == checksum(record, end);
}
bool failure(const char *operation, esp_err_t result) {
  Serial.printf("On-chip role profile %s failed: %s\n", operation,
                esp_err_to_name(result));
  return false;
}
} // namespace

bool loadProfileJournal(ProfileJournal &journal) {
  journal = {};
#if defined(MESHCORE_PUBLIC_PROVISIONING) && MESHCORE_PUBLIC_PROVISIONING
  if (!publicProvisioningReady())
    return failure("public setup unavailable", ESP_ERR_INVALID_STATE);
  const uint8_t initialRoles = publicProvisioning().roles;
#else
  const uint8_t initialRoles = RoleProfile::All;
#endif
  nvs_handle_t handle;
  esp_err_t result = nvs_open(Namespace, NVS_READONLY, &handle);
  if (result == ESP_ERR_NVS_NOT_FOUND) {
    journal.profile.enabled = initialRoles;
    return true;
  }
  if (result != ESP_OK)
    return failure("open", result);
  uint8_t record[JournalSize]{};
  size_t size = sizeof(record);
  result = nvs_get_blob(handle, Key, record, &size);
  nvs_close(handle);
  if (result == ESP_ERR_NVS_NOT_FOUND) {
    journal.profile.enabled = initialRoles;
    return true;
  }
  if (result != ESP_OK)
    return failure("read", result);
  if (!valid(record, size))
    return failure("validation", ESP_ERR_INVALID_STATE);
  journal.profile.enabled = record[5];
  if (size == JournalSize) {
    journal.generation = get64(record + 8);
    journal.nonce = get64(record + 16);
  }
  return true;
}

bool loadRoleProfile(RoleProfile &profile) {
  ProfileJournal journal;
  const bool ok = loadProfileJournal(journal);
  profile = ok ? journal.profile : RoleProfile{0};
  return ok;
}

static bool writeProfile(const ProfileJournal &journal, size_t size) {
  if (!journal.profile.valid())
    return failure("validation", ESP_ERR_INVALID_ARG);
  uint8_t record[JournalSize]{};
  encode(record, size, journal);
  nvs_handle_t handle;
  esp_err_t result = nvs_open(Namespace, NVS_READWRITE, &handle);
  if (result != ESP_OK)
    return failure("open", result);
  result = nvs_set_blob(handle, Key, record, size);
  if (result != ESP_OK) {
    nvs_close(handle);
    return failure("write", result);
  }
  result = nvs_commit(handle);
  nvs_close(handle);
  return result == ESP_OK || failure("commit", result);
}

bool saveRoleProfile(const RoleProfile &profile) {
  ProfileJournal existing;
  if (!loadProfileJournal(existing))
    return false;
  if (existing.generation)
    return failure("legacy save after RF update", ESP_ERR_INVALID_STATE);
  return writeProfile({profile, 0, 0}, RecordSize);
}

bool commitProfileJournal(const ProfileJournal &journal) {
  if (!journal.generation || !journal.nonce)
    return failure("journal validation", ESP_ERR_INVALID_ARG);
  return writeProfile(journal, JournalSize);
}
bool loadOriginPathWidth(uint8_t &width) {
  width = 1;
#if defined(MESHCORE_PUBLIC_PROVISIONING) && MESHCORE_PUBLIC_PROVISIONING
  if (!publicProvisioningReady())
    return failure("public setup unavailable", ESP_ERR_INVALID_STATE);
  width = publicProvisioning().pathWidth;
#endif
  nvs_handle_t handle;
  auto result = nvs_open(Namespace, NVS_READONLY, &handle);
  if (result == ESP_ERR_NVS_NOT_FOUND) return true;
  if (result != ESP_OK) return failure("origin path open", result);
  uint8_t record[5]{};
  size_t size = sizeof(record);
  result = nvs_get_blob(handle, "origin-path", record, &size);
  nvs_close(handle);
  if (result == ESP_ERR_NVS_NOT_FOUND) return true;
  if (result != ESP_OK) return failure("origin path read", result);
  if (size != sizeof(record) || memcmp(record, "MCP\1", 4) || record[4] < 1 || record[4] > 3)
    return failure("origin path validation", ESP_ERR_INVALID_STATE);
  width = record[4];
  return true;
}
bool saveOriginPathWidth(uint8_t width) {
  if (width < 1 || width > 3) return failure("origin path argument", ESP_ERR_INVALID_ARG);
  const uint8_t record[] = {'M', 'C', 'P', 1, width};
  nvs_handle_t handle;
  auto result = nvs_open(Namespace, NVS_READWRITE, &handle);
  if (result != ESP_OK) return failure("origin path open", result);
  result = nvs_set_blob(handle, "origin-path", record, sizeof(record));
  if (result == ESP_OK) result = nvs_commit(handle);
  nvs_close(handle);
  if (result != ESP_OK) return failure("origin path commit", result);
  uint8_t readback;
  return (loadOriginPathWidth(readback) && readback == width) ||
         failure("origin path readback", ESP_ERR_INVALID_STATE);
}
} // namespace onchip
