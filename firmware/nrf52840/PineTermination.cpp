// SPDX-License-Identifier: Apache-2.0
#include <cstdlib>
#include <new>

// Pine disables C++ exceptions; keep fatal termination without the hosted demangler.
namespace __gnu_cxx {
void __verbose_terminate_handler() { std::abort(); }
}

// libstdc++'s nothrow allocators wrap operator new in try/catch, which links the
// exception unwinder. The Arduino core's operator new reports failure as null
// and never throws, so these forward to it unchanged.
void *operator new(std::size_t size, const std::nothrow_t &) noexcept { return ::operator new(size); }
void *operator new[](std::size_t size, const std::nothrow_t &) noexcept { return ::operator new[](size); }
