// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "SntpConfig.h"
#include <Arduino.h>
#include <esp_sntp.h>
#include <nvs.h>
#include <time.h>
#include <atomic>

namespace radio_time {
inline SntpConfig settings;
inline bool settingsLoaded = false, settingsFault = false;
using SampleHandler = void (*)(uint32_t, uint32_t);
inline std::atomic<SampleHandler> sampleHandler{nullptr};
inline std::atomic<bool> samplesEnabled{false};
inline std::atomic<uint32_t> epoch{0}, receipt{0}, version{0};
inline std::atomic<uint32_t> generation{0}, sampleGeneration{0};
inline uint32_t previousMillis = 0;
inline uint64_t elapsedMillis = 0;
inline uint32_t consumedVersion = 0, currentEpoch = 0;

inline bool readConfig(SntpConfig &config, bool &present) {
  present = false;
  nvs_handle_t handle;
  auto result = nvs_open("mc-time", NVS_READONLY, &handle);
  if (result == ESP_ERR_NVS_NOT_FOUND) return true;
  if (result != ESP_OK) return false;
  size_t size = sizeof(config);
  result = nvs_get_blob(handle, "sntp", &config, &size);
  nvs_close(handle);
  if (result == ESP_ERR_NVS_NOT_FOUND) return true;
  present = result == ESP_OK;
  return result == ESP_OK && size == sizeof(config) && validConfig(config);
}
inline void loadConfig() {
  if (settingsLoaded) return;
  settingsLoaded = true;
  SntpConfig saved;
  bool present;
  settingsFault = !readConfig(saved, present);
  if (settingsFault) settings.server[0] = 0;
  else if (present) settings = saved;
}
inline void publish(uint32_t value) {
  if (!samplesEnabled.load()) return;
  const uint32_t sampledGeneration = generation.load();
  version.fetch_add(1);
  epoch.store(value);
  receipt.store(millis());
  sampleGeneration.store(sampledGeneration);
  version.fetch_add(1);
  const auto handler = sampleHandler.load();
  if (handler) handler(value, sampledGeneration);
}
inline void start(SampleHandler handler = nullptr) {
  loadConfig();
  samplesEnabled.store(false);
  generation.fetch_add(1);
  sampleHandler.store(handler);
  currentEpoch = 0;
  elapsedMillis = 0;
  previousMillis = millis();
  consumedVersion = version.load();
  if (!settings.server[0]) {
    sntp_stop();
    return;
  }
  samplesEnabled.store(!settingsFault);
  sntp_set_time_sync_notification_cb([](struct timeval *sample) {
    publish(sample->tv_sec >= 1715770351 && uint64_t(sample->tv_sec) <= 4102444800u ?
            uint32_t(sample->tv_sec) : 0);
  });
  sntp_set_sync_interval(settings.interval * 1000u);
  configTime(0, 0, settings.server);
}
inline bool saveConfig(const SntpConfig &next) {
  if (!validConfig(next)) return false;
  nvs_handle_t handle;
  if (nvs_open("mc-time", NVS_READWRITE, &handle) != ESP_OK) return false;
  auto result = nvs_set_blob(handle, "sntp", &next, sizeof(next));
  if (result == ESP_OK) result = nvs_commit(handle);
  nvs_close(handle);
  SntpConfig actual;
  bool present;
  if (result != ESP_OK || !readConfig(actual, present) || !present ||
      memcmp(&next, &actual, sizeof(next))) return false;
  settings = next;
  settingsFault = false;
  // configTime replaces the existing lwIP SNTP service, not a second client.
  // An off setting suppresses acceptance even if a callback is already in flight.
  start(sampleHandler.load());
  return true;
}
inline bool configCommand(const char *command, char *reply, size_t capacity, bool writeAllowed) {
  loadConfig();
  if (!strcmp(command, "get sntp.server") || !strcmp(command, "get sntp.interval")) {
    SntpConfig saved;
    bool present;
    const bool readable = readConfig(saved, present);
    if (!strcmp(command, "get sntp.server"))
      snprintf(reply, capacity, "live=%s saved=%s fault=%u",
          settings.server[0] ? settings.server : "off",
          readable ? (saved.server[0] ? saved.server : "off") : "unreadable", settingsFault);
    else snprintf(reply, capacity, "live=%u saved=%u seconds saved-readable=%u fault=%u",
          unsigned(settings.interval), readable ? unsigned(saved.interval) : 0, readable, settingsFault);
  }
  else if (!strncmp(command, "set sntp.", 9)) {
    SntpConfig next = settings;
    if (!writeAllowed) snprintf(reply, capacity, "Error: shared SNTP settings require Management administration or local USB");
    else if (!editConfig(next, command)) snprintf(reply, capacity, "Error: use set sntp.server HOST|off or set sntp.interval 60..86400");
    else if (!saveConfig(next)) snprintf(reply, capacity, "Error: SNTP save/readback failed; inspect saved settings before retry");
    else snprintf(reply, capacity, "OK saved and applied; waiting for a new SNTP sample");
  } else return false;
  return true;
}
inline void poll() {
  const uint32_t now = millis();
  elapsedMillis += uint32_t(now - previousMillis);
  previousMillis = now;
  const uint32_t sequence = version.load(), value = epoch.load(), received = receipt.load();
  const uint32_t sampledGeneration = sampleGeneration.load();
  if (!(sequence & 1) && sequence != consumedVersion && sequence == version.load()) {
    consumedVersion = sequence;
    currentEpoch = sampledGeneration == generation.load() ? value : 0;
    elapsedMillis = uint32_t(now - received);
  }
  if (elapsedMillis > uint64_t(settings.interval) * 2000 || !settings.server[0])
    currentEpoch = 0;
}
inline bool bounds(uint32_t &lower, uint32_t &upper) {
  poll();
  lower = upper = 0;
  const uint64_t utc = uint64_t(currentEpoch) + elapsedMillis / 1000;
  if (!currentEpoch || utc < 1715770351u || utc + 1 > 4102444800u) return false;
  lower = utc; upper = utc + 1;
  return true;
}
} // namespace radio_time
