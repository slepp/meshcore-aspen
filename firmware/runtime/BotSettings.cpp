// SPDX-License-Identifier: Apache-2.0
#include "BotSettings.h"
#include "BotJournal.h"
#include "RoleStorage.h"
#include "Config.h"
#include <Arduino.h>
#include <SPIFFS.h>
#include <Utils.h>
#include <nvs.h>
#include <string.h>
#include <memory>
#ifndef ONCHIP_COMMAND_BOT_DEFAULT_ENABLED
#define ONCHIP_COMMAND_BOT_DEFAULT_ENABLED 0
#endif
namespace onchip {
namespace {
bool failed(const char *operation, esp_err_t result) {
  Serial.printf("On-chip command bot selection %s failed: %s\n",
                operation, esp_err_to_name(result));
  return false;
}
bool readFlag(const char *key, const char magic[4], bool fallback, bool &enabled) {
  enabled = false;
  nvs_handle_t handle;
  esp_err_t result = nvs_open("mc-onchip", NVS_READONLY, &handle);
  if (result == ESP_ERR_NVS_NOT_FOUND) {
    enabled = fallback;
    return true;
  }
  if (result != ESP_OK) return failed("open", result);
  uint8_t record[5]{};
  size_t size = sizeof(record);
  result = nvs_get_blob(handle, key, record, &size);
  nvs_close(handle);
  if (result == ESP_ERR_NVS_NOT_FOUND) {
    enabled = fallback;
    return true;
  }
  if (result != ESP_OK) return failed("read", result);
  if (size != sizeof(record) || memcmp(record, magic, 4) || record[4] > 1)
    return failed("validation", ESP_ERR_INVALID_STATE);
  enabled = record[4];
  return true;
}
bool writeFlag(const char *key, const char magic[4], bool enabled) {
  nvs_handle_t handle;
  esp_err_t result = nvs_open("mc-onchip", NVS_READWRITE, &handle);
  if (result != ESP_OK) return failed("open", result);
  uint8_t record[5]{};
  memcpy(record, magic, 4); record[4] = enabled;
  result = nvs_set_blob(handle, key, record, sizeof(record));
  if (result == ESP_OK) result = nvs_commit(handle);
  nvs_close(handle);
  return result == ESP_OK || failed("commit", result);
}
}
bool loadBotEnabled(bool &enabled) {
  return readFlag("command-select", "MCB\1", ONCHIP_COMMAND_BOT_DEFAULT_ENABLED != 0, enabled);
}
bool saveBotEnabled(bool enabled) { return writeFlag("command-select", "MCB\1", enabled); }
bool loadBotSharedState(bool &enabled) { return readFlag("bot-shared-kv", "MCS\1", false, enabled); }
bool saveBotSharedState(bool enabled) { return writeFlag("bot-shared-kv", "MCS\1", enabled); }
bool loadBotHomeAccess(bool &enabled) { return readFlag("bot-home", "BHP\1", false, enabled); }
bool loadBotDiscovery(bool &enabled) { return readFlag("bot-discovery", "BDP\1", false, enabled); }
bool loadBotAdaptiveAdmission(bool &enabled) {
  return readFlag("bot-adaptive", "BAA\1", false, enabled);
}
bool saveBotAdaptiveAdmission(bool enabled) {
  if (!writeFlag("bot-adaptive", "BAA\1", enabled)) return false;
  bool actual = false;
  return (loadBotAdaptiveAdmission(actual) && actual == enabled) ||
      failed("adaptive admission readback; outcome unknown", ESP_ERR_INVALID_STATE);
}
bool saveBotDiscovery(bool enabled) {
  if (!writeFlag("bot-discovery", "BDP\1", enabled)) return false;
  bool actual = false;
  return (loadBotDiscovery(actual) && actual == enabled) ||
         failed("discovery readback; outcome unknown", ESP_ERR_INVALID_STATE);
}
bool saveBotHomeAccess(bool enabled) {
  if (!writeFlag("bot-home", "BHP\1", enabled)) return false;
  bool actual = false;
  return (loadBotHomeAccess(actual) && actual == enabled) ||
         failed("home grant readback", ESP_ERR_INVALID_STATE);
}
bool loadBotReminderAccess(bool &enabled) { return readFlag("bot-reminders", "BRG\1", false, enabled); }
bool loadBotEventAccess(uint8_t &mask) {
  mask = 0;
  nvs_handle_t handle;
  auto result = nvs_open("mc-onchip", NVS_READONLY, &handle);
  if (result == ESP_ERR_NVS_NOT_FOUND) return true;
  if (result != ESP_OK) return failed("event grant open", result);
  uint8_t record[5]{}; size_t size = sizeof(record);
  result = nvs_get_blob(handle, "bot-events", record, &size); nvs_close(handle);
  if (result == ESP_ERR_NVS_NOT_FOUND) return true;
  if (result != ESP_OK || size != sizeof(record) || memcmp(record, "BEG\1", 4) || record[4] > 31)
    return failed("event grant read/shape", ESP_ERR_INVALID_STATE);
  mask = record[4]; return true;
}
bool saveBotEventAccess(uint8_t mask) {
  if (mask > 31) return failed("event grant mask", ESP_ERR_INVALID_ARG);
  nvs_handle_t handle;
  auto result = nvs_open("mc-onchip", NVS_READWRITE, &handle);
  if (result != ESP_OK) return failed("event grant open", result);
  uint8_t record[5] = {'B', 'E', 'G', 1, mask};
  result = nvs_set_blob(handle, "bot-events", record, sizeof(record));
  if (result == ESP_OK) result = nvs_commit(handle);
  nvs_close(handle);
  if (result != ESP_OK) return failed("event grant commit; outcome unknown", result);
  uint8_t actual = 0;
  return (loadBotEventAccess(actual) && actual == mask) ||
      failed("event grant readback; outcome unknown", ESP_ERR_INVALID_STATE);
}
bool saveBotReminderAccess(bool enabled) {
  if (!writeFlag("bot-reminders", "BRG\1", enabled)) return false;
  bool actual = false;
  return (loadBotReminderAccess(actual) && actual == enabled) ||
         failed("reminder grant readback", ESP_ERR_INVALID_STATE);
}
bool BotRepeaterPolicy::valid() const {
  if (intervalSeconds < 60 || intervalSeconds > 86400 ||
      discoverySeconds < 60 || discoverySeconds > 86400) return false;
  for (unsigned i = 0; i < BotRepeaterLimit; ++i) {
    const auto &target = targets[i];
    if (!target.used) continue;
    const size_t length = strnlen(target.alias, sizeof(target.alias));
    if (!length || length == sizeof(target.alias) ||
        (target.path.known && !target.path.valid()) ||
        (target.frequencyHz && (target.frequencyHz < 150000000 || target.frequencyHz > 2500000000u)))
      return false;
    for (size_t j = 0; j < length; ++j)
      if (!((target.alias[j] >= 'a' && target.alias[j] <= 'z') ||
            (j && target.alias[j] >= '0' && target.alias[j] <= '9') ||
            (j && (target.alias[j] == '-' || target.alias[j] == '_')))) return false;
    bool key = false;
    for (auto byte : target.key) key = key || byte;
    if (!key) return false;
    for (unsigned j = 0; j < i; ++j)
      if (targets[j].used && (!strcmp(targets[j].alias, target.alias) ||
                             !memcmp(targets[j].key, target.key, 32))) return false;
  }
  return true;
}
namespace {
constexpr const char *RepeaterSlots[] = {"/repeaters-a.bin", "/repeaters-b.bin"};
struct RepeaterReference {
  uint8_t magic[4]{'B', 'R', 'F', 1}, slot = 0, digest[32]{};
  bool valid() const { return !memcmp(magic, "BRF\1", 4) && slot < 2; }
};
struct RepeaterRecord {
  char magic[4]{'B', 'R', 'M', 2};
  BotRepeaterPolicy policy;
};
constexpr size_t LegacyRepeaterSize =
    offsetof(RepeaterRecord, policy) + offsetof(BotRepeaterPolicy, discoverySeconds);
struct RepeaterWorkspace { RepeaterRecord record, check; };
struct RepeaterWorkspaceDeleter {
  void operator()(RepeaterWorkspace *workspace) const { releaseRoleStorage(workspace); }
};
using RepeaterStorage = std::unique_ptr<RepeaterWorkspace, RepeaterWorkspaceDeleter>;
static_assert(sizeof(RepeaterReference) == 37, "Keep repeater NVS authority bounded");
bool readRepeaterReference(RepeaterReference &reference, bool &present) {
  present = false;
  nvs_handle_t handle;
  auto result = nvs_open("mc-onchip", NVS_READONLY, &handle);
  if (result == ESP_ERR_NVS_NOT_FOUND) return true;
  if (result != ESP_OK) return failed("repeater monitor open", result);
  size_t size = sizeof(reference);
  result = nvs_get_blob(handle, "bot-repeaters", &reference, &size);
  nvs_close(handle);
  if (result == ESP_ERR_NVS_NOT_FOUND) return true;
  if (result != ESP_OK || size != sizeof(reference) || !reference.valid())
    return failed("repeater monitor authority", ESP_ERR_INVALID_STATE);
  present = true;
  return true;
}
void repeaterDigest(const RepeaterRecord &record, uint8_t digest[32],
                    size_t size = sizeof(RepeaterRecord)) {
  mesh::Utils::sha256(digest, 32, reinterpret_cast<const uint8_t *>(&record), size);
}
bool readRepeaterFile(const RepeaterReference &reference, RepeaterRecord &record) {
  auto file = SPIFFS.open(RepeaterSlots[reference.slot], "r");
  if (!file) return failed("repeater monitor file open", ESP_ERR_INVALID_STATE);
  record = {};
  const size_t size = file.size();
  const bool legacy = size == LegacyRepeaterSize;
  const bool complete = (legacy || size == sizeof(record)) &&
      file.read(reinterpret_cast<uint8_t *>(&record), size) == size && file.size() == size;
  file.close();
  uint8_t digest[32];
  if (!complete) return failed("repeater monitor file read", ESP_ERR_INVALID_STATE);
  repeaterDigest(record, digest, size);
  if (memcmp(digest, reference.digest, sizeof(digest)) ||
      memcmp(record.magic, legacy ? "BRM\1" : "BRM\2", 4) || !record.policy.valid())
    return failed("repeater monitor file validation", ESP_ERR_INVALID_STATE);
  // Version 1 ends before discoverySeconds; retain its targets and flood times.
  memcpy(record.magic, "BRM\2", 4);
  return true;
}
}
bool loadBotRepeaterPolicy(BotRepeaterPolicy &policy) {
  policy = {};
  RepeaterReference reference;
  bool present;
  if (!readRepeaterReference(reference, present)) return false;
  if (!present) return true;
  RepeaterStorage storage(allocateRoleStorage<RepeaterWorkspace>("repeater monitor read"));
  if (!storage) return failed("repeater monitor read workspace unavailable", ESP_FAIL);
  if (!readRepeaterFile(reference, storage->record)) return false;
  policy = storage->record.policy;
  return true;
}
void botRepeaterStorageStatus(char *reply, size_t capacity) {
  RepeaterReference reference;
  bool present;
  nvs_stats_t stats{};
  if (!readRepeaterReference(reference, present) || nvs_get_stats(nullptr, &stats) != ESP_OK) {
    snprintf(reply, capacity, "Error: repeater storage authority or NVS statistics unavailable");
    return;
  }
  snprintf(reply, capacity, "Repeater storage nvs-free=%u required=%u authority=%s",
           unsigned(stats.free_entries),
           unsigned(BotCoreNvsReserveEntries + BotNvsMutationEntries + (present ? 0u : 5u)),
           present ? "saved" : "none");
}
bool saveBotRepeaterPolicy(const BotRepeaterPolicy &policy, char *error, size_t capacity) {
  if (error && capacity) error[0] = 0;
  const auto failSave = [&](const char *operation, esp_err_t result) {
    if (error && capacity)
      snprintf(error, capacity, "%s: %s", operation, esp_err_to_name(result));
    return failed(operation, result);
  };
  if (!policy.valid()) return failSave("repeater monitor validation", ESP_ERR_INVALID_ARG);
  RepeaterReference previous, next;
  bool present;
  if (!readRepeaterReference(previous, present))
    return failSave("repeater monitor authority read", ESP_ERR_INVALID_STATE);
  nvs_stats_t stats{};
  auto result = nvs_get_stats(nullptr, &stats);
  if (result != ESP_OK) return failSave("repeater monitor headroom read", result);
  // A new four-entry reference must leave the existing public KV floor intact.
  const size_t required = BotCoreNvsReserveEntries + BotNvsMutationEntries + (present ? 0u : 5u);
  if (stats.free_entries < required) {
    if (error && capacity)
      snprintf(error, capacity, "repeater NVS headroom: %u free, %u required",
               unsigned(stats.free_entries), unsigned(required));
    return failed("repeater monitor NVS headroom; existing Lua data reserve required",
                  ESP_ERR_NVS_NOT_ENOUGH_SPACE);
  }
  RepeaterStorage storage(allocateRoleStorage<RepeaterWorkspace>("repeater monitor save"));
  if (!storage) return failSave("repeater monitor save workspace unavailable", ESP_FAIL);
  auto &record = storage->record, &check = storage->check;
  if (present && !readRepeaterFile(previous, check))
    return failSave("repeater monitor saved file read/validation", ESP_ERR_INVALID_STATE);
  record.policy = policy;
  next.slot = present ? 1 - previous.slot : 0;
  repeaterDigest(record, next.digest);
  auto file = SPIFFS.open(RepeaterSlots[next.slot], "w");
  if (!file) return failSave("repeater monitor inactive file open", ESP_ERR_INVALID_STATE);
  const bool complete = file.write(reinterpret_cast<const uint8_t *>(&record), sizeof(record)) ==
                        sizeof(record);
  file.flush();
  file.close();
  if (!complete || !readRepeaterFile(next, check) || memcmp(&record, &check, sizeof(record)))
    return failSave("repeater monitor inactive file readback", ESP_ERR_INVALID_STATE);
  nvs_handle_t handle;
  result = nvs_open("mc-onchip", NVS_READWRITE, &handle);
  if (result != ESP_OK) return failSave("repeater monitor open", result);
  result = nvs_set_blob(handle, "bot-repeaters", &next, sizeof(next));
  if (result == ESP_OK) result = nvs_commit(handle);
  nvs_close(handle);
  if (result != ESP_OK) return failSave("repeater monitor commit; outcome unknown", result);
  RepeaterReference observed;
  if (!readRepeaterReference(observed, present) || !present ||
      memcmp(&observed, &next, sizeof(next)))
    return failSave("repeater monitor authority readback; outcome unknown", ESP_ERR_INVALID_STATE);
  if (!readRepeaterFile(observed, check) || memcmp(&record, &check, sizeof(record)))
    return failSave("repeater monitor readback; outcome unknown", ESP_ERR_INVALID_STATE);
  return true;
}
bool BotForwardPolicy::enabled() const {
  for (auto byte : from) if (byte) return true;
  return false;
}
bool BotForwardPolicy::valid() const {
  bool target = false;
  for (auto byte : to) target = target || byte;
  return enabled() == target && (!target || memcmp(from, to, sizeof(from)));
}
bool loadBotForwardPolicy(BotForwardPolicy &policy) {
  policy = {};
  nvs_handle_t handle;
  auto result = nvs_open("mc-onchip", NVS_READONLY, &handle);
  if (result == ESP_ERR_NVS_NOT_FOUND) return true;
  if (result != ESP_OK) return failed("forward open", result);
  uint8_t record[68]{};
  size_t size = sizeof(record);
  result = nvs_get_blob(handle, "bot-forward", record, &size);
  nvs_close(handle);
  if (result == ESP_ERR_NVS_NOT_FOUND) return true;
  if (result != ESP_OK || size != sizeof(record) || memcmp(record, "BFP\1", 4))
    return failed("forward read/shape", ESP_ERR_INVALID_STATE);
  memcpy(policy.from, record + 4, 32); memcpy(policy.to, record + 36, 32);
  if (!policy.valid()) { policy = {}; return failed("forward validation", ESP_ERR_INVALID_STATE); }
  return true;
}
bool saveBotForwardPolicy(const BotForwardPolicy &policy) {
  if (!policy.valid()) return failed("forward validation", ESP_ERR_INVALID_ARG);
  uint8_t record[68]{};
  memcpy(record, "BFP\1", 4);
  memcpy(record + 4, policy.from, 32); memcpy(record + 36, policy.to, 32);
  nvs_handle_t handle;
  auto result = nvs_open("mc-onchip", NVS_READWRITE, &handle);
  if (result != ESP_OK) return failed("forward open", result);
  result = nvs_set_blob(handle, "bot-forward", record, sizeof(record));
  if (result == ESP_OK) result = nvs_commit(handle);
  nvs_close(handle);
  if (result != ESP_OK) return failed("forward commit; outcome unknown", result);
  BotForwardPolicy actual;
  return (loadBotForwardPolicy(actual) && !memcmp(actual.from, policy.from, 32) &&
          !memcmp(actual.to, policy.to, 32)) || failed("forward readback; outcome unknown", ESP_ERR_INVALID_STATE);
}
bool BotRadioPolicy::valid() const {
  const size_t length = strnlen(channel, sizeof(channel));
  if (length == sizeof(channel) || (length && !channelKeySet && (length < 2 || channel[0] != '#')) ||
      pathWidth < 1 || pathWidth > 3 || airtimeMs < 360 || airtimeMs > 3600) return false;
  bool keyPresent = false;
  for (auto byte : channelKey) keyPresent = keyPresent || byte;
  if (channelKeySet != keyPresent || (channelKeySet && !length)) return false;
  if (channelKeySet) {
    for (size_t i = 0; i < length; ++i)
      if (channel[i] < 32 || channel[i] > 126) return false;
    return true;
  }
  for (size_t i = 1; i < length; ++i)
    if (!((channel[i] >= 'a' && channel[i] <= 'z') ||
          (channel[i] >= '0' && channel[i] <= '9') || channel[i] == '-' || channel[i] == '_'))
      return false;
  return true;
}
bool loadBotRadioPolicy(BotRadioPolicy &policy) {
  policy = {};
  nvs_handle_t handle;
  auto result = nvs_open("mc-onchip", NVS_READONLY, &handle);
  if (result == ESP_ERR_NVS_NOT_FOUND) return true;
  if (result != ESP_OK) return failed("policy open", result);
  uint8_t record[57]{};
  size_t size = sizeof(record);
  result = nvs_get_blob(handle, "bot-radio", record, &size);
  nvs_close(handle);
  if (result == ESP_ERR_NVS_NOT_FOUND) return true;
  if (result != ESP_OK) return failed("policy read", result);
  const bool storedHashtag = size == 40 && !memcmp(record, "BRP\1", 4);
  if (!storedHashtag && (size != sizeof(record) || memcmp(record, "BRP\2", 4) || record[40] > 1))
    return failed("policy shape", ESP_ERR_INVALID_STATE);
  memcpy(policy.channel, record + 4, sizeof(policy.channel));
  policy.pathWidth = record[37];
  policy.airtimeMs = record[38] | (uint16_t(record[39]) << 8);
  if (!storedHashtag) {
    policy.channelKeySet = record[40];
    memcpy(policy.channelKey, record + 41, 16);
  }
  if (!policy.valid()) return failed("policy validation", ESP_ERR_INVALID_STATE);
  return true;
}
bool saveBotRadioPolicy(const BotRadioPolicy &policy) {
  if (!policy.valid()) return failed("policy validation", ESP_ERR_INVALID_ARG);
  uint8_t record[57]{};
  memcpy(record, "BRP\2", 4);
  memcpy(record + 4, policy.channel, strlen(policy.channel));
  record[37] = policy.pathWidth;
  record[38] = policy.airtimeMs & 255; record[39] = policy.airtimeMs >> 8;
  record[40] = policy.channelKeySet; memcpy(record + 41, policy.channelKey, 16);
  nvs_handle_t handle;
  auto result = nvs_open("mc-onchip", NVS_READWRITE, &handle);
  if (result != ESP_OK) return failed("policy open", result);
  result = nvs_set_blob(handle, "bot-radio", record, sizeof(record));
  if (result == ESP_OK) result = nvs_commit(handle);
  nvs_close(handle);
  if (result != ESP_OK) return failed("policy commit", result);
  BotRadioPolicy readback;
  if (!loadBotRadioPolicy(readback) || strcmp(readback.channel, policy.channel) ||
      readback.pathWidth != policy.pathWidth || readback.airtimeMs != policy.airtimeMs ||
      readback.channelKeySet != policy.channelKeySet || memcmp(readback.channelKey, policy.channelKey, 16))
    return failed("policy readback", ESP_ERR_INVALID_STATE);
  return true;
}
bool BotMeshPolicy::allows(const uint8_t key[32]) const {
  bool present = false;
  for (unsigned i = 0; i < 32; ++i) present = present || key[i];
  if (!present) return false;
  for (const auto &destination : destinations) if (!memcmp(destination, key, 32)) return true;
  return false;
}
bool BotMeshPolicy::valid() const {
  const size_t size = strnlen(name, sizeof(name));
  if (!size || size > 31) return false;
  for (size_t i = 0; i < size; ++i)
    if (name[i] < 32 || name[i] > 126 || name[i] == ':') return false;
  for (unsigned i = 0; i < 4; ++i) {
    bool present = false;
    for (auto byte : destinations[i]) present = present || byte;
    if (present) for (unsigned j = 0; j < i; ++j)
      if (!memcmp(destinations[i], destinations[j], 32)) return false;
  }
  return true;
}
bool loadBotMeshPolicy(BotMeshPolicy &policy) {
  policy = {};
  static_assert(sizeof(ONCHIP_COMMAND_BOT_NAME) <= 32, "Bot name exceeds native name capacity");
  strcpy(policy.name, ONCHIP_COMMAND_BOT_NAME);
  nvs_handle_t handle;
  auto result = nvs_open("mc-onchip", NVS_READONLY, &handle);
  if (result == ESP_ERR_NVS_NOT_FOUND) return true;
  if (result != ESP_OK) return failed("mesh policy open", result);
  uint8_t record[166]{};
  size_t size = sizeof(record);
  result = nvs_get_blob(handle, "bot-mesh", record, &size);
  nvs_close(handle);
  if (result == ESP_ERR_NVS_NOT_FOUND) return true;
  if (result != ESP_OK || size != sizeof(record) || memcmp(record, "BMP\1", 4) || record[4] > 1)
    return failed("mesh policy read/shape", ESP_ERR_INVALID_STATE);
  policy.channelWait = record[4];
  memcpy(policy.name, record + 5, 33); memcpy(policy.destinations, record + 38, 128);
  return policy.valid() || failed("mesh policy validation", ESP_ERR_INVALID_STATE);
}
bool saveBotMeshPolicy(const BotMeshPolicy &policy) {
  if (!policy.valid()) return failed("mesh policy validation", ESP_ERR_INVALID_ARG);
  uint8_t record[166]{};
  memcpy(record, "BMP\1", 4); record[4] = policy.channelWait;
  memcpy(record + 5, policy.name, strlen(policy.name)); memcpy(record + 38, policy.destinations, 128);
  nvs_handle_t handle;
  auto result = nvs_open("mc-onchip", NVS_READWRITE, &handle);
  if (result != ESP_OK) return failed("mesh policy open", result);
  result = nvs_set_blob(handle, "bot-mesh", record, sizeof(record));
  if (result == ESP_OK) result = nvs_commit(handle);
  nvs_close(handle);
  if (result != ESP_OK) return failed("mesh policy commit; outcome unknown", result);
  BotMeshPolicy actual;
  return (loadBotMeshPolicy(actual) && !strcmp(actual.name, policy.name) &&
          actual.channelWait == policy.channelWait && !memcmp(actual.destinations, policy.destinations, 128)) ||
         failed("mesh policy readback; outcome unknown", ESP_ERR_INVALID_STATE);
}
}
