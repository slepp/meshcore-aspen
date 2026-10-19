// SPDX-License-Identifier: Apache-2.0
// Production Lua images link the full newlib C library. These definitions
// replace the few entry points whose newlib implementations pull large tables
// or duplicate engines into the application image. The tables and engines are
// not used. The newlib-nano profiles keep nano's own scanf and assert code.
#if defined(NRFMAST_PRODUCTION_LUA) && NRFMAST_PRODUCTION_LUA
#include <cstdarg>
#include <cstdlib>
#include <cstdio>
#include <cwctype>

extern "C" {
// newlib's scanf engines call iswspace, and its Unicode category tables add
// about 28 KB. The scanf formats used here read ASCII command text.
int iswspace(wint_t c) { return c == ' ' || (c >= '\t' && c <= '\r'); }

// The first sscanf reference otherwise adds newlib's floating-point scanf engine
// beside the integer-only engine that time conversion already links. Command
// parsing reads %u, %s and %c; a %f, %e or %g conversion would not be read.
int vsiscanf(const char *, const char *, va_list);
int sscanf(const char *input, const char *format, ...) {
  va_list arguments;
  va_start(arguments, format);
  const int count = vsiscanf(input, format, arguments);
  va_end(arguments);
  return count;
}

// newlib's dtoa and malloc code assert on internal invariants. The default
// handler formats a message through stdio; stop without it.
[[noreturn]] void __assert_func(const char *, int, const char *, const char *) { abort(); }
}
#endif
