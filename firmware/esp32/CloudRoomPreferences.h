// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "CloudRoomSettings.h"
#include <stddef.h>
namespace onchip {
// Called by direct authenticated Management only; runtime connections use a boot copy.
bool cloudRoomPreferencesCommand(const char *, char *, size_t);

// Record access for the lifecycle verbs (CloudRoomLink). Every buffer is a
// CloudRoomSettingsV2 from allocateRoleStorage; the caller wipes and releases it.
enum class CloudRoomLoad : uint8_t { Ready, Invalid, Unavailable };
// The saved record or, when nothing is saved, the compiled provider's aliases.
// Nothing saved and no provider gives Ready with count 0 and enabled 1.
CloudRoomLoad cloudRoomLoadSettings(CloudRoomSettingsV2 &out);
// Validates, picks the lowest record version and publishes through the atomic
// journal path. `reply` receives the failure reason; false leaves the saved
// record untouched.
bool cloudRoomPublishSettings(CloudRoomSettingsV2 &settings, char *reply, size_t capacity);
// A Management upload is staged; link jobs refuse to run until it commits or aborts.
bool cloudRoomUploadStaged();
}
