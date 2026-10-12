// SPDX-License-Identifier: Apache-2.0
#include "session.h"
#include "runtime.hpp"
#include <atomic>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
struct HwamrInstance {
  HwamrLimits limits;
  uint8_t *bytes=nullptr;
  wasm_module_t module=nullptr;
  wasm_module_inst_t instance=nullptr;
  wasm_exec_env_t env=nullptr;
  hwamr::Meter meter;
  std::atomic<bool> cancelled{false};
  hwamr::Context context{0,&meter,this};
  wasm_val_t arguments[8]{}, results[4]{};
  uint32_t argumentCount=0, resultCount=0;
  char error[256]{};
  explicit HwamrInstance(HwamrLimits selected):limits(selected) {meter.cancelled=&cancelled;}
  bool fail(const char *message) {std::snprintf(error,sizeof(error),"%s",message);return false;}
};
namespace {
thread_local char openError[256]{};
thread_local char configError[256]{};
bool range(HwamrInstance *s,uint64_t offset,size_t size) {
  const auto capacity=hwamr_memory_size(s);
  if(offset>capacity || size>capacity-offset || offset>UINT32_MAX || size>UINT32_MAX)
    return s->fail("Wasm memory range exceeds the instance memory");
  // A zero-length range at the end is valid and does not need a native pointer.
  return !size || wasm_runtime_validate_app_addr(s->instance,uint32_t(offset),uint32_t(size)) ||
    s->fail("Wasm memory range validation failed");
}
wasm_valkind_t kind(int value) {
  switch(value) {case HWAMR_I32:return WASM_I32;case HWAMR_I64:return WASM_I64;
    case HWAMR_F32:return WASM_F32;default:return WASM_F64;}
}
}
extern "C" {
int hwamr_pool(uint32_t bytes) {
  hwamr::Config config;config.poolBytes=bytes;
  configError[0]=0;return hwamr::configure(config,configError,sizeof(configError));
}
const char *hwamr_pool_error(void){return configError;}
HwamrInstance *hwamr_open(const uint8_t *bytes,size_t size,HwamrLimits limits) {
  hwamr::RuntimeLock lock;openError[0]=0;
  if(!bytes || !size || size>limits.source_bytes || limits.source_bytes>1048576 ||
      !limits.memory_pages || limits.memory_pages>256 || limits.stack_bytes<1024 || limits.stack_bytes>65536 ||
      !limits.instructions || limits.instructions>INT_MAX || !limits.active_us) {
    std::snprintf(openError,sizeof(openError),"Invalid Wasm source, memory, stack or execution limits");return nullptr;
  }
  if(!hwamr::profile(bytes,size,"",nullptr,0,openError,sizeof(openError),{limits.memory_pages,false}) ||
     !hwamr::initialise(openError,sizeof(openError)))return nullptr;
  auto *s=new(std::nothrow) HwamrInstance(limits);
  if(!s){std::snprintf(openError,sizeof(openError),"Wasm instance allocation failed");return nullptr;}
  s->bytes=static_cast<uint8_t *>(std::malloc(size));
  if(!s->bytes){std::snprintf(openError,sizeof(openError),"Wasm source copy allocation failed");hwamr_close(s);return nullptr;}
  std::memcpy(s->bytes,bytes,size);
  s->module=wasm_runtime_load(s->bytes,uint32_t(size),openError,sizeof(openError));
  if(s->module)s->instance=wasm_runtime_instantiate(s->module,limits.stack_bytes,0,openError,sizeof(openError));
  if(s->instance){
    wasm_runtime_set_custom_data(s->instance,&s->context);
    s->env=wasm_runtime_create_exec_env(s->instance,limits.stack_bytes);
  }
  if(!s->env){
    if(!openError[0])std::snprintf(openError,sizeof(openError),"Wasm execution environment allocation failed");
    hwamr_close(s);return nullptr;
  }
  return s;
}
void hwamr_close(HwamrInstance *s) {
  hwamr::RuntimeLock lock;
  if(!s)return;
  if(s->env)wasm_runtime_destroy_exec_env(s->env);
  if(s->instance)wasm_runtime_deinstantiate(s->instance);
  if(s->module)wasm_runtime_unload(s->module);
  std::free(s->bytes);delete s;
}
int hwamr_arguments(HwamrInstance *s) {
  if(!s)return 0;
  s->argumentCount=s->resultCount=0;s->error[0]=0;
  if(s->cancelled.load())return s->fail("Wasm instance cancelled");
  return 1;
}
int hwamr_argument(HwamrInstance *s,HwamrValue value) {
  if(!s)return 0;
  if(s->argumentCount==8 || value.kind<HWAMR_I32 || value.kind>HWAMR_F64 ||
      (value.kind==HWAMR_I32 && (value.integer<INT32_MIN || value.integer>INT32_MAX)))
    return s->fail("Wasm accepts at most eight typed numeric arguments");
  auto &v=s->arguments[s->argumentCount++];v.kind=kind(value.kind);
  switch(value.kind){case HWAMR_I32:v.of.i32=int32_t(value.integer);break;
    case HWAMR_I64:v.of.i64=value.integer;break;case HWAMR_F32:v.of.f32=float(value.number);break;
    case HWAMR_F64:v.of.f64=value.number;break;}
  return 1;
}
int hwamr_call(HwamrInstance *s,const uint8_t *name,size_t size) {
  hwamr::RuntimeLock lock;
  if(!s)return 0;
  s->resultCount=0;
  if(s->cancelled.load())return s->fail("Wasm instance cancelled");
  if(!name || !size || size>128 || std::memchr(name,0,size))return s->fail("Wasm export name must be 1..128 bytes without NUL");
  char exported[129];std::memcpy(exported,name,size);exported[size]=0;
  auto function=wasm_runtime_lookup_function(s->instance,exported);
  if(!function)return s->fail("Wasm function export does not exist");
  const auto argc=wasm_func_get_param_count(function,s->instance);
  const auto count=wasm_func_get_result_count(function,s->instance);
  if(argc!=s->argumentCount || argc>8 || count>4)return s->fail("Wasm export argument/result count does not match the numeric call");
  wasm_valkind_t types[8]{};wasm_func_get_param_types(function,s->instance,types);
  for(uint32_t i=0;i<argc;++i)if(types[i]!=s->arguments[i].kind)
    return s->fail("Wasm export argument type does not match");
  wasm_func_get_result_types(function,s->instance,types);
  for(uint32_t i=0;i<count;++i)if(types[i]!=WASM_I32 && types[i]!=WASM_I64 && types[i]!=WASM_F32 && types[i]!=WASM_F64)
    return s->fail("Wasm export result must be numeric");
  s->meter={};s->meter.cancelled=&s->cancelled;s->meter.fuel=s->limits.instructions;
  const uint64_t started=hwamr::nowUs();
  s->meter.deadline=UINT64_MAX-started<s->limits.active_us?UINT64_MAX:started+s->limits.active_us;
  wasm_runtime_set_instruction_count_limit(s->env,s->limits.instructions);
  wasm_runtime_clear_exception(s->instance);
  const bool ok=wasm_runtime_call_wasm_a(s->env,function,count,s->results,argc,s->arguments);
  s->argumentCount=0;
  if(!ok){const auto *message=wasm_runtime_get_exception(s->instance);return s->fail(message?message:"Wasm execution failed");}
  if(s->cancelled.load())return s->fail("Wasm instance cancelled");
  if(hwamr::nowUs()>=s->meter.deadline)return s->fail("Wasm active-time budget exceeded");
  s->resultCount=count;return 1;
}
size_t hwamr_count(const HwamrInstance *s){return s?s->resultCount:0;}
HwamrValue hwamr_value(const HwamrInstance *s,size_t index) {
  HwamrValue out{};if(!s||index>=s->resultCount)return out;
  const auto &v=s->results[index];
  switch(v.kind){case WASM_I32:out.kind=HWAMR_I32;out.integer=v.of.i32;break;
    case WASM_I64:out.kind=HWAMR_I64;out.integer=v.of.i64;break;
    case WASM_F32:out.kind=HWAMR_F32;out.number=v.of.f32;break;
    case WASM_F64:out.kind=HWAMR_F64;out.number=v.of.f64;break;default:break;}
  return out;
}
int hwamr_readable(HwamrInstance *s,uint64_t offset,size_t size) {
  hwamr::RuntimeLock lock;return s&&range(s,offset,size);
}
int hwamr_read(HwamrInstance *s,uint64_t offset,uint8_t *out,size_t size) {
  hwamr::RuntimeLock lock;if(!s)return 0;
  if(!range(s,offset,size))return 0;
  if(size&&!out)return s->fail("Wasm memory read requires an output buffer");
  if(size)std::memcpy(out,wasm_runtime_addr_app_to_native(s->instance,uint32_t(offset)),size);
  return 1;
}
int hwamr_write(HwamrInstance *s,uint64_t offset,const uint8_t *bytes,size_t size) {
  hwamr::RuntimeLock lock;if(!s)return 0;
  if(size&&!bytes)return s->fail("Wasm memory write requires bytes");
  if(!range(s,offset,size))return 0;
  if(size)std::memcpy(wasm_runtime_addr_app_to_native(s->instance,uint32_t(offset)),bytes,size);
  return 1;
}
uint64_t hwamr_memory_size(HwamrInstance *s) {
  if(!s||!s->instance)return 0;
  const auto memory=wasm_runtime_get_default_memory(s->instance);
  return wasm_memory_get_cur_page_count(memory)*wasm_memory_get_bytes_per_page(memory);
}
uint32_t hwamr_instructions(const HwamrInstance *s){return s?s->meter.used:0;}
const char *hwamr_error(const HwamrInstance *s){return s?s->error:openError;}
void hwamr_cancel(HwamrInstance *s){if(s)s->cancelled.store(true);}
}
