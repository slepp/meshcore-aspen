// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <Identity.h>

namespace onchip {
enum class Role : uint8_t { Repeater, Room, Companion };
inline const char *roleName(Role role) {
  switch (role) {
  case Role::Repeater:
    return "repeater";
  case Role::Room:
    return "room";
  case Role::Companion:
    return "companion";
  }
  return nullptr;
}
class HardwareRNG final : public mesh::RNG {
public:
  void random(uint8_t *data, size_t size) override;
};
bool loadIdentity(const char *role, mesh::LocalIdentity &identity);
bool identityPublicKey(const char *role, uint8_t publicKey[32], bool pending = false);
bool rotateIdentity(const char *role, uint8_t pendingPublicKey[32]);
enum class IdentityChange { Rejected, Conflict, Unknown, Active, Staged, Duplicate };
IdentityChange importIdentity(const char *role, const uint8_t privateKey[PRV_KEY_SIZE],
                              uint8_t publicKey[32]);
IdentityChange cancelIdentityChange(const char *role, uint8_t publicKey[32]);
bool stageIdentity(Role role, const mesh::LocalIdentity &identity);
bool resetPending(Role role, bool &pending);
bool prepareIdentityReset(Role role);
bool finishIdentityReset(Role role);
} // namespace onchip
