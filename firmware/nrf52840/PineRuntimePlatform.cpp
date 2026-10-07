// SPDX-License-Identifier: Apache-2.0
#if NRFMAST_PRODUCTION_LUA
#include "PineRuntimePlatform.h"
#include "PineTrustedTime.h"
#include "PineAdmin.h"
#include "onchip/Clock.h"
#include "onchip/RoleIdentity.h"
#include "onchip/MastSource.h"
#include "onchip/CommandBot.h"
#include "platform/nvs.h"
#include <Arduino.h>
#include <bluefruit.h>
#include <target.h>
#include <atomic>
#include <cstring>

namespace {
mesh::LocalIdentity luaIdentity;
std::atomic_flag clockGate = ATOMIC_FLAG_INIT;
std::atomic_flag randomGate = ATOMIC_FLAG_INIT;
nrfmast::TrustedTimeSample trustedTime;
std::atomic<nrfmast::LuaTimeSource> trustedSource{nrfmast::LuaTimeSource::None};
}
namespace nrfmast {
void setLuaIdentity(const mesh::LocalIdentity &identity) { luaIdentity = identity; }
bool trustLuaTime(uint32_t utc, LuaTimeSource source) {
  if (utc < 1715770351u || utc > 4102444800u) return false;
  while (clockGate.test_and_set(std::memory_order_acquire)) delay(1);
  const uint32_t now = millis();
  const bool fresh = trustedTime.fresh(now);
  if (source == LuaTimeSource::BleCompanion) {
    const uint32_t current = rtc_clock.getCurrentTime();
    if (!companionTimeStepAllowed(current, utc, fresh)) {
      clockGate.clear(std::memory_order_release); return false;
    }
    // The two-second tolerance fits the trusted bounds' initial uncertainty.
    utc = companionClockValue(current, utc, fresh);
    if (utc > 4102444800u) {
      clockGate.clear(std::memory_order_release); return false;
    }
  }
  trustedTime.refresh(utc, now);
  trustedSource.store(source, std::memory_order_release);
  rtc_clock.setCurrentTime(utc);
  onchip::commandBotService().synchronizeTime(utc);
  clockGate.clear(std::memory_order_release);
  return true;
}
void pollLuaTime() {
  if (clockGate.test_and_set(std::memory_order_acquire)) return;
  trustedTime.poll(millis());
  clockGate.clear(std::memory_order_release);
}
bool trustRadioTime(uint32_t lower, uint32_t upper, uint32_t lifetimeMs) {
  while (clockGate.test_and_set(std::memory_order_acquire)) delay(1);
  const uint32_t now = millis();
  if (trustedSource.load() == LuaTimeSource::Gps && trustedTime.fresh(now)) {
    clockGate.clear(std::memory_order_release); return false;
  }
  const bool accepted = trustedTime.refreshBounds(lower, upper, now, lifetimeMs);
  if (accepted) {
    trustedSource.store(LuaTimeSource::RadioProvider);
    // RTC unique timestamps and pending deadlines must never move backwards.
    if (lower > rtc_clock.getCurrentTime()) rtc_clock.setCurrentTime(lower);
    onchip::commandBotService().synchronizeTime(lower);
  }
  clockGate.clear(std::memory_order_release);
  return accepted;
}
void revokeRadioTime() {
  while (clockGate.test_and_set(std::memory_order_acquire)) delay(1);
  if (trustedSource.load() == LuaTimeSource::RadioProvider) {
    trustedTime.revoke(); trustedSource.store(LuaTimeSource::None);
  }
  clockGate.clear(std::memory_order_release);
}
const char* luaTimeSource() {
  switch (trustedSource.load(std::memory_order_acquire)) {
    case LuaTimeSource::Admin: return "admin-console";
    case LuaTimeSource::BleCompanion: return "paired-ble";
    case LuaTimeSource::Gps: return "gps";
    case LuaTimeSource::RadioProvider: return "radio-provider";
    default: return "none";
  }
}
bool secureRandom(uint8_t *bytes, size_t size) {
  const uint32_t deadline = millis() + 100;
  while (randomGate.test_and_set(std::memory_order_acquire)) {
    if (int32_t(millis() - deadline) >= 0) return false;
    delay(1);
  }
  struct Release {
    ~Release() { randomGate.clear(std::memory_order_release); }
  } release;
  uint8_t enabled = 0;
  if (!bytes || sd_softdevice_is_enabled(&enabled) != NRF_SUCCESS) return false;
  if (enabled) {
    const uint32_t start = millis();
    while (size) {
      const uint8_t count = size > 8 ? 8 : size;
      const auto status = sd_rand_application_vector_get(bytes, count);
      if (status == NRF_SUCCESS) { bytes += count; size -= count; }
      else if (status != NRF_ERROR_SOC_RAND_NOT_ENOUGH_VALUES || uint32_t(millis() - start) >= 100) return false;
      else delay(1);
    }
    return true;
  }
  NRF_RNG->CONFIG = RNG_CONFIG_DERCEN_Enabled;
  NRF_RNG->EVENTS_VALRDY = 0; NRF_RNG->TASKS_START = 1;
  const uint32_t start = millis();
  while (size) {
    if (NRF_RNG->EVENTS_VALRDY) {
      *bytes++ = NRF_RNG->VALUE; --size; NRF_RNG->EVENTS_VALRDY = 0;
    } else if (uint32_t(millis() - start) >= 100) {
      NRF_RNG->TASKS_STOP = 1; return false;
    }
  }
  NRF_RNG->TASKS_STOP = 1; return true;
}
}
namespace onchip {
void HardwareRNG::random(uint8_t *bytes, size_t size) {
  if (!nrfmast::secureRandom(bytes, size)) {
    Serial.println("radio: hardware RNG unavailable; reboot required");
    NVIC_SystemReset();
    while (true) delay(1);
  }
}
bool loadIdentity(const char *name, mesh::LocalIdentity &identity) {
  if (strcmp(name, "command-bot")) return false;
  identity = luaIdentity;
  return true;
}
RoleClock::RoleClock(uint32_t baseline) : seconds_(baseline), previousMillis_(millis()) {}
uint32_t RoleClock::getCurrentTime() { tick(); return seconds_; }
void RoleClock::setCurrentTime(uint32_t value) { synchronize(value, ClockSource::Native); }
void RoleClock::synchronize(uint32_t value, ClockSource) {
  seconds_ = value; previousMillis_ = millis(); remainder_ = 0; source_ = ClockSource::Native;
}
void RoleClock::tick() {
  const uint32_t now = millis(), elapsed = uint32_t(now - previousMillis_);
  previousMillis_ = now;
  const uint64_t total = uint64_t(elapsed) + remainder_;
  seconds_ += total / 1000; remainder_ = total % 1000;
}
bool clockSnapshot(ClockSnapshot &value) {
  value = ClockSnapshot{};
  if (clockGate.test_and_set(std::memory_order_acquire)) return false;
  const uint32_t now = millis();
  trustedTime.poll(now);
  value.network_epoch = trustedTime.epoch(); value.network_age_ms = trustedTime.ageMs(now);
  value.network_age_seconds = value.network_age_ms / 1000; value.sampled_at_ms = now;
  clockGate.clear(std::memory_order_release); return true;
}
bool trustedNetworkTime(uint32_t &earliest, uint32_t &latest, const char **reason) {
  earliest = latest = 0;
  if (clockGate.test_and_set(std::memory_order_acquire)) {
    if (reason) *reason = "UTC publication busy";
    return false;
  }
  const uint32_t now = millis();
  trustedTime.poll(now);
  const bool valid = trustedTime.bounds(now, earliest, latest);
  clockGate.clear(std::memory_order_release);
  if (!valid) {
    if (reason) *reason = "UTC unavailable/expired; obtain GPS fix, refresh paired companion/admin time or inspect bot time.provider";
    return false;
  }
  if (reason) *reason = nullptr;
  return true;
}
bool mastRecord(const char *key, void *data, size_t size, bool write, bool &present) {
  present = false;
  nvs_handle_t handle;
  auto result = nvs_open("mc-mast", write ? NVS_READWRITE : NVS_READONLY, &handle);
  if (!write && result == ESP_ERR_NVS_NOT_FOUND) return true;
  if (result != ESP_OK) return false;
  if (write) {
    result = nvs_set_blob(handle, key, data, size);
    if (result == ESP_OK) result = nvs_commit(handle);
  } else {
    size_t actual = size;
    result = nvs_get_blob(handle, key, data, &actual);
    if (result == ESP_OK && actual != size) result = ESP_ERR_NVS_INVALID_LENGTH;
    present = result == ESP_OK;
    if (result == ESP_ERR_NVS_NOT_FOUND) result = ESP_OK;
  }
  nvs_close(handle); return result == ESP_OK;
}
}
#endif
