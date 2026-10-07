// SPDX-License-Identifier: Apache-2.0
namespace contact_recovery_test {
struct Saved {
  uint8_t key[32]{};
  Bytes advert;
};
static std::vector<Saved> saved;
static unsigned lookups;
static bool lookup(const uint8_t *hash, unsigned &cursor, uint8_t *key,
                   uint8_t *advert, uint8_t &size) {
  ++lookups;
  while (cursor < saved.size()) {
    const auto &entry = saved[cursor++];
    if (!mesh::Identity(entry.key).isHashMatch(hash)) continue;
    memcpy(key, entry.key, 32);
    assert(entry.advert.size() <= 255);
    size = uint8_t(entry.advert.size());
    if (size) memcpy(advert, entry.advert.data(), size);
    return true;
  }
  return false;
}
static Saved snapshot(Peer &peer) {
  Saved entry;
  memcpy(entry.key, peer.self_id.pub_key, 32);
  entry.advert = peer.advert();
  return entry;
}
static std::string status(Fixture &fixture) {
  char text[256];
  fixture.bot.contactStatus(text, sizeof(text));
  return text;
}
}

static void native_contact_recovery() {
  using namespace contact_recovery_test;
  assert(saveBotEnabled(true));
  Peer peer;
  std::unique_ptr<Peer> collision;
  for (unsigned i = 0; i < 10000; ++i) {
    auto candidate = std::make_unique<Peer>();
    if (candidate->self_id.isHashMatch(peer.self_id.pub_key)) {
      collision = std::move(candidate);
      break;
    }
  }
  assert(collision && !collision->self_id.matches(peer.self_id.pub_key));
  const auto first = snapshot(peer), second = snapshot(*collision);
  saved = {first, second};
  lookups = 0;
  {
    saved = {first};
    Fixture f;
    f.bot.setContactLookup(lookup);
    f.start();
    std::vector<std::unique_ptr<Peer>> observed;
    for (unsigned i = 0; i < 16; ++i) {
      observed.push_back(std::make_unique<Peer>());
      f.learn(*observed.back());
    }
    assert(status(f).find("Contacts=16/16 observed=16") == 0);
    f.ingest(peer.command(f.bot.publicKey(), "!ping"));
    f.step();
    assert(peer.replies(f.bot.publicKey(), f.radio).empty());
    assert(status(f).find("Contacts=16/16 observed=16") == 0);
    assert(status(f).find("recovered=0 rejected=1") != std::string::npos);
    assert(f.command(*observed.front(), "!ping") == "Pong");
  }
  saved = {first, second};
  {
    Fixture f;
    f.bot.setContactLookup(lookup);
    f.start();
    assert(f.command(peer, "!ping") == "Pong");
    auto info = status(f);
    assert(info.find("Contacts=2/16 observed=0 routed=0") == 0);
    assert(info.find("recovered=2 rejected=0") != std::string::npos);
    assert(f.command(*collision, "!ping") == "Pong");
    assert(f.command(peer, "!neighbors") == "No observed signed adverts; not a live neighbor scan");
    for (const auto &raw : f.radio.sent) {
      mesh::Packet packet{};
      assert(packet.readFrom(raw.data(), raw.size()));
      assert(packet.getPayloadType() != PAYLOAD_TYPE_ADVERT);
    }
    f.ingest(first.advert);
    f.step();
    assert(status(f).find("Contacts=2/16 observed=1 routed=0") == 0);
    assert(f.command(peer, "!neighbors").find("Seen 1/1 native-test-peer") == 0);
    const auto prior = lookups;
    f.ingest(peer.command(f.bot.publicKey(), "!ping"));
    f.step(5);
    assert(lookups == prior && "Recovery scans must not repeat within one second");
    f.bot.stop();
    f.start();
    assert(f.command(peer, "!ping") == "Pong");
    assert(status(f).find("Contacts=2/16 observed=0 routed=0") == 0);
  }
  for (unsigned failure = 0; failure < 4; ++failure) {
    saved = {first};
    if (failure == 0) saved[0].advert.clear();
    if (failure == 1) {
      mesh::Packet packet{};
      assert(packet.readFrom(saved[0].advert.data(), saved[0].advert.size()));
      packet.payload[36] ^= 1;
      saved[0].advert.resize(packet.getRawLength());
      packet.writeTo(saved[0].advert.data());
    }
    if (failure == 2) saved[0].advert = second.advert;
    if (failure == 3) {
      mesh::Packet packet{};
      assert(packet.readFrom(saved[0].advert.data(), saved[0].advert.size()));
      packet.header = (PAYLOAD_TYPE_REQ << PH_TYPE_SHIFT) | ROUTE_TYPE_DIRECT;
      saved[0].advert.resize(packet.getRawLength());
      packet.writeTo(saved[0].advert.data());
    }
    Fixture f;
    f.bot.setContactLookup(lookup);
    f.start();
    f.ingest(peer.command(f.bot.publicKey(), "!ping"));
    f.step();
    assert(peer.replies(f.bot.publicKey(), f.radio).empty());
    assert(status(f).find("Contacts=0/16 observed=0 routed=0") == 0);
    assert(status(f).find("recovered=0 rejected=1") != std::string::npos);
  }
  {
    Fixture f;
    f.start();
    f.ingest(peer.command(f.bot.publicKey(), "!ping"));
    f.step();
    assert(peer.replies(f.bot.publicKey(), f.radio).empty());
    assert(status(f).find("lookup=none") != std::string::npos);
  }
  saved.clear();
  puts("PASS native contact recovery: full-key signature checks, hash collisions, restart, no routes/RF/rebroadcast, bounded scans, full table and absent source");
}
