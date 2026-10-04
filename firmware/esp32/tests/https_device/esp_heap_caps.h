// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../seams/esp_heap_caps.h"
#include <algorithm>
#include <vector>
constexpr unsigned MALLOC_CAP_INTERNAL = 1 << 11;
namespace device_heap_test {
inline size_t free = 200000;
inline size_t largest = 100000;
inline unsigned long recoverAt = 0;
inline std::vector<size_t> layout, remaining;
struct Allocation { size_t block, charge; };
inline std::map<void *, Allocation> allocations;
inline size_t used = 0, minimum = SIZE_MAX;
inline int failAllocation = -1;
inline size_t externalFree = 8 * 1024 * 1024, externalLargest = 4 * 1024 * 1024;
constexpr size_t Overhead = 16;
constexpr size_t charge(size_t size) { return ((size + 3) & ~size_t(3)) + Overhead; }
inline bool release(void *memory) {
  const auto found = allocations.find(memory);
  if (found == allocations.end()) return false;
  remaining[found->second.block] += found->second.charge;
  used -= found->second.charge;
  allocations.erase(found);
  std::free(memory);
  return true;
}
}
unsigned long millis();
inline size_t heap_caps_get_free_size(unsigned caps) {
  using namespace device_heap_test;
  if (caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) {
    size_t allocated = 0;
    for (const auto &entry : psram_test::allocations) allocated += charge(entry.second);
    return allocated < externalFree ? externalFree - allocated : 0;
  }
  return (recoverAt && millis() >= recoverAt ? 200000 : device_heap_test::free) - used;
}
inline size_t heap_caps_get_largest_free_block(unsigned caps) {
  using namespace device_heap_test;
  if (caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT))
    return std::min(externalLargest, heap_caps_get_free_size(caps));
  return recoverAt && millis() >= recoverAt ? 100000 : largest;
}
inline size_t heap_caps_get_minimum_free_size(unsigned caps) {
  return std::min(device_heap_test::minimum, heap_caps_get_free_size(caps));
}
inline void *heap_caps_malloc(size_t size, unsigned caps) {
  using namespace device_heap_test;
  if (caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) {
    if (charge(size) > heap_caps_get_free_size(caps) ||
        charge(size) > heap_caps_get_largest_free_block(caps)) return nullptr;
    return heap_caps_calloc(1, size, caps);
  }
  assert(caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  psram_test::freeOther = release;
  if (failAllocation == 0) return nullptr;
  if (failAllocation > 0) --failAllocation;
  if (allocations.empty()) {
    remaining = layout;
    if (remaining.empty()) {
      size_t available = heap_caps_get_free_size(caps);
      const auto block = heap_caps_get_largest_free_block(caps);
      while (available) {
        const size_t next = std::min(available, block);
        assert(next);
        remaining.push_back(next); available -= next;
      }
    }
  }
  for (size_t i = 0; i < remaining.size(); ++i) {
    if (remaining[i] < charge(size)) continue;
    void *memory = std::malloc(size);
    assert(memory);
    remaining[i] -= charge(size);
    allocations.emplace(memory, Allocation{i, charge(size)});
    used += charge(size);
    minimum = std::min(minimum, heap_caps_get_free_size(caps));
    return memory;
  }
  return nullptr;
}
