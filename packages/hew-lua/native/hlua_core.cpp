// SPDX-License-Identifier: Apache-2.0
#include "hlua_core.hpp"
static_assert(LUA_VERSION_RELEASE_NUM == 50501, "Lua 5.5.1 is required");
static_assert(sizeof(lua_Integer) == 8 && sizeof(lua_Number) == 8, "Lua scalars must be 64-bit");
namespace hlua {
Heap &heap(lua_State *state) {
  void *owner = nullptr;
  lua_getallocf(state, &owner);
  return *static_cast<Heap *>(owner);
}
lua_State *newState(lua_Alloc allocator, Heap &owner) {
  return lua_newstate(allocator, &owner, luaL_makeseed(nullptr));
}
void instructionHook(lua_State *state, lua_Debug *) {
  auto &owner = heap(state);
  if (!owner.instructionStep) luaL_error(state, "Lua instruction execution context unavailable");
  owner.instructionStep(state, owner);
}
}
extern "C" void hlua_parser_step(lua_State *state) {
  auto &owner = hlua::heap(state);
  if (!owner.parserStep) luaL_error(state, "Lua compiler execution context unavailable");
  owner.parserStep(state, owner);
}
