// SPDX-License-Identifier: Apache-2.0
#include "BotSettings.h"
#include "BotJournal.h"
#include <SPIFFS.h>
#include <esp_heap_caps.h>
#include <Utils.h>
#include <cassert>
#include <cstring>

using namespace onchip;
unsigned long millis() { return 0; }
void delay(unsigned long) {}
static const identity_test::Key Key{"mc-onchip", "bot-repeaters"};
static void clearFaults() {
  identity_test::failRead = identity_test::failWrite = identity_test::failCommit = false;
  identity_test::afterCommit = nullptr;
  psram_test::failAfter = -1;
  filesystem_test::failOpen = false;
  filesystem_test::writeLimit = filesystem_test::readLimit = SIZE_MAX;
  filesystem_test::afterFlush = nullptr;
}
static BotRepeaterPolicy fixture() {
  BotRepeaterPolicy policy;
  policy.enabled = true;
  for (unsigned i = 0; i < BotRepeaterLimit; ++i) {
    auto &target = policy.targets[i];
    snprintf(target.alias, sizeof(target.alias), "peer%u", i);
    target.key[0] = i + 1;
    target.frequencyHz = 912525000;
    target.lastFloodUtc = 1800000000 + i;
    target.used = true;
  }
  assert(policy.valid());
  return policy;
}
static void expect(const BotRepeaterPolicy &policy) {
  BotRepeaterPolicy actual;
  assert(loadBotRepeaterPolicy(actual));
  assert(actual.enabled == policy.enabled && actual.intervalSeconds == policy.intervalSeconds &&
         actual.discoverySeconds == policy.discoverySeconds && actual.disabledTargets == policy.disabledTargets);
  for (unsigned i = 0; i < BotRepeaterGroupLimit; ++i)
    assert(!strcmp(actual.groups[i].name, policy.groups[i].name) && actual.groups[i].members == policy.groups[i].members);
  for (unsigned i = 0; i < BotRepeaterLimit; ++i)
    assert(!strcmp(actual.targets[i].alias, policy.targets[i].alias) &&
           !memcmp(actual.targets[i].key, policy.targets[i].key, 32) &&
           actual.targets[i].lastFloodUtc == policy.targets[i].lastFloodUtc);
  assert(identity_test::handles.empty());
}
int main() {
  char status[162]{}, error[128]{};
  BotRepeaterPolicy empty;
  assert(loadBotRepeaterPolicy(empty) && !empty.enabled && empty.discoverySeconds == 900);
  empty.discoverySeconds = 59; assert(!empty.valid());
  empty.discoverySeconds = 86401; assert(!empty.valid());
  empty.discoverySeconds = 86400; assert(empty.valid());
  botRepeaterStorageStatus(status, sizeof(status));
  assert(strstr(status, "required=202 authority=none"));
  auto old = fixture(), next = old;
  assert(saveBotRepeaterPolicy(old));
  botRepeaterStorageStatus(status, sizeof(status));
  assert(strstr(status, "required=197 authority=saved"));
  assert(identity_test::durable.at(Key).size() == 37);
  expect(old);
  next.intervalSeconds = 600;
  next.discoverySeconds = 1800;
  next.disabledTargets = 1;
  for (unsigned fault = 0; fault < 8; ++fault) {
    const auto authority = identity_test::durable.at(Key);
    switch (fault) {
      case 0: filesystem_test::failOpen = true; break;
      case 1: filesystem_test::writeLimit = 1; break;
      case 2: filesystem_test::readLimit = 1; break;
      case 3: filesystem_test::afterFlush = [] {
        filesystem_test::files.at("/repeaters-b.bin")[20] ^= 1;
      }; break;
      case 4: identity_test::failWrite = true; break;
      case 5: identity_test::failCommit = true; break;
      case 6: identity_test::failRead = true; break;
      case 7: psram_test::failAfter = 0; break;
    }
    assert(!saveBotRepeaterPolicy(next, error, sizeof(error)) && error[0]);
    assert(identity_test::durable.at(Key) == authority);
    clearFaults(); expect(old);
  }
  identity_test::eagerWrites = true;
  identity_test::failCommit = true;
  assert(!saveBotRepeaterPolicy(next));
  clearFaults(); expect(next);
  identity_test::eagerWrites = false;
  assert(saveBotRepeaterPolicy(old)); expect(old);
  identity_test::afterCommit = [] { identity_test::failRead = true; };
  assert(!saveBotRepeaterPolicy(next));
  clearFaults(); expect(next);
  assert(saveBotRepeaterPolicy(old)); expect(old);
  assert(filesystem_test::files.size() == 2);
  const auto authority = identity_test::durable.at(Key);
  filesystem_test::files.at("/repeaters-a.bin")[20] ^= 1;
  assert(!loadBotRepeaterPolicy(empty) && !empty.enabled);
  assert(!saveBotRepeaterPolicy(next));
  assert(identity_test::durable.at(Key) == authority);
  identity_test::durable.clear(); filesystem_test::files.clear();
  identity_test::freeEntries = BotKvPublicEntries + 4;
  assert(!saveBotRepeaterPolicy(old, error, sizeof(error)) && identity_test::durable.empty() &&
         filesystem_test::files.empty());
  assert(strstr(error, "201 free, 202 required"));
  identity_test::freeEntries++;
  assert(saveBotRepeaterPolicy(old, error, sizeof(error)) && !error[0]);
  botRepeaterStorageStatus(status, sizeof(status));
  assert(strstr(status, "required=197 authority=saved"));
  const auto savedFiles = filesystem_test::files;
  const auto savedAuthority = identity_test::durable;
  identity_test::freeEntries = BotKvPublicEntries - 1;
  assert(!saveBotRepeaterPolicy(next, error, sizeof(error)) &&
         strstr(error, "196 free, 197 required"));
  assert(filesystem_test::files == savedFiles && identity_test::durable == savedAuthority);
  expect(old);
  identity_test::freeEntries++;
  assert(saveBotRepeaterPolicy(next, error, sizeof(error)) && !error[0]);
  expect(next);
  identity_test::freeEntries = 301;
  assert(saveBotRepeaterPolicy(old, error, sizeof(error)) && !error[0]);
  expect(old);
  identity_test::durable.clear(); filesystem_test::files.clear();
  const size_t legacySize = 4 + offsetof(BotRepeaterPolicy, discoverySeconds);
  auto &legacy = filesystem_test::files["/repeaters-a.bin"];
  legacy.resize(legacySize);
  memcpy(legacy.data(), "BRM\1", 4);
  memcpy(legacy.data() + 4, &old, legacySize - 4);
  auto &reference = identity_test::durable[Key];
  reference.resize(37);
  memcpy(reference.data(), "BRF\1", 4); reference[4] = 0;
  mesh::Utils::sha256(reference.data() + 5, 32, legacy.data(), legacy.size());
  expect(old);
  assert(!memcmp(legacy.data(), "BRM\1", 4));
  assert(saveBotRepeaterPolicy(next)); expect(next);
  assert(!memcmp(filesystem_test::files.at("/repeaters-b.bin").data(), "BRM\4", 4));
  identity_test::durable.clear(); filesystem_test::files.clear();
  auto &version2 = filesystem_test::files["/repeaters-a.bin"];
  const size_t version2Size = 4 + offsetof(BotRepeaterPolicy, disabledTargets);
  version2.resize(version2Size);
  memcpy(version2.data(), "BRM\2", 4);
  memcpy(version2.data() + 4, &old, version2Size - 4);
  auto &v2Reference = identity_test::durable[Key];
  v2Reference.resize(37);memcpy(v2Reference.data(), "BRF\1", 4);v2Reference[4] = 0;
  mesh::Utils::sha256(v2Reference.data() + 5, 32, version2.data(), version2.size());
  expect(old);
  assert(saveBotRepeaterPolicy(next));expect(next);
  // Aspen 0.1.18 wrote version 3 as its whole struct: the 2-byte disabledTargets field
  // plus 2 tail-padding bytes (arbitrary stack contents), 4 bytes past the new groups offset.
  identity_test::durable.clear(); filesystem_test::files.clear();
  auto &version3 = filesystem_test::files["/repeaters-a.bin"];
  const size_t version3Size = 4 + offsetof(BotRepeaterPolicy, disabledTargets) + 4;
  static_assert(offsetof(BotRepeaterPolicy, groups) == offsetof(BotRepeaterPolicy, disabledTargets) + 2,
                "Groups follow disabledTargets directly");
  version3.assign(version3Size, 0xA5);
  memcpy(version3.data(), "BRM\3", 4);
  memcpy(version3.data() + 4, &old, offsetof(BotRepeaterPolicy, disabledTargets) + 2);
  auto &v3Reference = identity_test::durable[Key];
  v3Reference.resize(37);memcpy(v3Reference.data(), "BRF\1", 4);v3Reference[4] = 0;
  mesh::Utils::sha256(v3Reference.data() + 5, 32, version3.data(), version3.size());
  expect(old);
  auto grouped = old;
  snprintf(grouped.groups[0].name, sizeof(grouped.groups[0].name), "all");
  grouped.groups[0].members = 0b11;
  assert(grouped.valid() && saveBotRepeaterPolicy(grouped)); expect(grouped);
  BotRepeaterPolicy badGroup = grouped;
  badGroup.groups[1] = badGroup.groups[0]; assert(!badGroup.valid());                     // duplicate name
  badGroup = grouped; badGroup.groups[0].members = 0; assert(!badGroup.valid());          // empty named group
  badGroup = grouped; badGroup.groups[0].members = uint16_t(1u << BotRepeaterLimit); assert(!badGroup.valid());
  badGroup = grouped; badGroup.targets[1] = {}; assert(!badGroup.valid());                // member slot unused
  badGroup = grouped; snprintf(badGroup.groups[0].name, sizeof(badGroup.groups[0].name), "%s", old.targets[0].alias);
  assert(!badGroup.valid());                                                              // group shares an alias
  badGroup = grouped; badGroup.groups[2].members = 1; assert(!badGroup.valid());          // members without a name
  BotRepeaterPolicy invalid;
  invalid.disabledTargets = 1;assert(!invalid.valid());
  invalid = old;invalid.disabledTargets = uint16_t(1u << BotRepeaterLimit);assert(!invalid.valid());
  assert(psram_test::allocations.empty());
  puts("PASS repeater storage: v1/v2/v3 migration and groups, persisted per-target disable and cooldowns, 37-byte authority, verified slots, explicit faults and unchanged Lua reserve");
}
