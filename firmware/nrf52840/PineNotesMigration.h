// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "NoteStore.h"
namespace nrfmast {
bool migrateLuaNotes(const NoteStore &notes, char *error, size_t capacity);
}
