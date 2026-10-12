// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "wasm_export.h"
#include <atomic>
#include <mutex>
#include <stddef.h>
#include <stdint.h>

namespace hwamr {
constexpr uint32_t StackBytes = 8192, PoolBytes = 1024 * 1024;
class RuntimeLock {
  std::unique_lock<std::recursive_mutex> lock_;
public:
  RuntimeLock();
};
struct Meter {
  uint32_t fuel = 0, used = 0, native = 0;
  uint64_t deadline = 0;
  std::atomic<uint32_t> *epoch = nullptr;
  uint32_t eventEpoch = 0;
  std::atomic<bool> *cancelled = nullptr;
};
struct Context {
  uint32_t kind;
  Meter *meter;
  void *owner;
  bool (*tick)(void *) = nullptr;
  uint64_t (*clock)() = nullptr;
};
uint64_t nowUs();
struct Config {
  uint32_t poolBytes = PoolBytes;
  void *(*allocate)(size_t) = nullptr;
  void (*release)(void *) = nullptr;
};
struct Profile { uint32_t memoryPages = 1; bool exactMemoryPages = false; };
bool configure(Config config, char *error, size_t capacity);
bool initialise(char *error, size_t capacity);
bool ensure(const char *module, NativeSymbol *symbols, uint32_t count,
            char *error, size_t capacity);
bool profile(const uint8_t *bytes, size_t size, const char *module,
             const char *const *imports, size_t importCount, char *error, size_t capacity, Profile limits = {});
bool signature(wasm_module_inst_t instance, wasm_function_inst_t function, uint32_t argc);
Context *context(wasm_exec_env_t env, uint32_t kind);
} // namespace hwamr
