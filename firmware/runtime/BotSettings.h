// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>

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
struct BotForwardPolicy {
  uint8_t from[32]{}, to[32]{};
  bool enabled() const;
  bool valid() const;
};
bool loadBotForwardPolicy(BotForwardPolicy &policy);
bool saveBotForwardPolicy(const BotForwardPolicy &policy);
struct BotRadioPolicy {
  char channel[33]{};
  uint8_t pathWidth = 1;
  uint16_t airtimeMs = 360;
  bool channelKeySet = false;
  uint8_t channelKey[16]{};
  bool valid() const;
};
// Native authenticated administration only; applied on the next bot startup.
bool loadBotRadioPolicy(BotRadioPolicy &policy);
bool saveBotRadioPolicy(const BotRadioPolicy &policy);
struct BotMeshPolicy {
  char name[33]{};
  uint8_t destinations[4][32]{};
  bool channelWait = false;
  bool valid() const;
  bool allows(const uint8_t key[32]) const;
};
bool loadBotMeshPolicy(BotMeshPolicy &policy);
bool saveBotMeshPolicy(const BotMeshPolicy &policy);
}
