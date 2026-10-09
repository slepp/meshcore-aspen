// SPDX-License-Identifier: Apache-2.0
#if defined(MESHCORE_ONCHIP_BOT) && MESHCORE_ONCHIP_BOT && ONCHIP_BOT_WASM
#include "PacketWasm.h"
#include "WamrRuntime.h"
#include "wasm/sdk/packet.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#ifdef ARDUINO_ARCH_ESP32
#include <esp_heap_caps.h>
#endif

namespace onchip {
using namespace packet_engine;
static_assert(unsigned(Stage::PlainCompose) == MP_PLAIN_COMPOSE &&
              unsigned(Decision::Continue) == MP_CONTINUE &&
              unsigned(Decision::Drop) == MP_DROP, "Packet ABI preserves pipeline values");
struct PacketWasm::Impl {
  wamr::Meter meter;
  wamr::Context context{wamr::Kind::Packet, &meter, this, tick};
  wasm_module_t module = nullptr;
  wasm_module_inst_t instance = nullptr;
  wasm_exec_env_t env = nullptr;
  wasm_function_inst_t process = nullptr;
  uint8_t bytes[PacketWasmSourceLimit]{};
  const Metadata *metadata = nullptr;
  Call *call = nullptr;
  static bool tick(void *owner) {
    auto &s = *static_cast<Impl *>(owner);
    return !s.call || s.call->consume();
  }
  static Impl *self(wasm_exec_env_t env) {
    auto *context = wamr::context(env, wamr::Kind::Packet);
    return context ? static_cast<Impl *>(context->owner) : nullptr;
  }
  int32_t fail(const char *message, Fault fault = Fault::Execution) {
    if (call) call->fail(fault);
    wasm_runtime_set_exception(instance, message); return -1;
  }
  bool native() {
    if (!call || !metadata) { fail("Packet import requires an active packet"); return false; }
    if (++meter.native > 64) {
      fail("Packet native-call budget exceeded (64 calls)", Fault::Fuel); return false;
    }
    return call->consume();
  }
  void *range(uint32_t offset, uint32_t length) {
    if (!wasm_runtime_validate_app_addr(instance, offset, length)) {
      fail("Packet import linear-memory range out of bounds", Fault::Bounds); return nullptr;
    }
    return wasm_runtime_addr_app_to_native(instance, offset);
  }
  bool bounds(uint32_t offset, uint32_t length) {
    if (offset > Capacity || length > Capacity) {
      fail("Packet offset or length exceeds 255 bytes", Fault::Bounds); return false;
    }
    return true;
  }
  static int32_t info(wasm_exec_env_t env, uint32_t output, uint32_t capacity) {
    auto *s = self(env); if (!s || !s->native()) return -1;
    if (capacity < sizeof(mp_info)) return s->fail("Packet metadata output is too small", Fault::Bounds);
    void *out = s->range(output, sizeof(mp_info)); if (!out) return -1;
    const auto &m = *s->metadata;
    mp_info info{};
    info.version = MP_ABI_VERSION; info.stage = unsigned(m.stage);
    info.length = s->call->size(); info.capacity = s->call->capacity();
    info.source = m.source; info.destination = m.destination; info.payload_type = m.payloadType;
    info.flags = (m.local ? MP_LOCAL : 0) | (m.authenticated ? MP_AUTHENTICATED : 0) |
        (m.engineOrigin ? MP_ENGINE_ORIGIN : 0) | (m.reflectionOrigin ? MP_REFLECTION_ORIGIN : 0);
    info.generation = m.generation; info.job = m.job; info.rssi = m.rssi;
    info.snr_quarter_db = m.snrQuarterDb; memcpy(info.identity, m.identity, sizeof(info.identity));
    memcpy(out, &info, sizeof(info)); return sizeof(info);
  }
  static int32_t read(wasm_exec_env_t env, uint32_t offset, uint32_t output, uint32_t length) {
    auto *s = self(env); if (!s || !s->native() || !s->bounds(offset, length)) return -1;
    auto *out = static_cast<uint8_t *>(s->range(output, length)); if (!out) return -1;
    return s->call->read(uint16_t(offset), out, uint16_t(length)) ? int32_t(length) : -1;
  }
  static int32_t write(wasm_exec_env_t env, uint32_t offset, uint32_t input, uint32_t length) {
    auto *s = self(env); if (!s || !s->native() || !s->bounds(offset, length)) return -1;
    auto *bytes = static_cast<const uint8_t *>(s->range(input, length)); if (!bytes) return -1;
    return s->call->write(uint16_t(offset), bytes, uint16_t(length)) ? int32_t(length) : -1;
  }
  static int32_t replace(wasm_exec_env_t env, uint32_t input, uint32_t length) {
    auto *s = self(env); if (!s || !s->native() || !s->bounds(0, length)) return -1;
    auto *bytes = static_cast<const uint8_t *>(s->range(input, length)); if (!bytes) return -1;
    return s->call->replace(bytes, uint16_t(length)) ? int32_t(length) : -1;
  }
  static int32_t emit(wasm_exec_env_t env, uint32_t input, uint32_t length, uint32_t priority,
                      uint32_t delay, uint32_t expiry) {
    auto *s = self(env); if (!s || !s->native() || !s->bounds(0, length)) return -1;
    if (priority > UINT8_MAX) return s->fail("Packet emission priority exceeds 255", Fault::Bounds);
    auto *bytes = static_cast<const uint8_t *>(s->range(input, length)); if (!bytes) return -1;
    return s->call->emit(bytes, uint16_t(length), uint8_t(priority), delay, expiry) ? int32_t(length) : -1;
  }
};
PacketWasm::~PacketWasm() { clear(); }
void PacketWasm::clear() {
  wamr::RuntimeLock lock;
  if (!impl_) return;
  if (impl_->env) wasm_runtime_destroy_exec_env(impl_->env);
  if (impl_->instance) wasm_runtime_deinstantiate(impl_->instance);
  if (impl_->module) wasm_runtime_unload(impl_->module);
  impl_->~Impl();
#ifdef ARDUINO_ARCH_ESP32
  heap_caps_free(impl_);
#else
  free(impl_);
#endif
  impl_ = nullptr;
}
bool PacketWasm::load(const uint8_t *bytes, size_t length, char *error, size_t capacity,
                      uint32_t fuel, uint32_t initUs) {
  wamr::RuntimeLock lock;
  clear(); stats_ = {};
  if (capacity) error[0] = 0;
  const auto failed = [&](const char *message) {
    snprintf(error, capacity, "%s", message);
    snprintf(stats_.error, sizeof(stats_.error), "%s", message);
    clear(); return false;
  };
  if (!bytes || length > PacketWasmSourceLimit || !fuel || fuel > 100000 ||
      !initUs || initUs > 20000) return failed("Invalid packet Wasm source or initialization budget");
  static const char *const imports[] = {"info", "read", "write", "replace", "emit"};
  if (!wamr::profile(bytes, length, "meshcore_packet_v1", imports, 5, error, capacity)) {
    snprintf(stats_.error, sizeof(stats_.error), "%s", error); return false;
  }
  static NativeSymbol symbols[] = {
    {"info", reinterpret_cast<void *>(Impl::info), "(ii)i", nullptr},
    {"read", reinterpret_cast<void *>(Impl::read), "(iii)i", nullptr},
    {"write", reinterpret_cast<void *>(Impl::write), "(iii)i", nullptr},
    {"replace", reinterpret_cast<void *>(Impl::replace), "(ii)i", nullptr},
    {"emit", reinterpret_cast<void *>(Impl::emit), "(iiiii)i", nullptr}
  };
  if (!wamr::ensure("meshcore_packet_v1", symbols, 5, error, capacity)) {
    snprintf(stats_.error, sizeof(stats_.error), "%s", error); return false;
  }
#ifdef ARDUINO_ARCH_ESP32
  void *storage = heap_caps_calloc(1, sizeof(Impl), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
  void *storage = calloc(1, sizeof(Impl));
#endif
  if (!storage) return failed("Packet Wasm session storage unavailable");
  impl_ = new (storage) Impl;
  auto &s = *impl_;
  memcpy(s.bytes, bytes, length);
  stats_.sourceBytes = uint32_t(length); stats_.sessionBytes = sizeof(Impl);
  stats_.poolBytes = wamr::PoolBytes; stats_.stackBytes = wamr::StackBytes;
  const auto loadStarted = wamr::nowUs();
  s.module = wasm_runtime_load(s.bytes, uint32_t(length), error, uint32_t(capacity));
  if (s.module) s.instance = wasm_runtime_instantiate(s.module, wamr::StackBytes, 0, error, uint32_t(capacity));
  wasm_function_inst_t init = nullptr;
  if (s.instance) {
    stats_.linearBytes = 65536;
    wasm_runtime_set_custom_data(s.instance, &s.context);
    s.env = wasm_runtime_create_exec_env(s.instance, wamr::StackBytes);
    init = wasm_runtime_lookup_function(s.instance, "mp_init");
    s.process = wasm_runtime_lookup_function(s.instance, "mp_process");
  }
  stats_.loadUs = wamr::nowUs() - loadStarted;
  if (!s.env || !wamr::signature(s.instance, init, 0) || !wamr::signature(s.instance, s.process, 0)) {
    if (!error[0]) snprintf(error, capacity, "Packet Wasm requires mp_init()->i32 and mp_process()->i32");
    snprintf(stats_.error, sizeof(stats_.error), "%s", error); clear(); return false;
  }
  if (stats_.loadUs >= initUs) return failed("Packet Wasm loader time budget exceeded");
  const auto initStarted = wamr::nowUs();
  s.meter.fuel = fuel; s.meter.deadline = initStarted + initUs;
  wasm_runtime_set_instruction_count_limit(s.env, int(fuel));
  uint32_t argv[1]{};
  const bool ok = wasm_runtime_call_wasm(s.env, init, 0, argv);
  stats_.initUs = wamr::nowUs() - initStarted;
  stats_.instructions = s.meter.used; stats_.nativeCalls = s.meter.native;
  mem_alloc_info_t memory{}; wasm_runtime_get_mem_alloc_info(&memory);
  stats_.poolHighWaterBytes = memory.highmark_size;
  if (!ok || argv[0] != MP_ABI_VERSION || stats_.initUs >= initUs) {
    snprintf(error, capacity, "%s", stats_.initUs >= initUs ? "Packet Wasm initialization time budget exceeded" :
        wasm_runtime_get_exception(s.instance) ? wasm_runtime_get_exception(s.instance) :
        "Packet Wasm initialization must return ABI version 1");
    snprintf(stats_.error, sizeof(stats_.error), "%s", error); clear(); return false;
  }
  return true;
}
Decision PacketWasm::process(const Metadata &metadata, Call &call) {
  if (!impl_) {
    snprintf(stats_.error, sizeof(stats_.error), "Packet Wasm engine is not loaded");
    return Decision::Failed;
  }
  auto &s = *impl_;
  if (s.call) {
    call.fail(Fault::Reentrant); return Decision::Failed;
  }
  s.call = &call; s.metadata = &metadata;
  s.meter = {}; s.meter.fuel = 100001; s.meter.deadline = UINT64_MAX;
  wasm_runtime_clear_exception(s.instance);
  // The pipeline meters every opcode and import against this invocation's
  // declared budget; WAMR's counter supplies an additional absolute ceiling.
  wasm_runtime_set_instruction_count_limit(s.env, 100001);
  uint32_t argv[1]{};
  const auto started = wamr::nowUs();
  const bool ok = wasm_runtime_call_wasm(s.env, s.process, 0, argv);
  stats_.invokeUs = wamr::nowUs() - started;
  stats_.instructions = s.meter.used; stats_.nativeCalls = s.meter.native;
  s.call = nullptr; s.metadata = nullptr;
  if (!ok) {
    snprintf(stats_.error, sizeof(stats_.error), "%s", wasm_runtime_get_exception(s.instance) ?
        wasm_runtime_get_exception(s.instance) : "Packet Wasm execution failed");
    return Decision::Failed;
  }
  if (argv[0] != MP_CONTINUE && argv[0] != MP_DROP) {
    snprintf(stats_.error, sizeof(stats_.error), "Packet Wasm disposition must be CONTINUE or DROP");
    call.fail(Fault::InvalidDecision); return Decision::Failed;
  }
  stats_.error[0] = 0;
  return argv[0] == MP_DROP ? Decision::Drop : Decision::Continue;
}
} // namespace onchip
#endif
