// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
extern "C" {
#include "lua.h"
#include "lauxlib.h"
}
namespace hlua {
// Count the rejected step too, saturating instead of wrapping at the limit.
template<class Counter, class Limit>
bool charge(Counter &counter, Limit limit, uint64_t units = 1) {
  const auto maximum = std::numeric_limits<Counter>::max();
  const bool overflow = units > uint64_t(maximum - counter);
  counter = overflow ? maximum : Counter(counter + units);
  return !overflow && uint64_t(counter) <= uint64_t(limit);
}
struct Heap {
  enum AllocationFailure { NoAllocationFailure, HeapLimit, AllocatorFailure };
  size_t live = 0;
  AllocationFailure allocationFailure = NoAllocationFailure;
  void (*parserStep)(lua_State *, Heap &) = nullptr;
  void (*instructionStep)(lua_State *, Heap &) = nullptr;
  // Platform adapters implement allocation/headroom policy here.
  void *(*reallocate)(Heap &, void *, size_t, size_t) = systemReallocate;
  static void *systemReallocate(Heap &, void *pointer, size_t, size_t size) {
    if (!size) { std::free(pointer); return nullptr; }
    return std::realloc(pointer, size);
  }
  template<class Size>
  void *resize(void *pointer, size_t oldSize, size_t size, size_t limit, Size &peak) {
    if (!pointer) oldSize = 0;
    if (!size) {
      reallocate(*this, pointer, oldSize, 0);
      live -= oldSize;
      return nullptr;
    }
    if (size > limit || live - oldSize > limit - size) {
      allocationFailure = HeapLimit;
      return nullptr;
    }
    void *replacement = reallocate(*this, pointer, oldSize, size);
    if (!replacement) { allocationFailure = AllocatorFailure; return nullptr; }
    live = live - oldSize + size;
    if (live > peak) peak = Size(live);
    return replacement;
  }
};
Heap &heap(lua_State *state);
lua_State *newState(lua_Alloc allocator, Heap &owner);
void instructionHook(lua_State *state, lua_Debug *);
}
