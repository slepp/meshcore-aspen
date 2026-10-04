// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "RadioDashboard.h"
#include "RoleIdentity.h"
#include "RoleProfile.h"

class WifiKissMultiplexer;
namespace onchip {
enum class LifecycleAction : uint8_t { Reboot, Reset };
enum class RolePhase : uint8_t {
  Disabled,
  Start,
  Running,
  Requested,
  Erase,
  Activate,
  Failed
};
struct RoleCallbacks {
  bool (*start)(WifiKissMultiplexer &);
  bool (*flush)();
  void (*stop)();
  bool (*eraseStep)(bool &done);
  void (*loop)();
  void (*requestFailed)();
};
enum class CompanionLifecycle : uint8_t {
  Unhandled,
  Requested,
  BadArgument,
  Busy
};
void beginLifecycles(WifiKissMultiplexer &mux,
                     const RoleCallbacks (&callbacks)[3], RoleProfile profile);
void loopLifecycles();
bool requestLifecycle(Role role, LifecycleAction action);
bool lifecycleBusy(Role role);
void reportRoleIdentity(Role role, const char *name, const uint8_t *public_key,
                        int slot, uint32_t generation, bool radio_ready);
void retireRoleSource(Role role);
void roleStatus(Role role, RadioDashboard::RoleStatus &status);
RolePhase rolePhase(Role role);
bool lifecycleCommand(Role role, uint32_t timestamp, const char *command,
                      char *reply);
CompanionLifecycle companionLifecycleCommand(const uint8_t *frame, size_t size);
} // namespace onchip
