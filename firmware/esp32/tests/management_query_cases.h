// SPDX-License-Identifier: Apache-2.0
#pragma once

static void management_status_queries() {
  onchip::ProfileJournal original;
  assert(onchip::loadProfileJournal(original));
  Fixture f(true, original.profile);
  onchip::HardwareRNG rng;
  mesh::LocalIdentity signer(&rng);
  char signerHex[65];
  mesh::Utils::toHex(signerHex, signer.pub_key, 32);
  onchip::Management receiver;
#if defined(MESHCORE_MAST_ADMIN) && MESHCORE_MAST_ADMIN
  const auto records = identity_test::durable;
  identity_test::durable[{"mc-mast-admin", "replay"}] = {1};
#endif
  assert(receiver.begin(f.mux, signerHex));
#if defined(MESHCORE_MAST_ADMIN) && MESHCORE_MAST_ADMIN
  assert(!receiver.admin().ready());
#endif
  f.management = &receiver;
  Peer sender;
  sender.server = mesh::Identity(receiver.publicKey());
  auto put64 = [](uint8_t *dest, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i)
      dest[i] = uint8_t(value >> (8 * i));
  };
  auto read64 = [](const uint8_t *source) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i)
      value |= uint64_t(source[i]) << (8 * i);
    return value;
  };
  auto query = [&](uint64_t nonce) {
    Bytes body(121);
    memcpy(body.data(), "MCORE-ROLE-QRY-V1", 17);
    memcpy(body.data() + 17, receiver.publicKey(), 32);
    put64(body.data() + 49, nonce);
    signer.sign(body.data() + 57, body.data(), 57);
    return body;
  };
  auto transmit = [&](const Bytes &body, bool scoped = false) {
    uint8_t secret[32];
    sender.self_id.calcSharedSecret(secret, sender.server);
    auto *packet = sender.createAnonDatagram(
        PAYLOAD_TYPE_ANON_REQ, sender.self_id, sender.server, secret,
        body.data(), body.size());
    assert(packet);
    if (scoped) {
      uint16_t codes[2] = {0x1234, 0};
      sender.sendFlood(packet, codes, 0, 1);
    } else {
      sender.sendFlood(packet);
    }
    now += 20;
    sender.loop();
    sender.loop();
    assert(sender.radio.sent.size() == 1);
    Bytes wire = sender.radio.sent.front();
    sender.radio.sent.pop_front();
    assert((body.size() == 121 || body.size() == 129) &&
           wire.size() == (body.size() == 121 ? 165u : 181u) +
                              (scoped ? 4u : 0u) &&
           wire[0] == (scoped ? 0x1c : 0x1d) &&
           wire[scoped ? 6 : 2] == receiver.publicKey()[0]);
    f.mux.received(wire.data(), wire.size(), -90, 5);
    f.step(80);
    if (f.radio.sent.empty())
      return Bytes{};
    assert(f.radio.sent.size() == 1);
    wire = f.radio.sent.front();
    f.radio.sent.pop_front();
    return wire;
  };
  auto status = [&](const Bytes &wire, uint64_t nonce, bool valid,
                    bool sealed) {
    mesh::Packet response;
    assert(response.readFrom(wire.data(), wire.size()) &&
           response.getPayloadType() == PAYLOAD_TYPE_RESPONSE &&
           response.isRouteFlood() &&
           response.payload_len == 180);
    uint8_t secret[32], plain[184];
    sender.self_id.calcSharedSecret(secret, sender.server);
    assert(mesh::Utils::MACThenDecrypt(
               secret, plain, response.payload + 2,
               response.payload_len - 2) == 176);
    assert(!memcmp(plain, "MCORE-ROLE-STAT-V1", 18) &&
           !memcmp(plain + 18, receiver.publicKey(), 32) &&
           !memcmp(plain + 50, sender.self_id.pub_key, 32) &&
           read64(plain + 82) == nonce &&
           mesh::Identity(receiver.publicKey()).verify(
               plain + 101, plain, 101));
    assert(plain[100] == uint8_t((valid ? 1 : 0) | (sealed ? 2 : 0)));
    if (valid) {
      onchip::ProfileJournal journal;
      assert(onchip::loadProfileJournal(journal));
      assert(read64(plain + 90) == journal.generation &&
             plain[98] == journal.profile.enabled &&
             plain[99] == original.profile.enabled);
    } else {
      assert(read64(plain + 90) == 0 && plain[98] == 0 && plain[99] == 0);
    }
    for (size_t i = 165; i < 176; ++i)
      assert(!plain[i]);
  };

  const auto saved = stored("role-profile");
  const unsigned commits = identity_test::commits;
  status(transmit(query(122), true), 122, true, false);
  now += 1200;
  status(transmit(query(123)), 123, true, false);
  auto bad = query(124);
  bad[57] ^= 1;
  assert(transmit(bad).empty());
  now += 1200;
  status(transmit(query(125)), 125, true, false);
  now += 1200;
  identity_test::failRead = true;
  auto unreadable = transmit(query(126));
  identity_test::failRead = false;
  status(unreadable, 126, false, false);
  assert(stored("role-profile") == saved &&
         identity_test::commits == commits);

  Bytes update(129);
  memcpy(update.data(), "MCORE-ROLE-RF-V1", 16);
  memcpy(update.data() + 16, receiver.publicKey(), 32);
  put64(update.data() + 48, original.generation + 1);
  put64(update.data() + 56, 127);
  update[64] = onchip::RoleProfile::Room;
  signer.sign(update.data() + 65, update.data(), 65);
  now += 1200;
  identity_test::failCommit = true;
  const auto failed = transmit(update);
  identity_test::failCommit = false;
  assert(!failed.empty() && stored("role-profile") == saved);
  now += 1200;
  status(transmit(query(128)), 128, true, true);
  assert(identity_test::commits == commits + 1);
  f.management = nullptr;
  receiver.stop();
#if defined(MESHCORE_MAST_ADMIN) && MESHCORE_MAST_ADMIN
  identity_test::durable = records;
#endif
  puts("PASS signed RF status readback, invalid signature, NVS fault and sealed query");
}
