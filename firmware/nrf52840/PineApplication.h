// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <cstddef>

namespace nrfmast {
inline size_t applicationBytes(uintptr_t start, uintptr_t codeEnd,
                               uintptr_t dataStart, uintptr_t dataEnd, uintptr_t limit) {
  if (start != 0x27000 || codeEnd < start || dataEnd < dataStart || codeEnd > limit) return 0;
  if (dataEnd - dataStart > limit - codeEnd) return 0;
  const auto bytes = codeEnd - start + dataEnd - dataStart;
  return bytes && bytes <= limit - start && limit <= 0xED000 ? bytes : 0;
}
bool applicationCommand(const char*, char*);
}
