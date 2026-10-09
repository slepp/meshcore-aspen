// SPDX-License-Identifier: Apache-2.0
#if defined(MESHCORE_ONCHIP_BOT) && MESHCORE_ONCHIP_BOT
#include "PacketLua.h"
extern "C" {
#include <lua.h>
#include <lauxlib.h>
}
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <new>
#ifdef ARDUINO_ARCH_ESP32
#include <esp_timer.h>
#elif defined(NRF52_PLATFORM)
#include <Arduino.h>
#endif
#include "VmHeap.h"
#ifdef ONCHIP_BOT_VM_TEST
extern uint64_t onchipBotVmTestClock();
#endif

namespace onchip {
using namespace packet_engine;
namespace {
uint64_t nowUs() {
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
}
static_assert(LUA_VERSION_RELEASE_NUM == 50501, "Packet VM requires Lua 5.5.1");
struct PacketLua::Impl : VmHeap {
  PacketLuaStats &stats;
  lua_State *state = nullptr;
  int function = LUA_NOREF;
  const char *source = nullptr;
  size_t size = 0;
  uint32_t initFuel = 0;
  uint64_t deadline = 0;
  bool running = true;
  const Metadata *metadata = nullptr;
  Call *call = nullptr;
  explicit Impl(PacketLuaStats &s) : stats(s) { parserStep = compileStep; }
  static void compileStep(lua_State *state, VmHeap &heap) {
    auto &s = static_cast<Impl &>(heap);
    if (++s.stats.parserSteps > PacketLuaSourceLimit * 4 || s.expired())
      luaL_error(state, "Packet Lua compile/time budget exceeded");
  }
  static Impl &self(lua_State *state) { return **static_cast<Impl **>(lua_getextraspace(state)); }
  bool expired() { return call ? !call->consume(0) : nowUs() >= deadline; }
  static void *allocate(void *owner, void *pointer, size_t oldSize, size_t size) {
    auto &s = *static_cast<Impl *>(static_cast<VmHeap *>(owner));
    if (size && s.running && s.expired()) return nullptr;
    return s.resize(pointer, oldSize, size, s.stats.heapLimit, s.stats.peakBytes);
  }
  static void hook(lua_State *state, lua_Debug *) {
    auto &s = self(state);
    ++s.stats.instructions;
    if (s.call ? !s.call->consume() : s.stats.instructions > s.initFuel || s.expired())
      luaL_error(state, "Packet Lua instruction/time budget exceeded");
  }
  int fail(lua_State *state, const char *message, Fault fault = Fault::Execution) {
    if (call) call->fail(fault);
    return luaL_error(state, "%s", message);
  }
  void native(lua_State *state) {
    if (!call || !metadata) { fail(state, "Packet import requires an active packet"); return; }
    if (++stats.nativeCalls > 64) { fail(state, "Packet native-call budget exceeded (64 calls)", Fault::Fuel); return; }
    if (!call->consume()) fail(state, "Packet Lua instruction/time budget exceeded");
  }
  uint32_t integer(lua_State *state, int index, uint32_t maximum, uint32_t fallback = 0, bool optional = false) {
    if (optional && lua_isnoneornil(state, index)) return fallback;
    if (!lua_isinteger(state, index) || lua_tointeger(state, index) < 0 ||
        uint64_t(lua_tointeger(state, index)) > maximum) {
      fail(state, "Packet integer argument is outside its allowed range", Fault::Bounds); return 0;
    }
    return uint32_t(lua_tointeger(state, index));
  }
  const uint8_t *bytes(lua_State *state, int index, size_t &length) {
    luaL_checktype(state, index, LUA_TSTRING);
    const char *bytes = lua_tolstring(state, index, &length);
    if (length > Capacity) fail(state, "Packet string exceeds 255 bytes", Fault::Bounds);
    return reinterpret_cast<const uint8_t *>(bytes);
  }
  static void field(lua_State *state, const char *name, lua_Integer value) {
    lua_pushinteger(state, value); lua_setfield(state, -2, name);
  }
  static void flag(lua_State *state, const char *name, bool value) {
    lua_pushboolean(state, value); lua_setfield(state, -2, name);
  }
  static int info(lua_State *state) {
    auto &s = self(state); s.native(state);
    const auto &m = *s.metadata;
    lua_createtable(state, 0, 19);
    field(state, "version", 1); field(state, "stage", unsigned(m.stage));
    field(state, "length", s.call->size()); field(state, "capacity", s.call->capacity());
    field(state, "source", m.source); field(state, "destination", m.destination);
    field(state, "payload_type", m.payloadType); field(state, "generation", m.generation); field(state, "job", m.job);
    field(state, "rssi", m.rssi); field(state, "snr_quarter_db", m.snrQuarterDb);
    field(state, "flags", (m.local ? 1 : 0) | (m.authenticated ? 2 : 0) |
        (m.engineOrigin ? 4 : 0) | (m.reflectionOrigin ? 8 : 0));
    flag(state, "local", m.local); flag(state, "authenticated", m.authenticated);
    flag(state, "engine_origin", m.engineOrigin); flag(state, "reflection_origin", m.reflectionOrigin);
    lua_pushlstring(state, reinterpret_cast<const char *>(m.identity), sizeof(m.identity));
    lua_setfield(state, -2, "identity");
    return 1;
  }
  static int read(lua_State *state) {
    auto &s = self(state); s.native(state);
    const auto offset = s.integer(state, 1, Capacity);
    const auto length = s.integer(state, 2, Capacity);
    uint8_t bytes[Capacity];
    if (!s.call->read(uint16_t(offset), bytes, uint16_t(length)))
      return s.fail(state, "Packet read range is outside the current packet");
    lua_pushlstring(state, reinterpret_cast<const char *>(bytes), length);
    return 1;
  }
  static int write(lua_State *state) {
    auto &s = self(state); s.native(state);
    const auto offset = s.integer(state, 1, Capacity);
    size_t length; const auto *bytes = s.bytes(state, 2, length);
    if (!s.call->write(uint16_t(offset), bytes, uint16_t(length)))
      return s.fail(state, "Packet write range is outside the current packet");
    lua_pushinteger(state, length); return 1;
  }
  static int replace(lua_State *state) {
    auto &s = self(state); s.native(state);
    size_t length; const auto *bytes = s.bytes(state, 1, length);
    if (!s.call->replace(bytes, uint16_t(length)))
      return s.fail(state, "Packet replacement length is invalid");
    lua_pushinteger(state, length); return 1;
  }
  static int emit(lua_State *state) {
    auto &s = self(state); s.native(state);
    size_t length; const auto *bytes = s.bytes(state, 1, length);
    const auto priority = s.integer(state, 2, UINT8_MAX, 4, true);
    const auto delay = s.integer(state, 3, MaxDelayMs, 0, true);
    const auto expiry = s.integer(state, 4, MaxDelayMs, 0, true);
    if (!s.call->emit(bytes, uint16_t(length), uint8_t(priority), delay, expiry))
      return s.fail(state, "Packet emission failed");
    lua_pushinteger(state, length); return 1;
  }
  static int assertValue(lua_State *state) {
    if (!lua_toboolean(state, 1)) return luaL_error(state, "Packet assertion failed");
    return lua_gettop(state);
  }
  static int initialize(lua_State *state) {
    auto &s = self(state);
    luaL_checkversion(state);
    lua_newtable(state);
    for (const auto &function : {luaL_Reg{"info", info}, {"read", read}, {"write", write},
                                {"replace", replace}, {"emit", emit}}) {
      lua_pushcfunction(state, function.func); lua_setfield(state, -2, function.name);
    }
    field(state, "CONTINUE", 0); field(state, "DROP", 1); field(state, "ABI_VERSION", 1);
    lua_setglobal(state, "packet");
    lua_pushcfunction(state, assertValue); lua_setglobal(state, "assert");
    if (luaL_loadbufferx(state, s.source, s.size, "packet", "t") != LUA_OK) return lua_error(state);
    lua_call(state, 0, 1);
    if (!lua_isfunction(state, -1) || lua_iscfunction(state, -1))
      return luaL_error(state, "Packet Lua source must return a process function");
    s.function = luaL_ref(state, LUA_REGISTRYINDEX);
    return 0;
  }
  static int invoke(lua_State *state) {
    auto &s = self(state);
    lua_rawgeti(state, LUA_REGISTRYINDEX, s.function);
    lua_call(state, 0, 1);
    return 1;
  }
};
PacketLua::~PacketLua() { clear(); }
void PacketLua::clear() {
  if (!impl_) return;
  impl_->running = false;
  if (impl_->state) lua_close(impl_->state);
  stats_.liveBytes = uint32_t(impl_->live);
  impl_->~Impl();
#ifdef ARDUINO_ARCH_ESP32
  heap_caps_free(impl_);
#else
  free(impl_);
#endif
  impl_ = nullptr;
}
bool PacketLua::load(const char *source, size_t size, char *error, size_t capacity,
                    uint32_t heapBytes, uint32_t fuel, uint32_t initUs) {
  clear(); stats_ = {};
  const auto failed = [&](const char *message) {
    snprintf(error, capacity, "%s", message);
    snprintf(stats_.error, sizeof(stats_.error), "%s", message);
    clear(); return false;
  };
  if (!source || !size || size > PacketLuaSourceLimit || memchr(source, 0, size) ||
      static_cast<unsigned char>(source[0]) == 27 || heapBytes < 8192 || heapBytes > 262144 ||
      !fuel || fuel > 100000 || !initUs || initUs > 20000)
    return failed("Invalid packet Lua text, heap limit or initialization budget");
#ifdef ARDUINO_ARCH_ESP32
  void *storage = heap_caps_calloc(1, sizeof(Impl), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
  void *storage = calloc(1, sizeof(Impl));
#endif
  if (!storage) return failed("Packet Lua session storage unavailable");
  impl_ = new (storage) Impl(stats_);
  auto &s = *impl_;
  stats_.sourceBytes = uint32_t(size); stats_.sessionBytes = sizeof(Impl); stats_.heapLimit = heapBytes;
  s.source = source; s.size = size; s.initFuel = fuel;
  const auto started = nowUs(); s.deadline = started + initUs;
  s.state = lua_newstate(Impl::allocate, static_cast<VmHeap *>(&s), luaL_makeseed(nullptr));
  if (!s.state) return failed("Packet Lua heap allocation/time budget exceeded");
  *static_cast<Impl **>(lua_getextraspace(s.state)) = &s;
  lua_sethook(s.state, Impl::hook, LUA_MASKCOUNT, 1);
  lua_pushcfunction(s.state, Impl::initialize);
  const int result = lua_pcall(s.state, 0, 0, 0);
  stats_.loadUs = nowUs() - started; stats_.liveBytes = uint32_t(s.live);
  s.source = nullptr; s.size = 0;
  if (result != LUA_OK || s.expired()) {
    snprintf(error, capacity, "%s", result != LUA_OK && lua_type(s.state, -1) == LUA_TSTRING ?
        lua_tostring(s.state, -1) : "Packet Lua initialization time budget exceeded");
    snprintf(stats_.error, sizeof(stats_.error), "%s", error); clear(); return false;
  }
  s.running = false;
  if (capacity) error[0] = 0;
  return true;
}
Decision PacketLua::process(const Metadata &metadata, Call &call) {
  if (!impl_) {
    snprintf(stats_.error, sizeof(stats_.error), "Packet Lua engine is not loaded"); return Decision::Failed;
  }
  auto &s = *impl_;
  if (s.call) { call.fail(Fault::Reentrant); return Decision::Failed; }
  s.call = &call; s.metadata = &metadata; s.running = true;
  s.allocationFailure = VmHeap::NoAllocationFailure;
  stats_.instructions = 0; stats_.nativeCalls = 0;
  const auto started = nowUs();
  lua_pushcfunction(s.state, Impl::invoke);
  const int result = lua_pcall(s.state, 0, 1, 0);
  stats_.invokeUs = nowUs() - started; stats_.liveBytes = uint32_t(s.live);
  s.running = false; s.call = nullptr; s.metadata = nullptr;
  const bool valid = result == LUA_OK && lua_isinteger(s.state, -1) &&
                     (lua_tointeger(s.state, -1) == 0 || lua_tointeger(s.state, -1) == 1);
  const Decision decision = valid && lua_tointeger(s.state, -1) == 1 ? Decision::Drop : Decision::Continue;
  if (!valid || s.allocationFailure != VmHeap::NoAllocationFailure) {
    snprintf(stats_.error, sizeof(stats_.error), "%s", s.allocationFailure == VmHeap::HeapLimit ?
        "Packet Lua heap limit exceeded" : s.allocationFailure == VmHeap::AllocatorFailure ?
        "Packet Lua allocator failed" : result != LUA_OK && lua_type(s.state, -1) == LUA_TSTRING ?
        lua_tostring(s.state, -1) : "Packet Lua disposition must be CONTINUE or DROP");
    if (result == LUA_OK && !valid) call.fail(Fault::InvalidDecision);
    lua_settop(s.state, 0);
    return Decision::Failed;
  }
  lua_settop(s.state, 0); stats_.error[0] = 0;
  return decision;
}
} // namespace onchip
#endif
