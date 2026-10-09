// SPDX-License-Identifier: Apache-2.0
#include "BotSettings.h"
#include "BotJournal.h"
#include "RoleStorage.h"
#include "BotRegistry.h"
#include "Config.h"
#include <Arduino.h>
#include <SPIFFS.h>
#include <Utils.h>
#include <Mesh.h>
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
BotRadioPolicy::Membership BotRadioPolicy::membership(unsigned slot) const {
  if (slot >= ChannelLimit) return {};
  if (slot) return additional[slot - 1];
  Membership result;
  memcpy(result.name, channel, sizeof(channel));
  memcpy(result.key, channelKey, sizeof(channelKey));
  result.keySet = channelKeySet;
  return result;
}
bool BotRadioPolicy::setMembership(unsigned slot, const Membership &value) {
  if (slot >= ChannelLimit) return false;
  const auto previous = membership(slot);
  if (strcmp(previous.name, value.name) || previous.keySet != value.keySet ||
      memcmp(previous.key, value.key, sizeof(value.key))) {
    access[slot + 1] = !strcmp(value.name, "Public") ? 0 : All;
    for (auto &rule : rules) if (rule.context == slot + 1) rule = {};
  }
  if (slot) additional[slot - 1] = value;
  else {
    memcpy(channel, value.name, sizeof(channel));
    memcpy(channelKey, value.key, sizeof(channelKey));
    channelKeySet = value.keySet;
  }
  return true;
}
void BotRadioPolicy::nativeChannel(const Membership &value, mesh::GroupChannel &result) {
  result = {};
  static const uint8_t publicKey[16] =
      {0x8b,0x33,0x87,0xe9,0xc5,0xcd,0xea,0x6a,0xc9,0xe5,0xed,0xba,0xa1,0x15,0xcd,0x72};
  if (value.keySet) memcpy(result.secret, value.key, sizeof(value.key));
  else if (!strcmp(value.name, "Public")) memcpy(result.secret, publicKey, sizeof(publicKey));
  else mesh::Utils::sha256(result.secret, 16,
      reinterpret_cast<const uint8_t *>(value.name), strlen(value.name));
  mesh::Utils::sha256(result.hash, sizeof(result.hash), result.secret, 16);
}
namespace {
bool validMembership(const BotRadioPolicy::Membership &value) {
  const size_t length = strnlen(value.name, sizeof(value.name));
  const bool publicChannel = length < sizeof(value.name) && !strcmp(value.name, "Public");
  if (length == sizeof(value.name) ||
      (length && !value.keySet && !publicChannel && (length < 2 || value.name[0] != '#')))
    return false;
  bool keyPresent = false;
  for (auto byte : value.key) keyPresent = keyPresent || byte;
  if (value.keySet != keyPresent || (value.keySet && (!length || publicChannel))) return false;
  if (value.keySet) {
    for (size_t i = 0; i < length; ++i)
      if (value.name[i] < 32 || value.name[i] > 126) return false;
    return true;
  }
  if (publicChannel) return true;
  for (size_t i = 1; i < length; ++i)
    if (!((value.name[i] >= 'a' && value.name[i] <= 'z') ||
          (value.name[i] >= '0' && value.name[i] <= '9') || value.name[i] == '-' || value.name[i] == '_'))
      return false;
  return true;
}
constexpr const char *RadioSlots[] = {"/command-bot/radio-a.bin", "/command-bot/radio-b.bin"};
struct RadioReference {
  uint8_t magic[4]{'B','R','P',3}, slot = 0, digest[32]{};
  bool valid() const { return !memcmp(magic, "BRP\3", 4) && slot < 2; }
};
struct RadioRecord {
  uint8_t magic[4]{'B','R','C',1};
  BotRadioPolicy policy;
};
struct RadioWorkspace { RadioRecord record, check; };
struct RadioWorkspaceDeleter {
  void operator()(RadioWorkspace *value) const { releaseRoleStorage(value); }
};
using RadioStorage = std::unique_ptr<RadioWorkspace, RadioWorkspaceDeleter>;
static_assert(sizeof(RadioReference) == 37, "Keep native policy authority bounded");
bool readRadioFile(const RadioReference &reference, RadioRecord &record) {
  auto file = SPIFFS.open(RadioSlots[reference.slot], "r");
  const bool complete = file && file.size() == sizeof(record) &&
      file.read(reinterpret_cast<uint8_t *>(&record), sizeof(record)) == sizeof(record) &&
      file.size() == sizeof(record);
  file.close();
  uint8_t digest[32];
  mesh::Utils::sha256(digest, sizeof(digest), reinterpret_cast<const uint8_t *>(&record), sizeof(record));
  return (complete && !memcmp(record.magic, "BRC\1", 4) &&
          !memcmp(reference.digest, digest, sizeof(digest)) && record.policy.valid()) ||
         failed("radio policy file read/validation", ESP_ERR_INVALID_STATE);
}
}
bool BotRadioPolicy::valid() const {
  if (pathWidth < 1 || pathWidth > 3 || airtimeMs < 360 || airtimeMs > 3600) return false;
  for (auto flags : access) if (flags > All) return false;
  for (unsigned i = 0; i < ChannelLimit; ++i) {
    const auto value = membership(i);
    if (!validMembership(value)) return false;
    if (!value.name[0]) continue;
    mesh::GroupChannel current;
    nativeChannel(value, current);
    for (unsigned j = 0; j < i; ++j) {
      const auto other = membership(j);
      if (!other.name[0]) continue;
      mesh::GroupChannel previous;
      nativeChannel(other, previous);
      if (!strcmp(value.name, other.name) ||
          !memcmp(current.secret, previous.secret, sizeof(current.secret))) return false;
    }
  }
  for (unsigned i = 0; i < RuleLimit; ++i) {
    const auto &rule = rules[i];
    if (!rule.name[0]) continue;
    if (!memchr(rule.name, 0, sizeof(rule.name)) || rule.kind > 1 ||
        !(rule.kind ? botThreadName(rule.name, strlen(rule.name)) : botCommandName(rule.name)) ||
        rule.context > NativeContext || rule.access > All ||
        (rule.kind && (rule.access & ~(Read | Write)))) return false;
    unsigned threads = rule.kind;
    for (unsigned j = 0; j < i; ++j)
      if (rules[j].name[0] && rules[j].context == rule.context && rules[j].kind == rule.kind) {
        if (!strcmp(rules[j].name, rule.name)) return false;
        if (rule.kind && ++threads > BotThreadRuleLimit) return false;
      }
  }
  return true;
}
bool BotRadioPolicy::extended() const {
  if (!strcmp(channel, "Public")) return true;
  for (const auto &value : additional) if (value.name[0]) return true;
  for (auto flags : access) if (flags != All) return true;
  for (const auto &rule : rules) if (rule.name[0]) return true;
  return false;
}
int BotRadioPolicy::context(const BotEvent &event) const {
  if (event.kind != BotEvent::Command && event.kind != BotEvent::Message) return NativeContext;
  if (event.channel[0]) {
    if (!event.channelVerified || event.authenticated) return -1;
    for (unsigned i = 0; i < ChannelLimit; ++i) {
      const auto value = membership(i);
      if (!value.name[0]) continue;
      mesh::GroupChannel native;
      nativeChannel(value, native);
      uint8_t digest[32];
      static const uint8_t domain[] = "meshcore-bot-channel-v1";
      mesh::Utils::sha256(digest, sizeof(digest), domain, sizeof(domain) - 1,
                          native.secret, sizeof(native.secret));
      if (!memcmp(digest, event.channelId, sizeof(digest))) {
        return int(i + 1);
      }
    }
    return -1;
  }
  return event.authenticated && !event.local ? 0 : -1;
}
uint8_t BotRadioPolicy::flags(const BotEvent &event, const char *name) const {
  const int origin = context(event);
  if (origin < 0) return 0;
  for (const auto &rule : rules)
    if (rule.name[0] && !rule.kind && rule.context == origin && !strcmp(rule.name, name)) return rule.access;
  return access[origin];
}
void BotRadioPolicy::bindStorage(BotEvent &event) const {
  event.policyFlags = flags(event, event.name);
  for (auto &rule : event.threadRules) rule = {};
  const int origin = context(event);
  unsigned count = 0;
  for (const auto &rule : rules)
    if (rule.name[0] && rule.kind == 1 && rule.context == origin && count < BotThreadRuleLimit) {
      auto &bound = event.threadRules[count++];
      strcpy(bound.name, rule.name); bound.access = rule.access;
    }
}
bool loadBotRadioPolicy(BotRadioPolicy &policy) {
  policy = BotRadioPolicy{};
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
  if (size == sizeof(RadioReference) && !memcmp(record, "BRP\3", 4)) {
    RadioReference reference;
    memcpy(&reference, record, sizeof(reference));
    RadioStorage storage(allocateRoleStorage<RadioWorkspace>("radio policy read"));
    if (!reference.valid() || !storage || !readRadioFile(reference, storage->record))
      return failed("radio policy authority/file", ESP_ERR_INVALID_STATE);
    policy = storage->record.policy;
    return true;
  }
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
  if (!strcmp(policy.channel, "Public"))
    return failed("legacy Public policy has no native access masks", ESP_ERR_INVALID_STATE);
  if (!policy.valid()) return failed("policy validation", ESP_ERR_INVALID_STATE);
  return true;
}
bool saveBotRadioPolicy(const BotRadioPolicy &policy) {
  if (!policy.valid()) return failed("policy validation", ESP_ERR_INVALID_ARG);
  // Validate the current authority before replacing either its file or its reference.
  RadioStorage storage(allocateRoleStorage<RadioWorkspace>("radio policy save"));
  if (!storage || !loadBotRadioPolicy(storage->check.policy))
    return failed("radio policy saved authority unavailable", ESP_ERR_INVALID_STATE);
  RadioReference next;
  bool extended = policy.extended();
  if (extended) {
    bool present = false;
    nvs_handle_t previousHandle;
    auto result = nvs_open("mc-onchip", NVS_READONLY, &previousHandle);
    if (result != ESP_OK && result != ESP_ERR_NVS_NOT_FOUND) return failed("policy open", result);
    if (result == ESP_OK) {
      uint8_t previous[57]{};
      size_t size = sizeof(previous);
      result = nvs_get_blob(previousHandle, "bot-radio", previous, &size);
      nvs_close(previousHandle);
      if (result != ESP_OK && result != ESP_ERR_NVS_NOT_FOUND) return failed("policy read", result);
      present = result == ESP_OK;
      if (result == ESP_OK && size == sizeof(RadioReference) && !memcmp(previous, "BRP\3", 4))
        next.slot = 1 - previous[4];
    }
    nvs_stats_t stats{};
    const size_t required = BotCoreNvsReserveEntries + BotNvsMutationEntries + (present ? 0u : 5u);
    result = nvs_get_stats(nullptr, &stats);
    if (result != ESP_OK) return failed("radio policy NVS headroom read", result);
    if (stats.free_entries < required) {
      Serial.printf("Native command policy NVS headroom: %u free, %u required; policy not published\n",
                    unsigned(stats.free_entries), unsigned(required));
      return failed("radio policy NVS headroom; existing Lua data reserve required",
                    ESP_ERR_NVS_NOT_ENOUGH_SPACE);
    }
    storage->record.policy = policy;
    mesh::Utils::sha256(next.digest, sizeof(next.digest),
        reinterpret_cast<const uint8_t *>(&storage->record), sizeof(storage->record));
    auto file = SPIFFS.open(RadioSlots[next.slot], "w");
    if (!file) return failed("radio policy inactive file open", ESP_ERR_INVALID_STATE);
    const bool complete = file.write(reinterpret_cast<const uint8_t *>(&storage->record),
                                    sizeof(storage->record)) == sizeof(storage->record);
    file.flush(); file.close();
    if (!complete || !readRadioFile(next, storage->check) ||
        memcmp(&storage->record, &storage->check, sizeof(storage->record)))
      return failed("radio policy inactive file readback", ESP_ERR_INVALID_STATE);
  }
  uint8_t record[57]{};
  memcpy(record, "BRP\2", 4);
  memcpy(record + 4, policy.channel, strlen(policy.channel));
  record[37] = policy.pathWidth;
  record[38] = policy.airtimeMs & 255; record[39] = policy.airtimeMs >> 8;
  record[40] = policy.channelKeySet; memcpy(record + 41, policy.channelKey, 16);
  nvs_handle_t handle;
  auto result = nvs_open("mc-onchip", NVS_READWRITE, &handle);
  if (result != ESP_OK) return failed("policy open", result);
  result = nvs_set_blob(handle, "bot-radio", extended ? static_cast<const void *>(&next) : record,
                        extended ? sizeof(next) : sizeof(record));
  if (result == ESP_OK) result = nvs_commit(handle);
  nvs_close(handle);
  if (result != ESP_OK) return failed("policy commit", result);
  auto &readback = storage->check.policy;
  if (!loadBotRadioPolicy(readback) || strcmp(readback.channel, policy.channel) ||
      readback.pathWidth != policy.pathWidth || readback.airtimeMs != policy.airtimeMs ||
      readback.channelKeySet != policy.channelKeySet || memcmp(readback.channelKey, policy.channelKey, 16) ||
      memcmp(readback.additional, policy.additional, sizeof(policy.additional)) ||
      memcmp(readback.access, policy.access, sizeof(policy.access)) ||
      memcmp(readback.rules, policy.rules, sizeof(policy.rules)))
    return failed("policy readback", ESP_ERR_INVALID_STATE);
  return true;
}
bool botRadioPolicyCommand(BotRadioPolicy &policy, const char *command,
                          char *reply, size_t capacity, bool &changed) {
  changed = false;
  const auto error = [&](const char *text) {
    snprintf(reply, capacity, "Error: %s", text); return false;
  };
  const auto number = [](const char *text, unsigned maximum, unsigned &value) {
    value = 0;
    if (!text || !*text) return false;
    for (const char *p = text; *p; ++p) {
      if (*p < '0' || *p > '9' || value > maximum / 10) return false;
      value = value * 10 + unsigned(*p - '0');
      if (value > maximum) return false;
    }
    return true;
  };
  const auto hex = [](const char *text, uint8_t *output, size_t limit, size_t &size) {
    size = text ? strlen(text) : 0;
    if (!size || size % 2 || size > limit * 2) return false;
    for (size_t i = 0; i < size; ++i) {
      const char c = text[i];
      const int digit = c >= '0' && c <= '9' ? c - '0' :
                        c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                        c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
      if (digit < 0) return false;
      if (!(i % 2)) output[i / 2] = uint8_t(digit << 4);
      else output[i / 2] |= uint8_t(digit);
    }
    size /= 2;
    return true;
  };
  if (!command || strlen(command) > 160) return error("native policy command exceeds 160 bytes");
  char copy[161];
  strcpy(copy, command);
  char *parts[6]{}, *save = nullptr;
  unsigned count = 0;
  for (char *part = strtok_r(copy, " ", &save); part; part = strtok_r(nullptr, " ", &save)) {
    if (count == 6) return error("too many native policy arguments");
    parts[count++] = part;
  }
  if (!count || (strcmp(parts[0], "membership") && strcmp(parts[0], "access") && strcmp(parts[0], "thread")))
    return error("use membership SLOT; access dm|native|SLOT; thread dm|native|SLOT NAME [0|16|32|48|inherit]");
  const bool membership = !strcmp(parts[0], "membership");
  const bool thread = !strcmp(parts[0], "thread");
  if (count < 2) {
    snprintf(reply, capacity, membership ?
        "membership SLOT 0..7 [off|public|#tag|private NAMEHEX KEY32]; slot 0 is legacy; one Public; PSKs hidden" :
        thread ? "thread dm|native|SLOT NAME [0|16|32|48|inherit]; list OFFSET; at most 8 thread overrides/context" :
        "access dm|native|SLOT [default|COMMAND|action_NAME [MASK|inherit]]; list OFFSET; mask 0..63");
    return true;
  }
  unsigned context = 0;
  if (!membership && !strcmp(parts[1], "native")) context = BotRadioPolicy::NativeContext;
  else if (membership || strcmp(parts[1], "dm")) {
    if (!number(parts[1], BotRadioPolicy::ChannelLimit - 1, context))
      return error("membership slot requires 0..7; access/thread also accept dm or native");
    if (!membership) ++context;
  }
  if (membership) {
    const auto previous = policy.membership(context);
    if (count == 2) {
      snprintf(reply, capacity, "Membership %u name=%s type=%s; access %u default=%u",
          context, previous.name[0] ? previous.name : "off",
          previous.keySet ? "private" : !strcmp(previous.name, "Public") ? "public" : "hashtag",
          context, policy.access[context + 1]);
      return true;
    }
    BotRadioPolicy::Membership value;
    if (count == 3) {
      if (!strcmp(parts[2], "public")) strcpy(value.name, "Public");
      else if (strcmp(parts[2], "off")) {
        if (parts[2][0] != '#' || strlen(parts[2]) >= sizeof(value.name))
          return error("use off, public, #tag or private NAMEHEX KEY32");
        strcpy(value.name, parts[2]);
      }
    } else if (count == 5 && !strcmp(parts[2], "private")) {
      size_t size;
      if (!hex(parts[3], reinterpret_cast<uint8_t *>(value.name), sizeof(value.name) - 1, size))
        return error("private membership needs a printable NAMEHEX up to 32 bytes");
      for (size_t i = 0; i < size; ++i)
        if (value.name[i] < 32 || value.name[i] > 126)
          return error("private channel name must be printable");
      if (!hex(parts[4], value.key, sizeof(value.key), size) || size != sizeof(value.key))
        return error("private membership needs NAMEHEX up to 32 bytes and KEY32 exactly 16 bytes");
      value.keySet = true;
    } else return error("use membership SLOT off|public|#tag|private NAMEHEX KEY32");
    if (!validMembership(value)) return error("invalid channel name or zero private key");
    policy.setMembership(context, value);
    if (!policy.valid()) return error("duplicate channel name/key or Public membership");
    changed = true;
    return true;
  }
  if (count == 2) {
    unsigned rules = 0;
    for (const auto &rule : policy.rules)
      rules += rule.name[0] && rule.context == context && rule.kind == unsigned(thread);
    snprintf(reply, capacity, "Access context=%s default=%u overrides=%u; list OFFSET; bits execute/reply bare=1/2 addressed=4/8 read/write=16/32",
             parts[1], policy.access[context], rules);
    return true;
  }
  if (!strcmp(parts[2], "list")) {
    unsigned offset;
    if (count != 4 || !number(parts[3], BotRadioPolicy::RuleLimit, offset))
      return error("use access CONTEXT list OFFSET 0..64");
    unsigned seen = 0, returned = 0;
    snprintf(reply, capacity, "Access %s default=%u", parts[1], policy.access[context]);
    for (const auto &rule : policy.rules)
      if (rule.name[0] && rule.context == context && rule.kind == unsigned(thread)) {
      if (seen++ < offset || returned == 4) continue;
      const size_t used = strlen(reply);
      snprintf(reply + used, capacity > used ? capacity - used : 0, " %s=%u", rule.name, rule.access);
      ++returned;
    }
    const size_t used = strlen(reply);
    snprintf(reply + used, capacity > used ? capacity - used : 0, "; next=%u", offset + returned);
    return true;
  }
  const bool defaults = !thread && !strcmp(parts[2], "default");
  if (!defaults && !(thread ? botThreadName(parts[2], strlen(parts[2])) : botCommandName(parts[2])))
    return error("invalid command/action or thread name");
  BotRadioPolicy::Rule *found = nullptr, *empty = nullptr;
  unsigned threadCount = 0;
  for (auto &rule : policy.rules) {
    if (!rule.name[0] && !empty) empty = &rule;
    if (rule.name[0] && rule.context == context && rule.kind == unsigned(thread)) {
      threadCount += thread;
      if (!strcmp(rule.name, parts[2])) found = &rule;
    }
  }
  if (count == 3) {
    snprintf(reply, capacity, "Access %s %s=%u %s", parts[1], parts[2],
             found ? found->access : thread ? (policy.access[context] & 48) : policy.access[context],
             found ? "override" : "default");
    return true;
  }
  if (count != 4) return error("use access CONTEXT default|COMMAND MASK|inherit");
  if (!strcmp(parts[3], "inherit")) {
    if (defaults) return error("default requires an explicit mask");
    if (found) *found = {};
  } else {
    unsigned flags;
    if (!number(parts[3], BotRadioPolicy::All, flags)) return error("access mask requires 0..63");
    if (thread && (flags & ~48u)) return error("thread mask requires 0,16,32 or 48");
    if (defaults) policy.access[context] = uint8_t(flags);
    else {
      if (!found && thread && threadCount == BotThreadRuleLimit)
        return error("thread override limit is 8/context; remove one with inherit");
      if (!found) found = empty;
      if (!found) return error("native policy command override limit is 64; remove an override with inherit");
      strcpy(found->name, parts[2]); found->context = uint8_t(context);
      found->access = uint8_t(flags); found->kind = uint8_t(thread);
    }
  }
  changed = true;
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
bool BotTargetAliases::valid() const {
  bool ended = false;
  for (unsigned i = 0; i < 4; ++i) {
    const size_t size = strnlen(names[i], sizeof(names[i]));
    if (size == sizeof(names[i])) return false;
    if (!size) { ended = true; continue; }
    if (ended || names[i][0] < 'a' || names[i][0] > 'z') return false;
    bool hex = true;
    for (size_t j = 0; j < size; ++j) {
      const char c = names[i][j];
      if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_'))
        return false;
      hex = hex && ((c >= 'a' && c <= 'f') || (c >= '0' && c <= '9'));
    }
    if (size == 8 && hex) return false;
    for (unsigned j = 0; j < i; ++j)
      if (!strcmp(names[i], names[j])) return false;
  }
  return true;
}
bool BotTargetAliases::matches(const char *text, size_t size) const {
  if (size >= 2 && text[0] == '[' && text[size - 1] == ']') {
    ++text;
    size -= 2;
  }
  for (const auto &name : names) {
    if (!size || strlen(name) != size) continue;
    size_t i = 0;
    for (; i < size; ++i) {
      char c = text[i];
      if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
      if (c != name[i]) break;
    }
    if (i == size) return true;
  }
  return false;
}
bool loadBotTargetAliases(BotTargetAliases &aliases) {
  aliases = {};
  nvs_handle_t handle;
  auto result = nvs_open("mc-onchip", NVS_READONLY, &handle);
  if (result == ESP_ERR_NVS_NOT_FOUND) return true;
  if (result != ESP_OK) return failed("target aliases open", result);
  uint8_t record[4 + sizeof(aliases.names)]{};
  size_t size = sizeof(record);
  result = nvs_get_blob(handle, "bot-aliases", record, &size);
  nvs_close(handle);
  if (result == ESP_ERR_NVS_NOT_FOUND) return true;
  if (result != ESP_OK || size != sizeof(record) || memcmp(record, "BTA\1", 4))
    return failed("target aliases read/shape", ESP_ERR_INVALID_STATE);
  memcpy(aliases.names, record + 4, sizeof(aliases.names));
  if (aliases.valid()) return true;
  aliases = {};
  return failed("target aliases validation", ESP_ERR_INVALID_STATE);
}
bool saveBotTargetAliases(const BotTargetAliases &aliases) {
  if (!aliases.valid()) return failed("target aliases validation", ESP_ERR_INVALID_ARG);
  uint8_t record[4 + sizeof(aliases.names)]{};
  memcpy(record, "BTA\1", 4);
  memcpy(record + 4, aliases.names, sizeof(aliases.names));
  nvs_handle_t handle;
  auto result = nvs_open("mc-onchip", NVS_READWRITE, &handle);
  if (result != ESP_OK) return failed("target aliases open", result);
  result = nvs_set_blob(handle, "bot-aliases", record, sizeof(record));
  if (result == ESP_OK) result = nvs_commit(handle);
  nvs_close(handle);
  if (result != ESP_OK) return failed("target aliases commit; outcome unknown", result);
  BotTargetAliases actual;
  return (loadBotTargetAliases(actual) && !memcmp(actual.names, aliases.names, sizeof(aliases.names))) ||
      failed("target aliases readback; outcome unknown", ESP_ERR_INVALID_STATE);
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
