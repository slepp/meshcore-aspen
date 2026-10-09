// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stddef.h>
namespace onchip {
// Called by direct authenticated Management only; runtime connections use a boot copy.
bool cloudRoomPreferencesCommand(const char *, char *, size_t);
}
