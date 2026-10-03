// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <string>

inline std::string nestedBotFunctions(unsigned depth, bool global = false) {
  std::string source;
  for (unsigned i = 0; i < depth; ++i)
    source += global ? "global function nesting() " : "function nesting() ";
  for (unsigned i = 0; i < depth; ++i) source += "end ";
  return source;
}
