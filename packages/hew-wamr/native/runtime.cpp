#include "runtime.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <mutex>
namespace hwamr {
uint64_t nowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}
namespace {
std::recursive_mutex runtimeLock;
bool runtimeReady = false;
void *runtimePool = nullptr;
Config runtimeConfig{PoolBytes, std::malloc, std::free};
bool configured = false;
struct Registration {
  const char *module = nullptr;
  NativeSymbol *symbols = nullptr;
  uint32_t count = 0;
} registrations[2];
struct Reader {
  const uint8_t *p, *end;
  bool ok = true;
  uint32_t leb() {
    uint32_t result = 0;
    for (unsigned i = 0; i < 5; ++i) {
      if (p == end) { ok = false; return 0; }
      const uint8_t b = *p++;
      if (i == 4 && (b & 0xf0)) { ok = false; return 0; }
      result |= uint32_t(b & 127) << (7 * i);
      if (!(b & 128)) return result;
    }
    ok = false; return 0;
  }
  bool name(char *out, size_t capacity) {
    const uint32_t n = leb();
    if (!ok || n >= capacity || n > size_t(end - p) || memchr(p, 0, n)) {
      ok = false; return false;
    }
    memcpy(out, p, n); out[n] = 0; p += n; return true;
  }
};
}
RuntimeLock::RuntimeLock() : lock_(runtimeLock) {}
bool configure(Config config, char *error, size_t capacity) {
  RuntimeLock lock;
  if (bool(config.allocate) != bool(config.release)) {
    snprintf(error, capacity, "WAMR allocator requires both allocate and release"); return false;
  }
  if (!config.allocate) config.allocate = std::malloc;
  if (!config.release) config.release = std::free;
  if (config.poolBytes < 128 * 1024 || config.poolBytes > 64 * 1024 * 1024) {
    snprintf(error, capacity, "WAMR pool must be 128KiB..64MiB"); return false;
  }
  if (configured && (runtimeConfig.poolBytes != config.poolBytes ||
      runtimeConfig.allocate != config.allocate || runtimeConfig.release != config.release)) {
    snprintf(error, capacity, "WAMR runtime configuration is already fixed"); return false;
  }
  runtimeConfig = config; configured = true; return true;
}
bool initialise(char *error, size_t capacity) {
  RuntimeLock lock;
  if (runtimeReady) return true;
  configured = true;
  runtimePool = runtimeConfig.allocate(runtimeConfig.poolBytes);
  RuntimeInitArgs args{};
  args.mem_alloc_type = Alloc_With_Pool;
  args.mem_alloc_option.pool.heap_buf = runtimePool;
  args.mem_alloc_option.pool.heap_size = runtimeConfig.poolBytes;
  runtimeReady = runtimePool && wasm_runtime_full_init(&args);
  if (!runtimeReady) {
    if (runtimePool) runtimeConfig.release(runtimePool); runtimePool = nullptr;
    snprintf(error, capacity, "WAMR %u-byte runtime pool unavailable", runtimeConfig.poolBytes);
  }
  return runtimeReady;
}
bool ensure(const char *module, NativeSymbol *symbols, uint32_t count,
            char *error, size_t capacity) {
  RuntimeLock lock;
  Registration *free = nullptr;
  for (auto &r : registrations) {
    if (r.module && !strcmp(r.module, module)) {
      if (r.symbols == symbols && r.count == count) return true;
      snprintf(error, capacity, "WAMR native module registration conflicts: %s", module);
      return false;
    }
    if (!r.module && !free) free = &r;
  }
  if (!free) {
    snprintf(error, capacity, "WAMR native module registry is full"); return false;
  }
  if (!initialise(error, capacity)) return false;
  if (!wasm_runtime_register_natives(module, symbols, count)) {
    snprintf(error, capacity, "WAMR native module registration failed: %s", module); return false;
  }
  *free = {module, symbols, count};
  return true;
}
bool profile(const uint8_t *bytes, size_t size, const char *module,
             const char *const *imports, size_t importCount, char *error, size_t capacity, Profile limits) {
  const auto fail = [&](const char *message) { snprintf(error, capacity, "%s", message); return false; };
  if (size < 8 || memcmp(bytes, "\0asm\1\0\0\0", 8)) return fail("Expected portable Wasm version 1");
  Reader r{bytes + 8, bytes + size};
  bool memory = false;
  uint16_t seen = 0;
  while (r.p < r.end && r.ok) {
    const uint8_t id = *r.p++;
    const uint32_t length = r.leb();
    if (!r.ok || length > size_t(r.end - r.p) || id > 11 ||
        (id && (seen & (1u << id)))) return fail("Invalid Wasm section envelope");
    if (id) seen |= 1u << id;
    Reader s{r.p, r.p + length}; r.p += length;
    if (id == 2) {
      const uint32_t count = s.leb();
      if (count > importCount) return fail("Wasm import count exceeds ABI limit");
      for (uint32_t i = 0; i < count; ++i) {
        char importedModule[32]{}, name[32]{};
        if (!s.name(importedModule, sizeof(importedModule)) || !s.name(name, sizeof(name)) ||
            s.p == s.end || strcmp(importedModule, module) || *s.p++ != 0)
          return fail("Wasm import requires an allowed ABI function");
        bool known = false;
        for (size_t n = 0; n < importCount; ++n) known = known || !strcmp(name, imports[n]);
        if (!known) return fail("Unsupported Wasm ABI import");
        s.leb();
      }
    } else if (id == 4) {
      if (s.leb() != 0) return fail("Wasm tables are unavailable in ABI v1");
    } else if (id == 5) {
      if (s.leb() != 1 || s.leb() != 1)
        return fail("Wasm requires one unshared memory with an explicit maximum");
      const uint32_t initial = s.leb(), maximum = s.leb();
      if (!initial || initial > maximum || maximum > limits.memoryPages ||
          (limits.exactMemoryPages && (initial != limits.memoryPages || maximum != initial)))
        return fail("Wasm memory exceeds its configured page profile");
      memory = true;
    } else if (id == 8) return fail("Wasm start section is unavailable; use a metered initialization export");
    else if (id == 7) {
      const uint32_t count = s.leb();
      if (count > 16) return fail("Wasm export limit is 16");
      for (uint32_t i = 0; i < count; ++i) {
        char name[64]{};
        if (!s.name(name, sizeof(name)) || s.p == s.end) return fail("Invalid Wasm export");
        ++s.p; s.leb();
        for (const char *implicit : {"_start", "_initialize", "__post_instantiate", "__wasm_call_ctors"})
          if (!strcmp(name, implicit)) return fail("Wasm implicit initialization export is unavailable");
      }
    }
    if (!s.ok) return fail("Invalid bounded Wasm section");
  }
  return r.ok && memory ? true : fail("Wasm module requires bounded linear memory");
}
bool signature(wasm_module_inst_t instance, wasm_function_inst_t function, uint32_t argc) {
  if (!function || argc > 3 || wasm_func_get_param_count(function, instance) != argc ||
      wasm_func_get_result_count(function, instance) != 1) return false;
  wasm_valkind_t types[3]{};
  wasm_func_get_param_types(function, instance, types);
  for (unsigned i = 0; i < argc; ++i) if (types[i] != WASM_I32) return false;
  wasm_func_get_result_types(function, instance, types); return types[0] == WASM_I32;
}
Context *context(wasm_exec_env_t env, uint32_t kind) {
  auto instance = wasm_runtime_get_module_inst(env);
  auto *c = static_cast<Context *>(wasm_runtime_get_custom_data(instance));
  if (!c || c->kind != kind || !c->meter || !c->owner) {
    wasm_runtime_set_exception(instance, "Wasm execution context unavailable"); return nullptr;
  }
  return c;
}
} // namespace hwamr

extern "C" bool hwamr_tick(wasm_exec_env_t env) {
  auto instance = wasm_runtime_get_module_inst(env);
  auto *c = static_cast<hwamr::Context *>(wasm_runtime_get_custom_data(instance));
  if (!c || !c->meter || !c->owner) {
    wasm_runtime_set_exception(instance, "Wasm execution has no owned meter"); return false;
  }
  auto &meter = *c->meter;
  if (meter.used >= meter.fuel) {
    if (meter.used != UINT32_MAX) ++meter.used;
    wasm_runtime_set_exception(instance, "Wasm instruction budget exceeded"); return false;
  }
  ++meter.used;
  if (meter.cancelled && meter.cancelled->load()) {
    wasm_runtime_set_exception(instance, "Wasm instance cancelled"); return false;
  }
  if (c->tick && !c->tick(c->owner)) {
    wasm_runtime_set_exception(instance, "Wasm packet instruction/time budget exceeded"); return false;
  }
  if ((meter.used & 63) == 1 &&
      ((c->clock ? c->clock() : hwamr::nowUs()) >= meter.deadline ||
       (meter.eventEpoch && meter.epoch && meter.eventEpoch != meter.epoch->load()))) {
    wasm_runtime_set_exception(instance, "Wasm deadline/event epoch revoked"); return false;
  }
  return true;
}
