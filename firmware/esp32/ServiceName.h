// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "Config.h"
#include <Arduino.h>
#include <nvs.h>
#include <string.h>

namespace onchip {
enum class NamedService { Management, Kiss };

inline bool validRuntimeName(const char *name) {
  if (!name || !name[0] || strlen(name) > 31) return false;
  for (const unsigned char *p = reinterpret_cast<const unsigned char *>(name); *p; ++p)
    if (*p < 32 || *p > 126 || *p == ':') return false;
  return true;
}

namespace service_name {
struct Record {
  char magic[4] = {'M', 'S', 'N', 1};
  char name[32]{};
};
static_assert(sizeof(Record) == 36, "Service name record layout changed");
inline const char *key(NamedService service) {
  return service == NamedService::Management ? "management-name" : "kiss-name";
}
inline bool failed(NamedService service, const char *operation) {
  Serial.printf("On-chip %s %s failed; saved name may be uncertain\n", key(service), operation);
  return false;
}
}

inline bool loadServiceName(NamedService service, char name[32]) {
  using namespace service_name;
  nvs_handle_t handle;
  auto result = nvs_open("mc-onchip", NVS_READONLY, &handle);
  Record record;
  size_t size = sizeof(record);
  if (result == ESP_OK) {
    result = nvs_get_blob(handle, key(service), &record, &size);
    nvs_close(handle);
  }
  if (result == ESP_ERR_NVS_NOT_FOUND) {
    const char *fallback = service == NamedService::Management ? ONCHIP_MANAGEMENT_NAME : ONCHIP_BOT_NAME;
    if (!validRuntimeName(fallback)) return failed(service, "default validation");
    strcpy(name, fallback);
    return true;
  }
  if (result != ESP_OK || size != sizeof(record) || memcmp(record.magic, "MSN\1", 4) ||
      !memchr(record.name, 0, sizeof(record.name)) || !validRuntimeName(record.name))
    return failed(service, "read/validation");
  strcpy(name, record.name);
  return true;
}

inline bool saveServiceName(NamedService service, const char *name) {
  using namespace service_name;
  if (!validRuntimeName(name)) return failed(service, "name validation");
  Record record;
  strcpy(record.name, name);
  nvs_handle_t handle;
  auto result = nvs_open("mc-onchip", NVS_READWRITE, &handle);
  if (result != ESP_OK) return failed(service, "open");
  result = nvs_set_blob(handle, key(service), &record, sizeof(record));
  if (result == ESP_OK) result = nvs_commit(handle);
  nvs_close(handle);
  if (result != ESP_OK) return failed(service, "commit");
  char actual[32]{};
  return (loadServiceName(service, actual) && !strcmp(actual, name)) ||
         failed(service, "readback");
}
} // namespace onchip
