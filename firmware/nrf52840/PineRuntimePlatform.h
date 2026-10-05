// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <Identity.h>
namespace nrfmast {
void setLuaIdentity(const mesh::LocalIdentity &identity);
bool secureRandom(uint8_t *bytes, size_t size);
enum class LuaTimeSource : uint8_t { None, Admin, BleCompanion, Gps, RadioProvider };
bool trustLuaTime(uint32_t utc, LuaTimeSource source = LuaTimeSource::Admin);
void pollLuaTime();
const char* luaTimeSource();
bool trustRadioTime(uint32_t lower, uint32_t upper, uint32_t lifetimeMs);
void revokeRadioTime();
bool saveLuaCarrierName(const char *name);
}
