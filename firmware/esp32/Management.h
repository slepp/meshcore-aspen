// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "LocalRadio.h"
#include "RoleProfile.h"
#include "RoleIdentity.h"
#include <helpers/ArduinoHelpers.h>
#if defined(MESHCORE_MAST_ADMIN) && MESHCORE_MAST_ADMIN
#include "MastAdmin.h"
#include "Clock.h"
#endif

namespace onchip {
class Management {
  struct Core;
  LocalRadio radio_;
  HardwareRNG rng_;
  ArduinoMillis clock_;
#if defined(MESHCORE_MAST_ADMIN) && MESHCORE_MAST_ADMIN
  RoleClock rtc_;
#else
  VolatileRTCClock rtc_;
#endif
  Core *core_ = nullptr;
  ProfileJournal journal_{};
  uint8_t appliedMask_ = 0;
  uint8_t operatorKey_[32]{};
  bool provisioned_ = false, sealed_ = false;
  uint32_t lastVerification_ = 0;
  RadioDashboard::RoleStatus status_{};

  void receive(mesh::Packet *packet, const uint8_t *secret,
               const mesh::Identity &sender, const uint8_t *data, size_t length);
  bool acceptVerifiedRequest();

public:
  // Empty key leaves a receive-only, unprovisioned management source.
  bool begin(WifiKissMultiplexer &mux, const char *operatorPublicKeyHex);
  void loop();
  void stop();
  void dashboardStatus(RadioDashboard::RoleStatus &status) const;
  const uint8_t *publicKey() const;
  const char *name() const { return status_.name; }
  bool setName(const char *name);
  bool advertise(bool zeroHop);
  uint8_t pathWidth() const;
  bool setPathWidth(uint8_t width);
  uint64_t generation() const { return journal_.generation; }
#if defined(MESHCORE_MAST_ADMIN) && MESHCORE_MAST_ADMIN
  MastAdmin &admin();
  bool authenticatedNativeSender(const uint8_t *key) const;
#endif
  friend struct Core;
};
} // namespace onchip
