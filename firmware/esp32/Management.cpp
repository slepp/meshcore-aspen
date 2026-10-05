// SPDX-License-Identifier: Apache-2.0
#include "Management.h"
#include "Config.h"
#include "RoleStorage.h"
#include "ReplyRouting.h"
#include "ServiceName.h"
#include "FirmwareIdentity.h"
#include <helpers/AdvertDataHelpers.h>
#include <helpers/SimpleMeshTables.h>
#if defined(MESHCORE_MAST_ADMIN) && MESHCORE_MAST_ADMIN
#include "MastWeb.h"
#include "BuildClock.h"
#include "repeater/OnchipRepeater.h"
#endif

namespace onchip {
namespace {
namespace management_pool {
#define ONCHIP_PACKET_CAPACITY 4
#include "support/PacketPool.h"
#include "support/PacketPool.inc"
#undef ONCHIP_PACKET_CAPACITY
}
constexpr uint8_t Domain[] = "MCORE-ROLE-RF-V1";
constexpr uint8_t ReceiptDomain[] = "MCORE-ROLE-RCPT-V1";
constexpr uint8_t QueryDomain[] = "MCORE-ROLE-QRY-V1";
constexpr uint8_t StatusDomain[] = "MCORE-ROLE-STAT-V1";
constexpr size_t SignedSize = sizeof(Domain) - 1 + 32 + 8 + 8 + 1;
constexpr size_t EnvelopeSize = SignedSize + SIGNATURE_SIZE;
constexpr size_t PaddedSize = (EnvelopeSize + 15) & ~size_t(15);
constexpr size_t ReceiptSignedSize = sizeof(ReceiptDomain) - 1 + 32 + 32 + 8 + 8 + 1;
constexpr size_t ReceiptSize = (ReceiptSignedSize + SIGNATURE_SIZE + 15) & ~size_t(15);
constexpr size_t QuerySignedSize = sizeof(QueryDomain) - 1 + 32 + 8;
constexpr size_t QueryEnvelopeSize = QuerySignedSize + SIGNATURE_SIZE;
constexpr size_t QuerySize = (QueryEnvelopeSize + 15) & ~size_t(15);
constexpr size_t StatusSignedSize = sizeof(StatusDomain) - 1 + 32 + 32 + 8 + 8 + 3;
constexpr size_t StatusSize = (StatusSignedSize + SIGNATURE_SIZE + 15) & ~size_t(15);
enum : uint8_t { Success = 1, AlreadyApplied = 2, Failure = 3 };
enum : uint8_t { StatusValid = 1, StatusSealed = 2 };
static_assert(SignedSize == 65 && PaddedSize == 144 &&
                  1 + 32 + CIPHER_MAC_SIZE + PaddedSize <= MAX_PACKET_PAYLOAD,
              "Signed management request exceeds one native anon datagram");
static_assert(ReceiptSignedSize == 99 && ReceiptSize == 176 &&
                  2 + CIPHER_MAC_SIZE + ReceiptSize <= MAX_PACKET_PAYLOAD &&
                  2 + MAX_PATH_SIZE + 2 + CIPHER_MAC_SIZE + ReceiptSize <=
                      MAX_TRANS_UNIT,
              "Signed receipt exceeds one native response datagram");
static_assert(QuerySignedSize == 57 && QuerySize == 128 &&
                  1 + 32 + CIPHER_MAC_SIZE + QuerySize <= MAX_PACKET_PAYLOAD &&
                  StatusSignedSize == 101 && StatusSize == 176 &&
                  2 + CIPHER_MAC_SIZE + StatusSize <= MAX_PACKET_PAYLOAD &&
                  2 + MAX_PATH_SIZE + 2 + CIPHER_MAC_SIZE + StatusSize <=
                      MAX_TRANS_UNIT,
              "Signed status exchange exceeds one native datagram");
uint64_t read64(const uint8_t *bytes) {
  uint64_t result = 0;
  for (unsigned i = 0; i < 8; ++i)
    result |= uint64_t(bytes[i]) << (i * 8);
  return result;
}
void write64(uint8_t *bytes, uint64_t value) {
  for (unsigned i = 0; i < 8; ++i)
    bytes[i] = uint8_t(value >> (i * 8));
}
int nibble(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}
} // namespace

struct Management::Core : mesh::Mesh {
  struct Tables : SimpleMeshTables {
    bool wasSeen(const mesh::Packet *packet) override {
      return packet->getPayloadType() == PAYLOAD_TYPE_TXT_MSG ? false :
             SimpleMeshTables::wasSeen(packet);
    }
  } tables;
  management_pool::StaticPoolPacketManager packets{4};
  Management &owner;
  ReplyRouting routing;
  uint8_t originWidth = 1;
#if defined(MESHCORE_MAST_ADMIN) && MESHCORE_MAST_ADMIN
  MastAdmin admin;
  uint32_t lastMillis = millis();
  uint64_t uptimeMillis = lastMillis;
  struct Session {
    mesh::Identity id;
    uint8_t secret[32]{}, requestHash[16]{};
    PeerRoute route;
    bool used = false;
    uint32_t at = 0, timestamp = 0;
    uint32_t loginAt = 0;
    MastAdmin::Reply reply;
  } sessions[5];
  uint8_t matches[5]{};
  mesh::Packet *effectPacket = nullptr;
  uint32_t effectTicket = 0;
  uint32_t ownerQueryWindow = millis();
  uint8_t ownerQueryCount = 0;
  mesh::DispatcherAction onRecvPacket(mesh::Packet *p) override {
    if (p->getPayloadVer() != PAYLOAD_VER_1 ||
        !mesh::Packet::isValidPathLen(p->path_len))
      return ACTION_RELEASE;
    const auto type = p->getPayloadType();
    if (type == PAYLOAD_TYPE_ANON_REQ) {
      if (p->payload_len < 51 || (p->payload_len - 35) % 16) return ACTION_RELEASE;
    } else if (type == PAYLOAD_TYPE_TXT_MSG || type == PAYLOAD_TYPE_PATH ||
               type == PAYLOAD_TYPE_REQ) {
      if (p->payload_len < 20 || (p->payload_len - 4) % 16) return ACTION_RELEASE;
    } else return ACTION_RELEASE;
    return Mesh::onRecvPacket(p);
  }
  void sendTo(Session &session, mesh::Packet *packet) {
    routing.send(*this, packet, session.route, originWidth);
  }
  void publicOwnerRequest(mesh::Packet *packet, const uint8_t *secret,
                          const mesh::Identity &sender, const uint8_t *data, size_t length) {
    if (!packet->isRouteDirect() || length < 6) {
      Serial.println("Management public owner request requires a direct route and return path"); return;
    }
    const uint8_t pathLength = data[5];
    const size_t pathBytes = size_t(pathLength & 63) * ((pathLength >> 6) + 1);
    if (!mesh::Packet::isValidPathLen(pathLength) || length < 6 + pathBytes) {
      Serial.println("Management public owner request has an invalid return path"); return;
    }
    const uint32_t now = millis();
    if (uint32_t(now - ownerQueryWindow) >= 180000) {
      ownerQueryWindow = now;
      ownerQueryCount = 0;
    }
    if (ownerQueryCount >= 4) {
      Serial.println("Management public owner request rate limit reached; retry after three minutes"); return;
    }
    ++ownerQueryCount;
    char info[MastAdmin::OwnerInfoLimit + 1];
    if (!admin.ownerInfo(info, sizeof(info))) return;
    uint8_t body[MAX_PACKET_PAYLOAD - CIPHER_MAC_SIZE - (CIPHER_BLOCK_SIZE - 1)]{};
    queued_tx::put32(body, queued_tx::get32(data));
    queued_tx::put32(body + 4, getRTCClock()->getCurrentTime());
    const int textLength = snprintf(reinterpret_cast<char *>(body + 8), sizeof(body) - 8,
                                    "%s\n%s", owner.name(), info);
    if (textLength < 0 || size_t(textLength) >= sizeof(body) - 8) {
      Serial.println("Management public owner information exceeds reply capacity"); return;
    }
    auto *response = createDatagram(PAYLOAD_TYPE_RESPONSE, sender, secret, body, 8 + textLength);
    if (!response) {
      Serial.println("Management public owner response packet unavailable"); return;
    }
    sendDirect(response, data + 6, pathLength, 300);
  }
  void maintainSessions() {
    for (auto &session : sessions) {
      if (!session.used) continue;
      if (uint32_t(millis() - session.at) >= 900000 || !admin.lastTimestamp(session.id.pub_key)) {
        session = {};
      } else if (admin.cancelled(session.reply.ticket)) {
        char *text = session.reply.text;
        const auto *separator = strchr(text, '|');
        if (separator && (separator - text == 2 || separator - text == 16))
          text += separator - text + 1;
        strcpy(text, "Error: acceptance failed or timed out; deferred effect cancelled");
        session.reply.ticket = 0;
      }
    }
  }
  bool onPeerPathRecv(mesh::Packet *packet, int index, const uint8_t *,
                      uint8_t *path, uint8_t length, uint8_t, uint8_t *, uint8_t) override {
    if (packet->_localReflection || !mesh::Packet::isValidPathLen(length) ||
        2u + (length & 63u) * ((length >> 6) + 1u) > unsigned(packet->payload_len - 4))
      return false;
    auto &session = sessions[matches[index]];
    session.route.learn(path, length);
    session.route.scoped = routing.scope(packet);
    return false;
  }
  bool login(mesh::Packet *packet, const uint8_t *secret,
             const mesh::Identity &sender, const uint8_t *data, size_t length) {
    if (length < 5 || length > 32) return false;
    if (!admin.ready()) return true;
    maintainSessions();
    const uint32_t now = millis();
    const auto *end = static_cast<const uint8_t *>(memchr(data + 4, 0, length - 4));
    const size_t passwordSize = end ? size_t(end - data - 4) : length - 4;
    if (passwordSize > 15) return true;
    if (end) for (const auto *p = end; p < data + length; ++p) if (*p) return true;
    char password[16]{};
    memcpy(password, data + 4, passwordSize);
    const bool authorized = (data[4] == 0 && admin.trusted(sender.pub_key)) ||
                            MastAdmin::passwordMatches(password);
    auto *passwordBytes = reinterpret_cast<volatile uint8_t *>(password);
    for (size_t i = 0; i < sizeof(password); ++i) passwordBytes[i] = 0;
    if (!authorized) {
      Serial.println("Mast login denied"); return true;
    }
    Session *slot = nullptr;
    for (auto &session : sessions)
      if (session.used && session.id.matches(sender)) { slot = &session; break; }
    if (slot && uint32_t(now - slot->loginAt) < 1000) return true;
    if (!slot && admin.compiledTrusted(sender.pub_key)) slot = &sessions[0];
    for (unsigned i = 1; !slot && i < 5; ++i)
      if (!sessions[i].used) slot = &sessions[i];
    const uint32_t timestamp = queued_tx::get32(data);
    if (!slot || !admin.rememberTimestamp(sender.pub_key, timestamp)) {
      Serial.println("Mast login replay/storage/session admission rejected"); return true;
    }
    const PeerRoute known = slot->used && slot->id.matches(sender) ? slot->route : PeerRoute{};
    *slot = {};
    slot->id = sender; slot->used = true; slot->at = now;
    slot->loginAt = now;
    slot->timestamp = timestamp;
    memcpy(slot->secret, secret, 32);
    slot->route = known;
    slot->route.scoped = routing.scope(packet);
    uint8_t response[13]{};
    queued_tx::put32(response, getRTCClock()->getCurrentTimeUnique());
    response[6] = 1; response[7] = 3;
    getRNG()->random(response + 8, 4);
    response[12] = 2; // Pinned native repeater login response revision.
    const auto route = mesh::chooseReplyRoute(packet->isRouteFlood(), false, slot->route.length != 0xff);
    auto *reply = route == mesh::REPLY_ROUTE_PATH_RETURN
        ? createPathReturn(sender, secret, packet->path, packet->path_len,
                           PAYLOAD_TYPE_RESPONSE, response, sizeof(response))
        : createDatagram(PAYLOAD_TYPE_RESPONSE, sender, secret, response, sizeof(response));
    if (reply) {
      if (route == mesh::REPLY_ROUTE_PATH_RETURN)
        routing.flood(*this, reply, slot->route.scoped, packet->getPathHashSize());
      else sendTo(*slot, reply);
    } else Serial.println("Mast login response allocation failed");
    return true;
  }
  int searchPeersByHash(const uint8_t *hash) override {
    if (!admin.ready()) return 0;
    maintainSessions();
    unsigned count = 0;
    for (unsigned i = 0; i < 5; ++i)
      if (sessions[i].used && uint32_t(millis() - sessions[i].at) < 900000 &&
          sessions[i].id.isHashMatch(hash)) matches[count++] = i;
    return count;
  }
  void getPeerSharedSecret(uint8_t *secret, int index) override {
    memcpy(secret, sessions[matches[index]].secret, 32);
  }
  void nativeRequest(mesh::Packet *packet, Session &session, const uint8_t *data, size_t size) {
    if (size < 6 || (data[4] != 1 && data[4] != 3 && data[4] != 7)) {
      Serial.println("Management native request unsupported; use status, telemetry or owner information"); return;
    }
    const uint32_t timestamp = queued_tx::get32(data);
    if (!admin.rememberTimestamp(session.id.pub_key, timestamp)) {
      Serial.println("Mast native request replay/storage admission rejected"); return;
    }
    session.at = millis();
    session.route.scoped = routing.scope(packet);
    uint8_t body[MAX_PACKET_PAYLOAD - 4]{};
    queued_tx::put32(body, timestamp);
    const size_t responseCapacity = packet->isRouteFlood() ?
        MAX_PACKET_PAYLOAD - 2 - CIPHER_BLOCK_SIZE - 5 - packet->getPathByteLen() :
        MAX_PACKET_PAYLOAD - CIPHER_MAC_SIZE - (CIPHER_BLOCK_SIZE - 1);
    size_t length;
    if (data[4] == 1) {
      RepeaterStats stats{};
      static_assert(sizeof(stats) == 56, "Pinned native status layout changed");
      stats.batt_milli_volts = board.getBattMilliVolts();
      stats.curr_tx_queue_len = owner.radio_.queuedCount();
      stats.noise_floor = owner.radio_.getNoiseFloor();
      stats.last_rssi = nativeRSSI(owner.radio_);
      stats.n_packets_recv = owner.radio_.getPacketsRecv();
      stats.n_packets_sent = owner.radio_.getPacketsSent();
      stats.total_air_time_secs = getTotalAirTimeSeconds();
      stats.total_up_time_secs = uptimeMillis / 1000;
      stats.n_sent_flood = getNumSentFlood();
      stats.n_sent_direct = getNumSentDirect();
      stats.n_recv_flood = getNumRecvFlood();
      stats.n_recv_direct = getNumRecvDirect();
      stats.err_events = _err_flags;
      stats.last_snr = nativeSNR(owner.radio_) * 4;
      stats.n_direct_dups = tables.getNumDirectDups();
      stats.n_flood_dups = tables.getNumFloodDups();
      stats.total_rx_air_time_secs = getReceiveAirTime() / 1000;
      stats.n_recv_errors = owner.radio_.getPacketsRecvErrors();
      memcpy(body + 4, &stats, sizeof(stats));
      length = 4 + sizeof(stats);
    } else if (data[4] == 7) {
      char info[MastAdmin::OwnerInfoLimit + 1];
      if (!admin.ownerInfo(info, sizeof(info))) return;
      const size_t capacity = responseCapacity - 4 + 1;
      const int needed = snprintf(reinterpret_cast<char *>(body + 4), capacity,
                                  "%s\n%s\n%s", ONCHIP_FIRMWARE_VERSION, owner.name(), info);
      if (needed < 0) {
        Serial.println("Management owner information encoding failed"); return;
      }
      if (size_t(needed) >= capacity)
        Serial.println("Management owner information shortened to fit reply; use get owner.info for full text");
      length = 4 + strlen(reinterpret_cast<const char *>(body + 4));
    } else {
      const size_t capacity = responseCapacity - 4;
      CayenneLPP telemetry(capacity);
      if (!telemetry.getBuffer()) {
        Serial.println("Mast native telemetry allocation failed"); return;
      }
      const auto battery = board.getBattMilliVolts();
      if (battery) telemetry.addVoltage(TELEM_CHANNEL_SELF, battery / 1000.0f);
      sensors.querySensors(uint8_t(~data[5]), telemetry);
      const float temperature = board.getMCUTemperature();
      if (std::isfinite(temperature)) telemetry.addTemperature(TELEM_CHANNEL_SELF, temperature);
      if (telemetry.getError() != LPP_ERROR_OK) {
        Serial.println("Mast native telemetry exceeds reply capacity"); return;
      }
      length = 4 + telemetry.getSize();
      memcpy(body + 4, telemetry.getBuffer(), telemetry.getSize());
    }
    auto *response = packet->isRouteFlood()
        ? createPathReturn(session.id, session.secret, packet->path, packet->path_len,
                           PAYLOAD_TYPE_RESPONSE, body, length)
        : createDatagram(PAYLOAD_TYPE_RESPONSE, session.id, session.secret, body, length);
    if (!response) {
      Serial.println("Management native response packet unavailable"); return;
    }
    if (packet->isRouteFlood())
      routing.flood(*this, response, session.route.scoped, packet->getPathHashSize(), 300);
    else routing.send(*this, response, session.route, originWidth, 300);
  }
  void onPeerDataRecv(mesh::Packet *packet, uint8_t type, int index,
                      const uint8_t *, uint8_t *data, size_t size) override {
    struct ClearPlaintext {
      uint8_t *data;
      size_t size;
      ~ClearPlaintext() {
        auto *secret = reinterpret_cast<volatile uint8_t *>(data);
        for (size_t i = 0; i < size; ++i) secret[i] = 0;
      }
    } clear{data, size};
    if (type == PAYLOAD_TYPE_REQ) {
      nativeRequest(packet, sessions[matches[index]], data, size); return;
    }
    if (type != PAYLOAD_TYPE_TXT_MSG || size < 6 || (data[4] >> 2) > 1) return;
    auto &session = sessions[matches[index]];
    const auto *end = static_cast<const uint8_t *>(memchr(data + 5, 0, size - 5));
    const size_t length = end ? size_t(end - data - 5) : size - 5;
    if (!length || length > MastAdmin::TextLimit) return;
    if (end) for (auto *p = end; p < data + size; ++p) if (*p) return;
    const uint32_t timestamp = queued_tx::get32(data);
    uint8_t hash[16];
    mesh::Utils::sha256(hash, sizeof(hash), data + 5, length);
    MastAdmin::Reply result;
    if (timestamp == session.timestamp && !memcmp(hash, session.requestHash, sizeof(hash))) {
      result = session.reply;
      result.ticket = 0;
    } else {
      if (!admin.rememberTimestamp(session.id.pub_key, timestamp)) {
        Serial.println("Mast CLI replay/storage admission rejected"); return;
      }
      char command[MastAdmin::TextLimit + 1];
      memcpy(command, data + 5, length); command[length] = 0;
      const char *separator = strchr(command, '|');
      const size_t tagSize = separator ? size_t(separator - command) : 0;
      size_t prefix = tagSize == 2 || tagSize == 16 ? tagSize + 1 : 0;
      for (size_t i = 0; prefix && i < tagSize; ++i)
        if (nibble(command[i]) < 0) prefix = 0;
      admin.execute(command + prefix, result, 0, MastAdmin::Transport::NativeEncrypted,
                    session.id.pub_key);
      if (prefix) {
        if (strlen(result.text) > MastAdmin::TextLimit - prefix) {
          admin.acknowledged(result.ticket, false);
          result.ticket = 0;
          strcpy(result.text, "Error: response needs an untagged CLI request");
        }
        memmove(result.text + prefix, result.text, strlen(result.text) + 1);
        memcpy(result.text, command, prefix);
      }
      auto *secret = reinterpret_cast<volatile uint8_t *>(command);
      for (size_t i = 0; i < sizeof(command); ++i) secret[i] = 0;
      session.timestamp = timestamp;
      memcpy(session.requestHash, hash, sizeof(hash));
      session.reply = result;
    }
    session.at = millis();
    session.route.scoped = routing.scope(packet);
    if ((data[4] >> 2) == 0) {
      uint32_t ack;
      mesh::Utils::sha256(reinterpret_cast<uint8_t *>(&ack), 4, data, length + 5,
                          session.id.pub_key, 32);
      auto *response = packet->isRouteFlood()
          ? createPathReturn(session.id, session.secret, packet->path, packet->path_len,
                             PAYLOAD_TYPE_ACK, reinterpret_cast<uint8_t *>(&ack), 4)
          : createAck(ack);
      if (response) {
        if (packet->isRouteFlood())
          routing.flood(*this, response, session.route.scoped, packet->getPathHashSize());
        else sendTo(session, response);
      }
    }
    uint8_t text[5 + MastAdmin::TextLimit]{};
    uint32_t stamp = getRTCClock()->getCurrentTimeUnique();
    if (stamp == timestamp) ++stamp;
    queued_tx::put32(text, stamp); text[4] = 4;
    const size_t replyLength = strlen(result.text);
    memcpy(text + 5, result.text, replyLength);
    auto *response = createDatagram(PAYLOAD_TYPE_TXT_MSG, session.id, session.secret,
                                    text, 5 + replyLength);
    if (!response) {
      admin.acknowledged(result.ticket, false);
      Serial.println("Mast CLI response packet unavailable"); return;
    }
    if (result.ticket) { effectPacket = response; effectTicket = result.ticket; }
    sendTo(session, response);
  }
  void logTx(mesh::Packet *packet, int) override {
    if (packet == effectPacket) {
      admin.acknowledged(effectTicket, true); effectPacket = nullptr;
    }
  }
  void logTxFail(mesh::Packet *packet, int) override {
    if (packet == effectPacket) {
      admin.acknowledged(effectTicket, false); effectPacket = nullptr;
    }
  }
#endif
  Core(Management &management)
      : Mesh(management.radio_, management.clock_, management.rng_,
             management.rtc_, packets, tables),
        owner(management) {}
  void onAnonDataRecv(mesh::Packet *packet, const uint8_t *secret,
                      const mesh::Identity &sender, uint8_t *data,
                      size_t length) override {
#if defined(MESHCORE_MAST_ADMIN) && MESHCORE_MAST_ADMIN
    if (packet->isRouteDirect() && packet->getPathHashCount() == 0) {
      uint8_t reply[32];
      if (networkTimeReply(data, length, reply)) {
        static uint32_t lastReply = 0;
        static bool sent = false;
        const uint32_t now = millis();
        if ((sent && uint32_t(now - lastReply) < 10000u) ||
            packets.getOutboundTotal() || _radio->getEstAirtimeFor(80) > 1000u) return;
        sent = true; lastReply = now;
        sendEncrypted(packet, secret, sender, reply, sizeof(reply));
        return;
      }
    }
    if (length >= 5 && data[4] == 2) {
      publicOwnerRequest(packet, secret, sender, data, length); return;
    }
    if (login(packet, secret, sender, data, length)) return;
#endif
    owner.receive(packet, secret, sender, data, length);
  }
  bool allowPacketForward(const mesh::Packet *) override { return false; }
  void sendEncrypted(mesh::Packet *request, const uint8_t *secret,
                     const mesh::Identity &sender, const uint8_t *body,
                     size_t length) {
    if (!owner.radio_.queuedReady() || owner.radio_.hasPendingWork() ||
        packets.getOutboundTotal()) {
      Serial.println("On-chip RF management response skipped: radio or queue unavailable");
      return;
    }
    auto *response = obtainNewPacket();
    if (!response) {
      Serial.println("On-chip RF management response packet unavailable");
      return;
    }
    response->header = PAYLOAD_TYPE_RESPONSE << PH_TYPE_SHIFT;
    response->payload[0] = sender.pub_key[0];
    response->payload[1] = self_id.pub_key[0];
    response->payload_len = 2 + mesh::Utils::encryptThenMAC(
        secret, response->payload + 2, body, length);
    if (request->isRouteFlood())
      routing.flood(*this, response, routing.scope(request), request->getPathHashSize());
    else if (request->isRouteDirect() &&
             request->getPathHashCount() == 0)
      sendZeroHop(response);
    else
      releasePacket(response);
  }
  void receipt(mesh::Packet *request, const uint8_t *secret,
               const mesh::Identity &sender, uint64_t generation,
               uint64_t nonce, uint8_t result) {
    uint8_t body[ReceiptSize]{};
    size_t offset = 0;
    memcpy(body + offset, ReceiptDomain, sizeof(ReceiptDomain) - 1);
    offset += sizeof(ReceiptDomain) - 1;
    memcpy(body + offset, self_id.pub_key, 32);
    offset += 32;
    memcpy(body + offset, sender.pub_key, 32);
    offset += 32;
    write64(body + offset, generation);
    offset += 8;
    write64(body + offset, nonce);
    offset += 8;
    body[offset] = result;
    self_id.sign(body + ReceiptSignedSize, body, ReceiptSignedSize);
    sendEncrypted(request, secret, sender, body, sizeof(body));
  }
  void status(mesh::Packet *request, const uint8_t *secret,
              const mesh::Identity &sender, uint64_t nonce) {
    ProfileJournal current;
    const bool valid = loadProfileJournal(current);
    if (valid)
      owner.journal_ = current;
    uint8_t body[StatusSize]{};
    size_t offset = 0;
    memcpy(body + offset, StatusDomain, sizeof(StatusDomain) - 1);
    offset += sizeof(StatusDomain) - 1;
    memcpy(body + offset, self_id.pub_key, 32);
    offset += 32;
    memcpy(body + offset, sender.pub_key, 32);
    offset += 32;
    write64(body + offset, nonce);
    offset += 8;
    write64(body + offset, valid ? current.generation : 0);
    offset += 8;
    body[offset++] = valid ? current.profile.enabled : 0;
    body[offset++] = valid ? owner.appliedMask_ : 0;
    body[offset] = (valid ? StatusValid : 0) |
                   (owner.sealed_ ? StatusSealed : 0);
    self_id.sign(body + StatusSignedSize, body, StatusSignedSize);
    sendEncrypted(request, secret, sender, body, sizeof(body));
  }
};

bool Management::begin(WifiKissMultiplexer &mux,
                       const char *operatorPublicKeyHex) {
  if (core_)
    return false;
  if (!loadServiceName(NamedService::Management, status_.name))
    return false;
  journal_ = {};
  if (!loadProfileJournal(journal_))
    return false;
  appliedMask_ = journal_.profile.enabled;
  provisioned_ = operatorPublicKeyHex && operatorPublicKeyHex[0];
  if (provisioned_) {
    if (strlen(operatorPublicKeyHex) != 64) {
      Serial.println("On-chip operator verification key must be 64 hex digits");
      return false;
    }
    for (unsigned i = 0; i < 32; ++i) {
      const int high = nibble(operatorPublicKeyHex[i * 2]);
      const int low = nibble(operatorPublicKeyHex[i * 2 + 1]);
      if (high < 0 || low < 0) {
        Serial.println("On-chip operator verification key is not hex");
        return false;
      }
      operatorKey_[i] = (high << 4) | low;
    }
  }
  if (!radio_.attach(mux)) {
    Serial.println("On-chip management source unavailable");
    return false;
  }
  core_ = allocateRoleStorage<Core>("management", *this);
  if (!core_) {
    radio_.detach();
    return false;
  }
  if (!loadOriginPathWidth(core_->originWidth)) {
    stop();
    return false;
  }
  mesh::LocalIdentity id;
  if (!loadIdentity("management", id)) {
    stop();
    return false;
  }
  core_->self_id = id;
#if defined(MESHCORE_MAST_ADMIN) && MESHCORE_MAST_ADMIN
  rtc_.synchronize(ONCHIP_CLOCK_BUILD_EPOCH);
  if (!core_->admin.begin(mux, appliedMask_, journal_, *this))
    Serial.println("Native mast administration failed closed; signed management and shared roles retained");
#endif
  auto *secret = reinterpret_cast<volatile uint8_t *>(&id);
  for (size_t i = 0; i < sizeof(id); ++i)
    secret[i] = 0;
  core_->begin();
  strcpy(status_.role, "management");
  memcpy(status_.public_key, core_->self_id.pub_key, 32);
  status_.has_identity = true;
#if defined(MESHCORE_MAST_ADMIN) && MESHCORE_MAST_ADMIN
  Serial.print("Mast management public key: ");
  for (unsigned i = 0; i < 32; ++i) Serial.printf("%02x", core_->self_id.pub_key[i]);
  Serial.println();
#endif
  if (!provisioned_)
    Serial.println("On-chip RF management unprovisioned (no operator verification key)");
  return true;
}

bool Management::setName(const char *name) {
  if (!core_ || !saveServiceName(NamedService::Management, name)) return false;
  strcpy(status_.name, name);
  return true;
}

bool Management::advertiseZeroHop() {
  if (!core_ || !radio_.queuedReady() || radio_.hasPendingWork() ||
      core_->packets.getOutboundTotal()) {
    Serial.println("Management advert unavailable: radio or queue busy");
    return false;
  }
  uint8_t data[MAX_ADVERT_DATA_SIZE]{};
  AdvertDataBuilder builder(ADV_TYPE_REPEATER, status_.name);
  auto *packet = core_->createAdvert(core_->self_id, data, builder.encodeTo(data));
  if (!packet) {
    Serial.println("Management advert packet capacity exhausted");
    return false;
  }
  core_->sendZeroHop(packet);
  return true;
}

bool Management::acceptVerifiedRequest() {
  const uint32_t now = millis();
  if (lastVerification_ && uint32_t(now - lastVerification_) < 1000)
    return false;
  lastVerification_ = now ? now : 1;
  return true;
}

void Management::receive(mesh::Packet *packet, const uint8_t *secret,
                         const mesh::Identity &sender, const uint8_t *data,
                         size_t length) {
  if (!provisioned_ ||
      !(packet->isRouteFlood() ||
        (packet->isRouteDirect() &&
         packet->getPathHashCount() == 0)))
    return;
  const bool query = length == QuerySize &&
                     !memcmp(data, QueryDomain, sizeof(QueryDomain) - 1);
  if (query) {
    if (memcmp(data + sizeof(QueryDomain) - 1, core_->self_id.pub_key, 32))
      return;
    for (size_t i = QueryEnvelopeSize; i < QuerySize; ++i)
      if (data[i])
        return;
    const uint64_t nonce = read64(data + QuerySignedSize - 8);
    if (!nonce ||
        !mesh::Identity(operatorKey_).verify(
            data + QuerySignedSize, data, QuerySignedSize))
      return;
    if (!acceptVerifiedRequest())
      return;
    core_->status(packet, secret, sender, nonce);
    return;
  }
  if (sealed_ || length != PaddedSize ||
      memcmp(data, Domain, sizeof(Domain) - 1) ||
      memcmp(data + sizeof(Domain) - 1, core_->self_id.pub_key, 32))
    return;
  for (size_t i = EnvelopeSize; i < PaddedSize; ++i)
    if (data[i])
      return;
  const uint64_t generation = read64(data + 48);
  const uint64_t nonce = read64(data + 56);
  const RoleProfile requested{data[64]};
  if (!requested.bootableWith(ONCHIP_ADMIN_PASSWORD, ONCHIP_ROOM_PASSWORD) ||
      !nonce || !generation)
    return;
  if (!mesh::Identity(operatorKey_).verify(data + SignedSize, data, SignedSize))
    return;
  if (!acceptVerifiedRequest())
    return;
#if defined(MESHCORE_MAST_ADMIN) && MESHCORE_MAST_ADMIN
  ProfileJournal current;
  if (!loadProfileJournal(current)) {
    sealed_ = true;
    core_->receipt(packet, secret, sender, generation, nonce, Failure);
    return;
  }
  journal_ = current;
#endif
  if (generation <= journal_.generation) {
    const bool repeat = generation == journal_.generation &&
                        nonce == journal_.nonce &&
                        requested.enabled == journal_.profile.enabled;
    core_->receipt(packet, secret, sender, generation, nonce,
                   repeat ? AlreadyApplied : Failure);
    return;
  }
  const ProfileJournal next{requested, generation, nonce};
  if (!commitProfileJournal(next)) {
    sealed_ = true;
    Serial.println("On-chip RF management NVS commit failed; refusing updates until reboot");
    core_->receipt(packet, secret, sender, generation, nonce, Failure);
    return;
  }
  journal_ = next;
  Serial.printf("On-chip signed role profile generation %llu saved for next boot\n",
                static_cast<unsigned long long>(generation));
  core_->receipt(packet, secret, sender, generation, nonce, Success);
}

void Management::loop() {
  if (core_) {
#if defined(MESHCORE_MAST_ADMIN) && MESHCORE_MAST_ADMIN
    const uint32_t now = millis();
    core_->uptimeMillis += uint32_t(now - core_->lastMillis);
    core_->lastMillis = now;
    ClockSnapshot clocks;
    if (clockSnapshot(clocks) && (clocks.network_epoch || clocks.gps_epoch)) {
      const bool gps = clocks.gps_epoch && clocks.gps_age_ms < 3600000u;
      const uint64_t epoch = gps ? uint64_t(clocks.gps_epoch) + clocks.gps_age_ms / 1000 :
                                  uint64_t(clocks.network_epoch) + clocks.network_age_seconds;
      if (epoch <= 4102444800u)
        rtc_.synchronize(static_cast<uint32_t>(epoch), gps ? ClockSource::Gps : ClockSource::Network);
    }
    rtc_.tick();
#endif
    core_->loop();
#if defined(MESHCORE_MAST_ADMIN) && MESHCORE_MAST_ADMIN
    serviceMastWeb(core_->admin);
    core_->admin.loop();
    core_->maintainSessions();
#endif
  }
}
#if defined(MESHCORE_MAST_ADMIN) && MESHCORE_MAST_ADMIN
MastAdmin &Management::admin() { return core_->admin; }
bool Management::authenticatedNativeSender(const uint8_t *key) const {
  if (!core_ || !key || !core_->admin.ready() || !core_->admin.lastTimestamp(key)) return false;
  for (const auto &session : core_->sessions)
    if (session.used && !memcmp(session.id.pub_key, key, 32) &&
        uint32_t(millis() - session.at) < 900000) return true;
  return false;
}
#endif
void Management::stop() {
#if defined(MESHCORE_MAST_ADMIN) && MESHCORE_MAST_ADMIN
  if (core_) core_->admin.stop();
#endif
  radio_.detach();
  releaseRoleStorage(core_);
  status_ = {};
  appliedMask_ = 0;
  provisioned_ = sealed_ = false;
  lastVerification_ = 0;
}
const uint8_t *Management::publicKey() const {
  return core_ ? core_->self_id.pub_key : nullptr;
}
uint8_t Management::pathWidth() const {
  return core_ ? core_->originWidth : 0;
}
bool Management::setPathWidth(uint8_t width) {
  if (!core_ || !saveOriginPathWidth(width)) return false;
  core_->originWidth = width;
  return true;
}
void Management::dashboardStatus(RadioDashboard::RoleStatus &status) const {
  status = status_;
  status.profile_generation = journal_.generation;
  mesh::QueuedRadioStats stats{};
  if (core_ && radio_.getQueuedRadioStats(stats)) {
    status.source_slot = radio_.sourceSlot();
    status.source_generation = stats.generation;
  }
  status.ready = core_ && provisioned_ && !sealed_ && radio_.queuedReady();
  strcpy(status.state, !core_        ? "fault"
                       : sealed_     ? "fault"
                       : !provisioned_ ? "unprovisioned"
                       : !radio_.queuedReady() ? "waiting-radio"
                                               : "running");
#if defined(MESHCORE_MAST_ADMIN) && MESHCORE_MAST_ADMIN
  if (core_ && !sealed_ && core_->admin.ready() && radio_.queuedReady() &&
      (ONCHIP_MAST_PASSWORD[0] || ONCHIP_TRUSTED_COMPANION_PUBKEY[0])) {
    status.ready = true;
    strcpy(status.state, "running");
  }
  if (core_ && !core_->admin.ready()) {
    status.ready = false;
    strcpy(status.state, "admin-fault");
    strcpy(status.fault, "Mast replay invalid; shared roles retained");
  }
#endif
  if (sealed_)
    strcpy(status.fault, "Profile commit failed; reboot required");
  else if (!core_)
    strcpy(status.fault, "Management source unavailable");
}
} // namespace onchip
