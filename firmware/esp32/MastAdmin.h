// SPDX-License-Identifier: Apache-2.0
#pragma once
#ifdef NRF52_PLATFORM
#include "PineAdmin.h"
#else
#if defined(MESHCORE_MAST_ADMIN) && MESHCORE_MAST_ADMIN
#include "LocalRadio.h"
#include "RoleProfile.h"
#include "MastSource.h"

namespace onchip {
class Management;
class MastAdmin {
public:
  static constexpr size_t TextLimit = 162;
  static constexpr unsigned ACLSlots = 5;
  static constexpr unsigned LegacyReplaySlots = 4;
  static constexpr unsigned ExtraReplaySlots = ACLSlots + 1;
  static constexpr unsigned ReplaySlots = LegacyReplaySlots + ExtraReplaySlots;
  static constexpr unsigned SessionSlots = ACLSlots + 2;
  enum class Transport { Other, NativeEncrypted, AuthenticatedWeb };
  struct Reply {
    char text[TextLimit + 1]{};
    uint32_t ticket = 0;
  };
  struct WifiCredentials {
    char ssid[33]{}, password[65]{};
  };
  bool begin(WifiKissMultiplexer &mux, uint8_t appliedMask, ProfileJournal &journal,
             Management &management);
  void execute(const char *command, Reply &reply, uint32_t invokingBotJob = 0,
               Transport transport = Transport::Other, const uint8_t *nativeSender = nullptr,
               size_t replyCapacity = TextLimit + 1);
  void acknowledged(uint32_t ticket, bool transmitted);
  void loop();
  static bool loadWifi(WifiCredentials &credentials, bool &present);
  static bool loadWifiEnabled(bool &enabled);
  static constexpr size_t OwnerInfoLimit = 119;
  bool ownerInfo(char *text, size_t size) const;
  bool trusted(const uint8_t key[32]) const;
  bool compiledTrusted(const uint8_t key[32]) const;
  bool ready() const { return ready_; }
  bool cancelled(uint32_t ticket) const { return ticket && ticket == cancelledTicket_; }
  bool rememberTimestamp(const uint8_t key[32], uint32_t timestamp);
  uint32_t lastTimestamp(const uint8_t key[32]) const;
  static bool passwordMatches(const char *password);
  const char *outcome() const { return outcome_; }
  void stop();
  static MastAdmin *service();

private:
  struct Settings {
    uint32_t version = 1;
    WifiCredentials wifi;
    uint8_t wifiSet = 0, trustedSet = 0, reserved[2]{};
    uint8_t trustedKey[32]{};
  } settings_;
  struct Replay {
    uint32_t version = 1;
    struct Peer { uint8_t key[32]{}; uint32_t timestamp = 0; } peers[LegacyReplaySlots];
  } replay_;
  struct ExtraReplay {
    uint32_t version = 1;
    Replay::Peer peers[ExtraReplaySlots];
    uint8_t digest[32]{};
  } extraReplay_;
  struct OwnerReplay { uint32_t version = 1; Replay::Peer peer; } ownerReplay_;
  struct ACL {
    uint32_t version = 1;
    struct Entry { uint8_t key[32]{}, permissions = 0; } entries[ACLSlots];
    uint8_t reserved[3]{}, digest[32]{};
  } acl_;
  static_assert(sizeof(Settings) == 140 && sizeof(Replay) == 148 && sizeof(OwnerReplay) == 40,
                "Legacy mast administration records require explicit migration");
  static_assert(sizeof(ACL) == 204 && sizeof(ExtraReplay) == 252,
                "Management ACL/replay record layout changed");
  bool aclReady_ = false;
  bool ready_ = false;
  MastSource source_;
  WifiKissMultiplexer *mux_ = nullptr;
  Management *management_ = nullptr;
  ProfileJournal *journal_ = nullptr;
  uint8_t appliedMask_ = 0;
  enum class Effect { None, Reboot, Wifi, Radio, Temporary, CAD, Interference, Airtime, Controls } effect_ = Effect::None;
  uint32_t ticket_ = 0, nextTicket_ = 0, admittedAt_ = 0, readyAt_ = 0;
  uint32_t cancelledTicket_ = 0;
  bool armed_ = false, temporary_ = false;
  uint32_t duration_ = 0, restoreAt_ = 0;
  RadioConfig nextRadio_{};
  bool nextCAD_ = true;
  uint8_t nextInterference_ = 0;
  float nextAirtime_ = 1;
  uint16_t nextAGCSeconds_ = 30;
  bool nextRxConfigured_ = false, nextRxBoost_ = false;
  char outcome_[80] = "idle";
  bool reserve(Effect effect, Reply &reply);
  void roleCommand(char *command, Reply &reply, Transport transport,
                   const uint8_t *nativeSender, uint32_t invokingBotJob);
  void keyCommand(const char *command, Reply &reply, Transport transport);
  void passwordCommand(const char *argument, Reply &reply, Transport transport);
  void wifiCommand(char *command, Reply &reply, Transport transport, uint32_t invokingBotJob);
  void preferenceCommand(char *command, Reply &reply, Transport transport, uint32_t invokingBotJob);
  void statsCommand(const char *topic, Reply &reply);
  void setupCommand(const char *command, Reply &reply);
  void helpCommand(const char *argument, Reply &reply);
  void rolesCommand(const char *argument, Reply &reply);
  bool validACL(const ACL &acl) const;
  bool validExtraReplay(const ExtraReplay &replay) const;
  bool loadReplay();
  bool saveReplay(const Replay &replay, const ExtraReplay &extra);
#if defined(MESHCORE_MAST_WEB_TEST)
  friend struct MastReplayStorageTest;
#endif
  void aclCommand(char *command, Reply &reply);
};
} // namespace onchip
#endif
#endif
