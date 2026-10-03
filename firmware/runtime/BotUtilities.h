// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "BotTypes.h"

namespace onchip {
struct BotUtilityResult {
  char text[BotReplyLimit + 1]{}, error[128]{};
};
using BotUtilityRandom = bool (*)(uint32_t &);
bool botUtilityRandom(uint32_t &value);
bool botCalculate(const char *expression, BotUtilityResult &result);
bool botConvert(const char *value, const char *from, const char *to, BotUtilityResult &result);
bool botRoll(const char *dice, BotUtilityResult &result, BotUtilityRandom random = botUtilityRandom);
bool botChoose(const char *choices, BotUtilityResult &result, BotUtilityRandom random = botUtilityRandom);
extern const char BotUtilitySource[];
}
