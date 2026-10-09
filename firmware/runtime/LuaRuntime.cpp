// SPDX-License-Identifier: Apache-2.0
#if defined(MESHCORE_ONCHIP_BOT) && MESHCORE_ONCHIP_BOT
extern "C" {
#include <lua.h>
#include <lauxlib.h>
}
#ifdef NRF52_PLATFORM
#include <Arduino.h>
#endif
#include "VmHeap.h"

// The pinned lexer calls this for every consumed character; instruction hooks
// do not run while compiling source.
extern "C" void onchip_lua_parser_step(lua_State *state) {
  void *owner = nullptr;
  lua_getallocf(state, &owner);
  auto *heap = static_cast<onchip::VmHeap *>(owner);
  if (!heap || !heap->parserStep) {
    luaL_error(state, "Lua compiler execution context unavailable"); return;
  }
  heap->parserStep(state, *heap);
}
#endif
