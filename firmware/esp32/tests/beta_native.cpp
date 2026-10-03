// SPDX-License-Identifier: Apache-2.0
#define main previous_bot_runtime_main
#include "bot_native.cpp"
#undef main
#include "Management.h"
#include "MastWeb.h"
#include "ServiceName.h"
#include "FirmwareIdentity.h"
#include "bot_parser_cases.h"
#include <functional>
#include <limits>

struct DashboardStreamTest {
  static void maximumCounters(RadioDashboard &dashboard) {
    auto &s = dashboard._published;
    s.publication = 1;
    s.uptime_ms = s.rx_packets = s.tx_succeeded = s.tx_failed = s.tx_unknown =
        s.tx_rf_ms = s.rx_estimated_ms = s.tx_accepted = s.tx_rejected = UINT64_MAX;
    s.radio.rx_errors = UINT32_MAX;
    s.radio.queued = UINT8_MAX;
  }
};

struct BetaFixture {
  Radio radio;
  RadioDashboard dashboard;
  HardwareRNG rng;
  WifiKissMultiplexer mux;
  Management management;
  CommandBot &bot = commandBotService();
  bool rolesActive = false;
  BetaFixture(bool nativeRoles = false) {
    mux.observeWith(dashboard);
    mux.attachRadio(radio, rng, [](float, float, uint8_t, uint8_t) {}, [](uint8_t) {});
    assert(mux.setInitialConfiguration({912525000, 250000, 7, 5, 2}, true));
    assert(management.begin(mux, ""));
    assert(bot.begin(mux));
    if (nativeRoles) {
      const RoleCallbacks callbacks[] = {
        {repeaterBegin, repeaterFlush, repeaterStop, repeaterEraseStep, repeaterLoop, nullptr},
        {roomBegin, roomFlush, roomStop, roomEraseStep, roomLoop, nullptr},
        {companionBegin, companionFlush, companionStop, companionEraseStep, companionLoop, companionRequestFailed}
      };
      assert(companionSessions().begin());
      beginLifecycles(mux, callbacks, RoleProfile(7));
      rolesActive = true;
    }
    step();
  }
  ~BetaFixture() {
    if (rolesActive) {
      repeaterStop(); roomStop(); companionStop(); companionSessions().end();
    }
    management.stop(); bot.stop();
    assert(psram_test::allocations.empty());
  }
  void step(unsigned count = 100) {
    while (count--) {
      timeMs += 10;
      bot.loop(); management.loop(); mux.serviceTransmit();
      RadioDashboard::RadioStatus status;
      mux.dashboardStatus(status);
      dashboard.publish(timeMs, status);
      if (rolesActive) loopLifecycles();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  std::string action(const char *command) {
    MastAdmin::Reply reply;
    management.admin().execute(command, reply);
    return reply.text;
  }
  Bytes send(Peer &peer, const char *text, bool login, bool flood = true, uint8_t width = 1,
             bool terminate = true, unsigned ticks = 100, uint8_t requestType = 0,
             uint8_t anonymousType = 0) {
    uint8_t secret[32];
    peer.self_id.calcSharedSecret(secret, management.publicKey());
    Bytes data(login ? 4 : 5);
    queued_tx::put32(data.data(), ++peer.timestamp);
    if (!login) data[4] = anonymousType ? anonymousType : requestType ? requestType : 4;
    data.insert(data.end(), text, text + strlen(text) + (terminate ? 1 : 0));
    auto *packet = login || anonymousType
        ? peer.createAnonDatagram(PAYLOAD_TYPE_ANON_REQ, peer.self_id,
                                  mesh::Identity(management.publicKey()), secret, data.data(), data.size())
        : peer.createDatagram(requestType ? PAYLOAD_TYPE_REQ : PAYLOAD_TYPE_TXT_MSG, mesh::Identity(management.publicKey()),
                               secret, data.data(), data.size());
    assert(packet);
    packet->header |= flood ? ROUTE_TYPE_FLOOD : ROUTE_TYPE_DIRECT;
    const Bytes path = width == 1 ? Bytes{0xa1, 0xb2} :
                       width == 2 ? Bytes{0xa1, 0xa2, 0xb1, 0xb2} :
                                    Bytes{0xa1, 0xa2, 0xa3, 0xb1, 0xb2, 0xb3};
    packet->setPathHashSizeAndCount(width, flood ? 2 : 0);
    if (flood) memcpy(packet->path, path.data(), path.size());
    assert(packet->getPayloadVer() == PAYLOAD_VER_1 && !packet->hasTransportCodes() &&
           packet->payload_len <= 184);
    const auto wire = peer.wire(packet);
    radio.sent.clear();
    mux.received(wire.data(), wire.size(), -90, 5);
    step(ticks);
    for (const auto &raw : radio.sent) {
      mesh::Packet p;
      assert(p.readFrom(raw.data(), raw.size()));
      if ((p.getPayloadType() != PAYLOAD_TYPE_PATH &&
           p.getPayloadType() != PAYLOAD_TYPE_RESPONSE &&
           p.getPayloadType() != PAYLOAD_TYPE_TXT_MSG) ||
          p.payload[0] != peer.self_id.pub_key[0] ||
          p.payload[1] != management.publicKey()[0]) continue;
      uint8_t plain[184]{};
      const auto size = mesh::Utils::MACThenDecrypt(secret, plain, p.payload + 2, p.payload_len - 2);
      assert(size > 0);
      assert(p.getPayloadVer() == PAYLOAD_VER_1 && p.payload_len <= 184);
      assert(p.hasTransportCodes() == (!anonymousType && !flood && ONCHIP_SERVICE_REGION[0]));
      if (p.hasTransportCodes()) {
        TransportKey region;
        TransportKeyStore keys;
        keys.getAutoKeyFor(1, ONCHIP_SERVICE_REGION, region);
        const auto code = region.calcTransportCode(&p);
        assert(p.transport_codes[0] == code && p.transport_codes[1] == code);
      }
      if (p.getPayloadType() == PAYLOAD_TYPE_PATH) {
        assert(plain[0] == ((width - 1) << 6 | 2) &&
               !memcmp(plain + 1, path.data(), path.size()) &&
               plain[1 + path.size()] == PAYLOAD_TYPE_RESPONSE);
        return Bytes(plain + 2 + path.size(), plain + size);
      }
      if (anonymousType) {
        assert(p.isRouteDirect() && p.path_len == data[5] &&
               !memcmp(p.path, data.data() + 6, p.getPathByteLen()));
      } else assert(p.isRouteFlood() && p.getPathHashCount() == 0);
      return Bytes(plain, plain + size);
    }
    return {};
  }
};
static std::string encode(const uint8_t *bytes, size_t size);
static void management_cli_compatibility() {
  const auto durable = identity_test::durable;
  const auto files = filesystem_test::files;
  {
    BetaFixture f;
    Peer peer, outsider;
    assert(!f.send(peer, "mast-pass-12", true).empty());
    const auto rf = [&](const char *command) {
      const auto response = f.send(peer, command, false);
      assert(response.size() > 5 && response[4] == 4);
      return std::string(response.begin() + 5, std::find(response.begin() + 5, response.end(), 0));
    };
    for (const char *topic : {"wifi", "radio", "tempradio", "role", "roles", "key",
                             "source", "bot", "auth", "trust", "data", "room", "companion", "get", "set"}) {
      const auto before = identity_test::durable;
      const auto bare = rf(topic);
      assert(bare.find("Error: usage: ") == 0);
      assert(bare.size() <= MastAdmin::TextLimit - 17);
      const auto help = rf((std::string("0123456789abcdef|help ") + topic).c_str());
      assert(help.find("0123456789abcdef|") == 0 && help.find("needs an untagged") == std::string::npos);
      assert(identity_test::durable.size() == before.size());
      for (const auto &record : before)
        if (record.first != std::make_pair(std::string("mc-mast-admin"), std::string("replay")))
          assert(identity_test::durable.at(record.first) == record.second);
    }
    assert(rf("wifi help") == rf("help wifi"));
    assert(rf("stats help") == rf("help stats"));
    assert(rf("stats") == rf("get stats"));
    assert(rf("stats radio").find("schema=1 scope=modem rx_packets=") == 0);
    assert(rf("stats reset") == "Error: unknown stats topic; use stats help");
    assert(rf("stats sensors") == "schema=1 scope=device battery_mv=4000 mcu_temp_c=unavailable");
    const auto previousTemperature = board.mcuTemperature;
    board.mcuTemperature = -12.5f;
    board.batteryMv = 0;
    assert(rf("stats sensors") == "schema=1 scope=device battery_mv=unavailable mcu_temp_c=-12.50");
    const auto measurements = f.send(peer, "", false, true, 3, true, 100, 3);
    assert(measurements.size() >= 8 && measurements[4] == 1 && measurements[5] == 103 &&
           measurements[6] == 0xff && measurements[7] == 0x83);
    board.mcuTemperature = std::numeric_limits<float>::max();
    assert(rf("stats sensors") == "Error: MCU temperature exceeds stats representation");
    board.batteryMv = 4000;
    board.mcuTemperature = previousTemperature;
    for (const char *topic : {"radio", "tx", "airtime", "admission", "sensors", "memory", "psram", "bot", "vm"}) {
      const auto answer = rf((std::string("0123456789abcdef|stats ") + topic).c_str());
      assert(answer.find("0123456789abcdef|") == 0 && answer.size() <= 147);
    }
    DashboardStreamTest::maximumCounters(f.dashboard);
    for (const char *topic : {"radio", "tx", "airtime", "admission"}) {
      const auto answer = f.action((std::string("stats ") + topic).c_str());
      assert(answer.find("schema=1 scope=modem ") == 0 && answer.size() <= 130);
      assert(answer.find("18446744073709551615") != std::string::npos);
    }
    assert(rf("help missing").find("unknown help topic") != std::string::npos);
    for (const char *command : {"wifi ssid", "wifi password", "set wifi.ssid", "set wifi.pwd", "set wifi.enabled",
                                "set name", "set owner.info"})
      assert(rf(command).find("Error: usage: ") == 0);
    assert(rf("ver").find("v" MESHCORE_SLP_ASPEN_VERSION) == 0);
    assert(rf("get radio").find("> 912.525") == 0);
    assert(rf("get freq").find("> 912.525") == 0);
    assert(rf("get tx") == "> 2");
    const auto shared = identity_test::durable;
    for (const char *command : {"set radio 915,250,7,5", "set freq 915", "set tx 22"})
      assert(rf(command).find("use radio FREQ_HZ") != std::string::npos);
    assert(f.mux.currentConfiguration().freq_hz == 912525000);
    assert(rf("get owner.info") == "> ");
    assert(rf("set owner.info First|Second").find("OK - saved") == 0);
    assert(rf("get owner.info") == "> First|Second");
    const auto key = encode(f.management.publicKey(), 32);
    assert(rf("set name App Admin").find("OK - saved") == 0);
    assert(rf("get name") == "> App Admin" && encode(f.management.publicKey(), 32) == key);
    assert(rf("get wifi.enabled") == "> 1");
    const auto configuration = identity_test::durable;
    for (const char *command : {"set wifi.ssid lab network", "set wifi.pwd test pass",
                               "set wifi.enabled 0", "get wifi.pwd", "wifi ssid 6162",
                               "wifi password 6162636465666768", "wifi 6162 6162636465666768", "wifi forget"})
      assert(f.action(command).find("encrypted Management RF") != std::string::npos);
    assert(identity_test::durable == configuration);
    assert(rf("set wifi.ssid lab network").find("OK - saved") == 0);
    assert(rf("set wifi.pwd test pass").find("OK - saved") == 0);
    assert(rf("get wifi.ssid") == "> lab network");
    assert(rf("get wifi.pwd") == "> test pass");
    assert(rf("set wifi.pwd ").find("OK - saved") == 0);
    assert(rf("get wifi.pwd") == "> ");
    assert(rf("set wifi.pwd short").find("Error:") == 0);
    const auto savedWifi = identity_test::durable;
    identity_test::failCommit = true;
    MastAdmin::Reply failed;
    f.management.admin().execute("set wifi.ssid changed", failed, 0, MastAdmin::Transport::NativeEncrypted);
    assert(strstr(failed.text, "commit/readback unknown"));
    identity_test::failCommit = false;
    assert(identity_test::durable.at({"mc-mast-admin", "settings"}) ==
           savedWifi.at({"mc-mast-admin", "settings"}));
    assert(rf("get wifi.ssid") == "> lab network");
    for (const auto &record : shared)
      if (record.first.first != "mc-mast-admin" &&
          record.first != std::make_pair(std::string("mc-onchip"), std::string("management-name")))
        assert(identity_test::durable.at(record.first) == record.second);
    for (const char *command : {"set wifi.enabled invalid", "set wifi.enabled 2", "set wifi.enabled 1 extra"})
      assert(rf(command).find("Error: usage: ") == 0);
    MastAdmin::Reply reply;
    f.management.admin().execute("set wifi.enabled 0", reply, 0, MastAdmin::Transport::NativeEncrypted);
    assert(reply.ticket && rf("get wifi.enabled") == "> 0");
    assert(f.action("job").find("waiting for acceptance") == 0);
    f.management.admin().acknowledged(reply.ticket, false); f.step();
    assert(f.action("job").find("cancelled") != std::string::npos);
    assert(rf("set wifi.enabled on").find("OK - saved") == 0);
    assert(f.action("job") == "WiFi reconnect requested; association not yet confirmed");
    assert(rf("set wifi.enabled off").find("OK - saved") == 0);
    assert(f.action("job") == "WiFi disabled; RF roles remain active");
    assert(rf("wifi apply").find("WiFi disabled; use set wifi.enabled 1") != std::string::npos);
    bool enabled;
    assert(MastAdmin::loadWifiEnabled(enabled) && !enabled);
    assert(f.send(outsider, "", false, true, 3, true, 100, 7).empty());
    for (uint8_t width : {1, 2, 3}) {
      const auto body = f.send(peer, "", false, true, width, true, 100, 7);
      assert(body.size() > 4 && queued_tx::get32(body.data()) == peer.timestamp);
      assert(std::string(reinterpret_cast<const char *>(body.data() + 4)) ==
             MESHCORE_SLP_ASPEN_VERSION "\nApp Admin\nFirst\nSecond");
      --peer.timestamp;
      assert(f.send(peer, "", false, true, width, true, 100, 7).empty());
    }
    assert(rf(("set owner.info " + std::string(MastAdmin::OwnerInfoLimit, 'x')).c_str()).find("OK - saved") == 0);
    assert(rf(("set owner.info " + std::string(MastAdmin::OwnerInfoLimit + 1, 'x')).c_str()).find("exceeds 119") != std::string::npos);
    assert(rf(("set name " + std::string(31, 'n')).c_str()).find("OK - saved") == 0);
    const auto body = f.send(peer, "", false, true, 3, true, 100, 7);
    assert(body.size() > 4 && std::string(reinterpret_cast<const char *>(body.data() + 4)).find(
        MESHCORE_SLP_ASPEN_VERSION "\n" + std::string(31, 'n') + "\n") == 0);
    assert(std::string(reinterpret_cast<const char *>(body.data() + 4)).size() <
           strlen(MESHCORE_SLP_ASPEN_VERSION) + 2 + 31 + MastAdmin::OwnerInfoLimit);
    const auto direct = f.send(peer, "", false, false, 3, true, 100, 7);
    const std::string expected = MESHCORE_SLP_ASPEN_VERSION "\n" + std::string(31, 'n') + "\n" + std::string(MastAdmin::OwnerInfoLimit, 'x');
    assert(std::string(reinterpret_cast<const char *>(direct.data() + 4)) == expected.substr(0, 163));
    assert(rf("set name App Admin").find("OK - saved") == 0);
    assert(rf("get owner.info") == "> " + std::string(MastAdmin::OwnerInfoLimit, 'x'));
    const auto principals = rf("auth status");
    for (uint8_t width : {1, 2, 3}) {
      const char returnPath[] = {char((width - 1) << 6), 0};
      const auto publicInfo = f.send(outsider, returnPath, false, false, width, true, 100, 0, 2);
      assert(publicInfo.size() > 8 && queued_tx::get32(publicInfo.data()) == outsider.timestamp);
      assert(std::string(reinterpret_cast<const char *>(publicInfo.data() + 8)) ==
             "App Admin\n" + std::string(MastAdmin::OwnerInfoLimit, 'x'));
    }
    assert(rf("auth status") == principals);
    assert(f.send(outsider, "\xff", false, false, 3, true, 100, 0, 2).empty());
    assert(f.send(outsider, "\x80", false, true, 3, true, 100, 0, 2).empty());
    assert(!f.send(outsider, "\x80", false, false, 3, true, 100, 0, 2).empty());
    assert(f.send(outsider, "\x80", false, false, 3, true, 100, 0, 2).empty());
    timeMs += 180001;
    assert(!f.send(outsider, "\x80", false, false, 3, true, 100, 0, 2).empty());
    assert(f.action(("set name " + std::string(31, 'n')).c_str()).find("OK - saved") == 0);
    for (uint8_t width : {1, 2, 3}) {
      std::string returnPath(1, char(((width - 1) << 6) | 3));
      returnPath += std::string(3 * width, 'a');
      const auto publicInfo = f.send(outsider, returnPath.c_str(), false, false, width, true, 100, 0, 2);
      assert(publicInfo.size() > 8 && queued_tx::get32(publicInfo.data()) == outsider.timestamp);
      assert(std::string(reinterpret_cast<const char *>(publicInfo.data() + 8)) ==
             std::string(31, 'n') + "\n" + std::string(MastAdmin::OwnerInfoLimit, 'x'));
    }
    assert(f.action("set name App Admin").find("OK - saved") == 0);
    identity_test::failRead = true;
    assert(f.action("get owner.info").find("unavailable") != std::string::npos);
    identity_test::failRead = false;
  }
  {
    BetaFixture restarted;
    assert(restarted.action("get name") == "> App Admin");
    assert(restarted.action("get wifi.enabled") == "> 0");
    assert(restarted.action("get owner.info") == "> " + std::string(MastAdmin::OwnerInfoLimit, 'x'));
  }
  identity_test::durable = durable; filesystem_test::files = files;
  puts("PASS Management CLI help/profile/native preferences, encrypted-only WiFi aliases, persistence and response-gated WiFi, native owner-info over routed crypto");
}
static std::string encode(const uint8_t *bytes, size_t size) {
  std::string text(size * 2, '0');
  const char *digits = "0123456789abcdef";
  for (size_t i = 0; i < size; ++i) {
    text[2 * i] = digits[bytes[i] >> 4]; text[2 * i + 1] = digits[bytes[i] & 15];
  }
  return text;
}
static void public_users_and_private_routes() {
  BetaFixture f;
  Peer owner, publicUser;
  assert(memcmp(owner.self_id.pub_key, publicUser.self_id.pub_key, 32));
  assert(!f.management.admin().trusted(publicUser.self_id.pub_key));
  const std::string source(96, ' ');
  uint8_t hash[32];
  mesh::Utils::sha256(hash, sizeof(hash), reinterpret_cast<const uint8_t *>(source.data()), source.size());
  const std::string id = "fedcba9876543210";
  const std::string begin = "source begin " + id + " 96 " + encode(hash, sizeof(hash));
  const std::string chunk = "source chunk " + id + " 0 " +
      encode(reinterpret_cast<const uint8_t *>(source.data()), 48);
  const auto before = f.action("source status");
  assert(f.send(publicUser, begin.c_str(), false).empty());
  assert(f.action("source status") == before);
  assert(f.send(publicUser, "bot https status", false).empty());
  for (uint8_t width : {1, 2, 3}) {
    timeMs += 1100;
    assert(!f.send(owner, "mast-pass-12", true, true, width, false).empty());
    const auto manifest = f.send(owner, begin.c_str(), false, true, width);
    assert(manifest.size() > 5 && manifest[4] == 4);
    assert(std::string(manifest.begin() + 5, manifest.end()).find("ACK fedcba9876543210") == 0);
    const auto receipt = f.send(owner, chunk.c_str(), false, true, width);
    assert(receipt.size() > 5 && receipt[4] == 4);
    assert(std::string(receipt.begin() + 5, receipt.end()).find("ACK fedcba9876543210 next=1") == 0);
    const auto progress = f.action("source status");
    assert(f.send(publicUser, "source cancel", false, true, width).empty());
    assert(f.action("source status") == progress);
  }
  assert(!f.send(owner, "source cancel", false, true, 3).empty());
  timeMs += 61000;
  const auto advert = publicUser.advert();
  f.mux.received(advert.data(), advert.size(), -90, 5);
  f.step();
  f.radio.sent.clear();
  const auto ping = publicUser.command(f.bot.publicKey(), "!ping");
  f.mux.received(ping.data(), ping.size(), -90, 5);
  f.step();
  const auto replies = publicUser.replies(f.bot.publicKey(), f.radio);
  assert(replies.size() == 1 && replies[0] == "Pong");
  puts("PASS public user without owner login; private chunks use native crypto/TXT_MSG "
       "and 1/2/3-byte-hash routed replies, unauthorized upload remains unchanged");
}
static void origin_path_policy() {
  const auto baseline = identity_test::durable;
  ProfileJournal journal, unchanged;
  assert(loadProfileJournal(journal));
  uint8_t width = 0;
  assert(loadOriginPathWidth(width) && width == 1);
  assert(!saveOriginPathWidth(0) && !saveOriginPathWidth(4));
  {
    BetaFixture f;
    assert(f.management.setPathWidth(3) && f.management.pathWidth() == 3);
    Peer peer;
    assert(!f.send(peer, "mast-pass-12", true, true, 1, false).empty());
    assert(!f.send(peer, "status", false).empty());
    bool text = false;
    for (const auto &wire : f.radio.sent) {
      mesh::Packet packet;
      assert(packet.readFrom(wire.data(), wire.size()));
      if (packet.getPayloadType() == PAYLOAD_TYPE_TXT_MSG) {
        assert(packet.isRouteFlood() && packet.getPathHashSize() == 3);
        text = true;
      }
    }
    assert(text);
  }
  {
    BetaFixture f;
    assert(f.management.pathWidth() == 3);
    identity_test::failCommit = true;
    assert(!f.management.setPathWidth(2) && f.management.pathWidth() == 3);
    identity_test::failCommit = false;
  }
  assert(loadProfileJournal(unchanged));
  assert(unchanged.profile.enabled == journal.profile.enabled &&
         unchanged.generation == journal.generation && unchanged.nonce == journal.nonce);
  identity_test::durable = baseline;
  puts("PASS durable management origin width independent of native role selection; "
       "requester-width return PATH preserved, no role-journal change");
}
static void source_copy_limits() {
  BotWorker worker;
  assert(worker.begin());
  const auto poll = [&]() {
    BotWorker::Result result;
    for (unsigned i = 0; i < 1000; ++i) {
      if (worker.poll(result)) return result;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(false && "Source copy worker did not complete");
    return result;
  };
  const char *from = "/command-bot/upload.lua";
  filesystem_test::files[from] = Bytes(BotSourceLimit, ' ');
  assert(!worker.copyFile("/unapproved", BotStagedSourcePath, 1));
  assert(!worker.copyFile(from, "/unapproved", 1));
  assert(!worker.copyFile(from, BotStagedSourcePath, BotSourceLimit + 1));
  assert(worker.copyFile(from, BotStagedSourcePath, BotSourceLimit));
  assert(poll().ok);
  assert(filesystem_test::files[from] == filesystem_test::files[BotStagedSourcePath]);
  filesystem_test::writeLimit = 1;
  assert(worker.copyFile(from, BotStagedSourcePath, BotSourceLimit));
  auto result = poll();
  assert(!result.ok && strstr(result.error, "write incomplete"));
  filesystem_test::writeLimit = SIZE_MAX;
  filesystem_test::readDelayMs = BotSourceCopyBudgetMs;
  assert(worker.copyFile(from, BotStagedSourcePath, BotSourceLimit));
  result = poll();
  filesystem_test::readDelayMs = 0;
  assert(!result.ok && strstr(result.error, "deadline"));
  assert(worker.activate() && !poll().ok);
  worker.stop();
  puts("PASS source copy worker: exact 4 KiB, restricted paths, short write and deadline failure");
}
static void upload(BetaFixture &f, const std::string &source, bool enabled = true, bool wait = true,
                   const std::function<std::string(const char *)> &transport = {}) {
  const auto action = [&](const char *text) { return transport ? transport(text) : f.action(text); };
  uint8_t digest[32];
  mesh::Utils::sha256(digest, 32, reinterpret_cast<const uint8_t *>(source.data()), source.size());
  const auto hash = encode(digest, 32), id = hash.substr(0, 16);
  const auto begin = "source begin " + id + " " + std::to_string(source.size()) + " " + hash;
  const auto beginReply = action(begin.c_str());
  if (beginReply.find("ACK ") != 0)
    fprintf(stderr, "source begin failed size=%zu source=%.80s: %s; %s\n", source.size(),
            source.c_str(), beginReply.c_str(), action("source status").c_str());
  assert(beginReply.find("ACK ") == 0);
  for (size_t offset = 0; offset < source.size(); offset += 48) {
    const auto chunk = "source chunk " + id + " " + std::to_string(offset / 48) + " " +
        encode(reinterpret_cast<const uint8_t *>(source.data()) + offset, std::min(size_t(48), source.size() - offset));
    assert(action(chunk.c_str()).find("ACK ") == 0);
    assert(action(chunk.c_str()).find("ACK ") == 0);
  }
  assert(action(("source commit " + id).c_str()).find("Accepted") == 0);
  if (!wait) return;
  f.step(300);
  const auto status = action("source status");
  const char *expected = enabled ? "durably saved and active" : "bot disabled";
  if (status.find(expected) == std::string::npos) fprintf(stderr, "%s\n", status.c_str());
  assert(status.find(expected) != std::string::npos);
}
static std::string packageSource(const char *version, const char *schema, const char *rollback,
                                 const char *reply, const char *capabilities = "none") {
  return std::string("--@meshcore-bot/1;name=release-test;version=") + version +
      ";runtime=lua-5.5.1;api=named-commands-v1;caps=" + capabilities + ";schema=" + schema +
      ";rollback=" + rollback + "\nfunction release_echo() reply('" + reply + "') end";
}
#ifdef PINE_PARSER_STACK_TEST
static void sourceParserStackValidation() {
  static_assert(ONCHIP_BOT_COMPACT_PROFILE && ONCHIP_BOT_SINGLE_SESSION);
  BetaFixture f;
  upload(f, "function prior() return 'retained' end");
  const auto hash = f.action("source hash");
  const auto generation = f.bot.sourceWorker().sourceGeneration();
  for (bool global : {false, true}) {
    const auto source = nestedBotFunctions(39, global);
    upload(f, source, true, false);
    f.step(400);
    const auto status = f.action("source status");
    if (status.find("C stack overflow") == std::string::npos)
      fprintf(stderr, "Nested source commit outcome: %s\n", status.c_str());
    assert(status.find("C stack overflow") != std::string::npos &&
           f.action("source hash") == hash &&
           f.bot.sourceWorker().sourceGeneration() == generation &&
           !f.bot.sourceWorker().sourceSuspended());
    assert(f.action("bot status").find("ready=1") != std::string::npos);
    assert(f.action("source cancel").find("Upload cancelled") == 0);
  }
  upload(f, "function replacement() return 'recovered' end");
  assert(f.action("source hash") != hash);
  puts("PASS compact native source begin/chunk/commit: 39-level named/global functions reject without publication; prior hash/generation and native admin retained; next update succeeds");
}
#endif
static void packageMetadataLifecycle() {
  BetaFixture f;
  assert(f.action("source cancel").find("Upload cancelled") == 0);
  assert(f.action("source status").find("upload=none") != std::string::npos);
  const auto v1 = packageSource("1234567890123456789.1.1", "notes@1", "none", "v1",
                                "cmdmeta,mesh-chan,mesh-dest");
  upload(f, v1);
  const auto firstHash = f.action("source hash");
  assert(f.action("source metadata").find("schema=notes@1 rollback=none") != std::string::npos);

  const auto v2 = packageSource("2.0.0", "notes@2", "notes@1", "v2");
  upload(f, v2);
  const auto secondHash = f.action("source hash");
  assert(secondHash != firstHash);
  assert(f.action("source metadata").find("schema=notes@2 rollback=notes@1") != std::string::npos);
  assert(f.action("source rollback").find("Accepted") == 0);
  f.step(300);
  assert(f.action("source status").find("durably saved and active") != std::string::npos);
  assert(f.action("source metadata").find("schema=notes@1") != std::string::npos);
  const auto rollbackHash = f.action("source hash");
  assert(rollbackHash.find("SHA256 " + firstHash.substr(7, 64) + " gen=") == 0);

  const auto refused = f.action("source rollback");
  assert(refused.find("Error: rollback schema is not declared compatible") == 0);
  assert(f.action("source hash") == rollbackHash);

  const auto incompatible = packageSource("3.0.0", "notes@3", "none", "v3");
  upload(f, incompatible, true, false);
  f.step(300);
  assert(f.action("source status").find("Error: schema change requires rollback=notes@1") !=
         std::string::npos);
  assert(f.action("source hash") == rollbackHash);
  assert(f.action("source remove").find("Accepted") == 0);
  f.step(300);
  assert(f.action("source metadata") == "BUNDLED schema=none");

  const std::string plain = "\t-- UTF-8 comment \xc3\xa9\nfunction release_echo() reply('plain') end";
  upload(f, plain);
  assert(f.action("source metadata") == "PLAIN schema=unknown");
  assert(f.action("source remove").find("Accepted") == 0);
  f.step(300);
  assert(f.action("source metadata") == "BUNDLED schema=none");

  upload(f, v1);
  assert(f.action("source rollback").find("Accepted") == 0);
  f.step(300);
  assert(f.action("source status").find("durably saved and active") != std::string::npos);
  assert(f.action("source metadata") == "BUNDLED schema=none");

  const auto bundledHash = f.action("source hash");
  const auto tooLongVersion =
      packageSource("12345678901234567890.1.1", "notes@1", "none", "too long");
  upload(f, tooLongVersion, true, false);
  f.step(300);
  assert(f.action("source status").find("Error: package metadata value invalid or too long") !=
         std::string::npos);
  assert(f.action("source hash") == bundledHash);
  assert(f.action("source cancel").find("Upload cancelled") == 0);
  assert(f.action("source status").find("upload=none") != std::string::npos);
}
static void interrupted_upload() {
  const std::string source = "module('stored',function() return {text='resumed'} end) "
      "function custom() reply(require('stored').text) end command('custom','','Resumed')" + std::string(90, ' ');
  uint8_t digest[32];
  mesh::Utils::sha256(digest, 32, reinterpret_cast<const uint8_t *>(source.data()), source.size());
  const auto hash = encode(digest, 32), id = hash.substr(0, 16);
  const auto begin = "source begin " + id + " " + std::to_string(source.size()) + " " + hash;
  const auto chunk = [&](size_t i) {
    return "source chunk " + id + " " + std::to_string(i) + " " +
        encode(reinterpret_cast<const uint8_t *>(source.data()) + i * 48,
               std::min(size_t(48), source.size() - i * 48));
  };
  {
    BetaFixture f;
    assert(f.action("source api") == "API named-commands-v1 lua=5.5.1 commands=8 arguments=4 source-bytes=4096 runtime=shared-v1 jobs=" +
        std::to_string(BotJobLimit) + " kv=2 sleep=1 mesh=dm,wait,trace,advert rpc=1");
    assert(f.action("source api package").find(
        "Package api=named-commands-v1 caps=cmdmeta,events,") == 0);
    assert(f.action("source api storage") == "Storage kv=2 scopes=caller,conversation,bot,channel timer=set,get,cancel,wait autonomous-reminders=1");
    assert(f.action("source api notes") == "Notes remember,recall,forget,notes,list-memories private-dm=1 kv-list=1 keys=8 key-bytes=32 text=120 overwrite=explicit");
    assert(f.action("source api utility") == "Utility calc,convert,roll,choose calls=8 digits=10 arithmetic-depth=8 tokens=64 ops=32 dice=12d1000 choices=8 channel=verified");
    assert(f.action("source api services") == "Services weather,service home=health,echo,weather private-dm=1 grant=bot-home place=required place-bytes=80 echo-bytes=120");
    assert(f.action("source api board") == "Board put,get,list,delete channel=verified grant=bot-shared dm=denied prefix=board: key-bytes=26 text=120 quota=shared-8");
    assert(f.action("source api diagnostics") == "Diagnostics about,version,uptime,status,signal,air readonly=1 channel=verified air-pages=4 battery=unavailable counters=u32");
    assert(f.action("source api reminders").find("private-dm=1 autonomous=1") != std::string::npos);
    assert(f.action("bot reminders").find("saved=0 applied=0") != std::string::npos);
    assert(f.action("bot reminders invalid").find("Error:") == 0);
    assert(f.action("bot reminders on").find("Saved") == 0);
    assert(f.action("bot reminders").find("saved=1") != std::string::npos);
    identity_test::failCommit = true;
    assert(f.action("bot reminders off").find("Error:") == 0);
    identity_test::failCommit = false;
    assert(f.action("bot reminders off").find("Saved") == 0);
    assert(f.action("source api mesh") ==
        "Mesh compose=dm,channel,trace dm=caller+owner-fullkey[1-4] "
        "wait=kind:ack/text/channel/trace chan-wait=owner-gated/default-off "
        "fwd=pair-dm multi=3 raw=0");
    assert(f.action("source api paths") ==
        "Paths ordinary_mode=0/1/2->1/2/3B bot path/role-path args=bytes(1..3) "
        "TRACE explicit=1/2/4/8B inferred=1/2B TRACE width3=reject");
    assert(f.action("bot forward").find("saved=0 applied=0") != std::string::npos);
    assert(f.action("bot forward invalid").find("Error:") == 0);
    const std::string from(64, '1'), to(64, '2');
    assert(f.action(("bot forward " + from + ":" + from).c_str()).find("Error:") == 0);
    assert(f.action(("bot forward " + std::string(64, '0') + ":" + to).c_str()).find("Error:") == 0);
    assert(f.action(("bot forward " + from + ":" + to).c_str()).find("Saved") == 0);
    assert(f.action("bot forward").find("saved=1") != std::string::npos);
    assert(f.action("bot forward from") == "KEY " + from);
    assert(f.action("bot forward to") == "KEY " + to);
    identity_test::failCommit = true;
    assert(f.action("bot forward off").find("Error:") == 0);
    identity_test::failCommit = false;
    assert(f.action("bot forward off") == "Saved and applied DM forwarding off");
    assert(f.action("bot home") == "Home HTTPS configured=0 saved=0 applied=0 clock=0");
    assert(f.action("bot home on").find("Error:") == 0);
    assert(f.action("bot home off") == "Saved and applied home RPC grant");
#if !ONCHIP_BOT_HTTPS
    assert(f.action("source api fetch") == "Owner fetch unavailable");
    const auto sourceBeforeFetch = f.action("source hash");
    assert(f.action(("source fetch packages " + std::string(64, 'a')).c_str()).find("Error:") == 0);
    assert(f.action("source hash") == sourceBeforeFetch);
#else
    assert(f.action("source api fetch") ==
        "Owner GET JSON=2048; Package GET alias=package raw=1..4096 "
        "type=text/plain|application/octet-stream|application/x-lua SHA256=source");
    const auto sourceBeforeFetch = f.action("source hash");
    assert(f.action(("source fetch packages " + std::string(64, 'a')).c_str()).find("Error:") == 0);
    assert(f.action("source hash") == sourceBeforeFetch);
#endif
    assert(f.action("bot shared") == "Shared KV off");
    assert(f.action("bot shared on") == "Saved and applied shared KV policy");
    assert(f.action("bot shared") == "Shared KV on");
    assert(f.action("bot shared off") == "Saved and applied shared KV policy");
    assert(f.action("bot events").find("saved=0 subscribed=0") != std::string::npos);
    assert(f.action("bot events 16").find("Error:") == 0);
    assert(f.action("bot events 15").find("Saved event") == 0);
    assert(f.action("bot events").find("saved=15") != std::string::npos);
    identity_test::failCommit = true;
    assert(f.action("bot events 0").find("Error:") == 0);
    identity_test::failCommit = false;
    assert(f.action("bot events 0").find("Saved event") == 0);
    assert(f.action("bot channel #example1") == "Saved bot radio policy; apply reboots");
    assert(f.action("bot path 3") == "Saved bot radio policy; apply reboots");
    assert(f.action("bot airtime 1200") == "Saved bot radio policy; apply reboots");
    assert(f.action("bot policy").find("channel=#example1 path-bytes=3 airtime-ms/min=1200") != std::string::npos);
    assert(f.action("bot stats").find("Replies=") == 0);
    assert(f.action("bot stats").find("vm-fail=") != std::string::npos);
    assert(f.action("bot admission").find("Last=none wait-ms=0 active=0") == 0);
    assert(f.action("bot adaptive").find("saved=0 live=0") != std::string::npos);
    assert(f.action("bot adaptive on") == "Saved adaptive admission; reboot required");
    assert(f.action("bot adaptive").find("saved=1 live=0") != std::string::npos);
    assert(f.action("bot adaptive everyone").find("Error:") == 0);
    identity_test::failCommit = true;
    assert(f.action("bot adaptive off").find("Error:") == 0);
    identity_test::failCommit = false;
    assert(f.action("bot adaptive off") == "Saved adaptive admission; reboot required");
    identity_test::durable[{"mc-onchip", "bot-adaptive"}] = {'B', 'A', 'D', 1, 0};
    assert(f.action("bot adaptive").find("policy-fault=1") != std::string::npos);
    assert(f.action("bot adaptive off") == "Saved adaptive admission; reboot required");
    assert(f.action("bot adaptive").find("saved=0 live=0") != std::string::npos);
    assert(f.action("bot channel #Bad").find("Error:") == 0);
    assert(f.action("bot path 4").find("Error:") == 0);
    assert(f.action("bot airtime 3601").find("Error:") == 0);
    assert(saveBotRadioPolicy({}));
    assert(f.action(begin.c_str()).find("next=0") != std::string::npos);
    assert(f.action(chunk(0).c_str()).find("next=1") != std::string::npos);
    assert(f.action(chunk(2).c_str()).find("Error: expected chunk 1") == 0);
    auto conflict = chunk(0);
    conflict.back() = conflict.back() == '0' ? '1' : '0';
    assert(f.action(conflict.c_str()).find("Error: conflicting") == 0);
    filesystem_test::writeLimit = 1;
    assert(f.action(chunk(1).c_str()).find("Error: incomplete") == 0);
    filesystem_test::writeLimit = std::numeric_limits<size_t>::max();
  }
  // Web controls must remain available while a durable upload is interrupted.
  const auto authenticated_web = [](bool installCommands = false) {
    BetaFixture f;
    TestHTTPServer server;
    assert(registerMastWeb(&server) == ESP_OK && server.routes.size() == 4);
    const auto request = [&](const char *path, const std::string &body,
                             const std::string &token = "", const std::string &origin = "") {
      httpd_req_t r;
      r.body = body; r.content_len = body.size();
      r.headers["Host"] = "mast.test";
      if (!token.empty()) r.headers["X-Mast-Session"] = token;
      if (!origin.empty()) r.headers["Origin"] = origin;
      assert(server.routes.at(path).handler(&r) == ESP_OK);
      return r;
    };
    assert(request("/admin/command", "status").status == "403 Forbidden");
    assert(request("/admin/command", "bot diagnostics").status == "403 Forbidden");
    assert(request("/admin/command", "data export bot " + std::string(64, '0')).status == "403 Forbidden");
    assert(request("/admin/command", "data restore 0000000000000000").status == "403 Forbidden");
    assert(request("/admin/command", "data export timers bot " + std::string(64, '0')).status == "403 Forbidden");
    assert(request("/admin/command", "data restore 0000000000000000 no-rearm").status == "403 Forbidden");
    assert(request("/admin/command", "bot events 15").status == "403 Forbidden");
    assert(request("/admin/command", "role name management Unauthorized").status == "403 Forbidden");
    assert(request("/admin/command", "role name kiss Unauthorized").status == "403 Forbidden");
    assert(request("/admin/command", "role advert management zerohop").status == "403 Forbidden");
    assert(request("/admin/login", "wrong-password").status == "403 Forbidden");
    timeMs += 1100;
    const auto login = request("/admin/login", "mast-pass-12");
    assert(login.response.size() == 32 && login.status == "200 OK");
    const auto token = login.response;
    for (const char *text : {"telemetry endpoint ca", "telemetry endpoint ca clear",
         "telemetry endpoint ca 4142", "telemetry endpoint token",
         "telemetry endpoint token clear", "telemetry endpoint token 736563726574"}) {
      const auto denied = request("/admin/command", text, token);
      assert(denied.status == "403 Forbidden" &&
             denied.response.find("encrypted authenticated RF") != std::string::npos &&
             denied.response.find("736563726574") == std::string::npos);
    }
    assert(request("/admin/command", "bot https token demo 616263", token).status == "403 Forbidden");
    assert(request("/admin/command", "role password repeater 736563726574", token).status == "403 Forbidden");
    assert(request("/admin/command", "role password room 736563726574", token).status == "403 Forbidden");
    assert(request("/admin/command", "bot https  ca demo 616263", token).status == "403 Forbidden");
    assert(request("/admin/command", "status", token, "http://untrusted.test").status == "403 Forbidden");
    assert(request("/admin/command", std::string(163, 'x'), token).status == "400 Bad Request");
    httpd_req_t slow;
    slow.body = "status"; slow.content_len = 6;
    slow.headers["X-Mast-Session"] = token;
    slow.headers["Host"] = "mast.test";
    slow.readDelay = 2000;
    assert(server.routes.at("/admin/command").handler(&slow) == ESP_OK);
    assert(slow.status == "400 Bad Request");
    const auto execute = [&](const char *text, bool failSend = false) {
      httpd_req_t r;
      r.body = text; r.content_len = r.body.size();
      r.headers["X-Mast-Session"] = token; r.failSend = failSend;
      r.headers["Host"] = "mast.test";
      std::atomic<bool> done{false};
      std::thread http([&]() {
        const auto result = server.routes.at("/admin/command").handler(&r);
        assert(result == (failSend ? ESP_FAIL : ESP_OK));
        done = true;
      });
      for (unsigned i = 0; i < 300 && !done; ++i) f.step(1);
      assert(done);
      http.join();
      return r;
    };
    assert(execute("status").response.find("roles applied=") == 0);
    {
      Peer diagnosticPeer;
      const auto advert = diagnosticPeer.advert();
      f.mux.received(advert.data(), advert.size(), -90, 5);
      f.step();
      const auto before = execute("bot diagnostics").response;
      unsigned queuedBefore = 0, droppedBefore = 0;
      assert(sscanf(before.c_str(), "Diagnostics queued=%u dropped=%u", &queuedBefore, &droppedBefore) == 2);
      f.bot.setDiagnosticSink([](const char *) { return false; });
      serial_test::forbidWrites = true;
      BotForwardPolicy invalid;
      invalid.from[0] = invalid.to[0] = 1;
      assert(!f.bot.setForwardPolicy(invalid));
      assert(execute("bot log").response.find("fault=Forward policy requires distinct non-bot") != std::string::npos);
      for (unsigned i = 0; i < 10; ++i) {
        f.radio.sent.clear();
        const auto packet = diagnosticPeer.command(f.bot.publicKey(), "!ping");
        f.mux.received(packet.data(), packet.size(), -90, 5);
        const auto response = execute("bot diagnostics");
        assert(response.status == "200 OK" && response.response.find("sink=async") != std::string::npos);
        f.step();
        assert(diagnosticPeer.replies(f.bot.publicKey(), f.radio) == std::vector<std::string>{"Pong"});
      }
      const auto after = execute("bot diagnostics").response;
      unsigned queuedAfter = 0, droppedAfter = 0;
      assert(sscanf(after.c_str(), "Diagnostics queued=%u dropped=%u", &queuedAfter, &droppedAfter) == 2);
      assert(queuedAfter == queuedBefore && droppedAfter >= droppedBefore + 20);
      assert(f.bot.counters().lastVm.peakBytes);
      serial_test::forbidWrites = false;
      f.bot.setDiagnosticSink(nullptr);
      puts("PASS authenticated mailbox and ten encrypted Pongs with diagnostic sink rejecting every log; no dispatch Serial writes");
    }
    const auto sessionStats = companionSessions().stats();
    const auto tcpStats = execute("companion stats").response;
    assert(tcpStats.find("TCP clients=" + std::to_string(sessionStats.connectedClients)) == 0);
    assert(tcpStats.find("accepted=" + std::to_string(sessionStats.acceptedClients)) != std::string::npos);
    assert(tcpStats.find("dropped=" + std::to_string(sessionStats.droppedClients)) != std::string::npos);
    const auto tcpErrors = execute("companion errors").response;
    assert(tcpErrors.find("TCP timeout=" + std::to_string(sessionStats.timedOutClients)) == 0);
    assert(tcpErrors.find("output-full=" + std::to_string(sessionStats.outputOverflows)) != std::string::npos);
    assert(tcpErrors.find("journal-full=" + std::to_string(sessionStats.journalOverruns)) != std::string::npos);
    assert(companionSessions().stats().nativeResets == sessionStats.nativeResets);
    assert(execute("companion help").response.find("companion stats:") == 0);
    const auto beforeImport = identity_test::durable;
    assert(execute(("key bot " + std::string(128, '0')).c_str()).response.find("encrypted Management RF") != std::string::npos);
    assert(execute("password 74657374").response.find("encrypted Management RF") != std::string::npos);
    assert(identity_test::durable == beforeImport);
    const auto originalName = execute("role name management").response.substr(6);
    assert(execute("role name management Web Admin").response.find("Saved and applied") == 0);
    assert(execute("role name management").response == "Name: Web Admin");
    assert(execute("role advert management zerohop").response.find("Queued zero-hop") == 0);
    assert(execute(("role name management " + originalName).c_str()).response.find("Saved and applied") == 0);
    for (const char *text : {"telemetry on", "telemetry interval 60", "telemetry status",
         "telemetry endpoint host vm.example", "telemetry endpoint path /write",
         "telemetry endpoint commit", "telemetry endpoint status"})
      assert(execute(text).status == "200 OK");
    assert(execute("source api atomic").response.find(
        "Atomic cas=1 transaction=" + std::to_string(BotTransactionLimit)) == 0);
    assert(execute("source api data").response.find("owner=required") != std::string::npos);
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    WiFiServer kissServer;
    kissServer.add(WiFiClient(sockets[0]));
    f.mux.poll(kissServer);
    const uint8_t hello[] = {0xc0, 6, queued_tx::HELLO, 1, 0, 0xc0};
    assert(send(sockets[1], hello, sizeof(hello), 0) == sizeof(hello));
    const auto configGet = [&]() {
      const uint8_t query[] = {0xc0, 6, queued_tx::CONFIG, 1, 0, 0xc0};
      assert(send(sockets[1], query, sizeof(query), 0) == sizeof(query));
      f.mux.poll(kissServer); f.mux.poll(kissServer);
      uint8_t bytes[512];
      const int n = recv(sockets[1], bytes, sizeof(bytes), MSG_DONTWAIT);
      assert(n > 0);
      Bytes frame;
      bool escaped = false;
      for (int i = 0; i < n; ++i) {
        const uint8_t value = bytes[i];
        if (value == 0xc0) {
          if (frame.size() == 26 && frame[0] == 6 && frame[1] == 0xa2) {
            assert(frame[2] == 1 && frame[3] == queued_tx::NONE);
            return frame;
          }
          frame.clear(); escaped = false;
        } else if (escaped) {
          assert(value == 0xdc || value == 0xdd);
          frame.push_back(value == 0xdc ? 0xc0 : 0xdb); escaped = false;
        } else if (value == 0xdb) escaped = true;
        else frame.push_back(value);
      }
      assert(false && "No CONFIG GET response");
      return Bytes{};
    };
    const auto generation = f.mux.configurationGeneration();
    auto config = configGet();
    assert(queued_tx::get32(config.data() + 4) == generation && config[16] == 7);
    assert(execute("radio 912525000 250000 8 5 2", true).response.find("Accepted") == 0);
    f.step();
    assert(f.mux.configurationGeneration() == generation && f.mux.currentConfiguration().sf == 7);
    LocalRadio queued;
    assert(queued.attach(f.mux));
    const uint8_t raw[] = {uint8_t(PAYLOAD_TYPE_RAW_CUSTOM << 2 | ROUTE_TYPE_DIRECT), 0};
    uint32_t job;
    assert(queued.queueTransmit(raw, sizeof(raw), 0, 5000, 0, job));
    mesh::QueuedTransmitResult result;
    assert(queued.pollQueuedResult(result) && result.state == queued_tx::ACCEPTED);
    assert(execute("tempradio 1 912525000 250000 8 5 2").response.find("Accepted") == 0);
    assert(execute("status").response.find("roles applied=") == 0);
    f.step(80);
    assert(f.mux.currentConfiguration().sf == 8);
    config = configGet();
    assert(queued_tx::get32(config.data() + 4) == generation + 1 && config[16] == 8);
    assert(queued.pollQueuedResult(result) && result.job == job &&
           result.state == queued_tx::FAILED && result.reason == queued_tx::STALE);
    queued.detach();
    f.step(150);
    assert(f.mux.currentConfiguration().sf == 7 && f.mux.configurationGeneration() == generation + 2);
    config = configGet();
    assert(queued_tx::get32(config.data() + 4) == generation + 2 && config[16] == 7);
    close(sockets[1]); f.mux.poll(kissServer);
    if (installCommands) {
      const std::string source = "function hello(name) reply('Hello '..name) end";
      upload(f, source, true, true, [&](const char *text) {
        const auto response = execute(text);
        assert(response.status == "200 OK");
        return response.response;
      });
      for (const auto &selection : {std::string("bot ") + std::string(64, '0'),
                                   std::string("timers bot ") + std::string(64, '0'),
                                   std::string("reminders caller 1") + std::string(63, '0')}) {
        assert(execute(("data export " + selection).c_str()).response.find("PENDING") == 0);
        f.step();
        const auto exported = execute("data status").response;
        assert(exported.find("EXPORTED ") == 0);
        const auto digest = exported.substr(9, 64), id = exported.substr(74, 16);
        std::vector<std::string> chunks;
        for (unsigned i = 0; i < 51; ++i) {
          const auto data = execute(("data read " + id + " " + std::to_string(i)).c_str()).response;
          assert(data.find("DATA ") == 0); chunks.push_back(data.substr(5));
        }
        assert(execute(("data begin " + id + " " + digest).c_str()).response.find("UPLOADING ") == 0);
        for (unsigned i = 0; i < chunks.size(); ++i)
          assert(execute(("data chunk " + id + " " + std::to_string(i) + " " + chunks[i]).c_str()).response.find("RECEIVED ") == 0);
        assert(execute(("data stage " + id).c_str()).response.find("PENDING ") == 0);
        f.step(); assert(execute("data status").response == "STAGED " + id);
        const bool scheduler = selection.find("bot ") != 0;
        if (scheduler) assert(execute(("data restore " + id).c_str()).response.find("Error:") == 0);
        assert(execute(("data restore " + id + (scheduler ? " no-rearm" : "")).c_str()).response.find("PENDING ") == 0);
        f.step(); assert(execute("data status").response.find("COMMITTED ") == 0);
      }
      Peer publicUser;
      const auto advert = publicUser.advert();
      f.mux.received(advert.data(), advert.size(), -90, 5); f.step();
      for (const auto &test : {std::pair<const char *, const char *>{"!hello slepp", "Hello slepp"},
                               {"!ping", "Pong"}}) {
        timeMs += 61000;
        const auto packet = publicUser.command(f.bot.publicKey(), test.first);
        f.radio.sent.clear();
        f.mux.received(packet.data(), packet.size(), -90, 5); f.step();
        assert(publicUser.replies(f.bot.publicKey(), f.radio) == std::vector<std::string>{test.second});
      }
      assert(execute("source rollback").response.find("Accepted") == 0);
      f.step(300);
      assert(execute("source status").response.find("active=3") != std::string::npos);
      puts("PASS authenticated HTTP named-handler installation, native encrypted Hello and protected Pong, rollback");
    }
    assert(request("/admin/logout", "", token).response == "Logged out");
    assert(request("/admin/command", "status", token).status == "403 Forbidden");
    puts("PASS authenticated web: session/login/logout, origin/body limits, shared backend and response-gated PHY");
  };
  authenticated_web();
  {
    BetaFixture f;
    assert(f.action(begin.c_str()).find("next=1") != std::string::npos);
    identity_test::failCommit = true;
    assert(f.action(chunk(1).c_str()).find("Error: chunk progress") == 0);
    identity_test::failCommit = false;
    assert(f.action(chunk(1).c_str()).find("next=2") != std::string::npos);
    for (size_t i = 2; i * 48 < source.size(); ++i)
      assert(f.action(chunk(i).c_str()).find("ACK ") == 0);
    assert(f.action(("source commit " + id).c_str()).find("Accepted") == 0);
    identity_test::failCommit = true;
    f.step(200);
    identity_test::failCommit = false;
    assert(f.action("source status").find("Error: activation journal") != std::string::npos);
    assert(f.action(("source commit " + id).c_str()).find("Accepted") == 0);
    f.step(300);
    assert(f.action("source status").find("durably saved and active") != std::string::npos);
    assert(f.action("source remove").find("Accepted") == 0);
    f.step(200);
    assert(f.action("source status").find("active=3") != std::string::npos);
  }
  authenticated_web(true);
}
#include "beta_review_cases.h"
static void assert_zero_hop_advert(const Radio &radio, const uint8_t key[32],
                                   const char *name, uint8_t type) {
  unsigned count = 0;
  for (const auto &wire : radio.sent) {
    mesh::Packet packet;
    assert(packet.readFrom(wire.data(), wire.size()));
    if (packet.getPayloadType() != PAYLOAD_TYPE_ADVERT) continue;
    assert(wire[0] == 0x12 && wire[1] == 0);
    assert(packet.isRouteDirect() && !packet.hasTransportCodes() &&
           packet.getPathHashCount() == 0 && !memcmp(packet.payload, key, 32));
    assert(packet.payload_len > 100 && queued_tx::get32(packet.payload + 32) > 0);
    AdvertDataParser advert(packet.payload + 100, packet.payload_len - 100);
    assert(advert.isValid() && advert.getType() == type && advert.hasName() &&
           !strcmp(advert.getName(), name));
    Bytes signedData(packet.payload, packet.payload + 36);
    signedData.insert(signedData.end(), packet.payload + 100, packet.payload + packet.payload_len);
    assert(mesh::Identity(key).verify(packet.payload + 36, signedData.data(), signedData.size()));
    ++count;
  }
  assert(count == 1);
}
static void service_names_without_app_roles() {
  const auto records = identity_test::durable;
  const auto files = filesystem_test::files;
  assert(saveRoleProfile({0}) && saveBotEnabled(false));
  mesh::LocalIdentity modem;
  assert(loadIdentity("modem", modem));
  uint8_t managementKey[32]{};
  for (unsigned boot = 0; boot < 2; ++boot) {
    Radio radio;
    HardwareRNG rng;
    WifiKissMultiplexer mux;
    mux.attachRadio(radio, rng, [](float, float, uint8_t, uint8_t) {}, [](uint8_t) {});
    assert(mux.setInitialConfiguration({912525000, 250000, 7, 5, 2}, true));
    assert(onchip::begin(mux, modem));
    const auto step = [&]() {
      for (unsigned i = 0; i < 200; ++i) {
        timeMs += 10; onchip::loop(); mux.serviceTransmit();
      }
    };
    step();
    auto *admin = MastAdmin::service();
    assert(admin && admin->ready());
    const auto action = [&](const char *text) {
      MastAdmin::Reply reply;
      admin->execute(text, reply);
      return std::string(reply.text);
    };
    RadioDashboard::RadioStatus status{};
    const auto dashboardName = [&](const std::string &expected) {
      onchip::dashboardStatus(status, false);
      RadioDashboard dashboard;
      RadioDashboard::Snapshot snapshot;
      char json[RadioDashboard::JSON_CAPACITY];
      assert(dashboard.publish(timeMs, status) && dashboard.snapshot(snapshot));
      assert(RadioDashboard::formatJSON(snapshot, "build-time fallback", json, sizeof(json)));
      assert(std::string(json).find("\"device_name\":\"" + expected + "\"") != std::string::npos);
    };
    onchip::dashboardStatus(status, false);
    assert(!status.wifi_connected && status.roles[5].ready);
    if (!boot) memcpy(managementKey, status.roles[5].public_key, 32);
    assert(!memcmp(managementKey, status.roles[5].public_key, 32));
    if (!boot) {
      const auto unchanged = identity_test::durable;
      const auto unchangedFiles = filesystem_test::files;
      assert(action("role name management Aspen-Admin").find("Saved and applied") == 0);
      assert(action("role name kiss Aspen-KISS").find("Saved and applied") == 0);
      dashboardName("Aspen-KISS");
      for (const auto &entry : unchanged)
        if (entry.first.second != "management-name" && entry.first.second != "kiss-name")
          assert(identity_test::durable.at(entry.first) == entry.second);
      assert(filesystem_test::files == unchangedFiles);
      for (const char *service : {"management", "kiss"}) {
        const auto before = action(("role name " + std::string(service)).c_str());
        const auto saved = identity_test::durable;
        for (unsigned failure = 0; failure < 3; ++failure) {
          identity_test::failWrite = failure == 0;
          identity_test::failCommit = failure == 1;
          identity_test::afterCommit = failure == 2 ? +[]() { identity_test::failRead = true; } : nullptr;
          assert(action(("role name " + std::string(service) + " Not Applied").c_str()).find("Error:") == 0);
          identity_test::failWrite = identity_test::failCommit = identity_test::failRead = false;
          identity_test::afterCommit = nullptr;
          assert(action(("role name " + std::string(service)).c_str()) == before);
          dashboardName("Aspen-KISS");
          identity_test::durable = saved;
        }
        assert(action(("role name " + std::string(service) + " bad:name").c_str()).find("Error:") == 0);
        assert(action(("role name " + std::string(service) + " " + std::string(32, 'x')).c_str()).find("Error:") == 0);
      }
      const std::string longest(31, 'x');
      assert(action(("role name kiss " + longest).c_str()).find("Saved") == 0);
      assert(action("role name kiss") == "Name: " + longest);
      dashboardName(longest);
      assert(action("role name kiss Aspen-KISS").find("Saved") == 0);
    }
    assert(action("role name management") == "Name: Aspen-Admin");
    assert(action("role name kiss") == "Name: Aspen-KISS");
    dashboardName("Aspen-KISS");
    assert(action("role config management").find("advert=zerohop RF=shared") != std::string::npos);
    assert(action("role config kiss").find("advert=none RF=service-only") != std::string::npos);
    for (const char *role : {"repeater", "room", "companion", "bot", "kiss", "observer"})
      assert(action(("role advert " + std::string(role) + " zerohop").c_str()).find("Error:") == 0);
    assert(action("role key management rotate").find("Error:") == 0);
    assert(action("role key kiss rotate").find("Error:") == 0);
    onchip::dashboardStatus(status, false);
    assert(!strcmp(status.roles[5].name, "Aspen-Admin") && !strcmp(status.roles[4].name, "Aspen-KISS"));
    assert(!memcmp(status.roles[4].public_key, modem.pub_key, 32));
    radio.sent.clear();
    assert(action("role advert management flood").find("Error:") == 0);
    assert(action("role advert management zerohop").find("Queued zero-hop") == 0);
    assert(action("role advert management zerohop").find("Error:") == 0);
    step();
    assert_zero_hop_advert(radio, managementKey, "Aspen-Admin", ADV_TYPE_REPEATER);
    const auto phy = mux.currentConfiguration();
    assert(phy.freq_hz == 912525000 && phy.bw_hz == 250000 && phy.sf == 7 && phy.cr == 5 && phy.tx_power == 2);
    stopManagementForTest();
    assert(psram_test::allocations.empty());
  }
  char name[32]{};
  const auto valid = identity_test::durable;
  identity_test::durable[{"mc-onchip", "management-name"}][3] = 2;
  assert(!loadServiceName(NamedService::Management, name));
  identity_test::durable = valid;
  identity_test::durable[{"mc-onchip", "kiss-name"}].resize(8);
  assert(!loadServiceName(NamedService::Kiss, name));
  identity_test::durable = records;
  filesystem_test::files = files;
  puts("PASS service names: live/readback/reboot, 31-byte bound, denied/failed writes, unchanged keys/policies/data/PHY; management advert without WiFi/app roles; no KISS/observer advert");
}
static void renamed_role_adverts() {
  const auto records = identity_test::durable;
  const auto files = filesystem_test::files;
  assert(saveRoleProfile({7}) && saveBotEnabled(true));
  beginClocks(RoleProfile(7));
  {
    BetaFixture f(true);
    const uint32_t started = millis();
    timeMs += 20000; f.step(300);
    bool botStartupAdvert = false;
    for (const auto &wire : f.radio.sent) {
      mesh::Packet packet;
      assert(packet.readFrom(wire.data(), wire.size()));
      if (packet.getPayloadType() == PAYLOAD_TYPE_ADVERT &&
          !memcmp(packet.payload, f.bot.publicKey(), 32))
        botStartupAdvert = packet.isRouteFlood();
    }
    assert(botStartupAdvert && !f.bot.advertise() && !f.bot.advertise(true));
    struct NamedRole { const char *role, *name, *key; uint8_t type; };
    const NamedRole roles[] = {
      {"repeater", "Aspen-Relay", "repeater", ADV_TYPE_REPEATER},
      {"room", "Aspen-Room", "room", ADV_TYPE_ROOM},
      {"companion", "Aspen-Base", "companion", ADV_TYPE_CHAT},
      {"bot", "Aspen-Bot", "command-bot", ADV_TYPE_CHAT},
      {"management", "Aspen-Admin", "management", ADV_TYPE_REPEATER},
    };
    Peer outsider, owner;
    assert(f.send(outsider, "role name management Unauthorized", false).empty());
    assert(f.send(outsider, "role name kiss Unauthorized", false).empty());
    assert(f.send(outsider, "role advert management zerohop", false).empty());
    assert(f.send(outsider, "role advert bot zerohop", false).empty());
    for (const auto &wire : f.radio.sent) {
      mesh::Packet packet;
      assert(packet.readFrom(wire.data(), wire.size()));
      assert(packet.getPayloadType() != PAYLOAD_TYPE_ADVERT);
    }
    assert(f.action("role name management") == std::string("Name: ") + f.management.name());
    assert(f.action("role name management") != "Name: Unauthorized");
    assert(f.action("role name kiss") != "Name: Unauthorized");
    assert(!f.send(owner, "mast-pass-12", true).empty());
    for (const auto &role : roles) {
      timeMs += 1000; f.step();
      const std::string rename = "role name " + std::string(role.role) + " " + role.name;
      const auto reply = f.send(owner, rename.c_str(), false);
      assert(reply.size() > 5 && std::string(reply.begin() + 5, reply.end()).find("Saved and applied") == 0);
      const std::string advertise = "role advert " + std::string(role.role) + " zerohop";
      const auto queued = f.send(owner, advertise.c_str(), false, true, 1, true, 300);
      assert(queued.size() > 5 && std::string(queued.begin() + 5, queued.end()).find("Queued zero-hop") == 0);
      uint8_t key[32];
      assert(identityPublicKey(role.key, key));
      assert_zero_hop_advert(f.radio, key, role.name, role.type);
    }
    assert(uint32_t(millis() - started) < 900000);
    assert(!f.bot.advertise() && !f.bot.advertise(true));
    f.radio.sent.clear();
    assert(f.action("role advert bot zerohop").find("Queued zero-hop") == 0);
    assert(f.action("role advert bot zerohop").find("Error:") == 0);
    f.step(300);
    assert_zero_hop_advert(f.radio, f.bot.publicKey(), "Aspen-Bot", ADV_TYPE_CHAT);
    BotRadioPolicy policy;
    assert(loadBotRadioPolicy(policy));
    f.radio.airtime = policy.airtimeMs + 1;
    assert(f.action("role advert bot zerohop").find("Error:") == 0);
    f.radio.airtime = 10;
    const char *source = "function announce() return advert() end";
    BotWorker::Result result;
    assert(f.bot.stageSource(source, strlen(source))); f.step();
    assert(f.bot.pollSourceResult(result) && result.ok && f.bot.activateStaged()); f.step();
    assert(f.bot.pollSourceResult(result) && result.ok);
    const auto contact = outsider.advert();
    f.mux.received(contact.data(), contact.size(), -90, 5); f.step();
    f.radio.sent.clear();
    const auto announce = outsider.command(f.bot.publicKey(), "!announce");
    f.mux.received(announce.data(), announce.size(), -90, 5); f.step(300);
    const auto replies = outsider.replies(f.bot.publicKey(), f.radio);
    assert(replies.size() == 1 && replies[0].find("advert unavailable") != std::string::npos);
    for (const auto &wire : f.radio.sent) {
      mesh::Packet packet;
      assert(packet.readFrom(wire.data(), wire.size()));
      assert(packet.getPayloadType() != PAYLOAD_TYPE_ADVERT);
    }
  }
  identity_test::durable = records;
  filesystem_test::files = files;
  puts("PASS authenticated native role rename/adverts: five identities, exact 0x12 wire/name/signatures; immediate owner bot rename advert after startup, public/Lua rate limit retained, queue/airtime failures");
}
static void runtime_role_configuration() {
  const auto records = identity_test::durable;
  const auto files = filesystem_test::files;
  std::map<std::string, std::string> rotated;
  assert(saveRoleProfile(RoleProfile(7)) && saveBotEnabled(true));
  {
    BetaFixture f(true);
    const auto phy = f.mux.currentConfiguration();
    for (const char *role : {"bot", "repeater", "room", "companion"}) {
      const auto key = f.action(("role key " + std::string(role)).c_str());
      assert(key.find("KEY ") == 0 && key.size() == 68);
      const auto name = "Aspen " + std::string(role);
      assert(f.action(("role name " + std::string(role) + " " + name).c_str()).find("Saved") == 0);
      assert(f.action(("role name " + std::string(role)).c_str()) == "Name: " + name);
      assert(f.action(("role key " + std::string(role)).c_str()) == key);
      assert(f.action(("role config " + std::string(role)).c_str()).find("RF=shared") != std::string::npos);
    }
    assert(f.action("role name repeater bad:name").find("Error:") == 0);
    assert(f.action("role key bot import deadbeef").find("no private key import/export") != std::string::npos);
    assert(f.action("role channel repeater 0 off").find("no application group-channel keys") != std::string::npos);
    assert(f.action("role channel room 0 off").find("no application group-channel keys") != std::string::npos);
    uint8_t channelKey[16];
    for (unsigned i = 0; i < sizeof(channelKey); ++i) channelKey[i] = uint8_t(i + 1);
    const std::string setChannel = " 4f7073 " + encode(channelKey, sizeof(channelKey));
    assert(f.action(("role channel companion 1" + setChannel).c_str()).find("Saved and applied") == 0);
    auto channel = f.action("role channel companion 1");
    assert(channel.find("name=Ops key-id=") != std::string::npos &&
           channel.find(encode(channelKey, sizeof(channelKey))) == std::string::npos);
    assert(f.action("role channel companion 99 off").find("Error:") == 0);
    assert(f.action("role channel companion 1 4f7073 bad").find("Error:") == 0);
    filesystem_test::failOpen = true;
    assert(f.action("role name companion Should Not Apply").find("Error:") == 0);
    assert(f.action("role channel companion 1 off").find("Error:") == 0);
    filesystem_test::failOpen = false;
    assert(f.action("role name companion") == "Name: Aspen companion");
    assert(f.action("role channel companion 1") == channel);
    for (Role role : {Role::Repeater, Role::Room, Role::Companion}) {
      assert(requestLifecycle(role, LifecycleAction::Reboot)); f.step();
      assert(f.action(("role name " + std::string(roleName(role))).c_str()) == "Name: Aspen " + std::string(roleName(role)));
    }
    assert(f.action("role channel companion 1") == channel);
    assert(f.action(("role channel bot 0" + setChannel).c_str()).find("reboot required") != std::string::npos);
    BotRadioPolicy radio;
    assert(loadBotRadioPolicy(radio) && radio.channelKeySet && !memcmp(radio.channelKey, channelKey, 16));
    radio.pathWidth = 3; assert(saveBotRadioPolicy(radio));
    f.bot.stop(); assert(f.bot.begin(f.mux)); f.step();
    assert(f.action("role name bot") == "Name: Aspen bot");
    Peer peer;
    const auto query = peer.targetedGroup(f.bot.publicKey(), "caller: !ping", "Ops", 3, {}, channelKey);
    f.radio.sent.clear(); f.mux.received(query.data(), query.size(), -91, 5); f.step();
    const auto replies = peer.groupReplies(f.radio, 3, "Ops", channelKey);
    assert(std::count(replies.begin(), replies.end(), "Pong") == 1);
    const auto botKey = f.action("role key bot");
    identity_test::failCommit = true;
    assert(f.action("role key bot rotate").find("Error:") == 0);
    identity_test::failCommit = false;
    assert(f.action("role key bot") == botKey);
    for (const char *role : {"bot", "repeater", "room", "companion"}) {
      const auto before = f.action(("role key " + std::string(role)).c_str());
      const auto pending = f.action(("role key " + std::string(role) + " rotate").c_str());
      assert(pending.find("Pending ") == 0 && pending.find("reboot required") != std::string::npos);
      assert(f.action(("role key " + std::string(role)).c_str()) == before);
      assert(f.action(("role key " + std::string(role) + " pending").c_str()) == pending);
      assert(f.action(("role key " + std::string(role) + " rotate").c_str()).find("Error:") == 0);
      if (!strcmp(role, "bot")) {
        f.bot.stop(); assert(f.bot.begin(f.mux)); f.step();
      } else {
        const Role native = !strcmp(role, "repeater") ? Role::Repeater :
                            !strcmp(role, "room") ? Role::Room : Role::Companion;
        assert(requestLifecycle(native, LifecycleAction::Reboot)); f.step();
      }
      assert(f.action(("role key " + std::string(role)).c_str()) == "KEY " + pending.substr(8, 64));
      assert(f.action(("role key " + std::string(role)).c_str()) != before);
      rotated[role] = "KEY " + pending.substr(8, 64);
      assert(f.action(("role name " + std::string(role)).c_str()) == "Name: Aspen " + std::string(role));
    }
    assert(f.action("role channel companion 1") == channel);
    assert(f.action("role channel companion 1 off").find("Saved") == 0);
    assert(f.action("role channel companion 1").find("name=off") != std::string::npos);
    const auto current = f.mux.currentConfiguration();
    assert(current.freq_hz == phy.freq_hz && current.bw_hz == phy.bw_hz &&
           current.sf == phy.sf && current.cr == phy.cr && current.tx_power == phy.tx_power);
  }
  {
    BetaFixture restarted(true);
    for (const auto &entry : rotated) {
      assert(restarted.action(("role key " + entry.first).c_str()) == entry.second);
      assert(restarted.action(("role name " + entry.first).c_str()) == "Name: Aspen " + entry.first);
    }
    assert(restarted.action("role channel companion 1").find("name=off") != std::string::npos);
    assert(restarted.action("role channel bot 0").find("name=Ops key-id=") != std::string::npos);
  }
  identity_test::durable = records;
  filesystem_test::files = files;
  puts("PASS runtime role configuration: persistent names, deliberate staged identity rotation, 128-bit bot/companion channels, restart/failure behavior, private-key non-export and unchanged shared PHY");
}
static void supplied_identity_keys() {
  const auto records = identity_test::durable;
  const auto files = filesystem_test::files;
  assert(saveRoleProfile(RoleProfile(7)) && saveBotEnabled(true));
  const char *roles[] = {"repeater", "room", "companion", "command-bot", "management"};
  std::map<std::string, std::string> imports, publicKeys, names;
  Peer owner, outsider;
  const std::string tag = "0123456789abcdef|";
  {
    BetaFixture f(true);
    assert(!f.send(owner, "mast-pass-12", true, true, 3).empty());
    const auto action = [&](const std::string &command) {
      const auto request = tag + command;
      assert(request.size() <= MastAdmin::TextLimit);
      const auto response = f.send(owner, request.c_str(), false, true, 3);
      assert(response.size() > 5 && response[4] == 4);
      const std::string text(response.begin() + 5, std::find(response.begin() + 5, response.end(), 0));
      assert(text.find(tag) == 0);
      return text.substr(tag.size());
    };
    assert(action("trust " + encode(owner.self_id.pub_key, 32)).find("Saved") == 0);
    const auto phy = identity_test::durable.at({"mesh-phy", "profile"});
    const auto beforeDuplicates = identity_test::durable;
    const std::string duplicate = "Error: public identity already active or pending for another role; no change";
    for (const char *target : roles) for (const char *source : roles) {
      if (!strcmp(target, source)) continue;
      const auto &raw = beforeDuplicates.at({"mc-onchip", source});
      assert(raw.size() == PRV_KEY_SIZE);
      const std::string wireRole = !strcmp(target, "command-bot") ? "bot" : target;
      assert(action("key " + wireRole + " " + encode(raw.data(), raw.size())) == duplicate);
    }
    assert(identity_test::durable.size() == beforeDuplicates.size());
    for (const auto &record : beforeDuplicates)
      if (record.first != std::make_pair(std::string("mc-mast-admin"), std::string("replay")))
        assert(identity_test::durable.at(record.first) == record.second);
    for (const char *role : roles) {
      mesh::LocalIdentity replacement(&f.rng);
      uint8_t raw[PRV_KEY_SIZE];
      replacement.writeTo(raw, sizeof(raw));
      imports[role] = encode(raw, sizeof(raw));
      publicKeys[role] = encode(replacement.pub_key, 32);
      names[role] = action("role name " + std::string(role));
      const std::string wireRole = !strcmp(role, "command-bot") ? "bot" : role;
      const auto command = "key " + wireRole + " " + imports[role];
      assert(tag.size() + command.size() <= MAX_TEXT_LEN);
      if (!strcmp(role, "management")) {
        NativeChat sender(3);
        ContactInfo contact{};
        contact.id = mesh::Identity(f.management.publicKey());
        contact.type = ADV_TYPE_REPEATER; contact.out_path_len = OUT_PATH_UNKNOWN;
        uint32_t timeout = 0;
        const auto text = tag + command;
        assert(text.size() == MAX_TEXT_LEN);
        assert(sender.sendCommandData(contact, owner.timestamp + 1, 0, (text + "x").c_str(), timeout) == MSG_SEND_FAILED);
        assert(sender.sendCommandData(contact, owner.timestamp + 1, 0, text.c_str(), timeout) == MSG_SEND_SENT_FLOOD);
        for (unsigned i = 0; i < 20; ++i) { timeMs += 10; sender.loop(); }
        assert(sender.radio.sent.size() == 1);
        mesh::Packet packet;
        assert(packet.readFrom(sender.radio.sent[0].data(), sender.radio.sent[0].size()) &&
               packet.payload_len == 180 && packet.getPathHashSize() == 3);
        uint8_t secret[32], plain[184];
        sender.self_id.calcSharedSecret(secret, contact.id.pub_key);
        assert(mesh::Utils::MACThenDecrypt(secret, plain, packet.payload + 2, packet.payload_len - 2) == 176);
        assert(!memcmp(plain + 5, text.data(), text.size()));
      }
      const auto active = action("key " + std::string(role));
      const auto before = identity_test::durable;
      assert(f.send(outsider, command.c_str(), false, true, 3).empty());
      assert(f.action(command.c_str()).find("encrypted Management RF") != std::string::npos);
      assert(identity_test::durable == before);
      if (strcmp(role, "repeater"))
        assert(action("key " + wireRole + " " + imports["repeater"]) == duplicate);
      const auto staged = action(command);
      assert(staged == "Pending " + publicKeys[role] + "; reboot required; peers must learn new key");
      assert(staged.find(imports[role]) == std::string::npos);
      for (const auto &record : before)
        if (record.first != std::make_pair(std::string("mc-onchip"), std::string(role)) &&
            record.first != std::make_pair(std::string("mc-mast-admin"), std::string("replay")))
          assert(identity_test::durable.at(record.first) == record.second);
      assert(action("key " + std::string(role)) == active);
      assert(action("key " + std::string(role) + " pending") == staged);
      const auto journal = identity_test::durable.at({"mc-onchip", role});
      assert(journal.size() == 133 && action(command) == staged);
      assert(identity_test::durable.at({"mc-onchip", role}) == journal);
      mesh::LocalIdentity different(&f.rng);
      different.writeTo(raw, sizeof(raw));
      assert(action("key " + wireRole + " " + encode(raw, sizeof(raw))).find("different pending identity") != std::string::npos);
      assert(identity_test::durable.at({"mc-onchip", role}) == journal);
    }
    assert(action("key observer " + imports["command-bot"]).find("Error:") == 0);
    assert(action("key kiss " + imports["command-bot"]).find("Error:") == 0);
    assert(action("key bot 00").find("128 hex digits") != std::string::npos);
    assert(action("key bot " + std::string(128, '0')).find("invalid private identity") != std::string::npos);
    assert(action("key management export").find("Error:") == 0);
    assert(action("key repeater cancel").find("no pending change; running key unchanged") != std::string::npos);
    assert(action("key repeater pending").find("Error:") == 0);
    assert(action("key repeater " + imports["repeater"]).find("Pending " + publicKeys["repeater"]) == 0);
    for (const char *role : roles) assert(action("role name " + std::string(role)) == names[role]);
    assert(identity_test::durable.at({"mesh-phy", "profile"}) == phy);
  }
  {
    BetaFixture restarted(true);
    assert(restarted.management.admin().trusted(owner.self_id.pub_key) &&
           restarted.management.admin().lastTimestamp(owner.self_id.pub_key) == owner.timestamp);
    assert(!restarted.send(owner, "", true, true, 3).empty());
    assert(encode(restarted.management.publicKey(), 32) == publicKeys["management"]);
    assert(encode(restarted.bot.publicKey(), 32) == publicKeys["command-bot"]);
    for (const char *role : roles) {
      const auto response = restarted.send(owner, ("key " + std::string(role)).c_str(), false, true, 3);
      assert(response.size() > 5);
      assert(std::string(response.begin() + 5, response.end()).find("KEY " + publicKeys[role]) == 0);
      assert(restarted.action(("role name " + std::string(role)).c_str()) == names[role]);
      const auto identity = identity_test::durable.at({"mc-onchip", role});
      assert(identity.size() == PRV_KEY_SIZE && encode(identity.data(), identity.size()) == imports[role]);
    }
    const auto response = restarted.send(owner, ("key management " + imports["management"]).c_str(), false, true, 3);
    assert(std::string(response.begin() + 5, response.end()).find("already active; no reboot required") != std::string::npos);
    assert(restarted.action("role config management").find("key=import-stage/reboot") != std::string::npos);
  }
  identity_test::durable = records;
  filesystem_test::files = files;
  puts("PASS supplied identity keys: encrypted owner RF only, all five native keys, 16-char nonce/three-byte paths, public-only readback, cross-role active/pending duplicate denial, idempotent staging, conflict/cancel, unchanged names/PHY/other records, restart and retained owner trust/replay");
}
static Bytes native_role_exchange(BetaFixture &f, Peer &peer, const char *role, const char *text,
                                 bool login, uint8_t requestType = 0, bool flood = true,
                                 uint8_t width = 3, uint8_t count = 0) {
  const auto key = f.action(("key " + std::string(role)).c_str()).substr(4, 64);
  uint8_t target[32], secret[32];
  for (unsigned i = 0; i < 32; ++i) target[i] = std::stoul(key.substr(i * 2, 2), nullptr, 16);
  peer.self_id.calcSharedSecret(secret, target);
  Bytes data(login ? (!strcmp(role, "room") ? 8 : 4) : 5);
  queued_tx::put32(data.data(), ++peer.timestamp);
  if (!login) data[4] = requestType ? requestType : 4;
  data.insert(data.end(), text, text + strlen(text) + 1);
  auto *packet = login ? peer.createAnonDatagram(PAYLOAD_TYPE_ANON_REQ, peer.self_id,
      mesh::Identity(target), secret, data.data(), data.size()) :
      peer.createDatagram(requestType ? PAYLOAD_TYPE_REQ : PAYLOAD_TYPE_TXT_MSG,
                          mesh::Identity(target), secret, data.data(), data.size());
  assert(packet);
  packet->header |= flood ? ROUTE_TYPE_FLOOD : ROUTE_TYPE_DIRECT;
  packet->setPathHashSizeAndCount(width, count);
  for (unsigned i = 0; i < unsigned(width) * count; ++i) packet->path[i] = 0x11 + i;
  const auto wire = peer.wire(packet);
  f.radio.sent.clear(); f.mux.received(wire.data(), wire.size(), -90, 5); f.step(200);
  for (const auto &raw : f.radio.sent) {
    mesh::Packet response;
    assert(response.readFrom(raw.data(), raw.size()));
    if ((response.getPayloadType() != PAYLOAD_TYPE_PATH &&
         response.getPayloadType() != PAYLOAD_TYPE_TXT_MSG &&
         response.getPayloadType() != PAYLOAD_TYPE_RESPONSE) ||
        response.payload[0] != peer.self_id.pub_key[0] || response.payload[1] != target[0]) continue;
    uint8_t plain[184]{};
    const auto size = mesh::Utils::MACThenDecrypt(secret, plain, response.payload + 2,
                                                response.payload_len - 2);
    if (!size) continue;
    assert(response.getPayloadVer() == PAYLOAD_VER_1 && response.payload_len <= 184);
    const size_t offset = response.getPayloadType() == PAYLOAD_TYPE_PATH ?
        2 + (plain[0] & 63) * ((plain[0] >> 6) + 1) : 0;
    assert(offset < size);
    return Bytes(plain + offset, plain + size);
  }
  return {};
}
static Bytes role_password_login(BetaFixture &f, Peer &peer, const char *role, const char *password) {
  return native_role_exchange(f, peer, role, password, true);
}
static void native_role_profile_owner_info() {
  const auto durable = identity_test::durable;
  const auto files = filesystem_test::files;
  {
    BetaFixture f(true);
    Peer peer;
    assert(!role_password_login(f, peer, "repeater", "test-admin").empty());
    const auto cli = [&](const std::string &text) {
      const auto body = native_role_exchange(f, peer, "repeater", text.c_str(), false);
      assert(body.size() > 5 && body[4] == 4);
      return std::string(body.begin() + 5, std::find(body.begin() + 5, body.end(), 0));
    };
    assert(cli("ver").find("v" MESHCORE_SLP_ASPEN_VERSION) == 0);
    assert(cli("set name " + std::string(31, 'n')).find("Error") == std::string::npos);
    assert(cli("set owner.info " + std::string(119, 'x')).find("Error") == std::string::npos);
    const std::string expected = MESHCORE_SLP_ASPEN_VERSION "\n" + std::string(31, 'n') + "\n" + std::string(119, 'x');
    const auto direct = native_role_exchange(f, peer, "repeater", "", false, 7, false);
    assert(direct.size() > 4 && std::string(direct.begin() + 4, std::find(direct.begin() + 4, direct.end(), 0)) == expected.substr(0, 163));
    for (uint8_t width : {1, 2, 3}) {
      const auto body = native_role_exchange(f, peer, "repeater", "", false, 7, true, width, 3);
      assert(body.size() > 4 && queued_tx::get32(body.data()) == peer.timestamp);
      const std::string text(body.begin() + 4, std::find(body.begin() + 4, body.end(), 0));
      assert(expected.find(text) == 0 && text.size() == 157u - 3u * width);
    }
    assert(cli("get owner.info") == "> " + std::string(119, 'x'));
  }
  identity_test::durable = durable; filesystem_test::files = files;
  puts("PASS native Relay profile and owner-info: bounded direct and 1/2/3-byte routed replies, stored owner text unchanged");
}
static void runtime_role_passwords() {
  const auto baseline = identity_test::durable;
  const auto files = filesystem_test::files;
  const char password[] = "role-recover-15";
  static_assert(sizeof(password) == 16, "Native 15-byte password maximum");
  const auto encoded = encode(reinterpret_cast<const uint8_t *>(password), strlen(password));
  Peer owner, outsider, roleAdmin;
  std::map<std::string, std::string> keys, names;
  {
    BetaFixture f(true);
    for (const char *role : {"repeater", "room", "companion", "management", "bot"}) {
      keys[role] = f.action(("key " + std::string(role)).c_str());
      names[role] = f.action(("role name " + std::string(role)).c_str());
    }
    for (const auto role : {Role::Repeater, Role::Room}) {
      char name[32];
      assert(nativeRoleName(role, name) && setNativeRoleName(role, name));
    }
    for (const char *role : {"repeater", "room"}) {
      const auto login = role_password_login(f, roleAdmin, role, "test-admin");
      assert(login.size() >= 13 && login[6] == 1 && (login[7] & 3) == 3);
    }
    assert(repeaterFlush() && roomFlush());
    const auto command = "role password repeater " + encoded;
    const auto configurationFiles = [] {
      auto records = filesystem_test::files;
      // Signed role adverts can independently update the companion contact cache.
      records.erase("/companion/contacts3");
      for (auto entry = records.begin(); entry != records.end();)
        if (entry->first.find("/companion/bl/") == 0) entry = records.erase(entry);
        else ++entry;
      return records;
    };
    const auto saved = configurationFiles();
    assert(f.send(outsider, command.c_str(), false, true, 3).empty());
    assert(f.action(command.c_str()).find("authenticated encrypted Management RF") != std::string::npos);
    assert(f.action(("trust " + encode(outsider.self_id.pub_key, 32)).c_str()).find("Saved") == 0);
    MastAdmin::Reply reply;
    f.management.admin().execute(command.c_str(), reply, 0, MastAdmin::Transport::NativeEncrypted);
    assert(strstr(reply.text, "authenticated encrypted"));
    f.management.admin().execute(command.c_str(), reply, 0, MastAdmin::Transport::NativeEncrypted,
                                outsider.self_id.pub_key);
    assert(strstr(reply.text, "authenticated encrypted"));
    assert(configurationFiles() == saved);
    assert(!f.send(owner, "mast-pass-12", true, true, 3).empty());
    const auto action = [&](const std::string &text) {
      const auto response = f.send(owner, text.c_str(), false, true, 3);
      assert(response.size() > 5 && response[4] == 4);
      return std::string(response.begin() + 5, std::find(response.begin() + 5, response.end(), 0));
    };
    f.management.admin().execute(command.c_str(), reply, 1, MastAdmin::Transport::NativeEncrypted,
                                owner.self_id.pub_key);
    assert(strstr(reply.text, "web/Lua denied"));
    f.management.admin().execute(command.c_str(), reply, 0, MastAdmin::Transport::Other,
                                owner.self_id.pub_key);
    assert(strstr(reply.text, "web/Lua denied"));
    for (const auto &invalid : {"role password bot 61", "role password companion 61",
         "role password management 61", "role password kiss 61", "role password unknown 61",
         "role password repeater", "role password repeater 0", "role password repeater zz",
         "role password repeater 00", "role password repeater 1f", "role password repeater 7f",
         "role password repeater ff", "role password repeater 610062",
         "role password repeater 61616161616161616161616161616161",
         "role password repeater 61 extra"}) {
      assert(action(invalid).find("Error:") == 0);
      assert(configurationFiles() == saved);
    }
    const auto phy = identity_test::durable.at({"mesh-phy", "profile"});
    const auto generation = f.mux.configurationGeneration();
    const auto trust = f.action("auth status");
    const auto pathPolicy = f.action("role-path");
    const auto channel = f.action("role channel companion 0");
    for (const char *role : {"repeater", "room"}) {
      for (const char *candidate : {"a", "! \\\"|~"}) {
        const auto hexPassword = encode(reinterpret_cast<const uint8_t *>(candidate), strlen(candidate));
        assert(action("role password " + std::string(role) + " " + hexPassword).find("Saved and applied") == 0);
        Peer printable;
        const auto login = role_password_login(f, printable, role, candidate);
        assert(login.size() >= 13 && login[6] == 1 && (login[7] & 3) == 3);
      }
      assert(action("role password " + std::string(role) + " 746573742d61646d696e").find("Saved and applied") == 0);
      const std::string path = "/" + std::string(role) + "/prefs.json";
      const auto before = configurationFiles();
      const auto result = action("role password " + std::string(role) + " " + encoded);
      assert(result == "Saved and applied " + std::string(role) + " administrator password; ACL/sessions unchanged");
      assert(result.find(password) == std::string::npos && result.find(encoded) == std::string::npos);
      const auto normalize = [](const Bytes &bytes) {
        std::string text(bytes.begin(), bytes.end());
        const auto start = text.find("pass:\"");
        assert(start != std::string::npos);
        const auto end = text.find('"', start + 6);
        assert(end != std::string::npos);
        text.replace(start + 6, end - start - 6, "<redacted>");
        return text;
      };
      assert(normalize(filesystem_test::files.at(path)) == normalize(before.at(path)));
      for (const auto &file : before)
        if (file.first != path) assert(filesystem_test::files.at(file.first) == file.second);
      assert(roomGuestAccess() == 0 && MastAdmin::passwordMatches("mast-pass-12"));
      // Native password changes retain existing administrator ACL admissions.
      auto login = role_password_login(f, roleAdmin, role, "");
      assert(login.size() >= 13 && login[6] == 1 && (login[7] & 3) == 3);
      Peer newAdmin;
      login = role_password_login(f, newAdmin, role, password);
      assert(login.size() >= 13 && login[6] == 1 && (login[7] & 3) == 3);
      Peer oldPassword;
      login = role_password_login(f, oldPassword, role, "test-admin");
      assert(login.empty() || (login.size() >= 13 && login[6] != 1 && (login[7] & 3) != 3));
      const auto committed = filesystem_test::files;
      for (unsigned failure = 0; failure < 3; ++failure) {
        filesystem_test::failOpen = failure == 0;
        filesystem_test::writeLimit = failure == 1 ? 1 : std::numeric_limits<size_t>::max();
        filesystem_test::afterWrite = failure == 2 ? +[] { filesystem_test::readLimit = 0; } : nullptr;
        const auto result = action("role password " + std::string(role) + " 7265706c616365");
        assert(result.find("persistence unknown; live unchanged; saved may differ") != std::string::npos);
        filesystem_test::failOpen = false; filesystem_test::afterWrite = nullptr;
        filesystem_test::writeLimit = std::numeric_limits<size_t>::max();
        filesystem_test::readLimit = std::numeric_limits<size_t>::max();
        Peer unchangedLive;
        login = role_password_login(f, unchangedLive, role, password);
        assert(login.size() >= 13 && login[6] == 1 && (login[7] & 3) == 3);
        filesystem_test::files = committed;
      }
    }
    for (const auto &key : keys) assert(f.action(("key " + key.first).c_str()) == key.second);
    for (const auto &name : names) assert(f.action(("role name " + name.first).c_str()) == name.second);
    assert(identity_test::durable.at({"mesh-phy", "profile"}) == phy &&
           f.mux.configurationGeneration() == generation && f.action("auth status") == trust &&
           f.action("role-path") == pathPolicy && f.action("role channel companion 0") == channel);
    timeMs += 900001;
    f.management.admin().execute(command.c_str(), reply, 0, MastAdmin::Transport::NativeEncrypted,
                                owner.self_id.pub_key);
    assert(strstr(reply.text, "authenticated encrypted"));
  }
  {
    BetaFixture restarted(true);
    for (const char *role : {"repeater", "room"}) {
      Peer newAdmin;
      const auto login = role_password_login(restarted, newAdmin, role, password);
      assert(login.size() >= 13 && login[6] == 1 && (login[7] & 3) == 3);
      assert(restarted.action(("key " + std::string(role)).c_str()) == keys[role]);
    }
    assert(roomGuestAccess() == 0 && MastAdmin::passwordMatches("mast-pass-12"));
    Peer guest;
    const auto login = role_password_login(restarted, guest, "room", "test-room");
    assert(login.size() >= 13 && login[6] != 1 && (login[7] & 3) == 2);
  }
  {
    BetaFixture inactive;
    assert(!inactive.send(owner, "mast-pass-12", true, true, 3).empty());
    const auto saved = filesystem_test::files;
    const auto response = inactive.send(owner, ("role password room " + encoded).c_str(), false, true, 3);
    assert(response.size() > 5 &&
           std::string(response.begin() + 5, response.end()).find("Error: role inactive/busy; password unchanged") == 0);
    assert(filesystem_test::files == saved);
  }
  identity_test::durable = baseline; filesystem_test::files = files;
  puts("PASS role administrator recovery: authenticated encrypted Management dispatcher, web/source denial, native 15-byte limits, live and restart login, unchanged preferences/guest/keys/PHY/ACL, expired sender denial and truthful IO uncertainty");
}
static void runtime_management_password() {
  const auto baseline = identity_test::durable;
  const auto files = filesystem_test::files;
  const char updated[] = "new-mast-pass15";
  static_assert(sizeof(updated) == 16, "Exercise the native password maximum");
  const auto command = "password " + encode(reinterpret_cast<const uint8_t *>(updated), strlen(updated));
  Peer owner, stranger;
  {
    BetaFixture f;
    assert(f.action("password").find("source=compiled") != std::string::npos);
    assert(MastAdmin::passwordMatches("mast-pass-12") && !MastAdmin::passwordMatches(""));
    const auto before = identity_test::durable;
    assert(f.send(stranger, command.c_str(), false, true, 3).empty());
    assert(f.action(command.c_str()).find("encrypted Management RF") != std::string::npos);
    assert(identity_test::durable == before);
    assert(!f.send(owner, "mast-pass-12", true, true, 3).empty());
    const auto action = [&](const std::string &text) {
      const auto response = f.send(owner, ("0123456789abcdef|" + text).c_str(), false, true, 3);
      assert(response.size() > 22 && response[4] == 4);
      const std::string body(response.begin() + 5, std::find(response.begin() + 5, response.end(), 0));
      assert(body.find("0123456789abcdef|") == 0);
      return body.substr(17);
    };
    assert(action("trust " + encode(owner.self_id.pub_key, 32)).find("Saved") == 0);
    for (const auto &argument : {"0", "zz", "00", "1f", "610062", "ffffffff", "61616161616161616161616161616161aa"}) {
      assert(action("password " + std::string(argument)).find("Error: password needs") == 0);
      assert(MastAdmin::passwordMatches("mast-pass-12"));
    }
    const auto original = identity_test::durable;
    const auto entries = identity_test::usedEntries();
    const auto reply = action(command);
    assert(reply.find("Saved password; applies to new logins") == 0 &&
           reply.find(updated) == std::string::npos && reply.find(command.substr(9)) == std::string::npos);
    assert(identity_test::usedEntries() == entries + 4);
    assert(identity_test::durable.at({"mc-mast-admin", "password"}).size() == 52);
    for (const auto &record : original)
      if (record.first != std::make_pair(std::string("mc-mast-admin"), std::string("replay")))
        assert(identity_test::durable.at(record.first) == record.second);
    assert(!MastAdmin::passwordMatches("mast-pass-12") && MastAdmin::passwordMatches(updated));
    assert(action("password").find("source=runtime") != std::string::npos);
    assert(action("password 61").find("Saved password") == 0 && MastAdmin::passwordMatches("a"));
    assert(action(command).find("Saved password") == 0 && MastAdmin::passwordMatches(updated));
    assert(!MastAdmin::passwordMatches("new-mast-pass1"));
    assert(!f.send(owner, "", true, true, 3).empty());
    assert(f.send(stranger, "mast-pass-12", true, true, 3).empty());
    assert(!f.send(stranger, updated, true, true, 3).empty());
    const auto saved = identity_test::durable;
    for (unsigned cut = 0; cut < 4; ++cut) {
      identity_test::durable = saved;
      identity_test::failWrite = cut == 0;
      identity_test::failCommit = cut == 1 || cut == 2;
      identity_test::eagerWrites = cut == 2;
      identity_test::afterCommit = cut == 3 ? +[] { identity_test::failRead = true; } : nullptr;
      MastAdmin::Reply result;
      f.management.admin().execute("password 7265636f76657279", result, 0, MastAdmin::Transport::NativeEncrypted);
      assert(strstr(result.text, "commit/readback unknown"));
      identity_test::failWrite = identity_test::failCommit = identity_test::failRead = false;
      identity_test::eagerWrites = false; identity_test::afterCommit = nullptr;
      assert(MastAdmin::passwordMatches(cut >= 2 ? "recovery" : updated));
      assert(!MastAdmin::passwordMatches("mast-pass-12"));
    }
    identity_test::durable = saved;
  }
  {
    BetaFixture restarted;
    assert(MastAdmin::passwordMatches(updated) && !MastAdmin::passwordMatches("mast-pass-12"));
    assert(restarted.send(owner, "status", false, true, 3).empty());
    assert(restarted.send(stranger, "mast-pass-12", true, true, 3).empty());
    assert(!restarted.send(owner, "", true, true, 3).empty());
    assert(!restarted.send(stranger, updated, true, true, 3).empty());
    TestHTTPServer server;
    assert(registerMastWeb(&server) == ESP_OK);
    const auto webLogin = [&](const char *password) {
      timeMs += 1100;
      httpd_req_t request;
      request.body = password; request.content_len = request.body.size();
      request.headers["Host"] = "mast.test";
      assert(server.routes.at("/admin/login").handler(&request) == ESP_OK);
      return request;
    };
    assert(webLogin("mast-pass-12").status == "403 Forbidden");
    assert(webLogin(updated).status == "200 OK");
    const auto valid = identity_test::durable.at({"mc-mast-admin", "password"});
    for (unsigned malformed = 0; malformed < 3; ++malformed) {
      auto &record = identity_test::durable.at({"mc-mast-admin", "password"});
      record = valid;
      if (malformed == 0) record[4] ^= 1;
      else if (malformed == 1) record[3] = 2;
      else record.pop_back();
      assert(!MastAdmin::passwordMatches(updated) && !MastAdmin::passwordMatches("mast-pass-12"));
      assert(restarted.action("password").find("Error: password record unavailable") == 0);
      assert(!restarted.send(owner, "", true, true, 3).empty());
      const auto repaired = restarted.send(owner, command.c_str(), false, true, 3);
      assert(repaired.size() > 5 &&
             std::string(repaired.begin() + 5, repaired.end()).find("Saved password") == 0);
      assert(MastAdmin::passwordMatches(updated));
    }
    identity_test::failRead = true;
    assert(!MastAdmin::passwordMatches(updated) && !MastAdmin::passwordMatches("mast-pass-12"));
    identity_test::failRead = false;
  }
  identity_test::durable = baseline;
  filesystem_test::files = files;
  puts("PASS runtime Management password: encrypted owner RF only, shared RF/web login, checked 52-byte record, no fallback on corruption, trusted-key recovery, write/readback uncertainty, reboot and unrelated credentials preserved");
}
static void bot_facing_owner_admin() {
  BotRadioPolicy originalRadio; assert(loadBotRadioPolicy(originalRadio));
  auto radio = originalRadio; radio.airtimeMs = 3600; assert(saveBotRadioPolicy(radio));
  BetaFixture f;
  Peer owner, outsider;
  for (auto *peer : {&owner, &outsider}) {
    const auto advert = peer->advert();
    f.mux.received(advert.data(), advert.size(), -91, 6); f.step(15);
  }
  const auto command = [&](Peer &peer, const char *text) {
    timeMs += 61000; f.radio.sent.clear();
    const auto wire = peer.command(f.bot.publicKey(), text, 3, {0x31, 0x32, 0x33});
    f.mux.received(wire.data(), wire.size(), -91, 6); f.step();
    const auto replies = peer.replies(f.bot.publicKey(), f.radio);
    assert(replies.size() == 1); return replies[0];
  };
  assert(command(outsider, "!admin bot status").find("not granted") != std::string::npos);
  assert(command(outsider, "!admin bot discovery on").find("not granted") != std::string::npos);
  assert(f.action("bot discovery").find("saved=0 live=0") != std::string::npos);
  assert(command(outsider, "!admin role name management Unauthorized").find("not granted") != std::string::npos);
  assert(command(outsider, "!admin role advert management zerohop").find("not granted") != std::string::npos);
  assert(f.action(("trust " + encode(owner.self_id.pub_key, 32)).c_str()).find("Saved") == 0);
  assert(command(owner, "!admin bot status").find("bot applied=1") == 0);
  const auto grants = identity_test::durable;
  assert(command(owner, "!admin bot discovery on").find("Saved and applied") == 0);
  assert(f.action("bot discovery").find("saved=1 live=1") != std::string::npos);
  for (const auto &entry : grants)
    if (entry.first.first != "mc-mast-admin" &&
        entry.first != std::make_pair(std::string("mc-onchip"), std::string("bot-discovery")))
      assert(identity_test::durable.at(entry.first) == entry.second);
  assert(f.action("bot discovery invalid").find("Error:") == 0);
  assert(command(owner, "!admin bot discovery off").find("Saved and applied") == 0);
  const auto limitsReport = command(owner, "!admin bot limits");
  assert(limitsReport.find("Cooldown 0s; no command/min cap") == 0);
  assert(limitsReport.find("VM load/init/active 330/50/20ms") != std::string::npos);
  assert(command(owner, "!admin bot contention").find("best-effort") != std::string::npos);
  assert(command(owner, "!admin bot log").find("Current state=") == 0);
  assert(command(owner, "!admin role config bot").find("RF=shared") != std::string::npos);
  assert(command(owner, "!admin role advert management zerohop").find("Queued zero-hop") == 0);
  assert(command(owner, "!admin role key bot").find("KEY ") == 0);
  const auto credentialFiles = filesystem_test::files;
  assert(command(owner, "!admin role password repeater 736563726574").find("bot programs denied") != std::string::npos);
  assert(command(owner, "!admin role password room 736563726574").find("bot programs denied") != std::string::npos);
  assert(filesystem_test::files == credentialFiles);
  assert(command(outsider, "!admin role key bot rotate").find("not granted") != std::string::npos);
  assert(command(owner, "!admin role channel bot 0 off").find("native management CLI/web") != std::string::npos);
  assert(command(owner, "!admin wifi status").find("native management CLI/web") != std::string::npos);
  BotMeshPolicy original; assert(loadBotMeshPolicy(original));
  const auto name = command(owner, "!admin bot name Maple Bot");
  assert(name.find("Saved and applied") == 0);
  BotNodeSnapshot snapshot; f.bot.nodeSnapshot(snapshot);
  assert(!strcmp(snapshot.name, "Maple Bot"));
  assert(command(owner, "!admin bot destination 1 bad-key").find("Error:") == 0);
  assert(command(owner, ("!admin bot destination 1 " + std::string(64, '0')).c_str()).find("Error:") == 0);
  const auto grant = "!admin bot destination 1 " + encode(outsider.self_id.pub_key, 32);
  assert(command(owner, grant.c_str()).find("Saved and applied") == 0);
  assert(command(owner, "!admin bot mesh").find("destinations=1/4") != std::string::npos);
  assert(command(owner, "!admin bot destination 1 off").find("Saved and applied") == 0);
  const char *source = "function waiting() sleep(30000) return 'should not finish' end";
  BotWorker::Result staged;
  assert(f.bot.stageSource(source, strlen(source))); f.step();
  assert(f.bot.pollSourceResult(staged) && staged.ok && f.bot.activateStaged()); f.step();
  assert(f.bot.pollSourceResult(staged) && staged.ok);
  timeMs += 61000;
  const auto waiting = outsider.command(f.bot.publicKey(), "!waiting");
  f.mux.received(waiting.data(), waiting.size(), -91, 6); f.step(10);
  f.radio.sent.clear();
  const auto cancel = owner.command(f.bot.publicKey(), "!admin bot cancel");
  f.mux.received(cancel.data(), cancel.size(), -91, 6); f.step();
  const auto accepted = owner.replies(f.bot.publicKey(), f.radio);
  assert(accepted.size() == 1 && accepted[0].find("Cancellation requested") == 0);
  const auto cancelled = outsider.replies(f.bot.publicKey(), f.radio);
  assert(cancelled.size() == 1 && cancelled[0].find("cancelled") != std::string::npos);
  timeMs += 121000; f.radio.sent.clear();
  f.mux.received(cancel.data(), cancel.size(), -91, 6); f.step();
  const auto replay = owner.replies(f.bot.publicKey(), f.radio);
  assert(replay.size() == 1 && replay[0].find("replay/old timestamp") != std::string::npos);
  timeMs += 61000; f.radio.sent.clear(); f.radio.airtime = 1000; f.radio.holdTx = true;
  const auto reboot = owner.command(f.bot.publicKey(), "!admin reboot", 3, {}, false);
  f.mux.received(reboot.data(), reboot.size(), -91, 6); f.step(80);
  const auto gated = f.action("job");
  if (gated.find("waiting for acceptance reply") != 0) fprintf(stderr, "Bot reboot before TX: %s\n", gated.c_str());
  assert(gated.find("waiting for acceptance reply") == 0);
  f.radio.holdTx = false; f.step();
  assert(f.action("job") == "Reboot requested");
  assert(f.bot.setMeshPolicy(original) && saveBotRadioPolicy(originalRadio));
  assert(f.action(("auth forget " + encode(owner.self_id.pub_key, 32)).c_str()).find("Forgot") == 0);
  assert(f.action("trust none").find("Saved") == 0);
  puts("PASS native bot administration: existing owner trust/replay backend, policy/name/destinations, denied public/credential operations, cancellation and TX-gated reboot");
}
static void cross_runtime_source_credentials() {
  const auto durable = identity_test::durable;
  const auto files = filesystem_test::files;
  {
    BetaFixture f(true);
    Peer owner, outsider, roleAdmin;
    for (auto *peer : {&owner, &outsider}) {
      const auto advert = peer->advert();
      f.mux.received(advert.data(), advert.size(), -91, 6); f.step(15);
    }
    assert(f.action(("trust " + encode(owner.self_id.pub_key, 32)).c_str()).find("Saved") == 0);
    assert(!f.send(owner, "mast-pass-12", true, true, 3).empty());
    const char lua[] =
        "function lguard(text) return node.admin(text) end\n"
        "command('lguard','text:text:155','Owner admin fixture','lguard','owner')";
    BotWorker::Result result;
    assert(f.bot.stageSource(lua, strlen(lua))); f.step();
    assert(f.bot.pollSourceResult(result) && result.ok && f.bot.activateStaged()); f.step();
    assert(f.bot.pollSourceResult(result) && result.ok);
#if ONCHIP_BOT_WASM
    const char *path = getenv("BOT_WASM_CREDENTIAL_MODULE");
    assert(path && "Use the lifecycle-test Make target to build the Wasm credential fixture");
    std::ifstream module(path, std::ios::binary);
    const std::string wasm{std::istreambuf_iterator<char>(module), std::istreambuf_iterator<char>()};
    assert(wasm.size() > 8 && wasm.size() <= BotSourceLimit);
    assert(f.bot.stageSource(wasm.data(), wasm.size())); f.step();
    assert(f.bot.pollSourceResult(result) && result.ok && f.bot.activateStaged()); f.step();
    assert(f.bot.pollSourceResult(result) && result.ok);
#endif
    const auto invoke = [&](Peer &peer, const std::string &text) {
      timeMs += 61000; f.radio.sent.clear();
      const auto wire = peer.command(f.bot.publicKey(), text.c_str(), 3, {0x31, 0x32, 0x33});
      f.mux.received(wire.data(), wire.size(), -91, 6); f.step();
      const auto replies = peer.replies(f.bot.publicKey(), f.radio);
      if (replies.size() != 1) fprintf(stderr, "Credential fixture reply count: %zu (%s)\n", replies.size(), text.c_str());
      assert(replies.size() == 1); return replies[0];
    };
    const auto configurationFiles = [] {
      auto records = filesystem_test::files;
      records.erase("/companion/contacts3");
      for (auto entry = records.begin(); entry != records.end();)
        if (entry->first.find("/companion/bl/") == 0) entry = records.erase(entry);
        else ++entry;
      return records;
    };
    const auto credentialRecords = [] {
      auto records = identity_test::durable;
      // Permitted bot HTTPS commands record the sender before the transport guard denies staging.
      records.erase({"mc-mast-admin", "replay"});
      return records;
    };
    for (const char *handler : {"lguard"
#if ONCHIP_BOT_WASM
        , "wguard"
#endif
    }) {
      const std::string prefix = "!" + std::string(handler) + " ";
      assert(invoke(outsider, prefix + "bot status").find("not granted") != std::string::npos);
      assert(invoke(owner, prefix + "bot status").find("bot applied=1") == 0);
      assert(repeaterFlush() && roomFlush());
      const auto before = credentialRecords();
      const auto configuration = configurationFiles();
      const std::string identityImport = "key management " + std::string(128, '1');
      for (const char *command : {"password 736563726574", "role password repeater 736563726574",
           "role password room 736563726574", "wifi 66697874757265 7365637265743132",
           "bot https token home 736563726574", "bot https ca home 736563726574",
           identityImport.c_str(),
           "data begin 0123456789abcdef 0000000000000000000000000000000000000000000000000000000000000000",
           "source begin 0123456789abcdef 8 0000000000000000000000000000000000000000000000000000000000000000"}) {
        const auto reply = invoke(owner, prefix + command);
        const char *reason = !strncmp(command, "role password ", 14) ? "bot programs denied" :
                            !strncmp(command, "bot https ", 10) ? "HTTPS CA/token staging requires encrypted Management RF" :
                            "credentials and bulk source/data require native management CLI/web";
        if (reply.find(reason) == std::string::npos) fprintf(stderr, "Credential fixture unexpected reply: %s\n", reply.c_str());
        assert(reply.find(reason) != std::string::npos);
        assert(credentialRecords() == before && configurationFiles() == configuration);
        assert(MastAdmin::passwordMatches("mast-pass-12") && !MastAdmin::passwordMatches("secret"));
      }
      for (const char *role : {"repeater", "room"}) {
        const auto login = role_password_login(f, roleAdmin, role, "test-admin");
        assert(login.size() >= 13 && login[6] == 1 && (login[7] & 3) == 3);
      }
    }
  }
  identity_test::durable = durable;
  filesystem_test::files = files;
  printf("PASS source credentials: real %s owner Admin I/O, native backend denial, unchanged Management/Relay/Room credentials and secret settings\n",
         ONCHIP_BOT_WASM ? "Lua/Wasm" : "Lua (Wasm compiled out)");
}
static void native_discovery_admin() {
  const auto baseline = identity_test::durable;
  assert(saveBotDiscovery(false));
  {
    BetaFixture f;
    Peer owner, outsider;
    assert(f.send(outsider, "bot discovery on", false).empty());
    assert(f.action("bot discovery").find("saved=0 live=0") != std::string::npos);
    assert(!f.send(owner, "mast-pass-12", true).empty());
    const auto reply = f.send(owner, "bot discovery on", false);
    assert(reply.size() > 5 &&
           std::string(reply.begin() + 5, reply.end()).find("Saved and applied") == 0);
    assert(f.action("bot discovery").find("saved=1 live=1") != std::string::npos);
    assert(f.action("bot discovery invalid").find("Error:") == 0);
    assert(f.action("bot discovery off").find("Saved and applied") == 0);
    assert(f.action("bot discovery").find("saved=0 live=0") != std::string::npos);
  }
  identity_test::durable = baseline;
  puts("PASS native discovery management: unauthenticated denial, encrypted login/CLI, persisted live grant and revocation");
}
static void adaptive_policy_admin() {
  assert(saveBotAdaptiveAdmission(false));
  BetaFixture f;
  Peer owner, outsider;
  const auto flag = identity_test::durable.at({"mc-onchip", "bot-adaptive"});
  assert(f.send(outsider, "bot adaptive on", false).empty());
  assert(identity_test::durable.at({"mc-onchip", "bot-adaptive"}) == flag);
  assert(!f.send(owner, "mast-pass-12", true).empty());
  const auto execute = [&](const char *command) {
    const auto reply = f.send(owner, command, false);
    assert(reply.size() > 5);
    const auto first = reply.begin() + 5;
    return std::string(first, std::find(first, reply.end(), uint8_t(0)));
  };
  assert(execute("bot adaptive on") == "Saved adaptive admission; reboot required");
  assert(execute("bot adaptive").find("saved=1 live=0") != std::string::npos);
  for (const auto &bad : std::vector<std::vector<uint8_t>>{
         {'B', 'A', 'D', 1, 0}, {'B', 'A', 'A', 1}, {'B', 'A', 'A', 1, 2}}) {
    identity_test::durable[{"mc-onchip", "bot-adaptive"}] = bad;
    assert(execute("bot adaptive").find("policy-fault=1") != std::string::npos);
    assert(execute("bot adaptive off") == "Saved adaptive admission; reboot required");
    assert(execute("bot adaptive").find("saved=0 live=0") != std::string::npos);
  }
  puts("PASS adaptive owner admin: unauthenticated denial, encrypted owner ON/OFF, corrupt magic/size/value repair and reportable fault");
}

int main(int argc, char **argv) {
  setbuf(stdout, nullptr);
#ifdef ONCHIP_SOURCE_BOOT_HEALTH_TEST
  assert(saveRoleProfile({0}) && saveBotEnabled(true));
  selected_source_boot_health();
  live_source_retry();
  packageMetadataLifecycle();
  return 0;
#endif
#ifdef PINE_PARSER_STACK_TEST
  assert(saveRoleProfile({0}) && saveBotEnabled(true));
  sourceParserStackValidation();
  return 0;
#endif
  if (argc == 2 && !strcmp(argv[1], "--adaptive-admin-test")) {
    assert(saveRoleProfile({0}) && saveBotEnabled(true));
    adaptive_policy_admin();
    return 0;
  }
  if (argc == 2 && !strcmp(argv[1], "--role-password-test")) {
    assert(saveRoleProfile({0}) && saveBotEnabled(true));
    runtime_role_passwords();
    return 0;
  }
  if (argc == 2 && !strcmp(argv[1], "--native-discovery-test")) {
    assert(saveRoleProfile({0}) && saveBotEnabled(true));
    native_discovery_admin();
    board.mcuTemperature = 21.5f;
    native_bot_discovery();
    board.mcuTemperature = NAN;
    native_bot_discovery_limits();
    return 0;
  }
  source_copy_limits();
  assert(saveRoleProfile({0}) && saveBotEnabled(true));
  service_names_without_app_roles();
  renamed_role_adverts();
  runtime_role_configuration();
  supplied_identity_keys();
  runtime_role_passwords();
  runtime_management_password();
  management_cli_compatibility();
  native_role_profile_owner_info();
  bot_facing_owner_admin();
  cross_runtime_source_credentials();
  packageMetadataLifecycle();
  origin_path_policy();
  beta_review_regressions();
  native_discovery_admin();
  board.mcuTemperature = 21.5f;
  native_bot_discovery();
  board.mcuTemperature = NAN;
  native_bot_discovery_limits();
  stock_chat_through_native_relay();
  public_users_and_private_routes();
  std::string source = "function custom(text) reply('Installed:'..text) end "
                       "command('custom','text:text:150','Installed handler') "
                       "function hello(name) reply('Hello '..name) end";
  {
    BetaFixture f;
    Peer peer;
    assert(f.send(peer, "wrong-password", true).empty());
    timeMs += 1100;
    assert(f.send(peer, "status", false).empty());
    assert(f.send(peer, "test-admin", true).empty());
    const auto login = f.send(peer, "mast-pass-12", true, true, 1, false);
    assert(login.size() >= 13 && login[4] == 0 && login[6] == 1 && login[7] == 3);
    const auto response = f.send(peer, "status", false);
    assert(response.size() > 5 && response[4] == 4);
    assert(strstr(reinterpret_cast<const char *>(response.data() + 5), "roles applied=0 saved=0"));
    const auto tagged = f.send(peer, "a2|status", false);
    assert(tagged.size() > 8 && !memcmp(tagged.data() + 5, "a2|", 3));
    const auto taggedLong = f.send(peer, "0123456789abcdef|help", false);
    assert(taggedLong.size() > 22 && !memcmp(taggedLong.data() + 5, "0123456789abcdef|status;", 24));
    assert(f.action("help").find("bot https") != std::string::npos);
    assert(f.action("bot https").find("ca/token NAME HEX <=128") != std::string::npos);
    assert(f.action("bot https status") == "Error: HTTPS not built into this profile");
    const std::string staging = "bot https token demo " + std::string(128, 'a');
    const std::string rejected = "Error: HTTPS CA/token staging requires encrypted Management RF; web/Lua staging denied";
    assert(f.action(staging.c_str()) == rejected);
    for (const char *text : {"bot https ca demo 616263", "bot https  token demo 616263",
                            "bot https ca", "bot https token"}) {
      MastAdmin::Reply secretReply;
      f.management.admin().execute(text, secretReply, 0, MastAdmin::Transport::Other);
      assert(secretReply.text == rejected);
    }
    MastAdmin::Reply encryptedReply;
    f.management.admin().execute(staging.c_str(), encryptedReply, 0,
                                MastAdmin::Transport::NativeEncrypted);
    assert(std::string(encryptedReply.text) == "Error: HTTPS not built into this profile");
    assert(f.action((staging + "aaaaaaaaaaaaaaaa").c_str()) == "Error: CLI text exceeds 162 bytes");
    const auto httpsReply = f.send(peer, "bot https status", false);
    assert(httpsReply.size() > 5 && httpsReply[4] == 4);
    assert(std::string(httpsReply.begin() + 5, httpsReply.end()).find("Error: HTTPS not built into this profile") == 0);
    assert(f.action(("trust " + encode(peer.self_id.pub_key, 32)).c_str()).find("Saved") == 0);
    const auto authCommand = "auth status " + encode(peer.self_id.pub_key, 32);
    const auto authBefore = f.action(authCommand.c_str());
    assert(authBefore.find("trusted=1 compiled=0 timestamp=" + std::to_string(peer.timestamp)) != std::string::npos);
    assert(authBefore.size() <= MastAdmin::TextLimit - 17);
    assert(f.action("auth status").find("RF peers=") == 0);
    bool foundPeer = false;
    for (unsigned slot = 1; slot <= 4; ++slot) {
      const auto info = f.action(("auth peer " + std::to_string(slot)).c_str());
      if (info.find(encode(peer.self_id.pub_key, 32)) != std::string::npos) foundPeer = true;
      assert(info.size() <= MastAdmin::TextLimit - 17);
    }
    assert(foundPeer && f.action(authCommand.c_str()) == authBefore);
    assert(f.action("auth status bad").find("Error:") == 0);
    assert(f.action("auth peer 0").find("Error:") == 0);
    assert(f.action("auth peer 5").find("Error:") == 0);
    assert(f.action("auth peer -1").find("Error:") == 0);
    timeMs += 1100;
    assert(!f.send(peer, "", true).empty());
    MastAdmin::Reply reply;
    const auto before = f.mux.configurationGeneration();
    f.management.admin().execute("tempradio 2 912525000 250000 8 5 2", reply);
    assert(reply.ticket);
    f.step();
    assert(f.mux.configurationGeneration() == before);
    f.management.admin().acknowledged(reply.ticket, true);
    f.step();
    assert(f.mux.currentConfiguration().sf == 8 && f.mux.configurationGeneration() == before + 1);
    identity_test::failCommit = true;
    f.step(250);
    identity_test::failCommit = false;
    assert(f.mux.currentConfiguration().sf == 7 && f.mux.configurationGeneration() == before + 2);
    RadioDashboard::RadioStatus radioStatus;
    f.mux.dashboardStatus(radioStatus);
    assert(radioStatus.committed);
    f.management.admin().execute("radio 912525000 250000 9 5 2", reply);
    f.management.admin().acknowledged(reply.ticket, false);
    f.step();
    assert(f.mux.currentConfiguration().sf == 7);
    f.management.admin().execute("radio 912525000 250000 8 5 2", reply);
    f.management.admin().acknowledged(reply.ticket, true);
    f.step();
    assert(f.mux.currentConfiguration().sf == 8);
    WifiKissMultiplexer restarted;
    restarted.attachRadio(f.radio, f.rng, [](float, float, uint8_t, uint8_t) {}, [](uint8_t) {});
    assert(restarted.setInitialConfiguration({912525000, 250000, 7, 5, 2}, true));
    assert(restarted.currentConfiguration().sf == 8);
    f.management.admin().execute("radio 912525000 250000 7 5 2", reply);
    f.management.admin().acknowledged(reply.ticket, true);
    f.step();
    identity_test::failCommit = true;
    assert(f.action("roles 4").find("Error:") == 0);
    identity_test::failCommit = false;
    assert(f.action("roles 0").find("Saved") == 0);
    const auto wifiReply = f.send(peer, "wifi 6c61622d66697874757265 646973706f7361626c65", false);
    assert(wifiReply.size() > 5 &&
           std::string(wifiReply.begin() + 5, wifiReply.end()).find("Saved") == 0);
    MastAdmin::WifiCredentials wifi;
    bool present;
    assert(MastAdmin::loadWifi(wifi, present) && present &&
           !strcmp(wifi.ssid, "lab-fixture") && !strcmp(wifi.password, "disposable"));
    upload(f, source, true, true, [&](const char *text) {
      const auto response = f.send(peer, text, false);
      assert(response.size() > 5 && response[4] == 4);
      return std::string(response.begin() + 5, response.end());
    });
    assert(f.action("source help Example editable help").find("Saved") == 0);
    assert(f.action("source helptext") == "Example editable help");
  }
  {
    BetaFixture f;
    f.step(300);
    assert(f.action("source status").find("durably saved and active") != std::string::npos);
    Peer peer;
    const auto advert = peer.advert();
    f.mux.received(advert.data(), advert.size(), -90, 5); f.step();
    const auto wire = peer.command(f.bot.publicKey(), "!custom ready");
    f.radio.sent.clear();
    f.mux.received(wire.data(), wire.size(), -90, 5); f.step();
    const auto replies = peer.replies(f.bot.publicKey(), f.radio);
    assert(replies.size() == 1 && replies[0] == "Installed:ready");
    for (const auto &test : {std::pair<const char *, const char *>{"!hello slepp", "Hello slepp"},
                             {"!ping", "Pong"}}) {
      timeMs += 61000;
      const auto request = peer.command(f.bot.publicKey(), test.first);
      f.radio.sent.clear();
      f.mux.received(request.data(), request.size(), -90, 5); f.step();
      assert(peer.replies(f.bot.publicKey(), f.radio) == std::vector<std::string>{test.second});
    }
    assert(f.action("source rollback").find("Accepted") == 0);
    f.step(300);
    assert(f.action("source status").find("active=3") != std::string::npos);
    assert(f.action("source helptext") == "(no source help)");
    assert(f.action("source rollback").find("Accepted") == 0);
    f.step(300);
    assert(f.action("source status").find("durably saved and active") != std::string::npos);
    assert(f.action("source helptext") == "Example editable help");
    assert(f.action("source remove").find("Accepted") == 0);
    f.step(300);
    assert(f.action("source status").find("active=3") != std::string::npos);
  }
  interrupted_upload();
  identity_test::durable.at({"mc-mast-admin", "source"})[0] ^= 1;
  {
    BetaFixture f;
    assert(f.action("source status").find("Error: source journal") == 0);
    assert(f.action("status").find("roles applied=") == 0);
    RadioDashboard::RoleStatus status;
    f.bot.dashboardStatus(status);
    assert(!status.ready && !strcmp(status.state, "source-recovery"));
    Peer peer;
    const auto advert = peer.advert();
    f.mux.received(advert.data(), advert.size(), -90, 5); f.step();
    timeMs += 61000;
    const auto ping = peer.command(f.bot.publicKey(), "!ping");
    f.radio.sent.clear();
    f.mux.received(ping.data(), ping.size(), -90, 5); f.step();
    assert(peer.replies(f.bot.publicKey(), f.radio).empty());
    assert(f.action("source remove").find("Removed corrupt") == 0);
    f.step(300);
    assert(f.action("source status").find("durably saved and active") != std::string::npos);
    f.bot.dashboardStatus(status);
    assert(status.ready);
    timeMs += 61000;
    const auto recovered = peer.command(f.bot.publicKey(), "!ping");
    f.radio.sent.clear();
    f.mux.received(recovered.data(), recovered.size(), -90, 5); f.step();
    assert(peer.replies(f.bot.publicKey(), f.radio) == std::vector<std::string>{"Pong"});
  }
  assert(saveBotEnabled(false));
  {
    BetaFixture f;
    assert(!f.bot.publicKey());
    upload(f, source, false);
    assert(f.action("status").find("roles applied=0") == 0);
  }
  assert(saveBotEnabled(true));
  {
    BetaFixture f;
    f.step(300);
    assert(f.action("source status").find("durably saved and active") != std::string::npos);
  }
  puts("PASS mast beta: native login/CLI and routed path return, full-key trust, "
       "reply-gated PHY/restore generation, durable WiFi/roles, source install/reboot/rollback/remove");
}
