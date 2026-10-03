// SPDX-License-Identifier: Apache-2.0
struct DiscoveryPeer : NativeChat {
  using NativeChat::NativeChat;
  uint32_t tag = 0;
  unsigned responses = 0;
  Bytes response;
  void onContactResponse(const ContactInfo &, const uint8_t *data, uint8_t size) override {
    assert(size > 4 && queued_tx::get32(data) == tag);
    response.assign(data, data + size);
    ++responses;
  }
  int request(ContactInfo &contact, bool flood, uint8_t mask = 0xfe, uint8_t type = 3) {
    // companion-v1.17.1 MyMesh::CMD_SEND_PATH_DISCOVERY_REQ: inverse BASE
    // mask, three reserved zero bytes and four random bytes via sendRequest.
    uint8_t data[9] = {type, mask, 0, 0, 0};
    getRNG()->random(data + 5, 4);
    const auto saved = contact.out_path_len;
    if (flood) contact.out_path_len = OUT_PATH_UNKNOWN;
    uint32_t timeout;
    const auto result = sendRequest(contact, data, sizeof(data), tag, timeout);
    contact.out_path_len = saved;
    return result;
  }
};

static void native_bot_discovery() {
  const auto baseline = identity_test::durable;
  for (bool scoped : {false, true}) for (uint8_t width : {1, 2, 3}) {
    identity_test::durable = baseline;
    assert(saveBotDiscovery(false));
    Fixture f;
    DiscoveryPeer peer(width, scoped);
    NativeRelay relay(scoped);
    assert(f.bot.begin(f.mux));
    size_t botSent = 0, peerSent = 0, relaySent = 0;
    const auto run = [&](unsigned ticks = 400) {
      while (ticks--) {
        f.step(1); peer.loop(); relay.loop();
        while (botSent < f.radio.sent.size()) relay.radio.input.push_back(f.radio.sent[botSent++]);
        while (peerSent < peer.radio.sent.size()) relay.radio.input.push_back(peer.radio.sent[peerSent++]);
        while (relaySent < relay.radio.sent.size()) {
          peer.radio.input.push_back(relay.radio.sent[relaySent]);
          f.deliver(relay.radio.sent[relaySent++]);
        }
      }
    };
    auto *advert = peer.createSelfAdvert("discovery-companion");
    assert(advert); peer.sendFlood(advert, 0, width);
    run();
    auto *contact = peer.lookupContactByPubKey(f.bot.publicKey(), 32);
    assert(contact && contact->out_path_len == OUT_PATH_UNKNOWN);
    timeMs += 61000;
    assert(peer.request(*contact, true) == MSG_SEND_SENT_FLOOD);
    run();
    assert(peer.response.empty() && contact->out_path_len == OUT_PATH_UNKNOWN);
    assert(f.bot.setDiscovery(true));
    bool saved = false;
    assert(loadBotDiscovery(saved) && saved);
    // Direct REQ without a cached return route must flood RESPONSE, not invent
    // a direct return path from an inbound direct packet.
    contact->out_path_len = 0;
    const auto before = f.radio.sent.size();
    assert(peer.request(*contact, false) == MSG_SEND_SENT_DIRECT);
    while (peerSent == peer.radio.sent.size()) { f.step(1); peer.loop(); }
    f.deliver(peer.radio.sent[peerSent++]);
    run();
    assert(peer.responses == 1 && contact->out_path_len == 0);
    bool fallback = false;
    for (size_t i = before; i < f.radio.sent.size(); ++i) {
      mesh::Packet p; assert(p.readFrom(f.radio.sent[i].data(), f.radio.sent[i].size()));
      if (p.getPayloadType() == PAYLOAD_TYPE_RESPONSE) fallback = p.isRouteFlood();
    }
    assert(fallback);
    timeMs += 2000;
    contact->out_path_len = OUT_PATH_UNKNOWN;
    assert(peer.request(*contact, true) == MSG_SEND_SENT_FLOOD);
    run(750);
    assert(peer.responses == 2 && peer.paths == 1);
    const uint8_t pathLength = ((width - 1) << 6) | 1;
    assert(contact->out_path_len == pathLength &&
           !memcmp(contact->out_path, relay.self_id.pub_key, width));
    const auto directStart = f.radio.sent.size();
    timeMs += 2000;
    assert(peer.request(*contact, false, 0) == MSG_SEND_SENT_DIRECT);
    run();
    assert(peer.responses == 3);
    bool direct = false;
    for (size_t i = directStart; i < f.radio.sent.size(); ++i) {
      mesh::Packet p; assert(p.readFrom(f.radio.sent[i].data(), f.radio.sent[i].size()));
      if (p.getPayloadType() == PAYLOAD_TYPE_RESPONSE)
        direct |= p.isRouteDirect() && p.path_len == pathLength &&
                  !memcmp(p.path, relay.self_id.pub_key, width);
    }
    assert(direct);
#if defined(ONCHIP_BOT_RUNTIME_TEST)
    assert(peer.response.size() >= 8);
    assert(peer.response[4] == TELEM_CHANNEL_SELF && peer.response[5] == LPP_VOLTAGE &&
           peer.response[6] == 1 && peer.response[7] == 0x90);
    unsigned meaningful = 8;
    if (std::isfinite(board.getMCUTemperature())) {
      assert(peer.response.size() >= 12 && peer.response[8] == TELEM_CHANNEL_SELF &&
             peer.response[9] == LPP_TEMPERATURE && peer.response[10] == 0 &&
             peer.response[11] == 215);
      meaningful = 12;
    }
#else
    // Host worker has no fabricated radio voltage or location.
    const unsigned meaningful = 4;
#endif
    for (unsigned i = meaningful; i < peer.response.size(); ++i) assert(peer.response[i] == 0);
    timeMs += 2000;
    assert(peer.request(*contact, true, 0xff) == MSG_SEND_SENT_FLOOD);
    run();
    assert(peer.responses == 3);  // Request explicitly excludes BASE.
    assert(peer.request(*contact, true, 0, 1) == MSG_SEND_SENT_FLOOD);
    run();
    assert(peer.responses == 3);  // Chat bot does not implement repeater status.
    assert(f.bot.setDiscovery(false));
    assert(peer.request(*contact, true) == MSG_SEND_SENT_FLOOD);
    run();
    assert(peer.responses == 3);
  }
  identity_test::durable = baseline;
  puts("PASS native BaseChat discovery: explicit grant, signed contacts, direct/flood, scoped/unscoped 1/2/3-byte paths, reciprocal route learning, base-only padded telemetry");
}

static void native_bot_discovery_limits() {
  const auto baseline = identity_test::durable;
  assert(saveBotDiscovery(false));
  Fixture f; f.start();
  Peer peer, unknown;
  f.learn(peer);
  const auto request = [&](Peer &from, Bytes data, bool flood = true, uint8_t width = 1,
                           unsigned count = 0) {
    uint8_t secret[32]; from.self_id.calcSharedSecret(secret, f.bot.publicKey());
    auto *p = from.createDatagram(PAYLOAD_TYPE_REQ, mesh::Identity(f.bot.publicKey()),
                                 secret, data.data(), data.size());
    assert(p);
    p->header |= flood ? ROUTE_TYPE_FLOOD : ROUTE_TYPE_DIRECT;
    p->setPathHashSizeAndCount(width, count);
    memset(p->path, 0x55, width * count);
    return from.wire(p);
  };
  uint32_t sequence = 0;
  const auto body = [&] {
    Bytes data(13); queued_tx::put32(data.data(), ++sequence); data[4] = 3; data[5] = 0xfe;
    return data;
  };
  const auto attempt = [&](const Bytes &wire, unsigned expected) {
    timeMs += 2000; f.radio.sent.clear(); f.deliver(wire); f.step(250);
    assert(f.radio.sent.size() == expected);
  };
  const auto quietDenial = [&](const Bytes &wire) {
    RadioDashboard::RoleStatus before, after;
    f.bot.dashboardStatus(before);
    const auto rejected = f.bot.counters().rejected, malformed = f.bot.counters().malformed;
    attempt(wire, 0);
    f.bot.dashboardStatus(after);
    assert(!strcmp(before.fault, after.fault) && before.ready == after.ready);
    assert(f.bot.counters().rejected == rejected + 1 && f.bot.counters().malformed == malformed);
  };
  for (unsigned i = 0; i < 8; ++i) quietDenial(request(peer, body(), i % 2));
  assert(f.bot.setDiscovery(true));
  attempt(request(unknown, body()), 0);
  do { unknown.self_id = mesh::LocalIdentity(&unknown.rng); }
  while (unknown.self_id.pub_key[0] != peer.self_id.pub_key[0]);
  assert(memcmp(unknown.self_id.pub_key, peer.self_id.pub_key, 32));
  attempt(request(unknown, body()), 0);  // Same routing hash is not authentication.
  auto forged = unknown.advert();
  forged.back() ^= 1;
  f.deliver(forged); f.step(20);
  attempt(request(unknown, body()), 0);
  f.learn(unknown);
  attempt(request(unknown, body()), 1);
  f.bot.setCommandAdmission(false);
  quietDenial(request(peer, body()));
  f.bot.setCommandAdmission(true);
  auto badMac = request(peer, body()); badMac.back() ^= 1;
  attempt(badMac, 0);
  auto shortCipher = request(peer, body()); shortCipher.pop_back();
  attempt(shortCipher, 0);
  auto badVersion = request(peer, body()); badVersion[0] |= 0x40;
  attempt(badVersion, 0);
  auto invalidPath = request(peer, body()); invalidPath[1] = 0xc0;
  attempt(invalidPath, 0);
  for (unsigned position : {6, 7, 8, 13, 14, 15, 16}) {
    auto data = body(); data.resize(std::max(size_t(position + 1), data.size()));
    data[position] = 1;
    attempt(request(peer, data), 0);
  }
  auto unknownType = body(); unknownType[4] = 0x7f;
  quietDenial(request(peer, unknownType));
  unknownType[4] = 1;
  quietDenial(request(peer, unknownType));
  auto noBase = body(); noBase[5] = 1;
  quietDenial(request(peer, noBase));
  RadioDashboard::RoleStatus malformedStatus;
  f.bot.dashboardStatus(malformedStatus);
  assert(strstr(malformedStatus.fault, "invalid length"));
  assert(f.bot.setDiscovery(false));
  quietDenial(request(peer, body()));
  assert(f.bot.setDiscovery(true));
  const auto duplicate = request(peer, body());
  attempt(duplicate, 1);
  attempt(duplicate, 0);
  // Reflected encrypted requests cannot trigger native responses.
  f.radio.sent.clear();
  f.reflect(request(peer, body())); f.step(250);
  assert(f.radio.sent.size() == 1);
  timeMs += 61000;
  for (uint8_t width : {1, 2, 3}) {
    const unsigned boundary = width == 1 ? 10 : width == 2 ? 5 : 14;
    for (unsigned count : {0u, boundary, std::min(63u, unsigned(MAX_PATH_SIZE / width))}) {
      const auto data = body();
      attempt(request(peer, data, true, width, count), 1);
      mesh::Packet p;
      assert(p.readFrom(f.radio.sent[0].data(), f.radio.sent[0].size()));
      assert(p.getPayloadType() == PAYLOAD_TYPE_PATH && p.payload_len <= MAX_PACKET_PAYLOAD &&
             p.getRawLength() <= 255 && p.getPathHashSize() == width);
      uint8_t secret[32], plain[MAX_PACKET_PAYLOAD]{};
      peer.self_id.calcSharedSecret(secret, f.bot.publicKey());
      const int size = mesh::Utils::MACThenDecrypt(secret, plain, p.payload + 2, p.payload_len - 2);
      const unsigned pathBytes = count * width;
      assert(size > int(pathBytes + 6) && plain[0] == ((width - 1) << 6 | count));
      assert(plain[pathBytes + 1] == PAYLOAD_TYPE_RESPONSE &&
             !memcmp(plain + pathBytes + 2, data.data(), 4));
    }
  }
  timeMs += 61000; f.radio.sent.clear();
  RadioDashboard::RoleStatus beforeLimit, afterLimit;
  f.bot.dashboardStatus(beforeLimit);
  f.deliver(request(peer, body())); f.deliver(request(unknown, body())); f.step(250);
  assert(f.radio.sent.size() == 1);
  f.bot.dashboardStatus(afterLimit);
  assert(!strcmp(beforeLimit.fault, afterLimit.fault));
  timeMs += 61000; f.radio.airtime = 361;
  quietDenial(request(peer, body()));
  f.radio.airtime = 10;
  // A failed write/readback revokes live access; retry or reboot must be explicit.
  identity_test::failCommit = true;
  assert(!f.bot.setDiscovery(false));
  identity_test::failCommit = false;
  attempt(request(peer, body()), 0);
  identity_test::failRead = true;
  assert(!f.bot.setDiscovery(true));
  identity_test::failRead = false;
  attempt(request(peer, body()), 0);
  assert(f.bot.setDiscovery(true));
  f.bot.stop(); f.start(); f.learn(peer);
  attempt(request(peer, body(), false), 1);
  assert(f.bot.setDiscovery(false));
  f.bot.stop(); f.start(); f.learn(peer);
  attempt(request(peer, body()), 0);
  identity_test::durable[{"mc-onchip", "bot-discovery"}] = Bytes{'B', 'D', 'P', 1, 2};
  bool enabled = true;
  assert(!loadBotDiscovery(enabled) && !enabled);
  f.bot.stop();
  assert(!f.bot.begin(f.mux));
  identity_test::durable = baseline;
  puts("PASS native discovery bounds: unknown/forged keys, MAC, malformed ciphertext/version/path/reserved/padding, replay/reflection, maximum paths, quiet policy/rate/airtime denial preserving faults, persistence and fail-closed policy");
}
