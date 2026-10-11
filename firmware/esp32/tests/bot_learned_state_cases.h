// SPDX-License-Identifier: Apache-2.0
static void native_learned_state() {
  const auto initialFiles = filesystem_test::files;
  const auto initialRecords = identity_test::durable;
  assert(saveBotEnabled(true) && saveBotRadioPolicy({}));
  const auto clock = [](uint32_t epoch) {
    beginClocks(); beginNetworkClock(true); receiveNetworkTime(epoch); loopClocks();
    uint32_t first, last; assert(trustedNetworkTime(first, last));
  };
  constexpr uint32_t epoch = 1791700000;
  clock(epoch);
  Peer peer;
  std::map<std::string, std::vector<uint8_t>> checkpoint;
  uint8_t owner[32]{};
  {
    Fixture f; f.start(); f.learn(peer);
    memcpy(owner, f.bot.publicKey(), 32);
    f.deliver(peer.pathReturn(owner, 2, {0x12, 0x34})); f.step();
    assert(contact_recovery_test::status(f).find("routed=1") != std::string::npos);
    assert(contact_recovery_test::status(f).find("dirty=1") != std::string::npos);
    timeMs += BotLearnedState::CheckpointMs; f.step(1);
    assert(contact_recovery_test::status(f).find("dirty=0") != std::string::npos);
    checkpoint = filesystem_test::files;
    f.step(100); assert(filesystem_test::files == checkpoint);
    f.bot.stop();
  }
  // Recreate the process with exactly the periodic checkpoint; no companion authority or new advert.
  filesystem_test::files = checkpoint; timeMs = 1000; clock(epoch + 350);
  {
    Fixture f; f.start();
    assert(!memcmp(owner, f.bot.publicKey(), 32));
    assert(contact_recovery_test::status(f).find("Contacts=1/16 observed=0 routed=1") == 0);
    assert(f.command(peer, "!ping") == "Pong");
    assert(contact_recovery_test::status(f).find("observed=0") != std::string::npos);
    f.bot.stop();
  }
  // An unknown boot clock cannot renew receipt age or expose a cached path.
  filesystem_test::files = checkpoint; timeMs = 1000; beginClocks();
  {
    Fixture f; f.start();
    assert(contact_recovery_test::status(f).find("Contacts=0/16") == 0);
    f.deliver(peer.command(owner, "!ping")); f.step();
    assert(peer.replies(owner, f.radio).empty());
    clock(epoch + 400); f.step();
    assert(contact_recovery_test::status(f).find("Contacts=1/16 observed=0 routed=1") == 0);
    assert(f.command(peer, "!ping") == "Pong");
    f.bot.stop();
  }
  const auto restart = [&](uint32_t utc, const char *expected) {
    filesystem_test::files = checkpoint; timeMs = 1000; clock(utc);
    Fixture f; f.start();
    assert(contact_recovery_test::status(f).find(expected) == 0);
    f.bot.stop();
  };
  restart(epoch - 100, "Contacts=0/16");
  restart(epoch + 1000, "Contacts=1/16 observed=0 routed=1");
  restart(epoch + BotLearnedState::ContactSeconds + 1000, "Contacts=0/16");
  filesystem_test::files = checkpoint; timeMs = 1000; clock(epoch + 1000);
  {
    Fixture f; f.start(); Peer caller; f.learn(caller);
    BotMeshPolicy policy; assert(loadBotMeshPolicy(policy));
    memcpy(policy.destinations[0], peer.self_id.pub_key, 32); assert(f.bot.setMeshPolicy(policy));
    char key[65]; for (unsigned i = 0; i < 32; ++i) snprintf(key + 2 * i, 3, "%02x", peer.self_id.pub_key[i]);
    const auto source = std::string("function send_cached() return mesh.send(mesh.compose{to='") + key + "',text='stale'}) end";
    BotWorker::Result result;
    assert(f.bot.stageSource(source.data(), source.size())); f.step();
    assert(f.bot.pollSourceResult(result) && result.ok && f.bot.activateStaged()); f.step();
    assert(f.bot.pollSourceResult(result) && result.ok);
    assert(f.command(caller, "!send_cached").find("fresh authenticated direct route") != std::string::npos);
    assert(peer.replies(f.bot.publicKey(), f.radio).empty());
    f.bot.stop();
    memset(policy.destinations[0], 0, 32); assert(saveBotMeshPolicy(policy));
  }

  // A new PHY retains the public signed identity, but discards the old bot's route.
  for (auto &entry : checkpoint) if (entry.first.find("/command-bot/learned-") == 0) {
    auto *snapshot = reinterpret_cast<BotLearnedState::Snapshot *>(entry.second.data());
    snapshot->frequency += 1000000;
    mesh::Utils::sha256(snapshot->digest, 32, entry.second.data(), offsetof(BotLearnedState::Snapshot, digest));
  }
  restart(epoch + 350, "Contacts=1/16 observed=0 routed=0");
  // Rehashing an invalid signature must not make it a contact.
  for (auto &entry : checkpoint) if (entry.first.find("/command-bot/learned-") == 0) {
    auto *snapshot = reinterpret_cast<BotLearnedState::Snapshot *>(entry.second.data());
    snapshot->entries[0].advert[snapshot->entries[0].advertSize - 1] ^= 1;
    mesh::Utils::sha256(snapshot->digest, 32, entry.second.data(), offsetof(BotLearnedState::Snapshot, digest));
  }
  restart(epoch + 350, "Contacts=0/16");
  filesystem_test::files = initialFiles; identity_test::durable = initialRecords; beginClocks();
  puts("PASS learned contacts: 300s checkpoint, clean write suppression, encrypted DM after restart without companion/new advert, trusted UTC quarantine/rollback/expiry, PHY route fencing and signature revalidation");
}
static void learned_state_storage() {
  const auto files = filesystem_test::files;
  filesystem_test::files.clear();
  uint8_t owner[32]{1}; BotLearnedState cache;
  cache.load(owner); cache.changed(1000);
  assert(cache.save(1000 + BotLearnedState::CheckpointMs - 1, false) && cache.dirty);
  assert(filesystem_test::files.empty());
  assert(cache.save(1000 + BotLearnedState::CheckpointMs, false) && !cache.dirty);
  const auto first = filesystem_test::files;
  cache.data.entries[0].acceptedUtc = 10; cache.changed(400000);
  filesystem_test::writeLimit = 0;
  assert(!cache.save(400000, true) && cache.dirty && cache.failed);
  assert(filesystem_test::files.at("/command-bot/learned-0") == first.at("/command-bot/learned-0"));
  filesystem_test::writeLimit = std::numeric_limits<size_t>::max();
  assert(cache.save(400001, false) && cache.dirty); // bounded retry, no tight write loop
  assert(cache.save(700000, false) && !cache.dirty);
  assert(cache.writes == 2 && filesystem_test::files.count("/command-bot/learned-1"));
  filesystem_test::files["/command-bot/learned-1"].resize(7);
  BotLearnedState restored; restored.load(owner);
  assert(restored.rejected == 1 && restored.data.sequence == 1);
  owner[0] = 2; restored.load(owner);
  assert(restored.rejected == 2 && restored.data.sequence == 0);
  filesystem_test::files = files;
  printf("PASS learned snapshot: %zu bytes, two-slot interrupted-write recovery, failed-write retry and identity binding\n", sizeof(BotLearnedState::Snapshot));
}
static void native_find_path_queue() {
  const auto files = filesystem_test::files;
  const auto records = identity_test::durable;
  assert(saveBotEnabled(true) && saveBotRadioPolicy({}) && saveBotRepeaterPolicy({}));
  beginClocks(); beginNetworkClock(true); receiveNetworkTime(1791700000); loopClocks();
  {
    Fixture f; f.start(); Peer first, second, third, target;
    f.learn(first); f.learn(second); f.learn(third); f.learn(target);
    char command[96] = "!find-path ";
    for (unsigned i = 0; i < 32; ++i) snprintf(command + 11 + 2 * i, 3, "%02x", target.self_id.pub_key[i]);
    const auto probes = [&]() {
      unsigned count = 0;
      for (const auto &raw : f.radio.sent) {
        mesh::Packet packet{}; assert(packet.readFrom(raw.data(), raw.size()));
        count += packet.getPayloadType() == PAYLOAD_TYPE_ANON_REQ;
      }
      return count;
    };
    const auto ask = [&](Peer &caller) { f.deliver(caller.command(f.bot.publicKey(), command)); f.step(300); };
    ask(first); assert(probes() == 1 && f.bot.jobsInUse());
    timeMs += 10000; f.step(300); assert(!f.bot.jobsInUse());
    ask(second); assert(probes() == 1 && f.bot.jobsInUse());
    ask(third);
    const auto rejected = third.replies(f.bot.publicKey(), f.radio);
    assert(rejected.size() == 1 && rejected[0].find("already waiting") != std::string::npos);
    // The actual Lua command remains suspended past its original 10s response budget.
    timeMs += 11000; f.step(300); assert(probes() == 1 && f.bot.jobsInUse());
    timeMs += 10000; f.step(300); assert(probes() == 2 && f.bot.jobsInUse());
    timeMs += 8000; f.step(300); assert(f.bot.jobsInUse());
    timeMs += 3000; f.step(300); assert(!f.bot.jobsInUse());
    const auto response = second.replies(f.bot.publicKey(), f.radio);
    assert(response.size() == 1 && response[0].find("no fresh path (10s)") != std::string::npos);
    // Revoke the permission while a new command waits for the shared spacing.
    timeMs += 61000; ask(first); timeMs += 10000; f.step(300);
    ask(second); const auto admitted = probes();
    char reply[192]{};
    f.bot.radioPolicyCommand("access dm action_trace 2", reply, sizeof(reply)); f.step(300);
    assert(!f.bot.jobsInUse() && probes() == admitted);
    f.bot.radioPolicyCommand("access dm action_trace inherit", reply, sizeof(reply));
    timeMs += 61000; ask(first); timeMs += 10000; f.step(300); ask(second);
    assert(f.bot.jobsInUse()); const auto beforeCancel = probes();
    f.bot.cancelJobs(); f.step(300); timeMs += 31000; f.step(300);
    assert(!f.bot.jobsInUse() && probes() == beforeCancel);
    f.bot.stop();
  }
  filesystem_test::files = files; identity_test::durable = records; beginClocks();
  puts("PASS find-path VM queue: one 30s admission wait then full10s response, second waiter rejected, permission revocation and cancellation emit no probe");
}
