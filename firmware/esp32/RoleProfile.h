// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>
#include <string.h>

namespace onchip {
enum class Role : uint8_t;

struct RoleProfile {
  static constexpr uint8_t Repeater = 1u << 0;
  static constexpr uint8_t Room = 1u << 1;
  static constexpr uint8_t Companion = 1u << 2;
  static constexpr uint8_t Observer = 1u << 3;
  static constexpr uint8_t All = Repeater | Room | Companion | Observer;

  uint8_t enabled;
  constexpr RoleProfile(uint8_t mask = All) : enabled(mask) {}
  bool valid() const { return (enabled & ~All) == 0; }
  bool bootableWith(const char *adminPassword, const char *roomPassword) const {
    return valid() && adminPassword && roomPassword &&
           strlen(adminPassword) <= 15 && strlen(roomPassword) <= 15 &&
           (!(enabled & (Repeater | Room | Companion)) || adminPassword[0]);
  }
  bool has(Role role) const {
    return static_cast<unsigned>(role) < 3 &&
           (enabled & (1u << static_cast<unsigned>(role))) != 0;
  }
  bool observer() const { return (enabled & Observer) != 0; }
};

struct ProfileJournal {
  RoleProfile profile;
  uint64_t generation;
  uint64_t nonce;
  constexpr ProfileJournal(RoleProfile selected = {}, uint64_t sequence = 0,
                           uint64_t requestNonce = 0)
      : profile(selected), generation(sequence), nonce(requestNonce) {}
};

// Saving never changes the running boot profile. Once an RF generation has
// been committed, legacy local saves cannot roll its replay journal back.
// Missing storage defaults to All; any other read/validation failure returns
// false and clears the output rather than enabling roles.
bool loadRoleProfile(RoleProfile &profile);
bool loadProfileJournal(ProfileJournal &journal);
bool saveRoleProfile(const RoleProfile &profile);
bool commitProfileJournal(const ProfileJournal &journal);
bool loadOriginPathWidth(uint8_t &width);
bool saveOriginPathWidth(uint8_t width);
} // namespace onchip
