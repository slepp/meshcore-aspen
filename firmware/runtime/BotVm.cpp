// SPDX-License-Identifier: Apache-2.0
#if defined(MESHCORE_ONCHIP_BOT) && MESHCORE_ONCHIP_BOT
#include "BotVm.h"
#include "BotBuiltinManifest.h"
#if ONCHIP_BOT_WASM
#include "BotWasm.h"
#endif
#include "BotTimers.h"
#include "BotReminders.h"
#include "BotUtilities.h"
extern "C" {
#include <lua.h>
#include <lauxlib.h>
}
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <cmath>
#include <algorithm>
#include <array>
#include <initializer_list>
#include <new>
#include <memory>
#ifdef ONCHIP_BOT_VM_TEST
extern uint64_t onchipBotVmTestClock();
#endif
#ifdef ONCHIP_BOT_HEAP_MODEL
extern void onchipBotVmHeapModel(size_t oldSize, size_t newSize);
#endif
#ifdef ARDUINO_ARCH_ESP32
#include <esp_heap_caps.h>
#include <esp_timer.h>
#elif defined(NRF52_PLATFORM)
#include <Arduino.h>
#undef abs
#else
#include <chrono>
#endif

namespace {
static_assert(LUA_VERSION_RELEASE_NUM == 50501, "Command VM requires the checked Lua 5.5.1 sources");
static_assert(sizeof(lua_Integer) == 8 && sizeof(lua_Number) == 8,
              "Host and device require 64-bit Lua integers and double-precision numbers");
uint64_t clockUs() {
#ifdef ONCHIP_BOT_VM_TEST
  return onchipBotVmTestClock();
#elif defined(ARDUINO_ARCH_ESP32)
  return esp_timer_get_time();
#elif defined(NRF52_PLATFORM)
  return uint64_t(xTaskGetTickCount()) * 1000000u / configTICK_RATE_HZ;
#else
  return std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}
bool bundledSource(const char *source, size_t size) {
  return size == strlen(onchip::BotDefaultSource) &&
         !memcmp(source, onchip::BotDefaultSource, size);
}
onchip::BotVmLimits luaSourceLimits(const char *source, size_t size, onchip::BotVmLimits limits) {
  if (limits.loadWallUs == onchip::BotVmLimits{}.loadWallUs && bundledSource(source, size))
    limits.loadWallUs = onchip::BotBundledLoadWallUs;
  return limits;
}
struct Budget {
  enum Phase { Load, Init, Invoke, Cleanup };
  enum AllocationFailure { NoAllocationFailure, HeapLimit, AllocatorFailure };
  onchip::BotVmLimits limits;
  onchip::BotVmStats stats{};
  size_t live = 0;
  uint64_t started;
  uint64_t phaseStarted;
  Phase phase = Load;
  bool timedOut = false;
  AllocationFailure allocationFailure = NoAllocationFailure;
#ifdef NRF52_PLATFORM
  size_t failedOldSize = 0, failedSize = 0;
  unsigned failedFree = 0;
#endif
  Phase failedPhase = Load;
  Budget *active = nullptr;
  bool running = true;
  explicit Budget(onchip::BotVmLimits selected = {})
      : limits(selected), started(clockUs()), phaseStarted(started) {}
  uint64_t phaseLimit() const {
    const uint64_t maximum = phase == Load ? limits.loadWallUs :
                             phase == Init ? limits.initWallUs : limits.wallUs;
    const uint64_t used = phase == Load ? stats.loadUs : phase == Init ? stats.initUs :
                          phase == Invoke ? stats.invokeUs : stats.cleanupUs;
    return used < maximum ? maximum - used : 0;
  }
  bool expired() const { return timedOut || clockUs() - phaseStarted >= phaseLimit(); }
  const char *failurePhase() const {
    switch (failedPhase) {
    case Load: return "load";
    case Init: return "initialization";
    case Invoke: return "invocation";
    case Cleanup: return "cleanup";
    }
    return "unknown";
  }
  void transition(Phase next) {
    const auto now = clockUs();
    const auto elapsed = now - phaseStarted;
    if (!timedOut && elapsed >= phaseLimit()) {
      timedOut = true;
      failedPhase = phase;
    }
    switch (phase) {
    case Load: stats.loadUs += elapsed; break;
    case Init: stats.initUs += elapsed; break;
    case Invoke: stats.invokeUs += elapsed; break;
    case Cleanup: stats.cleanupUs += elapsed; break;
    }
    phase = next; phaseStarted = now;
    stats.elapsedUs = now - started;
  }
};
Budget &budget(lua_State *state);
void *allocate(void *context, void *pointer, size_t oldSize, size_t size) {
  auto &b = *static_cast<Budget *>(context);
  if (!pointer) oldSize = 0;
  if (!size) {
#ifdef ONCHIP_BOT_HEAP_MODEL
    if (pointer) onchipBotVmHeapModel(oldSize, 0);
#endif
#ifdef ARDUINO_ARCH_ESP32
    heap_caps_free(pointer);
#else
    free(pointer);
#endif
    b.live -= oldSize;
    return nullptr;
  }
  if (b.running && (b.active ? b.active->expired() : b.expired()))
    return nullptr;
  if (size > b.limits.heapBytes || b.live - oldSize > b.limits.heapBytes - size) {
    b.allocationFailure = Budget::HeapLimit;
    return nullptr;
  }
#ifdef NRF52_PLATFORM
  if (size > oldSize && size - oldSize + 8192u > unsigned(std::max(dbgHeapFree(), 0))) {
    b.allocationFailure = Budget::AllocatorFailure;
    b.failedOldSize = oldSize; b.failedSize = size;
    b.failedFree = unsigned(std::max(dbgHeapFree(), 0));
    return nullptr;
  }
#endif
#ifdef ARDUINO_ARCH_ESP32
  void *replacement = heap_caps_realloc(pointer, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
  void *replacement = realloc(pointer, size);
#endif
  if (replacement) {
#ifdef ONCHIP_BOT_HEAP_MODEL
    onchipBotVmHeapModel(oldSize, size);
#endif
    b.live = b.live - oldSize + size;
    if (b.live > b.stats.peakBytes) b.stats.peakBytes = b.live;
  } else {
    b.allocationFailure = Budget::AllocatorFailure;
#ifdef NRF52_PLATFORM
    b.failedOldSize = oldSize; b.failedSize = size;
    b.failedFree = unsigned(std::max(dbgHeapFree(), 0));
#endif
  }
  return replacement;
}
void hook(lua_State *state, lua_Debug *) {
  auto &b = budget(state);
  if (++b.stats.instructions > b.limits.instructions || b.expired())
    luaL_error(state, "command instruction/time budget exceeded");
}
int readonly(lua_State *state) {
  return luaL_error(state, "command event is read-only");
}
#if ONCHIP_BOT_COMPACT_PROFILE
int readonlyIndex(lua_State *state) {
  luaL_checkudata(state, 1, "onchip.bot.readonly");
  lua_getiuservalue(state, 1, 1);
  lua_pushvalue(state, 2);
  lua_gettable(state, -2);
  return 1;
}
#endif
void freeze(lua_State *state) {
  // A userdata proxy prevents writes to existing keys as well as new keys.
#if ONCHIP_BOT_COMPACT_PROFILE
  lua_newuserdatauv(state, 1, 1);
  lua_pushvalue(state, -2); lua_setiuservalue(state, -2, 1);
  if (luaL_newmetatable(state, "onchip.bot.readonly")) {
    lua_pushcfunction(state, readonlyIndex); lua_setfield(state, -2, "__index");
    lua_pushcfunction(state, readonly); lua_setfield(state, -2, "__newindex");
    lua_pushboolean(state, false); lua_setfield(state, -2, "__metatable");
  }
#else
  lua_newuserdatauv(state, 1, 0);
  lua_createtable(state, 0, 3);
  lua_pushvalue(state, -3);
  lua_setfield(state, -2, "__index");
  lua_pushcfunction(state, readonly);
  lua_setfield(state, -2, "__newindex");
  lua_pushboolean(state, 0);
  lua_setfield(state, -2, "__metatable");
#endif
  lua_setmetatable(state, -2);
  lua_remove(state, -2);
}
void text(lua_State *s, const char *key, const char *value) {
  lua_pushstring(s, value); lua_setfield(s, -2, key);
}
void number(lua_State *s, const char *key, lua_Number value) {
  lua_pushnumber(s, value); lua_setfield(s, -2, key);
}
void boolean(lua_State *s, const char *key, bool value) {
  lua_pushboolean(s, value); lua_setfield(s, -2, key);
}
bool rfText(const char *value, size_t limit) {
  if (!value[0] || strlen(value) > limit) return false;
  for (const auto *p = reinterpret_cast<const unsigned char *>(value); *p; ++p)
    if (*p < 32 || *p > 126) return false;
  return true;
}
int stringInteger(lua_State *s) {
  char value[32];
  if (lua_isinteger(s, 1))
    snprintf(value, sizeof(value), "%lld", static_cast<long long>(lua_tointeger(s, 1)));
  else if (lua_type(s, 1) == LUA_TNUMBER && std::isfinite(lua_tonumber(s, 1)) &&
           std::abs(lua_tonumber(s, 1)) <= 1e9)
    snprintf(value, sizeof(value), "%.6g", double(lua_tonumber(s, 1)));
  else
    return luaL_error(s, "tostring accepts bounded finite numbers only");
  lua_pushstring(s, value);
  return 1;
}
void integer(lua_State *s, const char *key, lua_Integer value) {
  lua_pushinteger(s, value); lua_setfield(s, -2, key);
}
void pushEvent(lua_State *s, const onchip::BotEvent &e) {
  lua_createtable(s, 0, 7);
  const char *kinds[] = {"command", "startup", "connectivity", "message", "node_status", "scheduled"};
  text(s, "kind", kinds[e.kind]);
  text(s, "message", e.message);
  boolean(s, "targeted", e.targeted);
  lua_newtable(s);
  boolean(s, "owner", e.owner); boolean(s, "shared", e.sharedState);
  boolean(s, "home", e.homeAccess); boolean(s, "reminders", e.reminderAccess);
  boolean(s, "channel_wait", e.channelWait);
  lua_newtable(s);
  unsigned destinations = 0;
  for (const auto &destination : e.destinations) {
    bool present = false;
    for (auto byte : destination) present = present || byte;
    if (!present) continue;
    char address[65];
    for (unsigned i = 0; i < 32; ++i) snprintf(address + 2 * i, 3, "%02x", destination[i]);
    lua_pushstring(s, address); lua_rawseti(s, -2, ++destinations);
  }
  freeze(s); lua_setfield(s, -2, "destinations");
  freeze(s); lua_setfield(s, -2, "grants");
  lua_createtable(s, 0, 2);
  char key[65];
  for (unsigned i = 0; i < 32; ++i) snprintf(key + 2 * i, 3, "%02x", e.sender[i]);
  if (e.authenticated) text(s, "public_key", key);
  boolean(s, "authenticated", e.authenticated);
  text(s, "nickname", e.nickname);
  freeze(s); lua_setfield(s, -2, "sender");
  lua_createtable(s, 0, 2);
  boolean(s, "present", e.channel[0] != 0);
  boolean(s, "verified", e.channelVerified);
  text(s, "name", e.channel);
  if (e.channelVerified) {
    for (unsigned i = 0; i < 32; ++i) snprintf(key + 2 * i, 3, "%02x", e.channelId[i]);
    text(s, "id", key);
  }
  freeze(s); lua_setfield(s, -2, "channel");
  lua_createtable(s, 0, 8);
  char path[4 + 2 * onchip::BotPathLimit]{};
  onchip::formatBotPath(e.path, path, sizeof(path));
  text(s, "path", path);
  boolean(s, "path_known", e.path.valid());
  integer(s, "path_width", e.path.width); integer(s, "path_count", e.path.count);
  integer(s, "timestamp", e.timestamp); integer(s, "route_type", e.routeType);
  boolean(s, "trace_available", e.routeSize != 0);
  boolean(s, "trace_explicit", e.routeExplicit);
  freeze(s); lua_setfield(s, -2, "packet");
  lua_createtable(s, 0, 9);
  boolean(s, "local_reflection", e.local); boolean(s, "measured", e.signal);
  // Quarter-dB values are also available without lossy display rounding.
  integer(s, "rssi", static_cast<int>(e.rssi));
  integer(s, "snr", static_cast<int>(e.snr));
  number(s, "rssi_dbm", e.rssi); number(s, "snr_db", e.snr);
  integer(s, "frequency_hz", e.frequency); integer(s, "bandwidth_hz", e.bandwidth);
  integer(s, "sf", e.sf); integer(s, "cr", e.cr);
  freeze(s); lua_setfield(s, -2, "radio");
  const auto &n = e.node;
  lua_createtable(s, 0, 15);
  boolean(s, "available", n.available); boolean(s, "enabled", n.enabled);
  boolean(s, "ready", n.ready); boolean(s, "fault", n.fault);
  boolean(s, "roles_known", n.rolesKnown);
  integer(s, "selected_roles", n.selectedRoles); integer(s, "ready_roles", n.readyRoles);
  boolean(s, "wifi_known", n.wifiKnown); boolean(s, "wifi_connected", n.wifiConnected);
  boolean(s, "battery_available", false);
  integer(s, "uptime_ms", n.uptimeMs);
  text(s, "native_revision", n.nativeRevision); text(s, "build", n.build);
  text(s, "name", n.name); integer(s, "source_generation", n.sourceGeneration);
  text(s, "lua_version", LUA_VERSION_MAJOR "." LUA_VERSION_MINOR "." LUA_VERSION_RELEASE);
  if (n.hasIdentity) {
    for (unsigned i = 0; i < 32; ++i) snprintf(key + 2 * i, 3, "%02x", n.publicKey[i]);
    text(s, "public_key", key);
  }
  freeze(s); lua_setfield(s, -2, "node");
  const auto &a = e.air;
  lua_createtable(s, 0, 18);
  boolean(s, "available", a.available); boolean(s, "transmitting", a.transmitting);
  integer(s, "queued", a.queued); integer(s, "aggregate_queued", a.aggregateQueued);
  integer(s, "generation", a.generation); integer(s, "configuration_generation", a.configurationGeneration);
  integer(s, "captured_ms", a.capturedMs);
  integer(s, "credit_ms", a.creditMs); integer(s, "aggregate_credit_ms", a.aggregateCreditMs);
  integer(s, "rf_ms", a.rfMs); integer(s, "aggregate_rf_ms", a.aggregateRfMs);
  integer(s, "successes", a.successes); integer(s, "failures", a.failures);
  integer(s, "aggregate_successes", a.aggregateSuccesses); integer(s, "aggregate_failures", a.aggregateFailures);
  integer(s, "reserved_ms", a.reservedMs); integer(s, "limit_ms", a.limitMs);
  integer(s, "remaining_ms", a.remainingMs);
  freeze(s); lua_setfield(s, -2, "air");
  lua_createtable(s, 0, 4);
  integer(s, "unique", e.observationCount); integer(s, "window_ms", e.windowMs);
  boolean(s, "truncated", e.truncated);
  integer(s, "capacity", onchip::BotObservationLimit);
  lua_createtable(s, e.observationCount, 0);
  for (unsigned i = 0; i < e.observationCount; ++i) {
    char observed[4 + 2 * onchip::BotPathLimit]{};
    onchip::formatBotPath(e.observations[i], observed, sizeof(observed));
    lua_pushstring(s, observed); lua_rawseti(s, -2, i + 1);
  }
  freeze(s); lua_setfield(s, -2, "paths");
  freeze(s); lua_setfield(s, -2, "observation");
  lua_createtable(s, 0, 4);
  integer(s, "reply_bytes", e.replyLimit);
  integer(s, "path_bytes", onchip::BotPathLimit);
  integer(s, "trace_bytes", onchip::BotTraceLimit);
  integer(s, "trace_hops", onchip::BotTraceHopLimit);
  freeze(s); lua_setfield(s, -2, "limits");
  freeze(s);
}
struct Call {
  const char *source;
  size_t size;
  const onchip::BotEvent *event;
  onchip::BotAction *action;
  onchip::BotManifestView *manifest = nullptr;
  const onchip::BotManifestView *installed = nullptr;
  bool bundled = false, initializing = true;
  Budget *budget = nullptr;
  bool retained = false;
  onchip::BotIoRequest *io = nullptr;
  const onchip::BotIoResult *completion = nullptr;
  uint32_t generation = 0, job = 0, operations = 0;
  uint32_t builtinDeclarations = 0;
  unsigned utilityCalls = 0;
  int eventReference = LUA_NOREF;
  int moduleReference = LUA_NOREF;
  int originalReference = LUA_NOREF;
  int sourceOwnersReference = LUA_NOREF;
  unsigned sourcePart = 0;
  bool buildingOriginals = false;
  bool moduleInitializing = false;
  Call *subscriptionOwner = nullptr;
  std::atomic<uint32_t> *eventEpoch = nullptr;
  bool replyRequested = false, replyCompleted = false;
  uint8_t tracesRemaining = 0, tracesCompleted = 0;
  char traceSummary[onchip::BotReplyLimit + 1]{};
  bool traceTruncated = false;
};
Call &context(lua_State *s) {
  return **static_cast<Call **>(lua_getextraspace(s));
}
Budget &budget(lua_State *s) { return *context(s).budget; }
int contextIndex(lua_State *s) {
  auto &call = context(s);
  if (call.initializing || !call.event) return luaL_error(s, "ctx requires an invocation");
  if (call.eventReference == LUA_NOREF) {
    pushEvent(s, *call.event);
    call.eventReference = luaL_ref(s, LUA_REGISTRYINDEX);
  }
  lua_rawgeti(s, LUA_REGISTRYINDEX, call.eventReference);
  lua_pushvalue(s, 2);
  lua_gettable(s, -2);
  return 1;
}
bool nativeName(const char *name) {
  for (const char *helper : {"reply", "command", "override_command", "call_original", "request_trace", "command_help",
                             "tostring", "ctx", "kv", "timer", "reminder", "sleep", "mesh", "advert", "rpc", "http", "json", "utility", "node",
                             "module", "require", "events", "repeater"})
    if (!strcmp(name, helper)) return true;
  return onchip::botReservedCommand(name);
}
int environmentWrite(lua_State *s) {
  auto &call = context(s);
  size_t size;
  const char *name = lua_type(s, 2) == LUA_TSTRING ? lua_tolstring(s, 2, &size) : nullptr;
  if (name && size == strlen(name) && nativeName(name) &&
      !(call.initializing && call.bundled && onchip::botReservedCommand(name)))
    return luaL_error(s, "native command/helper name is reserved");
  if (call.initializing && !call.bundled && call.sourcePart &&
      call.sourceOwnersReference != LUA_NOREF) {
    lua_rawgeti(s, LUA_REGISTRYINDEX, call.sourceOwnersReference);
    lua_pushvalue(s, 2); lua_rawget(s, -2);
    const unsigned owner = unsigned(lua_tointeger(s, -1));
    lua_pop(s, 1);
    if (owner && owner != call.sourcePart)
      return luaL_error(s, "Lua source global collision: %s", name ? name : "(non-string)");
    lua_pushvalue(s, 2); lua_pushinteger(s, call.sourcePart); lua_rawset(s, -3);
    lua_pop(s, 1);
  }
  lua_pushvalue(s, 2); lua_pushvalue(s, 3);
  lua_rawset(s, lua_upvalueindex(1));
  return 0;
}
struct PacketHandle {
  uint32_t generation, job, packet;
};
int packetIndex(lua_State *s) {
  const auto *packet = static_cast<const PacketHandle *>(luaL_checkudata(s, 1, "onchip.bot.packet"));
  auto &call = context(s);
  if (packet->generation != call.generation || packet->job != call.job)
    return luaL_error(s, "Packet belongs to a different invocation/generation");
  lua_getiuservalue(s, 1, 1);
  lua_pushvalue(s, 2); lua_gettable(s, -2);
  return 1;
}
void freezePacket(lua_State *s, const onchip::BotIoResult &result) {
  auto *handle = static_cast<PacketHandle *>(lua_newuserdatauv(s, sizeof(PacketHandle), 1));
  *handle = {result.token.generation, result.token.job, result.packet.id};
  lua_pushvalue(s, -2); lua_setiuservalue(s, -2, 1);
  if (luaL_newmetatable(s, "onchip.bot.packet")) {
    lua_pushcfunction(s, packetIndex); lua_setfield(s, -2, "__index");
    lua_pushcfunction(s, readonly); lua_setfield(s, -2, "__newindex");
    lua_pushboolean(s, false); lua_setfield(s, -2, "__metatable");
  }
  lua_setmetatable(s, -2);
  lua_remove(s, -2);
}
constexpr unsigned JsonDepthLimit = 8, JsonElementLimit = 64;
constexpr size_t JsonStringLimit = 1024;
char jsonNullKey, jsonArrayKey;
bool utf8(const unsigned char *text, size_t length, size_t &width) {
  if (!length) return false;
  const unsigned char first = text[0];
  if (first < 0x80) { width = 1; return true; }
  width = first >= 0xc2 && first <= 0xdf ? 2 :
          first >= 0xe0 && first <= 0xef ? 3 :
          first >= 0xf0 && first <= 0xf4 ? 4 : 0;
  if (!width || width > length) return false;
  for (size_t i = 1; i < width; ++i)
    if ((text[i] & 0xc0) != 0x80) return false;
  return !(width == 3 && ((first == 0xe0 && text[1] < 0xa0) ||
                          (first == 0xed && text[1] >= 0xa0))) &&
         !(width == 4 && ((first == 0xf0 && text[1] < 0x90) ||
                          (first == 0xf4 && text[1] >= 0x90)));
}
bool jsonArray(lua_State *s, int index) {
  if (!lua_getmetatable(s, index)) return false;
  lua_rawgetp(s, LUA_REGISTRYINDEX, &jsonArrayKey);
  const bool equal = lua_rawequal(s, -1, -2);
  lua_pop(s, 2);
  return equal;
}
struct JsonWriter {
  char *buffer;
  size_t capacity, size = 0;
  unsigned elements = 0;
  const void *ancestors[JsonDepthLimit]{};
  const char *error = "Invalid bounded JSON value";
  bool append(const char *bytes, size_t count) {
    if (count > capacity - size) { error = "JSON output exceeds byte limit"; return false; }
    memcpy(buffer + size, bytes, count);
    size += count; buffer[size] = 0;
    return true;
  }
  bool character(char c) { return append(&c, 1); }
  bool string(const char *text, size_t length, lua_State *s) {
    if (length > JsonStringLimit) { error = "JSON string exceeds byte limit"; return false; }
    if (!character('"')) return false;
    constexpr char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < length;) {
      if ((i & 63u) == 0 && budget(s).expired()) { error = "JSON wall budget exceeded"; return false; }
      const unsigned char c = static_cast<unsigned char>(text[i]);
      if (c >= 0x80) {
        size_t width = 0;
        if (!utf8(reinterpret_cast<const unsigned char *>(text + i), length - i, width)) {
          error = "Invalid JSON UTF-8"; return false;
        }
        if (!append(text + i, width)) return false;
        i += width;
      } else {
        if (c < 32) {
          const char escape[] = {'\\', 'u', '0', '0', digits[c >> 4], digits[c & 15]};
          if (!append(escape, sizeof(escape))) return false;
        } else if (c == '"' || c == '\\') {
          if (!character('\\') || !character(char(c))) return false;
        } else if (!character(char(c))) return false;
        ++i;
      }
    }
    return character('"');
  }
  bool value(lua_State *s, int index, unsigned depth) {
    index = lua_absindex(s, index);
    if (budget(s).expired()) { error = "JSON wall budget exceeded"; return false; }
    switch (lua_type(s, index)) {
    case LUA_TNIL: return append("null", 4);
    case LUA_TBOOLEAN: return lua_toboolean(s, index) ? append("true", 4) : append("false", 5);
    case LUA_TSTRING: {
      size_t length;
      const char *text = lua_tolstring(s, index, &length);
      return string(text, length, s);
    }
    case LUA_TNUMBER: {
      char number[64];
      int count = 0;
      if (lua_isinteger(s, index))
        count = snprintf(number, sizeof(number), "%lld", static_cast<long long>(lua_tointeger(s, index)));
      else {
        const double n = lua_tonumber(s, index);
        if (!std::isfinite(n)) { error = "JSON requires finite numbers"; return false; }
        count = snprintf(number, sizeof(number), "%.17g", n);
      }
      return count > 0 && size_t(count) < sizeof(number) && append(number, size_t(count));
    }
    case LUA_TUSERDATA:
      if (luaL_testudata(s, index, "onchip.bot.json_null")) return append("null", 4);
      break;
    case LUA_TTABLE: {
      if (depth == JsonDepthLimit) { error = "JSON nesting limit exceeded"; return false; }
      const void *identity = lua_topointer(s, index);
      for (unsigned i = 0; i < depth; ++i)
        if (ancestors[i] == identity) { error = "JSON table cycle"; return false; }
      ancestors[depth] = identity;
      const size_t length = lua_rawlen(s, index);
      unsigned count = 0;
      bool numeric = false, named = false;
      lua_pushnil(s);
      while (lua_next(s, index)) {
        if (++elements > JsonElementLimit) {
          lua_pop(s, 2); error = "JSON element limit exceeded"; return false;
        }
        ++count;
        if (lua_type(s, -2) == LUA_TSTRING) named = true;
        else if (lua_isinteger(s, -2) && lua_tointeger(s, -2) >= 1 &&
                 size_t(lua_tointeger(s, -2)) <= length) numeric = true;
        else {
          lua_pop(s, 2); error = "JSON object keys must be strings or array indexes"; return false;
        }
        lua_pop(s, 1);
      }
      const bool array = jsonArray(s, index) || numeric;
      if ((array && (named || count != length)) || (numeric && named)) {
        error = "JSON arrays require consecutive integer indexes"; return false;
      }
      if (!character(array ? '[' : '{')) return false;
      if (array) {
        for (size_t i = 1; i <= length; ++i) {
          if (i > 1 && !character(',')) return false;
          lua_rawgeti(s, index, lua_Integer(i));
          const bool ok = value(s, -1, depth + 1);
          lua_pop(s, 1);
          if (!ok) return false;
        }
      } else {
        unsigned field = 0;
        lua_pushnil(s);
        while (lua_next(s, index)) {
          size_t keyLength;
          const char *key = lua_tolstring(s, -2, &keyLength);
          const bool ok = (field++ == 0 || character(',')) &&
                          string(key, keyLength, s) && character(':') &&
                          value(s, -1, depth + 1);
          lua_pop(s, 1);
          if (!ok) { lua_pop(s, 1); return false; }
        }
      }
      return character(array ? ']' : '}');
    }
    }
    error = "JSON value type is unsupported";
    return false;
  }
};
struct JsonReader {
  lua_State *s;
  const char *data, *cursor, *end;
  unsigned elements = 0;
  size_t stringBytes = 0;
  char stringBuffer[JsonStringLimit + 1]{};
  const char *error = "Invalid bounded JSON text";
  void whitespace() {
    while (cursor < end && (*cursor == ' ' || *cursor == '\t' ||
                             *cursor == '\r' || *cursor == '\n')) ++cursor;
  }
  bool string() {
    if (cursor == end || *cursor++ != '"') return false;
    size_t length = 0;
    while (cursor < end) {
      if ((size_t(cursor - data) & 63u) == 0 && budget(s).expired()) return false;
      const unsigned char c = static_cast<unsigned char>(*cursor++);
      if (c == '"') {
        stringBytes += length;
        if (stringBytes > onchip::BotNetworkResponseLimit) return false;
        lua_pushlstring(s, stringBuffer, length);
        return true;
      }
      if (c < 32) return false;
      if (c == '\\') {
        if (cursor == end) return false;
        const char escaped = *cursor++;
        if (escaped == 'u') {
          const auto hex = [](char digit) -> int {
            return digit >= '0' && digit <= '9' ? digit - '0' :
                   digit >= 'a' && digit <= 'f' ? digit - 'a' + 10 :
                   digit >= 'A' && digit <= 'F' ? digit - 'A' + 10 : -1;
          };
          if (end - cursor < 4) return false;
          unsigned code = 0;
          for (unsigned j = 0; j < 4; ++j) {
            const int digit = hex(*cursor++);
            if (digit < 0) return false;
            code = (code << 4) | unsigned(digit);
          }
          if (code >= 0xd800 && code <= 0xdbff) {
            if (end - cursor < 6 || cursor[0] != '\\' || cursor[1] != 'u') return false;
            cursor += 2;
            unsigned low = 0;
            for (unsigned j = 0; j < 4; ++j) {
              const int digit = hex(*cursor++);
              if (digit < 0) return false;
              low = (low << 4) | unsigned(digit);
            }
            if (low < 0xdc00 || low > 0xdfff) return false;
            code = 0x10000 + ((code - 0xd800) << 10) + low - 0xdc00;
          } else if (code >= 0xdc00 && code <= 0xdfff) return false;
          if (length + 4 > JsonStringLimit) return false;
          if (code < 0x80) stringBuffer[length++] = char(code);
          else {
            if (code < 0x800) stringBuffer[length++] = char(0xc0 | (code >> 6));
            else if (code < 0x10000) stringBuffer[length++] = char(0xe0 | (code >> 12));
            else stringBuffer[length++] = char(0xf0 | (code >> 18));
            if (code >= 0x10000) stringBuffer[length++] = char(0x80 | ((code >> 12) & 63));
            if (code >= 0x800) stringBuffer[length++] = char(0x80 | ((code >> 6) & 63));
            stringBuffer[length++] = char(0x80 | (code & 63));
          }
          continue;
        }
        const char *escapes = "\"\\/bfnrt";
        const char *match = strchr(escapes, escaped);
        if (!match) return false;
        const char decoded[] = {'"', '\\', '/', '\b', '\f', '\n', '\r', '\t'};
        if (length == JsonStringLimit) return false;
        stringBuffer[length++] = decoded[match - escapes];
      } else {
        size_t width = 1;
        if (c >= 0x80 && (!utf8(reinterpret_cast<const unsigned char *>(cursor - 1),
                                size_t(end - (cursor - 1)), width))) return false;
        if (length + width > JsonStringLimit) return false;
        memcpy(stringBuffer + length, cursor - 1, width);
        length += width; cursor += width - 1;
      }
    }
    return false;
  }
  bool value(unsigned depth) {
    whitespace();
    if (cursor == end || budget(s).expired()) return false;
    const char c = *cursor;
    if (c == '{' || c == '[') {
      if (depth == JsonDepthLimit) return false;
      const bool array = c == '[';
      ++cursor;
      lua_newtable(s);
      if (array) {
        lua_rawgetp(s, LUA_REGISTRYINDEX, &jsonArrayKey);
        lua_setmetatable(s, -2);
      }
      const int table = lua_gettop(s);
      whitespace();
      if (cursor < end && *cursor == (array ? ']' : '}')) { ++cursor; return true; }
      unsigned index = 0;
      do {
        if (++elements > JsonElementLimit) return false;
        if (array) {
          if (!value(depth + 1)) return false;
          lua_rawseti(s, table, ++index);
        } else {
          if (!string()) return false;
          lua_pushvalue(s, -1); lua_rawget(s, table);
          const bool duplicate = !lua_isnil(s, -1);
          lua_pop(s, 1);
          if (duplicate) return false;
          whitespace();
          if (cursor == end || *cursor++ != ':' || !value(depth + 1)) return false;
          lua_rawset(s, table);
        }
        whitespace();
        if (cursor == end) return false;
        const char separator = *cursor++;
        if (separator == (array ? ']' : '}')) return true;
        if (separator != ',') return false;
        whitespace();
      } while (cursor < end);
      return false;
    }
    if (c == '"') return string();
    if (c == 't' || c == 'f' || c == 'n') {
      const char *word = c == 't' ? "true" : c == 'f' ? "false" : "null";
      const size_t length = strlen(word);
      if (size_t(end - cursor) < length || memcmp(cursor, word, length)) return false;
      cursor += length;
      if (c == 'n') lua_rawgetp(s, LUA_REGISTRYINDEX, &jsonNullKey);
      else lua_pushboolean(s, c == 't');
      return true;
    }
    const char *start = cursor;
    if (*cursor == '-') ++cursor;
    if (cursor == end) return false;
    if (*cursor == '0') ++cursor;
    else {
      if (*cursor < '1' || *cursor > '9') return false;
      do { ++cursor; } while (cursor < end && *cursor >= '0' && *cursor <= '9');
    }
    bool floating = false;
    if (cursor < end && *cursor == '.') {
      floating = true; ++cursor;
      if (cursor == end || *cursor < '0' || *cursor > '9') return false;
      do { ++cursor; } while (cursor < end && *cursor >= '0' && *cursor <= '9');
    }
    if (cursor < end && (*cursor == 'e' || *cursor == 'E')) {
      floating = true; ++cursor;
      if (cursor < end && (*cursor == '+' || *cursor == '-')) ++cursor;
      if (cursor == end || *cursor < '0' || *cursor > '9') return false;
      do { ++cursor; } while (cursor < end && *cursor >= '0' && *cursor <= '9');
    }
    if (cursor == start || cursor - start > 63) return false;
    char number[64]{};
    memcpy(number, start, size_t(cursor - start));
    if (!floating) {
      errno = 0;
      const long long integer = strtoll(number, nullptr, 10);
      if (errno != ERANGE) { lua_pushinteger(s, lua_Integer(integer)); return true; }
    }
    const double n = strtod(number, nullptr);
    if (!std::isfinite(n)) return false;
    lua_pushnumber(s, n);
    return true;
  }
  bool parse(size_t length) {
    if (!length || length > onchip::BotNetworkResponseLimit || memchr(data, 0, length)) return false;
    const int top = lua_gettop(s);
    if (value(0)) {
      whitespace();
      if (cursor == end) return true;
    }
    lua_settop(s, top);
    return false;
  }
};
bool decodeJson(lua_State *s, const char *text, size_t length) {
  JsonReader reader{s, text, text, text + length};
  return reader.parse(length);
}
int encodeJson(lua_State *s) {
  if (lua_gettop(s) != 1) return luaL_error(s, "json.encode accepts one value");
  char encoded[onchip::BotNetworkPayloadLimit + 1]{};
  JsonWriter writer{encoded, onchip::BotNetworkPayloadLimit};
  if (!writer.value(s, 1, 0)) return luaL_error(s, "%s", writer.error);
  lua_pushlstring(s, encoded, writer.size);
  return 1;
}
int decodeJsonApi(lua_State *s) {
  if (lua_gettop(s) != 1 || lua_type(s, 1) != LUA_TSTRING)
    return luaL_error(s, "json.decode accepts one JSON string");
  size_t size;
  const char *text = lua_tolstring(s, 1, &size);
  if (!decodeJson(s, text, size)) return luaL_error(s, "Invalid bounded JSON text");
  return 1;
}
int jsonArrayApi(lua_State *s) {
  if (lua_gettop(s) == 0) lua_newtable(s);
  else if (lua_gettop(s) != 1 || !lua_istable(s, 1))
    return luaL_error(s, "json.array accepts an optional table");
  lua_rawgetp(s, LUA_REGISTRYINDEX, &jsonArrayKey);
  lua_setmetatable(s, 1);
  return 1;
}
void jsonApi(lua_State *s) {
  lua_newtable(s);
  lua_pushboolean(s, false); lua_setfield(s, -2, "__metatable");
  lua_rawsetp(s, LUA_REGISTRYINDEX, &jsonArrayKey);
  luaL_newmetatable(s, "onchip.bot.json_null");
  lua_pushcfunction(s, readonly); lua_setfield(s, -2, "__newindex");
  lua_pushboolean(s, false); lua_setfield(s, -2, "__metatable");
  lua_pop(s, 1);
  lua_newuserdatauv(s, 1, 0);
  luaL_getmetatable(s, "onchip.bot.json_null"); lua_setmetatable(s, -2);
  lua_pushvalue(s, -1);
  lua_rawsetp(s, LUA_REGISTRYINDEX, &jsonNullKey);
  lua_newtable(s);
  lua_insert(s, -2);
  lua_setfield(s, -2, "null");
  lua_pushcfunction(s, encodeJson); lua_setfield(s, -2, "encode");
  lua_pushcfunction(s, decodeJsonApi); lua_setfield(s, -2, "decode");
  lua_pushcfunction(s, jsonArrayApi); lua_setfield(s, -2, "array");
  freeze(s); lua_setglobal(s, "json");
}
int finishIo(lua_State *s, int, lua_KContext) {
  auto &call = context(s);
  if (call.io->kind == onchip::BotIoRequest::RepeaterNext && call.completion) {
    if (!call.completion->ok) return luaL_error(s, "%s", call.completion->error);
    if (call.completion->found) lua_pushstring(s, call.completion->value);
    else lua_pushnil(s);
    return 1;
  }
  if ((call.io->kind == onchip::BotIoRequest::RepeaterStatus ||
       call.io->kind == onchip::BotIoRequest::RepeaterLogin) && call.completion) {
    const auto &r = *call.completion;
    const auto &p = r.repeater;
    lua_newtable(s);
    boolean(s, "ok", r.ok); text(s, "alias", p.alias);
    text(s, "error", r.error); text(s, "code", onchip::botRepeaterErrorName(p.error));
    boolean(s, "queued", r.queued); boolean(s, "transmitted", r.transmitted);
    boolean(s, "available", p.available); boolean(s, "fresh", p.fresh);
    integer(s, "age_seconds", p.ageSeconds);
    integer(s, "sample_time_seconds", p.sampledUtc);
    integer(s, "next_poll_seconds", p.nextPollSeconds);
    integer(s, "next_discovery_seconds", p.nextDiscoverySeconds);
    text(s, "wait", onchip::botRepeaterWaitName(p.wait));
    integer(s, "permissions", r.repeaterPermissions);
    if (r.ok && call.io->kind == onchip::BotIoRequest::RepeaterStatus) {
      const auto &v = p.stats;
      if (v.batteryMv) number(s, "battery_volts", double(v.batteryMv) / 1000);
      integer(s, "queued_packets", v.queued); integer(s, "uptime_seconds", v.uptimeSeconds);
      integer(s, "rx_packets_total", v.received); integer(s, "tx_packets_total", v.sent);
      integer(s, "tx_airtime_seconds_total", v.txSeconds);
      integer(s, "error_flags", v.errors);
      integer(s, "noise_dbm", v.noise); integer(s, "rssi_dbm", v.rssi);
      number(s, "snr_db", double(v.snrQuarterDb) / 4);
    }
    freeze(s); return 1;
  }
  if ((call.io->kind == onchip::BotIoRequest::Cas || call.io->kind == onchip::BotIoRequest::Transaction) &&
      call.completion) {
    const auto &result = *call.completion;
    lua_newtable(s);
    text(s, "status", result.outcome == onchip::BotIoResult::Committed ? "committed" :
                     result.outcome == onchip::BotIoResult::Conflict ? "conflict" :
                     result.outcome == onchip::BotIoResult::Unknown ? "unknown" : "rejected");
    boolean(s, "ok", result.ok && result.outcome == onchip::BotIoResult::Committed);
    text(s, "error", result.error);
    freeze(s);
    return 1;
  }
  if ((call.io->kind == onchip::BotIoRequest::Rpc ||
       call.io->kind == onchip::BotIoRequest::HttpGet ||
       call.io->kind == onchip::BotIoRequest::HttpPost) && call.completion) {
    const auto &result = *call.completion;
    const bool rpc = call.io->kind == onchip::BotIoRequest::Rpc;
    const bool typed = rpc && !strcmp(call.io->endpoint, "home") &&
        (!strcmp(call.io->key, "health") || !strcmp(call.io->key, "echo") ||
         !strcmp(call.io->key, "weather"));
    const bool hasJson = result.ok && result.json[0] != 0;
    // Even a successful transport completion is not a successful call if its body is malformed.
    const bool parsed = !hasJson || decodeJson(s, result.json, strlen(result.json));
    const bool invalid = !parsed || (result.ok && !hasJson && !typed);
    lua_newtable(s);
    boolean(s, "ok", result.ok && !invalid);
    integer(s, "http_status", result.httpStatus);
    boolean(s, "submitted", result.networkSubmitted);
    if (parsed && hasJson) {
      // The parsed value is below the completion table, including JSON null.
      lua_insert(s, -2);
      lua_setfield(s, -2, rpc ? "result" : "body");
    }
    if (result.ok && typed && !hasJson) {
      lua_newtable(s);
      if (!strcmp(call.io->key, "health")) text(s, "status", result.value);
      else if (!strcmp(call.io->key, "echo")) text(s, "text", result.value);
      else {
        text(s, "location", result.value); text(s, "country", result.country);
        text(s, "source", result.source); text(s, "observed_at", result.observedAt);
        number(s, "temperature_c", result.temperatureC);
        integer(s, "weather_code", result.weatherCode);
        integer(s, "source_age_seconds", result.sourceAgeSeconds);
      }
      freeze(s); lua_setfield(s, -2, "result");
    }
    if (!result.ok || invalid) {
      lua_newtable(s);
      text(s, "code", invalid ? "invalid_response" :
                         result.rpcCode[0] ? result.rpcCode : "native_error");
      text(s, "message", invalid ? "Invalid bounded network response" : result.error);
      freeze(s); lua_setfield(s, -2, "error");
    }
    freeze(s);
    return 1;
  }
  if (!call.completion || !call.completion->ok)
    return luaL_error(s, "%s", call.completion ? call.completion->error : "Missing I/O completion");
  if (call.io->reply) call.replyCompleted = true;
  if (call.io->reminder()) {
    const auto &result = *call.completion;
    if (call.io->kind == onchip::BotIoRequest::ReminderList) lua_pushstring(s, result.value);
    else {
      lua_newtable(s);
      integer(s, "id", result.revision); integer(s, "deadline_utc", result.deadlineUtc);
      text(s, "state", onchip::botReminderStateName(result.reminderState));
      freeze(s);
    }
  } else if (call.io->durableTimer()) {
    const auto &result = *call.completion;
    lua_newtable(s);
    text(s, "state", onchip::botTimerStateName(result.timerState));
    integer(s, "deadline_utc", result.deadlineUtc);
    integer(s, "revision", result.revision);
    boolean(s, "replaced", result.replaced);
    boolean(s, "time_trusted", result.timeTrusted);
    freeze(s);
  } else if (call.io->kind == onchip::BotIoRequest::Get) {
    if (call.completion->found) lua_pushstring(s, call.completion->value);
    else lua_pushnil(s);
    lua_pushboolean(s, call.completion->found && rfText(call.completion->value, call.event->replyLimit));
    return 2;
  } else if (call.io->kind == onchip::BotIoRequest::List) {
    const auto &keys = call.completion->keys;
    lua_newtable(s);
    integer(s, "count", keys.count);
    boolean(s, "truncated", false);
    bool safe = true;
    for (unsigned i = 0; i < keys.count; ++i)
      safe = safe && rfText(keys.keys[i], onchip::BotKeyLimit);
    boolean(s, "rf_safe", safe);
    lua_createtable(s, keys.count, 0);
    for (unsigned i = 0; i < keys.count; ++i) {
      lua_pushstring(s, keys.keys[i]); lua_rawseti(s, -2, i + 1);
    }
    freeze(s); lua_setfield(s, -2, "keys");
    lua_createtable(s, keys.count, 0);
    const size_t prefix = strlen(call.io->key);
    for (unsigned i = 0; i < keys.count; ++i) {
      lua_pushstring(s, keys.keys[i] + prefix); lua_rawseti(s, -2, i + 1);
    }
    freeze(s); lua_setfield(s, -2, "suffixes");
    freeze(s);
  } else if (call.io->kind == onchip::BotIoRequest::Put || call.io->kind == onchip::BotIoRequest::Delete) {
    lua_pushboolean(s, true);
    lua_pushstring(s, call.io->kind == onchip::BotIoRequest::Put ?
                     (call.completion->found ? "replaced" : "created") :
                     (call.completion->found ? "deleted" : "absent"));
    return 2;
  } else if (call.io->kind == onchip::BotIoRequest::Send || call.io->kind == onchip::BotIoRequest::Forward ||
             call.io->kind == onchip::BotIoRequest::Wait ||
             (call.io->kind == onchip::BotIoRequest::Trace && call.io->traceSendOnly)) {
    const auto &result = *call.completion;
    lua_newtable(s);
    text(s, "text", result.value);
    boolean(s, "queued", result.queued);
    boolean(s, "transmitted", result.transmitted);
    boolean(s, "acknowledged", result.acknowledged);
    boolean(s, "truncated", result.truncated);
    boolean(s, "authenticated", result.packet.authenticated);
    integer(s, "job", result.radioJob);
    if (call.io->kind == onchip::BotIoRequest::Wait &&
        (call.io->waitKind == onchip::BotIoRequest::TextWait || call.io->waitKind == onchip::BotIoRequest::ChannelWait)) {
      text(s, "kind", result.packet.channel ? "channel" : "text");
      char path[4 + 2 * onchip::BotPathLimit]{}, key[65];
      onchip::formatBotPath(result.packet.path, path, sizeof(path));
      if (result.packet.authenticated) {
        for (unsigned i = 0; i < 32; ++i) snprintf(key + i * 2, 3, "%02x", result.packet.sender[i]);
        text(s, "from", key);
      }
      text(s, "nickname", result.packet.nickname); text(s, "path", path);
      integer(s, "timestamp", result.packet.timestamp);
      integer(s, "path_width", result.packet.path.width);
      boolean(s, "path_known", result.packet.path.valid());
      integer(s, "route_type", result.packet.routeType);
      boolean(s, "forwardable", result.packet.forwardable);
      boolean(s, "measured", result.packet.signal);
      number(s, "rssi_dbm", result.packet.rssi); number(s, "snr_db", result.packet.snr);
      freezePacket(s, result);
    } else {
      const bool trace = call.io->kind == onchip::BotIoRequest::Trace ||
                         call.io->waitKind == onchip::BotIoRequest::TraceWait;
      text(s, "kind", trace ? "trace" : call.io->kind == onchip::BotIoRequest::Wait ? "ack" : "tx");
      if (trace) {
        boolean(s, "correlated", result.found);
        integer(s, "path_width", result.trace.width); integer(s, "hops", result.trace.count);
        lua_createtable(s, result.trace.count, 0);
        for (unsigned i = 0; i < result.trace.count; ++i) {
          lua_pushnumber(s, double(result.trace.snr[i]) / 4); lua_rawseti(s, -2, i + 1);
        }
        freeze(s); lua_setfield(s, -2, "snr");
      }
      freeze(s);
    }
  } else if (call.io->kind == onchip::BotIoRequest::Trace || call.io->kind == onchip::BotIoRequest::Advert ||
             call.io->kind == onchip::BotIoRequest::Inspect || call.io->kind == onchip::BotIoRequest::Admin) {
    lua_pushstring(s, call.completion->value);
  } else lua_pushboolean(s, true);
  return 1;
}
onchip::BotIoRequest &prepareIo(lua_State *s, onchip::BotIoRequest::Kind kind) {
  auto &call = context(s);
  if (call.initializing || !call.event || !call.io || !lua_isyieldable(s))
    luaL_error(s, "I/O requires a retained invocation");
  if (call.event->kind != onchip::BotEvent::Command && call.subscriptionOwner &&
      call.subscriptionOwner->eventEpoch &&
      call.event->eventEpoch != call.subscriptionOwner->eventEpoch->load())
    luaL_error(s, "Event epoch revoked before I/O");
  if (++call.operations > onchip::BotIoLimit)
    luaL_error(s, "Invocation I/O limit exceeded");
  if (call.event->kind != onchip::BotEvent::Command &&
      kind != onchip::BotIoRequest::Sleep && kind != onchip::BotIoRequest::Get &&
      kind != onchip::BotIoRequest::Put && kind != onchip::BotIoRequest::Delete &&
      kind != onchip::BotIoRequest::List && kind != onchip::BotIoRequest::Cas &&
      kind != onchip::BotIoRequest::Transaction &&
      kind != onchip::BotIoRequest::RepeaterNext &&
      kind != onchip::BotIoRequest::RepeaterStatus &&
      kind != onchip::BotIoRequest::RepeaterLogin &&
      !(kind >= onchip::BotIoRequest::TimerSet && kind <= onchip::BotIoRequest::TimerWait))
    luaL_error(s, "Subscriptions have no radio reply, forwarding, reminder or private RPC authority");
  *call.io = {};
  call.io->kind = kind;
  call.io->token = {call.generation, call.job, call.operations};
  call.io->eventEpoch = call.event->eventEpoch;
  return *call.io;
}
int sleepFor(lua_State *s) {
  const lua_Integer ms = luaL_checkinteger(s, 1);
  if (ms < 1 || ms > 30000) return luaL_error(s, "sleep requires 1..30000 milliseconds");
  auto &request = prepareIo(s, onchip::BotIoRequest::Sleep);
  request.delayMs = uint32_t(ms);
  return lua_yieldk(s, 0, 0, finishIo);
}
int repeaterNext(lua_State *s) {
  if (lua_gettop(s)) return luaL_error(s, "repeater.next accepts no arguments");
  auto &request = prepareIo(s, onchip::BotIoRequest::RepeaterNext);
  request.delayMs = 1000;
  return lua_yieldk(s, 0, 0, finishIo);
}
void boundedString(lua_State *s, int index, char *output, size_t capacity) {
  if (lua_type(s, index) != LUA_TSTRING) luaL_error(s, "Expected bounded text");
  size_t size;
  const char *text = lua_tolstring(s, index, &size);
  if (!size || size >= capacity || memchr(text, 0, size))
    luaL_error(s, "Text length/NUL exceeds native bound");
  memcpy(output, text, size + 1);
}
void moduleName(lua_State *s, char name[onchip::BotNameLimit + 1]) {
  boundedString(s, 1, name, onchip::BotNameLimit + 1);
  if (!onchip::botIdentifier(name) || nativeName(name))
    luaL_error(s, "Module requires a non-native lowercase identifier (24 bytes)");
}
int declareModule(lua_State *s) {
  auto &call = context(s);
  if (!call.initializing || call.moduleInitializing || lua_gettop(s) != 2)
    return luaL_error(s, "Declare modules only at source initialization, outside module loaders");
  char name[onchip::BotNameLimit + 1]; moduleName(s, name);
  if (!lua_isfunction(s, 2) || lua_iscfunction(s, 2))
    return luaL_error(s, "Module loader must be a source-defined Lua function");
  lua_getfield(s, lua_upvalueindex(1), name);
  if (!lua_isnil(s, -1) || call.manifest->moduleCount >= onchip::BotModuleLimit)
    return luaL_error(s, "Module duplicate or eight-module limit exceeded");
  lua_pop(s, 1);
  lua_createtable(s, 3, 0);
  lua_pushvalue(s, 2); lua_rawseti(s, -2, 1);
  lua_pushinteger(s, 0); lua_rawseti(s, -2, 2);
  lua_setfield(s, lua_upvalueindex(1), name);
  strcpy(call.manifest->modules[call.manifest->moduleCount++], name);
  return 0;
}
int requireModule(lua_State *s) {
  if (lua_gettop(s) != 1) return luaL_error(s, "require accepts one declared module name");
  char name[onchip::BotNameLimit + 1]; moduleName(s, name);
  lua_getfield(s, lua_upvalueindex(1), name);
  if (lua_isnil(s, -1)) return luaL_error(s, "Undeclared package module: %s", name);
  const int entry = lua_gettop(s);
  lua_rawgeti(s, entry, 2);
  const int status = int(lua_tointeger(s, -1)); lua_pop(s, 1);
  if (status == 1) return luaL_error(s, "Module dependency cycle: %s", name);
  if (status == 3) return luaL_error(s, "Module initialization previously failed: %s", name);
  if (status == 2) { lua_rawgeti(s, entry, 3); return 1; }
  auto &call = context(s);
  if (!call.initializing) return luaL_error(s, "Module was not initialized during source validation");
  lua_pushinteger(s, 1); lua_rawseti(s, entry, 2);
  lua_rawgeti(s, entry, 1);
  const bool previous = call.moduleInitializing;
  call.moduleInitializing = true;
  const int result = lua_pcall(s, 0, 1, 0);
  call.moduleInitializing = previous;
  if (result != LUA_OK || !lua_istable(s, -1)) {
    lua_pushinteger(s, 3); lua_rawseti(s, entry, 2);
    if (result != LUA_OK) return lua_error(s);
    return luaL_error(s, "Module loader must return a table: %s", name);
  }
  lua_pushvalue(s, -1); lua_rawseti(s, entry, 3);
  lua_pushinteger(s, 2); lua_rawseti(s, entry, 2);
  return 1;
}
void moduleApi(lua_State *s) {
  lua_newtable(s);
  lua_pushvalue(s, -1); context(s).moduleReference = luaL_ref(s, LUA_REGISTRYINDEX);
  lua_pushvalue(s, -1); lua_pushcclosure(s, declareModule, 1); lua_setglobal(s, "module");
  lua_pushcclosure(s, requireModule, 1); lua_setglobal(s, "require");
}
void initializeModules(lua_State *s) {
  auto &call = context(s);
  for (const char *name : {"module", "require"}) {
    lua_getglobal(s, name);
    const bool valid = lua_tocfunction(s, -1) == (name[0] == 'm' ? declareModule : requireModule);
    lua_pop(s, 1);
    if (!valid) luaL_error(s, "Module API names are reserved");
  }
  for (unsigned i = 0; i < call.manifest->moduleCount; ++i) {
    lua_rawgeti(s, LUA_REGISTRYINDEX, call.moduleReference);
    lua_pushcclosure(s, requireModule, 1);
    lua_pushstring(s, call.manifest->modules[i]);
    lua_call(s, 1, 0);
  }
}
int eventIndex(lua_State *s) {
  char name[24]; boundedString(s, 1, name, sizeof(name));
  const char *names[] = {"startup", "connectivity", "message", "node_status", "scheduled"};
  for (unsigned i = 0; i < 5; ++i) if (!strcmp(name, names[i])) return int(i);
  return luaL_error(s, "Event requires startup, connectivity, message, node_status or scheduled");
}
int subscribeEvent(lua_State *s) {
  auto &call = context(s);
  if (!call.initializing || lua_gettop(s) != 2)
    return luaL_error(s, "events.on requires a declared name and function name during source initialization");
  const unsigned index = unsigned(eventIndex(s));
  if (index == 4) return luaL_error(s, "Scheduled events require events.every(seconds, function_name)");
  char name[onchip::BotNameLimit + 1]; boundedString(s, 2, name, sizeof(name));
  if (!onchip::botIdentifier(name) || nativeName(name) || call.manifest->events[index][0])
    return luaL_error(s, "Event export invalid/native or event already subscribed");
  strcpy(call.manifest->events[index], name); call.manifest->eventMask |= uint8_t(1u << index);
  return 0;
}
int recurringEvent(lua_State *s) {
  auto &call = context(s);
  if (!call.initializing || lua_gettop(s) != 2)
    return luaL_error(s, "events.every requires seconds and function name during source initialization");
  const auto seconds = luaL_checkinteger(s, 1);
  char name[onchip::BotNameLimit + 1]; boundedString(s, 2, name, sizeof(name));
  if (seconds < 15 || seconds > 86400 || !onchip::botIdentifier(name) ||
      nativeName(name) || call.manifest->events[4][0])
    return luaL_error(s, "Recurring event requires 15..86400 seconds, a non-native function and one declaration");
  strcpy(call.manifest->events[4], name);
  call.manifest->scheduleSeconds = uint32_t(seconds);
  call.manifest->eventMask |= 16;
  return 0;
}
int unsubscribeEvent(lua_State *s) {
  auto &call = context(s);
  if (call.initializing || !call.subscriptionOwner || lua_gettop(s) != 1)
    return luaL_error(s, "events.off requires one event name during an invocation");
  const auto bit = uint8_t(1u << unsigned(eventIndex(s)));
  auto &owner = *call.subscriptionOwner;
  const bool enabled = (owner.manifest->eventMask & bit) != 0;
  if (enabled) {
    owner.manifest->eventMask &= uint8_t(~bit);
    if (owner.eventEpoch) {
      if (owner.eventEpoch->load() == UINT32_MAX) {
        owner.manifest->eventMask = 0; *owner.eventEpoch = 0;
        return luaL_error(s, "Event epoch exhausted; reboot required");
      }
      ++*owner.eventEpoch;
    }
  }
  lua_pushboolean(s, enabled); return 1;
}
void eventApi(lua_State *s) {
  lua_newtable(s);
  lua_pushcfunction(s, subscribeEvent); lua_setfield(s, -2, "on");
  lua_pushcfunction(s, unsubscribeEvent); lua_setfield(s, -2, "off");
  lua_pushcfunction(s, recurringEvent); lua_setfield(s, -2, "every");
  freeze(s); lua_setglobal(s, "events");
}
void validateEvents(lua_State *s, const Call &call) {
  lua_getglobal(s, "events");
  if (lua_type(s, -1) != LUA_TUSERDATA) luaL_error(s, "Event API name is reserved");
  lua_getfield(s, -1, "on");
  const bool validOn = lua_tocfunction(s, -1) == subscribeEvent;
  lua_pop(s, 1); lua_getfield(s, -1, "off");
  const bool validOff = lua_tocfunction(s, -1) == unsubscribeEvent;
  lua_pop(s, 2);
  if (!validOn || !validOff) luaL_error(s, "Event API name is reserved");
  for (const auto &name : call.manifest->events) if (name[0]) {
    lua_getglobal(s, name);
    const bool valid = lua_isfunction(s, -1) && !lua_iscfunction(s, -1);
    lua_pop(s, 1);
    if (!valid) luaL_error(s, "Event export is not a Lua function");
  }
}
int repeaterRequest(lua_State *s) {
  if (lua_gettop(s) != 1) return luaL_error(s, "Repeater request requires one owner-configured alias");
  char alias[17]; boundedString(s, 1, alias, sizeof(alias));
  const auto kind = lua_toboolean(s, lua_upvalueindex(1)) ?
      onchip::BotIoRequest::RepeaterLogin : onchip::BotIoRequest::RepeaterStatus;
  auto &request = prepareIo(s, kind);
  strcpy(request.key, alias); request.delayMs = 30000;
  return lua_yieldk(s, 0, 0, finishIo);
}
void repeaterApi(lua_State *s) {
  lua_newtable(s);
  lua_pushcfunction(s, repeaterNext); lua_setfield(s, -2, "next");
  lua_pushboolean(s, false); lua_pushcclosure(s, repeaterRequest, 1); lua_setfield(s, -2, "status");
  lua_pushboolean(s, true); lua_pushcclosure(s, repeaterRequest, 1); lua_setfield(s, -2, "login");
  freeze(s); lua_setglobal(s, "repeater");
}
void networkAuthority(lua_State *s, onchip::BotIoRequest &request) {
#if ONCHIP_BOT_COMPACT_PROFILE
  luaL_error(s, "HTTPS is unsupported on Pine; use authenticated mesh and persistent KV");
#endif
  auto &call = context(s);
  if (!call.event->authenticated || !call.event->homeAccess ||
      call.event->channel[0] || call.event->local)
    luaL_error(s, "Network access requires a private authenticated DM and native home grant");
  memcpy(request.principal, call.event->sender, sizeof(request.principal));
  request.grant = call.event->homeGrant;
}
void networkName(lua_State *s, int index, char output[onchip::BotNameLimit + 1]) {
  boundedString(s, index, output, onchip::BotNameLimit + 1);
  if (!onchip::botIdentifier(output) || output[0] == '_')
    luaL_error(s, "Network name requires a lowercase identifier (24 bytes)");
}
void networkJson(lua_State *s, int index, char output[onchip::BotNetworkPayloadLimit + 1]) {
  if (lua_type(s, index) != LUA_TTABLE) luaL_error(s, "Network payload requires a table");
  JsonWriter writer{output, onchip::BotNetworkPayloadLimit};
  if (!writer.value(s, index, 0)) luaL_error(s, "%s", writer.error);
}
int networkRpc(lua_State *s) {
  if (lua_gettop(s) != 3) return luaL_error(s, "rpc.call requires service, operation and args table");
  auto &request = prepareIo(s, onchip::BotIoRequest::Rpc);
  networkAuthority(s, request);
  networkName(s, 1, request.endpoint);
  networkName(s, 2, request.key);
  if (lua_type(s, 3) != LUA_TTABLE || lua_rawlen(s, 3) || jsonArray(s, 3))
    return luaL_error(s, "RPC args require a JSON object table");
  networkJson(s, 3, request.json);
  if (!strcmp(request.endpoint, "home")) {
    const bool health = !strcmp(request.key, "health");
    const char *argument = !strcmp(request.key, "echo") ? "text" :
                           !strcmp(request.key, "weather") ? "place" : nullptr;
    if (health || argument) {
      unsigned fields = 0;
      lua_pushnil(s);
      while (lua_next(s, 3)) {
        size_t length = 0;
        const char *name = lua_type(s, -2) == LUA_TSTRING ?
            lua_tolstring(s, -2, &length) : nullptr;
        if (!argument || !name || length != strlen(argument) ||
            memcmp(name, argument, length) || ++fields != 1)
          return luaL_error(s, "Home RPC argument fields are not allowed");
        boundedString(s, -1, request.value,
                      !strcmp(argument, "place") ? 81 : sizeof(request.value));
        lua_pop(s, 1);
      }
      if (argument && fields != 1) return luaL_error(s, "Home RPC argument is missing");
    }
  }
  return lua_yieldk(s, 0, 0, finishIo);
}
int networkHttp(lua_State *s) {
  const bool post = lua_toboolean(s, lua_upvalueindex(1));
  if (lua_gettop(s) != (post ? 2 : 1))
    return luaL_error(s, post ? "http.post requires name and payload table" :
                               "http.get requires one named endpoint");
  auto &request = prepareIo(s, post ? onchip::BotIoRequest::HttpPost :
                                      onchip::BotIoRequest::HttpGet);
  networkAuthority(s, request);
  networkName(s, 1, request.endpoint);
  if (post) networkJson(s, 2, request.json);
  return lua_yieldk(s, 0, 0, finishIo);
}
int rpcText(lua_State *s);
void networkApi(lua_State *s) {
  lua_newtable(s);
  lua_pushboolean(s, false); lua_pushcclosure(s, networkHttp, 1); lua_setfield(s, -2, "get");
  lua_pushboolean(s, true); lua_pushcclosure(s, networkHttp, 1); lua_setfield(s, -2, "post");
  freeze(s); lua_setglobal(s, "http");
  lua_newtable(s);
  lua_pushcfunction(s, networkRpc); lua_setfield(s, -2, "call");
  lua_pushcfunction(s, rpcText); lua_setfield(s, -2, "text");
  freeze(s); lua_setglobal(s, "rpc");
}
void validateNetworkApi(lua_State *s) {
  const struct { const char *global, *method; lua_CFunction function; } methods[] = {
      {"json", "encode", encodeJson}, {"json", "decode", decodeJsonApi},
      {"json", "array", jsonArrayApi}, {"http", "get", networkHttp},
      {"http", "post", networkHttp}, {"rpc", "call", networkRpc},
      {"rpc", "text", rpcText}};
  for (const auto &method : methods) {
    lua_getglobal(s, method.global);
    if (lua_type(s, -1) != LUA_TUSERDATA)
      luaL_error(s, "Native network/JSON API is reserved");
    lua_getfield(s, -1, method.method);
    const bool valid = lua_tocfunction(s, -1) == method.function;
    lua_pop(s, 2);
    if (!valid) luaL_error(s, "Native network/JSON API is reserved");
  }
}
int rpcText(lua_State *s) {
  auto &call = context(s);
  if (call.initializing || !call.event) return luaL_error(s, "RPC text formatting requires an invocation");
  if (lua_gettop(s) != 2) return luaL_error(s, "rpc.text requires text and byte limit");
  char value[onchip::BotValueLimit + 1];
  boundedString(s, 1, value, sizeof(value));
  const lua_Integer limit = luaL_checkinteger(s, 2);
  if (limit < 16 || limit > call.event->replyLimit) return luaL_error(s, "RPC text limit must be 16..reply_bytes");
  size_t needed = 0;
  for (const auto *p = reinterpret_cast<const unsigned char *>(value); *p; ++p)
    needed += *p == '\\' ? 2 : *p < 32 || *p > 126 ? 4 : 1;
  constexpr char suffix[] = "...[truncated]";
  const bool truncated = needed > size_t(limit);
  const size_t available = size_t(limit) - (truncated ? sizeof(suffix) - 1 : 0);
  char out[onchip::BotReplyLimit + 1]{};
  size_t used = 0;
  for (const auto *p = reinterpret_cast<const unsigned char *>(value); *p; ++p) {
    const size_t width = *p == '\\' ? 2 : *p < 32 || *p > 126 ? 4 : 1;
    if (used + width > available) break;
    if (width == 4) snprintf(out + used, sizeof(out) - used, "\\x%02X", unsigned(*p));
    else {
      out[used] = char(*p);
      if (width == 2) out[used + 1] = '\\';
    }
    used += width;
  }
  if (truncated) strcpy(out + used, suffix);
  else out[used] = 0;
  lua_pushstring(s, out);
  return 1;
}
void tableFields(lua_State *s, int index, std::initializer_list<const char *> allowed);
void stateScope(lua_State *s, onchip::BotIoRequest &request, int scopeIndex) {
  auto &call = context(s);
  const bool thread = lua_istable(s, scopeIndex);
  char label[onchip::BotNameLimit + 1]{};
  if (thread) {
    tableFields(s, scopeIndex, {"scope", "thread"});
    lua_getfield(s, scopeIndex, "thread");
    boundedString(s, -1, label, sizeof(label)); lua_pop(s, 1);
    if (!onchip::botThreadName(label, strlen(label))) luaL_error(s, "Invalid thread name");
    lua_getfield(s, scopeIndex, "scope");
    scopeIndex = lua_gettop(s);
  }
  size_t size = 0;
  const char *scope = lua_isnoneornil(s, scopeIndex) ? "caller" : luaL_checklstring(s, scopeIndex, &size);
  if (size && size != strlen(scope)) luaL_error(s, "Invalid storage scope");
  if (!strcmp(scope, "caller")) request.scope = onchip::BotIoRequest::Caller;
  else if (!strcmp(scope, "conversation")) request.scope = onchip::BotIoRequest::Conversation;
  else if (!strcmp(scope, "bot")) request.scope = onchip::BotIoRequest::Bot;
  else if (!strcmp(scope, "channel")) request.scope = onchip::BotIoRequest::Channel;
  else luaL_error(s, "Storage scope unavailable");
  if (thread) {
    lua_pop(s, 1);
    request.scope = onchip::BotIoRequest::Scope(unsigned(request.scope) + 4);
    const auto encode = [&](char *key) {
      const size_t prefix = strlen(label) + 1, size = strlen(key);
      if (prefix + size > onchip::BotKeyLimit)
        luaL_error(s, "Thread/name and key together exceed 32 bytes");
      memmove(key + prefix, key, size + 1);
      memcpy(key, label, prefix - 1); key[prefix - 1] = '/';
    };
    encode(request.key);
    for (unsigned i = 0; i < request.mutations; ++i) encode(request.mutation[i].key);
  }
  char error[128]{};
  if (!onchip::botStorageScope(*call.event, request, error, sizeof(error)))
    luaL_error(s, "%s", error);
  request.grant = call.event->sharedGrant;
}
int stateIo(lua_State *s, onchip::BotIoRequest::Kind kind) {
  auto &request = prepareIo(s, kind);
  boundedString(s, 1, request.key, sizeof(request.key));
  if (kind == onchip::BotIoRequest::Put) boundedString(s, 2, request.value, sizeof(request.value));
  stateScope(s, request, kind == onchip::BotIoRequest::Put ? 3 : 2);
  return lua_yieldk(s, 0, 0, finishIo);
}
int stateGet(lua_State *s) { return stateIo(s, onchip::BotIoRequest::Get); }
int statePut(lua_State *s) { return stateIo(s, onchip::BotIoRequest::Put); }
int stateDelete(lua_State *s) { return stateIo(s, onchip::BotIoRequest::Delete); }
void mutationValue(lua_State *s, int index, char *value, bool &absent) {
  absent = lua_type(s, index) == LUA_TBOOLEAN && !lua_toboolean(s, index);
  if (absent) return;
  if (lua_type(s, index) != LUA_TSTRING) luaL_error(s, "KV value requires text or false for absence");
  size_t size;
  const char *text = lua_tolstring(s, index, &size);
  if (size > onchip::BotValueLimit || memchr(text, 0, size))
    luaL_error(s, "KV value exceeds 256 bytes or contains NUL");
  memcpy(value, text, size + 1);
}
int stateCas(lua_State *s) {
  if (lua_gettop(s) < 3 || lua_gettop(s) > 4)
    return luaL_error(s, "kv.cas requires key, expected, value and optional scope");
  auto &request = prepareIo(s, onchip::BotIoRequest::Cas);
  request.mutations = 1;
  auto &op = request.mutation[0];
  boundedString(s, 1, op.key, sizeof(op.key));
  bool absent = false;
  mutationValue(s, 2, op.expected, absent); op.compare = true; op.present = !absent;
  mutationValue(s, 3, op.value, op.remove);
  stateScope(s, request, 4);
  return lua_yieldk(s, 0, 0, finishIo);
}
void tableFields(lua_State *s, int index, std::initializer_list<const char *> allowed);
int stateTransaction(lua_State *s) {
  if (lua_gettop(s) > 2) return luaL_error(s, "kv.transaction accepts operations and optional scope");
  auto &request = prepareIo(s, onchip::BotIoRequest::Transaction);
  luaL_checktype(s, 1, LUA_TTABLE);
  const size_t count = lua_rawlen(s, 1);
  if (!count || count > onchip::BotTransactionLimit)
    return luaL_error(s, "KV transaction requires 1..%d operations", int(onchip::BotTransactionLimit));
  lua_pushnil(s);
  while (lua_next(s, 1)) {
    if (!lua_isinteger(s, -2) || lua_tointeger(s, -2) < 1 || lua_tointeger(s, -2) > lua_Integer(count))
      return luaL_error(s, "KV transaction requires a dense operation array");
    lua_pop(s, 1);
  }
  request.mutations = uint8_t(count);
  for (unsigned i = 0; i < count; ++i) {
    lua_rawgeti(s, 1, i + 1);
    const int index = lua_gettop(s);
    tableFields(s, index, {"key", "expect", "value"});
    auto &op = request.mutation[i];
    lua_getfield(s, index, "key"); boundedString(s, -1, op.key, sizeof(op.key)); lua_pop(s, 1);
    lua_getfield(s, index, "expect");
    if (!lua_isnil(s, -1)) {
      bool absent = false; mutationValue(s, -1, op.expected, absent);
      op.compare = true; op.present = !absent;
    }
    lua_pop(s, 1);
    lua_getfield(s, index, "value"); mutationValue(s, -1, op.value, op.remove); lua_pop(s, 1);
    for (unsigned j = 0; j < i; ++j)
      if (!strcmp(op.key, request.mutation[j].key)) return luaL_error(s, "KV transaction duplicate key");
    lua_pop(s, 1);
  }
  stateScope(s, request, 2);
  return lua_yieldk(s, 0, 0, finishIo);
}
int stateList(lua_State *s) {
  if (lua_gettop(s) > 2) return luaL_error(s, "kv.list accepts only optional prefix and scope");
  auto &request = prepareIo(s, onchip::BotIoRequest::List);
  if (!lua_isnoneornil(s, 1)) {
    if (lua_type(s, 1) != LUA_TSTRING) return luaL_error(s, "Expected bounded text prefix");
    size_t length;
    const auto *prefix = lua_tolstring(s, 1, &length);
    if (length > onchip::BotKeyLimit || memchr(prefix, 0, length))
      return luaL_error(s, "KV prefix exceeds 32 bytes or contains NUL");
    memcpy(request.key, prefix, length);
  }
  stateScope(s, request, 2);
  return lua_yieldk(s, 0, 0, finishIo);
}
int durableTimer(lua_State *s, onchip::BotIoRequest::Kind kind) {
  auto &request = prepareIo(s, kind);
  boundedString(s, 1, request.key, sizeof(request.key));
  const bool setting = kind == onchip::BotIoRequest::TimerSet;
  if (setting) {
    const auto seconds = luaL_checkinteger(s, 2);
    if (seconds < 1 || seconds > onchip::BotTimerMaximumSeconds)
      return luaL_error(s, "Durable timer requires 1..86400 seconds");
    request.delaySeconds = uint32_t(seconds);
  }
  stateScope(s, request, setting ? 3 : 2);
  return lua_yieldk(s, 0, 0, finishIo);
}
int timerSet(lua_State *s) { return durableTimer(s, onchip::BotIoRequest::TimerSet); }
int timerGet(lua_State *s) { return durableTimer(s, onchip::BotIoRequest::TimerGet); }
int timerCancel(lua_State *s) { return durableTimer(s, onchip::BotIoRequest::TimerCancel); }
int timerWait(lua_State *s) { return durableTimer(s, onchip::BotIoRequest::TimerWait); }
uint32_t reminderNumber(lua_State *s, int index, bool duration) {
  if (lua_isinteger(s, index)) {
    const auto value = lua_tointeger(s, index);
    if (value < 1 || value > (duration ? onchip::BotTimerMaximumSeconds : UINT32_MAX))
      luaL_error(s, "Reminder number outside bounds");
    return uint32_t(value);
  }
  char text[11];
  boundedString(s, index, text, sizeof(text));
  uint64_t value = 0;
  size_t i = 0;
  while (text[i] >= '0' && text[i] <= '9') value = value * 10 + unsigned(text[i++] - '0');
  if (!i || !value) luaL_error(s, "Reminder requires a positive integer/duration");
  if (text[i]) {
    if (!duration || text[i + 1]) luaL_error(s, "Use duration seconds or a single s/m/h/d suffix");
    const unsigned multiplier = text[i] == 's' ? 1 : text[i] == 'm' ? 60 :
                                text[i] == 'h' ? 3600 : text[i] == 'd' ? 86400 : 0;
    if (!multiplier) luaL_error(s, "Use duration seconds or a single s/m/h/d suffix");
    value *= multiplier;
  }
  if (value > (duration ? onchip::BotTimerMaximumSeconds : UINT32_MAX))
    luaL_error(s, "Reminder number outside bounds");
  return uint32_t(value);
}
int reminderIo(lua_State *s, onchip::BotIoRequest::Kind kind) {
  auto &request = prepareIo(s, kind);
  auto &call = context(s);
  if (!call.event->authenticated || call.event->channel[0] || call.event->local)
    return luaL_error(s, "Personal reminders require an authenticated private DM; channels/local input unsupported");
  memcpy(request.principal, call.event->sender, sizeof(request.principal));
  if (kind == onchip::BotIoRequest::ReminderSet) {
    if (!call.event->reminderAccess) return luaL_error(s, "Personal reminders require the owner reminder grant");
    request.delaySeconds = reminderNumber(s, 1, true);
    boundedString(s, 2, request.value, onchip::BotReminderTextLimit + 1);
    request.grant = call.event->reminderGrant;
  } else if (kind == onchip::BotIoRequest::ReminderCancel) request.revision = reminderNumber(s, 1, false);
  return lua_yieldk(s, 0, 0, finishIo);
}
int reminderAfter(lua_State *s) { return reminderIo(s, onchip::BotIoRequest::ReminderSet); }
int reminderList(lua_State *s) { return reminderIo(s, onchip::BotIoRequest::ReminderList); }
int reminderCancel(lua_State *s) { return reminderIo(s, onchip::BotIoRequest::ReminderCancel); }
struct Draft {
  uint32_t generation, job;
  onchip::BotIoRequest::PacketKind kind;
  uint8_t route[onchip::BotTraceLimit], routeSize, routeWidth;
  uint8_t destination[32];
  bool used;
  char text[onchip::BotReplyLimit + 1];
};
void meshTarget(lua_State *s, int index, uint8_t key[32]) {
  auto &call = context(s);
  if (!call.event || !call.event->authenticated || call.event->channel[0] || call.event->local)
    luaL_error(s, "Native DM destinations require an authenticated private caller");
  char text[65]; boundedString(s, index, text, sizeof(text));
  if (strlen(text) != 64) luaL_error(s, "Destination requires a full 64-hex public key");
  for (unsigned i = 0; i < 32; ++i) {
    unsigned value = 0;
    for (unsigned j = 0; j < 2; ++j) {
      const char c = text[2 * i + j];
      const int digit = c >= '0' && c <= '9' ? c - '0' :
                        c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                        c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
      if (digit < 0) luaL_error(s, "Destination requires a full 64-hex public key");
      value = value * 16 + unsigned(digit);
    }
    key[i] = value;
  }
  if (!memcmp(key, call.event->sender, 32)) return;
  bool nonzero = false;
  for (unsigned i = 0; i < 32; ++i) nonzero = nonzero || key[i];
  if (nonzero) for (const auto &destination : call.event->destinations)
    if (!memcmp(key, destination, 32)) return;
  luaL_error(s, "Native DM destination not granted by owner");
}
void tableFields(lua_State *s, int index, std::initializer_list<const char *> allowed) {
  luaL_checktype(s, index, LUA_TTABLE);
  lua_pushnil(s);
  while (lua_next(s, index)) {
    size_t length;
    const char *key = lua_type(s, -2) == LUA_TSTRING ? lua_tolstring(s, -2, &length) : nullptr;
    bool found = false;
    if (key && length == strlen(key))
      for (auto field : allowed) if (!strcmp(field, key)) found = true;
    if (!found) luaL_error(s, "Unsupported native packet/filter field");
    lua_pop(s, 1);
  }
}
void copyTraceRoute(lua_State *s, int index, onchip::BotIoRequest &request) {
  auto &call = context(s);
  if (call.initializing || !call.event) luaL_error(s, "TRACE requires an invocation");
  onchip::BotEvent event = *call.event;
  if (!lua_isnoneornil(s, index)) {
    char route[155], command[162], error[128];
    boundedString(s, index, route, sizeof(route));
    snprintf(command, sizeof(command), "!trace %s", route);
    if (!onchip::parseBotCommand(command, strlen(command), event, error, sizeof(error)))
      luaL_error(s, "%s", error);
  }
  if (!event.routeSize || event.routeSize > onchip::BotTraceLimit ||
      (event.routeWidth != 1 && event.routeWidth != 2 && event.routeWidth != 4 && event.routeWidth != 8) ||
      event.routeSize % event.routeWidth || event.routeSize / event.routeWidth > onchip::BotTraceHopLimit)
    luaL_error(s, "TRACE requires a valid explicit/inferred direct route");
  request.routeSize = event.routeSize; request.routeWidth = event.routeWidth;
  memcpy(request.route, event.route, event.routeSize);
  request.delayMs = 5000;
}
int compose(lua_State *s) {
  auto &call = context(s);
  if (call.initializing || !call.event || call.event->local ||
      (!call.event->authenticated && !call.event->channelVerified))
    return luaL_error(s, "compose requires an authenticated invocation or verified native channel");
  luaL_checktype(s, 1, LUA_TTABLE);
  onchip::BotIoRequest::PacketKind packetKind = onchip::BotIoRequest::TextPacket;
  lua_getfield(s, 1, "kind");
  if (!lua_isnil(s, -1)) {
    size_t size;
    const char *kind = luaL_checklstring(s, -1, &size);
    if (size != strlen(kind)) return luaL_error(s, "Invalid packet kind");
    if (!strcmp(kind, "channel")) packetKind = onchip::BotIoRequest::ChannelPacket;
    else if (!strcmp(kind, "trace")) packetKind = onchip::BotIoRequest::TracePacket;
    else if (strcmp(kind, "dm")) return luaL_error(s, "compose supports dm, channel or trace only");
  }
  lua_pop(s, 1);
  if (packetKind != onchip::BotIoRequest::TracePacket &&
      (packetKind == onchip::BotIoRequest::ChannelPacket ?
       (!call.event->channelVerified || !call.event->channel[0]) :
       (!call.event->authenticated || call.event->channel[0])))
    return luaL_error(s, "Packet kind requires its native authenticated invocation authority");
  if (packetKind == onchip::BotIoRequest::ChannelPacket && !call.event->targeted)
    return luaL_error(s, "Channel sends require explicit !@BOTKEY8 targeting");
  if (packetKind == onchip::BotIoRequest::TracePacket) tableFields(s, 1, {"kind", "route"});
  else tableFields(s, 1, {"kind", "to", "text"});
  lua_getfield(s, 1, "to");
  uint8_t destination[32]{};
  memcpy(destination, call.event->sender, 32);
  if (!lua_isnil(s, -1)) {
    if (packetKind == onchip::BotIoRequest::TextPacket) meshTarget(s, -1, destination);
    else {
    char expected[65], target[65];
    const auto *key = packetKind == onchip::BotIoRequest::ChannelPacket ? call.event->channelId : call.event->sender;
    for (unsigned i = 0; i < 32; ++i) snprintf(expected + i * 2, 3, "%02x", key[i]);
    boundedString(s, -1, target, sizeof(target));
    if (strcmp(expected, target)) return luaL_error(s, "Only the current caller/selected channel destination is granted");
    }
  }
  lua_pop(s, 1);
  auto *draft = static_cast<Draft *>(lua_newuserdatauv(s, sizeof(Draft), 0));
  *draft = {};
  draft->generation = call.generation; draft->job = call.job;
  draft->kind = packetKind;
  memcpy(draft->destination, destination, 32);
  if (packetKind == onchip::BotIoRequest::TracePacket) {
    lua_getfield(s, 1, "route");
    onchip::BotIoRequest route;
    copyTraceRoute(s, -1, route);
    draft->routeSize = route.routeSize; draft->routeWidth = route.routeWidth;
    memcpy(draft->route, route.route, route.routeSize);
  } else {
    lua_getfield(s, 1, "text");
    boundedString(s, -1, draft->text, call.event->replyLimit + 1);
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(draft->text); *p; ++p)
      if (*p < 32 || *p > 126) return luaL_error(s, "Native text requires printable ASCII");
  }
  lua_pop(s, 1);
  if (luaL_newmetatable(s, "onchip.bot.draft")) {
    lua_pushcfunction(s, readonly); lua_setfield(s, -2, "__newindex");
    lua_pushboolean(s, false); lua_setfield(s, -2, "__metatable");
  }
  lua_setmetatable(s, -2);
  return 1;
}
int sendDraft(lua_State *s) {
  auto *draft = static_cast<Draft *>(luaL_checkudata(s, 1, "onchip.bot.draft"));
  auto &call = context(s);
  if (draft->generation != call.generation || draft->job != call.job)
    return luaL_error(s, "Draft belongs to a different invocation/generation");
  if (draft->used) return luaL_error(s, "Draft already submitted; no automatic retry");
  auto &request = prepareIo(s, draft->kind == onchip::BotIoRequest::TracePacket ?
                              onchip::BotIoRequest::Trace : onchip::BotIoRequest::Send);
  draft->used = true;
  request.packetKind = draft->kind;
  request.traceSendOnly = draft->kind == onchip::BotIoRequest::TracePacket;
  request.routeWidth = draft->routeWidth; request.routeSize = draft->routeSize;
  memcpy(request.route, draft->route, draft->routeSize);
  strcpy(request.value, draft->text);
  memcpy(request.principal, draft->destination, 32);
  request.grant = call.event->meshGrant;
  request.delayMs = 5000;
  return lua_yieldk(s, 0, 0, finishIo);
}
int forwardPacket(lua_State *s) {
  const auto *packet = static_cast<const PacketHandle *>(luaL_checkudata(s, 1, "onchip.bot.packet"));
  auto &call = context(s);
  if (lua_gettop(s) != 1) return luaL_error(s, "forward destination is operator-configured, not a Lua argument");
  if (packet->generation != call.generation || packet->job != call.job || !packet->packet ||
      !call.event || !call.event->authenticated || !call.event->forwardAccess)
    return luaL_error(s, "forward requires an owned received DM and operator pair grant");
  auto &request = prepareIo(s, onchip::BotIoRequest::Forward);
  request.packetId = packet->packet; request.grant = call.event->forwardGrant; request.delayMs = 5000;
  return lua_yieldk(s, 0, 0, finishIo);
}
int waitPacket(lua_State *s) {
  tableFields(s, 1, {"kind", "timeout_ms", "prefix", "exact", "from", "path_width", "route"});
  auto &request = prepareIo(s, onchip::BotIoRequest::Wait);
  lua_getfield(s, 1, "timeout_ms");
  const lua_Integer timeout = lua_isnil(s, -1) ? 5000 : luaL_checkinteger(s, -1);
  if (timeout < 1 || timeout > 30000) return luaL_error(s, "wait timeout requires 1..30000 milliseconds");
  request.delayMs = uint32_t(timeout);
  lua_pop(s, 1);
  lua_getfield(s, 1, "kind");
  size_t size;
  const char *kind = luaL_checklstring(s, -1, &size);
  if (size != strlen(kind) || (strcmp(kind, "text") && strcmp(kind, "ack") && strcmp(kind, "trace") && strcmp(kind, "channel")))
    return luaL_error(s, "wait kind must be text, channel, ack or trace");
  request.waitKind = !strcmp(kind, "ack") ? onchip::BotIoRequest::AckWait :
                     !strcmp(kind, "trace") ? onchip::BotIoRequest::TraceWait :
                     !strcmp(kind, "channel") ? onchip::BotIoRequest::ChannelWait : onchip::BotIoRequest::TextWait;
  request.waitAck = request.waitKind == onchip::BotIoRequest::AckWait; lua_pop(s, 1);
  const auto &event = *context(s).event;
  if (request.waitKind == onchip::BotIoRequest::ChannelWait) {
    if (!event.channelVerified || !event.channel[0] || event.local || !event.channelWait)
      return luaL_error(s, "Channel follow-up waits require owner grant and selected-channel invocation");
    request.grant = event.meshGrant;
  } else if (request.waitKind != onchip::BotIoRequest::TextWait) tableFields(s, 1, {"kind", "timeout_ms"});
  else memcpy(request.principal, event.sender, 32);
  lua_getfield(s, 1, "prefix");
  if (!lua_isnil(s, -1)) boundedString(s, -1, request.key, sizeof(request.key));
  lua_pop(s, 1);
  lua_getfield(s, 1, "exact");
  if (!lua_isnil(s, -1)) {
    if (request.key[0]) return luaL_error(s, "Choose prefix or exact, not both");
    boundedString(s, -1, request.key, sizeof(request.key)); request.exact = true;
  }
  lua_pop(s, 1);
  lua_getfield(s, 1, "from");
  if (!lua_isnil(s, -1)) {
    if (request.waitKind != onchip::BotIoRequest::TextWait)
      return luaL_error(s, "Channel nickname is not authenticated sender authority");
    meshTarget(s, -1, request.principal); request.grant = event.meshGrant;
  }
  lua_pop(s, 1);
  lua_getfield(s, 1, "path_width");
  if (!lua_isnil(s, -1)) {
    const auto width = luaL_checkinteger(s, -1);
    if (width < 1 || width > 3) return luaL_error(s, "Text path_width requires 1..3");
    request.pathWidth = uint8_t(width);
  }
  lua_pop(s, 1);
  lua_getfield(s, 1, "route");
  if (!lua_isnil(s, -1)) {
    char route[8]; boundedString(s, -1, route, sizeof(route));
    if (!strcmp(route, "flood")) request.routeType = 1;
    else if (!strcmp(route, "direct")) request.routeType = 2;
    else return luaL_error(s, "Text route filter requires flood or direct");
  }
  lua_pop(s, 1);
  return lua_yieldk(s, 0, 0, finishIo);
}
int traceRoute(lua_State *s) {
  auto &request = prepareIo(s, onchip::BotIoRequest::Trace);
  copyTraceRoute(s, 1, request);
  return lua_yieldk(s, 0, 0, finishIo);
}
int finishMultiTrace(lua_State *s, int, lua_KContext) {
  auto &call = context(s);
  if (!call.completion || !call.completion->ok)
    return luaL_error(s, "multitrace failed on trace %d: %s", int(call.tracesCompleted + 1),
                      call.completion ? call.completion->error : "Missing completion");
  ++call.tracesCompleted;
  char part[onchip::BotIoTextLimit + 16];
  snprintf(part, sizeof(part), "%s%u: %s", call.tracesCompleted > 1 ? " | " : "",
           unsigned(call.tracesCompleted), call.completion->value);
  if (!call.traceTruncated) {
    const size_t used = strlen(call.traceSummary), added = strlen(part);
    if (used + added + strlen("; truncated") > call.event->replyLimit) {
      strcat(call.traceSummary, "; truncated"); call.traceTruncated = true;
    } else strcat(call.traceSummary, part);
  }
  if (--call.tracesRemaining) {
    const auto previous = *call.io;
    auto &request = prepareIo(s, onchip::BotIoRequest::Trace);
    request.delayMs = 5000; request.routeSize = previous.routeSize; request.routeWidth = previous.routeWidth;
    memcpy(request.route, previous.route, previous.routeSize);
    return lua_yieldk(s, 0, 0, finishMultiTrace);
  }
  lua_pushstring(s, call.traceSummary);
  return 1;
}
int multiTrace(lua_State *s) {
  const lua_Integer count = lua_isnoneornil(s, 2) ? 2 : luaL_checkinteger(s, 2);
  if (count < 1 || count > 3) return luaL_error(s, "multitrace count requires 1..3");
  auto &request = prepareIo(s, onchip::BotIoRequest::Trace);
  copyTraceRoute(s, 1, request);
  auto &call = context(s);
  call.tracesRemaining = uint8_t(count); call.tracesCompleted = 0;
  call.traceSummary[0] = 0; call.traceTruncated = false;
  return lua_yieldk(s, 0, 0, finishMultiTrace);
}
int sendAdvert(lua_State *s) {
  auto &request = prepareIo(s, onchip::BotIoRequest::Advert);
  request.delayMs = 5000;
  return lua_yieldk(s, 0, 0, finishIo);
}
int registerCommand(lua_State *s) {
  auto &call = context(s);
  if (!call.initializing) return luaL_error(s, "commands may only be registered during initialization");
  for (int i = 1; i <= lua_gettop(s); ++i) {
    if (i >= 4 && i <= 6 && lua_isnil(s, i)) continue;
    if (i > 6 || lua_type(s, i) != LUA_TSTRING)
      return luaL_error(s, "command requires name/schema/help and optional export/permission/example strings");
    size_t size;
    const char *value = lua_tolstring(s, i, &size);
    if (size != strlen(value)) return luaL_error(s, "manifest strings must not contain NUL");
  }
  const char *name = luaL_checkstring(s, 1), *schema = luaL_checkstring(s, 2);
  const char *help = luaL_checkstring(s, 3);
  const char *function = lua_isnoneornil(s, 4) ? name : luaL_checkstring(s, 4);
  if (!onchip::botCommandName(name) || !onchip::botIdentifier(function))
    return luaL_error(s, "command names may contain dashes; exports require lowercase Lua identifiers");
  if (!call.bundled && onchip::botReservedCommand(name))
    return luaL_error(s, "Builtin command is reserved; use override_command(name, export)");
  const bool verifyBuiltin = call.buildingOriginals
#if ONCHIP_BOT_COMPACT_PROFILE
      || (call.retained && call.bundled)
#endif
      ;
  // Declarations use source order; the immutable catalogue is sorted by name.
  const auto *expected = verifyBuiltin ? onchip::BotBuiltinManifest.find(name) : nullptr;
  if (verifyBuiltin && !expected)
    return luaL_error(s, "Bundled command %s changed; regenerate BotBuiltinManifest.h", name);
  const unsigned index = verifyBuiltin ? unsigned(expected - onchip::BotBuiltinManifest.commands) : 0;
  if (verifyBuiltin) {
    if (call.builtinDeclarations & (uint32_t(1) << index))
      return luaL_error(s, "command registry full or duplicate name");
  } else
  if (call.manifest->count == (call.bundled ? onchip::BotBuiltinCommandLimit : onchip::BotCommandLimit) ||
      call.manifest->find(name))
    return luaL_error(s, "command registry full or duplicate name");
  char error[128];
  if (!onchip::validateBotSchema(schema, error, sizeof(error)))
    return luaL_error(s, "%s", error);
  if (strlen(help) > 64) return luaL_error(s, "command help exceeds 64 bytes");
  for (const unsigned char *p = reinterpret_cast<const unsigned char *>(help); *p; ++p)
    if (*p < 32 || *p > 126) return luaL_error(s, "command help must be printable ASCII");
  onchip::BotCommand declaration;
  auto &entry = verifyBuiltin ? declaration : call.manifest->writable()[call.manifest->count++];
  strcpy(entry.name, name); strcpy(entry.function, function);
  strcpy(entry.schema, schema); strcpy(entry.help, help);
  if (!lua_isnoneornil(s, 5)) {
    const char *permission = lua_tostring(s, 5);
    const char *names[] = {"public", "dm", "owner", "channel", "shared", "reminder", "home"};
    bool found = false;
    for (unsigned i = 0; i < sizeof(names) / sizeof(*names); ++i) if (!strcmp(permission, names[i])) {
      entry.permission = onchip::BotCommand::Permission(i); found = true;
    }
    if (!found) return luaL_error(s, "Unknown command permission");
  }
  if (!lua_isnoneornil(s, 6)) {
    const char *example = lua_tostring(s, 6);
    if (strlen(example) > 64) return luaL_error(s, "command example exceeds 64 bytes");
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(example); *p; ++p)
      if (*p < 32 || *p > 126) return luaL_error(s, "command example must be printable ASCII");
    strcpy(entry.example, example);
  } else if (call.bundled) {
    const struct { const char *name, *example; } examples[] = {
      {"help", "!help 2"}, {"plugins", "!plugins 1"}, {"neighbors", "!neighbors 1"},
      {"admin", "!admin bot status"}, {"remember", "!remember camp bring tea"},
      {"recall", "!recall camp"}, {"forget", "!forget camp"}, {"cancel", "!cancel 1"},
      {"remind", "!remind 5m check kettle"}, {"calc", "!calc (2+3)*4"}, {"convert", "!convert 1 km mi"},
      {"roll", "!roll 2d6+1"}, {"choose", "!choose tea|coffee"}, {"weather", "!weather Ottawa"},
      {"service", "!service health"}, {"trace", "!trace 1:a1b2"},
      {"board", "!@BOTKEY8 board put camp tea"}
    };
    if (!entry.schema[0]) snprintf(entry.example, sizeof(entry.example), "!%s", name);
    for (const auto &example : examples)
      if (!strcmp(name, example.name)) strcpy(entry.example, example.example);
  }
  if (verifyBuiltin) {
    if (memcmp(&declaration, expected, sizeof(declaration)))
      return luaL_error(s, "Bundled command %s changed; regenerate BotBuiltinManifest.h", name);
    call.builtinDeclarations |= uint32_t(1) << index;
  }
  return 0;
}
int registerOverride(lua_State *s) {
  auto &call = context(s);
  if (!call.initializing || call.bundled || call.moduleInitializing || lua_gettop(s) != 2)
    return luaL_error(s, "override_command requires name/export at source initialization");
  char name[onchip::BotNameLimit + 1], function[onchip::BotNameLimit + 1];
  boundedString(s, 1, name, sizeof(name));
  boundedString(s, 2, function, sizeof(function));
  const auto *original = onchip::BotBuiltinManifest.find(name);
  if (!original) return luaL_error(s, "Unknown builtin command !%s", name);
  if (!onchip::botIdentifier(function) || nativeName(function))
    return luaL_error(s, "Override export requires a non-native lowercase Lua identifier");
  if (call.manifest->count == onchip::BotCommandLimit || call.manifest->find(name))
    return luaL_error(s, "command registry full or duplicate name");
  auto &entry = call.manifest->writable()[call.manifest->count++];
  entry = *original;
  strcpy(entry.function, function);
  return 0;
}
void pushArguments(lua_State *s, const onchip::BotArguments &arguments) {
  for (unsigned i = 0; i < arguments.count; ++i) {
    const auto &value = arguments.values[i];
    switch (value.type) {
    case onchip::BotValue::String: lua_pushstring(s, value.text); break;
    case onchip::BotValue::Integer: lua_pushinteger(s, value.integer); break;
    case onchip::BotValue::Boolean: lua_pushboolean(s, value.integer); break;
    case onchip::BotValue::Missing: lua_pushnil(s); break;
    }
  }
}
void captureOriginals(lua_State *s, Call &call) {
  if (call.manifest->count != onchip::BotBuiltinManifest.count)
    luaL_error(s, "Bundled command metadata changed; regenerate BotBuiltinManifest.h");
  lua_createtable(s, 0, onchip::BotBuiltinManifest.count);
  for (unsigned i = 0; i < onchip::BotBuiltinManifest.count; ++i) {
    const auto &entry = onchip::BotBuiltinManifest.commands[i];
    const auto *actual = call.manifest->find(entry.name);
    if (!actual || memcmp(actual, &entry, sizeof(entry)))
      luaL_error(s, "Bundled command %s changed; regenerate BotBuiltinManifest.h", entry.name);
    lua_getglobal(s, entry.function);
    lua_setfield(s, -2, entry.name);
  }
  call.originalReference = luaL_ref(s, LUA_REGISTRYINDEX);
}
// The synchronous diagnostic harness does not keep bundled handlers loaded.
// Build their private environment only when dispatch needs an original.
void ensureOriginals(lua_State *s, Call &call) {
  if (call.originalReference != LUA_NOREF) return;
  const bool initializing = call.initializing, bundled = call.bundled;
  call.initializing = call.bundled = true;
  call.buildingOriginals = true;
  call.builtinDeclarations = 0;
  lua_newtable(s);
  const int environment = lua_gettop(s);
  lua_newtable(s);
  lua_pushglobaltable(s); lua_setfield(s, -2, "__index");
  lua_setmetatable(s, environment);
  for (const char *source : {onchip::BotDefaultSource, onchip::BotUtilitySource,
                            onchip::BotNetworkSource, onchip::BotBoardSource, onchip::BotDiagnosticSource}) {
    budget(s).transition(Budget::Load);
    if (luaL_loadbufferx(s, source, strlen(source), "bot-originals", "t") != LUA_OK)
      lua_error(s);
    lua_pushvalue(s, environment); lua_setupvalue(s, -2, 1);
    budget(s).transition(Budget::Init);
    lua_call(s, 0, 0);
  }
  if (call.builtinDeclarations != (uint32_t(1) << onchip::BotBuiltinManifest.count) - 1)
    luaL_error(s, "Bundled command metadata changed; regenerate BotBuiltinManifest.h");
  lua_createtable(s, 0, onchip::BotBuiltinManifest.count);
  for (unsigned i = 0; i < onchip::BotBuiltinManifest.count; ++i) {
    const auto &entry = onchip::BotBuiltinManifest.commands[i];
    lua_getfield(s, environment, entry.function);
    lua_setfield(s, -2, entry.name);
  }
  call.originalReference = luaL_ref(s, LUA_REGISTRYINDEX);
  lua_pop(s, 1);
  call.buildingOriginals = false;
  call.initializing = initializing; call.bundled = bundled;
  budget(s).transition(Budget::Invoke);
}
int originalReturned(lua_State *s, int, lua_KContext) { return 1; }
int callOriginal(lua_State *s) {
  auto &call = context(s);
  if (call.initializing || !call.event || call.event->kind != onchip::BotEvent::Command)
    return luaL_error(s, "call_original requires a command invocation");
  if (lua_gettop(s) < 1 || lua_gettop(s) > 2)
    return luaL_error(s, "call_original accepts a builtin name and optional argument text; ctx is implicit");
  char name[onchip::BotNameLimit + 1];
  boundedString(s, 1, name, sizeof(name));
  const auto *entry = onchip::BotBuiltinManifest.find(name);
  if (!entry) return luaL_error(s, "Unknown builtin command !%s", name);
#if ONCHIP_BOT_COMPACT_PROFILE
  if (entry->permission == onchip::BotCommand::Home)
    return luaL_error(s, "!%s HTTPS is unsupported on this platform", entry->name);
#endif
  if (!onchip::botCommandAllowed(*entry, *call.event))
    return luaL_error(s, "!%s permission not granted: authenticated private DM or verified channel scope required", entry->name);
  const char *text = call.event->arguments;
  if (!lua_isnoneornil(s, 2)) {
    if (lua_type(s, 2) != LUA_TSTRING) return luaL_error(s, "Original arguments must be text; ctx is implicit");
    size_t size;
    text = lua_tolstring(s, 2, &size);
    if (size > onchip::BotArgumentsLimit || memchr(text, 0, size))
      return luaL_error(s, "Original arguments exceed text/NUL bound");
  }
  onchip::BotArguments arguments;
  char error[128];
  if (!onchip::parseBotArguments(*entry, text, arguments, error, sizeof(error)))
    return luaL_error(s, "!%s: %s", entry->name, error);
  ensureOriginals(s, call);
  lua_rawgeti(s, LUA_REGISTRYINDEX, call.originalReference);
  lua_getfield(s, -1, name); lua_remove(s, -2);
  pushArguments(s, arguments);
  lua_callk(s, arguments.count, 1, 0, originalReturned);
  return 1;
}
int reply(lua_State *s) {
  auto &call = context(s);
  if (call.event && call.event->kind != onchip::BotEvent::Command)
    return luaL_error(s, "Event subscriptions have no RF reply route");
  if (call.initializing || !call.action || call.action->kind != onchip::BotAction::None ||
      call.replyRequested)
    return luaL_error(s, "reply requires an invocation and at most one action");
  if (lua_type(s, 1) != LUA_TSTRING) return luaL_error(s, "reply text must be a string");
  size_t size;
  const char *value = lua_tolstring(s, 1, &size);
  if (!size || size > call.event->replyLimit || size > onchip::BotReplyLimit)
    return luaL_error(s, "reply exceeds %d bytes", int(call.event->replyLimit));
  for (size_t i = 0; i < size; ++i)
    if (static_cast<unsigned char>(value[i]) < 32 || static_cast<unsigned char>(value[i]) > 126)
      return luaL_error(s, "reply must be printable ASCII");
  if (call.retained) {
    auto &request = prepareIo(s, onchip::BotIoRequest::Send);
    memcpy(request.value, value, size); request.value[size] = 0;
    request.delayMs = 5000; request.reply = true;
    call.replyRequested = true;
    return lua_yieldk(s, 0, 0, finishIo);
  }
  call.action->kind = onchip::BotAction::Reply;
  memcpy(call.action->text, value, size); call.action->text[size] = 0;
  return 0;
}
int trace(lua_State *s) {
  auto &call = context(s);
  if (call.event && call.event->kind != onchip::BotEvent::Command)
    return luaL_error(s, "Event subscriptions have no RF trace route");
  if (call.retained) return traceRoute(s);
  if (call.initializing || !call.event || !call.action || call.action->kind != onchip::BotAction::None)
    return luaL_error(s, "trace requires an invocation and at most one action");
  const auto &event = *call.event;
  if (!event.routeSize || event.routeSize > onchip::BotTraceLimit ||
      (event.routeWidth != 1 && event.routeWidth != 2 &&
       event.routeWidth != 4 && event.routeWidth != 8) ||
      event.routeSize % event.routeWidth || event.routeSize / event.routeWidth > onchip::BotTraceHopLimit)
    return luaL_error(s, "TRACE has no validated direct route");
  call.action->kind = onchip::BotAction::Trace;
  return 0;
}
int commandHelp(lua_State *s) {
  auto &call = context(s);
  const char *name = lua_isnoneornil(s, 1) ? "" : luaL_checkstring(s, 1);
  char output[onchip::BotReplyLimit + 1]{};
  const size_t limit = call.event ? call.event->replyLimit : onchip::BotReplyLimit;
  unsigned page = 1;
  if (!lua_isnoneornil(s, 2)) {
    const auto value = luaL_checkinteger(s, 2);
    if (value < 1 || value > 40) return luaL_error(s, "Help page requires 1..40");
    page = unsigned(value);
  }
  if (*name >= '0' && *name <= '9') {
    if (!lua_isnoneornil(s, 2)) return luaL_error(s, "Use !help PAGE or !help NAME [PAGE]");
    page = 0;
    for (const char *p = name; *p; ++p) {
      if (*p < '0' || *p > '9' || page > 40) return luaL_error(s, "Invalid help page");
      page = page * 10 + unsigned(*p - '0');
    }
    if (!page || page > 40) return luaL_error(s, "Help page requires 1..40");
    name = "";
  }
  const auto available = [&](const onchip::BotCommand &command) {
    return !call.event || onchip::botCommandAllowed(command, *call.event);
  };
  if (*name) {
    auto *entry = call.manifest->find(name);
    if (!entry && call.installed) entry = call.installed->find(name);
    if (!entry) snprintf(output, sizeof(output), "Error: unknown command !%s", name);
    else if (!available(*entry))
      snprintf(output, sizeof(output), "Error: !%s unavailable here; requires its native scope/grant", entry->name);
    else {
      char detail[320];
      snprintf(detail, sizeof(detail), "!%s %s: %s%s%s", entry->name, entry->schema, entry->help,
               entry->example[0] ? "; e.g. " : "", entry->example);
      const size_t size = strlen(detail);
      if (limit <= 50) return luaL_error(s, "Help exceeds RF capacity");
      const size_t chunk = size <= limit ? limit : limit - 50;
      const unsigned pages = unsigned((size + chunk - 1) / chunk);
      if (page > pages) return luaL_error(s, "Command help has %d pages", int(pages));
      if (pages == 1) strcpy(output, detail);
      else {
        snprintf(output, sizeof(output), "Help %u/%u: %.*s", page, pages, int(chunk), detail + (page - 1) * chunk);
        if (page < pages) {
          char next[40]; snprintf(next, sizeof(next), "; !help %s %u", entry->name, page + 1); strcat(output, next);
        }
      }
    }
  } else {
    const onchip::BotCommand *commands[onchip::BotBuiltinCommandLimit + 2 * onchip::BotCommandLimit]{};
    unsigned count = 0;
    const onchip::BotManifestView *manifests[] = {call.installed, call.manifest};
    for (const auto *manifest : manifests)
      if (manifest) for (unsigned i = 0; i < manifest->count; ++i) {
        const auto &entry = manifest->commands[i];
        if (!available(entry)) continue;
        if (manifest == call.manifest && call.installed && call.installed->find(entry.name)) continue;
        commands[count++] = &entry;
      }
    std::sort(commands, commands + count, [](const onchip::BotCommand *a, const onchip::BotCommand *b) {
      return strcmp(a->name, b->name) < 0;
    });
    unsigned starts[onchip::BotBuiltinCommandLimit + 2 * onchip::BotCommandLimit + 1]{}, pages = 1;
    size_t used = 0;
    for (unsigned i = 0; i < count; ++i) {
      const size_t size = strlen(commands[i]->name) + 2;
      if (size + 42 > limit) return luaL_error(s, "Help exceeds RF capacity");
      if (used + size + 42 > limit) { starts[pages++] = i; used = 0; }
      used += size;
    }
    starts[pages] = count;
    if (page > pages) return luaL_error(s, "Help has %d pages", int(pages));
    snprintf(output, sizeof(output), "Help %u/%u:", page, pages);
    for (unsigned i = starts[page - 1]; i < starts[page]; ++i) {
      strcat(output, " !"); strcat(output, commands[i]->name);
    }
    char suffix[36];
    if (page < pages) snprintf(suffix, sizeof(suffix), "; !help %u; !help NAME", page + 1);
    else strcpy(suffix, "; !help NAME [PAGE]");
    strcat(output, suffix);
  }
  if (strlen(output) > limit) {
    if (limit < 3) return luaL_error(s, "Help exceeds RF capacity");
    strcpy(output + limit - 3, "...");
  }
  lua_pushstring(s, output);
  return 1;
}
int pluginInfo(lua_State *s) {
  auto &call = context(s);
  if (!call.event) return luaL_error(s, "Plugin discovery requires an invocation");
  const lua_Integer page = lua_isnoneornil(s, 1) ? 1 : luaL_checkinteger(s, 1);
  const auto &manifest = call.installed ? *call.installed : *call.manifest;
  const unsigned pages = 1 + (manifest.moduleCount + 2) / 3;
  if (page < 1 || page > pages) return luaL_error(s, "Plugins has %d pages", int(pages));
  char output[onchip::BotReplyLimit + 1]{};
  if (page == 1)
    snprintf(output, sizeof(output), "Bundled %u commands; active g%u %s %u commands, %u modules, events=%u%s",
             onchip::BotBuiltinCommandLimit, call.event->node.sourceGeneration,
             manifest.isBundled() ? "bundled" : "custom", manifest.count, manifest.moduleCount,
             manifest.eventMask, pages > 1 ? "; !plugins 2" : "; shared Lua namespace");
  else {
    snprintf(output, sizeof(output), "Modules %u/%u:", unsigned(page), pages);
    for (unsigned i = unsigned(page - 2) * 3; i < manifest.moduleCount && i < unsigned(page - 1) * 3; ++i) {
      strcat(output, " "); strcat(output, manifest.modules[i]);
    }
    if (page < pages) {
      char next[24]; snprintf(next, sizeof(next), "; !plugins %u", unsigned(page + 1)); strcat(output, next);
    }
  }
  if (strlen(output) > call.event->replyLimit) return luaL_error(s, "Plugin info exceeds RF capacity");
  lua_pushstring(s, output); return 1;
}
int nodeNeighbors(lua_State *s) {
  const lua_Integer page = lua_isnoneornil(s, 1) ? 1 : luaL_checkinteger(s, 1);
  if (page < 1 || page > 16) return luaL_error(s, "Neighbors page requires 1..16");
  auto &request = prepareIo(s, onchip::BotIoRequest::Inspect);
  strcpy(request.key, "neighbors"); request.revision = uint32_t(page); request.delayMs = 5000;
  return lua_yieldk(s, 0, 0, finishIo);
}
int nodeAdmin(lua_State *s) {
  const auto &event = context(s).event;
  if (!event || !event->owner || !event->authenticated || event->channel[0] || event->local)
    return luaL_error(s, "Admin requires an authenticated trusted-owner DM");
  auto &request = prepareIo(s, onchip::BotIoRequest::Admin);
  boundedString(s, 1, request.value, onchip::BotReplyLimit + 1);
  request.delayMs = 5000;
  return lua_yieldk(s, 0, 0, finishIo);
}
void helper(lua_State *s, Call &call, const char *name, lua_CFunction function) {
  lua_pushcfunction(s, function); lua_setglobal(s, name);
}
int utilityCall(lua_State *s) {
  auto &call = context(s);
  if (call.initializing || !call.event) return luaL_error(s, "Utilities require an invocation");
  const auto &e = *call.event;
  if (e.local || !(e.authenticated ? !e.channel[0] : e.channelVerified && e.channel[0]))
    return luaL_error(s, "Utilities require authenticated DM or verified selected channel");
  if (++call.utilityCalls > 8 || budget(s).expired())
    return luaL_error(s, "Utility invocation budget exceeded (8 calls)");
  const unsigned operation = unsigned(lua_tointeger(s, lua_upvalueindex(1)));
  const unsigned count = operation == 1 ? 3 : 1;
  if (unsigned(lua_gettop(s)) > count) return luaL_error(s, "Unexpected utility arguments");
  const char *args[3]{};
  for (unsigned i = 0; i < count; ++i) {
    if (operation == 2 && lua_isnoneornil(s, i + 1)) continue;
    luaL_checktype(s, i + 1, LUA_TSTRING);
    size_t size;
    args[i] = lua_tolstring(s, i + 1, &size);
    const size_t limit = operation == 0 ? 120 : operation == 1 ? (i ? 4 : 32) : operation == 2 ? 24 : 148;
    if (!size || size > limit || memchr(args[i], 0, size))
      return luaL_error(s, "Utility argument empty, too long or contains NUL");
  }
  onchip::BotUtilityResult result;
  const bool ok = operation == 0 ? onchip::botCalculate(args[0], result) :
                  operation == 1 ? onchip::botConvert(args[0], args[1], args[2], result) :
                  operation == 2 ? onchip::botRoll(args[0], result) :
                                   onchip::botChoose(args[0], result);
  if (!ok) return luaL_error(s, "%s", result.error);
  if (budget(s).expired()) return luaL_error(s, "Utility wall budget exceeded");
  if (!rfText(result.text, e.replyLimit)) return luaL_error(s, "Utility reply exceeds RF capacity");
  lua_pushstring(s, result.text);
  return 1;
}
void utilityApi(lua_State *s) {
  lua_newtable(s);
  unsigned operation = 0;
  for (const char *name : {"calc", "convert", "roll", "choose"}) {
    lua_pushinteger(s, operation++);
    lua_pushcclosure(s, utilityCall, 1); lua_setfield(s, -2, name);
  }
  freeze(s); lua_setglobal(s, "utility");
}
int nodeReport(lua_State *s) {
  auto &call = context(s);
  if (call.initializing || !call.event)
    return luaL_error(s, "Diagnostics require an invocation");
  if (lua_gettop(s) > 2) return luaL_error(s, "Unexpected diagnostic arguments");
  luaL_checktype(s, 1, LUA_TSTRING);
  size_t size;
  const char *name = lua_tolstring(s, 1, &size);
  if (!size || size > onchip::BotNameLimit || memchr(name, 0, size))
    return luaL_error(s, "Invalid diagnostic name");
  unsigned page = 1;
  if (!lua_isnoneornil(s, 2)) {
    if (strcmp(name, "air") || !lua_isinteger(s, 2) ||
        lua_tointeger(s, 2) < 1 || lua_tointeger(s, 2) > 4)
      return luaL_error(s, "Use !air [1..4]");
    page = unsigned(lua_tointeger(s, 2));
  }
  char output[onchip::BotReplyLimit + 1]{};
  if (!onchip::formatBotDiagnostic(*call.event, name, page,
          LUA_VERSION_MAJOR "." LUA_VERSION_MINOR "." LUA_VERSION_RELEASE,
          output, sizeof(output)))
    return luaL_error(s, "%s", output);
  lua_pushstring(s, output);
  return 1;
}
void nodeApi(lua_State *s) {
  lua_newtable(s);
  lua_pushcfunction(s, nodeReport); lua_setfield(s, -2, "report");
  lua_pushcfunction(s, pluginInfo); lua_setfield(s, -2, "plugins");
  lua_pushcfunction(s, nodeNeighbors); lua_setfield(s, -2, "neighbors");
  lua_pushcfunction(s, nodeAdmin); lua_setfield(s, -2, "admin");
  freeze(s); lua_setglobal(s, "node");
}
void inspectDeclarations(lua_State *s, Call &call) {
  lua_pushglobaltable(s);
  lua_pushnil(s);
  unsigned globals = 0;
  while (lua_next(s, -2)) {
    if (++globals > 64 || budget(s).expired())
      luaL_error(s, "command declaration inspection limit exceeded");
    if (lua_isfunction(s, -1) && !lua_iscfunction(s, -1)) {
      size_t size;
      const char *name = lua_type(s, -2) == LUA_TSTRING ? lua_tolstring(s, -2, &size) : nullptr;
      if (!name || size != strlen(name) || !onchip::botIdentifier(name))
        luaL_error(s, "exported commands require lowercase identifiers");
      if (name[0] == '_') { lua_pop(s, 1); continue; }
      if (!call.bundled && onchip::botReservedCommand(name)) {
        if (call.retained) { lua_pop(s, 1); continue; }
        else luaL_error(s, "Builtin command is reserved; use override_command(name, export)");
      }
      for (const char *helper : {"reply", "command", "override_command", "call_original", "request_trace", "command_help", "tostring", "ctx", "utility", "node", "module", "require", "events", "rpc", "http", "json"})
        if (!strcmp(name, helper)) luaL_error(s, "native helper name is reserved");
      bool registered = false;
      for (const auto &handler : call.manifest->events) registered = registered || !strcmp(handler, name);
      for (unsigned i = 0; i < call.manifest->count; ++i)
        registered = registered || !strcmp(call.manifest->commands[i].function, name);
      if (!registered) {
#if ONCHIP_BOT_COMPACT_PROFILE
        if (call.retained && call.bundled)
          luaL_error(s, "Bundled command metadata changed; regenerate BotBuiltinManifest.h");
#endif
        if (call.manifest->count == (call.bundled ? onchip::BotBuiltinCommandLimit : onchip::BotCommandLimit) ||
            call.manifest->find(name))
          luaL_error(s, "command registry full or duplicate inferred name");
        lua_Debug info{};
        lua_pushvalue(s, -1);
        if (!lua_getinfo(s, ">u", &info) || info.isvararg || info.nparams > onchip::BotParameterLimit)
          luaL_error(s, "inferred command requires zero to four fixed parameters");
        auto &entry = call.manifest->writable()[call.manifest->count];
        strcpy(entry.name, name); strcpy(entry.function, name);
        strcpy(entry.help, "Installed command");
        size_t used = 0;
        for (unsigned i = 1; i <= info.nparams; ++i) {
          // Source-only Lua retains parameter names; no debug library is exposed.
          const char *parameter = lua_getlocal(s, nullptr, i);
          if (!onchip::botIdentifier(parameter))
            luaL_error(s, "inferred parameter requires a lowercase identifier");
          const int added = snprintf(entry.schema + used, sizeof(entry.schema) - used,
                                     "%s%s:string:%u", i == 1 ? "" : ",", parameter,
                                     unsigned(onchip::BotArgumentsLimit));
          if (added < 0 || size_t(added) >= sizeof(entry.schema) - used)
            luaL_error(s, "inferred schema exceeds 96 bytes; use explicit command schema");
          used += added;
        }
        char error[128];
        if (!onchip::validateBotSchema(entry.schema, error, sizeof(error)))
          luaL_error(s, "%s", error);
        ++call.manifest->count;
      }
    }
    lua_pop(s, 1);
  }
  lua_pop(s, 1);
#if ONCHIP_BOT_COMPACT_PROFILE
  if (call.retained && call.bundled) return;
#endif
  std::sort(call.manifest->writable(), call.manifest->writable() + call.manifest->count,
            [](const onchip::BotCommand &a, const onchip::BotCommand &b) {
              return strcmp(a.name, b.name) < 0;
            });
}
int execute(lua_State *s) {
  auto &call = *static_cast<Call *>(lua_touserdata(s, 1));
  luaL_checkversion(s);
  lua_pushcfunction(s, stringInteger); lua_setglobal(s, "tostring");
  helper(s, call, "command", registerCommand);
  helper(s, call, "override_command", registerOverride);
  helper(s, call, "call_original", callOriginal);
  helper(s, call, "reply", reply);
  helper(s, call, "request_trace", trace);
  helper(s, call, "command_help", commandHelp);
  utilityApi(s);
  nodeApi(s);
  moduleApi(s);
  eventApi(s);
  repeaterApi(s);
  jsonApi(s);
  networkApi(s);
  if (luaL_loadbufferx(s, call.source, call.size, "registered-commands", "t") != LUA_OK)
    return lua_error(s);
  budget(s).transition(Budget::Init);
  lua_call(s, 0, 0);
  if (call.bundled) for (const char *source : {onchip::BotUtilitySource, onchip::BotNetworkSource, onchip::BotBoardSource, onchip::BotDiagnosticSource}) {
    budget(s).transition(Budget::Load);
    if (luaL_loadbufferx(s, source, strlen(source), "bot-bundled", "t") != LUA_OK)
      return lua_error(s);
    budget(s).transition(Budget::Init);
    lua_call(s, 0, 0);
  }
  initializeModules(s);
  validateEvents(s, call);
  validateNetworkApi(s);
  inspectDeclarations(s, call);
  if (!call.manifest->count && !call.manifest->eventMask) return luaL_error(s, "source must declare exported command functions or events");
  for (unsigned i = 0; i < call.manifest->count; ++i) {
    lua_getglobal(s, call.manifest->commands[i].function);
    if (!lua_isfunction(s, -1) || lua_iscfunction(s, -1))
      return luaL_error(s, "registered export is not a Lua function");
    lua_pop(s, 1);
  }
  call.initializing = false;
  if (!call.event) return 0;
  budget(s).transition(Budget::Invoke);
  if (call.event->kind != onchip::BotEvent::Command) {
    const unsigned index = unsigned(call.event->kind) - 1;
    if (!(call.manifest->eventMask & (1u << index))) return luaL_error(s, "Event subscription unavailable");
    call.subscriptionOwner = &call;
    pushEvent(s, *call.event); lua_setglobal(s, "ctx");
    lua_getglobal(s, call.manifest->events[index]); lua_call(s, 0, 0);
    return 0;
  }
  const auto *entry = call.manifest->find(call.event->name);
  if (!entry && onchip::botReservedCommand(call.event->name)) {
    ensureOriginals(s, call);
    entry = onchip::BotBuiltinManifest.find(call.event->name);
  }
  if (!entry) return luaL_error(s, "unknown command !%s", call.event->name);
#if ONCHIP_BOT_COMPACT_PROFILE
  if (entry->permission == onchip::BotCommand::Home)
    return luaL_error(s, "!%s HTTPS is unsupported on this platform", entry->name);
#endif
  if (!onchip::botCommandAllowed(*entry, *call.event))
    return luaL_error(s, "!%s permission not granted: authenticated private DM or verified channel scope required", entry->name);
  onchip::BotArguments arguments;
  char error[128];
  if (!onchip::parseBotArguments(*entry, call.event->arguments, arguments, error, sizeof(error)))
    return luaL_error(s, "!%s: %s", entry->name, error);
  pushEvent(s, *call.event); lua_setglobal(s, "ctx");
  if (onchip::botReservedCommand(entry->name) && !call.manifest->find(entry->name)) {
    lua_rawgeti(s, LUA_REGISTRYINDEX, call.originalReference);
    lua_getfield(s, -1, entry->name); lua_remove(s, -2);
  } else lua_getglobal(s, entry->function);
  pushArguments(s, arguments);
  lua_call(s, arguments.count, 1);
  if (call.action->kind == onchip::BotAction::None && lua_type(s, -1) == LUA_TSTRING) {
    lua_pushcfunction(s, reply); lua_insert(s, -2); lua_call(s, 1, 0);
  }
  if (call.action->kind == onchip::BotAction::None)
    return luaL_error(s, "command completed without an action");
  return 0;
}
bool validEvent(const onchip::BotEvent &event) {
  if (event.kind > onchip::BotEvent::Scheduled ||
      !memchr(event.message, 0, sizeof(event.message)) ||
      !memchr(event.name, 0, sizeof(event.name)) ||
      !memchr(event.arguments, 0, sizeof(event.arguments)) ||
      !memchr(event.channel, 0, sizeof(event.channel)) ||
      !memchr(event.nickname, 0, sizeof(event.nickname)) ||
      !memchr(event.node.nativeRevision, 0, sizeof(event.node.nativeRevision)) ||
      !memchr(event.node.build, 0, sizeof(event.node.build)) ||
      !memchr(event.node.name, 0, sizeof(event.node.name)) ||
      event.node.uptimeMs > uint64_t(INT64_MAX) ||
      !event.replyLimit || event.replyLimit > onchip::BotReplyLimit ||
      event.observationCount > onchip::BotObservationLimit || event.windowMs > 30000 ||
      (event.path.known && !event.path.valid()) ||
      !std::isfinite(event.rssi) || !std::isfinite(event.snr) ||
      event.rssi < -150 || event.rssi > 0 || event.snr < -32 || event.snr > 32) return false;
  for (unsigned i = 0; i < event.observationCount; ++i)
    if (!event.observations[i].valid()) return false;
  return true;
}
bool run(const char *source, size_t size, const onchip::BotEvent *event,
         onchip::BotAction *action, onchip::BotVmStats &stats,
         char *error, size_t errorSize, onchip::BotVmLimits limits,
         onchip::BotManifest *manifest, const onchip::BotManifest *installed) {
  stats = {};
  if (action) *action = {};
  if (manifest) manifest->clear();
  if (event && !validEvent(*event)) {
    snprintf(error, errorSize, "Invalid bounded command event");
    return false;
  }
  if (!source || !size || size > onchip::BotSourceLimit ||
      memchr(source, 0, size) || static_cast<unsigned char>(source[0]) == 27) {
    snprintf(error, errorSize, "Expected 1..4096 bytes of Lua source text");
    return false;
  }
  Budget b{luaSourceLimits(source, size, limits)};
  lua_State *s = lua_newstate(allocate, &b, luaL_makeseed(nullptr));
  if (!s) {
    snprintf(error, errorSize, "Command VM allocation/time budget exceeded");
    b.transition(Budget::Cleanup);
    stats = b.stats;
    return false;
  }
  Call call{source, size, event, action};
  std::unique_ptr<onchip::BotManifest> ownedManifest(new (std::nothrow) onchip::BotManifest);
  if (!ownedManifest) {
    lua_close(s);
    snprintf(error, errorSize, "Command manifest storage unavailable");
    return false;
  }
  call.manifest = ownedManifest.get();
  call.budget = &b;
  *static_cast<Call **>(lua_getextraspace(s)) = &call;
  lua_sethook(s, hook, LUA_MASKCOUNT, 1);
  call.installed = installed;
  call.bundled = bundledSource(source, size);
  lua_pushcfunction(s, execute);
  lua_pushlightuserdata(s, &call);
  const int result = lua_pcall(s, 1, 0, 0);
  bool ok = result == LUA_OK && !b.expired();
  if (!ok) {
    const char *message = result != LUA_OK && lua_type(s, -1) == LUA_TSTRING
                              ? lua_tostring(s, -1) : "Command wall deadline exceeded";
    snprintf(error, errorSize, "%s", message);
    if (action) *action = {};
  }
  b.transition(Budget::Cleanup);
  lua_close(s);
  b.transition(Budget::Cleanup);
  if (b.timedOut) {
    ok = false;
    if (action) *action = {};
    snprintf(error, errorSize, "Command wall deadline exceeded during %s", b.failurePhase());
  }
  stats = b.stats;
  const bool fallback = !ok && event && onchip::botReservedCommand(event->name) &&
                        !bundledSource(source, size) && !ownedManifest->find(event->name);
  if (fallback)
    return run(onchip::BotDefaultSource, strlen(onchip::BotDefaultSource), event,
               action, stats, error, errorSize, limits, manifest, installed);
  if (ok && manifest) *manifest = *call.manifest;
  return ok;
}
} // namespace

// Called by the pinned lexer for every consumed source character. Lua's normal
// instruction hook does not cover compilation.
extern "C" void onchip_lua_parser_step(lua_State *s) {
  auto &b = budget(s);
  if (++b.stats.parserSteps > b.limits.parserSteps || b.expired())
    luaL_error(s, "command compile/time budget exceeded");
}
namespace onchip {
bool BotVm::validate(const char *source, size_t size, BotVmStats &stats,
                     char *error, size_t errorSize, BotVmLimits limits, BotManifest *manifest) {
  if (source && size >= 21 && !memcmp(source, "--@meshcore-sources/1\n", 21)) {
    BotSession vm;
    const bool ok = vm.load(source, size, 1, stats, error, errorSize, limits);
    if (manifest) { if (ok) *manifest = vm.manifest(); else manifest->clear(); }
    return ok;
  }
#if !ONCHIP_BOT_WASM
  if (botSourceIsWasm(source, size)) {
    stats = {}; if (manifest) manifest->clear();
    snprintf(error, errorSize, "Wasm runtime unavailable in this build"); return false;
  }
#endif
#if ONCHIP_BOT_WASM
  if (botSourceIsWasm(source, size)) {
    BotSession vm;
    const bool ok = vm.load(source, size, 1, stats, error, errorSize, limits);
    if (manifest) {
      if (ok) *manifest = vm.manifest();
      else manifest->clear();
    }
    return ok;
  }
#endif
  return run(source, size, nullptr, nullptr, stats, error, errorSize, limits, manifest, nullptr);
}
bool BotVm::invoke(const char *source, size_t size, const BotEvent &event,
                   BotAction &action, BotVmStats &stats, char *error,
                   size_t errorSize, BotVmLimits limits, const BotManifest *installed) {
  if (source && size >= 21 && !memcmp(source, "--@meshcore-sources/1\n", 21)) {
    BotSession vm;
    if (!vm.load(source, size, 1, stats, error, errorSize, limits) ||
        !vm.start(1, event, error, errorSize)) return false;
    BotSession::Result result;
    if (!vm.poll(result)) {
      snprintf(error, errorSize, "Async source invocation requires retained BotSession"); return false;
    }
    action = result.action;
    if (!result.ok) snprintf(error, errorSize, "%s", result.error);
    return result.ok;
  }
#if !ONCHIP_BOT_WASM
  if (botSourceIsWasm(source, size)) {
    action = {}; stats = {};
    snprintf(error, errorSize, "Wasm runtime unavailable in this build"); return false;
  }
#endif
#if ONCHIP_BOT_WASM
  if (validEvent(event) && botSourceIsWasm(source, size)) {
    if (botReservedCommand(event.name))
      return run(BotDefaultSource, strlen(BotDefaultSource), &event, &action, stats,
                 error, errorSize, limits, nullptr, installed);
    BotSession vm;
    action = {};
    if (!vm.load(source, size, 1, stats, error, errorSize, limits) ||
        !vm.start(1, event, error, errorSize)) return false;
    BotSession::Result result;
    if (!vm.poll(result)) {
      snprintf(error, errorSize, "Wasm async invocation requires retained BotSession continuations"); return false;
    }
    result.stats.loadUs = stats.loadUs; result.stats.initUs = stats.initUs;
    stats = result.stats; action = result.action;
    if (!result.ok) snprintf(error, errorSize, "%s", result.error);
    return result.ok;
  }
#endif
  return run(source, size, &event, &action, stats, error, errorSize, limits, nullptr, installed);
}

struct BotSession::Impl {
  struct Job {
    enum State { Free, Running, Waiting, Done } state = Free;
    lua_State *thread = nullptr;
    int reference = LUA_NOREF;
    BotEvent event{};
    Result result{};
    BotIoRequest request{};
    BotIoResult completion{};
    bool dispatched = false;
    Budget budget{};
    Call call{};
  };
  using Jobs = std::array<Job, BotJobLimit>;
#if ONCHIP_BOT_COMPACT_PROFILE
  // Separate blocks avoid requiring one contiguous buffer for all jobs.
  std::array<std::unique_ptr<Job>, BotJobLimit> jobStorage;
  Job &job(unsigned index) { return *jobStorage[index]; }
#else
  Jobs jobStorage;
  Job &job(unsigned index) { return jobStorage[index]; }
#endif
  Budget heap{};
  Call loader{};
  BotSourcePart sourceParts[BotSourcePartLimit]{};
  char sourceError[128]{};
  lua_State *state = nullptr;
  int environment = LUA_NOREF;
  uint32_t generation = 0;
#if ONCHIP_BOT_COMPACT_PROFILE
  BotManifestView natives{BotBuiltinManifest};
  BotProgramManifest program{};
#else
  BotManifest natives{};
  BotManifest program{};
#endif
#if ONCHIP_BOT_SEPARATE_DIAGNOSTICS
  BotManifest help{};
  bool helpSet = false;
#endif
  Job *preparing = nullptr;

  static int initialize(lua_State *s) {
    auto &self = *static_cast<Impl *>(lua_touserdata(s, 1));
    auto &call = self.loader;
    luaL_checkversion(s);
    helper(s, call, "tostring", stringInteger);
    helper(s, call, "command", registerCommand);
    helper(s, call, "override_command", registerOverride);
    helper(s, call, "call_original", callOriginal);
    helper(s, call, "reply", reply);
    helper(s, call, "request_trace", trace);
    helper(s, call, "command_help", commandHelp);
    helper(s, call, "sleep", sleepFor);
    helper(s, call, "advert", sendAdvert);
    utilityApi(s);
    nodeApi(s);
    moduleApi(s);
    eventApi(s);
    repeaterApi(s);
    jsonApi(s);
    networkApi(s);
    lua_newtable(s);
    lua_pushcfunction(s, sleepFor); lua_setfield(s, -2, "sleep");
    lua_pushcfunction(s, timerSet); lua_setfield(s, -2, "set");
    lua_pushcfunction(s, timerGet); lua_setfield(s, -2, "get");
    lua_pushcfunction(s, timerCancel); lua_setfield(s, -2, "cancel");
    lua_pushcfunction(s, timerWait); lua_setfield(s, -2, "wait");
    freeze(s); lua_setglobal(s, "timer");
    lua_newtable(s);
    lua_pushcfunction(s, reminderAfter); lua_setfield(s, -2, "after");
    lua_pushcfunction(s, reminderList); lua_setfield(s, -2, "list");
    lua_pushcfunction(s, reminderCancel); lua_setfield(s, -2, "cancel");
    freeze(s); lua_setglobal(s, "reminder");
    lua_newtable(s);
    lua_pushcfunction(s, stateGet); lua_setfield(s, -2, "get");
    lua_pushcfunction(s, statePut); lua_setfield(s, -2, "put");
    lua_pushcfunction(s, stateDelete); lua_setfield(s, -2, "delete");
    lua_pushcfunction(s, stateList); lua_setfield(s, -2, "list");
    lua_pushcfunction(s, stateCas); lua_setfield(s, -2, "cas");
    lua_pushcfunction(s, stateTransaction); lua_setfield(s, -2, "transaction");
    freeze(s); lua_setglobal(s, "kv");
    lua_newtable(s);
    lua_pushcfunction(s, compose); lua_setfield(s, -2, "compose");
    lua_pushcfunction(s, sendDraft); lua_setfield(s, -2, "send");
    lua_pushcfunction(s, forwardPacket); lua_setfield(s, -2, "forward");
    lua_pushcfunction(s, waitPacket); lua_setfield(s, -2, "wait");
    lua_pushcfunction(s, traceRoute); lua_setfield(s, -2, "trace");
    lua_pushcfunction(s, multiTrace); lua_setfield(s, -2, "multitrace");
    lua_pushcfunction(s, sendAdvert); lua_setfield(s, -2, "advert");
    freeze(s); lua_setglobal(s, "mesh");
    lua_newuserdatauv(s, 1, 0);
    lua_newtable(s);
    lua_pushcfunction(s, contextIndex); lua_setfield(s, -2, "__index");
    lua_pushcfunction(s, readonly); lua_setfield(s, -2, "__newindex");
    lua_pushboolean(s, false); lua_setfield(s, -2, "__metatable");
    lua_setmetatable(s, -2); lua_setglobal(s, "ctx");
    lua_newtable(s);
    lua_newtable(s);
    lua_pushglobaltable(s); lua_setfield(s, -2, "__index");
    lua_pushglobaltable(s); lua_pushcclosure(s, environmentWrite, 1);
    lua_setfield(s, -2, "__newindex");
    lua_pushboolean(s, false); lua_setfield(s, -2, "__metatable");
    lua_setmetatable(s, -2);
    self.environment = luaL_ref(s, LUA_REGISTRYINDEX);
    lua_newtable(s); call.sourceOwnersReference = luaL_ref(s, LUA_REGISTRYINDEX);
    const char *source = call.source;
    const size_t size = call.size;
    const bool bundled = bundledSource(source, size);
    for (unsigned phase = 0; phase < (bundled ? 1u : 2u); ++phase) {
#if ONCHIP_BOT_COMPACT_PROFILE
      call.manifest = phase ? static_cast<BotManifestView *>(&self.program) : &self.natives;
      if (phase) call.manifest->clear();
#else
      call.manifest->clear();
#endif
      call.bundled = phase == 0;
      auto &parts = self.sourceParts;
      for (auto &part : parts) part = {};
      unsigned count = 1;
      auto &error = self.sourceError;
      error[0] = 0;
      if (phase) {
        if (!splitBotSources(source, size, parts, count, error, sizeof(error)))
          return luaL_error(s, "%s", error);
      } else {
        strcpy(parts[0].name, "bundled");
        parts[0].text = BotDefaultSource; parts[0].size = strlen(BotDefaultSource);
      }
      for (unsigned i = 0; i < count; ++i) {
        if (phase && (parts[i].builtin || bundledSource(parts[i].text, parts[i].size))) continue;
        call.sourcePart = phase ? i + 1 : 0;
        self.heap.transition(Budget::Load);
        const char *chunk = !phase || parts[i].text == source ? "bot-commands" : parts[i].name;
        if (luaL_loadbufferx(s, parts[i].text, parts[i].size, chunk, "t") != LUA_OK)
          return lua_error(s);
        lua_rawgeti(s, LUA_REGISTRYINDEX, self.environment);
        lua_setupvalue(s, -2, 1);
        self.heap.transition(Budget::Init);
        lua_call(s, 0, 0);
      }
      if (!phase) for (const char *extra : {BotUtilitySource, BotNetworkSource, BotBoardSource, BotDiagnosticSource}) {
#if ONCHIP_BOT_COMPACT_PROFILE
        lua_gc(s, LUA_GCCOLLECT);
#endif
        self.heap.transition(Budget::Load);
        if (luaL_loadbufferx(s, extra, strlen(extra), "bot-bundled", "t") != LUA_OK)
          return lua_error(s);
        lua_rawgeti(s, LUA_REGISTRYINDEX, self.environment);
        lua_setupvalue(s, -2, 1);
        self.heap.transition(Budget::Init);
        lua_call(s, 0, 0);
      }
      initializeModules(s);
      validateEvents(s, call);
      validateNetworkApi(s);
      inspectDeclarations(s, call);
      const bool builtinOnly = phase && count == 1 && parts[0].builtin;
      if (!call.manifest->count && !call.manifest->eventMask && !builtinOnly)
        return luaL_error(s, "source must declare exported command functions or events");
      for (unsigned i = 0; i < call.manifest->count; ++i) {
        lua_getglobal(s, call.manifest->commands[i].function);
        const bool valid = lua_isfunction(s, -1) && !lua_iscfunction(s, -1);
        lua_pop(s, 1);
        if (!valid) return luaL_error(s, "registered export is not a Lua function");
      }
      if (!phase) {
        captureOriginals(s, call);
#if ONCHIP_BOT_COMPACT_PROFILE
        static_assert(BotBuiltinManifest.count < 32, "Native declaration mask capacity");
        if (call.builtinDeclarations != (uint32_t(1) << BotBuiltinManifest.count) - 1 ||
            self.natives.moduleCount || self.natives.eventMask)
          return luaL_error(s, "Bundled command metadata changed; regenerate BotBuiltinManifest.h");
#else
        self.natives = *call.manifest;
#endif
      }
#if ONCHIP_BOT_COMPACT_PROFILE
      lua_gc(s, LUA_GCCOLLECT);
#endif
    }
    return 0;
  }
  static int prepare(lua_State *s) {
    auto &self = *static_cast<Impl *>(lua_touserdata(s, 1));
    auto &job = *self.preparing;
    if (job.event.kind != BotEvent::Command) {
      const unsigned index = unsigned(job.event.kind) - 1;
      if (!(self.loader.manifest->eventMask & (1u << index)))
        return luaL_error(s, "Event subscription unavailable");
      job.thread = lua_newthread(s);
      *static_cast<Call **>(lua_getextraspace(job.thread)) = &job.call;
      job.reference = luaL_ref(s, LUA_REGISTRYINDEX);
      lua_sethook(job.thread, hook, LUA_MASKCOUNT, 1);
      lua_getglobal(s, self.loader.manifest->events[index]);
      lua_xmove(s, job.thread, 1);
      return 0;
    }
    const auto *entry = self.loader.manifest->find(job.event.name);
    if (!entry) entry = self.natives.find(job.event.name);
    if (!entry) return luaL_error(s, "unknown command !%s", job.event.name);
#if ONCHIP_BOT_COMPACT_PROFILE
    if (entry->permission == BotCommand::Home)
      return luaL_error(s, "!%s HTTPS is unsupported on this platform", entry->name);
#endif
    if (!botCommandAllowed(*entry, job.event))
      return luaL_error(s, "!%s permission not granted: authenticated private DM or verified channel scope required", entry->name);
    BotArguments arguments;
    char error[128];
    if (!parseBotArguments(*entry, job.event.arguments, arguments, error, sizeof(error)))
      return luaL_error(s, "!%s: %s", entry->name, error);
    job.thread = lua_newthread(s);
    *static_cast<Call **>(lua_getextraspace(job.thread)) = &job.call;
    job.reference = luaL_ref(s, LUA_REGISTRYINDEX);
    lua_sethook(job.thread, hook, LUA_MASKCOUNT, 1);
    if (entry == self.natives.find(job.event.name)) {
      lua_rawgeti(s, LUA_REGISTRYINDEX, self.loader.originalReference);
      lua_getfield(s, -1, entry->name); lua_remove(s, -2);
    } else lua_getglobal(s, entry->function);
    pushArguments(s, arguments);
    if (!lua_checkstack(job.thread, arguments.count + 1))
      return luaL_error(s, "Coroutine stack capacity exhausted");
    lua_xmove(s, job.thread, arguments.count + 1);
    return 0;
  }
  void fail(Job &job, const char *message) {
    job.result.ok = false;
    job.result.action = {};
    snprintf(job.result.error, sizeof(job.result.error), "%s", message);
    job.state = Job::Done;
  }
  void resume(Job &job, int arguments) {
    auto &b = job.budget;
    b.phase = Budget::Invoke;
    b.phaseStarted = clockUs();
    b.started = b.phaseStarted - b.stats.elapsedUs;
    heap.active = &b; heap.running = true;
    job.state = Job::Running;
    int results = 0;
    const int status = lua_resume(job.thread, nullptr, arguments, &results);
    if (status != LUA_OK && status != LUA_YIELD)
      fail(job, lua_type(job.thread, -1) == LUA_TSTRING
                    ? lua_tostring(job.thread, -1) : "Command VM resource failure");
    else if (status == LUA_YIELD) {
      if (!job.request.token.operation) fail(job, "Unowned coroutine yield");
      else { job.state = Job::Waiting; job.dispatched = false; }
    } else {
      if (job.event.kind == BotEvent::Command && !job.call.replyCompleted &&
          job.result.action.kind == BotAction::None && results &&
          lua_type(job.thread, -results) == LUA_TSTRING) {
        size_t size;
        const char *text = lua_tolstring(job.thread, -results, &size);
        bool valid = size && size <= BotReplyLimit && size <= job.event.replyLimit;
        for (size_t i = 0; i < size; ++i)
          valid = valid && static_cast<unsigned char>(text[i]) >= 32 &&
                  static_cast<unsigned char>(text[i]) <= 126;
        if (valid) {
          job.result.action.kind = BotAction::Reply;
          memcpy(job.result.action.text, text, size);
          job.result.action.text[size] = 0;
        }
      }
      if (job.event.kind == BotEvent::Command && job.result.action.kind == BotAction::None && !job.call.replyCompleted)
        fail(job, "Command completed without a bounded reply/action");
      else { job.result.ok = true; job.state = Job::Done; }
    }
    b.transition(Budget::Invoke);
    if (b.timedOut) fail(job, "Command wall deadline exceeded during invocation");
    heap.active = nullptr; heap.running = false;
    job.result.stats = b.stats;
    job.result.stats.peakBytes = heap.stats.peakBytes;
  }
  void release(Job &job) {
    heap.running = false;
    if (job.thread) lua_closethread(job.thread, nullptr);
    if (job.call.eventReference != LUA_NOREF)
      luaL_unref(state, LUA_REGISTRYINDEX, job.call.eventReference);
    if (job.reference != LUA_NOREF) luaL_unref(state, LUA_REGISTRYINDEX, job.reference);
    job.~Job();
    new (&job) Job;
    // Retired coroutine allocations can outlive many invocations with Lua's
    // incremental GC; reclaim them before the bounded allocator runs dry.
    if (heap.live > heap.limits.heapBytes * 7 / 8)
      lua_gc(state, LUA_GCCOLLECT);
  }
};

const size_t BotSession::StorageBytes = sizeof(Impl)
#if ONCHIP_BOT_COMPACT_PROFILE
    + sizeof(Impl::Jobs)
#endif
    ;
const size_t BotSession::InitializationStorageBytes = sizeof(Impl);

BotSession::~BotSession() { clear(); }
void BotSession::clear() {
#if ONCHIP_BOT_WASM
  delete wasm_; wasm_ = nullptr;
#endif
  if (!impl_) return;
  impl_->heap.running = false;
  if (impl_->state) lua_close(impl_->state);
  impl_->~Impl();
#ifdef ARDUINO_ARCH_ESP32
  heap_caps_free(impl_);
#else
  free(impl_);
#endif
  impl_ = nullptr;
}
void BotSession::swap(BotSession &other) {
  std::swap(impl_, other.impl_);
#if ONCHIP_BOT_WASM
  std::swap(wasm_, other.wasm_);
#endif
}
bool BotSession::isWasm() const {
#if ONCHIP_BOT_WASM
  return wasm_ != nullptr;
#else
  return false;
#endif
}
bool BotSession::load(const char *source, size_t size, uint32_t generation,
                     BotVmStats &stats, char *error, size_t errorSize, BotVmLimits limits) {
  clear();
  stats = {};
#if !ONCHIP_BOT_WASM
  if (botSourceIsWasm(source, size)) {
    snprintf(error, errorSize, "Wasm runtime unavailable in this build"); return false;
  }
#endif
#if ONCHIP_BOT_WASM
  if (botSourceIsWasm(source, size)) {
    wasm_ = new (std::nothrow) BotWasmSession;
    if (!wasm_ || !wasm_->load(source, size, generation, stats, error, errorSize, limits)) {
      delete wasm_; wasm_ = nullptr; return false;
    }
    return true;
  }
#endif
  if (!generation || !source || !size || size > BotSourceLimit || memchr(source, 0, size) ||
      static_cast<unsigned char>(source[0]) == 27) {
    snprintf(error, errorSize, "Expected bounded Lua source text and nonzero generation");
    return false;
  }
  limits = luaSourceLimits(source, size, limits);
#ifdef ARDUINO_ARCH_ESP32
  void *memory = heap_caps_calloc(1, sizeof(Impl), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
  void *memory = calloc(1, sizeof(Impl));
#endif
  if (!memory) { snprintf(error, errorSize, "Retained VM storage unavailable"); return false; }
  impl_ = new (memory) Impl;
  auto &s = *impl_;
  s.generation = generation; s.heap.limits = limits;
  s.loader = {source, size, nullptr, nullptr};
  s.loader.manifest = &s.program;
  s.loader.budget = &s.heap; s.loader.retained = true;
  s.state = lua_newstate(allocate, &s.heap, luaL_makeseed(nullptr));
  if (!s.state) { snprintf(error, errorSize, "Retained Lua heap unavailable"); clear(); return false; }
  *static_cast<Call **>(lua_getextraspace(s.state)) = &s.loader;
  lua_sethook(s.state, hook, LUA_MASKCOUNT, 1);
  lua_pushcfunction(s.state, Impl::initialize); lua_pushlightuserdata(s.state, &s);
  const int result = lua_pcall(s.state, 1, 0, 0);
  s.heap.transition(Budget::Init);
  bool ok = result == LUA_OK && !s.heap.timedOut;
  if (!ok) {
    if (s.heap.timedOut) {
      const bool loading = s.heap.failedPhase == Budget::Load;
      snprintf(error, errorSize, "%s deadline %llu/%lluus peak=%uB",
               loading ? "Load" : "Init",
               static_cast<unsigned long long>(loading ? s.heap.stats.loadUs : s.heap.stats.initUs),
               static_cast<unsigned long long>(loading ? limits.loadWallUs : limits.initWallUs),
               unsigned(s.heap.stats.peakBytes));
    }
    else if (result == LUA_ERRMEM && s.heap.allocationFailure == Budget::HeapLimit)
      snprintf(error, errorSize, "Lua heap limit %uB peak=%uB", unsigned(limits.heapBytes), unsigned(s.heap.stats.peakBytes));
    else if (result == LUA_ERRMEM && s.heap.allocationFailure == Budget::AllocatorFailure)
#ifdef NRF52_PLATFORM
      snprintf(error, errorSize, "Lua allocation %u->%uB failed; free=%uB reserve=8192B peak=%uB",
               unsigned(s.heap.failedOldSize), unsigned(s.heap.failedSize), s.heap.failedFree,
               unsigned(s.heap.stats.peakBytes));
#else
      snprintf(error, errorSize, "Lua allocator failed peak=%uB cap=%uB", unsigned(s.heap.stats.peakBytes), unsigned(limits.heapBytes));
#endif
    else
      snprintf(error, errorSize, "%s", result != LUA_OK && lua_type(s.state, -1) == LUA_TSTRING
                                      ? lua_tostring(s.state, -1) : "Source initialization deadline exceeded");
  }
  stats = s.heap.stats;
  s.heap.running = false;
#if ONCHIP_BOT_COMPACT_PROFILE
  // Parser temporaries have been collected before reserving idle job buffers.
#ifdef NRF52_PLATFORM
  if (ok && sizeof(Impl::Jobs) + 8192u + BotJobLimit * 16u >
      unsigned(std::max(dbgHeapFree(), 0))) {
    snprintf(error, errorSize, "Lua job buffers need %uB; free=%uB reserve=8192B",
             unsigned(sizeof(Impl::Jobs)), unsigned(std::max(dbgHeapFree(), 0)));
    ok = false;
  }
#endif
  if (ok) for (auto &job : s.jobStorage) {
    job.reset(new (std::nothrow) Impl::Job);
    if (!job) {
      snprintf(error, errorSize, "Lua job buffer %uB unavailable", unsigned(sizeof(Impl::Job)));
      ok = false; break;
    }
  }
#endif
  s.loader.source = nullptr; s.loader.size = 0;
  if (!ok) clear();
  return ok;
}
const BotManifestView &BotSession::manifest() const {
#if ONCHIP_BOT_WASM
  if (wasm_) return wasm_->manifest();
#endif
  static const BotManifestView empty{nullptr, 0};
  return impl_ ? *impl_->loader.manifest : empty;
}
void BotSession::setHelp(const BotManifest &installed) {
#if ONCHIP_BOT_SEPARATE_DIAGNOSTICS
  if (impl_) { impl_->help = installed; impl_->helpSet = true; }
#else
  (void)installed;
#endif
}
void BotSession::setGeneration(uint32_t generation) {
#if ONCHIP_BOT_WASM
  if (wasm_) wasm_->setGeneration(generation);
#endif
  if (impl_) impl_->generation = generation;
}
uint8_t BotSession::subscriptions() const { return manifest().eventMask; }
void BotSession::setEventEpoch(std::atomic<uint32_t> *epoch) {
#if ONCHIP_BOT_WASM
  if (wasm_) wasm_->setEventEpoch(epoch);
#endif
  if (impl_) impl_->loader.eventEpoch = epoch;
}
void BotSession::cancelEvents() {
#if ONCHIP_BOT_WASM
  if (wasm_) wasm_->cancelEvents();
#endif
  if (impl_) for (unsigned i = 0; i < BotJobLimit; ++i) {
    auto &job = impl_->job(i);
    if (job.state != Impl::Job::Free && job.state != Impl::Job::Done && job.event.kind != BotEvent::Command)
      impl_->fail(job, "Event subscription/grant/source changed; admitted effects may have committed");
  }
}
bool BotSession::start(uint32_t id, const BotEvent &event, char *error, size_t errorSize) {
#if ONCHIP_BOT_WASM
  if (wasm_) {
    if (!validEvent(event)) { snprintf(error, errorSize, "Invalid bounded Wasm event"); return false; }
    return wasm_->start(id, event, error, errorSize);
  }
#endif
  if (!impl_ || !validEvent(event)) {
    snprintf(error, errorSize, "Retained VM unavailable or invalid event"); return false;
  }
  Impl::Job *job = nullptr;
  for (unsigned i = 0; i < BotJobLimit; ++i) {
    auto &candidate = impl_->job(i);
    if (candidate.state != Impl::Job::Free && candidate.result.job == id) {
      snprintf(error, errorSize, "Duplicate invocation ID"); return false;
    }
    if (candidate.state == Impl::Job::Free && !job) job = &candidate;
  }
  if (!job) { snprintf(error, errorSize, "Retained invocation capacity exhausted"); return false; }
  job->event = event; job->result.job = id;
  job->budget = Budget{impl_->heap.limits};
  job->budget.phase = Budget::Invoke;
  job->call = {nullptr, 0, &job->event, &job->result.action};
  job->call.budget = &job->budget; job->call.initializing = false;
  job->call.retained = true; job->call.io = &job->request;
  job->call.subscriptionOwner = &impl_->loader;
  job->call.originalReference = impl_->loader.originalReference;
  job->call.manifest = &impl_->natives;
#if ONCHIP_BOT_SEPARATE_DIAGNOSTICS
  job->call.installed = impl_->helpSet ? &impl_->help : impl_->loader.manifest;
#else
  job->call.installed = impl_->loader.manifest;
#endif
  job->call.generation = impl_->generation; job->call.job = id;
  impl_->preparing = job;
  impl_->heap.active = &job->budget; impl_->heap.running = true;
  lua_pushcfunction(impl_->state, Impl::prepare); lua_pushlightuserdata(impl_->state, impl_);
  const int result = lua_pcall(impl_->state, 1, 0, 0);
  job->budget.transition(Budget::Invoke);
  if (result != LUA_OK || job->budget.timedOut) {
    impl_->fail(*job, job->budget.timedOut ? "Invocation deadline exceeded during coroutine preparation" :
                     result != LUA_OK && lua_type(impl_->state, -1) == LUA_TSTRING ?
                         lua_tostring(impl_->state, -1) : "Coroutine allocation/time budget exceeded");
    if (result != LUA_OK) lua_pop(impl_->state, 1);
    job->result.stats = job->budget.stats;
    job->result.stats.peakBytes = impl_->heap.stats.peakBytes;
    impl_->heap.active = nullptr; impl_->heap.running = false;
  } else impl_->resume(*job, lua_gettop(job->thread) - 1);
  return true;
}
bool BotSession::hasPendingIo() const {
#if ONCHIP_BOT_WASM
  if (wasm_) return wasm_->hasPendingIo();
#endif
  if (impl_) for (unsigned i = 0; i < BotJobLimit; ++i) {
    const auto &job = impl_->job(i);
    if (job.state == Impl::Job::Waiting && !job.dispatched) return true;
  }
  return false;
}
bool BotSession::nextIo(BotIoRequest &request) {
#if ONCHIP_BOT_WASM
  if (wasm_) return wasm_->nextIo(request);
#endif
  if (impl_) for (unsigned i = 0; i < BotJobLimit; ++i) {
    auto &job = impl_->job(i);
    if (job.state == Impl::Job::Waiting && !job.dispatched) {
      request = job.request; job.dispatched = true; return true;
    }
  }
  return false;
}
bool BotSession::complete(const BotIoResult &result) {
#if ONCHIP_BOT_WASM
  if (wasm_) return wasm_->complete(result);
#endif
  if (!memchr(result.value, 0, sizeof(result.value)) ||
      !memchr(result.json, 0, sizeof(result.json)) ||
      !memchr(result.error, 0, sizeof(result.error)) || result.trace.count > BotTraceHopLimit ||
      (result.packet.path.known && !result.packet.path.valid()) ||
      !std::isfinite(result.packet.rssi) || !std::isfinite(result.packet.snr) ||
      !memchr(result.packet.nickname, 0, sizeof(result.packet.nickname)) ||
      !memchr(result.rpcCode, 0, sizeof(result.rpcCode)) ||
      !memchr(result.country, 0, sizeof(result.country)) ||
      !memchr(result.source, 0, sizeof(result.source)) ||
      !memchr(result.observedAt, 0, sizeof(result.observedAt)) ||
      !std::isfinite(result.temperatureC) ||
      result.keys.count > BotKeysPerScope) return false;
  for (unsigned i = 0; i < result.keys.count; ++i)
    if (!result.keys.keys[i][0] || !memchr(result.keys.keys[i], 0, sizeof(result.keys.keys[i])) ||
        (i && strcmp(result.keys.keys[i - 1], result.keys.keys[i]) >= 0)) return false;
  if (impl_) for (unsigned index = 0; index < BotJobLimit; ++index) {
    auto &job = impl_->job(index);
    if (job.state == Impl::Job::Waiting && job.dispatched && job.request.token == result.token) {
      if (job.event.kind != BotEvent::Command && impl_->loader.eventEpoch &&
          job.event.eventEpoch != impl_->loader.eventEpoch->load()) {
        impl_->fail(job, "Event epoch revoked before resumption; admitted effects may have committed");
        return true;
      }
      if (result.ok && job.request.kind == BotIoRequest::List)
        for (unsigned i = 0; i < result.keys.count; ++i)
          if (strncmp(result.keys.keys[i], job.request.key, strlen(job.request.key))) return false;
      job.completion = result; job.call.completion = &job.completion;
      impl_->resume(job, 0); return true;
    }
  }
  return false;
}
bool BotSession::poll(Result &result) {
#if ONCHIP_BOT_WASM
  if (wasm_) return wasm_->poll(result);
#endif
  if (impl_) for (unsigned i = 0; i < BotJobLimit; ++i) {
    auto &job = impl_->job(i);
    if (job.state == Impl::Job::Done) {
      result = job.result; impl_->release(job); return true;
    }
  }
  return false;
}
void BotSession::cancel(uint32_t except) {
#if ONCHIP_BOT_WASM
  if (wasm_) wasm_->cancel(except);
#endif
  if (impl_) for (unsigned i = 0; i < BotJobLimit; ++i) {
    auto &job = impl_->job(i);
    if (job.state != Impl::Job::Free && job.result.job != except)
      impl_->fail(job, "Source cancelled; pending operation outcome may be unknown");
  }
}
} // namespace onchip
#endif
