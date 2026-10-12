// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <runtime.hpp>
namespace onchip::wamr {
using hwamr::Meter;
using hwamr::RuntimeLock;
using hwamr::StackBytes;
using hwamr::PoolBytes;
enum class Kind : uint32_t { Bot = 1, Packet = 2 };
uint64_t nowUs();
struct Context : hwamr::Context {
  Context(Kind kind, Meter *meter, void *owner, bool (*tick)(void *) = nullptr)
      : hwamr::Context{uint32_t(kind), meter, owner, tick, nowUs} {}
};
bool ensure(const char *module, NativeSymbol *symbols, uint32_t count,
            char *error, size_t capacity);
inline bool profile(const uint8_t *bytes, size_t size, const char *module,
                    const char *const *imports, size_t count, char *error, size_t capacity) {
  return hwamr::profile(bytes, size, module, imports, count, error, capacity, {1, true});
}
using hwamr::signature;
inline Context *context(wasm_exec_env_t env, Kind kind) {
  return static_cast<Context *>(hwamr::context(env, uint32_t(kind)));
}
} // namespace onchip::wamr
