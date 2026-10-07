// SPDX-License-Identifier: Apache-2.0
static void stock_admin_routes() {
  struct RequestPeer : NativeChat {
    using NativeChat::NativeChat;
    uint32_t pending = 0;
    Bytes response;
    void onContactResponse(const ContactInfo &contact, const uint8_t *data, uint8_t size) override {
      if (pending && size >= 4 && queued_tx::get32(data) == pending)
        response.assign(data, data + size);
      else NativeChat::onContactResponse(contact, data, size);
    }
  };
  const auto baseline = identity_test::durable;
  const auto files = filesystem_test::files;
  for (bool scoped : {false, true}) for (uint8_t width : {1, 2, 3}) {
    identity_test::durable = baseline;
    filesystem_test::files = files;
    BetaFixture f;
    RequestPeer peer(width, scoped);
    NativeRelay relay(scoped);
    ContactInfo contact{};
    contact.id = mesh::Identity(f.management.publicKey());
    contact.type = ADV_TYPE_REPEATER;
    contact.out_path_len = OUT_PATH_UNKNOWN;
    assert(peer.addContact(contact));
    auto *mast = peer.lookupContactByPubKey(contact.id.pub_key, 32);
    assert(mast);
    size_t mastSent = f.radio.sent.size(), peerSent = 0, relaySent = 0;
    const auto run = [&]() {
      for (unsigned i = 0; i < 250; ++i) {
        f.step(1); peer.loop(); relay.loop();
        while (mastSent < f.radio.sent.size()) relay.radio.input.push_back(f.radio.sent[mastSent++]);
        while (peerSent < peer.radio.sent.size()) relay.radio.input.push_back(peer.radio.sent[peerSent++]);
        while (relaySent < relay.radio.sent.size()) {
          peer.radio.input.push_back(relay.radio.sent[relaySent]);
          f.mux.received(relay.radio.sent[relaySent].data(), relay.radio.sent[relaySent].size(), -93, 4);
          ++relaySent;
        }
      }
    };
    uint32_t timeout;
    assert(peer.sendRequest(*mast, uint8_t(1), peer.pending, timeout) == MSG_SEND_SENT_FLOOD);
    run();
    assert(peer.response.empty());
    assert(f.action(("trust " + encode(peer.self_id.pub_key, 32)).c_str()).find("Saved") == 0);
    assert(peer.sendLogin(*mast, "", timeout) == MSG_SEND_SENT_FLOOD);
    run();
    assert(peer.logins == 1 && peer.paths);
    assert(mast->out_path_len == uint8_t((width - 1) << 6 | 1));
    assert(!memcmp(mast->out_path, relay.self_id.pub_key, width));
    assert(peer.command(*mast, "status", TXT_TYPE_CLI_DATA) == MSG_SEND_SENT_DIRECT);
    run();
    assert(peer.commands.size() == 1 && peer.commands.back().find("roles applied=") == 0);
    bool direct = false;
    for (const auto &wire : f.radio.sent) {
      mesh::Packet packet;
      assert(packet.readFrom(wire.data(), wire.size()));
      if (packet.getPayloadType() == PAYLOAD_TYPE_TXT_MSG)
        direct |= packet.isRouteDirect() && packet.getPathHashSize() == width &&
                  packet.getPathHashCount() == 1 && !memcmp(packet.path, relay.self_id.pub_key, width);
    }
    assert(direct);
    for (bool flood : {false, true}) {
      for (uint8_t type : {1, 3}) {
        if (flood) mast->out_path_len = OUT_PATH_UNKNOWN;
        peer.response.clear();
        assert(peer.sendRequest(*mast, type, peer.pending, timeout) ==
               (flood ? MSG_SEND_SENT_FLOOD : MSG_SEND_SENT_DIRECT));
        run();
        assert(peer.response.size() >= 8 && queued_tx::get32(peer.response.data()) == peer.pending);
        if (type == 1) {
          const unsigned prefix = flood ? width + 2 : 0;
          assert(peer.response.size() == ((60 + prefix + 15) & ~15u) - prefix);
          assert(queued_tx::get16(peer.response.data() + 4) == 4000);
          for (size_t i = 60; i < peer.response.size(); ++i) assert(peer.response[i] == 0);
          assert(queued_tx::get16(peer.response.data() + 10) == uint16_t(-93));
          assert(queued_tx::get32(peer.response.data() + 12) > 0);
          assert(queued_tx::get16(peer.response.data() + 46) == 16);
        } else {
          assert(peer.response[4] == TELEM_CHANNEL_SELF && peer.response[5] == LPP_VOLTAGE);
          assert(peer.response[6] == 0x01 && peer.response[7] == 0x90);
        }
      }
    }
    const auto forget = "auth forget " + encode(peer.self_id.pub_key, 32);
    assert(f.action(forget.c_str()).find("Forgot") == 0);
    peer.response.clear();
    assert(peer.sendRequest(*mast, uint8_t(1), peer.pending, timeout) == MSG_SEND_SENT_DIRECT);
    run();
    assert(peer.response.empty());
  }
  identity_test::durable = baseline;
  filesystem_test::files = files;
  puts("PASS stock BaseChat admin: trusted blank login, scoped/unscoped relay, 1/2/3-byte CLI/status/telemetry, unknown/forgotten request denial");
}
static void web_expiry_and_host() {
  BetaFixture f;
  TestHTTPServer server;
  assert(registerMastWeb(&server) == ESP_OK);
  const auto request = [&](const char *path, const std::string &body = "",
                           const std::string &token = "", const std::string &host = "mast.test") {
    httpd_req_t r;
    r.body = body; r.content_len = body.size();
    r.headers["Host"] = host; r.headers["Origin"] = "http://" + host;
    if (!token.empty()) r.headers["X-Mast-Session"] = token;
    assert(server.routes.at(path).handler(&r) == ESP_OK);
    return r;
  };
  assert(request("/admin", "", "", "attacker.test").status == "403 Forbidden");
  assert(request("/admin/login", "mast-pass-12", "", "attacker.test").status == "403 Forbidden");
  assert(request("/admin", "", "", "mast.test:80").status == "200 OK");
  assert(request("/admin", "", "", "mast.test:81").status == "403 Forbidden");
  for (uint32_t start : {0x7fffff00u, 0xfffff000u}) {
    timeMs = start;
    serviceMastWeb(f.management.admin());
    for (unsigned i = 0; i < 20; ++i)
      assert(request("/admin/login", "not-the-password").status == "403 Forbidden");
    const auto login = request("/admin/login", "mast-pass-12");
    assert(login.status == "200 OK" && login.response.size() == 32);
    const auto token = login.response;
    timeMs = uint32_t(start + 599999);
    assert(request("/admin/command", std::string(163, 'x'), token).status == "400 Bad Request");
    timeMs = uint32_t(start + 600000);
    serviceMastWeb(f.management.admin());
    assert(request("/admin/command", "status", token).status == "403 Forbidden");
    timeMs = uint32_t(start + 0x80001000u);
    serviceMastWeb(f.management.admin());
    assert(request("/admin/command", "status", token).status == "403 Forbidden");
    const auto fresh = request("/admin/login", "mast-pass-12");
    assert(fresh.status == "200 OK" && fresh.response != token);
    assert(request("/admin/logout", "", fresh.response).status == "200 OK");
    assert(request("/admin/command", "status", std::string(32, '0')).status == "403 Forbidden");
  }
  puts("PASS web expiry: empty/expired slots, 24.8-day and millis wrap; bad-password isolation; Host/Origin allowlist");
}
static void replay_capacity_and_cancellation() {
  const auto baseline = identity_test::durable;
  const auto files = filesystem_test::files;
  struct FixtureSeed : mesh::RNG {
    void random(uint8_t *bytes, size_t size) override {
      for (size_t i = 0; i < size; ++i) bytes[i] = uint8_t(i);
    }
  } seed;
  const mesh::LocalIdentity compiledOwner(&seed);
  {
    BetaFixture f;
    Peer peers[MastAdmin::SessionSlots];
    constexpr unsigned last = MastAdmin::SessionSlots - 1;
    for (unsigned i = 0; i < last; ++i)
      assert(!f.send(peers[i], "mast-pass-12", true, true, 1, false).empty());
    assert(f.send(peers[last], "mast-pass-12", true).empty());
    assert(f.management.admin().compiledTrusted(compiledOwner.pub_key));
    Peer owner;
    owner.self_id = compiledOwner;
    assert(!f.send(owner, "", true).empty());
    assert(!f.send(owner, "status", false).empty());
    assert(f.management.admin().rememberTimestamp(compiledOwner.pub_key, 1800000010));
    assert(!f.management.admin().rememberTimestamp(compiledOwner.pub_key, 1800000010));
    const auto forget = "auth forget " + encode(peers[0].self_id.pub_key, 32);
    assert(f.action(forget.c_str()).find("Forgot") == 0);
    assert(f.send(peers[0], "status", false).empty());
    assert(!f.send(peers[last], "mast-pass-12", true).empty());
    assert(f.action(("auth forget " + encode(compiledOwner.pub_key, 32)).c_str()).find("Error:") == 0);
    f.radio.rejectTx = true;
    assert(f.send(peers[last], "a2|radio 912525000 250000 8 5 2", false).empty());
    f.step();
    f.radio.rejectTx = false;
    --peers[last].timestamp;
    const auto repeated = f.send(peers[last], "a2|radio 912525000 250000 8 5 2", false);
    assert(repeated.size() > 8 && !memcmp(repeated.data() + 5, "a2|Error:", 9));
    assert(f.mux.currentConfiguration().sf == 7);
    MastAdmin::Reply reply;
    f.management.admin().execute("tempradio 2 912525000 250000 8 5 2", reply);
    assert(reply.ticket);
    timeMs += 30001; f.step();
    assert(f.management.admin().cancelled(reply.ticket));
    assert(f.mux.currentConfiguration().sf == 7);
  }
  {
    BetaFixture f;
    assert(f.management.admin().lastTimestamp(compiledOwner.pub_key) == 1800000010);
  }
  identity_test::durable = baseline;
  filesystem_test::files = files;
  puts("PASS replay: six ordinary sessions preserve compiled owner across reboot, authenticated forget, failed TX/retry cache and timeout cancellation");
}
static void invalid_rf_logins_do_not_throttle_owner() {
  const auto baseline = identity_test::durable;
  const auto files = filesystem_test::files;
  {
    BetaFixture f;
    Peer invalid, owner;
    const auto start = timeMs.load();
    for (unsigned i = 0; i < 4; ++i)
      assert(f.send(invalid, "incorrect", true, true, 1, true, 4).empty());
    assert(!f.send(owner, "mast-pass-12", true, true, 1, false, 20).empty());
    assert(timeMs - start < 1000);
  }
  identity_test::durable = baseline;
  filesystem_test::files = files;
  puts("PASS invalid encrypted RF login burst cannot consume owner authentication gate");
}
static void corrupt_replay_keeps_services() {
  const auto baseline = identity_test::durable;
  const auto files = filesystem_test::files;
  for (const auto &record : {Bytes{1}, Bytes(149, 0), Bytes(148, 0)}) {
    identity_test::durable = baseline;
    filesystem_test::files = files;
    identity_test::durable[{"mc-mast-admin", "replay"}] = record;
    BetaFixture f;
    assert(!f.management.admin().ready());
    assert(f.management.publicKey() && f.bot.sourceReady());
    RadioDashboard::RoleStatus status;
    f.management.dashboardStatus(status);
    assert(!strcmp(status.state, "admin-fault"));
    assert(f.action("status").find("Error:") == 0);
    Peer peer;
    assert(f.send(peer, "mast-pass-12", true).empty());
    auto advert = peer.advert();
    f.mux.received(advert.data(), advert.size(), -93, 4); f.step();
    const auto ping = peer.command(f.bot.publicKey(), "!ping");
    f.radio.sent.clear();
    f.mux.received(ping.data(), ping.size(), -93, 4); f.step();
    assert(peer.replies(f.bot.publicKey(), f.radio) == std::vector<std::string>{"Pong"});
    LocalRadio ordinary;
    assert(ordinary.attach(f.mux) && ordinary.queuedReady());
    uint32_t job;
    const uint8_t frame[] = {uint8_t(PAYLOAD_TYPE_RAW_CUSTOM << 2 | ROUTE_TYPE_DIRECT), 0, 1};
    assert(ordinary.queueTransmit(frame, sizeof(frame), 0, 0, 0, job));
    f.step();
    mesh::QueuedTransmitResult result;
    assert(ordinary.pollQueuedResult(result) && result.state == queued_tx::ACCEPTED);
    assert(ordinary.pollQueuedResult(result) && result.state == queued_tx::SUCCEEDED);
    ordinary.detach();
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    WiFiServer kiss;
    kiss.add(WiFiClient(sockets[0])); f.mux.poll(kiss);
    const uint8_t query[] = {0xc0, 6, queued_tx::HELLO, 1, 0, 0xc0,
                             0xc0, 6, queued_tx::CONFIG, 1, 0, 0xc0};
    assert(send(sockets[1], query, sizeof(query), 0) == sizeof(query));
    f.mux.poll(kiss); f.mux.poll(kiss);
    uint8_t response[256];
    const auto length = recv(sockets[1], response, sizeof(response), MSG_DONTWAIT);
    const uint8_t config[] = {6, 0xa2, 1, 0};
    assert(length > 0 && std::search(response, response + length, config, config + sizeof(config)) != response + length);
    close(sockets[1]); f.mux.poll(kiss);
    TestHTTPServer server;
    assert(registerMastWeb(&server) == ESP_OK);
    httpd_req_t login;
    login.headers["Host"] = "mast.test"; login.body = "mast-pass-12"; login.content_len = login.body.size();
    assert(server.routes.at("/admin/login").handler(&login) == ESP_OK);
    assert(login.status == "503 Service Unavailable");
  }
  identity_test::durable = baseline;
  filesystem_test::files = files;
  puts("PASS corrupt/re-sized replay: admin fails closed; bot, shared queued radio and identities survive");
}
static void temporary_restores_durable() {
  BetaFixture f;
  assert(f.mux.applyMastConfiguration({912525000, 250000, 9, 5, 2}, false));
  const auto generation = f.mux.configurationGeneration();
  MastAdmin::Reply reply;
  f.management.admin().execute("tempradio 2 912525000 250000 8 5 2", reply);
  assert(reply.ticket);
  f.management.admin().acknowledged(reply.ticket, true); f.step();
  assert(f.mux.currentConfiguration().sf == 8);
  const auto commits = identity_test::commits;
  identity_test::failCommit = true;
  f.step(250);
  identity_test::failCommit = false;
  assert(f.mux.currentConfiguration().sf == 7 && f.mux.configurationGeneration() == generation + 2);
  assert(identity_test::commits == commits);
  RadioDashboard::RadioStatus status;
  f.mux.dashboardStatus(status);
  assert(status.committed);
  puts("PASS temporary PHY restores durable SF7, not transient SF9, without NVS write; generation advances twice");
}
static void live_source_retry() {
  const auto baseline = identity_test::durable;
  const auto files = filesystem_test::files;
  {
    BetaFixture f;
    const std::string source = "function recovered() reply('recovered') end command('recovered','','Retry recovery')";
    uint8_t digest[32];
    mesh::Utils::sha256(digest, 32, reinterpret_cast<const uint8_t *>(source.data()), source.size());
    const auto hash = encode(digest, 32);
    upload(f, source, true, false);
    for (unsigned i = 0; i < 500 && f.action("source hash").find(hash) == std::string::npos; ++i)
      f.step(1);
    assert(f.action("source hash").find(hash) != std::string::npos);
    filesystem_test::readLimit = 0;
    f.step(1100);
    assert(f.action("source status").find("source retry") != std::string::npos);
    filesystem_test::readLimit = SIZE_MAX;
    RadioDashboard::RoleStatus status;
    f.bot.dashboardStatus(status);
    assert(status.ready);
    assert(!f.bot.sourceDeploymentReady());
    assert(!f.bot.bootReady());
    Peer peer;
    const auto advert = peer.advert();
    f.mux.received(advert.data(), advert.size(), -90, 4); f.step();
    const auto ask = [&](const char *command = "!ping") {
      timeMs += 61000;
      const auto packet = peer.command(f.bot.publicKey(), command);
      f.radio.sent.clear();
      f.mux.received(packet.data(), packet.size(), -90, 4); f.step();
      return peer.replies(f.bot.publicKey(), f.radio);
    };
    assert(ask() == std::vector<std::string>{"Pong"});
    assert(f.action("source retry").find("Accepted") == 0);
    f.step(300);
    assert(f.action("source status").find("durably saved and active") != std::string::npos);
    assert(f.bot.sourceDeploymentReady());
    assert(f.bot.bootReady());
    assert(ask() == std::vector<std::string>{"Pong"});
    assert(ask("!recovered") == std::vector<std::string>{"recovered"});
    assert(f.action("source retry").find("Accepted") == 0);
    filesystem_test::readLimit = 0;
    f.step(30);
    assert(f.action("source status").find("live retry pending") != std::string::npos);
    filesystem_test::readLimit = SIZE_MAX;
    f.step(300);
    assert(f.action("source status").find("durably saved and active") != std::string::npos);
  }
  identity_test::durable = baseline;
  filesystem_test::files = files;
  puts("PASS live source: injected post-commit read failure, bounded retries, prior public replies and manual recovery");
}
static void selected_source_boot_health() {
  const auto baseline = identity_test::durable;
  const auto baselineFiles = filesystem_test::files;
  std::string selectedLuaPath, selectedWasmPath;
  const auto slotPath = [](const std::string &status, const char *field, bool wasm) {
    const auto at = status.find(field);
    assert(at != std::string::npos);
    const char slot = status[at + strlen(field)];
    assert(slot >= '0' && slot <= '2');
    const char *files[] = {"a.lua", "b.lua", "c.lua"};
    return std::string("/command-bot/") + (wasm ? "w" : "") +
           files[slot - '0'];
  };
  const std::string lua =
      "function bootcustom() return 'selected Lua' end "
      "command('bootcustom','','Boot fixture') "
      "function patched() return 'Selected ping' end override_command('ping','patched')";
  {
    BetaFixture f;
    assert(f.bot.sourceDeploymentReady());
    upload(f, lua);
    const auto luaStatus = f.action("source status");
    selectedLuaPath = slotPath(luaStatus, "active=", false);
#if ONCHIP_BOT_WASM
    const char *path = getenv("BOT_WASM_CREDENTIAL_MODULE");
    assert(path);
    std::ifstream module(path, std::ios::binary);
    const std::string wasm{std::istreambuf_iterator<char>(module), std::istreambuf_iterator<char>()};
    assert(wasm.size() > 8 && wasm.size() <= BotSourceLimit);
    upload(f, wasm, true, true, [&](const char *text) {
      return f.action((std::string("source wasm ") + (text + 7)).c_str());
    });
    selectedWasmPath = slotPath(f.action("source wasm status"), "active=", true);
#endif
    assert(f.bot.sourceDeploymentReady());
  }
  const auto selected = identity_test::durable;
  const auto selectedFiles = filesystem_test::files;
  const auto check = [](BetaFixture &f, bool ready) {
    RadioDashboard::RoleStatus status;
    f.bot.dashboardStatus(status);
    BotNodeSnapshot snapshot;
    f.bot.nodeSnapshot(snapshot);
    assert(f.bot.sourceReady()); // Bundled VM initialization alone must not pass health.
    if (f.bot.sourceDeploymentReady() != ready)
      fprintf(stderr, "Selected source health: expected=%d state=%s fault=%s lua=%s wasm=%s\n",
              ready, status.state, status.fault, f.action("source status").c_str(),
              f.action("source wasm status").c_str());
    assert(f.bot.sourceDeploymentReady() == ready);
    assert(status.ready == ready && snapshot.ready == ready);
    assert(f.bot.bootReady() == ready);
    if (!ready) {
      assert(!strcmp(status.state, "source-recovery"));
      assert(status.fault[0] && snapshot.fault);
    } else {
      if (status.fault[0]) fprintf(stderr, "Recovered source health fault: %s\n", status.fault);
      assert(!status.fault[0] && !snapshot.fault);
    }
  };
  for (bool wasm : {false, true}) {
#if !ONCHIP_BOT_WASM
    if (wasm) continue;
#endif
    const std::string selector = wasm ? "source wasm " : "source ";
    const std::string journal = wasm ? "wasm-source" : "source";
    const std::string path = wasm ? selectedWasmPath : selectedLuaPath;
    assert(selectedFiles.count(path));
    // Missing/open/size/metadata-read/header/hash/journal failures all retain the selection.
    for (unsigned failure = 0; failure < 8; ++failure) {
      identity_test::durable = selected;
      filesystem_test::files = selectedFiles;
      if (failure == 0) filesystem_test::files.erase(path);
      if (failure == 1) filesystem_test::failOpen = true;
      if (failure == 2) filesystem_test::files[path].push_back(' ');
      if (failure == 3) filesystem_test::readLimit = 0;
      if (failure == 4) {
        auto &bytes = filesystem_test::files[path];
        const char bad[] = "--@meshcore-bot/9;";
        std::copy(bad, bad + sizeof(bad) - 1, bytes.begin());
      }
      if (failure == 5) filesystem_test::files[path].back() ^= 1;
      if (failure == 6) identity_test::durable[{"mc-mast-admin", journal}].resize(1);
      if (failure == 7) identity_test::durable[{"mc-mast-admin", journal}][0] = 2;
      const auto failedRecords = identity_test::durable;
      {
        printf("Selected-source boot fault runtime=%s case=%u\n", wasm ? "wasm" : "lua", failure);
        BetaFixture f;
        f.step(1200);
        check(f, false);
        const auto status = f.action((selector + "status").c_str());
        assert(status.find("Error:") != std::string::npos);
        assert(status.find("retry") != std::string::npos);
        if (failure == 3) assert(status.find("metadata read incomplete") != std::string::npos);
        assert(identity_test::durable == failedRecords);
        Peer peer;
        const auto advert = peer.advert();
        f.mux.received(advert.data(), advert.size(), -90, 4); f.step();
        const auto ping = peer.command(f.bot.publicKey(), "!ping");
        f.radio.sent.clear();
        f.mux.received(ping.data(), ping.size(), -90, 4); f.step();
        assert(peer.replies(f.bot.publicKey(), f.radio).empty());
        filesystem_test::failOpen = false;
        filesystem_test::readLimit = SIZE_MAX;
        filesystem_test::files[path] = selectedFiles.at(path);
        if (failure >= 6) identity_test::durable = selected;
        const auto retry = f.action((selector + "retry").c_str());
        assert(retry.find("Accepted") == 0);
        f.step(500);
#if ONCHIP_BOT_WASM
        if (failure == 1 || failure == 3) {
          assert(!f.bot.sourceDeploymentReady());
          const auto other = f.action(wasm ? "source retry" : "source wasm retry");
          assert(other.find("Accepted") == 0);
          f.step(500);
        }
#endif
        check(f, true);
        assert(f.action((selector + "status").c_str()).find("durably saved and active") != std::string::npos);
        assert(identity_test::durable == selected);
        timeMs += 61000;
        const auto recovered = peer.command(f.bot.publicKey(), "!ping");
        f.radio.sent.clear();
        f.mux.received(recovered.data(), recovered.size(), -90, 4); f.step();
        assert(peer.replies(f.bot.publicKey(), f.radio) == std::vector<std::string>{"Selected ping"});
      }
    }
    identity_test::durable = selected;
    filesystem_test::files = selectedFiles;
    filesystem_test::files.erase(path);
    {
      BetaFixture f;
      check(f, false);
      filesystem_test::files[path] = selectedFiles.at(path);
      f.step(500);
      check(f, true);
      assert(identity_test::durable == selected);
    }
    identity_test::durable = selected;
    filesystem_test::files = selectedFiles;
    filesystem_test::files.erase(path);
    {
      BetaFixture f;
      f.step(1200);
      check(f, false);
      assert(f.action((selector + "remove").c_str()).find("Accepted") == 0);
      f.step(500);
      check(f, true);
      assert(f.action((selector + "status").c_str()).find("active=3") != std::string::npos);
      assert(!filesystem_test::files.count(path));
    }
  }
  identity_test::durable = selected;
  filesystem_test::files = selectedFiles;
  {
    BetaFixture f;
    f.step(400);
    check(f, true);
    assert(identity_test::durable == selected);
    const auto hash = f.action("source hash");
    upload(f, "function broken(", true, false);
    f.step(500);
    assert(f.action("source status").find("Error:") != std::string::npos);
    assert(f.action("source hash") == hash);
    check(f, true);
    assert(f.action("source cancel").find("Upload cancelled") == 0);
    filesystem_test::files.erase(selectedLuaPath);
    assert(f.action("source rollback").find("Error:") == 0);
    check(f, true);
    filesystem_test::files[selectedLuaPath] = selectedFiles.at(selectedLuaPath);
    Peer peer;
    const auto advert = peer.advert();
    f.mux.received(advert.data(), advert.size(), -90, 4); f.step();
    const auto ping = peer.command(f.bot.publicKey(), "!ping");
    f.radio.sent.clear();
    f.mux.received(ping.data(), ping.size(), -90, 4); f.step();
    assert(peer.replies(f.bot.publicKey(), f.radio) == std::vector<std::string>{"Selected ping"});
    assert(identity_test::durable == selected);
  }
  identity_test::durable = selected;
  filesystem_test::files = selectedFiles;
  assert(saveBotEnabled(false));
  {
    const auto disabled = identity_test::durable;
    BetaFixture f;
    RadioDashboard::RoleStatus status;
    f.bot.dashboardStatus(status);
    assert(!strcmp(status.state, "disabled") && !f.bot.sourceDeploymentReady());
    assert(identity_test::durable == disabled);
    assert(f.action("source status").find("bot disabled") != std::string::npos);
  }
  identity_test::durable = baseline;
  filesystem_test::files = baselineFiles;
#if !ONCHIP_BOT_WASM
  // A Lua-only application cannot declare a retained Wasm selection healthy.
  for (const auto &record : {selected.at({"mc-mast-admin", "source"}), Bytes{1}}) {
    identity_test::durable = selected;
    filesystem_test::files = selectedFiles;
    identity_test::durable[{"mc-mast-admin", "wasm-source"}] = record;
    const auto retained = identity_test::durable;
    BetaFixture f;
    check(f, false);
    assert(f.action("source wasm status").find("restore Wasm-enabled firmware") != std::string::npos);
    assert(identity_test::durable == retained);
  }
  identity_test::durable = baseline;
  filesystem_test::files = baselineFiles;
#endif
  puts("PASS selected Lua/Wasm boot health: missing/open/size/metadata/header/hash/journal faults, bundled fallback fenced, retained journals, retry and valid restart");
}
static void runtime_fault_boot_health() {
  const auto baseline = identity_test::durable;
  const auto baselineFiles = filesystem_test::files;
  assert(saveBotEnabled(true) && saveBotEventAccess(16));
  {
    BetaFixture f;
    assert(f.action("bot repeaters off").find("Saved") == 0);
    upload(f, "function fleet_poll() local peer=repeater.next() "
              "if peer then repeater.status(peer) end end events.every(15,'fleet_poll')");
    const auto source = f.action("source hash");
    f.step(2000);
    RadioDashboard::RoleStatus status;
    f.bot.dashboardStatus(status);
    assert(f.bot.counters().eventsFailed > 0);
    assert(strstr(status.fault, "Repeater polling"));
    assert(status.ready && f.bot.sourceDeploymentReady() && f.bot.bootReady());
    assert(f.action("source hash") == source);
    BotNodeSnapshot snapshot;
    f.bot.nodeSnapshot(snapshot);
    assert(snapshot.ready && snapshot.fault);
    identity_test::durable[{"mc-onchip", "bot-adaptive"}] = {'B', 'A', 'D', 1, 0};
    assert(f.action("bot adaptive").find("policy-fault=1") != std::string::npos);
    assert(!f.bot.bootReady());
    assert(f.action("bot adaptive off") == "Saved adaptive admission; reboot required");
    assert(f.bot.bootReady());
    f.bot.setSourceDeploymentState(false, true, true, false, "Selected Lua source unavailable");
    assert(!f.bot.bootReady());
    f.bot.setSourceDeploymentState(true, false, false, true, "Selected Wasm source unavailable");
    assert(!f.bot.bootReady());
    f.bot.setSourceDeploymentState(true, false, true, false, nullptr);
    assert(f.bot.bootReady());
  }
  assert(saveBotEnabled(false));
  {
    BetaFixture f;
    assert(!f.bot.bootReady());
  }
  identity_test::durable = baseline;
  filesystem_test::files = baselineFiles;
  assert(saveBotEnabled(false));
  {
    BetaFixture f;
    assert(f.bot.bootReady());
  }
  identity_test::durable = baseline;
  filesystem_test::files = baselineFiles;
  puts("PASS boot health: script errors remain visible without blocking a ready VM; source and policy faults still block");
}
static void beta_review_regressions() {
  stock_admin_routes();
  web_expiry_and_host();
  replay_capacity_and_cancellation();
  invalid_rf_logins_do_not_throttle_owner();
  corrupt_replay_keeps_services();
  temporary_restores_durable();
  live_source_retry();
  selected_source_boot_health();
}
