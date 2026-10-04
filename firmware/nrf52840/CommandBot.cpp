#include "CommandBot.h"
#if NRFMAST_PRODUCTION_LUA
#include "onchip/CommandBot.h"
#include "PineAdmin.h"
#endif
#include "NoteStore.h"
#include "CompanionInterface.h"
#include "SharedRadio.h"
#include <cmath>
#include <cstdio>

namespace nrfmast {
#if NRFMAST_PRODUCTION_LUA
bool CommandBot::shouldAckMessage(const char*) const {
  return !onchip::commandBotService().sourceReady();
}
#endif

bool CommandBot::validName(const char* value) {
  if (!value || !value[0] || strlen(value) >= 32) return false;
  for (const char* p = value; *p; ++p)
    if (*p < 32 || *p > 126) return false;
  return value[0] != ' ' && value[strlen(value) - 1] != ' ';
}

bool CommandBot::setName(const char* value) {
  if (!validName(value)) return false;
  strcpy(name, value);
  return true;
}

void CommandBot::begin(bool advertiseOnBoot) {
  mesh::Mesh::begin();
  startupAdvertAt = uint32_t(_ms->getMillis()) + 20000;
  startupAdvertPending = advertiseOnBoot;
}

void CommandBot::loop() {
  BaseChatMesh::loop();
#if !NRFMAST_PRODUCTION_LUA
  sampleAdmission();
#endif
  // The native loop has drained its single private imported-advert slot.
  importPending = false;
  if (startupAdvertPending && int32_t(uint32_t(_ms->getMillis()) - startupAdvertAt) >= 0)
    advertise();
}

#if !NRFMAST_PRODUCTION_LUA
bool CommandBot::sampleAdmission() {
  if (!adaptiveRadio) return false;
  adaptive.sample(_ms->getMillis(), 1, adaptiveRadio->aggregateActiveMs(),
                  adaptiveRadio->receivedAirtimeMs(),
                  adaptiveRadio->queuedPackets() ? adaptiveRadio->queuedPackets() : _mgr->getOutboundTotal());
  return true;
}

bool CommandBot::admitReply(const ContactInfo& contact, size_t textSize) {
  if (!adaptive.enabled()) return true;
  if (_mgr->getOutboundTotal() > 8) {
    adaptive.recordDenial(onchip::AdaptiveAdmission::ReservationsFull);
    ++throttled;
    Serial.println("bot: command not run; native reply queue exceeds tracking capacity; notes unchanged");
    return false;
  }
  const uint32_t now = _ms->getMillis();
  const bool metrics = sampleAdmission();
  const unsigned cipher = ((5 + textSize + CIPHER_BLOCK_SIZE - 1) / CIPHER_BLOCK_SIZE) * CIPHER_BLOCK_SIZE;
  const unsigned path = contact.out_path_len == OUT_PATH_UNKNOWN ? 0 :
      (contact.out_path_len & 63u) * ((contact.out_path_len >> 6) + 1u);
  const unsigned bytes = 4 + CIPHER_MAC_SIZE + cipher + path;
  const uint32_t estimate = _radio->getEstAirtimeFor(bytes > MAX_TRANS_UNIT ? MAX_TRANS_UNIT : bytes);
  auto reason = adaptive.work(now, contact.id.pub_key, metrics, adaptiveTx ? 1 : 0, adaptiveTx ? 1 : 0,
                              estimate);
  if (reason == onchip::AdaptiveAdmission::Allowed && adaptiveTx)
    reason = adaptive.recordDenial(onchip::AdaptiveAdmission::Congestion);
  if (reason == onchip::AdaptiveAdmission::Allowed)
    reason = adaptive.reserve(now, contact.id.pub_key, &adaptiveTx,
                              estimate, metrics);
  if (reason != onchip::AdaptiveAdmission::Allowed) {
    ++throttled;
    Serial.printf("bot: command not run; %s; inspect bot adaptive; notes unchanged; no automatic retry\n",
                  onchip::AdaptiveAdmission::name(reason));
    return false;
  }
  adaptiveStarts = adaptiveRadio->bot.transmissionsStarted;
  adaptiveTx = true;
  return true;
}

void CommandBot::settleReply(bool succeeded) {
  if (!adaptiveTx) return;
  const bool started = adaptiveRadio && adaptiveRadio->bot.transmissionsStarted != adaptiveStarts;
  adaptive.finish(_ms->getMillis(), &adaptiveTx, succeeded || !started,
                  succeeded && adaptiveRadio ? adaptiveRadio->lastTransmitMs() : 0);
  adaptiveTx = false;
  adaptivePacket = nullptr;
}

void CommandBot::adaptiveStatus(bool saved, char* reply, size_t capacity) {
  const bool metrics = sampleAdmission();
  snprintf(reply, capacity,
           "Adaptive saved=%u live=%u metrics=%u congested=%u local-load-permille=%u allowance-permille=%u callers=%u pending=%u denied=%u last=%s",
           saved, adaptive.enabled(), metrics, adaptive.congested(), adaptive.loadPermille(),
           adaptive.scalePermille(), adaptive.callers(), adaptive.pending(), adaptive.denied(),
           onchip::AdaptiveAdmission::code(adaptive.last()));
}
#endif

bool CommandBot::importContact(const uint8_t* bytes, uint8_t length) {
  if (importPending) return false;
  importPending = BaseChatMesh::importContact(bytes, length);
  return importPending;
}

bool CommandBot::formatReply(const char* command, const mesh::Packet& packet, float rssi,
                             uint32_t uptimeSeconds, char* reply, size_t capacity) {
  int length = -1;
  if (strcmp(command, "!ping") == 0) {
    length = snprintf(reply, capacity, "pong uptime_s=%lu", static_cast<unsigned long>(uptimeSeconds));
  } else if (strcmp(command, "!help") == 0) {
    length = snprintf(reply, capacity, "!ping !signal !path !remember key value !recall key !forget key !list; DM-only; 4 notes/user,16/device,96B/value; one reply awaiting ACK; cooldown=0");
  } else if (strcmp(command, "!signal") == 0 || strcmp(command, "!path") == 0) {
    char path[42] = {};
    const size_t bytes = packet.getPathByteLen();
    const size_t shown = bytes > 18 ? 18 : bytes;
    if (bytes > sizeof(packet.path) || !mesh::Packet::isValidPathLen(packet.path_len)) return false;
    for (size_t i = 0; i < shown; ++i) snprintf(path + i * 2, 3, "%02x", packet.path[i]);
    if (bytes > shown) strcpy(path + shown * 2, "...");
    char signal[48];
    // Integer formatting avoids the nRF newlib-nano printf float omission.
    int quarter = packet._snr;
    int magnitude = quarter < 0 ? -quarter : quarter;
    char receivedRssi[16];
    if (std::isfinite(rssi) && rssi >= -200 && rssi <= 100)
      snprintf(receivedRssi, sizeof(receivedRssi), "%ddBm", int(rssi));
    else strcpy(receivedRssi, "unknown");
    snprintf(signal, sizeof(signal), "snr=%s%d.%02ddB rssi=%s",
             quarter < 0 ? "-" : "", magnitude / 4, (magnitude % 4) * 25, receivedRssi);
    if (packet.isRouteFlood()) {
      length = snprintf(reply, capacity, "%s flood_hops=%u hash_bytes=%u path=%s",
                        signal, packet.getPathHashCount(), packet.getPathHashSize(),
                        shown ? path : "(empty)");
    } else {
      length = snprintf(reply, capacity, "%s direct_remaining=%u end_to_end_hops=unknown path=%s",
                        signal, packet.getPathHashCount(), shown ? path : "(empty)");
    }
  }
  if (length < 0 || size_t(length) >= capacity) {
    if (capacity) reply[0] = 0;
    return false;
  }
  return true;
}

void CommandBot::onMessageRecv(const ContactInfo& contact, mesh::Packet* packet,
                               uint32_t timestamp, const char* text) {
#if NRFMAST_PRODUCTION_LUA
  if (companion) companion->received(contact, *packet, timestamp, TXT_TYPE_PLAIN, text);
  return;
#else
  char reply[MAX_TEXT_LEN + 1];
  uint32_t now = _ms->getMillis();
  const bool noteCommand = NoteStore::isCommand(text);
  if (!noteCommand && !formatReply(text, *packet, _radio->getLastRSSI(), now / 1000, reply, sizeof(reply))) {
    if (companion) companion->received(contact, *packet, timestamp, TXT_TYPE_PLAIN, text);
    return;
  }
  if (!transmitEnabled) {
    ++failures;
    Serial.println("bot: command rejected; RF transmission disabled; notes unchanged");
    return;
  }
  if (awaitingAck) {
    ++throttled;
    Serial.println("bot: command rejected; previous reply awaiting ACK; notes unchanged");
    return;
  }
  // This pool is owned by the mesh loop; BLE callbacks only enqueue transport frames.
  if (_mgr->getFreeCount() == 0) {
    ++failures;
    Serial.println("bot: command rejected; no reply packet slot; notes unchanged");
    return;
  }
  if (!admitReply(contact, noteCommand ? MAX_TEXT_LEN : strlen(reply))) return;
  if (noteCommand) {
    if (notes) notes->command(self_id.pub_key, contact.id.pub_key, timestamp, text, reply, sizeof(reply));
    else strcpy(reply, "Error: notes storage unavailable");
  }
  // Bind the newly queued native packet, not a companion TXT/ACK callback.
  mesh::Packet* previous[8]{};
  if (adaptiveTx)
    for (int i = 0; i < _mgr->getOutboundTotal() && i < 8; ++i) previous[i] = _mgr->getOutboundByIdx(i);
  uint32_t timeout;
  if (sendMessage(contact, getRTCClock()->getCurrentTimeUnique(), 0, reply,
                  expectedAck, timeout) == MSG_SEND_FAILED) {
    settleReply(false);
    ++failures;
    Serial.println("bot: no packet slot for reply");
    return;
  }
  adaptive.accepted(&adaptiveTx);
  if (adaptiveTx) {
    for (int i = 0; i < _mgr->getOutboundTotal(); ++i) {
      auto* queued = _mgr->getOutboundByIdx(i);
      bool existed = false;
      for (auto* old : previous) existed = existed || old == queued;
      if (!existed) { adaptivePacket = queued; break; }
    }
    if (!adaptivePacket) {
      adaptive.finish(now, &adaptiveTx, false, 0);
      adaptiveTx = false;
      Serial.println("bot: reply queue tracking unavailable; outcome unknown; no automatic retry");
    }
  }
  memcpy(recipient, contact.id.pub_key, sizeof(recipient));
  awaitingAck = true;
  lastReply = now;
  ++replies;
#endif
}

ContactInfo* CommandBot::processAck(const uint8_t* data) {
  if (!awaitingAck || memcmp(data, &expectedAck, sizeof(expectedAck)) != 0) return nullptr;
  awaitingAck = false;
  if (companion) companion->confirmed(expectedAck, uint32_t(_ms->getMillis()) - lastReply);
  return lookupContactByPubKey(recipient, sizeof(recipient));
}

int CommandBot::companionSend(const ContactInfo& to, uint8_t type, uint8_t attempt, uint32_t timestamp,
                              const char* text, uint32_t& ack, uint32_t& timeout) {
  if (!transmitEnabled || awaitingAck) return MSG_SEND_FAILED;
  ack = 0;
  if (type == TXT_TYPE_CLI_DATA)
    return sendCommandData(to, getRTCClock()->getCurrentTimeUnique(), attempt, text, timeout);
  const int result = sendMessage(to, timestamp, attempt, text, ack, timeout);
  if (result != MSG_SEND_FAILED) {
    expectedAck = ack;
    memcpy(recipient, to.id.pub_key, sizeof(recipient));
    awaitingAck = true;
    lastReply = _ms->getMillis();
  }
  return result;
}

void CommandBot::onDiscoveredContact(ContactInfo& contact, bool isNew, uint8_t, const uint8_t*) {
  if (companion) companion->discovered(contact, isNew);
}

void CommandBot::onContactPathUpdated(const ContactInfo& contact) {
  if (companion) companion->pathUpdated(contact);
}

void CommandBot::onCommandDataRecv(const ContactInfo& contact, mesh::Packet* packet,
                                   uint32_t timestamp, const char* text) {
  if (companion) companion->received(contact, *packet, timestamp, TXT_TYPE_CLI_DATA, text);
}

void CommandBot::onContactResponse(const ContactInfo& contact, const uint8_t* data, uint8_t length) {
  if (companion) companion->response(contact, data, length);
}

void CommandBot::onSendTimeout() {
  awaitingAck = false;
  ++timeouts;
  Serial.println("bot: reply ACK timed out; no automatic retry");
}
void CommandBot::onContactsFull() {
  ++failures;
  Serial.println("bot: contact table full");
}
#if !NRFMAST_PRODUCTION_LUA
void CommandBot::logTx(mesh::Packet* packet, int) {
  if (packet == adaptivePacket) settleReply(true);
}
#endif
void CommandBot::logTxFail(mesh::Packet* packet, int) {
#if !NRFMAST_PRODUCTION_LUA
  if (packet == adaptivePacket) settleReply(false);
#endif
  ++failures;
  Serial.println("bot: radio transmit failed; inspect RF mode and shared-radio status; no automatic retry");
}
bool CommandBot::advertise(bool zeroHop) {
  startupAdvertPending = false;
  auto* packet = createSelfAdvert(name);
  if (!packet) {
    ++failures;
    Serial.println("bot: no packet slot for advertisement");
    return false;
  }
  if (zeroHop) sendZeroHop(packet);
  else sendFlood(packet, uint32_t(0), uint8_t(3));
  return true;
}

}  // namespace nrfmast
