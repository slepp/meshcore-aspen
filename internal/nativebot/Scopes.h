// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <helpers/TransportKeyStore.h>

namespace onchip {
struct HostScopes {
  TransportKey home{}, fallback{};
  bool configured = false;
};
inline HostScopes nativeHostScopes;
}
