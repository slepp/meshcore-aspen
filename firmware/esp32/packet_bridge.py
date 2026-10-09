# SPDX-License-Identifier: Apache-2.0
"""Install checked packet-engine bridges into the pinned disposable Aspen core."""
from pathlib import Path
import hashlib
import shutil


def replace(text, old, new):
    if text.count(old) != 1:
        raise ValueError(f"Packet engine source anchor changed: {old[:100]!r}")
    return text.replace(old, new, 1)


def revision():
    source = Path(__file__).resolve().parent
    return hashlib.sha256((source / "packet_bridge.py").read_bytes() +
                          (source / "PacketEngineBridge.h").read_bytes()).hexdigest()


def check(target):
    marker = target / "src/onchip_packet_bridge.sha256"
    if not marker.is_file() or marker.read_text().strip() != revision():
        raise ValueError("Prepared packet-engine core is missing or older; run the full Aspen prepare target before field-rebuild")


def stage(target):
    source = Path(__file__).resolve().parent
    if "#define MESH_PACKET_ENGINE_API 1" in (target / "src/Mesh.h").read_text():
        check(target)
        return
    shutil.copy2(source / "PacketEngineBridge.h", target / "src/PacketEngineBridge.h")
    for name, transform in (
        ("src/Dispatcher.h", dispatcher_header),
        ("src/Dispatcher.cpp", dispatcher_source),
        ("src/Packet.h", packet_header),
        ("src/Mesh.h", mesh_header),
        ("src/Mesh.cpp", mesh_source),
        ("src/helpers/BaseChatMesh.cpp", chat_source),
        ("examples/simple_repeater/MyMesh.cpp", repeater_source),
        ("examples/simple_room_server/MyMesh.h", room_header),
        ("examples/simple_room_server/MyMesh.cpp", room_source),
    ):
        path = target / name
        path.write_text(transform(path.read_text()))
    (target / "src/onchip_packet_bridge.sha256").write_text(revision() + "\n")


def dispatcher_header(text):
    text = replace(text, "#include <Packet.h>",
                   "#include <Packet.h>\n#include <PacketEngineBridge.h>")
    text = replace(text, "  uint16_t _err_flags;",
                   "  uint16_t _err_flags;\n  PacketOrigin packet_origin;")
    text = replace(text, "  Packet* obtainNewPacket();",
                   "  PacketOrigin& packetOrigin() { return packet_origin; }\n  Packet* obtainNewPacket();")
    text = replace(text, "  virtual bool pollQueuedResult(QueuedTransmitResult&) { return false; }", """  virtual bool queueTransmitWithOrigin(const uint8_t* data, int size, uint8_t priority,
                                       uint32_t delay, uint32_t expiry, uint32_t& job,
                                       PacketOrigin) {
    return queueTransmit(data, size, priority, delay, expiry, job);
  }
  virtual PacketOrigin lastReceiveOrigin() const { return {}; }
  virtual bool pollQueuedResult(QueuedTransmitResult&) { return false; }""")
    text = replace(text, "  virtual void logQueuedTxResult(Packet*, const QueuedTransmitResult&) { }", """  virtual void logQueuedTxResult(Packet*, const QueuedTransmitResult&) { }
  virtual const uint8_t* packetEngineIdentity() const { return nullptr; }
  void sendRelayPacket(Packet* packet, uint8_t priority, uint32_t delay_millis);""")
    return replace(text, "class Radio {\npublic:", """class Radio {
public:
  virtual bool processPacketEngine(const PacketEngineInfo&, uint8_t*, uint16_t&,
                                   uint16_t) { return true; }""")


def packet_header(text):
    return replace(text, "  bool _localReflection = false;",
                   "  bool _localReflection = false;\n"
                   "  bool _engineOrigin = false, _reflectionOrigin = false;\n"
                   "  float _rssi = 0;\n  uint32_t _expectedAck = 0;")


def dispatcher_source(text):
    text = replace(text, '#include "Dispatcher.h"',
                   '#include "Dispatcher.h"\n#include "PacketEngineBridge.h"')
    text = replace(text, "          pkt->_localReflection = _radio->lastReceiveWasLocal();", """          pkt->_localReflection = _radio->lastReceiveWasLocal();
          const auto origin = _radio->lastReceiveOrigin();
          pkt->_engineOrigin = origin.engine;
          pkt->_reflectionOrigin = origin.reflection || pkt->_localReflection;
          pkt->_rssi = pkt->_localReflection ? 0 : _radio->getLastRSSI();""")
    text = replace(text, "void Dispatcher::processRecvPacket(Packet* pkt) {", """void Dispatcher::processRecvPacket(Packet* pkt) {
  PacketOriginScope origin(packet_origin, {pkt->_engineOrigin, pkt->_reflectionOrigin});""")
    text = replace(text, """    if (!_radio->queueTransmit(raw, length, priority, delay, 0,
                               entry.job)) return false;""", """    if (!_radio->queueTransmitWithOrigin(raw, length, priority, delay, 0,
                                         entry.job, {packet->_engineOrigin,
                                                     packet->_reflectionOrigin})) return false;""")
    text = replace(text, """    if (_radio->supportsQueuedTransmit()) sendPacket(pkt, priority, _delay);
    else _mgr->queueOutbound(pkt, priority, futureMillis(_delay));""",
                   "    sendRelayPacket(pkt, priority, _delay);")
    text = replace(text, "void Dispatcher::checkSend() {", """void Dispatcher::sendRelayPacket(Packet* packet, uint8_t priority, uint32_t delay_millis) {
    if (packet->_localReflection) {
      _mgr->free(packet);
      return;
    }
    uint8_t raw[MAX_TRANS_UNIT];
    uint16_t length = packet->writeTo(raw);
    if (!_radio->processPacketEngine(packetEngineInfo(PacketEngineStage::Relay,
                                                     *packet, packetEngineIdentity()),
                                    raw, length, sizeof(raw))) {
      _mgr->free(packet);
      return;
    }
    if (!packet->readFrom(raw, length)) {
      MESH_DEBUG_PRINTLN("Dispatcher relay packet encoding invalid");
      _mgr->free(packet);
      return;
    }
    sendPacket(packet, priority, delay_millis);
}

void Dispatcher::checkSend() {""")
    return replace(text, "    pkt->_localReflection = false;",
                   "    pkt->_localReflection = false;\n"
                   "    pkt->_engineOrigin = packet_origin.engine;\n"
                   "    pkt->_reflectionOrigin = packet_origin.reflection;\n"
                   "    pkt->_rssi = 0;\n    pkt->_expectedAck = 0;")


def mesh_header(text):
    text = replace(text, "#include <Dispatcher.h>",
                   "#include <Dispatcher.h>\n#include <PacketEngineBridge.h>\n#define MESH_PACKET_ENGINE_API 1")
    text = replace(text, "  MeshTables* _tables;", """  MeshTables* _tables;
  OriginalPlaintext received_plaintext;
  bool filterPlaintext(uint8_t type, bool composing, const Packet* packet,
                       const uint8_t* identity, uint8_t* data,
                       uint16_t& length, uint16_t capacity) {
    const auto stage = composing ? PacketEngineStage::PlainCompose : PacketEngineStage::PlainReceive;
    PacketEngineInfo info;
    if (packet) info = packetEngineInfo(stage, *packet, identity);
    else {
      info.stage = stage;
      info.identity = identity;
      info.origin = packet_origin;
    }
    info.type = type;
    return _radio->processPacketEngine(info, data, length, capacity);
  }""")
    text = replace(text, "  DispatcherAction onRecvPacket(Packet* pkt) override;",
                   "  DispatcherAction onRecvPacket(Packet* pkt) override;\n"
                   "  const uint8_t* packetEngineIdentity() const override { return self_id.pub_key; }")
    return replace(text, "  void begin();", """  const uint8_t* originalReceivedPlaintext(const uint8_t* fallback, size_t& length) const {
    if (!received_plaintext.bytes) return fallback;
    length = received_plaintext.length;
    return received_plaintext.bytes;
  }
  void receivedTextAck(uint8_t* ack, const uint8_t* data, size_t length,
                       const uint8_t* key, uint8_t* attempt = nullptr) const {
    data = originalReceivedPlaintext(data, length);
    plaintextTextAck(ack, data, length,
                     length >= 5 && (data[4] >> 2) == 2 ? self_id.pub_key : key, attempt);
  }
  void begin();""")


def mesh_source(text):
    text = replace(text, "        sendPacket(a1, 0, delay_millis);",
                   "        a1->_rssi = packet->_rssi;\n"
                   "        a1->_snr = packet->_snr;\n"
                   "        sendRelayPacket(a1, 0, delay_millis);")
    text = replace(text, "      sendPacket(a2, 0, delay_millis);",
                   "      a2->_rssi = packet->_rssi;\n"
                   "      a2->_snr = packet->_snr;\n"
                   "      sendRelayPacket(a2, 0, delay_millis);")
    anchor = "tmp._snr = pkt->_snr;"
    if text.count(anchor) != 2:
        raise ValueError("Packet engine multipart metadata source anchors changed")
    text = text.replace(anchor, anchor + """
    tmp._rssi = pkt->_rssi;
    tmp._engineOrigin = pkt->_engineOrigin;
    tmp._reflectionOrigin = pkt->_reflectionOrigin;""")
    text = replace(text, "    memcpy(&data[data_len], path, path_hash_count*path_hash_size); data_len += path_hash_count*path_hash_size;",
                   "    if (path_hash_count) memcpy(&data[data_len], path, path_hash_count*path_hash_size);\n"
                   "    data_len += path_hash_count*path_hash_size;")
    anchor = "            if (len > 0) {  // success!\n              if (pkt->getPayloadType() == PAYLOAD_TYPE_PATH)"
    text = replace(text, anchor, """            if (len > 0) {  // success!
              OriginalPlaintextScope original(received_plaintext, data, len);
              uint16_t filtered = len;
              if (!filterPlaintext(pkt->getPayloadType(), false, pkt,
                                   self_id.pub_key, data, filtered, sizeof(data) - 1)) {
                found = true;
                break;
              }
              len = filtered;
              original.changed(data, len);
              if (pkt->getPayloadType() == PAYLOAD_TYPE_PATH)""")
    text = replace(text, "                if (onPeerPathRecv(pkt, j, secret, path, path_len, extra_type, extra, extra_len)) {",
                   "                OriginalPathExtraScope original_extra(received_plaintext);\n"
                   "                if (onPeerPathRecv(pkt, j, secret, path, path_len, extra_type, extra, extra_len)) {")
    text = replace(text, "          if (len > 0) {  // success!\n            onAnonDataRecv", """          if (len > 0) {  // success!
            uint16_t filtered = len;
            const bool deliver = filterPlaintext(pkt->getPayloadType(), false, pkt,
                                                  self_id.pub_key, data, filtered, sizeof(data) - 1);
            if (deliver) onAnonDataRecv""")
    text = replace(text, "onAnonDataRecv(pkt, secret, sender, data, len);",
                   "onAnonDataRecv(pkt, secret, sender, data, filtered);")
    text = replace(text, "          if (len > 0) {  // success!\n            onGroupDataRecv", """          if (len > 0) {  // success!
            uint16_t filtered = len;
            if (filterPlaintext(pkt->getPayloadType(), false, pkt,
                                self_id.pub_key, data, filtered, sizeof(data) - 1))
              onGroupDataRecv""")
    text = replace(text, "onGroupDataRecv(pkt, pkt->getPayloadType(), channels[j], data, len);",
                   "onGroupDataRecv(pkt, pkt->getPayloadType(), channels[j], data, filtered);")
    # Const inputs are copied: guest edits never write caller memory or secret arguments.
    for signature, overhead, identity in (
        ("Packet* Mesh::createDatagram(", 2, "self_id.pub_key"),
        ("Packet* Mesh::createAnonDatagram(", 1 + 32, "sender.pub_key"),
        ("Packet* Mesh::createGroupDatagram(", 1, "self_id.pub_key"),
    ):
        start = text.index(signature)
        anchor = "  Packet* packet = obtainNewPacket();"
        position = text.index(anchor, start)
        capacity = f"((MAX_PACKET_PAYLOAD - {overhead} - CIPHER_MAC_SIZE) / CIPHER_BLOCK_SIZE) * CIPHER_BLOCK_SIZE"
        text = text[:position] + f"""  uint8_t composed[MAX_PACKET_PAYLOAD]{{}};
  if ((!data && data_len) || data_len > sizeof(composed)) return NULL;
  if (data_len) memcpy(composed, data, data_len);
  uint16_t composed_len = data_len;
  if (!filterPlaintext(type, true, nullptr, {identity}, composed, composed_len,
                       {capacity})) return NULL;
  data = composed;
  data_len = composed_len;

""" + text[position:]
    anchor = "  packet->header = (type << PH_TYPE_SHIFT);  // ROUTE_TYPE_* set later"
    position = text.index(anchor, text.index("Packet* Mesh::createDatagram("))
    position += len(anchor)
    text = text[:position] + """
  if (type == PAYLOAD_TYPE_TXT_MSG && data_len >= 5) {
    plaintextTextAck(reinterpret_cast<uint8_t*>(&packet->_expectedAck), data, data_len,
                     (data[4] >> 2) == 2 ? dest.pub_key : self_id.pub_key);
  } else if (type == PAYLOAD_TYPE_REQ && data_len == 9) {
    Utils::sha256(reinterpret_cast<uint8_t*>(&packet->_expectedAck), 4,
                  data, data_len, self_id.pub_key, PUB_KEY_SIZE);
  }
""" + text[position:]
    text = replace(text, "    len += Utils::encryptThenMAC(secret, &packet->payload[len], data, data_len);", """    uint16_t composed_len = data_len;
    if (!filterPlaintext(PAYLOAD_TYPE_PATH, true, nullptr, self_id.pub_key,
                         data, composed_len, MAX_COMBINED_PATH)) {
      releasePacket(packet);
      return NULL;
    }
    len += Utils::encryptThenMAC(secret, &packet->payload[len], data, composed_len);""")
    # Advert data is edited before signing and only after verification on receive.
    text = replace(text, "  if (app_data_len > MAX_ADVERT_DATA_SIZE) return NULL;", """  if (app_data_len > MAX_ADVERT_DATA_SIZE) return NULL;
  uint8_t composed[MAX_ADVERT_DATA_SIZE]{};
  if (app_data_len) memcpy(composed, app_data, app_data_len);
  uint16_t composed_len = app_data_len;
  if (!filterPlaintext(PAYLOAD_TYPE_ADVERT, true, nullptr, id.pub_key,
                       composed, composed_len, sizeof(composed))) return NULL;
  app_data = composed;
  app_data_len = composed_len;""")
    return replace(text, "          onAdvertRecv(pkt, id, timestamp, app_data, app_data_len);", """          uint8_t verified[MAX_ADVERT_DATA_SIZE]{};
          memcpy(verified, app_data, app_data_len);
          uint16_t filtered = app_data_len;
          if (filterPlaintext(PAYLOAD_TYPE_ADVERT, false, pkt,
                              self_id.pub_key, verified, filtered, sizeof(verified)))
            onAdvertRecv(pkt, id, timestamp, verified, filtered);""")


def chat_source(text):
    text = replace(text, "      mesh::Utils::sha256(ack_hash, 4, data, 5 + text_len, from.id.pub_key, PUB_KEY_SIZE);",
                   "      receivedTextAck(ack_hash, data, len, from.id.pub_key, &ack_hash[4]);")
    text = replace(text, "      ack_hash[4] = data[5 + text_len + 1];", "")
    text = replace(text, "      int text_len = strlen((char *)&data[5]);", "")
    text = replace(text, "      mesh::Utils::sha256((uint8_t *) &ack_hash, 4, data, 9 + strlen((char *)&data[9]), self_id.pub_key, PUB_KEY_SIZE);",
                   "      receivedTextAck((uint8_t*)&ack_hash, data, len, from.id.pub_key);")
    text = replace(text, "  mesh::Utils::sha256((uint8_t *)&expected_ack, 4, temp, 5 + text_len, self_id.pub_key, PUB_KEY_SIZE);", "")
    text = replace(text, "  return createDatagram(PAYLOAD_TYPE_TXT_MSG, recipient.id, recipient.getSharedSecret(self_id), temp, len);", """  auto packet = createDatagram(PAYLOAD_TYPE_TXT_MSG, recipient.id, recipient.getSharedSecret(self_id), temp, len);
  if (packet) expected_ack = packet->_expectedAck;
  return packet;""")
    text = replace(text, "      mesh::Utils::sha256((uint8_t *)&connections[i].expected_ack, 4, data, 9, self_id.pub_key, PUB_KEY_SIZE);", "")
    return replace(text, "      if (pkt) {\n        sendDirect(pkt, contact->out_path, contact->out_path_len);",
                   "      if (pkt) {\n        connections[i].expected_ack = pkt->_expectedAck;\n        sendDirect(pkt, contact->out_path, contact->out_path_len);")


def repeater_source(text):
    return replace(text, """        mesh::Utils::sha256((uint8_t *)&ack_hash, 4, data, 5 + strlen((char *)&data[5]), client->id.pub_key,
                            PUB_KEY_SIZE);""",
                   "        receivedTextAck((uint8_t*)&ack_hash, data, len, client->id.pub_key);")


def room_header(text):
    text = replace(text, "struct PostInfo {", "struct PostInfo {\n  mesh::PacketOrigin origin;")
    return replace(text, "  ClientACL acl;", """  ClientACL acl;
  struct ClientOrigin {
    uint8_t key[PUB_KEY_SIZE]{};
    mesh::PacketOrigin origin;
  } client_origins[MAX_CLIENTS]{};
  void rememberClientOrigin(ClientInfo* client) {
    for (int i = 0; i < acl.getNumClients(); ++i) if (acl.getClientByIdx(i) == client) {
      memcpy(client_origins[i].key, client->id.pub_key, PUB_KEY_SIZE);
      client_origins[i].origin = packetOrigin();
      return;
    }
  }
  mesh::PacketOrigin clientOrigin(ClientInfo* client) {
    for (int i = 0; i < acl.getNumClients(); ++i) if (acl.getClientByIdx(i) == client) {
      if (!memcmp(client_origins[i].key, client->id.pub_key, PUB_KEY_SIZE))
        return client_origins[i].origin;
      break;
    }
    return {};
  }""")


def room_source(text):
    text = replace(text, "      client->extra.room.pending_ack = 0; // clear this, so next push can happen",
                   "      rememberClientOrigin(client);\n"
                   "      client->extra.room.pending_ack = 0; // clear this, so next push can happen")
    text = replace(text, "  posts[idx].author = author; // add to cyclic queue",
                   "  posts[idx].origin = packetOrigin();\n"
                   "  posts[idx].author = author; // add to cyclic queue")
    text = replace(text, "void MyMesh::pushPostToClient(ClientInfo *client, PostInfo &post) {", """void MyMesh::pushPostToClient(ClientInfo *client, PostInfo &post) {
  mesh::PacketOriginScope origin(packetOrigin(),
      mesh::mergePacketOrigin(post.origin, clientOrigin(client)));""")
    text = replace(text, "    if (packet->isRouteFlood()) {\n      client->out_path_len = OUT_PATH_UNKNOWN;",
                   "    rememberClientOrigin(client);\n"
                   "    if (packet->isRouteFlood()) {\n      client->out_path_len = OUT_PATH_UNKNOWN;")
    text = replace(text, "  auto client = acl.getClientByIdx(i);\n  if (type == PAYLOAD_TYPE_TXT_MSG",
                   "  auto client = acl.getClientByIdx(i);\n"
                   "  rememberClientOrigin(client);\n  if (type == PAYLOAD_TYPE_TXT_MSG")
    text = replace(text, "\n    client->last_activity = getRTCClock()->getCurrentTime();",
                   "\n    rememberClientOrigin(client);\n"
                   "    client->last_activity = getRTCClock()->getCurrentTime();")
    text = replace(text, "  mesh::Utils::sha256((uint8_t *)&client->extra.room.pending_ack, 4, reply_data, len, client->id.pub_key, PUB_KEY_SIZE);", "")
    text = replace(text, "  if (reply) {\n    if (client->out_path_len == OUT_PATH_UNKNOWN)", """  if (reply) {
    client->extra.room.pending_ack = reply->_expectedAck;
    if (client->out_path_len == OUT_PATH_UNKNOWN)""")
    text = replace(text, """      mesh::Utils::sha256((uint8_t *)&ack_hash, 4, data, 5 + strlen((char *)&data[5]), client->id.pub_key,
                          PUB_KEY_SIZE);""",
                   "      receivedTextAck((uint8_t*)&ack_hash, data, len, client->id.pub_key);")
    return replace(text, "          mesh::Utils::sha256((uint8_t *)&ack_hash, 4, data, 9, client->id.pub_key, PUB_KEY_SIZE);", """          size_t ack_length = len;
          const uint8_t* ack_data = originalReceivedPlaintext(data, ack_length);
          mesh::Utils::sha256((uint8_t*)&ack_hash, 4, ack_data, 9, client->id.pub_key, PUB_KEY_SIZE);""")
