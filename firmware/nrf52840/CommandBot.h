#pragma once

#include <helpers/BaseChatMesh.h>
#ifndef NRFMAST_PRODUCTION_LUA
#define NRFMAST_PRODUCTION_LUA 0
#endif
#if !NRFMAST_PRODUCTION_LUA
#include "AdaptiveAdmission.h"
#endif

#ifndef NRFMAST_BOT_NAME
#define NRFMAST_BOT_NAME "nRF command bot"
#endif

namespace nrfmast {

class NoteStore;
class CompanionInterface;
class SharedRadio;

class CommandBot : public BaseChatMesh {
  char name[32] = NRFMAST_BOT_NAME;
  bool awaitingAck = false;
  bool importPending = false;
  const bool transmitEnabled;
  uint32_t lastReply = 0, expectedAck = 0;
  uint8_t recipient[PUB_KEY_SIZE] = {};
  uint32_t startupAdvertAt = 0;
  bool startupAdvertPending = false;
  NoteStore* notes = nullptr;
  CompanionInterface* companion = nullptr;
#if !NRFMAST_PRODUCTION_LUA
  SharedRadio* adaptiveRadio = nullptr;
  onchip::AdaptiveAdmission adaptive;
  bool adaptiveTx = false;
  mesh::Packet* adaptivePacket = nullptr;
  uint32_t adaptiveStarts = 0;
  bool sampleAdmission();
  bool admitReply(const ContactInfo& contact, size_t textSize);
  void settleReply(bool succeeded);
#endif

protected:
#if NRFMAST_PRODUCTION_LUA
  bool shouldAckMessage(const char* text) const override;
#endif
  void sendFloodScoped(const ContactInfo&, mesh::Packet* packet, uint32_t delay = 0) override {
    sendFlood(packet, delay, 3);
  }
  void sendFloodScoped(const mesh::GroupChannel&, mesh::Packet* packet, uint32_t delay = 0) override {
    sendFlood(packet, delay, 3);
  }
  bool allowPacketForward(const mesh::Packet*) override { return false; }
  int calcRxDelay(float, uint32_t) const override { return 0; }
  uint32_t getCADFailMaxDuration() const override { return UINT32_MAX; }
  bool shouldAutoAddContactType(uint8_t type) const override {
    return type == ADV_TYPE_CHAT || type == ADV_TYPE_REPEATER || type == ADV_TYPE_ROOM;
  }
  bool shouldOverwriteWhenFull() const override { return true; }
  void onDiscoveredContact(ContactInfo&, bool, uint8_t, const uint8_t*) override;
  void onContactsFull() override;
  ContactInfo* processAck(const uint8_t* data) override;
  void onContactPathUpdated(const ContactInfo&) override;
  void onMessageRecv(const ContactInfo&, mesh::Packet*, uint32_t, const char*) override;
  void onCommandDataRecv(const ContactInfo&, mesh::Packet*, uint32_t, const char*) override;
  void onSignedMessageRecv(const ContactInfo&, mesh::Packet*, uint32_t, const uint8_t*, const char*) override {}
  uint32_t calcFloodTimeoutMillisFor(uint32_t airtime) const override { return 5000 + airtime * 16; }
  uint32_t calcDirectTimeoutMillisFor(uint32_t airtime, uint8_t path) const override {
    return 3000 + airtime * 4 * ((path & 63) + 1);
  }
  void onSendTimeout() override;
  void onChannelMessageRecv(const mesh::GroupChannel&, mesh::Packet*, uint32_t, const char*) override {}
  uint8_t onContactRequest(const ContactInfo&, uint32_t, const uint8_t*, uint8_t, uint8_t*) override { return 0; }
  void onContactResponse(const ContactInfo&, const uint8_t*, uint8_t) override;
  void logTxFail(mesh::Packet*, int) override;
#if !NRFMAST_PRODUCTION_LUA
  void logTx(mesh::Packet*, int) override;
#endif

public:
  uint32_t replies = 0, throttled = 0, failures = 0, timeouts = 0;
  CommandBot(mesh::Radio& radio, mesh::MillisecondClock& ms, mesh::RNG& rng,
             mesh::RTCClock& rtc, mesh::PacketManager& pool, mesh::MeshTables& tables,
             bool canTransmit = true)
      : BaseChatMesh(radio, ms, rng, rtc, pool, tables), transmitEnabled(canTransmit) {}
  void begin(bool advertiseOnBoot = false);
  void loop();
  bool advertise(bool zeroHop = false);
  const char* getName() const { return name; }
  static bool validName(const char* value);
  bool setName(const char* value);
  void setNotes(NoteStore& store) { notes = &store; }
  void clearNotes() { notes = nullptr; }
  void setCompanion(CompanionInterface& interface) { companion = &interface; }
#if !NRFMAST_PRODUCTION_LUA
  void bindAdaptiveRadio(SharedRadio& radio) { adaptiveRadio = &radio; }
  // Boot configuration only: resetting a live controller would discard TX reservations.
  void setAdaptiveAdmission(bool enabled) { adaptive.configure(enabled, 3600, _ms->getMillis()); }
  bool adaptiveEnabled() const { return adaptive.enabled(); }
  void adaptiveStatus(bool saved, char* reply, size_t capacity);
#endif
  bool txBusy() const { return awaitingAck; }
  bool contactImportPending() const { return importPending; }
  bool importContact(const uint8_t* bytes, uint8_t length);
  uint32_t uptimeMillis() const { return _ms->getMillis(); }
  uint16_t errorFlags() const { return _err_flags; }
  uint8_t queueLength() const { return _mgr->getOutboundTotal(); }
  int companionSend(const ContactInfo& to, uint8_t type, uint8_t attempt, uint32_t timestamp,
                    const char* text, uint32_t& ack, uint32_t& timeout);
  static bool formatReply(const char* command, const mesh::Packet& packet, float rssi,
                          uint32_t uptimeSeconds, char* reply, size_t capacity);
};

}  // namespace nrfmast
