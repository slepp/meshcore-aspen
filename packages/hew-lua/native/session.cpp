// SPDX-License-Identifier: Apache-2.0
#include "session.h"
#include "hlua_core.hpp"
extern "C" {
#include "lualib.h"
}
#include <array>
#include <atomic>
#include <limits>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
namespace {
std::atomic<uint64_t> nextRequest{1};
uint64_t requestToken() {
  auto id=nextRequest.load(std::memory_order_relaxed);
  while(id && id<std::numeric_limits<uint64_t>::max())
    if(nextRequest.compare_exchange_weak(id,id+1,std::memory_order_relaxed)) return id;
  return 0; // Exhaustion never reuses a token, including across sessions.
}
uint64_t nowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}
struct OwnedValue { int kind=HLUA_NIL; int64_t integer=0; double number=0; std::string bytes; };
}
struct HluaSession : hlua::Heap {
  HluaLimits limits;
  lua_State *state=nullptr, *thread=nullptr;
  int threadRef=LUA_NOREF, status=HLUA_READY;
  bool loaded=false, loadAttempted=false, running=false, loading=false, terminal=false;
  size_t peak=0;
  uint64_t instructions=0, parserSteps=0, usedUs=0, started=0, token=0;
  size_t count=0, argumentCount=0;
  std::array<OwnedValue,8> arguments;
  std::string function;
  char error[256]{};
  uint32_t hostCalls=0;
  explicit HluaSession(HluaLimits selected):limits(selected) {
    parserStep=parse; instructionStep=step;
  }
  static HluaSession &self(lua_State *L) { return static_cast<HluaSession &>(hlua::heap(L)); }
  bool expired() const { return terminal || (running && nowUs()-started+usedUs >= limits.active_us); }
  void fail(const char *message) {
    std::snprintf(error,sizeof(error),"%s",message); status=HLUA_ERROR;
  }
  static int budgetError(lua_State *L, const char *message) {
    auto &s=self(L); s.terminal=true; s.fail(message); return luaL_error(L,"%s",message);
  }
  static void parse(lua_State *L, hlua::Heap &h) {
    auto &s=static_cast<HluaSession &>(h);
    if (!hlua::charge(s.parserSteps,s.limits.parser_steps) || s.expired())
      budgetError(L,"Lua parser/time budget exceeded");
  }
  static void step(lua_State *L, hlua::Heap &h) {
    auto &s=static_cast<HluaSession &>(h);
    if (!hlua::charge(s.instructions,s.limits.instructions) || s.expired())
      budgetError(L,"Lua instruction/time budget exceeded");
  }
  static void *allocate(void *context, void *pointer, size_t oldSize, size_t size) {
    auto &s=*static_cast<HluaSession *>(static_cast<hlua::Heap *>(context));
    if (size && s.expired()) { s.terminal=true; return nullptr; }
    return s.resize(pointer,oldSize,size,size_t(s.limits.heap_bytes),s.peak);
  }
  bool scalar(lua_State *L,int index) const {
    const int type=lua_type(L,index);
    return type==LUA_TNIL || type==LUA_TBOOLEAN || type==LUA_TNUMBER ||
      (type==LUA_TSTRING && lua_rawlen(L,index)<=limits.value_bytes);
  }
  static void push(lua_State *L, const OwnedValue &v) {
    switch(v.kind) {
    case HLUA_NIL: lua_pushnil(L); break;
    case HLUA_BOOL: lua_pushboolean(L,v.integer!=0); break;
    case HLUA_INTEGER: lua_pushinteger(L,v.integer); break;
    case HLUA_NUMBER: lua_pushnumber(L,v.number); break;
    case HLUA_BYTES: lua_pushlstring(L,v.bytes.data(),v.bytes.size()); break;
    }
  }
  static int completion(lua_State *L,int,lua_KContext) {
    auto &s=self(L);
    for(size_t i=0;i<s.argumentCount;++i) push(L,s.arguments[i]);
    return int(s.argumentCount);
  }
  static int host(lua_State *L) {
    auto &s=self(L);
    if(s.loading) return luaL_error(L,"host operations require an active function call");
    const int n=lua_gettop(L);
    if(n<1 || n>9 || lua_type(L,1)!=LUA_TSTRING || lua_rawlen(L,1)==0 || lua_rawlen(L,1)>64)
      return luaL_error(L,"host expects an operation name (1..64 bytes) and up to eight scalars");
    for(int i=2;i<=n;++i) if(!s.scalar(L,i))
      return luaL_error(L,"host argument must be a bounded scalar");
    if(!hlua::charge(s.hostCalls,s.limits.host_calls)) return budgetError(L,"Lua host-call budget exceeded");
    s.token=requestToken();
    if(!s.token) return budgetError(L,"Lua host request token space exhausted");
    return lua_yieldk(L,n,0,completion);
  }
  static int initialise(lua_State *L) {
    // Base conveniences only: no loaders, protected calls or coroutine access.
    luaL_requiref(L,"_G",luaopen_base,1); lua_pop(L,1);
    const char *removed[]={"getmetatable","setmetatable","collectgarbage","dofile","load","loadfile","pcall","xpcall","print","warn",nullptr};
    for(const char **name=removed;*name;++name) {lua_pushnil(L);lua_setglobal(L,*name);}
    luaL_requiref(L,LUA_MATHLIBNAME,luaopen_math,1);lua_pop(L,1);
    luaL_requiref(L,LUA_STRLIBNAME,luaopen_string,1);lua_pop(L,1);
    luaL_requiref(L,LUA_TABLIBNAME,luaopen_table,1);lua_pop(L,1);
    lua_pushcfunction(L,host);lua_setglobal(L,"host");
    return 0;
  }
  static int prepareCall(lua_State *L) {
    auto &s=self(L);
    s.thread=lua_newthread(L);
    s.threadRef=luaL_ref(L,LUA_REGISTRYINDEX);
    lua_sethook(s.thread,hlua::instructionHook,LUA_MASKCOUNT,1);
    lua_pushcfunction(s.thread,invoke);
    return 0;
  }
  static int invoke(lua_State *L) {
    auto &s=self(L);
    lua_getglobal(L,s.function.c_str());
    if(!lua_isfunction(L,-1)) return luaL_error(L,"Lua function does not exist");
    for(size_t i=0;i<s.argumentCount;++i)push(L,s.arguments[i]);
    lua_callk(L,int(s.argumentCount),LUA_MULTRET,0,finished);
    return finished(L,LUA_OK,0);
  }
  static int finished(lua_State *L,int,lua_KContext) { return lua_gettop(L); }
  void dropThread() {
    if(threadRef!=LUA_NOREF) luaL_unref(state,LUA_REGISTRYINDEX,threadRef);
    threadRef=LUA_NOREF;thread=nullptr;count=0;
  }
  void resetBudget() {
    instructions=parserSteps=usedUs=0;hostCalls=0;terminal=false;error[0]=0;
    allocationFailure=NoAllocationFailure;
  }
  void begin() {started=nowUs();running=true;}
  void end() {usedUs+=nowUs()-started;running=false;}
  void luaFailure(lua_State *L) {
    if(terminal && error[0]) {status=HLUA_ERROR;return;}
    if(allocationFailure==HeapLimit) fail("Lua heap limit exceeded");
    else if(allocationFailure==AllocatorFailure) fail("Lua allocation failed");
    else if(terminal) fail("Lua time budget exceeded");
    else {const char *message=lua_type(L,-1)==LUA_TSTRING ? lua_tostring(L,-1) : "Lua execution failed"; fail(message);}
  }
  int run() {
    int results=0;
    begin();const int result=lua_resume(thread,nullptr,0,&results);end();
    argumentCount=0;
    if(result!=LUA_OK && result!=LUA_YIELD) {luaFailure(thread);dropThread();return status;}
    if(expired() || usedUs>=limits.active_us) {fail("Lua time budget exceeded");dropThread();return status;}
    if(result==LUA_YIELD) {status=HLUA_HOST;count=size_t(results-1);}
    else {
      if(results>8) {fail("Lua returned more than eight values");dropThread();return status;}
      for(int i=1;i<=results;++i)if(!scalar(thread,i)) {
        fail("Lua result must be a bounded scalar");dropThread();return status;
      }
      status=HLUA_COMPLETE;count=size_t(results);
    }
    return status;
  }
};
extern "C" {
HluaSession *hlua_open(HluaLimits limits) {
  // Bound host-side copying separately from the Lua heap.
  if(limits.heap_bytes<16384 || limits.heap_bytes>64u*1024u*1024u || !limits.instructions ||
     !limits.parser_steps || !limits.active_us || !limits.source_bytes || limits.source_bytes>1024u*1024u ||
     !limits.value_bytes || limits.value_bytes>65536 || !limits.host_calls) return nullptr;
  auto *s=new(std::nothrow) HluaSession(limits);
  if(!s) return nullptr;
  s->state=hlua::newState(HluaSession::allocate,*s);
  if(!s->state) {delete s;return nullptr;}
  lua_pushcfunction(s->state,HluaSession::initialise);
  if(lua_pcall(s->state,0,0,0)!=LUA_OK) {hlua_close(s);return nullptr;}
  lua_sethook(s->state,hlua::instructionHook,LUA_MASKCOUNT,1);
  return s;
}
void hlua_close(HluaSession *s) {if(s){s->running=false;lua_close(s->state);delete s;}}
int hlua_status(const HluaSession *s){return s?s->status:HLUA_ERROR;}
int hlua_load(HluaSession *s,const uint8_t *source,size_t size) {
  if(!s)return HLUA_ERROR;
  if(s->loadAttempted) {std::snprintf(s->error,sizeof(s->error),"Lua source load was already attempted; open a new session");return HLUA_ERROR;}
  s->loadAttempted=true;
  if(size>s->limits.source_bytes || (size&&!source)){s->fail("Lua source exceeds its byte limit");return s->status;}
  s->resetBudget();s->loading=true;s->begin();
  int result=luaL_loadbufferx(s->state,reinterpret_cast<const char *>(source),size,"source","t");
  if(result==LUA_OK)result=lua_pcall(s->state,0,0,0);
  s->end();s->loading=false;
  if(result!=LUA_OK){s->luaFailure(s->state);lua_settop(s->state,0);return s->status;}
  if(s->usedUs>=s->limits.active_us){s->fail("Lua time budget exceeded");return s->status;}
  s->loaded=true;return s->status=HLUA_READY;
}
int hlua_prepare_arguments(HluaSession *s,uint64_t token,int resume) {
  if(!s)return 0;
  if(!s->loaded || (resume ? s->status!=HLUA_HOST || !token || s->token!=token : s->status==HLUA_HOST)) {
    std::snprintf(s->error,sizeof(s->error),"Lua request is not ready or its continuation token is stale");return 0;
  }
  s->argumentCount=0;return 1;
}
int hlua_argument(HluaSession *s,HluaValue v) {
  if(!s)return 0;
  if(s->argumentCount>=8 || v.kind<HLUA_NIL || v.kind>HLUA_BYTES ||
      (v.kind==HLUA_BYTES && (v.size>s->limits.value_bytes || (v.size&&!v.bytes)))) {
    std::snprintf(s->error,sizeof(s->error),"Lua argument must be one of at most eight bounded scalars");return 0;
  }
  auto &a=s->arguments[s->argumentCount];
  try {a.bytes.assign(v.kind==HLUA_BYTES && v.size?reinterpret_cast<const char *>(v.bytes):"",v.kind==HLUA_BYTES?v.size:0);}
  catch(const std::bad_alloc &){std::snprintf(s->error,sizeof(s->error),"Lua argument copy allocation failed");return 0;}
  a.kind=v.kind;a.integer=v.integer;a.number=v.number;++s->argumentCount;return 1;
}
int hlua_call(HluaSession *s,const uint8_t *name,size_t size) {
  if(!s)return HLUA_ERROR;
  if(!s->loaded || s->status==HLUA_HOST || !size || size>128 || !name || std::memchr(name,0,size)) {
    s->fail("Lua call requires loaded source, no pending request and a function name (1..128 bytes)");return s->status;
  }
  try{s->function.assign(reinterpret_cast<const char *>(name),size);}
  catch(const std::bad_alloc &){s->fail("Lua function name allocation failed");return s->status;}
  s->dropThread();s->resetBudget();s->begin();
  lua_pushcfunction(s->state,HluaSession::prepareCall);
  const int result=lua_pcall(s->state,0,0,0);s->end();
  if(result!=LUA_OK){s->luaFailure(s->state);lua_settop(s->state,0);s->dropThread();return s->status;}
  return s->run();
}
int hlua_resume(HluaSession *s,uint64_t token) {
  if(!s)return HLUA_ERROR;
  if(s->status!=HLUA_HOST || token!=s->token || !token) {std::snprintf(s->error,sizeof(s->error),"Lua host request is no longer pending");return HLUA_ERROR;}
  // The continuation pushes response arguments inside lua_resume's protection.
  lua_settop(s->thread,0);return s->run();
}
void hlua_cancel(HluaSession *s) {if(s){s->dropThread();s->argumentCount=0;s->token=0;s->status=HLUA_READY;}}
uint64_t hlua_token(const HluaSession *s){return s&&s->status==HLUA_HOST?s->token:0;}
size_t hlua_count(const HluaSession *s){return s?s->count:0;}
HluaValue hlua_value(const HluaSession *s,size_t index) {
  HluaValue v{};if(!s||!s->thread||index>=s->count)return v;
  const int i=int(index)+(s->status==HLUA_HOST?2:1);
  switch(lua_type(s->thread,i)) {
  case LUA_TBOOLEAN:v.kind=HLUA_BOOL;v.integer=lua_toboolean(s->thread,i);break;
  case LUA_TNUMBER:if(lua_isinteger(s->thread,i)){v.kind=HLUA_INTEGER;v.integer=lua_tointeger(s->thread,i);}
    else{v.kind=HLUA_NUMBER;v.number=lua_tonumber(s->thread,i);}break;
  case LUA_TSTRING:v.kind=HLUA_BYTES;v.bytes=reinterpret_cast<const uint8_t *>(lua_tolstring(s->thread,i,&v.size));break;
  }
  return v;
}
const uint8_t *hlua_operation(const HluaSession *s,size_t *size) {
  *size=0;return s&&s->status==HLUA_HOST?reinterpret_cast<const uint8_t *>(lua_tolstring(s->thread,1,size)):nullptr;
}
const char *hlua_error(const HluaSession *s){return s?s->error:"Lua session allocation failed";}
size_t hlua_peak(const HluaSession *s){return s?s->peak:0;}
uint64_t hlua_instructions(const HluaSession *s){return s?s->instructions:0;}
uint64_t hlua_parser_steps(const HluaSession *s){return s?s->parserSteps:0;}
}
