// SPDX-License-Identifier: Apache-2.0
#include "Telemetry.h"
#include <Arduino.h>
#include <nvs.h>
#include <cstring>

namespace onchip {
namespace {
bool failed(const char *operation) {
  Serial.printf("Telemetry settings %s failed; publishing disabled\n", operation);
  return false;
}
}
bool loadTelemetryConfig(TelemetryConfig &config) {
  config = {};
  nvs_handle_t handle;
  auto result = nvs_open("mc-onchip", NVS_READONLY, &handle);
  if (result == ESP_ERR_NVS_NOT_FOUND) return true;
  if (result != ESP_OK) return failed("open");
  uint8_t record[9]{};
  size_t size = sizeof(record);
  result = nvs_get_blob(handle, "telemetry", record, &size);
  nvs_close(handle);
  if (result == ESP_ERR_NVS_NOT_FOUND) return true;
  if (result != ESP_OK || size != sizeof(record) || memcmp(record, "MCT\1", 4) || record[4] > 1)
    return failed("read/shape");
  TelemetryConfig saved;
  saved.enabled = record[4];
  saved.intervalSeconds = queued_tx::get32(record + 5);
  if (!saved.valid()) return failed("validation");
  config = saved;
  return true;
}
bool saveTelemetryConfig(const TelemetryConfig &config) {
  if (!config.valid()) return failed("validation");
  nvs_handle_t handle;
  if (nvs_open("mc-onchip", NVS_READWRITE, &handle) != ESP_OK) return failed("open");
  uint8_t record[9] = {'M', 'C', 'T', 1, uint8_t(config.enabled)};
  queued_tx::put32(record + 5, config.intervalSeconds);
  auto result = nvs_set_blob(handle, "telemetry", record, sizeof(record));
  if (result == ESP_OK) result = nvs_commit(handle);
  nvs_close(handle);
  if (result != ESP_OK) return failed("commit (outcome unknown)");
  TelemetryConfig actual;
  return (loadTelemetryConfig(actual) && actual.enabled == config.enabled &&
          actual.intervalSeconds == config.intervalSeconds) || failed("readback (outcome unknown)");
}
} // namespace onchip
