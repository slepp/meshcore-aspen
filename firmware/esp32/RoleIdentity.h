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
// Copies a role's active 64-byte private key for one transfer to a shared-room service.
// Refuses an absent identity, a staged rotation and a reset in progress; it never
// creates or activates a key. The caller wipes `out` with a volatile loop.
enum class ExportResult : uint8_t { Ok, Absent, Staged, Reset, Invalid };
ExportResult exportRoleKey(const char *role, uint8_t out[PRV_KEY_SIZE]);
// First public-key byte of a role's active identity; false, without a diagnostic, when
// the role has none.
bool identityFirstByte(const char *role, uint8_t &first);
// Active public key of a role; false, without a diagnostic, when the role has none.
bool identityPublicKeyIfPresent(const char *role, uint8_t publicKey[32]);
// Signs with the management identity, loading the key per call and wiping it. False
// when the identity is absent or a rotation is staged.
bool signManagement(const uint8_t *message, size_t size, uint8_t signature[64], uint8_t publicKey[32]);
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
