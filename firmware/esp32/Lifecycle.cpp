// SPDX-License-Identifier: Apache-2.0
#include "Lifecycle.h"
#include "Config.h"
#include <Arduino.h>
#include <Utils.h>

namespace onchip {
namespace {
struct State {
  RoleCallbacks callbacks{};
  RolePhase phase = RolePhase::Start;
  LifecycleAction action = LifecycleAction::Reboot;
  uint32_t retryAt = 0;
  RadioDashboard::RoleStatus status{};
};
State roles[3];
WifiKissMultiplexer *radioMux;

void failed(Role role, State &state, const char *operation) {
  Serial.printf("On-chip %s lifecycle %s failed; role remains offline\n",
                roleName(role), operation);
  state.phase = RolePhase::Failed;
  snprintf(state.status.fault, sizeof(state.status.fault), "%s failed",
           operation);
  state.retryAt = millis() + 5000;
}
void service(Role role, State &state) {
  switch (state.phase) {
  case RolePhase::Disabled:
    break;
  case RolePhase::Running:
    state.callbacks.loop();
    break;
  case RolePhase::Requested:
    if (state.action == LifecycleAction::Reboot && !state.callbacks.flush()) {
      Serial.printf("On-chip %s reboot refused: native state flush failed\n",
                    roleName(role));
      if (state.callbacks.requestFailed)
        state.callbacks.requestFailed();
      state.phase = RolePhase::Running;
      strcpy(state.status.fault, "Native state flush failed");
      return;
    }
    if (state.action == LifecycleAction::Reset && !prepareIdentityReset(role)) {
      if (state.callbacks.requestFailed)
        state.callbacks.requestFailed();
      state.callbacks.stop();
      failed(role, state, "durable reset intent");
      return;
    }
    state.callbacks.stop();
    state.phase = state.action == LifecycleAction::Reset ? RolePhase::Erase
                                                         : RolePhase::Start;
    break;
  case RolePhase::Erase: {
    bool done = false;
    if (!state.callbacks.eraseStep(done))
      failed(role, state, "erase");
    else if (done)
      state.phase = RolePhase::Activate;
    break;
  }
  case RolePhase::Activate:
    if (!finishIdentityReset(role))
      failed(role, state, "reset activation");
    else
      state.phase = RolePhase::Start;
    break;
  case RolePhase::Failed:
    if (static_cast<int32_t>(millis() - state.retryAt) >= 0)
      state.phase = RolePhase::Start;
    break;
  case RolePhase::Start: {
    state.status.has_identity = false;
    if (!publicProvisioningReady()) {
      failed(role, state, "public setup unavailable");
      break;
    }
    bool reset;
    if (!resetPending(role, reset)) {
      failed(role, state, "identity read");
    } else if (reset) {
      state.phase = RolePhase::Erase;
    } else if (!state.callbacks.start(*radioMux)) {
      state.callbacks.stop();
      failed(role, state, "start");
    } else {
      state.phase = RolePhase::Running;
      state.status.fault[0] = 0;
      Serial.printf("On-chip %s role ready\n", roleName(role));
    }
    break;
  }
  }
}
} // namespace
void beginLifecycles(WifiKissMultiplexer &mux,
                     const RoleCallbacks (&callbacks)[3], RoleProfile profile) {
  radioMux = &mux;
  const char *names[] = {ONCHIP_REPEATER_NAME, ONCHIP_ROOM_NAME,
                         ONCHIP_COMPANION_NAME};
  for (unsigned i = 0; i < 3; ++i) {
    roles[i] = {};
    snprintf(roles[i].status.name, sizeof(roles[i].status.name), "%s",
             names[i]);
    roles[i].callbacks = callbacks[i];
    if (profile.has(static_cast<Role>(i)))
      service(static_cast<Role>(i), roles[i]);
    else {
      roles[i].phase = RolePhase::Disabled;
      roles[i].status.source_slot = -1;
    }
  }
}
void loopLifecycles() {
  for (unsigned i = 0; i < 3; ++i) {
    auto &state = roles[i];
    const auto before = state.phase;
    service(static_cast<Role>(i), state);
    if (before == RolePhase::Running && state.phase == RolePhase::Requested)
      service(static_cast<Role>(i), state);
  }
}
RolePhase rolePhase(Role role) {
  const unsigned i = static_cast<unsigned>(role);
  return i < 3 ? roles[i].phase : RolePhase::Failed;
}
bool lifecycleBusy(Role role) { return rolePhase(role) != RolePhase::Running; }
void reportRoleIdentity(Role role, const char *name, const uint8_t *public_key,
                        int slot, uint32_t generation, bool radio_ready) {
  auto &s = roles[static_cast<unsigned>(role)].status;
  snprintf(s.name, sizeof(s.name), "%s", name);
  memcpy(s.public_key, public_key, sizeof(s.public_key));
  s.has_identity = true;
  s.source_slot = slot;
  s.source_generation = generation;
  s.ready = radio_ready;
}
void retireRoleSource(Role role) {
  auto &s = roles[static_cast<unsigned>(role)].status;
  s.source_slot = -1;
  s.source_generation = 0;
  s.ready = false;
}
void roleStatus(Role role, RadioDashboard::RoleStatus &status) {
  const auto &s = roles[static_cast<unsigned>(role)];
  status = s.status;
  snprintf(status.role, sizeof(status.role), "%s", roleName(role));
  const char *phase = "starting";
  switch (s.phase) {
  case RolePhase::Disabled:
    phase = "disabled";
    break;
  case RolePhase::Running:
    phase = status.ready ? "running" : "waiting-radio";
    break;
  case RolePhase::Requested:
    phase = "restarting";
    break;
  case RolePhase::Erase:
    phase = "erasing";
    break;
  case RolePhase::Activate:
    phase = "activating";
    break;
  case RolePhase::Failed:
    phase = "fault";
    break;
  case RolePhase::Start:
    break;
  }
  snprintf(status.state, sizeof(status.state), "%s", phase);
  status.ready = status.ready && s.phase == RolePhase::Running;
}
bool requestLifecycle(Role role, LifecycleAction action) {
  const unsigned i = static_cast<unsigned>(role);
  if (i >= 3 ||
      (action != LifecycleAction::Reboot && action != LifecycleAction::Reset)) {
    Serial.println("Invalid on-chip lifecycle role or action");
    return false;
  }
  if (roles[i].phase != RolePhase::Running)
    return false;
  roles[i].action = action;
  roles[i].status.fault[0] = 0;
  roles[i].phase = RolePhase::Requested;
  return true;
}
bool lifecycleCommand(Role role, uint32_t timestamp, const char *command,
                      char *reply) {
  if (!strncmp(command, "reboot", 6)) {
    if (requestLifecycle(role, LifecycleAction::Reboot))
      reply[0] = 0;
    else
      strcpy(reply, "Error: role lifecycle busy");
    return true;
  }
  if (!strcmp(command, "erase") && timestamp == 0) {
    strcpy(reply, requestLifecycle(role, LifecycleAction::Reset)
                      ? "OK - role erase scheduled"
                      : "Error: role lifecycle busy");
    return true;
  }
  if (!strncmp(command, "set prv.key ", 12)) {
    if (lifecycleBusy(role)) {
      strcpy(reply, "Error: role lifecycle busy");
      return true;
    }
    uint8_t key[PRV_KEY_SIZE]{};
    const bool valid = strlen(command + 12) == PRV_KEY_SIZE * 2 &&
                       mesh::Utils::fromHex(key, PRV_KEY_SIZE, command + 12) &&
                       mesh::LocalIdentity::validatePrivateKey(key);
    if (!valid) {
      strcpy(reply, "Error, bad key");
    } else {
      mesh::LocalIdentity replacement;
      replacement.readFrom(key, sizeof(key));
      if (stageIdentity(role, replacement)) {
        strcpy(reply, "OK, reboot to apply! New pubkey: ");
        mesh::Utils::toHex(reply + 33, replacement.pub_key, PUB_KEY_SIZE);
      } else {
        strcpy(reply, "Error: identity staging failed");
      }
      auto p = reinterpret_cast<volatile uint8_t *>(&replacement);
      for (size_t i = 0; i < sizeof(replacement); ++i)
        p[i] = 0;
    }
    auto p = static_cast<volatile uint8_t *>(key);
    for (size_t i = 0; i < sizeof(key); ++i)
      p[i] = 0;
    return true;
  }
  return false;
}
CompanionLifecycle companionLifecycleCommand(const uint8_t *frame,
                                             size_t size) {
  if (!size || (frame[0] != 19 && frame[0] != 51))
    return CompanionLifecycle::Unhandled;
  const bool reboot = frame[0] == 19;
  const size_t expected = reboot ? 7 : 6;
  if (size != expected ||
      memcmp(frame + 1, reboot ? "reboot" : "reset", expected - 1))
    return CompanionLifecycle::BadArgument;
  return requestLifecycle(Role::Companion, reboot ? LifecycleAction::Reboot
                                                  : LifecycleAction::Reset)
             ? CompanionLifecycle::Requested
             : CompanionLifecycle::Busy;
}
} // namespace onchip
