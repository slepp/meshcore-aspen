// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "BotTypes.h"
#include <stddef.h>
#include <stdint.h>

namespace mesh { class GroupChannel; }
namespace onchip {
// Trusted native-management backend only. Selection takes effect at next boot;
// this independent record never changes the v1 role profile/replay journal.
bool loadBotEnabled(bool &enabled);
bool saveBotEnabled(bool enabled);
bool loadBotSharedState(bool &enabled);
bool saveBotSharedState(bool enabled);
bool loadBotHomeAccess(bool &enabled);
bool saveBotHomeAccess(bool enabled);
bool loadBotDiscovery(bool &enabled);
bool saveBotDiscovery(bool enabled);
bool loadBotAdaptiveAdmission(bool &enabled);
bool saveBotAdaptiveAdmission(bool enabled);
bool loadBotReminderAccess(bool &enabled);
bool saveBotReminderAccess(bool enabled);
bool loadBotEventAccess(uint8_t &mask);
bool saveBotEventAccess(uint8_t mask);
struct BotRepeaterPolicy {
  struct Target {
    char alias[17]{};
    uint8_t key[32]{};
    BotPath path{};
    uint32_t frequencyHz = 0;
    uint32_t lastFloodUtc = 0;
    bool used = false;
  } targets[BotRepeaterLimit];
  uint32_t intervalSeconds = 300;
  bool enabled = false;
  uint32_t discoverySeconds = 900;
  bool valid() const;
};
bool loadBotRepeaterPolicy(BotRepeaterPolicy &policy);
bool saveBotRepeaterPolicy(const BotRepeaterPolicy &policy, char *error = nullptr, size_t capacity = 0);
void botRepeaterStorageStatus(char *reply, size_t capacity);
struct BotForwardPolicy {
  uint8_t from[32]{}, to[32]{};
  bool enabled() const;
  bool valid() const;
};
bool loadBotForwardPolicy(BotForwardPolicy &policy);
bool saveBotForwardPolicy(const BotForwardPolicy &policy);
struct BotRadioPolicy {
  static constexpr unsigned ChannelLimit = 8, RuleLimit = 64, NativeContext = 9;
  enum Access : uint8_t {
    BareExecute = 1, BareReply = 2, AddressedExecute = 4, AddressedReply = 8,
    Read = 16, Write = 32, All = 63
  };
  struct Membership {
    char name[33]{};
    uint8_t key[16]{};
    bool keySet = false;
  };
  struct Rule {
    char name[BotNameLimit + 1]{};
    uint8_t context = 0, access = All, kind = 0;
  };
  char channel[33]{};
  uint8_t pathWidth = 1;
  uint16_t airtimeMs = 360;
  bool channelKeySet = false;
  uint8_t channelKey[16]{};
  Membership additional[ChannelLimit - 1]{};
  uint8_t access[NativeContext + 1]{All, All, All, All, All, All, All, All, All, All};
  Rule rules[RuleLimit]{};
  Membership membership(unsigned slot) const;
  bool setMembership(unsigned slot, const Membership &membership);
  bool extended() const;
  int context(const BotEvent &event) const;
  uint8_t flags(const BotEvent &event, const char *name) const;
  void bindStorage(BotEvent &event) const;
  static void nativeChannel(const Membership &membership, mesh::GroupChannel &channel);
  bool valid() const;
};
// Native authenticated administration only; applied on the next bot startup.
bool loadBotRadioPolicy(BotRadioPolicy &policy);
bool saveBotRadioPolicy(const BotRadioPolicy &policy);
bool botRadioPolicyCommand(BotRadioPolicy &policy, const char *command,
                           char *reply, size_t capacity, bool &changed);
struct BotMeshPolicy {
  char name[33]{};
  uint8_t destinations[4][32]{};
  bool channelWait = false;
  bool valid() const;
  bool allows(const uint8_t key[32]) const;
};
struct BotTargetAliases {
  char names[4][17]{};
  bool valid() const;
  bool matches(const char *text, size_t size) const;
};
bool loadBotTargetAliases(BotTargetAliases &aliases);
bool saveBotTargetAliases(const BotTargetAliases &aliases);
bool loadBotMeshPolicy(BotMeshPolicy &policy);
bool saveBotMeshPolicy(const BotMeshPolicy &policy);
}
