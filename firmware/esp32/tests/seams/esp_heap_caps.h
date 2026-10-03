#pragma once
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <map>

constexpr unsigned MALLOC_CAP_SPIRAM = 1 << 10;
constexpr unsigned MALLOC_CAP_8BIT = 1 << 2;
namespace psram_test {
inline std::map<void *, size_t> allocations;
inline int failAfter = -1;
inline unsigned attempts = 0, released = 0;
inline bool (*freeOther)(void *) = nullptr;
inline bool contains(const void *address) {
  const uintptr_t target = reinterpret_cast<uintptr_t>(address);
  for (const auto &entry : allocations) {
    const uintptr_t start = reinterpret_cast<uintptr_t>(entry.first);
    if (target >= start && target < start + entry.second)
      return true;
  }
  return false;
}
} // namespace psram_test
inline void *heap_caps_calloc(size_t count, size_t size, unsigned capabilities) {
  assert(capabilities == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  ++psram_test::attempts;
  if (psram_test::failAfter == 0)
    return nullptr;
  if (psram_test::failAfter > 0)
    --psram_test::failAfter;
  void *memory = std::calloc(count, size);
  assert(memory);
  psram_test::allocations.emplace(memory, count * size);
  return memory;
}
inline void heap_caps_free(void *memory) {
  if (psram_test::freeOther && psram_test::freeOther(memory)) return;
  auto entry = psram_test::allocations.find(memory);
  assert(entry != psram_test::allocations.end());
  auto bytes = static_cast<const uint8_t *>(memory);
  for (size_t i = 0; i < entry->second; ++i)
    assert(bytes[i] == 0 && "Role secrets and packets must be scrubbed before free");
  psram_test::allocations.erase(entry);
  ++psram_test::released;
  std::free(memory);
}
