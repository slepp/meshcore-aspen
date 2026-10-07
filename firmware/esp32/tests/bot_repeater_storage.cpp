// SPDX-License-Identifier: Apache-2.0
#include "BotSettings.h"
#include "BotJournal.h"
#include <SPIFFS.h>
#include <esp_heap_caps.h>
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
  assert(actual.enabled == policy.enabled && actual.intervalSeconds == policy.intervalSeconds);
  for (unsigned i = 0; i < BotRepeaterLimit; ++i)
    assert(!strcmp(actual.targets[i].alias, policy.targets[i].alias) &&
           !memcmp(actual.targets[i].key, policy.targets[i].key, 32) &&
           actual.targets[i].lastFloodUtc == policy.targets[i].lastFloodUtc);
  assert(identity_test::handles.empty());
}
int main() {
  char status[162]{}, error[128]{};
  BotRepeaterPolicy empty;
  assert(loadBotRepeaterPolicy(empty) && !empty.enabled);
  botRepeaterStorageStatus(status, sizeof(status));
  assert(strstr(status, "required=313 authority=none"));
  auto old = fixture(), next = old;
  assert(saveBotRepeaterPolicy(old));
  botRepeaterStorageStatus(status, sizeof(status));
  assert(strstr(status, "required=308 authority=saved"));
  assert(identity_test::durable.at(Key).size() == 37);
  expect(old);
  next.intervalSeconds = 600;
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
  identity_test::freeEntries = BotCoreNvsReserveEntries + BotNvsMutationEntries + 4;
  assert(!saveBotRepeaterPolicy(old, error, sizeof(error)) && identity_test::durable.empty() &&
         filesystem_test::files.empty());
  assert(strstr(error, "312 free, 313 required"));
  identity_test::freeEntries++;
  assert(saveBotRepeaterPolicy(old, error, sizeof(error)) && !error[0]);
  assert(psram_test::allocations.empty());
  puts("PASS repeater storage: 37-byte authority, two verified slots, full policy/cooldown retention, explicit storage faults and unchanged Lua reserve");
}
