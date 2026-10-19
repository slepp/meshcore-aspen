// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace onchip {
// Saves nothing itself: flush state, then restart after `delayMs`. The Management
// layer supplies the strong definition; the weak default reports that no restart
// was scheduled.
bool cloudRoomScheduleRestart(uint32_t delayMs);

// Runtime reports what the local Room role does this boot, for `cloudroom status`.
enum class CloudRoomRoomMode : uint8_t { Off, Local, Cloud };
void cloudRoomReportRoom(CloudRoomRoomMode mode);

// True when a saved owner alias holds the local Room role's key, so the Room role
// stays stopped while the shared room serves it. Reads the record header and keys
// only and keeps no large buffer.
bool cloudRoomOwnsLocalRoom();

// Role and trust fields of the alias at `alias` in the running configuration.
struct CloudRoomAliasInfo {
  uint8_t role = 0, flags = 0;
  bool systemRoots = false;
};
bool cloudRoomAliasInfo(unsigned alias, CloudRoomAliasInfo &info);
} // namespace onchip
