// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "NativePacketHost.h"
#include <helpers/BaseChatMesh.h>

namespace packet_bridge_test {
using namespace packet_engine;
static std::vector<Fault> errors;
static uint32_t microseconds() { return millis() * 1000; }
static void report(const char *, const Metadata &, Fault fault) { errors.push_back(fault); }

struct Module final : Engine {
  Stage stage = Stage::PlainReceive;
  uint8_t type = PAYLOAD_TYPE_TXT_MSG, slot = UINT8_MAX;
  uint16_t offset = 5;
  uint8_t byte = 'Z';
  bool shrink = false, invalidEmission = false, mutate = true;
  bool forbiddenEmission = false;
  Decision decision = Decision::Continue;
  std::vector<Metadata> seen;
  Decision process(const Metadata &metadata, Call &call) override {
    seen.push_back(metadata);
    if (metadata.stage != stage || (type != UINT8_MAX && metadata.payloadType != type) ||
        (slot != UINT8_MAX && slot != metadata.source && slot != metadata.destination))
      return Decision::Continue;
    if (mutate) assert(call.write(offset, &byte, 1));
    if (shrink) {
      uint8_t data[6];
      assert(call.read(0, data, sizeof(data)));
      assert(call.replace(data, sizeof(data)));
    }
    if (invalidEmission) {
      const uint8_t invalid[] = {0xff};
      assert(call.emit(invalid, sizeof(invalid)));
    }
    if (forbiddenEmission) {
      const uint8_t wire[] = {0x3d, 0, 0x42};
      assert(!call.emit(wire, sizeof(wire)));
    }
    return decision;
  }
};

struct ChatState {
  onchip::LocalRadio radio;
  ArduinoMillis clock;
  onchip::HardwareRNG rng;
  VolatileRTCClock rtc;
  SimpleMeshTables tables;
  peer_pool::StaticPoolPacketManager packets{16};
};
struct Chat : ChatState, BaseChatMesh {
  std::vector<std::string> received;
  int receiveDelay = 0;
  bool reply = false;
  Chat(Fixture &fixture) : BaseChatMesh(radio, clock, rng, rtc, packets, tables) {
    assert(radio.attach(fixture.mux));
    self_id = mesh::LocalIdentity(&rng);
    begin();
  }
  ~Chat() { radio.detach(); }
  bool allowPacketForward(const mesh::Packet *) override { return false; }
  int calcRxDelay(float, uint32_t) const override { return receiveDelay; }
  uint8_t getExtraAckTransmitCount() const override { return 0; }
  void onDiscoveredContact(ContactInfo &, bool, uint8_t, const uint8_t *) override {}
  ContactInfo *processAck(const uint8_t *) override { return nullptr; }
  void onContactPathUpdated(const ContactInfo &) override {}
  void onMessageRecv(const ContactInfo &contact, mesh::Packet *, uint32_t timestamp,
                     const char *text) override {
    received.emplace_back(text);
    if (reply) {
      reply = false;
      uint32_t ack, timeout;
      assert(sendMessage(contact, timestamp + 1, 0, "reply", ack, timeout) ==
             MSG_SEND_SENT_DIRECT);
    }
  }
  void onCommandDataRecv(const ContactInfo &, mesh::Packet *, uint32_t, const char *) override {}
  void onSignedMessageRecv(const ContactInfo &, mesh::Packet *, uint32_t,
                           const uint8_t *, const char *text) override { received.emplace_back(text); }
  uint32_t calcFloodTimeoutMillisFor(uint32_t) const override { return 1000; }
  uint32_t calcDirectTimeoutMillisFor(uint32_t, uint8_t) const override { return 1000; }
  void onSendTimeout() override {}
  void onChannelMessageRecv(const mesh::GroupChannel &, mesh::Packet *,
                            uint32_t, const char *text) override { received.emplace_back(text); }
  uint8_t onContactRequest(const ContactInfo &, uint32_t, const uint8_t *,
                           uint8_t, uint8_t *) override { return 0; }
  void onContactResponse(const ContactInfo &, const uint8_t *, uint8_t) override {}
  bool onPeerPathRecv(mesh::Packet *packet, int index, const uint8_t *secret,
                      uint8_t *path, uint8_t pathLength, uint8_t type,
                      uint8_t *extra, uint8_t length) override {
    // Native extensions can handle text extras; BaseChatMesh only handles ACK/RESPONSE.
    if (type == PAYLOAD_TYPE_TXT_MSG) {
      BaseChatMesh::onPeerDataRecv(packet, type, index, secret, extra, length);
      return false;
    }
    return BaseChatMesh::onPeerPathRecv(packet, index, secret, path, pathLength,
                                      type, extra, length);
  }
};

static Bytes textBody(uint32_t timestamp, const char *text, uint8_t attempt = 0) {
  Bytes data(5);
  queued_tx::put32(data.data(), timestamp);
  data[4] = attempt & 3;
  data.insert(data.end(), text, text + strlen(text));
  if (attempt > 3) { data.push_back(0); data.push_back(attempt); }
  return data;
}
static uint32_t ackFor(const Bytes &body, const uint8_t *key) {
  uint32_t ack;
  mesh::plaintextTextAck(reinterpret_cast<uint8_t *>(&ack), body.data(), body.size(), key);
  return ack;
}
static void inject(Fixture &fixture, Peer &peer, const Bytes &body, bool corrupt = false) {
  uint8_t secret[32], wire[MAX_TRANS_UNIT];
  peer.self_id.calcSharedSecret(secret, peer.server.pub_key);
  auto *packet = peer.createDatagram(PAYLOAD_TYPE_TXT_MSG, peer.server, secret,
                                    body.data(), body.size());
  assert(packet);
  packet->header = (packet->header & ~PH_ROUTE_MASK) | ROUTE_TYPE_DIRECT;
  const auto size = packet->writeTo(wire);
  peer.packets.free(packet);
  if (corrupt) wire[size - 1] ^= 1;
  fixture.mux.received(wire, size, -90, 5);
}
static void pump(Fixture &fixture, Peer &peer, Chat &chat) {
  for (unsigned i = 0; i < 100; ++i) {
    chat.loop();
    peer.pump(fixture, 1);
  }
}
static void attach(onchip::NativePacketHost &host, Module &module) {
  errors.clear();
  assert(host.begin());
  assert(host.pipeline().attach(module, {"bridge-test",
      stageMask(Stage::Relay) | stageMask(Stage::PlainReceive) | stageMask(Stage::PlainCompose),
      200, 1000}) == Registration::Attached);
}

static void incoming_ack_snapshot() {
  Fixture fixture;
  Chat chat(fixture);
  Peer peer;
  peer.server = mesh::Identity(chat.self_id.pub_key);
  ContactInfo contact{};
  contact.id = mesh::Identity(peer.self_id.pub_key);
  contact.out_path_len = 0;
  contact.type = ADV_TYPE_CHAT;
  assert(chat.addContact(contact));
  onchip::NativePacketHost host(fixture.mux, microseconds, report);
  Module module;
  module.slot = chat.radio.sourceSlot();
  attach(host, module);
  for (uint8_t attempt : {uint8_t(0), uint8_t(7)}) {
    module.shrink = attempt == 7;
    const auto body = textBody(++peer.timestamp, "original", attempt);
    const uint32_t expected = ackFor(body, peer.self_id.pub_key);
    inject(fixture, peer, body);
    pump(fixture, peer, chat);
    assert(chat.received.back() == (module.shrink ? "Z" : "Zriginal"));
    assert(std::find(peer.acknowledgements.begin(), peer.acknowledgements.end(), expected) !=
           peer.acknowledgements.end());
    const auto &metadata = module.seen.back();
    assert(metadata.stage == Stage::PlainReceive && metadata.authenticated &&
           !memcmp(metadata.identity, chat.self_id.pub_key, PUB_KEY_SIZE));
  }
  module.type = PAYLOAD_TYPE_PATH; module.offset = 7; module.shrink = false;
  uint8_t secret[PUB_KEY_SIZE];
  peer.self_id.calcSharedSecret(secret, chat.self_id.pub_key);
  for (uint8_t attempt : {uint8_t(0), uint8_t(7)}) {
    const auto body = textBody(++peer.timestamp, "original", attempt);
    const auto expected = ackFor(body, peer.self_id.pub_key);
    auto *packet = peer.createPathReturn(peer.server, secret, nullptr, 0,
                                        PAYLOAD_TYPE_TXT_MSG, body.data(), body.size());
    assert(packet);
    packet->header = (packet->header & ~PH_ROUTE_MASK) | ROUTE_TYPE_DIRECT;
    uint8_t raw[MAX_TRANS_UNIT];
    const auto size = packet->writeTo(raw);
    peer.releasePacket(packet);
    fixture.mux.received(raw, size, -90, 5);
    pump(fixture, peer, chat);
    assert(chat.received.back() == "Zriginal");
    assert(std::find(peer.acknowledgements.begin(), peer.acknowledgements.end(), expected) !=
           peer.acknowledgements.end());
  }
  module.type = PAYLOAD_TYPE_TXT_MSG; module.offset = 5;
  const auto delivered = chat.received.size();
  const auto calls = host.pipeline().calls(0);
  inject(fixture, peer, textBody(++peer.timestamp, "bad-mac"), true);
  pump(fixture, peer, chat);
  assert(host.pipeline().calls(0) == calls && chat.received.size() == delivered);
  module.decision = Decision::Drop;
  inject(fixture, peer, textBody(++peer.timestamp, "dropped"));
  pump(fixture, peer, chat);
  assert(chat.received.size() == delivered && errors.empty());
}

static void outgoing_ack_and_crypto() {
  Fixture fixture;
  Chat chat(fixture);
  Peer peer;
  ContactInfo contact{};
  contact.id = mesh::Identity(peer.self_id.pub_key);
  contact.out_path_len = 0;
  peer.server = mesh::Identity(chat.self_id.pub_key);
  onchip::NativePacketHost host(fixture.mux, microseconds, report);
  Module module;
  module.stage = Stage::PlainCompose;
  module.slot = chat.radio.sourceSlot();
  attach(host, module);
  for (uint8_t attempt : {uint8_t(0), uint8_t(7)}) {
    uint32_t expected = 0, timeout = 0;
    const auto timestamp = ++peer.timestamp;
    const auto original = textBody(timestamp, "outbound", attempt);
    assert(chat.sendMessage(contact, timestamp, attempt, "outbound", expected, timeout) ==
           MSG_SEND_SENT_DIRECT);
    pump(fixture, peer, chat);
    assert(!peer.text.empty());
    const auto &received = peer.text.back();
    assert(received[5] == 'Z');
    assert(expected == ackFor(received, chat.self_id.pub_key) &&
           expected != ackFor(original, chat.self_id.pub_key));
    if (attempt > 3) assert(received[14] == attempt);
  }
  uint8_t secret[32];
  chat.self_id.calcSharedSecret(secret, peer.self_id.pub_key);
  module.offset = 4;
  module.byte = 0xff;
  const auto body = textBody(++peer.timestamp, "fail-open");
  auto *packet = chat.createDatagram(PAYLOAD_TYPE_TXT_MSG, contact.id, secret,
                                    body.data(), body.size());
  assert(packet && packet->_expectedAck == ackFor(body, chat.self_id.pub_key) &&
         errors.back() == Fault::InvalidPacket && !host.pipeline().enabled(0));
  chat.releasePacket(packet);
  assert(host.pipeline().enable(0, true));
  module.offset = 5; module.byte = 'Z'; module.invalidEmission = true;
  packet = chat.createDatagram(PAYLOAD_TYPE_TXT_MSG, contact.id, secret,
                              body.data(), body.size());
  assert(packet && packet->_expectedAck == ackFor(body, chat.self_id.pub_key) &&
         errors.back() == Fault::InvalidPacket && fixture.mux.sourceQueuedCount(fixture.mux.engineSourceSlot()) == 0);
  chat.releasePacket(packet);
  assert(host.pipeline().enable(0, true));
  module.invalidEmission = false; module.decision = Decision::Drop;
  assert(!chat.createDatagram(PAYLOAD_TYPE_TXT_MSG, contact.id, secret,
                             body.data(), body.size()));
}

static void role_ack_snapshots() {
  Fixture fixture;
  for (Role role : {Role::Repeater, Role::Room}) {
    Peer peer;
    peer.target(role);
    assert(peer.login(fixture, role, "test-admin", 3));
    auto &radio = role == Role::Repeater ? onchip::repeaterRadio() : onchip::roomRadio();
    onchip::NativePacketHost host(fixture.mux, microseconds, report);
    Module module;
    module.slot = radio.sourceSlot();
    module.shrink = true;
    attach(host, module);
    const auto body = textBody(++peer.timestamp, "original-post");
    const auto expected = ackFor(body, peer.self_id.pub_key);
    peer.send(fixture, body, false);
    assert(std::find(peer.acknowledgements.begin(), peer.acknowledgements.end(), expected) !=
           peer.acknowledgements.end() && errors.empty());
  }
}

static Bytes decrypt(const mesh::Packet &packet, const uint8_t *secret, size_t prefix) {
  uint8_t bytes[MAX_PACKET_PAYLOAD];
  const auto length = mesh::Utils::MACThenDecrypt(secret, bytes, packet.payload + prefix,
                                                 packet.payload_len - prefix);
  assert(length > 0);
  return {bytes, bytes + length};
}
static void other_plaintext_paths() {
  Fixture fixture;
  Chat chat(fixture);
  Peer peer;
  uint8_t secret[PUB_KEY_SIZE];
  chat.self_id.calcSharedSecret(secret, peer.self_id.pub_key);
  const mesh::Identity destination(peer.self_id.pub_key);
  onchip::NativePacketHost host(fixture.mux, microseconds, report);
  Module module;
  module.stage = Stage::PlainCompose; module.slot = chat.radio.sourceSlot();
  attach(host, module);
  auto body = textBody(++peer.timestamp, "original");
  module.type = PAYLOAD_TYPE_ANON_REQ;
  auto *packet = chat.createAnonDatagram(PAYLOAD_TYPE_ANON_REQ, chat.self_id,
                                        destination, secret, body.data(), body.size());
  assert(packet && decrypt(*packet, secret, 1 + PUB_KEY_SIZE)[5] == 'Z');
  chat.releasePacket(packet);
  mesh::GroupChannel group{};
  memset(group.secret, 0x51, sizeof(group.secret));
  module.type = PAYLOAD_TYPE_GRP_TXT;
  packet = chat.createGroupDatagram(PAYLOAD_TYPE_GRP_TXT, group, body.data(), body.size());
  assert(packet && decrypt(*packet, group.secret, 1)[5] == 'Z');
  chat.releasePacket(packet);
  module.type = PAYLOAD_TYPE_PATH; module.offset = 2;
  packet = chat.createPathReturn(destination, secret, nullptr, 0,
                                PAYLOAD_TYPE_RESPONSE, body.data(), body.size());
  assert(packet && decrypt(*packet, secret, 2)[2] == 'Z');
  chat.releasePacket(packet);
  module.type = PAYLOAD_TYPE_ADVERT; module.offset = 1;
  packet = chat.createSelfAdvert("original");
  assert(packet && packet->payload[100 + 1] == 'Z');
  Bytes signedBody(packet->payload, packet->payload + PUB_KEY_SIZE + 4);
  signedBody.insert(signedBody.end(), packet->payload + 100,
                    packet->payload + packet->payload_len);
  assert(chat.self_id.verify(packet->payload + 36, signedBody.data(), signedBody.size()));
  chat.releasePacket(packet);
  module.mutate = false;
  const auto calls = host.pipeline().calls(0);
  packet = chat.createAdvert(chat.self_id, nullptr, 0);
  assert(packet && host.pipeline().calls(0) == calls + 1);
  chat.releasePacket(packet);
  module.decision = Decision::Drop;
  assert(!chat.createAdvert(chat.self_id, nullptr, 0));
  module.decision = Decision::Continue; module.mutate = true;
  module.type = PAYLOAD_TYPE_TXT_MSG; module.offset = 9;
  Bytes signedText(9);
  queued_tx::put32(signedText.data(), ++peer.timestamp);
  signedText[4] = TXT_TYPE_SIGNED_PLAIN << 2;
  signedText.insert(signedText.end(), {'s', 'i', 'g', 'n', 'e', 'd'});
  packet = chat.createDatagram(PAYLOAD_TYPE_TXT_MSG, destination, secret,
                              signedText.data(), signedText.size());
  assert(packet && packet->_expectedAck == ackFor(decrypt(*packet, secret, 2), destination.pub_key));
  chat.releasePacket(packet);
  assert(errors.empty());

  ChannelDetails channel{};
  strcpy(channel.name, "test");
  memset(channel.channel.secret, 0x31, 16);
  assert(chat.setChannel(0, channel) && chat.getChannel(0, channel));
  module.stage = Stage::PlainReceive; module.type = PAYLOAD_TYPE_GRP_TXT; module.offset = 5;
  body = textBody(++peer.timestamp, "group");
  packet = peer.createGroupDatagram(PAYLOAD_TYPE_GRP_TXT, channel.channel, body.data(), body.size());
  assert(packet);
  packet->header = (packet->header & ~PH_ROUTE_MASK) | ROUTE_TYPE_FLOOD;
  uint8_t raw[MAX_TRANS_UNIT];
  const auto size = packet->writeTo(raw);
  peer.releasePacket(packet);
  fixture.mux.received(raw, size, -90, 5);
  chat.loop();
  assert(chat.received.back() == "Zroup");
  module.type = PAYLOAD_TYPE_ADVERT; module.offset = 1; module.byte = 'Y';
  AdvertDataBuilder builder(ADV_TYPE_CHAT, "original");
  uint8_t appData[MAX_ADVERT_DATA_SIZE];
  const auto appSize = builder.encodeTo(appData);
  packet = peer.createAdvert(peer.self_id, appData, appSize);
  assert(packet);
  packet->header = (packet->header & ~PH_ROUTE_MASK) | ROUTE_TYPE_FLOOD;
  const auto advertSize = packet->writeTo(raw);
  peer.releasePacket(packet);
  fixture.mux.received(raw, advertSize, -90, 5);
  chat.loop();
  const auto *contact = chat.lookupContactByPubKey(peer.self_id.pub_key, PUB_KEY_SIZE);
  assert(contact && !strcmp(contact->name, "Yriginal") && errors.empty());
}

struct Forwarder : mesh::Dispatcher {
  Forwarder(onchip::LocalRadio &radio, ArduinoMillis &clock, mesh::PacketManager &packets)
      : Dispatcher(radio, clock, packets) { begin(); }
  mesh::DispatcherAction onRecvPacket(mesh::Packet *packet) override {
    packet->payload[0] = 0x41;
    return ACTION_RETRANSMIT(4);
  }
};
static void relay_edits_and_rollback() {
  Fixture fixture;
  onchip::LocalRadio radio;
  assert(radio.attach(fixture.mux));
  ArduinoMillis clock;
  peer_pool::StaticPoolPacketManager packets(16);
  Forwarder forwarder(radio, clock, packets);
  onchip::NativePacketHost host(fixture.mux, microseconds, report);
  Module module;
  module.stage = Stage::Relay; module.type = PAYLOAD_TYPE_RAW_CUSTOM;
  module.slot = radio.sourceSlot(); module.offset = 2; module.byte = 0x42;
  attach(host, module);
  const uint8_t raw[] = {(PAYLOAD_TYPE_RAW_CUSTOM << PH_TYPE_SHIFT) | ROUTE_TYPE_FLOOD, 0, 0x31};
  for (unsigned scenario = 0; scenario < 4; ++scenario) {
    if (scenario == 1) module.decision = Decision::Drop;
    if (scenario == 2) {
      module.decision = Decision::Continue;
      module.offset = 1; module.byte = 0xff;
    }
    if (scenario == 3) module.invalidEmission = true;
    if (!host.pipeline().enabled(0)) assert(host.pipeline().enable(0, true));
    fixture.mux.received(raw, sizeof(raw), -90, 5);
    forwarder.loop();
    if (scenario == 1) assert(radio.queuedCount() == 0);
    else {
      assert(radio.queuedCount() == 1);
      fixture.mux.serviceTransmit();
      assert(fixture.radio.sent.back()[2] == (scenario == 0 ? 0x42 : 0x41));
      now += 20; fixture.mux.serviceTransmit(); forwarder.loop();
      if (scenario > 1) assert(errors.back() == Fault::InvalidPacket && !host.pipeline().enabled(0));
    }
  }
  radio.detach();
}
struct ForwardMesh : ChatState, mesh::Mesh {
  ForwardMesh(Fixture &fixture) : Mesh(radio, clock, rng, rtc, packets, tables) {
    assert(radio.attach(fixture.mux));
    self_id = mesh::LocalIdentity(&rng);
    begin();
  }
  ~ForwardMesh() { radio.detach(); }
  bool allowPacketForward(const mesh::Packet *) override { return true; }
  uint8_t getExtraAckTransmitCount() const override { return 1; }
};
static void direct_ack_relay_paths() {
  Fixture fixture;
  ForwardMesh mesh(fixture);
  onchip::NativePacketHost host(fixture.mux, microseconds, report);
  Module module;
  module.stage = Stage::Relay; module.type = UINT8_MAX;
  module.slot = mesh.radio.sourceSlot(); module.mutate = false;
  module.decision = Decision::Drop;
  attach(host, module);
  for (uint8_t type : {uint8_t(PAYLOAD_TYPE_ACK), uint8_t(PAYLOAD_TYPE_MULTIPART)}) {
    uint8_t body[] = {0x41, 0x42, 0x43, 0x44};
    auto *packet = type == PAYLOAD_TYPE_ACK ? mesh.createAck(body, sizeof(body)) :
                                            mesh.createMultiAck(body, sizeof(body), 1);
    assert(packet);
    packet->header = (packet->header & ~PH_ROUTE_MASK) | ROUTE_TYPE_DIRECT;
    packet->path_len = 1; packet->path[0] = mesh.self_id.pub_key[0];
    uint8_t raw[MAX_TRANS_UNIT];
    const auto length = packet->writeTo(raw);
    mesh.releasePacket(packet);
    fixture.mux.received(raw, length, -90, 5);
    mesh.loop();
    assert(mesh.radio.queuedCount() == 0);
  }
  assert(module.seen.size() == 4 && errors.empty());
  for (const auto &metadata : module.seen)
    assert(metadata.stage == Stage::Relay && !metadata.local &&
           metadata.rssi == -90 && metadata.snrQuarterDb == 20 &&
           !memcmp(metadata.identity, mesh.self_id.pub_key, PUB_KEY_SIZE));
  assert(module.seen[0].payloadType == PAYLOAD_TYPE_MULTIPART &&
         module.seen[1].payloadType == PAYLOAD_TYPE_ACK);
}
static void delayed_packet_signal() {
  Fixture fixture;
  Chat chat(fixture);
  Peer peer;
  peer.server = mesh::Identity(chat.self_id.pub_key);
  ContactInfo contact{};
  contact.id = mesh::Identity(peer.self_id.pub_key);
  contact.out_path_len = 0;
  assert(chat.addContact(contact));
  onchip::NativePacketHost host(fixture.mux, microseconds, report);
  Module module;
  module.mutate = false;
  module.slot = chat.radio.sourceSlot();
  attach(host, module);
  uint8_t secret[PUB_KEY_SIZE], raw[MAX_TRANS_UNIT];
  peer.self_id.calcSharedSecret(secret, chat.self_id.pub_key);
  const auto body = textBody(++peer.timestamp, "delayed");
  auto *packet = peer.createDatagram(PAYLOAD_TYPE_TXT_MSG, peer.server, secret,
                                    body.data(), body.size());
  assert(packet);
  packet->header |= ROUTE_TYPE_FLOOD;
  const auto length = packet->writeTo(raw);
  peer.releasePacket(packet);
  chat.receiveDelay = 2000;
  fixture.mux.received(raw, length, -101, 1.25f);
  chat.loop();
  assert(chat.received.empty());
  inject(fixture, peer, textBody(++peer.timestamp, "immediate"));
  chat.loop();
  assert(chat.received.size() == 1 && chat.received[0] == "immediate");
  now += 2000;
  chat.loop();
  assert(chat.received.size() == 2 && chat.received[1] == "delayed");
  std::vector<Metadata> receptions;
  for (const auto &metadata : module.seen)
    if (metadata.stage == Stage::PlainReceive) receptions.push_back(metadata);
  assert(receptions.size() == 2 && receptions[0].rssi == -90 &&
         receptions[0].snrQuarterDb == 20 && receptions[1].rssi == -101 &&
         receptions[1].snrQuarterDb == 5 && errors.empty());
}
static void generated_native_reply_origin() {
  for (bool generated : {false, true}) for (bool forbidden : {false, true}) {
    Fixture fixture;
    Chat chat(fixture);
    Peer peer;
    peer.server = mesh::Identity(chat.self_id.pub_key);
    ContactInfo contact{};
    contact.id = mesh::Identity(peer.self_id.pub_key);
    contact.out_path_len = 0;
    assert(chat.addContact(contact));
    chat.reply = true;
    onchip::NativePacketHost host(fixture.mux, microseconds, report);
    Module module;
    module.stage = Stage::PlainCompose;
    module.slot = chat.radio.sourceSlot();
    module.forbiddenEmission = forbidden;
    module.mutate = forbidden;
    errors.clear();
    assert(host.begin());
    assert(host.pipeline().attach(module, {"origin-test", 0xff, 200, 1000}) ==
           Registration::Attached);
    uint8_t secret[PUB_KEY_SIZE];
    peer.self_id.calcSharedSecret(secret, chat.self_id.pub_key);
    const auto body = textBody(++peer.timestamp, "request");
    auto *packet = peer.createDatagram(PAYLOAD_TYPE_TXT_MSG, peer.server, secret,
                                      body.data(), body.size());
    assert(packet);
    packet->header |= ROUTE_TYPE_DIRECT;
    Emission emission;
    emission.length = packet->writeTo(emission.bytes);
    peer.releasePacket(packet);
    const auto free = chat.packets.getFreeCount();
    if (generated) assert(fixture.mux.admitEnginePackets(&emission, 1));
    else {
      uint32_t job;
      assert(onchip::companionRadio().queueTransmit(
          emission.bytes, emission.length, 0, 0, 0, job));
    }
    pump(fixture, peer, chat);
    assert(chat.received.size() == 1 && chat.received[0] == "request" &&
           peer.text.size() == 1 && peer.text[0][5] == 'r');
    assert(forbidden ? errors.size() == 1 && errors[0] == Fault::EmissionOrigin &&
                       !host.pipeline().enabled(0) : errors.empty());
    unsigned stages = 0;
    for (const auto &metadata : module.seen) {
      if (metadata.source != chat.radio.sourceSlot()) continue;
      if (metadata.stage == Stage::PlainCompose || metadata.stage == Stage::Admission ||
          metadata.stage == Stage::Transmit) {
        stages |= stageMask(metadata.stage);
        assert(!metadata.local && metadata.reflectionOrigin &&
               metadata.engineOrigin == generated);
      }
    }
    const auto expectedStages = stageMask(Stage::PlainCompose) |
        (forbidden ? 0 : stageMask(Stage::Admission) | stageMask(Stage::Transmit));
    assert(stages == expectedStages && chat.radio.getPacketsSent() == 2 &&
           chat.radio.queuedCount() == 0 && chat.packets.getFreeCount() == free);
    mesh::QueuedRadioStats stats;
    assert(chat.radio.getQueuedRadioStats(stats) && stats.source_successes == 2 &&
           stats.source_rf_ms == 40);
  }
}
static void wire_bounds() {
  const uint8_t valid[] = {0x3d, 0, 0x42};
  assert(onchip::validNativeWire(valid, sizeof(valid)));
  const uint8_t transport[] = {0x3c, 1, 2, 3, 4, 0, 0x42};
  assert(onchip::validNativeWire(transport, sizeof(transport)));
  for (const auto &bytes : {Bytes{}, Bytes{0x3d}, Bytes{0x3d, 0},
       Bytes{0x3c, 0, 0, 0}, Bytes{0x3d, 0xff, 1}, Bytes{0x3d, 63, 1},
       Bytes{0x7d, 0, 1}, Bytes{0xff, 0, 1}})
    assert(!onchip::validNativeWire(bytes.data(), bytes.size()));
  const uint8_t data[] = {1, 2, 3};
  mesh::OriginalPlaintext active;
  {
    mesh::OriginalPlaintextScope outer(active, data, sizeof(data));
    const uint8_t changed[] = {4, 5};
    outer.changed(changed, sizeof(changed));
    assert(active.bytes && active.length == sizeof(data));
    const auto original = active.bytes;
    {
      mesh::OriginalPlaintextScope nested(active, data, sizeof(data));
      nested.changed(data, sizeof(data));
      assert(!active.bytes);
    }
    assert(active.bytes == original);
  }
  assert(!active.bytes);
}
static void deferred_room_origin() {
  struct Recorder final : Engine {
    std::vector<Metadata> posts;
    Decision process(const Metadata &metadata, Call &call) override {
      if (metadata.stage != Stage::PlainCompose ||
          metadata.payloadType != PAYLOAD_TYPE_TXT_MSG) return Decision::Continue;
      uint8_t body[MAX_PACKET_PAYLOAD]{};
      assert(call.read(0, body, call.size()));
      if (call.size() >= 9 && body[4] >> 2 == 2 &&
          !strcmp(reinterpret_cast<const char *>(body + 9), "origin-post"))
        posts.push_back(metadata);
      return Decision::Continue;
    }
  };
  for (bool generated : {false, true}) for (bool postOrigin : {false, true}) {
    Fixture fixture;
    Peer author, reader;
    author.target(Role::Room); reader.target(Role::Room);
    assert(author.login(fixture, Role::Room, "test-room", 2));
    assert(reader.login(fixture, Role::Room, "test-room", 2));
    onchip::NativePacketHost host(fixture.mux, microseconds, report);
    Recorder recorder;
    errors.clear();
    assert(host.begin());
    assert(host.pipeline().attach(recorder, {"room-origin",
        stageMask(Stage::PlainCompose), 500, 1000}) == Registration::Attached);
    const auto encode = [](Peer &peer, uint8_t type, const Bytes &body) {
      uint8_t secret[PUB_KEY_SIZE];
      peer.self_id.calcSharedSecret(secret, peer.server.pub_key);
      auto *packet = peer.createDatagram(type, peer.server, secret, body.data(), body.size());
      assert(packet);
      packet->header |= ROUTE_TYPE_DIRECT;
      Emission emission;
      emission.length = packet->writeTo(emission.bytes);
      peer.releasePacket(packet);
      return emission;
    };
    const auto post = encode(author, PAYLOAD_TYPE_TXT_MSG,
                             textBody(++author.timestamp, "origin-post"));
    const auto reflect = [&](const Emission &packet) {
      if (generated) assert(fixture.mux.admitEnginePackets(&packet, 1));
      else {
        uint32_t job;
        assert(onchip::companionRadio().queueTransmit(
            packet.bytes, packet.length, 0, 0, 0, job));
      }
    };
    if (postOrigin) reflect(post);
    else {
      fixture.mux.received(post.bytes, post.length, -90, 5);
      fixture.step(3);
      Bytes keepalive(9);
      queued_tx::put32(keepalive.data(), ++reader.timestamp);
      keepalive[4] = REQ_TYPE_KEEP_ALIVE;
      reflect(encode(reader, PAYLOAD_TYPE_REQ, keepalive));
    }
    reader.pump(fixture, 600);
    assert(!reader.text.empty() && !recorder.posts.empty() && errors.empty());
    for (const auto &metadata : recorder.posts)
      assert(metadata.reflectionOrigin && metadata.engineOrigin == generated);
  }
}
static void live_role_composers() {
  Fixture fixture;
  const uint8_t body[] = {1, 0, 0, 0, 0, 'o', 'k', 0};
  const uint8_t key[16] = {0x51};
  assert(onchip::companionSetChannel(0, "#packet", key));
  for (Role role : {Role::Repeater, Role::Room, Role::Companion}) {
    RadioDashboard::RoleStatus status;
    onchip::roleStatus(role, status);
    assert(status.ready && status.has_identity);
    const auto saved = stored(onchip::roleName(role));
    ComposeRequest request;
    request.kind = uint32_t(ComposeKind::Advert); request.payloadType = PAYLOAD_TYPE_ADVERT;
    request.route = ROUTE_TYPE_DIRECT; memcpy(request.identity, status.public_key, 32);
    uint8_t wire[Capacity]; uint16_t length = sizeof(wire);
    assert(onchip::nativeRoleComposePacket(role, request, nullptr, 0, wire, length) == Fault::None);
    mesh::Packet advert;
    assert(advert.readFrom(wire, length) && advert.payload_len == 100 &&
           !memcmp(advert.payload, status.public_key, 32));
    mesh::Identity owner(status.public_key);
    assert(owner.verify(advert.payload + 36, advert.payload, 36));
    request.kind = uint32_t(ComposeKind::Group); request.payloadType = PAYLOAD_TYPE_GRP_TXT;
    length = sizeof(wire);
    const auto result = onchip::nativeRoleComposePacket(role, request, body, sizeof(body), wire, length);
    assert(result == (role == Role::Companion ? Fault::None : Fault::Unavailable));
    if (result == Fault::None) {
      mesh::Packet packet; assert(packet.readFrom(wire, length));
      uint8_t secret[32]{}, plaintext[MAX_PACKET_PAYLOAD];
      memcpy(secret, key, sizeof(key));
      assert(mesh::Utils::MACThenDecrypt(secret, plaintext, packet.payload + 1, packet.payload_len - 1) >= int(sizeof(body)));
      assert(!memcmp(plaintext, body, sizeof(body)));
      length = sizeof(wire); request.channel = MAX_GROUP_CHANNELS;
      assert(onchip::nativeRoleComposePacket(role, request, body, sizeof(body), wire, length) == Fault::Unavailable);
    }
    assert(stored(onchip::roleName(role)) == saved);
    length = sizeof(wire); request.identity[0] ^= 1;
    assert(onchip::nativeRoleComposePacket(role, request, body, sizeof(body), wire, length) == Fault::Unavailable);
  }
  puts("PASS active native-role composition, signed empty adverts, configured companion groups and unchanged identities");
}
static void run() {
  wire_bounds();
  incoming_ack_snapshot();
  outgoing_ack_and_crypto();
  role_ack_snapshots();
  other_plaintext_paths();
  relay_edits_and_rollback();
  direct_ack_relay_paths();
  delayed_packet_signal();
  generated_native_reply_origin();
  deferred_room_origin();
  live_role_composers();
  puts("Native relay/plaintext packet bridges and original/final ACK contracts passed");
}
} // namespace packet_bridge_test
