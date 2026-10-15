// SPDX-License-Identifier: Apache-2.0
#include <cstdlib>

// Pine disables C++ exceptions; keep fatal termination without the hosted demangler.
namespace __gnu_cxx {
void __verbose_terminate_handler() { std::abort(); }
}
