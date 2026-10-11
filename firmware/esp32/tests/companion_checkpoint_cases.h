// SPDX-License-Identifier: Apache-2.0
static void companion_checkpoint() {
  const auto records = identity_test::durable;
  const auto files = filesystem_test::files;
  assert(saveRoleProfile({7}) && saveBotEnabled(true) && saveAutomaticAdverts(false));
  beginClocks(RoleProfile(7));
  Peer caller; caller.rtc.setCurrentTime(1791700000);
  Bytes lastAdvert;
  std::map<std::string, std::vector<uint8_t>> committed;
  {
    BetaFixture f(true);
    const auto before = filesystem_test::files;
    lastAdvert = caller.advert();
    f.mux.received(lastAdvert.data(), lastAdvert.size(), -90, 5); f.step();
    assert(filesystem_test::files == before); // No signed-advert file or metadata write on reception.
    for (unsigned update = 1; update < 4; ++update) {
      timeMs += 60000; caller.rtc.setCurrentTime(1791700000 + update * 60);
      lastAdvert = caller.advert();
      f.mux.received(lastAdvert.data(), lastAdvert.size(), -90, 5); f.step();
      assert(filesystem_test::files == before);
      unsigned cursor = 0; uint8_t key[32]{}, raw[255]{}, size = 0;
      assert(companionContactAdvert(caller.self_id.pub_key, cursor, key, raw, size));
      assert(Bytes(raw, raw + size) == lastAdvert); // Export reads the pending latest version.
    }
    timeMs += 120000; f.step(1); // First dirty deadline, despite the repeated updates.
    assert(filesystem_test::files != before);
    committed = filesystem_test::files;
    f.step(100); assert(filesystem_test::files == committed);
    // A failed periodic write preserves the previous files and waits another300s.
    caller.rtc.setCurrentTime(1791700400); lastAdvert = caller.advert();
    f.mux.received(lastAdvert.data(), lastAdvert.size(), -90, 5); f.step();
    filesystem_test::writeLimit = 0; timeMs += 300000; f.step(1);
    const auto failed = filesystem_test::files;
    filesystem_test::writeLimit = std::numeric_limits<size_t>::max();
    f.step(100); assert(filesystem_test::files == failed);
    assert(companionFlush()); // Explicit force persists the pending latest blob+metadata.
    committed = filesystem_test::files;
    for (const auto &entry : committed)
      assert(entry.first.rfind("/companion/bl/", 0) != 0 || entry.first.find("-prev") == std::string::npos);
  }
  {
    BetaFixture restarted(true);
    unsigned cursor = 0; uint8_t key[32]{}, raw[255]{}, size = 0;
    assert(companionContactAdvert(caller.self_id.pub_key, cursor, key, raw, size));
    assert(Bytes(raw, raw + size) == lastAdvert);
    restarted.bot.setContactLookup(companionContactAdvert);
    const auto request = caller.command(restarted.bot.publicKey(), "!ping");
    restarted.mux.received(request.data(), request.size(), -90, 5); restarted.step(300);
    assert(caller.replies(restarted.bot.publicKey(), restarted.radio) == std::vector<std::string>{"Pong"});
  }
  // A truncated primary metadata file cannot hide the previous complete record set.
  filesystem_test::files = committed;
  for (auto &entry : filesystem_test::files)
    if (entry.first == "/companion/contacts3") entry.second.resize(3);
  {
    BetaFixture restarted(true);
    unsigned cursor = 0; uint8_t key[32]{}, raw[255]{}, size = 0;
    assert(companionContactAdvert(caller.self_id.pub_key, cursor, key, raw, size));
    mesh::Packet packet{}; assert(packet.readFrom(raw, size));
    assert(!memcmp(packet.payload, caller.self_id.pub_key, 32));
    restarted.bot.setContactLookup(companionContactAdvert);
    const auto request = caller.command(restarted.bot.publicKey(), "!ping");
    restarted.mux.received(request.data(), request.size(), -90, 5); restarted.step(300);
    assert(caller.replies(restarted.bot.publicKey(), restarted.radio) == std::vector<std::string>{"Pong"});
  }
  // A rolled-back application can legitimately clear legacy metadata; never resurrect its previous contacts.
  filesystem_test::files = committed;
  filesystem_test::files["/companion/contacts3"].clear();
  {
    BetaFixture restarted(true);
    unsigned cursor = 0; uint8_t key[32]{}, raw[255]{}, size = 0;
    assert(!companionContactAdvert(caller.self_id.pub_key, cursor, key, raw, size));
  }
  identity_test::durable = records; filesystem_test::files = files;
  assert(reloadAutomaticAdverts());
  puts("PASS companion checkpoint: latest pending signed export without per-advert writes, first-dirty300s bounded checkpoint, idle suppression, failed-write retry, forced flush, full restart DM and corrupt primary fallback");
}
