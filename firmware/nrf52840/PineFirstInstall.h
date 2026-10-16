// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstddef>
#include <cstdint>

namespace nrfmast {
inline bool erasedFlash(const volatile uint32_t* words, size_t count) {
  if (!words || !count) return false;
  for (size_t i = 0; i < count; ++i) if (words[i] != UINT32_MAX) return false;
  return true;
}
}
