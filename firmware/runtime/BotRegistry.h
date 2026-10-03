// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "BotTypes.h"
#include <cassert>
#include <cstring>

namespace onchip {
bool botSourceIsWasm(const char *source, size_t size);
constexpr unsigned BotCommandLimit = 8, BotParameterLimit = 4;
constexpr unsigned BotModuleLimit = 8;
constexpr unsigned BotBuiltinCommandLimit = 30;
struct BotCommand {
  enum Permission : uint8_t { Public, Private, Owner, Channel, Shared, Reminder, Home } permission = Public;
  char name[BotNameLimit + 1]{}, function[BotNameLimit + 1]{};
  char schema[97]{}, help[65]{}, example[65]{};
};
struct BotManifestView {
  const BotCommand *commands;
  unsigned capacity;
  uint8_t count = 0;
  char modules[BotModuleLimit][BotNameLimit + 1]{};
  uint8_t moduleCount = 0;
  char events[4][BotNameLimit + 1]{};
  uint8_t eventMask = 0;
  constexpr BotManifestView(const BotCommand *entries, unsigned limit, unsigned used = 0)
      : commands(entries), capacity(limit), count(used) {}
  const BotCommand *find(const char *name) const;
  bool isBundled() const {
    const auto *ping = find("ping");
    return ping && !strcmp(ping->function, "ping");
  }
  BotCommand *writable() { return const_cast<BotCommand *>(commands); }
  void clear() {
    for (unsigned i = 0; i < capacity; ++i) writable()[i] = {};
    count = moduleCount = eventMask = 0;
    memset(modules, 0, sizeof(modules)); memset(events, 0, sizeof(events));
  }
};
template <unsigned Capacity> struct BotManifestStorage : BotManifestView {
  BotCommand storage[Capacity]{};
  BotManifestStorage() : BotManifestView(storage, Capacity) {}
  BotManifestStorage(const BotManifestView &other) : BotManifestStorage() { *this = other; }
  BotManifestStorage(const BotManifestStorage &other) : BotManifestStorage() { *this = other; }
  BotManifestStorage &operator=(const BotManifestView &other) {
    assert(other.count <= Capacity);
    if (this == &other) return *this;
    if (commands != other.commands) {
      clear();
      if (other.count) memcpy(storage, other.commands, other.count * sizeof(BotCommand));
    }
    count = other.count; moduleCount = other.moduleCount; eventMask = other.eventMask;
    memcpy(modules, other.modules, sizeof(modules)); memcpy(events, other.events, sizeof(events));
    return *this;
  }
  BotManifestStorage &operator=(const BotManifestStorage &other) {
    return *this = static_cast<const BotManifestView &>(other);
  }
};
using BotManifest = BotManifestStorage<BotBuiltinCommandLimit>;
using BotProgramManifest = BotManifestStorage<BotCommandLimit>;
struct BotValue {
  enum Type : uint8_t { String, Integer, Boolean, Missing } type = Missing;
  char text[BotArgumentsLimit + 1]{};
  int32_t integer = 0;
};
struct BotArguments {
  BotValue values[BotParameterLimit]{};
  uint8_t count = 0;
};
bool botReservedCommand(const char *name);
bool botCommandAllowed(const BotCommand &, const BotEvent &);
bool botStorageScope(const BotEvent &, BotIoRequest &, char *error, size_t capacity);
bool botReadOnlyQuery(const char *name);
bool botIdentifier(const char *name);
bool botCommandName(const char *name);
bool validateBotSchema(const char *schema, char *error, size_t capacity);
bool parseBotArguments(const BotCommand &command, const char *text,
                       BotArguments &arguments, char *error, size_t capacity);
} // namespace onchip
