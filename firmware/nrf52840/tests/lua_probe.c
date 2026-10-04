#include <lua.h>
#include <lauxlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(LUA_VERSION_RELEASE_NUM == 50501, "probe requires Lua 5.5.1");

struct Budget { size_t current, peak; };

static void* allocate(void* context, void* ptr, size_t old, size_t size) {
  struct Budget* budget = context;
  if (!ptr) old = 0;
  if (!size) {
    free(ptr);
    budget->current -= old;
    return NULL;
  }
  if (size > 32768 || budget->current - old > 32768 - size) return NULL;
  void* result = realloc(ptr, size);
  if (result) {
    budget->current = budget->current - old + size;
    if (budget->current > budget->peak) budget->peak = budget->current;
  }
  return result;
}

void pineLuaProbe(void) {
  struct Budget budget = {0, 0};
  lua_State* state = lua_newstate(allocate, &budget, 0);
  const char* source = "local t={}; for i=1,32 do t[i]=i*i end; return t[32]";
  int result = state ? luaL_loadbufferx(state, source, strlen(source), "bounded-probe", "t") : LUA_ERRMEM;
  if (state && result == LUA_OK) result = lua_pcall(state, 0, 1, 0);
  const int value = state && result == LUA_OK ? (int)lua_tointeger(state, -1) : 0;
  if (state) lua_close(state);
  printf("Lua core probe version=5.5.1 pointer_bytes=%u limit=32768 peak=%u retained=%u status=%d value=%d\n",
         (unsigned)sizeof(void*), (unsigned)budget.peak, (unsigned)budget.current, result, value);
  if (result != LUA_OK || value != 1024 || budget.current) abort();
}

#ifndef NRFMAST_LUA_PROBE
int main(void) { pineLuaProbe(); return 0; }
#endif
