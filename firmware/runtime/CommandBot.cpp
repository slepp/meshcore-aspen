// SPDX-License-Identifier: Apache-2.0
#if defined(MESHCORE_ONCHIP_BOT) && MESHCORE_ONCHIP_BOT
#include "CommandBot.h"
#include "BotSignal.h"
#include "BotHttpsProbe.h"
#include "BotRegistry.h"
#include "AdaptiveAdmission.h"
#include "Config.h"
#include "BuildClock.h"
#include "RoleStorage.h"
#include "ReplyRouting.h"
#include "MastAdmin.h"
#include <helpers/AdvertDataHelpers.h>
#include <helpers/SimpleMeshTables.h>
#include <helpers/SensorManager.h>
#include <cmath>
#include <algorithm>
#include <cstdarg>
#include <cassert>
#if !defined(NRF52_PLATFORM) && !(defined(MESHCORE_HOST_BOT_SOURCE) && MESHCORE_HOST_BOT_SOURCE)
#include "RoleProfile.h"
#endif
#if defined(MESHCORE_HOST_BOT_SOURCE) && MESHCORE_HOST_BOT_SOURCE
#include <ctime>
#endif
#ifdef ARDUINO_ARCH_ESP32
#include <WiFi.h>
#endif
#if defined(ARDUINO_ARCH_ESP32) || defined(ONCHIP_BOT_RUNTIME_TEST) || defined(NRF52_PLATFORM)
#include <target.h>
#endif

namespace onchip {
namespace {
CommandBot *instance;
constexpr unsigned BotPacketCapacity = 4;
constexpr char AdaptivePolicyFault[] = "Adaptive policy unreadable; bot adaptive off";
static_assert(sizeof(AdaptivePolicyFault) <= sizeof(RadioDashboard::RoleStatus::fault),
              "Adaptive policy fault must fit dashboard status");
constexpr uint8_t NativeTelemetryRequest = 0x03;
namespace bot_pool {
#define ONCHIP_PACKET_CAPACITY BotPacketCapacity
#include "support/PacketPool.h"
#include "support/PacketPool.inc"
#undef ONCHIP_PACKET_CAPACITY
}
struct BotPackets : bot_pool::StaticPoolPacketManager {
#if defined(MESHCORE_HOST_BOT_SOURCE) && MESHCORE_HOST_BOT_SOURCE
  struct Scheduled {
    const mesh::Packet *packet = nullptr;
    uint32_t due = 0;
  } scheduled[BotPacketCapacity]{};
  void queueInbound(mesh::Packet *packet, uint32_t due) override {
    Scheduled *slot = nullptr;
    for (auto &entry : scheduled) if (!entry.packet) { slot = &entry; break; }
    assert(slot);
    *slot = {packet, due};
    StaticPoolPacketManager::queueInbound(packet, due);
  }
  mesh::Packet *getNextInbound(uint32_t now) override {
    auto *packet = StaticPoolPacketManager::getNextInbound(now);
    for (auto &entry : scheduled) if (entry.packet == packet) entry = {};
    return packet;
  }
  uint32_t nextInboundWait(uint32_t now) const {
    uint32_t wait = UINT32_MAX;
    for (const auto &entry : scheduled) if (entry.packet) botEarlier(wait, now, entry.due);
    return wait;
  }
#endif
  AdaptiveAdmission *admission = nullptr;
  struct Reception {
    const mesh::Packet *packet = nullptr;
    float rssi = 0, snr = 0;
  } receptions[BotPacketCapacity]{};
  BotPackets() : StaticPoolPacketManager(BotPacketCapacity) {}
  bool capture(const mesh::Packet *packet, float rssi, float snr) {
    for (auto &entry : receptions)
      if (!entry.packet) {
        entry = {packet, rssi, snr};
        return true;
      }
    return false;
  }
  const Reception *reception(const mesh::Packet *packet) const {
    for (const auto &entry : receptions)
      if (entry.packet == packet) return &entry;
    return nullptr;
  }
  void free(mesh::Packet *packet) override {
#if defined(MESHCORE_HOST_BOT_SOURCE) && MESHCORE_HOST_BOT_SOURCE
    for (auto &entry : scheduled) if (entry.packet == packet) entry = {};
#endif
    if (admission) admission->released(millis(), packet);
    for (auto &entry : receptions)
      if (entry.packet == packet) entry = {};
    StaticPoolPacketManager::free(packet);
  }
};
struct Tables : SimpleMeshTables {
  bool wasSeen(const mesh::Packet *packet) override {
    // Multi-test needs copies arriving along different paths. Authentication
    // and bounded request-level dedup still run for every addressed text copy.
    return packet->getPayloadType() == PAYLOAD_TYPE_TXT_MSG ||
                   packet->getPayloadType() == PAYLOAD_TYPE_GRP_TXT
               ? false : SimpleMeshTables::wasSeen(packet);
  }
};
void wipe(void *memory, size_t size) {
  auto *bytes = static_cast<volatile uint8_t *>(memory);
  while (size--) *bytes++ = 0;
}
bool samePath(const BotPath &a, const BotPath &b) {
  return a.known == b.known && a.width == b.width && a.count == b.count &&
         !memcmp(a.bytes, b.bytes, a.size());
}
const char *commandErrorMessage(const char *error) {
  if (!strncmp(error, "[string \"", 9)) {
    const char *location = strstr(error + 9, "\"]:");
    if (location) {
      const char *line = location + 3;
      const char *end = line;
      while (*end >= '0' && *end <= '9') ++end;
      if (end != line && end[0] == ':' && end[1] == ' ') error = end + 2;
    }
  }
  if (!strcmp(error, "Native TRACE timed out"))
    return "TRACE timed out; no reply received. Check the route and try again.";
  return error;
}
}

struct CommandBot::Core : mesh::Mesh {
  struct Contact;
#if defined(MESHCORE_HOST_BOT_SOURCE) && MESHCORE_HOST_BOT_SOURCE
  OwnerSendSink ownerSendSink = nullptr;
  OwnerInboxSink ownerInboxSink = nullptr;
  struct OwnerSend {
    uint8_t id[16]{}, state = 0, flags = 0, reason = 0;
    uint32_t ack = 0, at = 0;
    mesh::Packet *packet = nullptr;
    bool used = false;
  } ownerSends[4];
  struct OwnerSeen {
    uint8_t hash[16]{};
    uint32_t at = 0;
    bool used = false;
  } ownerSeen[64];
  void ownerState(OwnerSend &entry, uint8_t state, uint8_t reason = 0) {
    entry.state = state; entry.reason = reason;
    if (ownerSendSink) ownerSendSink(entry.id, state, entry.flags, reason);
  }
  void ownerTick() {
    for (auto &entry : ownerSends)
      if (entry.used && entry.state < 4 && uint32_t(millis() - entry.at) >= 45000)
        ownerState(entry, 6, 7);
  }
  void ownerMessage(const Contact &sender, mesh::Packet *packet, const uint8_t *data, size_t size);
  void ownerSend(const uint8_t request[16], const uint8_t recipient[32], const uint8_t *text, size_t size);
#endif
  struct Contact {
    mesh::Identity id;
    uint32_t advert = 0, heard = 0;
    uint32_t routeAt = 0;
    bool used = false;
    char name[33]{};
    uint32_t observedAt = 0;
    uint8_t observedWidth = 0, observedHops = 0;
    bool local = false, measured = false;
    bool ackSent = false;
    uint32_t ackAt = 0;
    uint8_t ackHash[4]{};
    float rssi = 0, snr = 0;
    PeerRoute route;
  } contacts[16];
  uint8_t matches[16]{};
  struct Seen {
    uint8_t request[16]{};
    uint32_t at = 0, ackAt = 0;
    bool used = false;
  } seen[64];
  struct Credit {
    uint32_t at = 0, airtime = 0;
    bool used = false;
  } credit[62];
  Credit noticeCredit{};
  // One-byte peer-hash collisions must not turn our reflected ciphertext into caller input.
  struct Sent {
    uint8_t digest[16]{};
    uint32_t at = 0;
    bool used = false;
  } sent[32];
  unsigned nextSent = 0;
  struct Forwarded {
    uint8_t digest[16]{};
    uint32_t at = 0;
    bool used = false;
  } forwarded[16];
  BotForwardPolicy forwarding;
  uint32_t forwardGrant = 1;
  BotMeshPolicy meshPolicy;
  uint32_t meshGrant = 1;
  BotRepeaterPolicy repeaterPolicy;
  struct RepeaterState {
    BotRepeaterSnapshot snapshot{};
    uint64_t sampledMs = 0;
    uint32_t due = 0, floodAt = 0, loginTimestamp = 0;
    uint8_t failures = 0;
    bool flooded = false;
  } repeaters[BotRepeaterLimit];
  uint32_t repeaterGrant = 1, repeaterAt = 0;
  bool repeaterSent = false;
  mesh::Packet *adminReplyPacket = nullptr;
  uint32_t adminReplyTicket = 0;
  BotReminderDispatch reminder{};
  mesh::Packet *reminderPacket = nullptr;
  uint32_t reminderDeadline = 0;
  bool reminderActive = false;
  ReplyRouting routing;
  BotRadioPolicy policy;
  AdaptiveAdmission adaptive;
  const char *capacityError = nullptr;
  mesh::GroupChannel channel{};
  const char *lastAdmission = "none";
  uint32_t admissionAt = 0, admissionWait = 0, noticeAt = 0;
  bool noticeSent = false;
  Tables tables;
  BotPackets packets;
  CommandBot &owner;
  Counters stats{};
  struct Invocation {
    bool used = false, collecting = false, deferred = false, cancelled = false;
    uint32_t job = 0, collectionStart = 0;
    uint32_t notBefore = 0, adminTicket = 0;
    uint8_t dmTarget[32]{};
    BotEvent event{};
    uint8_t secret[32]{};
    mesh::GroupChannel groupChannel{};
    PeerRoute route;
    BotIoRequest io{};
    BotIoResult result{};
    mesh::Packet *outbound = nullptr;
    uint32_t deadline = 0, expectedAck = 0, rxAckAt = 0, traceTag = 0, traceAuth = 0;
    bool ioPending = false, ackExpected = false, acked = false, overflow = false;
    struct Received {
      BotPacketInfo info{};
      char text[BotReplyLimit + 1]{};
      uint8_t digest[16]{};
      uint32_t at = 0;
    } received[BotReceiveLimit], offered;
    uint8_t hashes[BotReceiveDedupLimit][16]{};
    unsigned rxCount = 0, hashCount = 0;
    uint32_t nextPacket = 0, traceAt = 0, traceJob = 0, lastRadioJob = 0;
    uint8_t traceRoute[BotTraceLimit]{}, traceSize = 0, traceWidth = 0;
    BotTraceInfo traceResult{};
    bool traceActive = false, traceReady = false, traceTransmitted = false;
    bool lastQueued = false, lastTransmitted = false;
    uint32_t repeaterTag = 0;
    int8_t repeaterIndex = -1;
    bool repeaterResponse = false;
  } invocations[BotJobLimit];
  uint32_t nextJob = 0, sentTimestamp = 0;
  bool sourceResultReady = false,
       initializing = true, initialized = false, administratorBlocked = false, sharedState = false,
       homeAccess = false;
  uint32_t lastAdvert = 0, lastRxDropped = 0;
  uint32_t initializationRetryAt = 0;
  unsigned initializationRetries = 0;
  BotWorker::Result sourceResult{};
  uint32_t eventAt = 0, startupGeneration[2]{}, sampledGeneration = 0;
  uint32_t scheduledGeneration = 0, scheduledAt = 0;
  bool discovery = false, discoverySent = false;
  uint32_t discoveryAt = 0;
  bool eventRateSet = false, connectivityKnown = false, nodeKnown = false;
  BotNodeSnapshot previousEventNode{};
  Seen eventSeen[16]{};

  explicit Core(CommandBot &bot)
      : Mesh(bot.radio_, bot.millis_, bot.rng_, bot.rtc_, packets, tables), owner(bot) {
    packets.admission = &adaptive;
  }
  static const uint8_t *principal(const BotEvent &event) {
    static const uint8_t service[32]{};
    return event.authenticated ? event.sender : event.channelVerified ? event.channelId : service;
  }
  bool sampleAdmission() {
    mesh::QueuedRadioStats queue;
    if (!owner.radio_.getQueuedRadioStats(queue)) return false;
    adaptive.sample(millis(), queue.generation, queue.aggregate_rf_ms,
                    owner.radio_.receivedAirtimeMs(), queue.aggregate_queued);
    return true;
  }
  AdaptiveAdmission::Reason admitWork(const BotEvent &event) {
    unsigned active = 0, own = 0;
    for (const auto &job : invocations) if (job.used) {
      ++active;
      if (!memcmp(principal(job.event), principal(event), 32)) ++own;
    }
    active = std::max(active, owner.worker_.jobsInUse());
    const unsigned addresses = event.channelVerified ? 1 : 2;
    const uint32_t quantum = owner.radio_.getEstAirtimeFor(2 + addresses + CIPHER_MAC_SIZE + CIPHER_BLOCK_SIZE);
    return adaptive.work(millis(), principal(event), sampleAdmission(), active, own, quantum);
  }
  void reject(const char *message) {
    ++stats.rejected;
    owner.fault(message);
  }
  bool canInvoke(const BotEvent &event) const {
    unsigned collecting = 0, custom = 0;
    for (const auto &invocation : invocations) if (invocation.collecting || invocation.deferred) {
      ++collecting;
      if (!botReservedCommand(invocation.event.name)) ++custom;
    }
    return owner.worker_.canInvoke(event, collecting, custom);
  }
  bool emit(BotEvent &event) {
    const unsigned bit = 1u << (unsigned(event.kind) - 1);
    if (!(owner.worker_.eventMask() & bit) || !initialized || administratorBlocked || sourceResultReady)
      return false;
    const uint32_t now = millis();
    if ((eventRateSet && uint32_t(now - eventAt) < 1000) || nextJob == UINT32_MAX) {
      ++stats.eventsDropped; return false;
    }
    owner.nodeSnapshot(event.node);
    event.sharedState = sharedState;
    event.homeAccess = event.forwardAccess = event.reminderAccess = false;
    Invocation *scheduled = nullptr;
    if (event.kind == BotEvent::Scheduled) {
      for (auto &job : invocations) if (!job.used) { scheduled = &job; break; }
      if (!scheduled) { ++stats.eventsDropped; return false; }
    }
    if (admitWork(event) != AdaptiveAdmission::Allowed ||
        !canInvoke(event) || !owner.worker_.invoke(event, nextJob + 1)) { ++stats.eventsDropped; return false; }
    if (scheduled) {
      *scheduled = {};
      scheduled->used = true; scheduled->job = nextJob + 1; scheduled->event = event;
    }
    ++nextJob; ++stats.eventsQueued; eventRateSet = true; eventAt = now; return true;
  }
  void messageEvent(mesh::Packet *packet, const BotEvent &original, const char *text, size_t size) {
    if (!(owner.worker_.eventMask() & 4) || original.local || !size || size > BotTextLimit) return;
    for (size_t i = 0; i < size; ++i) if (text[i] < 32 || text[i] > 126) return;
    const uint32_t now = millis();
    Seen *slot = nullptr;
    for (auto &seen : eventSeen) {
      if (seen.used && uint32_t(now - seen.at) < 120000) {
        if (!memcmp(seen.request, original.request, 16)) return;
      } else if (!slot) slot = &seen;
    }
    if (!slot) { ++stats.eventsDropped; return; }
    BotEvent event = original;
    event.kind = BotEvent::Message;
    memcpy(event.message, text, size); event.message[size] = 0;
    event.routeType = packet->getRouteType();
    if (packet->isRouteFlood()) {
      event.path.known = true; event.path.width = packet->getPathHashSize();
      event.path.count = packet->getPathHashCount();
      memcpy(event.path.bytes, packet->path, event.path.size());
    }
    const auto *signal = packets.reception(packet);
    event.signal = signal && std::isfinite(signal->rssi) && std::isfinite(signal->snr) &&
        signal->rssi >= -150 && signal->rssi <= 0 && signal->snr >= -32 && signal->snr <= 32;
    if (event.signal) { event.rssi = signal->rssi; event.snr = signal->snr; }
    const auto configuration = owner.radio_.configuration();
    event.frequency = configuration.freq_hz; event.bandwidth = configuration.bw_hz;
    event.sf = configuration.sf; event.cr = configuration.cr;
    if (emit(event)) {
      *slot = {};
      slot->used = true; slot->at = now; memcpy(slot->request, original.request, 16);
    }
  }
  void statusEvents() {
    if (!initialized || initializing || administratorBlocked || sourceResultReady) return;
    if (sampledGeneration != owner.worker_.sourceGeneration()) {
      sampledGeneration = owner.worker_.sourceGeneration();
      connectivityKnown = nodeKnown = false;
    }
    BotEvent event;
    owner.nodeSnapshot(event.node);
    const auto snapshot = event.node;
    if (scheduledGeneration != owner.worker_.runtimeGeneration(BotWorker::Lua)) {
      scheduledGeneration = owner.worker_.runtimeGeneration(BotWorker::Lua); scheduledAt = millis();
    }
    const uint32_t seconds = owner.worker_.scheduleSeconds();
    if (seconds && uint32_t(millis() - scheduledAt) >= seconds * 1000) {
      scheduledAt = millis(); // Never catch up missed periods after a stall.
      event.kind = BotEvent::Scheduled;
      event.runtimeMask = 1u << BotWorker::Lua;
      emit(event);
      return;
    }
    for (auto runtime : {BotWorker::Lua, BotWorker::Wasm}) {
      if ((owner.worker_.eventMask(runtime) & 1) &&
          startupGeneration[runtime] != owner.worker_.runtimeGeneration(runtime)) {
        event.kind = BotEvent::Startup;
        event.runtimeMask = 1u << runtime;
        if (emit(event)) startupGeneration[runtime] = owner.worker_.runtimeGeneration(runtime);
        return;
      }
    }
    if ((owner.worker_.eventMask() & 2) &&
        (!connectivityKnown || snapshot.wifiKnown != previousEventNode.wifiKnown ||
         snapshot.wifiConnected != previousEventNode.wifiConnected)) {
      event.kind = BotEvent::Connectivity;
      if (emit(event)) {
        connectivityKnown = true; previousEventNode.wifiKnown = snapshot.wifiKnown;
        previousEventNode.wifiConnected = snapshot.wifiConnected;
      }
      return;
    }
    if ((owner.worker_.eventMask() & 8) &&
        (!nodeKnown || snapshot.ready != previousEventNode.ready || snapshot.fault != previousEventNode.fault ||
         snapshot.rolesKnown != previousEventNode.rolesKnown || snapshot.selectedRoles != previousEventNode.selectedRoles ||
         snapshot.readyRoles != previousEventNode.readyRoles)) {
      event.kind = BotEvent::NodeStatus;
      if (emit(event)) {
        nodeKnown = true; previousEventNode.ready = snapshot.ready; previousEventNode.fault = snapshot.fault;
        previousEventNode.rolesKnown = snapshot.rolesKnown; previousEventNode.selectedRoles = snapshot.selectedRoles;
        previousEventNode.readyRoles = snapshot.readyRoles;
      }
    }
  }
  bool messageTimestamp(uint32_t &value) {
    const auto now = getRTCClock()->getCurrentTime();
    if (sentTimestamp == UINT32_MAX) { reject("Bot message timestamp exhausted"); return false; }
    value = now > sentTimestamp ? now : sentTimestamp + 1;
    sentTimestamp = value;
    return true;
  }
  bool allowPacketForward(const mesh::Packet *) override { return false; }
  int searchChannelsByHash(const uint8_t *hash, mesh::GroupChannel dest[], int maximum) override {
    if (!policy.channel[0] || maximum < 1 || hash[0] != channel.hash[0]) return 0;
    dest[0] = channel;
    return 1;
  }
  void rememberOutbound(const mesh::Packet *packet) {
    if (packet->getPayloadType() != PAYLOAD_TYPE_TXT_MSG &&
        packet->getPayloadType() != PAYLOAD_TYPE_GRP_TXT) return;
    auto &entry = sent[nextSent++ % 32];
    mesh::Utils::sha256(entry.digest, sizeof(entry.digest), packet->payload, packet->payload_len);
    entry.at = millis(); entry.used = true;
  }
  void logRx(mesh::Packet *packet, int, float) override {
    // This hook runs before native flood deferral; pool release clears the
    // snapshot so a recycled packet cannot inherit another reception's signal.
    if (!packets.capture(packet, owner.radio_.getLastRSSI(), owner.radio_.getLastSNR()))
      owner.fault("Bot receive metadata capacity exhausted");
  }
  mesh::DispatcherAction onRecvPacket(mesh::Packet *p) override {
    const unsigned type = p->getPayloadType();
    if (p->getPayloadVer() != PAYLOAD_VER_1 ||
        p->path_len > 255 || !mesh::Packet::isValidPathLen(p->path_len)) {
      ++stats.malformed;
      owner.fault("Invalid command packet version/path");
      return ACTION_RELEASE;
    }
    // Reject malformed ciphertext before entering the pinned native decoder.
    if (type == PAYLOAD_TYPE_ADVERT) {
      if (p->payload_len < 32 + 4 + 64 || p->payload_len > 132) {
        ++stats.malformed; owner.fault("Malformed bot contact advert");
        return ACTION_RELEASE;
      }
    } else if (type == PAYLOAD_TYPE_TXT_MSG || type == PAYLOAD_TYPE_PATH || type == PAYLOAD_TYPE_REQ ||
               type == PAYLOAD_TYPE_RESPONSE ||
               type == PAYLOAD_TYPE_GRP_TXT) {
      const unsigned overhead = type == PAYLOAD_TYPE_GRP_TXT ? 3 : 4;
      if (p->payload_len < overhead + 16 || p->payload_len > MAX_PACKET_PAYLOAD ||
          (p->payload_len - overhead) % 16) {
        ++stats.malformed; owner.fault("Malformed encrypted bot packet");
        return ACTION_RELEASE;
      }
      if (type == PAYLOAD_TYPE_TXT_MSG || type == PAYLOAD_TYPE_GRP_TXT) {
        uint8_t digest[16];
        mesh::Utils::sha256(digest, sizeof(digest), p->payload, p->payload_len);
        for (const auto &entry : sent)
          if (entry.used && uint32_t(millis() - entry.at) < 120000 &&
              !memcmp(entry.digest, digest, sizeof(digest))) return ACTION_RELEASE;
      }
    } else if (type == PAYLOAD_TYPE_TRACE) {
      if (!p->isRouteDirect() || p->payload_len < 9 || p->payload_len > MAX_PACKET_PAYLOAD ||
          p->path_len >= MAX_PATH_SIZE || p->payload[8] > 3 ||
          (p->payload_len - 9) % (1u << p->payload[8])) {
        ++stats.malformed; owner.fault("Malformed native TRACE"); return ACTION_RELEASE;
      }
    } else if (type == PAYLOAD_TYPE_ACK) {
      // A local copy must not occupy the RF ACK dedup entry.
      if (p->_localReflection) return ACTION_RELEASE;
      if (p->payload_len < 4 || p->payload_len > MAX_PACKET_PAYLOAD) {
        ++stats.malformed; owner.fault("Malformed native ACK"); return ACTION_RELEASE;
      }
    } else {
      return ACTION_RELEASE;
    }
    return Mesh::onRecvPacket(p);
  }
  void onAdvertRecv(mesh::Packet *packet, const mesh::Identity &id,
                    uint32_t timestamp, const uint8_t *data, size_t size) override {
    AdvertDataParser advert(data, size);
    if (!advert.isValid() || !advert.hasName()) return;
    Contact *slot = nullptr;
    for (auto &contact : contacts) {
      if (contact.used && contact.id.matches(id)) {
        if (timestamp <= contact.advert) return;
        slot = &contact; break;
      }
      if (!slot && (!contact.used ||
                    (repeaterIndex(contact.id.pub_key) < 0 &&
                     uint32_t(millis() - contact.heard) >= 600000)))
        slot = &contact;
    }
    if (!slot) { reject("Bot contact table full (16)"); return; }
    if (!slot->used || !slot->id.matches(id)) *slot = {};
    slot->used = true;
    slot->id = id; slot->advert = timestamp; slot->heard = millis();
    snprintf(slot->name, sizeof(slot->name), "%.32s", advert.getName());
    for (char *p = slot->name; *p; ++p)
      if (static_cast<unsigned char>(*p) < 32 || static_cast<unsigned char>(*p) > 126) *p = '?';
    slot->observedAt = millis(); slot->local = packet->_localReflection;
    slot->observedWidth = packet->isRouteFlood() ? packet->getPathHashSize() : 0;
    slot->observedHops = packet->isRouteFlood() ? packet->getPathHashCount() : 0;
    const auto *signal = packets.reception(packet);
    slot->measured = !slot->local && signal && std::isfinite(signal->rssi) && std::isfinite(signal->snr);
    if (slot->measured) { slot->rssi = signal->rssi; slot->snr = signal->snr; }
  }
  bool ownerKey(const BotEvent &event) const {
#if defined(MESHCORE_MAST_ADMIN) && MESHCORE_MAST_ADMIN
    auto *admin = MastAdmin::service();
    return admin && admin->ready() && event.authenticated && !event.local && !event.channel[0] &&
           admin->trusted(event.sender);
#else
    return false;
#endif
  }
  void adminAcknowledged(uint32_t ticket, bool transmitted) {
#if defined(MESHCORE_MAST_ADMIN) && MESHCORE_MAST_ADMIN
    if (ticket && MastAdmin::service()) MastAdmin::service()->acknowledged(ticket, transmitted);
#endif
  }
  void neighbors(unsigned page, char *text, size_t capacity) const {
    unsigned count = 0;
    const Contact *selected = nullptr;
    for (const auto &contact : contacts) if (contact.used) {
      if (++count == page) selected = &contact;
    }
    if (!count && page == 1) { snprintf(text, capacity, "No observed signed adverts; not a live neighbor scan"); return; }
    if (!selected) { snprintf(text, capacity, "Error: neighbors has %u pages", count); return; }
    const auto &c = *selected;
    char key[9], rx[12] = "direct", route[20] = "none", signal[36] = "RF unmeasured";
    for (unsigned i = 0; i < 4; ++i) snprintf(key + 2 * i, 3, "%02x", c.id.pub_key[i]);
    if (c.observedWidth) snprintf(rx, sizeof(rx), "%ub/%uh", c.observedWidth, c.observedHops);
    if (c.route.length != 0xff)
      snprintf(route, sizeof(route), "%ub/%uh", (c.route.length >> 6) + 1, c.route.length & 63);
    if (c.local) strcpy(signal, "local reflection");
    else if (c.measured) snprintf(signal, sizeof(signal), "last-hop %.0fdBm/%.2gdB", double(c.rssi), double(c.snr));
    char next[32] = "; not live adjacency";
    if (page < count) snprintf(next, sizeof(next), "; !neighbors %u", page + 1);
    const int size = snprintf(text, capacity, "Seen %u/%u %.20s %s age=%us rx=%s cached=%s; %s%s",
             page, count, c.name, key, unsigned(millis() - c.observedAt) / 1000,
             rx, route, signal, next);
    if (size >= int(capacity))
      snprintf(text, capacity, "Observed %u/%u %s age=%us rx=%s cached=%s%s",
               page, count, key, unsigned(millis() - c.observedAt) / 1000, rx, route, next);
  }
  int searchPeersByHash(const uint8_t *hash) override {
    int count = 0;
    for (unsigned i = 0; i < 16; ++i)
      if (contacts[i].used && contacts[i].id.isHashMatch(hash))
        matches[count++] = i;
    return count;
  }
  void getPeerSharedSecret(uint8_t *secret, int index) override {
    self_id.calcSharedSecret(secret, contacts[matches[index]].id);
  }
  bool onPeerPathRecv(mesh::Packet *packet, int index, const uint8_t *secret,
                      uint8_t *path, uint8_t length, uint8_t extraType,
                      uint8_t *extra, uint8_t extraSize) override {
    if (packet->_localReflection || !mesh::Packet::isValidPathLen(length) ||
        2u + (length & 63u) * ((length >> 6) + 1u) > unsigned(packet->payload_len - 4))
      return false;
    auto &contact = contacts[matches[index]];
    const bool monitored = repeaterIndex(contact.id.pub_key) >= 0;
    if (monitored) {
      if (extraType != PAYLOAD_TYPE_RESPONSE || !repeaterResponse(packet, contact, extra, extraSize))
        return false;
    }
    contact.route.learn(path, length);
    contact.route.scoped = routing.scope(packet);
    contact.routeAt = contact.heard = millis();
    if (monitored) {
      if (packet->isRouteFlood()) {
        auto *reply = createPathReturn(contact.id, secret, packet->path, packet->path_len, 0, nullptr, 0);
        if (reply) {
          if (capacity(replyAirtime(reply, contact.route), true, false, reply, contact.id.pub_key))
            sendDirect(reply, path, length, 500);
          else {
            releasePacket(reply);
            owner.fault("Repeater return path not sent: bot airtime budget exhausted");
          }
        } else owner.fault("Repeater return path packet capacity exhausted");
      }
      return false;
    }
    if (extraType == PAYLOAD_TYPE_ACK && extraSize >= 4) {
      uint32_t ack;
      memcpy(&ack, extra, 4);
      onAckRecv(packet, ack);
    }
    return true;
  }
  int repeaterIndex(const uint8_t *key) const {
    for (unsigned i = 0; i < BotRepeaterLimit; ++i)
      if (repeaterPolicy.targets[i].used && !memcmp(key, repeaterPolicy.targets[i].key, 32))
        return int(i);
    return -1;
  }
  BotRepeaterSnapshot repeaterSnapshot(unsigned index) const {
    auto snapshot = repeaters[index].snapshot;
    const auto &target = repeaterPolicy.targets[index];
    snapshot.configured = target.used;
    snprintf(snapshot.alias, sizeof(snapshot.alias), "%s", target.alias);
    if (snapshot.available)
      snapshot.ageSeconds = uint32_t(std::min<uint64_t>(
          (owner.node_.uptimeMs - repeaters[index].sampledMs) / 1000, UINT32_MAX));
    snapshot.fresh = snapshot.available && snapshot.error == BotRepeaterError::None &&
        snapshot.ageSeconds <= repeaterPolicy.intervalSeconds * 2;
    if (!repeaterPolicy.enabled) { snapshot.error = BotRepeaterError::Disabled; snapshot.fresh = false; }
    else if (target.frequencyHz && target.frequencyHz != owner.radio_.configuration().freq_hz) {
      snapshot.error = BotRepeaterError::Frequency; snapshot.fresh = false;
    }
    return snapshot;
  }
  bool repeaterAuthority(const Invocation &job) const {
    return repeaterPolicy.enabled &&
        ((job.event.kind == BotEvent::Scheduled && (owner.worker_.eventAccess() & 16) &&
          job.io.eventEpoch == owner.worker_.eventEpoch()) ||
         ownerKey(job.event));
  }
  bool repeaterIo(const Invocation &job) const {
    return job.io.kind == BotIoRequest::RepeaterStatus || job.io.kind == BotIoRequest::RepeaterLogin;
  }
  void failRepeater(Invocation &job, BotRepeaterError code, const char *message) {
    job.result.repeater.error = code;
    if (job.repeaterIndex >= 0) {
      auto &state = repeaters[unsigned(job.repeaterIndex)];
      state.snapshot.error = code;
      ++state.snapshot.failures;
      state.failures = std::min<unsigned>(state.failures + 1, 6);
      state.due = millis() + std::max(repeaterPolicy.intervalSeconds,
          std::min<uint32_t>(3600, repeaterPolicy.intervalSeconds << state.failures)) * 1000;
      job.result.repeater = repeaterSnapshot(unsigned(job.repeaterIndex));
    }
    finishRadio(job, message);
  }
  bool repeaterReady(unsigned index) const {
    const auto &target = repeaterPolicy.targets[index];
    const auto &state = repeaters[index];
    return target.used && (!target.frequencyHz || target.frequencyHz == owner.radio_.configuration().freq_hz) &&
        int32_t(millis() - state.due) >= 0;
  }
  Contact *repeaterContact(unsigned index) {
    const auto &target = repeaterPolicy.targets[index];
    Contact *slot = nullptr;
    for (auto &contact : contacts) {
      if (contact.used && contact.id.matches(target.key)) return &contact;
      if (!contact.used && !slot) slot = &contact;
    }
    if (!slot) {
      for (auto &contact : contacts)
        if (repeaterIndex(contact.id.pub_key) < 0 &&
            (!slot || uint32_t(millis() - contact.heard) > uint32_t(millis() - slot->heard)))
          slot = &contact;
    }
    if (!slot) return nullptr;
    *slot = {};
    slot->used = true; slot->id = mesh::Identity(target.key);
    snprintf(slot->name, sizeof(slot->name), "%s", target.alias);
    slot->heard = millis();
    if (target.path.known) {
      slot->route.learn(target.path.bytes, uint8_t((target.path.width - 1) << 6 | target.path.count));
      slot->routeAt = millis();
    }
    return slot;
  }
  void startRepeater(Invocation &job) {
    if (!repeaterAuthority(job)) {
      failRepeater(job, BotRepeaterError::Permission, "Repeater polling requires the owner grant and a scheduled event or owner DM");
      return;
    }
    if (job.io.kind == BotIoRequest::RepeaterNext) {
      if (!repeaterSent || uint32_t(millis() - repeaterAt) >= 30000)
        for (unsigned i = 0; i < BotRepeaterLimit; ++i) if (repeaterReady(i)) {
          snprintf(job.result.value, sizeof(job.result.value), "%s", repeaterPolicy.targets[i].alias);
          job.result.found = true; break;
        }
      finishRadio(job); return;
    }
    int index = -1;
    for (unsigned i = 0; i < BotRepeaterLimit; ++i)
      if (repeaterPolicy.targets[i].used && !strcmp(job.io.key, repeaterPolicy.targets[i].alias))
        index = int(i);
    if (index < 0) { failRepeater(job, BotRepeaterError::Unavailable, "Repeater alias is not configured by the owner"); return; }
    job.result.repeater = repeaterSnapshot(unsigned(index));
    if (!repeaterReady(unsigned(index))) {
      failRepeater(job, job.result.repeater.error == BotRepeaterError::Frequency ?
                   BotRepeaterError::Frequency : BotRepeaterError::NotDue,
                   "Repeater frequency differs from the modem or its next poll is not due"); return;
    }
    if (repeaterSent && uint32_t(millis() - repeaterAt) < 30000) {
      failRepeater(job, BotRepeaterError::NotDue, "Repeater polls are limited to one request per 30 seconds"); return;
    }
    for (const auto &other : invocations)
      if (&other != &job && other.used && other.ioPending && repeaterIo(other)) {
        failRepeater(job, BotRepeaterError::Busy, "Another repeater request is pending"); return;
      }
    if (!owner.radio_.queuedReady() || owner.radio_.hasPendingWork()) {
      failRepeater(job, BotRepeaterError::Busy, "Shared modem is busy; repeater request was not sent"); return;
    }
    uint32_t tag = 0;
    uint32_t earliest = 0, latest = 0;
    if (!trustedNetworkTime(earliest, latest) || !messageTimestamp(tag)) {
      failRepeater(job, BotRepeaterError::Clock, "Repeater polling requires a trusted clock"); return;
    }
    auto *contact = repeaterContact(unsigned(index));
    if (!contact || contact->id.matches(self_id.pub_key)) {
      failRepeater(job, BotRepeaterError::Unavailable, "Repeater identity is unavailable or belongs to this bot"); return;
    }
    auto &state = repeaters[unsigned(index)];
    const bool flood = contact->route.length == 0xff || state.failures >= 3;
    if (flood && state.flooded && uint32_t(millis() - state.floodAt) < 3600000) {
      state.due = state.floodAt + 3600000;
      failRepeater(job, BotRepeaterError::NotDue, "Repeater flood discovery is limited to once per hour"); return;
    }
    const uint32_t lastFlood = repeaterPolicy.targets[unsigned(index)].lastFloodUtc;
    if (flood && lastFlood && uint64_t(tag) < uint64_t(lastFlood) + 3600) {
      const uint32_t remaining = uint32_t(std::min<uint64_t>(3600, uint64_t(lastFlood) + 3600 - tag));
      state.due = millis() + remaining * 1000;
      failRepeater(job, BotRepeaterError::NotDue, "Repeater flood cooldown persists across restarts"); return;
    }
    uint8_t secret[32]{}, data[13]{};
    self_id.calcSharedSecret(secret, contact->id);
    memcpy(data, &tag, 4);
    data[4] = job.io.kind == BotIoRequest::RepeaterLogin ? 0 : 1;
    auto *packet = job.io.kind == BotIoRequest::RepeaterLogin ?
        createAnonDatagram(PAYLOAD_TYPE_ANON_REQ, self_id, contact->id, secret, data, 5) :
        createDatagram(PAYLOAD_TYPE_REQ, contact->id, secret, data, sizeof(data));
    wipe(secret, sizeof(secret));
    if (!packet) { failRepeater(job, BotRepeaterError::Capacity, "Repeater request packet capacity exhausted"); return; }
    const unsigned pathBytes = flood ? 0 : (contact->route.length & 63) * ((contact->route.length >> 6) + 1);
    if (!capacity(owner.radio_.getEstAirtimeFor(2 + packet->payload_len + pathBytes), true, false,
                  packet, contact->id.pub_key)) {
      releasePacket(packet); failRepeater(job, BotRepeaterError::Capacity, capacityError); return;
    }
    if (flood) {
      auto candidate = repeaterPolicy;
      candidate.targets[unsigned(index)].lastFloodUtc = tag;
      if (!saveBotRepeaterPolicy(candidate)) {
        releasePacket(packet); repeaterPolicy.enabled = false;
        owner.fault("Repeater flood cooldown save failed; live monitor disabled");
        failRepeater(job, BotRepeaterError::Disabled, "Repeater flood cooldown could not be saved; request was not sent");
        return;
      }
      repeaterPolicy = candidate;
    }
    job.repeaterIndex = int8_t(index); job.repeaterTag = tag;
    job.repeaterResponse = false; job.io.grant = repeaterGrant; job.outbound = packet;
    job.result.repeater.error = BotRepeaterError::None;
    ++state.snapshot.attempts;
    state.due = millis() + repeaterPolicy.intervalSeconds * 1000;
    repeaterSent = true; repeaterAt = millis();
    if (flood) {
      state.flooded = true; state.floodAt = millis();
      routing.flood(*this, packet, routing.defaultScope(), policy.pathWidth);
    } else sendDirect(packet, contact->route.bytes, contact->route.length);
  }
  bool repeaterResponse(mesh::Packet *packet, Contact &contact, const uint8_t *data, size_t size) {
    if (packet->_localReflection || size < 4) return false;
    for (auto &job : invocations)
      if (job.used && job.ioPending && repeaterIo(job) && job.repeaterIndex >= 0 &&
          !job.repeaterResponse && contact.id.matches(repeaterPolicy.targets[unsigned(job.repeaterIndex)].key) &&
          job.io.grant == repeaterGrant && repeaterAuthority(job) &&
          owner.worker_.ioCurrent(job.io.token) && int32_t(millis() - job.deadline) < 0) {
        auto &state = repeaters[unsigned(job.repeaterIndex)];
        uint32_t tag = 0; memcpy(&tag, data, 4);
        size_t length = 0;
        if (job.io.kind == BotIoRequest::RepeaterStatus) {
          if (tag != job.repeaterTag) continue;
          length = 60;
        } else {
          const uint32_t now = owner.rtc_.getCurrentTime();
          if (tag <= state.loginTimestamp || uint64_t(tag) + 120 < now || uint64_t(now) + 120 < tag) continue;
          length = 13;
        }
        if (size < length || size >= length + CIPHER_BLOCK_SIZE) {
          failRepeater(job, BotRepeaterError::Malformed, "Repeater response length does not match the native status/login format");
          return false;
        }
        for (size_t i = length; i < size; ++i) if (data[i]) {
          failRepeater(job, BotRepeaterError::Malformed, "Repeater response has nonzero padding"); return false;
        }
        if (job.io.kind == BotIoRequest::RepeaterStatus) {
          BotRepeaterStats stats;
          if (!decodeBotRepeaterStats(data + 4, 56, stats)) {
            failRepeater(job, BotRepeaterError::Malformed, "Repeater status decoding failed"); return false;
          }
          state.snapshot.stats = stats; state.snapshot.available = true;
          state.sampledMs = owner.node_.uptimeMs;
        } else {
          if (data[4] || !(data[7] & 3)) {
            failRepeater(job, BotRepeaterError::Permission, "Repeater login did not grant read access"); return false;
          }
          state.loginTimestamp = tag; job.result.repeaterPermissions = data[7];
        }
        state.snapshot.error = BotRepeaterError::None; state.failures = 0;
        job.result.repeater = repeaterSnapshot(unsigned(job.repeaterIndex));
        job.repeaterResponse = true;
        if (job.result.transmitted) finishRadio(job);
        return true;
      }
    return false;
  }
  void onAckRecv(mesh::Packet *packet, uint32_t ack) override {
    if (packet->_localReflection) return;
#if defined(MESHCORE_HOST_BOT_SOURCE) && MESHCORE_HOST_BOT_SOURCE
    for (auto &entry : ownerSends)
      if (entry.used && entry.state < 5 && entry.ack == ack && uint32_t(millis() - entry.at) < 45000) {
        entry.flags |= 2;
        ownerState(entry, 4);
      }
#endif
    for (auto &invocation : invocations)
      if (invocation.used && invocation.ackExpected && invocation.expectedAck == ack)
        invocation.acked = true;
  }
  mesh::Packet *makeAdvert(bool ownerNotification = false, const uint8_t *key = nullptr) {
    if (!ownerNotification && lastAdvert && uint32_t(millis() - lastAdvert) < 900000) {
      reject("Bot advert rate limited (15 minutes)"); return nullptr;
    }
    uint8_t data[MAX_ADVERT_DATA_SIZE]{};
    AdvertDataBuilder builder(ADV_TYPE_CHAT, meshPolicy.name);
    auto *packet = createAdvert(self_id, data, builder.encodeTo(data));
    if (!packet) { reject("Bot advert packet capacity exhausted"); return nullptr; }
    if (!capacity(owner.radio_.getEstAirtimeFor(packet->getRawLength()), true, false, key ? packet : nullptr, key)) {
      releasePacket(packet); reject(capacityError); return nullptr;
    }
    lastAdvert = millis() ? millis() : 1;
    return packet;
  }
  void onTraceRecv(mesh::Packet *packet, uint32_t tag, uint32_t auth, uint8_t flags,
                   const uint8_t *snrs, const uint8_t *hashes, uint8_t size) override {
    if (packet->_localReflection) return;
    for (auto &job : invocations)
      if (job.used && job.traceActive && !job.traceReady && uint32_t(millis() - job.traceAt) < 30000 &&
          job.traceTag == tag && job.traceAuth == auth && job.traceSize == size &&
          job.traceWidth == (1u << flags) && !memcmp(job.traceRoute, hashes, size) &&
          packet->path_len == size / job.traceWidth) {
        job.traceResult.width = job.traceWidth;
        job.traceResult.count = packet->path_len;
        memcpy(job.traceResult.snr, snrs, job.traceResult.count);
        job.traceReady = true;
        if (job.ioPending && job.io.kind == BotIoRequest::Trace &&
            !job.io.traceSendOnly && job.traceTransmitted) completeTrace(job);
      }
  }
  void completeTrace(Invocation &job) {
    job.result.trace = job.traceResult;
    job.result.found = true;
    job.result.queued = true; job.result.transmitted = job.traceTransmitted;
    job.result.radioJob = job.traceJob;
    size_t used = snprintf(job.result.value, sizeof(job.result.value),
                           "Trace %u hops; SNR", unsigned(job.traceResult.count));
    const unsigned shown = std::min(unsigned(job.traceResult.count), 12u);
    for (unsigned i = 0; i < shown; ++i)
      used += snprintf(job.result.value + used, sizeof(job.result.value) - used,
                       " %.2f", double(job.traceResult.snr[i]) / 4);
    job.result.truncated = shown < job.traceResult.count;
    if (job.result.truncated) strcat(job.result.value, "; truncated");
    job.traceActive = false;
    finishRadio(job);
  }
  bool forwardAllowed(const Invocation &job) const {
    return forwarding.enabled() && job.event.forwardAccess && job.event.authenticated &&
        !job.event.channel[0] && !job.event.local && job.event.forwardGrant == forwardGrant &&
        !memcmp(job.event.sender, forwarding.from, 32) &&
        memcmp(self_id.pub_key, forwarding.from, 32) && memcmp(self_id.pub_key, forwarding.to, 32);
  }
  Contact *freshDirectContact(const uint8_t key[32]) {
    for (auto &contact : contacts)
      if (contact.used && contact.id.matches(key) && contact.route.length != 0xff &&
          mesh::Packet::isValidPathLen(contact.route.length) &&
          uint32_t(millis() - contact.routeAt) < 600000 &&
          uint32_t(millis() - contact.heard) < 600000) return &contact;
    return nullptr;
  }
  bool routeLoops(const PeerRoute &route) const {
    const unsigned width = (route.length >> 6) + 1u;
    for (unsigned i = 0; i < (route.length & 63u); ++i)
      if (!memcmp(route.bytes + i * width, self_id.pub_key, width)) return true;
    return false;
  }
  bool reminderEligible(const BotReminderDispatch &dispatch) {
    uint32_t earliest, latest;
    auto *contact = freshDirectContact(dispatch.principal);
    return owner.worker_.reminderCurrent(dispatch) && contact && !contact->id.matches(self_id.pub_key) &&
        !routeLoops(contact->route) && trustedNetworkTime(earliest, latest) &&
        earliest >= dispatch.due && earliest - dispatch.due <= BotTimerOverdueSeconds;
  }
  void finishReminder(BotReminderState outcome, const char *error = nullptr) {
    if (!owner.worker_.completeReminder(reminder.id, outcome))
      owner.fault("Reminder completion ownership lost; durable outcome unknown");
    if (error) owner.fault(error);
    reminderActive = false; reminderPacket = nullptr;
  }
  void reminderIo() {
    BotReminderDispatch offer;
    if (owner.worker_.inspectReminder(offer))
      owner.worker_.approveReminder(reminderEligible(offer) && owner.radio_.queuedReady() &&
                                     !owner.radio_.hasPendingWork() && capacity(0, false));
    if (!reminderActive && owner.worker_.takeReminder(reminder)) {
      reminderActive = true; reminderDeadline = millis() + 5000;
      if (!reminderEligible(reminder) || !owner.radio_.queuedReady()) {
        finishReminder(BotReminderState::Unknown, "Reminder claimed but route/time/grant/source unavailable; no replay"); return;
      }
      auto *contact = freshDirectContact(reminder.principal);
      char text[BotReplyLimit + 1];
      const int size = snprintf(text, sizeof(text), "Reminder #%u: %s", reminder.id, reminder.text);
      uint32_t timestamp;
      if (size <= 0 || size > int(BotReplyLimit) || !messageTimestamp(timestamp)) {
        finishReminder(BotReminderState::Unknown, "Reminder timestamp/text unavailable after claim"); return;
      }
      uint8_t plain[5 + BotReplyLimit]{}, secret[32];
      memcpy(plain, &timestamp, 4); memcpy(plain + 5, text, size);
      self_id.calcSharedSecret(secret, contact->id);
      auto *packet = createDatagram(PAYLOAD_TYPE_TXT_MSG, contact->id, secret, plain, size + 5);
      wipe(secret, sizeof(secret));
      if (!packet) { finishReminder(BotReminderState::Unknown, "Reminder native packet capacity exhausted"); return; }
      const auto &route = contact->route;
      const unsigned path = (route.length & 63u) * ((route.length >> 6) + 1u);
      if (!capacity(owner.radio_.getEstAirtimeFor(2 + packet->payload_len + path), true,
                    false, packet, reminder.principal)) {
        releasePacket(packet); finishReminder(BotReminderState::Unknown, capacityError); return;
      }
      reminderPacket = packet;
      rememberOutbound(packet);
      sendDirect(packet, route.bytes, route.length);
      ++stats.replies;
    }
    if (reminderActive && (!owner.worker_.reminderCurrent(reminder) ||
                          int32_t(millis() - reminderDeadline) >= 0))
      finishReminder(BotReminderState::Unknown, "Reminder TX cancelled/deadline; outcome unknown, no retry");
  }
  bool meshIoCurrent(const Invocation &job) const {
    if (job.io.kind == BotIoRequest::Wait && job.io.waitKind == BotIoRequest::ChannelWait)
      return meshPolicy.channelWait && job.io.grant == meshGrant;
    if (((job.io.kind == BotIoRequest::Send && !job.io.reply && !job.event.channel[0]) ||
         (job.io.kind == BotIoRequest::Wait && job.io.waitKind == BotIoRequest::TextWait)) &&
        memcmp(job.io.principal, job.event.sender, 32))
      return job.io.grant == meshGrant && meshPolicy.allows(job.io.principal);
    return true;
  }
  void finishRadio(Invocation &invocation, const char *error = nullptr) {
    if (!error && (!owner.worker_.ioCurrent(invocation.io.token) ||
                   int32_t(millis() - invocation.deadline) >= 0))
      error = "Radio completion cancelled/late; effect may already have occurred";
    if (invocation.io.kind == BotIoRequest::Forward && !forwardAllowed(invocation))
      error = "Forward grant revoked; remote outcome may be unknown";
    if (!meshIoCurrent(invocation))
      error = "Mesh grant revoked; admitted TX may already have occurred";
    if (repeaterIo(invocation) && invocation.repeaterIndex >= 0 &&
        (invocation.io.grant != repeaterGrant || !repeaterAuthority(invocation))) {
      invocation.result.repeater.error = BotRepeaterError::Cancelled;
      error = "Repeater grant revoked; admitted request may already have transmitted";
    }
    invocation.result.ok = error == nullptr;
    if (error) {
      snprintf(invocation.result.error, sizeof(invocation.result.error), "%s", error);
      invocation.result.value[0] = 0;
      invocation.result.packet = {}; invocation.result.trace = {};
    }
    if (!owner.worker_.completeRadio(invocation.result))
      reject("Native radio completion ownership lost");
    invocation.ioPending = false; invocation.outbound = nullptr;
    if (error && (invocation.io.kind == BotIoRequest::Trace ||
                  (invocation.io.kind == BotIoRequest::Wait && invocation.io.waitKind == BotIoRequest::TraceWait)))
      invocation.traceActive = false;
  }
  void logQueuedTxResult(mesh::Packet *packet, const mesh::QueuedTransmitResult &result) override {
#if defined(MESHCORE_HOST_BOT_SOURCE) && MESHCORE_HOST_BOT_SOURCE
    for (auto &entry : ownerSends) if (entry.used && entry.packet == packet) {
      if (result.state == queued_tx::ACCEPTED) ownerState(entry, entry.flags & 2 ? 4 : 2);
      else {
        entry.packet = nullptr;
        if (result.state == queued_tx::SUCCEEDED) {
          entry.flags |= 1;
          ownerState(entry, entry.flags & 2 ? 4 : 3);
        } else ownerState(entry, result.state == queued_tx::REJECTED ? 5 : 6, 6);
      }
    }
#endif
    if (result.state == queued_tx::ACCEPTED) adaptive.accepted(packet);
    else adaptive.finish(millis(), packet, result.has_rf_ms || result.state == queued_tx::REJECTED, result.rf_ms);
    if (packet == adminReplyPacket && result.state != queued_tx::ACCEPTED) {
      adminAcknowledged(adminReplyTicket, result.state == queued_tx::SUCCEEDED);
      adminReplyPacket = nullptr; adminReplyTicket = 0;
    }
    if (reminderActive && reminderPacket == packet && result.state != queued_tx::ACCEPTED) {
      const bool sent = result.state == queued_tx::SUCCEEDED && owner.worker_.reminderCurrent(reminder) &&
                        int32_t(millis() - reminderDeadline) < 0;
      finishReminder(sent ? BotReminderState::Sent : BotReminderState::Unknown,
                     sent ? nullptr : "Reminder TX failure/late completion; outcome unknown");
    }
    for (auto &invocation : invocations)
      if (invocation.used && invocation.ioPending && invocation.outbound == packet) {
        const bool message = invocation.io.kind == BotIoRequest::Send || invocation.io.kind == BotIoRequest::Forward;
        invocation.result.radioJob = result.job;
        if (result.state == queued_tx::ACCEPTED) {
          invocation.result.queued = true;
          if (message) {
            invocation.lastQueued = true; invocation.lastTransmitted = false;
            invocation.lastRadioJob = result.job;
          }
          if (invocation.io.kind == BotIoRequest::Trace) invocation.traceJob = result.job;
        } else if (result.state == queued_tx::SUCCEEDED) {
          invocation.result.transmitted = true;
          invocation.result.acknowledged = invocation.acked;
          if (message) { invocation.lastTransmitted = true; invocation.lastRadioJob = result.job; }
          if (invocation.io.kind == BotIoRequest::Advert)
            strcpy(invocation.result.value, "Advert TX completed");
          invocation.outbound = nullptr;
          if (invocation.io.kind == BotIoRequest::Trace) {
            invocation.traceTransmitted = true;
            if (invocation.io.traceSendOnly) finishRadio(invocation);
            else if (invocation.traceReady) completeTrace(invocation);
          } else if (repeaterIo(invocation)) {
            if (invocation.repeaterResponse) finishRadio(invocation);
          } else finishRadio(invocation);
        } else if (repeaterIo(invocation))
          failRepeater(invocation, BotRepeaterError::Transmission,
                       result.state == queued_tx::UNKNOWN || !result.has_rf_ms ?
                       "Repeater request TX outcome unknown; not retried" : "Repeater request TX failed");
        else finishRadio(invocation, result.state == queued_tx::UNKNOWN || !result.has_rf_ms ?
                          "Native TX outcome unknown" : "Native TX failed");
      }
  }
  void logTxFail(mesh::Packet *packet, int) override {
#if defined(MESHCORE_HOST_BOT_SOURCE) && MESHCORE_HOST_BOT_SOURCE
    for (auto &entry : ownerSends) if (entry.used && entry.packet == packet) {
      entry.packet = nullptr; ownerState(entry, 6, 6);
    }
#endif
    owner.fault("Bot radio transmission failed or uncertain; inspect radio status; no automatic retry");
    adaptive.released(millis(), packet);
    if (packet == adminReplyPacket) {
      adminAcknowledged(adminReplyTicket, false);
      adminReplyPacket = nullptr; adminReplyTicket = 0;
    }
    if (reminderActive && reminderPacket == packet)
      finishReminder(BotReminderState::Unknown, "Reminder native queue admission failed; claim not replayed");
    for (auto &invocation : invocations)
      if (invocation.used && invocation.ioPending && invocation.outbound == packet) {
        if (repeaterIo(invocation))
          failRepeater(invocation, BotRepeaterError::Transmission, "Repeater request queue admission failed; not retried");
        else finishRadio(invocation, "Native radio queue admission failed");
      }
  }
  void radioIo() {
    BotIoRequest request;
    while (owner.worker_.pollRadio(request)) {
      Invocation *invocation = nullptr;
      for (auto &candidate : invocations)
        if (candidate.used && !candidate.cancelled && candidate.job == request.token.job) { invocation = &candidate; break; }
      if (!invocation || !owner.worker_.ioCurrent(request.token)) {
        if (!owner.worker_.rejectRadio(request.token, "Radio request cancelled before admission"))
          reject("Stale radio request ownership lost");
        continue;
      }
      auto &job = *invocation;
      job.repeaterIndex = -1;
      job.io = request; resetBotIoResult(job.result); job.result.token = request.token;
      job.ioPending = true; job.deadline = millis() + request.delayMs;
      if (!request.delayMs || request.delayMs > 30000 ||
          !memchr(request.key, 0, sizeof(request.key))) {
        finishRadio(job, "Invalid native radio wait bounds"); continue;
      }
      if (request.kind == BotIoRequest::RepeaterNext || repeaterIo(job)) {
        startRepeater(job); continue;
      }
      if (request.kind == BotIoRequest::Inspect) {
        if (strcmp(request.key, "neighbors") || request.revision < 1 || request.revision > 16)
          finishRadio(job, "Invalid native inspection");
        else {
          neighbors(request.revision, job.result.value, job.event.replyLimit + 1);
          finishRadio(job);
        }
        continue;
      }
      if (request.kind == BotIoRequest::Admin) {
#if defined(MESHCORE_MAST_ADMIN) && MESHCORE_MAST_ADMIN
        if (!ownerKey(job.event)) { finishRadio(job, "Admin requires a currently trusted-owner private DM"); continue; }
        const char *command = request.value;
        if (!strncmp(command, "role password", 13)) {
          finishRadio(job, "Role administrator passwords require encrypted Management RF; bot programs denied");
          continue;
        }
        if (!memchr(command, 0, sizeof(request.value)) ||
            (strncmp(command, "bot ", 4) && strcmp(command, "status") && strcmp(command, "job") &&
             strcmp(command, "source status") && strcmp(command, "source help") &&
             strcmp(command, "source remove") && strcmp(command, "source rollback") &&
             strncmp(command, "roles ", 6) && strncmp(command, "role-path", 9) &&
             strcmp(command, "role help") &&
             strncmp(command, "role config ", 12) && strncmp(command, "role name ", 10) &&
             strncmp(command, "role key ", 9) && strncmp(command, "role advert ", 12) &&
             strcmp(command, "reboot") && strcmp(command, "apply"))) {
          finishRadio(job, "Use bot help; credentials and bulk source/data require native management CLI/web"); continue;
        }
        auto *admin = MastAdmin::service();
        if (!admin->rememberTimestamp(job.event.sender, job.event.timestamp)) {
          finishRadio(job, "Owner command replay/old timestamp or durable replay storage unavailable"); continue;
        }
        MastAdmin::Reply reply;
        admin->execute(command, reply, job.job);
        strcpy(job.result.value, reply.text); job.adminTicket = reply.ticket;
        finishRadio(job);
#else
        finishRadio(job, "Native owner administration not enabled in this build");
#endif
        continue;
      }
      if (request.kind == BotIoRequest::Advert) continue;
      if (request.kind == BotIoRequest::Wait) {
        if (request.waitKind > BotIoRequest::ChannelWait || request.pathWidth > 3 || request.routeType > 2 ||
            request.waitAck != (request.waitKind == BotIoRequest::AckWait))
          finishRadio(job, "Invalid native packet filter");
        else if (request.waitKind == BotIoRequest::TraceWait) {
          if (!job.traceActive || !job.traceTransmitted || uint32_t(millis() - job.traceAt) >= 30000)
            finishRadio(job, "TRACE wait requires a fresh unconsumed sent trace");
        } else if (request.waitKind == BotIoRequest::ChannelWait) {
          if (!job.event.channelVerified || !job.event.channel[0] || job.event.local ||
              !meshPolicy.channelWait || request.grant != meshGrant)
            finishRadio(job, "Channel wait grant absent/revoked");
        } else if (!job.event.authenticated || job.event.channel[0] || job.event.local)
          finishRadio(job, "Packet waits require an authenticated nonlocal DM caller");
        else if (request.waitKind == BotIoRequest::TextWait && memcmp(request.principal, job.event.sender, 32) &&
                 (request.grant != meshGrant || !meshPolicy.allows(request.principal) ||
                  memcmp(request.principal, job.dmTarget, 32)))
          finishRadio(job, "Additional peer wait requires live grant and preceding send to that peer");
        else if (request.waitAck && !job.ackExpected)
          finishRadio(job, "ACK wait requires a previous send in this invocation");
        continue;
      }
      if (request.kind == BotIoRequest::Trace) {
        if (job.traceActive && uint32_t(millis() - job.traceAt) < 30000) {
          finishRadio(job, "Previous TRACE pending; consume its result before another trace"); continue;
        }
        if (!request.routeSize || request.routeSize > BotTraceLimit ||
            (request.routeWidth != 1 && request.routeWidth != 2 &&
             request.routeWidth != 4 && request.routeWidth != 8) ||
            request.routeSize % request.routeWidth || request.routeSize / request.routeWidth > BotTraceHopLimit) {
          finishRadio(job, "Invalid native TRACE route"); continue;
        }
        getRNG()->random(reinterpret_cast<uint8_t *>(&job.traceTag), 4);
        getRNG()->random(reinterpret_cast<uint8_t *>(&job.traceAuth), 4);
        const uint8_t flags = request.routeWidth == 1 ? 0 : request.routeWidth == 2 ? 1 :
                              request.routeWidth == 4 ? 2 : 3;
        auto *packet = createTrace(job.traceTag, job.traceAuth, flags);
        if (!packet) { finishRadio(job, "Native TRACE packet capacity exhausted"); continue; }
        if (!capacity(owner.radio_.getEstAirtimeFor(2 + packet->payload_len + request.routeSize), true,
                      false, packet, principal(job.event))) {
          releasePacket(packet); finishRadio(job, capacityError); continue;
        }
        job.outbound = packet;
        job.traceActive = true; job.traceReady = job.traceTransmitted = false;
        job.traceAt = millis(); job.traceSize = request.routeSize; job.traceWidth = request.routeWidth;
        memcpy(job.traceRoute, request.route, request.routeSize);
        job.ackExpected = job.acked = false;
        sendDirect(packet, request.route, request.routeSize);
        ++stats.traces;
        continue;
      }
      if ((request.kind != BotIoRequest::Send && request.kind != BotIoRequest::Forward) ||
          !owner.radio_.queuedReady() ||
          !memchr(request.value, 0, sizeof(request.value))) {
        finishRadio(job, "Native radio unavailable or invalid operation"); continue;
      }
      Contact *destination = nullptr;
      if (request.kind == BotIoRequest::Forward) {
        const auto &copy = job.offered;
        if (!forwardAllowed(job) || request.grant != forwardGrant || !request.packetId ||
            request.packetId != copy.info.id || !copy.info.forwardable ||
            uint32_t(millis() - copy.at) >= 30000) {
          finishRadio(job, "Forward requires the latest eligible copied DM and live pair grant"); continue;
        }
        destination = freshDirectContact(forwarding.to);
        if (!destination) {
          finishRadio(job, "Forward destination has no fresh authenticated direct route; no flood fallback"); continue;
        }
        if (routeLoops(destination->route)) { finishRadio(job, "Forward destination route loops through this bot"); continue; }
        char forwardedText[BotReplyLimit + 1] = "Fwd ";
        for (unsigned i = 0; i < 32; ++i) snprintf(forwardedText + 4 + 2 * i, 3, "%02x", job.event.sender[i]);
        if (strlen(copy.text) > BotReplyLimit - 70) {
          finishRadio(job, "Forward text plus full sender attribution exceeds native bound"); continue;
        }
        strcpy(forwardedText + 68, ": "); strcpy(forwardedText + 70, copy.text);
        Forwarded *slot = nullptr;
        bool duplicate = false;
        for (auto &entry : forwarded) {
          if (entry.used && uint32_t(millis() - entry.at) < 120000)
            duplicate = duplicate || !memcmp(entry.digest, copy.digest, sizeof(entry.digest));
          else if (!slot) slot = &entry;
        }
        if (duplicate || !slot) {
          finishRadio(job, duplicate ? "Copied DM already forwarded/attempted" : "Forward dedup capacity exhausted"); continue;
        }
        memcpy(slot->digest, copy.digest, sizeof(slot->digest));
        slot->used = true; slot->at = millis();
        job.offered.info.forwardable = false;
        strcpy(request.value, forwardedText);
      } else if (!request.reply &&
                 (job.event.channel[0] ?
                    (!job.event.channelVerified || request.packetKind != BotIoRequest::ChannelPacket) :
                    (!job.event.authenticated || request.packetKind != BotIoRequest::TextPacket))) {
        finishRadio(job, "Composed packet authority/kind does not match invocation"); continue;
      }
      if (request.kind == BotIoRequest::Send && !request.reply && !job.event.channel[0] &&
          memcmp(request.principal, job.event.sender, 32)) {
        if (request.grant != meshGrant || !meshPolicy.allows(request.principal)) {
          finishRadio(job, "Native DM destination grant revoked/not granted"); continue;
        }
        destination = freshDirectContact(request.principal);
        if (!destination || routeLoops(destination->route)) {
          finishRadio(job, "Granted DM destination requires fresh authenticated direct route; no flood fallback"); continue;
        }
      }
      const size_t size = strlen(request.value);
      if (!size || size > job.event.replyLimit || size > BotReplyLimit) {
        finishRadio(job, "Native reply length invalid"); continue;
      }
      if (job.event.channel[0]) {
        auto *packet = channelReply(request.value, job);
        if (!packet) { finishRadio(job, "Native channel packet capacity/length unavailable"); continue; }
        if (!capacity(owner.radio_.getEstAirtimeFor(2 + packet->payload_len), true,
                      false, packet, principal(job.event))) {
          releasePacket(packet); finishRadio(job, capacityError); continue;
        }
        job.ackExpected = false; job.acked = false; job.outbound = packet;
        rememberOutbound(packet); routeReply(packet, job); ++stats.replies;
        continue;
      }
      uint8_t plain[5 + BotReplyLimit]{};
      for (size_t i = 0; i < size; ++i)
        if (static_cast<unsigned char>(request.value[i]) < 32 ||
            static_cast<unsigned char>(request.value[i]) > 126) {
          finishRadio(job, "Native text must be printable ASCII"); break;
        }
      if (!job.ioPending) continue;
      uint32_t timestamp;
      if (!messageTimestamp(timestamp)) { finishRadio(job, "Native timestamp unavailable"); continue; }
      memcpy(plain, &timestamp, 4); memcpy(plain + 5, request.value, size);
      uint8_t forwardingSecret[32]{};
      if (destination) self_id.calcSharedSecret(forwardingSecret, destination->id);
      auto *packet = createDatagram(PAYLOAD_TYPE_TXT_MSG,
                                    destination ? destination->id : mesh::Identity(job.event.sender),
                                    destination ? forwardingSecret : job.secret, plain, 5 + size);
      wipe(forwardingSecret, sizeof(forwardingSecret));
      if (!packet) { finishRadio(job, "Native DM packet capacity exhausted"); continue; }
      const auto &route = destination ? destination->route : job.route;
      const unsigned pathBytes = route.length == 0xff ? 0 :
          (route.length & 63u) * ((route.length >> 6) + 1u);
      if (!capacity(owner.radio_.getEstAirtimeFor(2 + packet->payload_len + pathBytes +
                                                (route.scoped ? 4 : 0)), true,
                    false, packet, principal(job.event))) {
        releasePacket(packet); finishRadio(job, capacityError); continue;
      }
      mesh::Utils::sha256(reinterpret_cast<uint8_t *>(&job.expectedAck), 4,
                          plain, size + 5, self_id.pub_key, 32);
      job.ackExpected = true; job.acked = false; job.outbound = packet;
      memcpy(job.dmTarget, destination ? destination->id.pub_key : job.event.sender, 32);
      rememberOutbound(packet);
      if (destination) sendDirect(packet, route.bytes, route.length);
      else routeReply(packet, job);
      ++stats.replies;
    }
    for (auto &job : invocations) if (job.used && job.ioPending) {
      if (!owner.worker_.ioCurrent(job.io.token)) {
        finishRadio(job, "Radio operation cancelled; outcome may be unknown");
      } else if (repeaterIo(job) && (job.io.grant != repeaterGrant || !repeaterAuthority(job))) {
        failRepeater(job, BotRepeaterError::Cancelled, "Repeater grant revoked; request outcome may be unknown");
      } else if (job.io.kind == BotIoRequest::Forward && !forwardAllowed(job)) {
        finishRadio(job, "Forward grant revoked; remote outcome may be unknown");
      } else if (!meshIoCurrent(job)) {
        finishRadio(job, "Mesh grant revoked; admitted TX may already have occurred");
      } else if (int32_t(millis() - job.deadline) >= 0) {
        if (repeaterIo(job)) {
          failRepeater(job, BotRepeaterError::Timeout, "Repeater response timed out; no automatic request retry");
          continue;
        }
        finishRadio(job, job.io.kind == BotIoRequest::Send || job.io.kind == BotIoRequest::Forward ||
                    job.io.kind == BotIoRequest::Advert ||
                    (job.io.kind == BotIoRequest::Trace && !job.result.transmitted) ?
                    "Native TX deadline; outcome unknown" :
                    job.io.kind == BotIoRequest::Trace ? "Native TRACE timed out" : "Native packet wait timed out");
      } else if (job.io.kind == BotIoRequest::Advert && !job.outbound &&
                 !owner.radio_.hasPendingWork() && owner.radio_.queuedReady()) {
        auto *packet = makeAdvert(false, principal(job.event));
        if (!packet) finishRadio(job, "Bot advert unavailable; inspect native rate/airtime status");
        else {
          job.outbound = packet;
#if defined(MESHCORE_HOST_BOT_SOURCE) && MESHCORE_HOST_BOT_SOURCE
          routing.flood(*this, packet, routing.defaultScope(), policy.pathWidth);
#else
          sendFlood(packet, 0, policy.pathWidth);
#endif
        }
      } else if (job.io.kind == BotIoRequest::Wait) {
        if (job.io.waitKind == BotIoRequest::TraceWait) {
          if (job.traceReady && job.traceTransmitted) completeTrace(job);
        } else if (job.io.waitAck) {
          if (job.acked) {
            job.result.acknowledged = true;
            job.result.queued = job.lastQueued; job.result.transmitted = job.lastTransmitted;
            job.result.radioJob = job.lastRadioJob;
            finishRadio(job);
          }
        } else for (unsigned i = 0; i < job.rxCount; ++i) {
          const auto &copy = job.received[i];
          const bool matches = (job.io.waitKind == BotIoRequest::ChannelWait ? copy.info.channel :
                                copy.info.authenticated && !memcmp(copy.info.sender, job.io.principal, 32)) &&
              (job.io.exact ? !strcmp(copy.text, job.io.key) :
                                !strncmp(copy.text, job.io.key, strlen(job.io.key))) &&
              (!job.io.pathWidth || (copy.info.path.valid() && copy.info.path.width == job.io.pathWidth)) &&
              (!job.io.routeType || (job.io.routeType == 1 ?
                (copy.info.routeType == ROUTE_TYPE_FLOOD || copy.info.routeType == ROUTE_TYPE_TRANSPORT_FLOOD) :
                (copy.info.routeType == ROUTE_TYPE_DIRECT || copy.info.routeType == ROUTE_TYPE_TRANSPORT_DIRECT)));
          if (matches) {
            job.offered = copy;
            job.offered.info.forwardable = copy.info.forwardable && forwardAllowed(job) &&
                                          !memcmp(copy.info.sender, job.event.sender, 32) &&
                                          uint32_t(millis() - copy.at) < 30000;
            job.result.packet = job.offered.info;
            strcpy(job.result.value, copy.text);
            for (unsigned j = i + 1; j < job.rxCount; ++j) job.received[j - 1] = job.received[j];
            --job.rxCount;
            job.result.truncated = job.overflow; job.overflow = false;
            finishRadio(job); break;
          }
        }
        if (job.ioPending && (job.io.waitKind == BotIoRequest::TextWait || job.io.waitKind == BotIoRequest::ChannelWait) && job.overflow) {
          job.result.truncated = true;
          finishRadio(job, "Native packet wait truncated; matching packet may have been dropped");
        }
      }
    }
  }
  bool collecting() const {
    for (const auto &invocation : invocations) if (invocation.collecting || invocation.deferred) return true;
    return false;
  }
  void observe(Invocation &invocation, const BotPath &path) {
    auto &pending = invocation.event;
    if (!path.valid()) {
      pending.truncated = true;
      ++stats.observationsDropped;
      return;
    }
    for (unsigned i = 0; i < pending.observationCount; ++i)
      if (samePath(path, pending.observations[i])) return;
    if (pending.observationCount == BotObservationLimit) {
      pending.truncated = true;
      ++stats.observationsDropped;
      return;
    }
    pending.observations[pending.observationCount++] = path;
  }
  uint32_t reservedAirtime(uint32_t now) const {
    uint32_t used = 0;
    for (const auto &c : credit)
      if (c.used && uint32_t(now - c.at) < 60000) used += c.airtime;
    if (noticeCredit.used && uint32_t(now - noticeCredit.at) < 60000)
      used += noticeCredit.airtime;
    return used;
  }
  bool capacity(uint32_t estimate, bool charge, bool notice = false,
                const mesh::Packet *packet = nullptr, const uint8_t *key = nullptr) {
    capacityError = "Bot configured TX airtime budget exceeded";
    const uint32_t now = millis(), used = reservedAirtime(now);
    Credit *slot = nullptr;
    for (auto &c : credit) {
      const bool current = c.used && uint32_t(now - c.at) < 60000;
      if (current && now / 1000 == c.at / 1000) { slot = &c; break; }
      if (!current && !slot) slot = &c;
    }
    if (notice) slot = !noticeCredit.used || uint32_t(now - noticeCredit.at) >= 60000 ?
        &noticeCredit : nullptr;
    if (!slot || estimate > policy.airtimeMs || used > policy.airtimeMs - estimate) return false;
    if (charge && packet) {
      static const uint8_t service[32]{};
      const auto decision = adaptive.reserve(now, key ? key : service, packet, estimate, sampleAdmission());
      if (decision != AdaptiveAdmission::Allowed) {
        capacityError = AdaptiveAdmission::name(decision);
        lastAdmission = capacityError; admissionAt = now; admissionWait = 0;
        return false;
      }
    }
    if (charge) {
      if (!notice && slot->used && uint32_t(now - slot->at) < 60000 &&
          now / 1000 == slot->at / 1000) {
        slot->airtime += estimate;
        slot->at = now;
      } else {
        *slot = {now, estimate, true};
      }
    }
    return true;
  }
  uint32_t creditWait(uint32_t now) const {
    uint32_t wait = 60000;
    for (const auto &entry : credit)
      if (entry.used && uint32_t(now - entry.at) < 60000)
        wait = std::min(wait, 60000 - uint32_t(now - entry.at));
    return wait;
  }
  void rejectAdmission(const BotEvent &event, mesh::Packet *request, Contact *sender,
                       const uint8_t *secret, const char *gate, uint32_t wait) {
    const uint32_t now = millis();
    ++stats.rejected;
    lastAdmission = gate; admissionAt = now; admissionWait = wait;
    const bool loadGate = !strncmp(gate, "adaptive ", 9);
    if (loadGate) owner.diagnostic("Bot command not run: %s; inspect bot adaptive; no automatic retry\n", gate);
    else owner.diagnostic("On-chip command admission: %s; retry in %u ms\n", gate, wait);
    if ((noticeSent && uint32_t(now - noticeAt) < 60000) || !owner.radio_.queuedReady()) {
      ++stats.noticesSuppressed; return;
    }
    char text[96];
    if (loadGate) snprintf(text, sizeof(text), "Not run: %s; inspect bot adaptive. No automatic retry.", gate);
    else snprintf(text, sizeof(text), "Not run: %s; retry in %us. Notices max 1/min.",
                  gate, unsigned((wait + 999) / 1000));
    PeerRoute route;
    mesh::Packet *response;
    if (sender) {
      route = sender->route;
      response = privateReply(text, sender->id.pub_key, secret);
    } else {
      route.scoped = routing.scope(request);
      response = channelReply(text, channel);
    }
    if (!response) { ++stats.noticesSuppressed; return; }
    if (!capacity(replyAirtime(response, route), true, true)) {
      releasePacket(response); ++stats.noticesSuppressed; return;
    }
    noticeSent = true; noticeAt = now;
    rememberOutbound(response);
    routeReply(response, event, route);
    ++stats.notices;
  }
  bool acknowledge(mesh::Packet *request, Contact &sender, const uint8_t *secret,
                   const uint8_t *data, size_t textSize, size_t size) {
    uint8_t ack[6]{};
    mesh::Utils::sha256(ack, 4, data, textSize + 5, sender.id.pub_key, 32);
    if (sender.ackSent && uint32_t(millis() - sender.ackAt) < 1000 &&
        !memcmp(sender.ackHash, ack, sizeof(sender.ackHash))) return true;
    if (textSize + 6 < size) ack[4] = data[textSize + 6];
    getRNG()->random(ack + 5, 1);
    auto *packet = mesh::chooseReplyRoute(request->isRouteFlood(), false,
                                         sender.route.length != 0xff) == mesh::REPLY_ROUTE_PATH_RETURN
        ? createPathReturn(sender.id, secret, request->path, request->path_len,
                           PAYLOAD_TYPE_ACK, ack, sizeof(ack))
        : createAck(ack, sizeof(ack));
    if (!packet) { reject("Bot native ACK packet unavailable"); return false; }
    const unsigned path = !request->isRouteFlood() && sender.route.length != 0xff
        ? (sender.route.length & 63u) * ((sender.route.length >> 6) + 1u) : 0;
    if (!capacity(owner.radio_.getEstAirtimeFor(2 + packet->payload_len + path +
                                              (sender.route.scoped ? 4 : 0)), true)) {
      releasePacket(packet); reject("Bot ACK airtime budget exceeded"); return false;
    }
    if (request->isRouteFlood())
      routing.flood(*this, packet, sender.route.scoped, request->getPathHashSize());
    else routing.send(*this, packet, sender.route, policy.pathWidth);
    sender.ackSent = true;
    sender.ackAt = millis();
    memcpy(sender.ackHash, ack, sizeof(sender.ackHash));
    return true;
  }
  void nativeRequest(mesh::Packet *request, Contact &sender, const uint8_t *secret,
                     const uint8_t *data, size_t size) {
    if (request->_localReflection || sender.id.matches(self_id.pub_key)) return;
    // sendRequest(type) and companion path discovery both encrypt 13 bytes,
    // zero padded to one AES block. Short binary telemetry forms also pad here.
    if (size != 16 || data[6] || data[7] || data[8] || data[13] || data[14] || data[15]) {
      ++stats.malformed; owner.fault("Bot native request has invalid length/reserved bytes"); return;
    }
    const auto now = millis();
    // Normal permission/readiness/contention denials must preserve actual
    // fault diagnostics and must not turn repeated app requests into log spam.
    if (data[4] != NativeTelemetryRequest || !discovery ||
        !(uint8_t(~data[5]) & TELEM_PERM_BASE) || !initialized ||
        administratorBlocked || !owner.radio_.queuedReady() ||
        (discoverySent && uint32_t(now - discoveryAt) < 1000)) {
      ++stats.rejected; return;
    }
    uint8_t body[12]{};
    memcpy(body, data, 4);
    // The host has no board sensors. Retain zero padding even at an exact
    // PATH block boundary: the companion requires response extra_len > 4.
    uint8_t length = 5;
#if defined(ARDUINO_ARCH_ESP32) || defined(ONCHIP_BOT_RUNTIME_TEST) || defined(NRF52_PLATFORM)
    CayenneLPP telemetry(sizeof(body) - 4);
    if (!telemetry.getBuffer()) { reject("Bot base telemetry allocation failed"); return; }
    const auto battery = board.getBattMilliVolts();
    if (battery) telemetry.addVoltage(TELEM_CHANNEL_SELF, battery / 1000.0f);
    const float temperature = board.getMCUTemperature();
    if (std::isfinite(temperature)) telemetry.addTemperature(TELEM_CHANNEL_SELF, temperature);
    if (telemetry.getError() != LPP_ERROR_OK) { reject("Bot base telemetry exceeds reply capacity"); return; }
    length = 4 + telemetry.getSize();
    memcpy(body + 4, telemetry.getBuffer(), telemetry.getSize());
#endif
    const bool pathReturn = mesh::chooseReplyRoute(request->isRouteFlood(), false,
        sender.route.length != 0xff) == mesh::REPLY_ROUTE_PATH_RETURN;
    auto *response = pathReturn
        ? createPathReturn(sender.id, secret, request->path, request->path_len,
                           PAYLOAD_TYPE_RESPONSE, body, length)
        : createDatagram(PAYLOAD_TYPE_RESPONSE, sender.id, secret, body, length);
    if (!response) { ++stats.rejected; return; }
    PeerRoute route = sender.route;
    route.scoped = routing.scope(request);
    if (pathReturn) route.length = 0xff;
    if (!capacity(replyAirtime(response, route), true)) {
      releasePacket(response); ++stats.rejected; return;
    }
    sender.route.scoped = route.scoped;
    sender.heard = now;
    discoverySent = true; discoveryAt = now;
    if (pathReturn)
      routing.flood(*this, response, route.scoped, request->getPathHashSize(), 300);
    else routing.send(*this, response, route, policy.pathWidth, 300);
    ++stats.replies;
  }
  void onPeerDataRecv(mesh::Packet *packet, uint8_t type, int index,
                      const uint8_t *secret, uint8_t *data, size_t size) override {
    if (type == PAYLOAD_TYPE_RESPONSE) {
      repeaterResponse(packet, contacts[matches[index]], data, size);
      return;
    }
    if (type == PAYLOAD_TYPE_REQ) {
      nativeRequest(packet, contacts[matches[index]], secret, data, size);
      return;
    }
    if (type != PAYLOAD_TYPE_TXT_MSG || size < 6 || (data[4] >> 2) != 0) {
      ++stats.malformed; owner.fault("Bot accepts plain native text only"); return;
    }
    const auto *end = static_cast<const uint8_t *>(memchr(data + 5, 0, size - 5));
    const size_t length = end ? size_t(end - data - 5) : size - 5;
    auto &sender = contacts[matches[index]];
    sender.route.scoped = routing.scope(packet);
    if (!packet->_localReflection && !sender.id.matches(self_id.pub_key))
      acknowledge(packet, sender, secret, data, length, size);
    if (!length) return;
    if (length > BotReplyLimit) {
      ++stats.malformed; owner.fault("Bot native text exceeds capacity"); return;
    }
#if defined(MESHCORE_HOST_BOT_SOURCE) && MESHCORE_HOST_BOT_SOURCE
    ownerMessage(sender, packet, data, length);
#endif
    if (data[5] != '!') {
      if (packet->_localReflection || sender.id.matches(self_id.pub_key)) return;
      for (size_t i = 0; i < length; ++i)
        if (data[5 + i] < 32 || data[5 + i] > 126) {
          ++stats.malformed; owner.fault("Follow-up DM requires printable text"); return;
        }
      uint8_t hash[16];
      uint32_t timestamp;
      memcpy(&timestamp, data, 4);
      uint8_t canonical[4 + BotReplyLimit];
      memcpy(canonical, data, 4); memcpy(canonical + 4, data + 5, length);
      mesh::Utils::sha256(hash, sizeof(hash), canonical, length + 4, sender.id.pub_key, 32);
      BotEvent event;
      event.authenticated = true; event.timestamp = timestamp;
      memcpy(event.sender, sender.id.pub_key, 32); memcpy(event.request, hash, sizeof(hash));
      messageEvent(packet, event, reinterpret_cast<const char *>(data + 5), length);
      bool ack = false;
      for (auto &job : invocations) if (job.used && job.event.authenticated && !job.event.local &&
                                      !job.event.channel[0] &&
                                      ((!memcmp(job.event.sender, sender.id.pub_key, 32) && timestamp >= job.event.timestamp) ||
                                       (job.event.meshGrant == meshGrant && meshPolicy.allows(sender.id.pub_key) &&
                                        !memcmp(job.dmTarget, sender.id.pub_key, 32)))) {
        bool duplicate = false;
        for (unsigned i = 0; i < job.hashCount; ++i)
          duplicate = duplicate || !memcmp(job.hashes[i], hash, sizeof(hash));
        if (!duplicate) {
          if (job.hashCount == BotReceiveDedupLimit) { job.overflow = true; continue; }
          memcpy(job.hashes[job.hashCount++], hash, sizeof(hash));
          if (job.rxCount == BotReceiveLimit || job.nextPacket == UINT32_MAX) job.overflow = true;
          else {
            auto &copy = job.received[job.rxCount++];
            copy = {};
            memcpy(copy.text, data + 5, length); copy.text[length] = 0;
            memcpy(copy.digest, hash, sizeof(hash));
            copy.at = millis(); copy.info.id = ++job.nextPacket;
            copy.info.timestamp = timestamp; copy.info.authenticated = true;
            memcpy(copy.info.sender, sender.id.pub_key, 32);
            copy.info.routeType = packet->getRouteType();
            copy.info.forwardable = strncmp(copy.text, "Fwd ", 4) != 0;
            if (packet->isRouteFlood()) {
              copy.info.path.known = true;
              copy.info.path.width = packet->getPathHashSize();
              copy.info.path.count = packet->getPathHashCount();
              memcpy(copy.info.path.bytes, packet->path, copy.info.path.size());
              for (unsigned i = 0; i < copy.info.path.count; ++i)
                if (!memcmp(packet->path + i * copy.info.path.width, self_id.pub_key, copy.info.path.width))
                  copy.info.forwardable = false;
            }
            const auto *signal = packets.reception(packet);
            copy.info.signal = signal && std::isfinite(signal->rssi) && std::isfinite(signal->snr) &&
                               signal->rssi >= -150 && signal->rssi <= 0 && signal->snr >= -32 && signal->snr <= 32;
            if (copy.info.signal) { copy.info.rssi = signal->rssi; copy.info.snr = signal->snr; }
          }
        }
        if (uint32_t(millis() - job.rxAckAt) >= 1000) { job.rxAckAt = millis(); ack = true; }
      }
#ifdef NRF52_PLATFORM
      // The secured BLE carrier copies this DM but does not ACK it separately.
      ack = true;
#endif
      if (ack) acknowledge(packet, sender, secret, data, length, size);
      return;
    }
    BotEvent event{};
    event.local = packet->_localReflection;
    memcpy(event.sender, sender.id.pub_key, 32);
    memcpy(&event.timestamp, data, 4);
    // Retry flags and encrypted block padding are deliberately excluded.
    uint8_t canonical[4 + BotTextLimit]{};
    if (length > sizeof(canonical) - 4) {
      ++stats.malformed; owner.fault("Bot command text exceeds capacity"); return;
    }
    memcpy(canonical, data, 4); memcpy(canonical + 4, data + 5, length);
    mesh::Utils::sha256(event.request, sizeof(event.request), canonical, length + 4,
                        sender.id.pub_key, 32);
    event.authenticated = true;
    receiveCommand(packet, event, reinterpret_cast<const char *>(data + 5), length,
                   &sender, secret, data, size);
    messageEvent(packet, event, reinterpret_cast<const char *>(data + 5), length);
  }
  void onGroupDataRecv(mesh::Packet *packet, uint8_t type, const mesh::GroupChannel &matched,
                       uint8_t *data, size_t size) override {
    if (type != PAYLOAD_TYPE_GRP_TXT || size < 6 || (data[4] >> 2) != 0 ||
        memcmp(matched.secret, channel.secret, sizeof(channel.secret))) {
      ++stats.malformed; owner.fault("Malformed native channel text"); return;
    }
    const auto *end = static_cast<const uint8_t *>(memchr(data + 5, 0, size - 5));
    const size_t length = end ? size_t(end - data - 5) : size - 5;
    const auto *colon = static_cast<const uint8_t *>(memchr(data + 5, ':', length));
    if (!colon || colon == data + 5 || size_t(colon - data - 5) > 31 ||
        size_t(colon - data - 5) + 2 >= length || colon[1] != ' ') {
      ++stats.malformed; owner.fault("Malformed native channel sender prefix"); return;
    }
    const size_t nicknameSize = size_t(colon - data - 5);
    for (size_t i = 0; i < nicknameSize; ++i)
      if (data[5 + i] < 32 || data[5 + i] > 126) {
        ++stats.malformed; owner.fault("Invalid channel nickname"); return;
      }
    BotEvent event{};
    event.local = packet->_localReflection;
    memcpy(event.nickname, data + 5, nicknameSize);
    strcpy(event.channel, policy.channel);
    static const uint8_t domain[] = "meshcore-bot-channel-v1";
    mesh::Utils::sha256(event.channelId, sizeof(event.channelId), domain, sizeof(domain) - 1,
                        matched.secret, sizeof(matched.secret));
    event.channelVerified = true;
    event.replyLimit = BotReplyLimit - strlen(meshPolicy.name) - 2;
    memcpy(&event.timestamp, data, 4);
    uint8_t canonical[MAX_PACKET_PAYLOAD]{};
    memcpy(canonical, data, 4); memcpy(canonical + 4, data + 5, length);
    mesh::Utils::sha256(event.request, sizeof(event.request), canonical, length + 4,
                        channel.secret, sizeof(channel.secret));
    const char *message = reinterpret_cast<const char *>(colon + 2);
    const size_t messageSize = length - nicknameSize - 2;
    if (!event.local && messageSize >= 13 && !strncmp(message, "[q:", 3) && message[11] == ']' && message[12] == ' ') {
      for (auto &job : invocations) if (job.used && job.deferred) {
        char marker[14] = "[q:";
        for (unsigned i = 0; i < 4; ++i) snprintf(marker + 3 + 2 * i, 3, "%02x", job.event.request[i]);
        strcpy(marker + 11, "] ");
        if (!memcmp(marker, message, 13)) {
          ++stats.readSuppressed; wipe(&job, sizeof(job));
        }
      }
    }
    if (colon[2] == '!' && !event.local)
      receiveCommand(packet, event, reinterpret_cast<const char *>(colon + 2),
                     length - nicknameSize - 2, nullptr, nullptr, nullptr, 0);
    else if (!event.local && meshPolicy.channelWait && messageSize <= BotReplyLimit) {
      bool printable = true;
      for (size_t i = 0; i < messageSize; ++i) printable = printable && message[i] >= 32 && message[i] <= 126;
      if (!printable) { ++stats.malformed; owner.fault("Channel follow-up requires printable text"); return; }
      for (auto &job : invocations) if (job.used && !job.cancelled && job.event.channelVerified &&
                                       job.event.meshGrant == meshGrant && job.event.channelWait &&
                                       !memcmp(job.event.channelId, event.channelId, 32)) {
        bool duplicate = false;
        for (unsigned i = 0; i < job.hashCount; ++i)
          duplicate = duplicate || !memcmp(job.hashes[i], event.request, 16);
        if (duplicate) continue;
        if (job.hashCount == BotReceiveDedupLimit) { job.overflow = true; continue; }
        memcpy(job.hashes[job.hashCount++], event.request, 16);
        if (job.rxCount == BotReceiveLimit || job.nextPacket == UINT32_MAX) { job.overflow = true; continue; }
        auto &copy = job.received[job.rxCount++];
        copy = {}; memcpy(copy.text, message, messageSize);
        copy.at = millis(); copy.info.id = ++job.nextPacket;
        copy.info.timestamp = event.timestamp; copy.info.channel = true;
        strcpy(copy.info.nickname, event.nickname);
        copy.info.routeType = packet->getRouteType();
        if (packet->isRouteFlood()) {
          copy.info.path.known = true; copy.info.path.width = packet->getPathHashSize();
          copy.info.path.count = packet->getPathHashCount();
          memcpy(copy.info.path.bytes, packet->path, copy.info.path.size());
        }
        const auto *signal = packets.reception(packet);
        copy.info.signal = signal && std::isfinite(signal->rssi) && std::isfinite(signal->snr);
        if (copy.info.signal) { copy.info.rssi = signal->rssi; copy.info.snr = signal->snr; }
      }
    }
    messageEvent(packet, event, reinterpret_cast<const char *>(colon + 2), length - nicknameSize - 2);
  }
  void receiveCommand(mesh::Packet *packet, BotEvent &event, const char *text, size_t length,
                      Contact *sender, const uint8_t *secret, const uint8_t *data, size_t size) {
    const size_t originalLength = length;
    char targeted[BotTextLimit + 1]{};
    if (packet->isRouteFlood()) {
      event.path.known = true;
      event.path.width = packet->getPathHashSize();
      event.path.count = packet->getPathHashCount();
      memcpy(event.path.bytes, packet->path, event.path.size());
    }
    const uint32_t now = millis();
    for (auto &invocation : invocations)
      if (invocation.collecting &&
          uint32_t(now - invocation.collectionStart) < invocation.event.windowMs &&
          !memcmp(event.request, invocation.event.request, sizeof(event.request)) && !event.local)
        observe(invocation, event.path);
    Seen *slot = nullptr;
    for (auto &entry : seen) {
      if (entry.used && uint32_t(now - entry.at) < 120000) {
        if (!memcmp(entry.request, event.request, sizeof(event.request))) {
          if (sender && uint32_t(now - entry.ackAt) >= 1000 &&
              acknowledge(packet, *sender, secret, data, originalLength, size)) entry.ackAt = now;
          ++stats.duplicates; return;
        }
      } else if (!slot) slot = &entry;
    }
    if (!slot) { reject("Bot dedup capacity exhausted"); return; }
    const auto syntaxError = [&](const char *message) {
      ++stats.malformed; reject(message);
      if (!sender || event.local || sender->id.matches(self_id.pub_key)) return;
      *slot = {};
      slot->used = true; slot->at = now; slot->ackAt = now;
      memcpy(slot->request, event.request, sizeof(event.request));
      char reply[BotReplyLimit + 1];
      snprintf(reply, sizeof(reply), "Error: %s", message);
      auto *response = privateReply(reply, sender->id.pub_key, secret);
      if (!response) { reject("Bot command error reply packet unavailable"); return; }
      if (!capacity(replyAirtime(response, sender->route), true, false, response, event.sender)) {
        releasePacket(response); reject(capacityError); return;
      }
      rememberOutbound(response);
      routeReply(response, event, sender->route);
      ++stats.replies;
    };
    if (length > 2 && text[1] == '@') {
      size_t end = 2;
      while (end < length && text[end] != ' ') ++end;
      const size_t digits = end - 2;
      if ((digits != 8 && digits != 64) || end == length) {
        syntaxError("Use !@BOTKEY8 COMMAND or !@FULLKEY COMMAND"); return;
      }
      char key[65];
      for (unsigned i = 0; i < 32; ++i) snprintf(key + 2 * i, 3, "%02x", self_id.pub_key[i]);
      for (size_t i = 0; i < digits; ++i) {
        char c = text[i + 2];
        if (c >= 'A' && c <= 'F') c += 'a' - 'A';
        if (c != key[i]) {
          if (sender) syntaxError("Target does not match this bot; use !help");
          return;
        }
      }
      while (end < length && text[end] == ' ') ++end;
      if (length - end + 1 > BotTextLimit) {
        syntaxError("Targeted command exceeds native capacity"); return;
      }
      targeted[0] = '!'; memcpy(targeted + 1, text + end, length - end);
      length = length - end + 1; text = targeted; event.targeted = true;
    }
    char error[128]{};
    if (!parseBotCommand(text, length, event,
                         error, sizeof(error))) {
      syntaxError(error); return;
    }
    if (event.channel[0] && !event.targeted && !botReadOnlyQuery(event.name)) {
      rejectAdmission(event, packet, sender, secret, "target required: !@BOTKEY8 COMMAND", 0); return;
    }
    const bool readQuery = event.channel[0] && !event.targeted && botReadOnlyQuery(event.name);
    if (readQuery) event.replyLimit -= 13;
    if (const char *error = owner.worker_.admissionError(event)) {
      reject(error);
      return;
    }
    if (administratorBlocked && !botReservedCommand(event.name)) {
      reject("Command source unavailable; use mast recovery"); return;
    }
    Invocation *invocation = nullptr;
    for (auto &candidate : invocations) {
      if (!candidate.used && !invocation) invocation = &candidate;
    }
    if (nextJob == UINT32_MAX) {
      reject("Bot job identifiers exhausted"); return;
    }
    if (!initialized || !invocation || !canInvoke(event) || sourceResultReady) {
      ++stats.busy;
      rejectAdmission(event, packet, sender, secret, "busy", 5000); return;
    }
    if (!capacity(0, false)) {
      ++stats.airtimeLimited;
      rejectAdmission(event, packet, sender, secret, "airtime budget", creditWait(now)); return;
    }
    const auto decision = admitWork(event);
    if (decision != AdaptiveAdmission::Allowed) {
      rejectAdmission(event, packet, sender, secret, AdaptiveAdmission::name(decision), 0);
      return;
    }
    *slot = {};
    slot->used = true; slot->at = now; slot->ackAt = now;
    memcpy(slot->request, event.request, sizeof(event.request));
    if (sender) sender->heard = now;
    event.routeType = packet->getRouteType();
    const auto *received = packets.reception(packet);
    if (!event.local && !received) owner.fault("Bot packet receive signal unavailable");
    event.rssi = received ? received->rssi : 0;
    event.snr = received ? received->snr : 0;
    event.signal = !event.local && received &&
                   std::isfinite(event.rssi) && std::isfinite(event.snr);
    if (!event.signal) event.rssi = event.snr = 0;
    const auto configuration = owner.radio_.configuration();
    event.frequency = configuration.freq_hz; event.bandwidth = configuration.bw_hz;
    event.sf = configuration.sf; event.cr = configuration.cr;
    owner.nodeSnapshot(event.node);
    mesh::QueuedRadioStats queue;
    if (owner.radio_.getQueuedRadioStats(queue)) {
      auto &a = event.air;
      a.available = true;
      a.generation = queue.generation; a.configurationGeneration = queue.configuration_generation;
      a.capturedMs = queue.captured_ms; a.queued = owner.radio_.queuedCount();
      a.aggregateQueued = queue.aggregate_queued; a.transmitting = queue.aggregate_transmitting;
      a.creditMs = queue.source_credit_ms; a.aggregateCreditMs = queue.aggregate_credit_ms;
      a.rfMs = queue.source_rf_ms; a.aggregateRfMs = queue.aggregate_rf_ms;
      a.successes = queue.source_successes; a.failures = queue.source_failures;
      a.aggregateSuccesses = queue.aggregate_successes; a.aggregateFailures = queue.aggregate_failures;
      a.reservedMs = reservedAirtime(now); a.limitMs = policy.airtimeMs;
      a.remainingMs = a.reservedMs < a.limitMs ? a.limitMs - a.reservedMs : 0;
    }
    event.sharedState = (event.authenticated || event.channelVerified) && sharedState;
    event.homeAccess = event.authenticated && homeAccess && botHttpsConfigured() &&
                       (!botHttpsConfig().valid() || botHttpsProbeReady());
    event.forwardGrant = forwardGrant;
    event.forwardAccess = event.authenticated && !event.local && forwarding.enabled() &&
                          !memcmp(event.sender, forwarding.from, 32);
    event.owner = ownerKey(event);
    event.meshGrant = meshGrant; event.channelWait = meshPolicy.channelWait;
    memcpy(event.destinations, meshPolicy.destinations, sizeof(event.destinations));
    // Ordinary width-three paths cannot be reinterpreted as TRACE width-four.
    if (!event.routeExplicit && !event.local && event.path.valid() &&
        event.path.count && event.path.width <= 2) {
      event.routeWidth = event.path.width;
      event.routeSize = event.path.size();
      for (unsigned i = 0; i < event.path.count; ++i)
        memcpy(event.route + i * event.path.width,
               event.path.bytes + (event.path.count - 1 - i) * event.path.width,
               event.path.width);
    }
    *invocation = {};
    invocation->used = true; invocation->job = ++nextJob;
    invocation->event = event;
    if (sender) { invocation->route = sender->route; memcpy(invocation->secret, secret, 32); }
    else {
      invocation->route.scoped = routing.scope(packet);
      invocation->groupChannel = channel;
    }
    if (readQuery) {
      uint16_t random = 0;
      getRNG()->random(reinterpret_cast<uint8_t *>(&random), sizeof(random));
      invocation->deferred = true; invocation->notBefore = now + event.windowMs + 250 + random % 1001;
      ++stats.readJittered;
    }
    if (event.windowMs) {
      invocation->collectionStart = now;
      invocation->collecting = true;
      if (!event.local) observe(*invocation, event.path);
    } else if (!invocation->deferred) {
      if (!owner.worker_.invoke(event, invocation->job)) {
        *slot = {}; wipe(invocation, sizeof(*invocation));
        reject("Bot worker admission failed"); return;
      }
    }
    if (sender) acknowledge(packet, *sender, secret, data, originalLength, size);
  }
  mesh::Packet *channelReply(const char *reply, const Invocation &invocation) {
    if (!invocation.event.targeted && botReadOnlyQuery(invocation.event.name)) {
      char marked[BotReplyLimit + 1] = "[q:";
      for (unsigned i = 0; i < 4; ++i) snprintf(marked + 3 + 2 * i, 3, "%02x", invocation.event.request[i]);
      strcpy(marked + 11, "] ");
      if (strlen(reply) + 13 > BotReplyLimit) return nullptr;
      strcpy(marked + 13, reply);
      return channelReply(marked, invocation.groupChannel);
    }
    return channelReply(reply, invocation.groupChannel);
  }
  mesh::Packet *channelReply(const char *reply, const mesh::GroupChannel &groupChannel) {
    uint8_t text[5 + BotTextLimit]{};
    const size_t prefix = strlen(meshPolicy.name) + 2;
    const size_t length = strlen(reply);
    if (!length || length + prefix > BotTextLimit) return nullptr;
    uint32_t timestamp;
    if (!messageTimestamp(timestamp)) return nullptr;
    memcpy(text, &timestamp, 4);
    memcpy(text + 5, meshPolicy.name, prefix - 2);
    text[5 + prefix - 2] = ':'; text[5 + prefix - 1] = ' ';
    memcpy(text + 5 + prefix, reply, length);
    return createGroupDatagram(PAYLOAD_TYPE_GRP_TXT, groupChannel,
                               text, 5 + prefix + length);
  }
  mesh::Packet *privateReply(const char *reply, const uint8_t recipient[32], const uint8_t secret[32]) {
    uint8_t text[5 + BotReplyLimit]{};
    uint32_t timestamp;
    if (!messageTimestamp(timestamp)) return nullptr;
    memcpy(text, &timestamp, 4);
    const size_t size = strlen(reply);
    if (!size || size > BotReplyLimit) return nullptr;
    memcpy(text + 5, reply, size);
    return createDatagram(PAYLOAD_TYPE_TXT_MSG, mesh::Identity(recipient), secret, text, 5 + size);
  }
  void routeReply(mesh::Packet *packet, const Invocation &invocation) {
    routeReply(packet, invocation.event, invocation.route);
  }
  void routeReply(mesh::Packet *packet, const BotEvent &event, const PeerRoute &route) {
    const uint8_t width = event.channel[0] ? policy.pathWidth :
        event.path.valid() ? event.path.width : policy.pathWidth;
    routing.send(*this, packet, route, width);
  }
  uint32_t replyAirtime(const mesh::Packet *packet, const PeerRoute &route, unsigned traceBytes = 0) {
    const unsigned path = route.length != 0xff ?
        (route.length & 63u) * ((route.length >> 6) + 1u) : 0;
    return owner.radio_.getEstAirtimeFor(
        2 + packet->payload_len + (traceBytes ? traceBytes : path) + (route.scoped ? 4 : 0));
  }
  bool act(const BotAction &action, const Invocation &invocation) {
    const auto &pending = invocation.event;
    if (!owner.radio_.queuedReady()) {
      reject("Bot shared radio source unavailable/busy"); return false;
    }
    mesh::Packet *response = nullptr;
    unsigned traceBytes = 0;
    if (action.kind == BotAction::Reply) {
      if (strlen(action.text) > pending.replyLimit) {
        reject("Bot reply exceeds native context text capacity"); return false;
      }
      if (pending.channel[0]) response = channelReply(action.text, invocation);
      else response = privateReply(action.text, pending.sender, invocation.secret);
    } else if (action.kind == BotAction::Trace && pending.routeSize) {
      uint32_t tag, auth;
      getRNG()->random(reinterpret_cast<uint8_t *>(&tag), sizeof(tag));
      getRNG()->random(reinterpret_cast<uint8_t *>(&auth), sizeof(auth));
      const uint8_t flags = pending.routeWidth == 1 ? 0 : pending.routeWidth == 2 ? 1 :
                            pending.routeWidth == 4 ? 2 : 3;
      response = createTrace(tag, auth, flags);
      traceBytes = pending.routeSize;
    }
    if (!response) {
      reject("Bot native response packet unavailable"); return false;
    }
    const uint32_t cost = replyAirtime(response, invocation.route, traceBytes);
    if (!capacity(cost, true, false, response, principal(pending))) {
      releasePacket(response);
      reject(capacityError); return false;
    }
    if (invocation.adminTicket) {
      adminReplyPacket = response; adminReplyTicket = invocation.adminTicket;
    }
    if (traceBytes) {
      sendDirect(response, pending.route, pending.routeSize);
      ++stats.traces;
    } else {
      rememberOutbound(response);
      routeReply(response, invocation);
      ++stats.replies;
    }
    owner.status_.fault[0] = 0;
    return true;
  }
};

#if defined(MESHCORE_HOST_BOT_SOURCE) && MESHCORE_HOST_BOT_SOURCE
void CommandBot::Core::ownerSend(const uint8_t request[16], const uint8_t recipient[32],
                                 const uint8_t *text, size_t size) {
  const auto fail = [&](uint8_t reason) {
    if (ownerSendSink) ownerSendSink(request, 5, 0, reason);
  };
  if (!size || size > BotReplyLimit) { fail(1); return; }
  for (size_t i = 0; i < size; ++i)
    if (text[i] < 32 || text[i] > 126) { fail(1); return; }
  OwnerSend *slot = nullptr;
  for (auto &entry : ownerSends) {
    if (entry.used && !memcmp(entry.id, request, 16)) {
      ownerState(entry, entry.state, entry.reason); return;
    }
    if (!slot && (!entry.used || (entry.state >= 4 && !entry.packet))) slot = &entry;
  }
  if (!slot || !initialized || !owner.radio_.queuedReady()) { fail(3); return; }
  auto *contact = freshDirectContact(recipient);
  if (!contact || contact->id.matches(self_id.pub_key) || routeLoops(contact->route)) { fail(2); return; }
  uint8_t plain[5 + BotReplyLimit]{}, secret[32]{};
  uint32_t timestamp;
  if (!messageTimestamp(timestamp)) { fail(4); return; }
  memcpy(plain, &timestamp, 4); memcpy(plain + 5, text, size);
  self_id.calcSharedSecret(secret, contact->id);
  auto *packet = createDatagram(PAYLOAD_TYPE_TXT_MSG, contact->id, secret, plain, 5 + size);
  wipe(secret, sizeof(secret));
  if (!packet) { fail(3); return; }
  if (!capacity(replyAirtime(packet, contact->route), true, false, packet, recipient)) {
    releasePacket(packet); fail(5); return;
  }
  *slot = {};
  slot->used = true; slot->at = millis(); slot->packet = packet; slot->flags = 4;
  memcpy(slot->id, request, 16);
  mesh::Utils::sha256(reinterpret_cast<uint8_t *>(&slot->ack), 4, plain, 5 + size, self_id.pub_key, 32);
  ownerState(*slot, 1);
  rememberOutbound(packet);
  routing.send(*this, packet, contact->route, policy.pathWidth);
}

void CommandBot::Core::ownerMessage(const Contact &sender, mesh::Packet *packet,
                                    const uint8_t *data, size_t size) {
  if (!ownerInboxSink || packet->_localReflection || sender.id.matches(self_id.pub_key)) return;
  for (size_t i = 0; i < size; ++i) if (data[5 + i] < 32 || data[5 + i] > 126) return;
  uint8_t canonical[4 + BotReplyLimit], hash[16];
  memcpy(canonical, data, 4); memcpy(canonical + 4, data + 5, size);
  mesh::Utils::sha256(hash, sizeof(hash), canonical, size + 4, sender.id.pub_key, 32);
  OwnerSeen *slot = nullptr;
  for (auto &entry : ownerSeen) {
    if (entry.used && uint32_t(millis() - entry.at) < 120000) {
      if (!memcmp(entry.hash, hash, sizeof(hash))) return;
    } else if (!slot) slot = &entry;
  }
  if (!slot) { ownerInboxSink(nullptr, 0, nullptr, 0); return; }
  slot->used = true; slot->at = millis(); memcpy(slot->hash, hash, sizeof(hash));
  uint32_t timestamp;
  memcpy(&timestamp, data, 4);
  ownerInboxSink(sender.id.pub_key, timestamp, data + 5, size);
}

void CommandBot::setOwnerSinks(OwnerSendSink send, OwnerInboxSink inbox) {
  if (core_) { core_->ownerSendSink = send; core_->ownerInboxSink = inbox; }
}
void CommandBot::ownerSend(const uint8_t request[16], const uint8_t recipient[32],
                           const uint8_t *text, size_t size) {
  if (core_) core_->ownerSend(request, recipient, text, size);
}
#endif

void CommandBot::admissionStatus(char *text, size_t capacity) const {
  if (!core_) { snprintf(text, capacity, "Bot inactive"); return; }
  const uint32_t age = uint32_t(millis() - core_->admissionAt);
  unsigned active = 0;
  for (const auto &job : core_->invocations) if (job.used) ++active;
  const auto &s = core_->stats;
#if ONCHIP_BOT_SINGLE_SESSION
  const char *pause = core_->initializing ? "initialization" :
      worker_.sourceSuspended() ? "source-unloaded" :
      (core_->administratorBlocked || !worker_.runtimeAdmissionEnabled(false)) ? "source-publication" :
      core_->sourceResultReady ? "source-result" : "none";
#endif
  snprintf(text, capacity,
#if ONCHIP_BOT_SINGLE_SESSION
           "pause=%s cooldown-ms=0 "
#endif
           "Last=%s wait-ms=%u active=%u sender=%u channel=%u global=%u airtime=%u busy=%u",
#if ONCHIP_BOT_SINGLE_SESSION
           pause,
#endif
           core_->lastAdmission, age < core_->admissionWait ? core_->admissionWait - age : 0,
           active, s.senderLimited, s.channelLimited, s.globalLimited, s.airtimeLimited, s.busy);
}
void CommandBot::adaptiveCommand(const char *command, char *reply, size_t capacity) {
  if (!strcmp(command, "bot adaptive on") || !strcmp(command, "bot adaptive off")) {
    const bool enabled = !strcmp(command, "bot adaptive on");
    const bool saved = saveBotAdaptiveAdmission(enabled);
    if (saved) {
      adaptivePolicyFault_ = false;
      if (!strcmp(status_.fault, AdaptivePolicyFault)) status_.fault[0] = 0;
    }
    snprintf(reply, capacity, "%s", saved ?
        "Saved adaptive admission; reboot required" :
        "Error: adaptive admission save/readback failed; inspect saved selection");
    return;
  }
  if (strcmp(command, "bot adaptive")) {
    snprintf(reply, capacity, "Error: use bot adaptive [on|off]"); return;
  }
  bool saved = false;
  const bool readable = loadBotAdaptiveAdmission(saved);
  if (!readable) {
    adaptivePolicyFault_ = true;
    fault(AdaptivePolicyFault);
  }
  if (adaptivePolicyFault_) {
    snprintf(reply, capacity, "Error: adaptive policy %s; live=%u policy-fault=1; use bot adaptive off then reboot",
             readable ? "fault retained" : "unreadable", core_ && core_->adaptive.enabled());
    return;
  }
  if (!core_) {
    snprintf(reply, capacity, "Adaptive saved=%u live=0; bot inactive", saved); return;
  }
  const bool metrics = core_->sampleAdmission();
  const auto &a = core_->adaptive;
  snprintf(reply, capacity,
           "Adaptive saved=%u live=%u metrics=%u congested=%u local-load-permille=%u allowance-permille=%u callers=%u pending=%u denied=%u last=%s",
           saved, a.enabled(), metrics, a.congested(), a.loadPermille(), a.scalePermille(),
           a.callers(), a.pending(), a.denied(), AdaptiveAdmission::name(a.last()));
}

const size_t CommandBot::StorageBytes = sizeof(Core);
#ifdef NRF52_PLATFORM
const char *CommandBot::botName() const { return core_ ? core_->meshPolicy.name : nullptr; }
#endif

void CommandBot::fault(const char *message) {
  sourceLifecycleFault_ = false;
  snprintf(status_.fault, sizeof(status_.fault), "%s", message);
  diagnostic("On-chip command bot: %s\n", message);
}
void CommandBot::diagnostic(const char *format, ...) {
  char text[DiagnosticCapacity];
  va_list args;
  va_start(args, format);
  const int size = vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  if (size < 0 || size_t(size) >= sizeof(text)) { ++diagnosticsDropped_; return; }
  if (diagnosticSink_) {
    if (diagnosticSink_(text)) ++diagnosticsQueued_;
    else ++diagnosticsDropped_;
  } else {
#ifdef ARDUINO_ARCH_ESP32
    // Never fall back to USB on the radio/management dispatch thread.
    ++diagnosticsDropped_;
#else
    Serial.write(reinterpret_cast<const uint8_t *>(text), size_t(size));
#endif
  }
}
void CommandBot::diagnosticStatus(char *text, size_t capacity) const {
  snprintf(text, capacity, "Diagnostics queued=%u dropped=%u sink=%s; counters since startup, USB delivery unconfirmed",
           diagnosticsQueued_, diagnosticsDropped_, diagnosticSink_ ? "async" :
#ifdef ARDUINO_ARCH_ESP32
           "unavailable"
#else
           "host"
#endif
  );
}
#if defined(MESHCORE_HOST_BOT_SOURCE) && MESHCORE_HOST_BOT_SOURCE
bool CommandBot::beginHost(const RadioConfig &profile, const uint32_t airtime[256], int noiseFloor) {
#elif defined(NRF52_PLATFORM)
bool CommandBot::begin(nrfmast::RadioPort &port, const RadioConfig &profile) {
#else
bool CommandBot::begin(WifiKissMultiplexer &mux) {
#endif
  if (core_ || (instance && instance != this)) {
    fault("Only one on-device command bot is supported"); return false;
  }
  status_ = {};
  adaptivePolicyFault_ = false;
  strcpy(status_.role, "command-bot");
  strcpy(status_.name, ONCHIP_COMMAND_BOT_NAME);
#if defined(MESHCORE_HOST_BOT_SOURCE) && MESHCORE_HOST_BOT_SOURCE
  enabled_ = true;  // Go owns host role selection; do not use the firmware boot selector.
#else
  if (!loadBotEnabled(enabled_)) { fault("Command bot selection unavailable"); return false; }
#endif
  BotMeshPolicy meshPolicy;
  if (!loadBotMeshPolicy(meshPolicy)) { fault("Command mesh policy unavailable"); return false; }
  snprintf(status_.name, sizeof(status_.name), "%.31s", meshPolicy.name);
  if (!enabled_) { strcpy(status_.state, "disabled"); return true; }
  instance = this;
#if defined(MESHCORE_HOST_BOT_SOURCE) && MESHCORE_HOST_BOT_SOURCE
  const auto wall = std::time(nullptr);
  if (wall < 1715770351 || wall > 4102444800LL) {
    instance = nullptr; fault("Host UTC clock unavailable"); return false;
  }
  rtc_.synchronize(static_cast<uint32_t>(wall));
  if (!radio_.attachHost(profile, airtime, noiseFloor)) {
    instance = nullptr; fault("Command bot host source unavailable"); return false;
  }
#else
  rtc_.synchronize(ONCHIP_CLOCK_BUILD_EPOCH);
#ifdef NRF52_PLATFORM
  if (!radio_.attach(port, profile)) { instance = nullptr; fault("Command bot radio unavailable"); return false; }
#else
  if (!radio_.attach(mux)) { instance = nullptr; fault("Command bot source unavailable"); return false; }
#endif
#endif
  core_ = allocateRoleStorage<Core>("command bot", *this);
  if (!core_) {
    stop();
#ifdef NRF52_PLATFORM
    fault("Command bot runtime RAM unavailable");
#else
    fault("Command bot PSRAM unavailable");
#endif
    return false;
  }
#if defined(MESHCORE_HOST_BOT_SOURCE) && MESHCORE_HOST_BOT_SOURCE
  core_->administratorBlocked = true;
#else
  core_->administratorBlocked = false;
#endif
  mesh::LocalIdentity identity;
  if (!loadIdentity("command-bot", identity)) { stop(); fault("Command bot identity unavailable"); return false; }
  core_->self_id = identity; wipe(&identity, sizeof(identity));
  if (!loadBotRadioPolicy(core_->policy)) { stop(); fault("Command radio policy unavailable"); return false; }
  bool adaptive = false;
  if (!loadBotAdaptiveAdmission(adaptive)) {
    adaptivePolicyFault_ = true;
    adaptive = false;
    fault(AdaptivePolicyFault);
  }
  core_->adaptive.configure(adaptive, core_->policy.airtimeMs, millis());
  if (!loadBotDiscovery(core_->discovery)) { stop(); fault("Bot discovery policy unavailable"); return false; }
  if (!loadBotRepeaterPolicy(core_->repeaterPolicy)) {
    stop(); fault("Repeater monitor policy unavailable"); return false;
  }
  for (unsigned i = 0; i < BotRepeaterLimit; ++i) {
    if (core_->repeaterPolicy.targets[i].used &&
        core_->self_id.matches(core_->repeaterPolicy.targets[i].key)) {
      stop(); fault("Repeater monitor target is this bot's identity"); return false;
    }
    core_->repeaters[i].due = millis() + i * 30000;
  }
  core_->meshPolicy = meshPolicy;
  if (core_->policy.channel[0]) {
    if (core_->policy.channelKeySet) memcpy(core_->channel.secret, core_->policy.channelKey, 16);
    else mesh::Utils::sha256(core_->channel.secret, 16,
        reinterpret_cast<const uint8_t *>(core_->policy.channel), strlen(core_->policy.channel));
    mesh::Utils::sha256(core_->channel.hash, sizeof(core_->channel.hash), core_->channel.secret, 16);
  }
  if (!worker_.begin(core_->self_id.pub_key)) { stop(); fault("Command VM worker unavailable"); return false; }
  if (!loadBotSharedState(core_->sharedState)) { stop(); fault("Command state policy unavailable"); return false; }
  worker_.setSharedState(core_->sharedState);
  if (!loadBotHomeAccess(core_->homeAccess)) { stop(); fault("Home RPC policy unavailable"); return false; }
  worker_.setHomeAccess(core_->homeAccess && botHttpsConfigured());
  bool reminders;
  if (!loadBotReminderAccess(reminders) || !worker_.setReminderAccess(reminders)) {
    stop(); fault("Reminder grant unavailable"); return false;
  }
  uint8_t eventMask = 0;
  if (!loadBotEventAccess(eventMask) || !worker_.setEventAccess(eventMask)) {
    stop(); fault("Event grant unavailable"); return false;
  }
  if (!loadBotForwardPolicy(core_->forwarding)) { stop(); fault("Forward policy unavailable"); return false; }
  if (core_->forwarding.enabled() &&
      (core_->self_id.matches(core_->forwarding.from) || core_->self_id.matches(core_->forwarding.to))) {
    core_->forwarding = {};
    fault("Forward policy references bot identity; live forwarding disabled");
  }
  core_->begin();
  status_.has_identity = true;
  memcpy(status_.public_key, core_->self_id.pub_key, 32);
  strcpy(status_.state, "running");
  // Validate the bundled source on the worker before accepting commands.
  if (!worker_.stage(BotDefaultSource, strlen(BotDefaultSource))) {
    stop(); fault("Command VM initialization unavailable"); return false;
  }
  return true;
}
void CommandBot::stop() {
  if (core_) {
    core_->adminAcknowledged(core_->adminReplyTicket, false);
    for (const auto &job : core_->invocations) core_->adminAcknowledged(job.adminTicket, false);
  }
  worker_.stop();
  radio_.detach();
  releaseRoleStorage(core_);
  if (instance == this) instance = nullptr;
  status_.ready = false;
  status_.has_identity = false;
  memset(status_.public_key, 0, sizeof(status_.public_key));
  status_.source_slot = -1;
}
#if defined(MESHCORE_HOST_BOT_SOURCE) && MESHCORE_HOST_BOT_SOURCE
bool CommandBot::hostSourceResultReady() const { return core_ && core_->sourceResultReady; }
uint32_t CommandBot::hostWaitMs() const {
  if (!core_) return UINT32_MAX;
  if (radio_.hasBufferedEvents() || worker_.ownerWorkPending()) return 0;
  const uint32_t now = millis();
  uint32_t wait = core_->packets.nextInboundWait(now);
  if (core_->initializing && core_->initializationRetryAt)
    botEarlier(wait, now, core_->initializationRetryAt);
  for (const auto &job : core_->invocations) if (job.used) {
    if (job.collecting) botEarlier(wait, now, job.collectionStart + job.event.windowMs);
    else if (job.deferred) botEarlier(wait, now, job.notBefore);
    if (job.ioPending) botEarlier(wait, now, job.deadline);
  }
  if (core_->reminderActive) botEarlier(wait, now, core_->reminderDeadline);
  const uint32_t scheduled = worker_.scheduleSeconds();
  if (scheduled && core_->initialized && !core_->administratorBlocked && !core_->sourceResultReady)
    botEarlier(wait, now, core_->scheduledAt + scheduled * 1000);
  for (const auto &entry : core_->ownerSends)
    if (entry.used && entry.state < 4) botEarlier(wait, now, entry.at + 45000);
  if (worker_.eventMask() & 11) {
    BotNodeSnapshot snapshot;
    nodeSnapshot(snapshot);
    const bool startupPending = ((worker_.eventMask(BotWorker::Lua) & 1) &&
        core_->startupGeneration[BotWorker::Lua] != worker_.runtimeGeneration(BotWorker::Lua)) ||
        ((worker_.eventMask(BotWorker::Wasm) & 1) &&
        core_->startupGeneration[BotWorker::Wasm] != worker_.runtimeGeneration(BotWorker::Wasm));
    const bool pending = startupPending ||
        ((worker_.eventMask() & 2) && (!core_->connectivityKnown ||
          snapshot.wifiKnown != core_->previousEventNode.wifiKnown ||
          snapshot.wifiConnected != core_->previousEventNode.wifiConnected)) ||
        ((worker_.eventMask() & 8) && (!core_->nodeKnown ||
          snapshot.ready != core_->previousEventNode.ready ||
          snapshot.fault != core_->previousEventNode.fault ||
          snapshot.rolesKnown != core_->previousEventNode.rolesKnown ||
          snapshot.selectedRoles != core_->previousEventNode.selectedRoles ||
          snapshot.readyRoles != core_->previousEventNode.readyRoles));
    if (pending && core_->initialized && !core_->initializing &&
        !core_->administratorBlocked && !core_->sourceResultReady)
      botEarlier(wait, now, core_->eventRateSet ? core_->eventAt + 1000 : now);
  }
  return wait;
}
#endif
void CommandBot::loop() {
#if defined(MESHCORE_HOST_BOT_SOURCE) && MESHCORE_HOST_BOT_SOURCE
  if (core_) core_->ownerTick();
#endif
  const uint32_t now = millis();
  node_.uptimeMs += uint32_t(now - previousMillis_);
  previousMillis_ = now;
  node_.available = true;
#ifdef ARDUINO_ARCH_ESP32
  node_.wifiKnown = true;
  node_.wifiConnected = WiFi.status() == WL_CONNECTED;
#endif
  if (!core_) return;
  ClockSnapshot clocks;
  if (clockSnapshot(clocks) && clocks.network_epoch) {
    const uint64_t epoch = uint64_t(clocks.network_epoch) + clocks.network_age_seconds;
    if (epoch <= 4102444800u) rtc_.synchronize(static_cast<uint32_t>(epoch));
  }
  rtc_.tick();
  if (core_->initializing && core_->initializationRetryAt &&
      int32_t(millis() - core_->initializationRetryAt) >= 0 && !worker_.busy()) {
    core_->initializationRetryAt = 0;
    if (!worker_.stage(BotDefaultSource, strlen(BotDefaultSource))) {
      core_->initializing = false;
      fault("Command VM initialization retry unavailable");
    }
  }
  worker_.setReminderReady(core_->initialized && !core_->initializing &&
                           !core_->administratorBlocked && !core_->sourceResultReady);
  core_->loop();
  core_->sampleAdmission();
  core_->radioIo();
  core_->reminderIo();
  core_->statusEvents();
  const uint32_t dropped = radio_.getPacketsRecvErrors();
  for (auto &invocation : core_->invocations)
    if (invocation.collecting && dropped != core_->lastRxDropped) {
      invocation.event.truncated = true;
      core_->stats.observationsDropped += dropped - core_->lastRxDropped;
    }
  core_->lastRxDropped = dropped;
  for (auto &invocation : core_->invocations)
    if ((invocation.collecting && uint32_t(millis() - invocation.collectionStart) >= invocation.event.windowMs) ||
        (invocation.deferred && !invocation.collecting && int32_t(millis() - invocation.notBefore) >= 0)) {
      invocation.collecting = false;
      if (invocation.deferred && int32_t(millis() - invocation.notBefore) < 0) continue;
      invocation.deferred = false;
      if (!worker_.invoke(invocation.event, invocation.job)) {
        core_->reject("Bot collection worker admission failed");
        BotAction action;
        action.kind = BotAction::Reply;
        snprintf(action.text, sizeof(action.text), "%.*s", int(invocation.event.replyLimit),
                 "Error: Bot busy; collected command not executed");
        core_->act(action, invocation);
        wipe(&invocation, sizeof(invocation));
      }
    }
  BotWorker::Result result;
  if (worker_.poll(result)) {
    core_->stats.lastVm = result.stats;
    if (result.operation == BotWorker::Operation::Event) {
      if (result.ok && worker_.resultCurrent(result)) ++core_->stats.eventsCompleted;
      else { ++core_->stats.eventsFailed; fault(result.error[0] ? result.error : "Event source replaced"); }
      for (auto &job : core_->invocations) if (job.used && job.job == result.job) {
        if (job.ioPending) core_->finishRadio(job, "Scheduled event ended; admitted request outcome may be unknown");
        wipe(&job, sizeof(job));
        break;
      }
    } else if (result.operation == BotWorker::Operation::Invoke) {
      for (auto &invocation : core_->invocations) if (invocation.used && invocation.job == result.job) {
        if (invocation.ioPending)
          core_->finishRadio(invocation, "Invocation ended; pending radio outcome may be unknown");
        if (!worker_.resultCurrent(result)) {
          result.ok = false;
          strcpy(result.error, "Runtime code changed; pending operation outcome may be unknown");
        }
        bool submitted = false;
        if (result.ok && result.action.kind != BotAction::None) submitted = core_->act(result.action, invocation);
        else if (!result.ok) {
          core_->adminAcknowledged(invocation.adminTicket, false); invocation.adminTicket = 0;
          ++core_->stats.vmFailures;
          fault(result.error);
          BotAction action;
          action.kind = BotAction::Reply;
          const char *requirement = nullptr;
          const char *name = invocation.event.name;
          if (strstr(result.error, "permission not granted:")) {
            if (!strcmp(name, "board"))
              requirement = "use the bot's channel; ask the owner to enable the shared board";
            else if (!strcmp(name, "weather") || !strcmp(name, "service"))
              requirement = "send a DM; if unavailable, ask the owner to enable home services";
            else if (!strcmp(name, "remind"))
              requirement = "send a DM; if unavailable, ask the owner to enable reminders";
            else if (!strcmp(name, "admin"))
              requirement = "only the node owner's DM can use administration";
            else for (const char *privateName : {"reminders", "cancel", "remember", "recall", "forget", "notes", "list-memories"})
              if (!strcmp(name, privateName)) requirement = "send this command by DM";
          }
          if (requirement)
            snprintf(action.text, sizeof(action.text), "Error: !%s permission not granted: %s", name, requirement);
          else if (strstr(result.error, "unknown command !"))
            snprintf(action.text, sizeof(action.text), "Error: unknown command !%s; use !help", name);
          else
            snprintf(action.text, std::min(sizeof(action.text), size_t(invocation.event.replyLimit) + 1),
                     "Error: !%s: %s", name, commandErrorMessage(result.error));
          core_->act(action, invocation);
        }
        if (!submitted) core_->adminAcknowledged(invocation.adminTicket, false);
        wipe(&invocation, sizeof(invocation));
        break;
      }
    } else if (core_->initializing) {
      if (result.ok && result.operation != BotWorker::Operation::Activate) {
        if (worker_.activate()) return;
        result.ok = false; strcpy(result.error, "Command VM initial activation unavailable");
      }
      core_->initialized = result.ok;
      if (!result.ok) {
        fault(result.error);
        core_->initializing = ++core_->initializationRetries <= 3;
        if (core_->initializing) core_->initializationRetryAt = millis() + 1000 * core_->initializationRetries;
      } else {
        core_->initializing = false;
        status_.fault[0] = 0;
        if (!core_->administratorBlocked && !sourceStartupBlocked_
#if !defined(NRF52_PLATFORM) && !(defined(MESHCORE_HOST_BOT_SOURCE) && MESHCORE_HOST_BOT_SOURCE)
            && automaticAdvertsEnabled()
#endif
            ) advertise();
      }
    } else {
      core_->sourceResult = result;
      core_->sourceResultReady = true;
      if (!result.ok) {
        fault(result.error);
        sourceLifecycleFault_ = true;
      } else if ((result.operation == BotWorker::Operation::Activate ||
                  result.operation == BotWorker::Operation::RemoveWasm) && sourceLifecycleFault_) {
        status_.fault[0] = 0;
        sourceLifecycleFault_ = false;
      }
    }
    diagnostic("Command VM: %llu us, %u peak allocator bytes, %u instructions, %u parser steps\n",
                  static_cast<unsigned long long>(result.stats.elapsedUs),
                  unsigned(result.stats.peakBytes), result.stats.instructions,
                  result.stats.parserSteps);
    if (result.stats.wasmPoolBytes)
      diagnostic("Wasm pool=%u peak=%u session=%u linear=%u stack=8192 bytes\n",
                 result.stats.wasmPoolBytes, result.stats.wasmPoolHighWaterBytes,
                 result.stats.wasmSessionBytes, result.stats.wasmLinearBytes);
    diagnostic("Command phases: load=%llu init=%llu invoke=%llu cleanup=%llu us\n",
                  static_cast<unsigned long long>(result.stats.loadUs),
                  static_cast<unsigned long long>(result.stats.initUs),
                  static_cast<unsigned long long>(result.stats.invokeUs),
                  static_cast<unsigned long long>(result.stats.cleanupUs));
    if (result.operation == BotWorker::Operation::StageFile)
#ifdef NRF52_PLATFORM
      diagnostic("Command LittleFS source read: %u ms\n", result.sourceReadMs);
#else
      diagnostic("Command SPIFFS source read: %u ms\n", result.sourceReadMs);
#endif
#ifdef ARDUINO_ARCH_ESP32
    diagnostic("Command task: free internal %u, PSRAM %u, minimum free stack %u bytes\n",
                  result.stats.freeInternalBytes, result.stats.freePsramBytes,
                  result.stats.stackHighWaterBytes);
#endif
  }
}
void CommandBot::nodeSnapshot(BotNodeSnapshot &snapshot) const {
  snapshot = node_;
  snapshot.enabled = enabled_;
  snapshot.hasIdentity = core_ != nullptr;
  if (core_) memcpy(snapshot.publicKey, core_->self_id.pub_key, 32);
  snapshot.ready = sourceReady() && !core_->administratorBlocked && !sourceStartupBlocked_;
  snapshot.fault = status_.fault[0] != 0 || adaptivePolicyFault_ || sourceStartupBlocked_;
  strcpy(snapshot.nativeRevision, ONCHIP_NATIVE_REVISION);
  strcpy(snapshot.build, __DATE__ " " __TIME__);
  snprintf(snapshot.name, sizeof(snapshot.name), "%s", core_ ? core_->meshPolicy.name : status_.name);
  snapshot.sourceGeneration = worker_.sourceGeneration();
}
void CommandBot::setNodeRoles(uint8_t selected, uint8_t ready) {
  node_.rolesKnown = true;
  node_.selectedRoles = selected;
  node_.readyRoles = ready;
}
void CommandBot::dashboardStatus(RadioDashboard::RoleStatus &status) const {
  status = status_;
  if (adaptivePolicyFault_)
    snprintf(status.fault, sizeof(status.fault), "%s", AdaptivePolicyFault);
  if (sourceStartupBlocked_ && (!status_.fault[0] || (core_ && core_->initialized)))
    snprintf(status.fault, sizeof(status.fault), "%s", sourceStartupFault_);
  status.ready = sourceReady() && !core_->administratorBlocked && !sourceStartupBlocked_;
  status.source_slot = radio_.sourceSlot();
  mesh::QueuedRadioStats stats{};
  if (core_ && radio_.getQueuedRadioStats(stats)) status.source_generation = stats.generation;
  if (core_ && core_->initializing) strcpy(status.state, "starting");
  else if (core_ && (core_->administratorBlocked || sourceStartupBlocked_)) strcpy(status.state, "source-recovery");
  else if (enabled_ && !status.ready) strcpy(status.state, "fault");
}
bool CommandBot::sourceReady() const {
  return core_ && core_->initialized && radio_.queuedReady();
}
bool CommandBot::retrySourceInitialization() {
  if (!core_ || core_->initialized || core_->initializing || worker_.busy()) return false;
  if (!worker_.stage(BotDefaultSource, strlen(BotDefaultSource))) return false;
  core_->initializing = true;
  core_->initializationRetries = 0;
  core_->initializationRetryAt = 0;
  return true;
}
void CommandBot::setCommandAdmission(bool enabled) {
  if (core_) core_->administratorBlocked = !enabled;
}
void CommandBot::setSourceDeploymentState(bool luaReady, bool luaBlocked, bool wasmReady,
                                          bool wasmBlocked, const char *fault) {
  selectedSourcesReady_ = luaReady && wasmReady;
  sourceStartupBlocked_ = luaBlocked || wasmBlocked;
  worker_.setRuntimeDeploymentBlocked(false, luaBlocked);
  worker_.setRuntimeDeploymentBlocked(true, wasmBlocked);
  snprintf(sourceStartupFault_, sizeof(sourceStartupFault_), "%s", fault ? fault : "");
  if (selectedSourcesReady_ && !sourceStartupBlocked_ && sourceLifecycleFault_) {
    status_.fault[0] = 0;
    sourceLifecycleFault_ = false;
  }
}
bool CommandBot::setSharedState(bool enabled) {
  if (!enabled) {
    if (core_) core_->sharedState = false;
    worker_.setSharedState(false);
  }
  if (!saveBotSharedState(enabled)) { fault("Shared KV policy commit failed"); return false; }
  if (core_) core_->sharedState = enabled;
  if (enabled && core_ && !worker_.setSharedState(true)) {
    core_->sharedState = false;
    fault("Shared storage saved but runtime grant unavailable");
    return false;
  }
  return true;
}
bool CommandBot::sharedState() const { return core_ && core_->sharedState; }
bool CommandBot::setHomeAccess(bool enabled) {
  if (!enabled) {
    if (core_) core_->homeAccess = false;
    worker_.setHomeAccess(false);
  }
  if (enabled && !botHttpsConfigured()) { fault("Verified home HTTPS is not configured"); return false; }
  if (enabled && !botHttpsClockTrusted()) { fault("Home HTTPS requires trusted synchronized time"); return false; }
  if (enabled && botHttpsConfig().valid() && !botHttpsProbeReady()) {
    fault("Native HTTPS probe failed or pending"); return false;
  }
  if (!saveBotHomeAccess(enabled)) { fault("Home RPC policy commit failed"); return false; }
  if (core_) core_->homeAccess = enabled;
  if (enabled) worker_.setHomeAccess(true);
  return true;
}
bool CommandBot::homeAccess() const {
  return core_ && core_->homeAccess && botHttpsConfigured() && botHttpsClockTrusted() &&
         (!botHttpsConfig().valid() || botHttpsProbeReady());
}
bool CommandBot::setDiscovery(bool enabled) {
  if (core_) core_->discovery = false;
  if (!saveBotDiscovery(enabled)) {
    fault("Bot discovery commit/readback failed; live access disabled"); return false;
  }
  if (core_) core_->discovery = enabled;
  return true;
}
void CommandBot::discoveryCommand(const char *command, char *reply, size_t capacity) {
  if (!*command || !strcmp(command, "status")) {
    bool saved = false;
    if (!loadBotDiscovery(saved)) { snprintf(reply, capacity, "Error: saved bot discovery policy unavailable"); return; }
    snprintf(reply, capacity, "Bot discovery saved=%u live=%u; signed contacts, base only; no location/environment; 1 response/s and bot airtime budget",
             saved, core_ && core_->discovery);
  } else if (!strcmp(command, "on") || !strcmp(command, "off")) {
    snprintf(reply, capacity, "%s", setDiscovery(!strcmp(command, "on")) ?
        (core_ ? "Saved and applied bot discovery; signed contacts only, base telemetry; management grants unchanged" :
                 "Saved bot discovery; bot disabled; applies at startup; management grants unchanged") :
        "Error: discovery commit/readback unknown; live access disabled; inspect bot discovery");
  } else snprintf(reply, capacity, "Error: discovery [status|on|off]; on permits base telemetry to all signed contacts");
}
bool CommandBot::setForwardPolicy(const BotForwardPolicy &policy) {
  if (!policy.valid() || (policy.enabled() && core_ &&
      (core_->self_id.matches(policy.from) || core_->self_id.matches(policy.to)))) {
    fault("Forward policy requires distinct non-bot full keys"); return false;
  }
  if (core_) {
    core_->forwarding = {};
    if (core_->forwardGrant == UINT32_MAX) {
      fault("Forward grant epoch exhausted; disabled until reboot"); return false;
    }
    ++core_->forwardGrant;
  }
  if (!saveBotForwardPolicy(policy)) { fault("Forward policy commit/readback failed; live access disabled"); return false; }
  if (core_) core_->forwarding = policy;
  return true;
}
bool CommandBot::forwardAccess() const { return core_ && core_->forwarding.enabled(); }
bool CommandBot::setMeshPolicy(const BotMeshPolicy &policy) {
  if (!policy.valid() || (core_ && policy.allows(core_->self_id.pub_key))) {
    fault("Mesh policy requires valid name and distinct non-bot destinations"); return false;
  }
  if (core_) {
    core_->meshPolicy.channelWait = false;
    memset(core_->meshPolicy.destinations, 0, sizeof(core_->meshPolicy.destinations));
    if (core_->meshGrant == UINT32_MAX) { fault("Mesh grant epoch exhausted; disabled until reboot"); return false; }
    ++core_->meshGrant;
  }
  if (!saveBotMeshPolicy(policy)) { fault("Mesh policy commit/readback unknown; live grants disabled"); return false; }
  if (core_) core_->meshPolicy = policy;
  snprintf(status_.name, sizeof(status_.name), "%.31s", policy.name);
  return true;
}
void CommandBot::meshPolicyStatus(char *text, size_t capacity) const {
  if (!core_) { snprintf(text, capacity, "Bot inactive; saved policy: bot name, bot destination SLOT, bot channel-wait"); return; }
  unsigned destinations = 0;
  for (const auto &key : core_->meshPolicy.destinations) if (core_->meshPolicy.allows(key)) ++destinations;
  snprintf(text, capacity, "Applied name=%s destinations=%u/4 channel-wait=%u epoch=%u; direct DM routes only; limits: bot limits",
           core_->meshPolicy.name, destinations, core_->meshPolicy.channelWait, core_->meshGrant);
}
void CommandBot::cancelJobs(uint32_t except) {
  if (!core_) return;
  for (auto &job : core_->invocations) if (job.used && job.job != except) {
    if (job.collecting || job.deferred) { wipe(&job, sizeof(job)); continue; }
    job.cancelled = true;
    if (job.ioPending) core_->finishRadio(job, "Owner cancelled; admitted radio outcome may be unknown");
  }
  worker_.cancelJobs(except);
}
bool CommandBot::setReminderAccess(bool enabled) {
  if (core_ && !worker_.setReminderAccess(false)) {
    fault("Reminder grant epoch exhausted; disabled until reboot"); return false;
  }
  if (!saveBotReminderAccess(enabled)) {
    fault("Reminder grant commit/readback failed; live access disabled"); return false;
  }
  return !core_ || worker_.setReminderAccess(enabled);
}
bool CommandBot::reminderAccess() const { return core_ && worker_.reminderAccess(); }
bool CommandBot::setEventAccess(uint8_t mask) {
  if (mask > 31 || (core_ && !worker_.setEventAccess(0))) return false;
  if (!saveBotEventAccess(mask)) { fault("Event grant commit/readback failed; live events disabled"); return false; }
  if (core_) {
    core_->connectivityKnown = core_->nodeKnown = false;
    core_->scheduledAt = millis();
    return worker_.setEventAccess(mask);
  }
  return true;
}
unsigned CommandBot::repeaterSnapshots(BotRepeaterSnapshot *snapshots, unsigned capacity) const {
  if (!core_ || !snapshots) return 0;
  unsigned count = 0;
  for (unsigned i = 0; i < BotRepeaterLimit && count < capacity; ++i)
    if (core_->repeaterPolicy.targets[i].used) snapshots[count++] = core_->repeaterSnapshot(i);
  return count;
}
void CommandBot::repeaterCommand(const char *command, char *reply, size_t capacity) {
  const auto error = [&](const char *text) { snprintf(reply, capacity, "Error: %s", text); };
  if (!core_) { error("command bot is not running; repeater monitor is unavailable"); return; }
  if (!command || strlen(command) >= 320) { error("repeater monitor command exceeds its bound"); return; }
  char input[320]{};
  strcpy(input, command);
  char *cursor = nullptr;
  const char *action = strtok_r(input, " ", &cursor);
  if (!action || !strcmp(action, "status")) {
    const char *alias = action ? strtok_r(nullptr, " ", &cursor) : nullptr;
    if (strtok_r(nullptr, " ", &cursor)) { error("bot repeaters status [ALIAS]"); return; }
    if (!alias) {
      unsigned count = 0;
      for (const auto &target : core_->repeaterPolicy.targets) count += target.used;
      snprintf(reply, capacity, "Repeaters on=%u peers=%u/%u interval=%us grant=%u; bot events 16 enables recurring Lua",
               core_->repeaterPolicy.enabled, count, BotRepeaterLimit,
               core_->repeaterPolicy.intervalSeconds, core_->repeaterGrant);
    } else {
      for (unsigned i = 0; i < BotRepeaterLimit; ++i)
        if (core_->repeaterPolicy.targets[i].used && !strcmp(alias, core_->repeaterPolicy.targets[i].alias)) {
          const auto s = core_->repeaterSnapshot(i);
          char battery[24] = "unavailable";
          if (s.available && s.stats.batteryMv)
            snprintf(battery, sizeof(battery), "%.3fV", double(s.stats.batteryMv) / 1000);
          snprintf(reply, capacity, "%s available=%u fresh=%u age=%us error=%s battery=%s uptime=%us attempts=%u failed=%u",
                   s.alias, s.available, s.fresh, s.ageSeconds, botRepeaterErrorName(s.error),
                   battery, s.stats.uptimeSeconds, s.attempts, s.failures);
          return;
        }
      error("repeater alias is not configured");
    }
    return;
  }
  if (!strcmp(action, "route")) {
    const char *alias = strtok_r(nullptr, " ", &cursor);
    if (!alias || strtok_r(nullptr, " ", &cursor)) { error("bot repeaters route ALIAS"); return; }
    for (unsigned i = 0; i < BotRepeaterLimit; ++i) {
      const auto &target = core_->repeaterPolicy.targets[i];
      if (!target.used || strcmp(alias, target.alias)) continue;
      PeerRoute route;
      if (target.path.known)
        route.learn(target.path.bytes, uint8_t((target.path.width - 1) << 6 | target.path.count));
      for (const auto &contact : core_->contacts)
        if (contact.used && contact.id.matches(target.key)) { route = contact.route; break; }
      char path[2 * MAX_PATH_SIZE + 3] = "unknown";
      if (route.length != 0xff) {
        const unsigned width = (route.length >> 6) + 1;
        snprintf(path, sizeof(path), "%u:", width);
        const unsigned size = (route.length & 63) * width;
        for (unsigned j = 0; j < size; ++j)
          snprintf(path + 2 + 2 * j, sizeof(path) - 2 - 2 * j, "%02x", route.bytes[j]);
      }
      snprintf(reply, capacity, "%s route=%s repair=%u", alias, path, core_->repeaters[i].failures >= 3);
      return;
    }
    error("repeater alias is not configured"); return;
  }
  if (!strcmp(action, "help")) {
    snprintf(reply, capacity, "bot repeaters on|off|interval SECONDS|status [ALIAS]|route ALIAS|remove ALIAS|add ALIAS KEY64 FREQ_HZ [WIDTH:HEX]; read ACL required");
    return;
  }
  auto candidate = core_->repeaterPolicy;
  if (!strcmp(action, "on") || !strcmp(action, "off")) candidate.enabled = !strcmp(action, "on");
  else if (!strcmp(action, "interval")) {
    const char *text = strtok_r(nullptr, " ", &cursor);
    uint32_t seconds = 0;
    if (!text || strlen(text) > 5) { error("repeater interval requires 60..86400 seconds"); return; }
    for (const char *p = text; *p; ++p) {
      if (*p < '0' || *p > '9') { error("repeater interval must be numeric"); return; }
      seconds = seconds * 10 + unsigned(*p - '0');
    }
    candidate.intervalSeconds = seconds;
  } else if (!strcmp(action, "add") || !strcmp(action, "remove")) {
    const char *alias = strtok_r(nullptr, " ", &cursor);
    if (!alias || strlen(alias) > 16) { error("repeater alias requires 1..16 lowercase identifier bytes"); return; }
    auto *target = static_cast<BotRepeaterPolicy::Target *>(nullptr);
    for (auto &entry : candidate.targets)
      if (entry.used && !strcmp(entry.alias, alias)) { target = &entry; break; }
    if (!strcmp(action, "remove")) {
      if (!target) { error("repeater alias is not configured"); return; }
      *target = {};
    } else {
      const char *key = strtok_r(nullptr, " ", &cursor);
      const char *frequency = strtok_r(nullptr, " ", &cursor);
      const char *path = strtok_r(nullptr, " ", &cursor);
      if (!target) for (auto &entry : candidate.targets) if (!entry.used) { target = &entry; break; }
      if (!target) { error("repeater target table is full"); return; }
      BotRepeaterPolicy::Target value{};
      if (!key || strlen(key) != 64 || !mesh::Utils::fromHex(value.key, 32, key) ||
          core_->self_id.matches(value.key)) {
        error("repeater target requires a complete key different from this bot"); return;
      }
      if (!frequency || !*frequency || strlen(frequency) > 10) {
        error("repeater target requires its frequency in Hz (0 permits the current modem frequency)"); return;
      }
      uint64_t hz = 0;
      for (const char *p = frequency; *p; ++p) {
        if (*p < '0' || *p > '9') { error("repeater frequency must be numeric Hz"); return; }
        hz = hz * 10 + unsigned(*p - '0');
      }
      if (hz > UINT32_MAX) { error("repeater frequency exceeds uint32 Hz"); return; }
      value.frequencyHz = uint32_t(hz);
      if (path) {
        const size_t bytes = strlen(path);
        if (bytes < 2 || path[0] < '1' || path[0] > '3' || path[1] != ':' ||
            (bytes - 2) % (2u * unsigned(path[0] - '0')) || (bytes - 2) / 2 > BotPathLimit ||
            (bytes > 2 && !mesh::Utils::fromHex(value.path.bytes, (bytes - 2) / 2, path + 2))) {
          error("repeater path requires WIDTH:HEX, width 1..3 and complete bounded hops"); return;
        }
        value.path.known = true; value.path.width = uint8_t(path[0] - '0');
        value.path.count = uint8_t((bytes - 2) / (2 * value.path.width));
      }
      strcpy(value.alias, alias); value.used = true;
      if (target->used && !memcmp(target->key, value.key, 32)) value.lastFloodUtc = target->lastFloodUtc;
      *target = value;
    }
  } else { error("unknown repeater monitor command; use bot repeaters help"); return; }
  if (strtok_r(nullptr, " ", &cursor) || !candidate.valid()) {
    error("invalid repeater settings, duplicate alias/key or extra arguments; policy unchanged"); return;
  }
  if (core_->repeaterGrant == UINT32_MAX) {
    core_->repeaterPolicy.enabled = false;
    error("repeater grant epoch exhausted; reboot required"); return;
  }
  core_->repeaterPolicy.enabled = false;
  ++core_->repeaterGrant;
  for (auto &job : core_->invocations) if (job.used && job.ioPending && core_->repeaterIo(job))
    core_->failRepeater(job, BotRepeaterError::Cancelled, "Repeater policy changed; admitted request outcome may be unknown");
  if (!saveBotRepeaterPolicy(candidate)) {
    fault("Repeater policy commit/readback failed; live monitor disabled");
    error("repeater policy commit/readback failed; live monitor disabled"); return;
  }
  for (unsigned i = 0; i < BotRepeaterLimit; ++i) {
    const auto &old = core_->repeaterPolicy.targets[i], &next = candidate.targets[i];
    if (old.used != next.used || strcmp(old.alias, next.alias) || memcmp(old.key, next.key, 32) ||
        old.frequencyHz != next.frequencyHz) core_->repeaters[i] = {};
    core_->repeaters[i].due = millis() + i * 30000;
  }
  core_->repeaterPolicy = candidate;
  snprintf(reply, capacity, "Saved/applied repeater policy; interval=%us; modem frequency unchanged",
           candidate.intervalSeconds);
}
const uint8_t *CommandBot::publicKey() const { return core_ ? core_->self_id.pub_key : nullptr; }
const CommandBot::Counters &CommandBot::counters() const {
  static const Counters empty;
  return core_ ? core_->stats : empty;
}
bool CommandBot::stageSource(const char *source, size_t size) {
  if (!core_ || core_->sourceResultReady ||
      !worker_.stage(source, size)) {
    fault("Command staging busy/invalid source size"); return false;
  }
  return true;
}
bool CommandBot::stageSourceFile(size_t expectedSize, const uint8_t expectedSha256[32], uint32_t publication) {
  if (!core_ || core_->sourceResultReady ||
      !worker_.stageFile(expectedSize, expectedSha256, publication)) {
    fault("Command file staging busy/invalid manifest"); return false;
  }
  return true;
}
bool CommandBot::activateStaged(uint32_t publication) {
  if (!core_ || core_->collecting() || core_->sourceResultReady ||
      !worker_.activate(publication)) {
    fault("Command activation busy"); return false;
  }
  return true;
}
bool CommandBot::removeWasm(uint32_t publication) {
  return core_ && !core_->collecting() && !core_->sourceResultReady && worker_.removeWasm(publication);
}
bool CommandBot::pollSourceResult(BotWorker::Result &result) {
  if (!core_ || !core_->sourceResultReady) return false;
  result = core_->sourceResult;
  core_->sourceResultReady = false;
  return true;
}
bool CommandBot::advertise(bool zeroHop) {
  if (!core_
#if defined(MESHCORE_HOST_BOT_SOURCE) && MESHCORE_HOST_BOT_SOURCE
      || core_->administratorBlocked
#endif
      || !radio_.queuedReady() || radio_.hasPendingWork() ||
      (core_->lastAdvert && uint32_t(millis() - core_->lastAdvert) < 900000)) {
    fault("Bot advert unavailable or rate limited"); return false;
  }
  auto *packet = core_->makeAdvert();
  if (!packet) return false;
  if (zeroHop) core_->sendZeroHop(packet);
  else {
#if defined(MESHCORE_HOST_BOT_SOURCE) && MESHCORE_HOST_BOT_SOURCE
    core_->routing.flood(*core_, packet, core_->routing.defaultScope(), core_->policy.pathWidth);
#else
    core_->sendFlood(packet, 0, core_->policy.pathWidth);
#endif
  }
  return true;
}
bool CommandBot::advertiseOwnerZeroHop() {
  if (!core_
#if defined(MESHCORE_HOST_BOT_SOURCE) && MESHCORE_HOST_BOT_SOURCE
      || core_->administratorBlocked || !sourceDeploymentReady()
#endif
      || !radio_.queuedReady() || radio_.hasPendingWork() ||
      core_->packets.getOutboundTotal()) {
    fault("Owner bot advert unavailable: radio or queue busy"); return false;
  }
  auto *packet = core_->makeAdvert(true);
  if (!packet) return false;
  core_->sendZeroHop(packet);
  return true;
}
} // namespace onchip
#endif
