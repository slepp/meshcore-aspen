// SPDX-License-Identifier: Apache-2.0
#include "BotSettings.h"
#include <Mesh.h>
#include <SPIFFS.h>
#include <Utils.h>
#include <esp_heap_caps.h>
#include <nvs.h>
#include <cassert>
#include <cstring>

using namespace onchip;
unsigned long millis() { return 0; }
void delay(unsigned long) {}
static const identity_test::Key Key{"mc-onchip", "bot-radio"};
static void clearFaults() {
  identity_test::failRead = identity_test::failWrite = identity_test::failCommit = false;
  identity_test::afterCommit = nullptr;
  psram_test::failAfter = -1;
  filesystem_test::failOpen = false;
  filesystem_test::writeLimit = filesystem_test::readLimit = SIZE_MAX;
  filesystem_test::afterFlush = nullptr;
}
static BotEvent channelEvent(const BotRadioPolicy &policy, unsigned slot, bool targeted = false) {
  BotEvent event;
  const auto membership = policy.membership(slot);
  strcpy(event.channel, membership.name);
  event.channelVerified = true;
  event.targeted = targeted;
  mesh::GroupChannel native;
  BotRadioPolicy::nativeChannel(membership, native);
  static const uint8_t domain[] = "meshcore-bot-channel-v1";
  mesh::Utils::sha256(event.channelId, sizeof(event.channelId), domain, sizeof(domain) - 1,
                      native.secret, sizeof(native.secret));
  return event;
}
int main() {
  BotRadioPolicy policy;
  assert(loadBotRadioPolicy(policy) && policy.valid() && !policy.extended());
  assert(saveBotRadioPolicy(policy) && identity_test::durable.at(Key).size() == 57);
  auto legacy = identity_test::durable.at(Key);
  legacy.resize(40); legacy[3] = 1;
  memcpy(legacy.data() + 4, "#legacy", 7);
  identity_test::durable[Key] = legacy;
  assert(loadBotRadioPolicy(policy) && !strcmp(policy.channel, "#legacy") && !policy.extended());
  char reply[162]{};
  bool changed;
  const auto command = [&](const char *text, bool ok = true) {
    changed = false;
    assert(botRadioPolicyCommand(policy, text, reply, sizeof(reply), changed) == ok);
    if (!ok) assert(!strncmp(reply, "Error:", 6));
  };
  command("membership 1 #other"); assert(changed);
  command("membership 2 public"); assert(changed && policy.access[3] == 0);
  command("membership 3 private 50726976617465 0102030405060708090a0b0c0d0e0f10");
  assert(changed && policy.valid() && policy.extended());
  command("membership 3");
  assert(strstr(reply, "name=Private type=private") && !strstr(reply, "010203"));
  BotEvent dm; dm.authenticated = true; dm.sender[0] = 7;
  assert(policy.flags(dm, "ping") == BotRadioPolicy::All);
  assert(policy.flags(channelEvent(policy, 0), "ping") == BotRadioPolicy::All);
  assert(policy.flags(channelEvent(policy, 2), "ping") == 0);
  command("access 2 ping 12");
  assert(policy.flags(channelEvent(policy, 2, true), "ping") == 12);
  assert(policy.flags(channelEvent(policy, 2), "other") == 0);
  command("access dm default 16");
  command("access dm ping 63");
  assert(policy.flags(dm, "ping") == 63 && policy.flags(dm, "other") == 16);
  command("access dm list 0"); assert(strstr(reply, "ping=63") && strstr(reply, "next=1"));
  command("access dm ping inherit"); assert(policy.flags(dm, "ping") == 16);
  command("thread dm notes 16");
  policy.bindStorage(dm);
  assert(!strcmp(dm.threadRules[0].name, "notes") && dm.threadRules[0].access == 16);
  command("thread dm notes 17", false);
  command("thread dm notes inherit");
  for (unsigned i = 0; i < BotThreadRuleLimit; ++i) {
    char text[64]; snprintf(text, sizeof(text), "thread dm thread%u 48", i);
    command(text);
  }
  command("thread dm extra 0", false);
  command("thread dm thread0 inherit");
  command("thread dm extra 0");
  command("access native default 16");
  BotEvent scheduled; scheduled.kind = BotEvent::Scheduled;
  assert(policy.flags(scheduled, "") == 16);
  command("thread native monitor 32");
  policy.bindStorage(scheduled);
  assert(scheduled.policyFlags == 16 && !strcmp(scheduled.threadRules[0].name, "monitor") &&
         scheduled.threadRules[0].access == 32);
  auto spoof = channelEvent(policy, 1); spoof.channelId[31] ^= 1;
  assert(policy.flags(spoof, "ping") == 0);
  assert(saveBotRadioPolicy(policy));
  assert(identity_test::durable.at(Key).size() == 37 &&
         !memcmp(identity_test::durable.at(Key).data(), "BRP\3", 4));
  BotRadioPolicy actual;
  assert(loadBotRadioPolicy(actual) && actual.valid() &&
         !strcmp(actual.membership(3).name, "Private") &&
         actual.flags(channelEvent(actual, 2), "ping") == 12 && actual.access[0] == 16);
  actual.bindStorage(scheduled);
  assert(scheduled.policyFlags == 16 && !strcmp(scheduled.threadRules[0].name, "monitor"));
  auto next = policy; next.access[0] = 0;
  for (unsigned fault = 0; fault < 8; ++fault) {
    const auto authority = identity_test::durable.at(Key);
    switch (fault) {
      case 0: filesystem_test::failOpen = true; break;
      case 1: filesystem_test::writeLimit = 1; break;
      case 2: filesystem_test::readLimit = 1; break;
      case 3: filesystem_test::afterFlush = [] {
        filesystem_test::files.at("/command-bot/radio-b.bin")[30] ^= 1;
      }; break;
      case 4: identity_test::failWrite = true; break;
      case 5: identity_test::failCommit = true; break;
      case 6: identity_test::failRead = true; break;
      case 7: psram_test::failAfter = 0; break;
    }
    assert(!saveBotRadioPolicy(next) && identity_test::durable.at(Key) == authority);
    clearFaults();
    assert(loadBotRadioPolicy(actual) && actual.access[0] == 16);
  }
  assert(saveBotRadioPolicy(next) && loadBotRadioPolicy(actual) && actual.access[0] == 0);
  command("membership 7 public", false);
  policy = actual;
  command("membership 3 private 410042 0102030405060708090a0b0c0d0e0f10", false);
  command("access dm ping 64", false);
  command("access dm ping -1", false);
  command("access 8 ping 0", false);
  command("access dm default inherit", false);
  for (unsigned i = 0; i < BotRadioPolicy::RuleLimit; ++i) {
    policy.rules[i] = {};
    snprintf(policy.rules[i].name, sizeof(policy.rules[i].name), "command%u", i);
    policy.rules[i].context = 0;
    policy.rules[i].access = 0;
  }
  command("access dm extra 0", false);
  command("access dm command0 inherit");
  command("access dm extra 0"); assert(changed && policy.valid());
  const auto authority = identity_test::durable.at(Key);
  filesystem_test::files.at("/command-bot/radio-b.bin")[30] ^= 1;
  assert(!loadBotRadioPolicy(actual) && !saveBotRadioPolicy(policy));
  assert(identity_test::durable.at(Key) == authority);
  identity_test::durable.clear(); filesystem_test::files.clear();
  assert(saveBotRadioPolicy({}) && loadBotRadioPolicy(actual) && !actual.extended());
  assert(identity_test::handles.empty() && psram_test::allocations.empty());
  printf("PASS native radio policy: 8 memberships, singleton Public deny default, 64 command/action overrides, full channel digest, legacy v1/v2, 37-byte authority and interrupted-save retention; policy=%u bytes\n",
         unsigned(sizeof(BotRadioPolicy)));
}
