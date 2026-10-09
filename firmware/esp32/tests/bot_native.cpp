// SPDX-License-Identifier: Apache-2.0
#include "CommandBot.h"
#include "BotRegistry.h"
#if ONCHIP_BOT_WASM
#include "BotWasm.h"
#endif
#include "RoleProfile.h"
#include "BotStore.h"
#include "Clock.h"
#include "support/BotNativeHarness.h"
#ifdef BOT_HOST_RUNNER
#include "support/BotReplayNetwork.h"
#include "BotNetworkConfig.h"
#endif
#include <helpers/AdvertDataHelpers.h>
#include <helpers/SimpleMeshTables.h>
#include <helpers/BaseChatMesh.h>
#include <helpers/TransportKeyStore.h>
#include <nvs.h>
#include <esp_heap_caps.h>
#include <SPIFFS.h>
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <deque>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <memory>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>
static void command_mesh_experience();
#if defined(ONCHIP_BOT_RUNTIME_TEST) || defined(BOT_HOST_RUNNER)
#include "Runtime.h"
#include "CompanionSessions.h"
#include <target.h>
#include <sys/socket.h>
LifecycleTestBoard board;
SensorManager sensors;
VolatileRTCClock rtc_clock;
#endif

using namespace onchip;
using Bytes = std::vector<uint8_t>;
using Radio = bot_native_test::Radio;
using Fixture = bot_native_test::BotNativeHarness;
std::atomic<unsigned long> timeMs{1000};
unsigned long millis() { return timeMs; }
void delay(unsigned long value) { timeMs += value; }
static uint32_t entropy = 0x31415926;
void randomSeed(long value) { entropy = value; }
long random(long low, long high) {
  entropy ^= entropy << 13; entropy ^= entropy >> 17; entropy ^= entropy << 5;
  return low + entropy % (high - low);
}
void esp_fill_random(void *data, size_t size) {
  auto bytes = static_cast<uint8_t *>(data);
  while (size--) *bytes++ = random(0, 256);
}
namespace packet_pool {
#define ONCHIP_PACKET_CAPACITY 4
#include "support/PacketPool.h"
#include "support/PacketPool.inc"
#undef ONCHIP_PACKET_CAPACITY
}
struct PeerState {
  Radio radio;
  ArduinoMillis clock;
  HardwareRNG rng;
  VolatileRTCClock rtc;
  packet_pool::StaticPoolPacketManager packets{4};
  SimpleMeshTables tables;
};
struct Peer : PeerState, mesh::Mesh {
  uint32_t timestamp = 1800000000;
  Peer() : Mesh(radio, clock, rng, rtc, packets, tables) {
    self_id = mesh::LocalIdentity(&rng); begin();
  }
  Bytes wire(mesh::Packet *packet) {
    uint8_t bytes[255];
    const size_t size = packet->writeTo(bytes);
    releasePacket(packet);
    return Bytes(bytes, bytes + size);
  }
  Bytes advert() {
    uint8_t data[MAX_ADVERT_DATA_SIZE]{};
    AdvertDataBuilder builder(ADV_TYPE_CHAT, "native-test-peer");
    auto *packet = createAdvert(self_id, data, builder.encodeTo(data));
    assert(packet);
    packet->header |= ROUTE_TYPE_FLOOD;
    packet->path_len = 0;
    return wire(packet);
  }
  Bytes command(const uint8_t *bot, const char *text, uint8_t pathWidth = 1,
                Bytes path = {}, bool flood = true) {
    uint8_t secret[32];
    self_id.calcSharedSecret(secret, bot);
    Bytes data(5);
    queued_tx::put32(data.data(), ++timestamp);
    data.insert(data.end(), text, text + strlen(text));
    auto *packet = createDatagram(PAYLOAD_TYPE_TXT_MSG, mesh::Identity(bot),
                                  secret, data.data(), data.size());
    assert(packet);
    packet->header |= flood ? ROUTE_TYPE_FLOOD : ROUTE_TYPE_DIRECT;
    packet->setPathHashSizeAndCount(pathWidth, path.size() / pathWidth);
    if (!path.empty()) memcpy(packet->path, path.data(), path.size());
    return wire(packet);
  }
  Bytes pathReturn(const uint8_t *bot, uint8_t width, const Bytes &path) {
    uint8_t secret[32];
    self_id.calcSharedSecret(secret, bot);
    auto *packet = createPathReturn(mesh::Identity(bot), secret, path.empty() ? secret : path.data(),
        ((width - 1) << 6) | (path.size() / width), PAYLOAD_TYPE_RAW_CUSTOM, secret, 0);
    assert(packet);
    packet->header |= ROUTE_TYPE_DIRECT; packet->path_len = 0;
    return wire(packet);
  }
  static mesh::GroupChannel hashtag(const char *name) {
    mesh::GroupChannel channel{};
    mesh::Utils::sha256(channel.secret, 16, reinterpret_cast<const uint8_t *>(name), strlen(name));
    mesh::Utils::sha256(channel.hash, sizeof(channel.hash), channel.secret, 16);
    return channel;
  }
  Bytes group(const char *text, const char *name = "#example1", uint8_t width = 3, Bytes path = {},
              const uint8_t *key = nullptr) {
    auto channel = hashtag(name);
    if (key) {
      memcpy(channel.secret, key, 16);
      mesh::Utils::sha256(channel.hash, sizeof(channel.hash), key, 16);
    }
    Bytes data(5);
    queued_tx::put32(data.data(), ++timestamp);
    data.insert(data.end(), text, text + strlen(text));
    auto *packet = createGroupDatagram(PAYLOAD_TYPE_GRP_TXT, channel, data.data(), data.size());
    assert(packet);
    packet->header |= ROUTE_TYPE_FLOOD;
    packet->setPathHashSizeAndCount(width, path.size() / width);
    if (!path.empty()) memcpy(packet->path, path.data(), path.size());
    return wire(packet);
  }
  Bytes targetedGroup(const uint8_t *bot, const char *text, const char *name = "#example1",
                      uint8_t width = 3, Bytes path = {}, const uint8_t *channelKey = nullptr) {
    std::string addressed = text;
    const auto at = addressed.find(": !");
    assert(at != std::string::npos);
    char key[9];
    for (unsigned i = 0; i < 4; ++i) snprintf(key + 2 * i, 3, "%02x", bot[i]);
    addressed.insert(at + 3, "@" + std::string(key) + " ");
    return group(addressed.c_str(), name, width, path, channelKey);
  }
  std::vector<std::string> groupReplies(const Radio &radio, uint8_t width = 3,
                                      const char *name = "#example1", const uint8_t *key = nullptr) {
    auto channel = hashtag(name);
    if (key) {
      memcpy(channel.secret, key, 16);
      mesh::Utils::sha256(channel.hash, sizeof(channel.hash), key, 16);
    }
    std::vector<std::string> result;
    for (const auto &raw : radio.sent) {
      mesh::Packet packet;
      assert(packet.readFrom(raw.data(), raw.size()));
      if (packet.getPayloadType() != PAYLOAD_TYPE_GRP_TXT) continue;
      if (packet.payload[0] != channel.hash[0]) continue;
      assert(packet.payload_len <= MAX_PACKET_PAYLOAD && packet.getPathHashSize() == width);
      uint8_t plain[MAX_PACKET_PAYLOAD]{};
      const int size = mesh::Utils::MACThenDecrypt(channel.secret, plain, packet.payload + 1,
                                                  packet.payload_len - 1);
      if (size <= 5) continue;
      assert(plain[4] == 0);
      const auto *end = std::find(plain + 5, plain + size, 0);
      const std::string body(reinterpret_cast<const char *>(plain + 5), reinterpret_cast<const char *>(end));
      const auto separator = body.find(": ");
      assert(separator > 0 && separator <= 31);
      result.push_back(body.substr(separator + 2));
    }
    return result;
  }
  std::vector<std::string> replies(const uint8_t *bot, const Radio &radio) {
    std::vector<std::string> result;
    uint8_t secret[32];
    self_id.calcSharedSecret(secret, bot);
    for (const auto &raw : radio.sent) {
      mesh::Packet p;
      assert(p.readFrom(raw.data(), raw.size()));
      if (p.getPayloadType() != PAYLOAD_TYPE_TXT_MSG ||
          p.payload[0] != self_id.pub_key[0] || p.payload[1] != bot[0])
        continue;
      uint8_t plain[184]{};
      const int size = mesh::Utils::MACThenDecrypt(secret, plain, p.payload + 2,
                                                  p.payload_len - 2);
      if (size <= 5) continue;
      assert(plain[4] == 0);
      const auto *end = std::find(plain + 5, plain + size, 0);
      result.emplace_back(reinterpret_cast<const char *>(plain + 5),
                          reinterpret_cast<const char *>(end));
    }
    return result;
  }
  Bytes ackLast(const uint8_t *bot, const Radio &radio) {
    uint8_t secret[32];
    self_id.calcSharedSecret(secret, bot);
    for (auto raw = radio.sent.rbegin(); raw != radio.sent.rend(); ++raw) {
      mesh::Packet packet;
      assert(packet.readFrom(raw->data(), raw->size()));
      if (packet.getPayloadType() != PAYLOAD_TYPE_TXT_MSG ||
          packet.payload[0] != self_id.pub_key[0]) continue;
      uint8_t plain[MAX_PACKET_PAYLOAD]{}, ack[4];
      const int size = mesh::Utils::MACThenDecrypt(secret, plain, packet.payload + 2,
                                                  packet.payload_len - 2);
      assert(size >= 5);
      const auto *end = std::find(plain + 5, plain + size, 0);
      mesh::Utils::sha256(ack, sizeof(ack), plain, end - plain, bot, 32);
      auto *response = createAck(ack, sizeof(ack)); assert(response);
      response->header |= ROUTE_TYPE_DIRECT; response->path_len = 0;
      return wire(response);
    }
    assert(false && "No outgoing native DM to ACK"); return {};
  }
};
static void native_channel_policy() {
  assert(saveBotEnabled(true) && saveBotRadioPolicy({}));
  Fixture f; f.start(); Peer peer;
  f.learn(peer);
  uint8_t identity[32]; memcpy(identity, f.bot.publicKey(), sizeof(identity));
  char reply[162]{};
  const auto policy = [&](const char *command) {
    f.bot.radioPolicyCommand(command, reply, sizeof(reply));
    assert(!strncmp(reply, "Saved and applied", 17));
  };
  policy("membership 0 #first");
  policy("membership 1 #second");
  policy("membership 2 public");
  policy("membership 3 private 50726976617465 0102030405060708090a0b0c0d0e0f10");
  f.bot.targetAliasesCommand("aspen,aspen-bot,a", reply, sizeof(reply));
  assert(!strncmp(reply, "Saved and applied", 17));
  f.bot.targetAliasesCommand("", reply, sizeof(reply));
  assert(!strcmp(reply, "Aliases: aspen,aspen-bot,a; live=applied"));
  for (unsigned failure = 0; failure < 3; ++failure) {
    const auto stored = identity_test::durable;
    identity_test::failWrite = failure == 0;
    identity_test::failCommit = failure == 1;
    identity_test::afterCommit = failure == 2 ? +[] { identity_test::failRead = true; } : nullptr;
    f.bot.targetAliasesCommand("new-alias", reply, sizeof(reply));
    assert(!strncmp(reply, "Error:", 6));
    identity_test::failWrite = identity_test::failCommit = identity_test::failRead = false;
    identity_test::afterCommit = nullptr;
    if (failure == 2) {
      f.bot.targetAliasesCommand("", reply, sizeof(reply));
      assert(!strcmp(reply, "Aliases: new-alias; live=differs; reapply saved aliases"));
    }
    identity_test::durable = stored;
    f.bot.targetAliasesCommand("", reply, sizeof(reply));
    assert(!strcmp(reply, "Aliases: aspen,aspen-bot,a; live=applied"));
  }
  const auto aliasRecord = identity_test::durable.at({"mc-onchip", "bot-aliases"});
  for (unsigned malformed = 0; malformed < 3; ++malformed) {
    auto &record = identity_test::durable[{"mc-onchip", "bot-aliases"}];
    record = aliasRecord;
    if (malformed == 0) record.pop_back();
    else if (malformed == 1) record[3] = 2;
    else record[4] = '!';
    BotTargetAliases invalid;
    assert(!loadBotTargetAliases(invalid));
    f.bot.targetAliasesCommand("aspen,aspen-bot,a", reply, sizeof(reply));
    assert(!strncmp(reply, "Saved and applied", 17));
  }
  for (const char *invalid : {"aspen,aspen", "ASPen", ",aspen", "aspen,", "a,b,c,d,e",
                              "1234", "bad alias", "abcdefab", "abcdefghijklmnopq"}) {
    f.bot.targetAliasesCommand(invalid, reply, sizeof(reply));
    assert(!strncmp(reply, "Error:", 6));
  }
  f.bot.targetAliasesCommand("", reply, sizeof(reply));
  assert(!strcmp(reply, "Aliases: aspen,aspen-bot,a; live=applied"));
  BotRadioPolicy saved;
  assert(loadBotRadioPolicy(saved));
  saved.airtimeMs = 3600; saved.pathWidth = 3; assert(saveBotRadioPolicy(saved));
  f.bot.stop(); f.start();
  f.learn(peer);
  assert(!memcmp(identity, f.bot.publicKey(), sizeof(identity)));
  mesh::GroupChannel publicChannel, privateChannel;
  BotRadioPolicy::nativeChannel(saved.membership(2), publicChannel);
  BotRadioPolicy::nativeChannel(saved.membership(3), privateChannel);
  const auto group = [&](const char *command, const char *name, const uint8_t *key = nullptr,
                         bool targeted = true) {
    timeMs += 61000; f.radio.sent.clear();
    f.deliver(targeted ? peer.targetedGroup(f.bot.publicKey(), command, name, 3, {}, key) :
                         peer.group(command, name, 3, {}, key));
    f.step(800);
    return peer.groupReplies(f.radio, 3, name, key);
  };
  assert(group("operator: !ping", "#first") == std::vector<std::string>{"Pong"});
  assert(group("operator: !ping", "#second") == std::vector<std::string>{"Pong"});
  assert(group("operator: !ping", "Private", privateChannel.secret) == std::vector<std::string>{"Pong"});
  assert(group("operator: !ping", "Public", publicChannel.secret).empty());
  policy("access 2 ping 12");
  assert(group("operator: !ping", "Public", publicChannel.secret) == std::vector<std::string>{"Pong"});
  assert(group("operator: !ping", "Public", publicChannel.secret, false).empty());
  policy("access 1 ping 12");
  assert(group("operator: !ping", "#second", nullptr, false).empty());
  assert(group("operator: !ping", "#second") == std::vector<std::string>{"Pong"});
  assert(group("operator: !ping", "#first", nullptr, false).size() == 1);
  policy("membership 4 #test");
  policy("access 4 default 12");
  for (const char *command : {"operator: !ping", "operator: !trace", "operator: !mt 5",
                              "operator: !not-a-command", "operator: !trace invalid-route",
                              "operator: !help", "operator: !@", "operator: !@aspen",
                              "operator: !@another-bot trace", "operator: ordinary text"}) {
    assert(group(command, "#test", nullptr, false).empty());
    assert(f.radio.sent.empty());
  }
  for (const char *command : {"operator: !@aspen ping", "operator: !@aspen-bot ping",
                              "operator: !@a ping", "operator: !@ASPEN ping"}) {
    assert(group(command, "#test", nullptr, false) == std::vector<std::string>{"Pong"});
  }
  assert(group("operator: !ping", "#test") == std::vector<std::string>{"Pong"});
  assert(group("operator: !@aspen trace", "#test", nullptr, false).size() == 1);
  f.bot.stop(); f.start(); f.learn(peer);
  f.bot.targetAliasesCommand("", reply, sizeof(reply));
  assert(!strcmp(reply, "Aliases: aspen,aspen-bot,a; live=applied"));
  assert(group("operator: !trace", "#test", nullptr, false).empty());
  assert(f.radio.sent.empty());
  assert(group("operator: !@a ping", "#test", nullptr, false) == std::vector<std::string>{"Pong"});
  f.bot.targetAliasesCommand("aspen", reply, sizeof(reply));
  assert(!strncmp(reply, "Saved and applied", 17));
  assert(group("operator: !@a ping", "#test", nullptr, false).empty());
  assert(f.radio.sent.empty());
  assert(group("operator: !@aspen ping", "#test", nullptr, false) == std::vector<std::string>{"Pong"});
  policy("access 1 recall 63");
  const auto privateDenied = group("operator: !recall item", "#second");
  assert(privateDenied.size() == 1 && privateDenied[0].find("permission") != std::string::npos);
  const char *source =
      "function where() sleep(1000) return ctx.channel.name end "
      "function keep() kv.put('item','retained') return 'written' end "
      "function fetch() return kv.get('item') or 'missing' end "
      "function emit() local p=mesh.compose{text='effect'} local r=mesh.send(p) "
      "if not r.ok then return r.error end return 'sent' end";
  BotWorker::Result result;
  assert(f.bot.stageSource(source, strlen(source))); f.step();
  assert(f.bot.pollSourceResult(result) && result.ok);
  assert(f.bot.activateStaged()); f.step();
  assert(f.bot.pollSourceResult(result) && result.ok);
  timeMs += 61000; f.radio.sent.clear();
  f.deliver(peer.targetedGroup(identity, "same-name: !where", "#first")); f.step(20);
  f.deliver(peer.targetedGroup(identity, "same-name: !where", "#second")); f.step(20);
  f.deliver(peer.targetedGroup(identity, "same-name: !where", "Private", 3, {}, privateChannel.secret));
  f.step(800);
  assert(peer.groupReplies(f.radio, 3, "#first") == std::vector<std::string>{"#first"});
  assert(peer.groupReplies(f.radio, 3, "#second") == std::vector<std::string>{"#second"});
  assert(peer.groupReplies(f.radio, 3, "Private", privateChannel.secret) == std::vector<std::string>{"Private"});
  policy("access dm keep 53");
  timeMs += 61000; f.radio.sent.clear();
  f.deliver(peer.command(identity, "!keep")); f.step();
  assert(peer.replies(identity, f.radio).empty());
  assert(f.command(peer, "!fetch") == "retained");
  policy("access dm keep 31");
  assert(f.command(peer, "!keep").find("denies storage write") != std::string::npos);
  policy("access dm fetch 47");
  assert(f.command(peer, "!fetch").find("denies storage read") != std::string::npos);
  policy("access dm action_send 49");
  timeMs += 61000; f.radio.sent.clear();
  f.deliver(peer.command(identity, "!emit")); f.step();
  assert(peer.replies(identity, f.radio) == std::vector<std::string>{"effect"});
  policy("access dm action_send 10");
  assert(f.command(peer, "!emit").find("denies radio action") != std::string::npos);
  policy("access dm ping 0");
  timeMs += 61000; f.radio.sent.clear();
  f.deliver(peer.command(identity, "!ping")); f.step();
  assert(peer.replies(identity, f.radio).empty());
  assert(!memcmp(identity, f.bot.publicKey(), sizeof(identity)));
  f.bot.stop(); assert(saveBotRadioPolicy({}));
  f.bot.targetAliasesCommand("off", reply, sizeof(reply));
  puts("PASS native channel policy: simultaneous hashtag/private/Public, configured aliases and key targets, addressed-only #test emits no RF for bare/malformed/unknown/wrong-target requests across restart, alias revocation, full-origin routing, independent grants and unchanged identity");
}
static void native_thread_policy() {
  assert(saveBotEnabled(true) && saveBotRadioPolicy({}) && saveBotEventAccess(0) && saveBotSharedState(false));
  Fixture f; f.start(); Peer first, second; f.learn(first); f.learn(second);
  char reply[162]{};
  const auto policy = [&](const char *command) {
    f.bot.radioPolicyCommand(command, reply, sizeof(reply));
    assert(!strncmp(reply, "Saved and applied", 17));
  };
  policy("membership 0 #first"); policy("membership 1 #second");
  assert(f.bot.setSharedState(true));
  const char *source =
      "ticks=0 "
      "function keep(scope,label,value) kv.put('key',value,{scope=scope,thread=label}) return 'written' end "
      "function fetch(scope,label) return kv.get('key',{scope=scope,thread=label}) or 'missing' end "
      "function total() return tostring(ticks) end "
      "function _tick() kv.put('tick','yes',{scope='bot',thread='monitor'}) ticks=ticks+1 end "
      "events.every(60,'_tick')";
  BotWorker::Result result;
  assert(f.bot.stageSource(source, strlen(source))); f.step();
  assert(f.bot.pollSourceResult(result) && result.ok && f.bot.activateStaged()); f.step();
  assert(f.bot.pollSourceResult(result) && result.ok);
  assert(f.command(first, "!keep conversation notes first") == "written");
  assert(f.command(second, "!fetch conversation notes") == "missing");
  assert(f.command(second, "!keep conversation notes second") == "written");
  assert(f.command(first, "!fetch conversation notes") == "first");
  assert(f.command(second, "!fetch conversation notes") == "second");
  policy("thread dm notes 16");
  assert(f.command(first, "!keep conversation notes denied").find("denies storage write") != std::string::npos);
  assert(f.command(first, "!fetch conversation notes") == "first");
  policy("thread dm notes inherit");
  const auto group = [&](const char *command, const char *name) {
    timeMs += 61000; f.radio.sent.clear();
    f.deliver(first.targetedGroup(f.bot.publicKey(), command, name)); f.step(800);
    const auto replies = first.groupReplies(f.radio, 1, name);
    assert(replies.size() == 1); return replies.front();
  };
  assert(group("same-name: !keep channel notes first", "#first") == "written");
  assert(group("same-name: !fetch channel notes", "#second") == "missing");
  assert(group("same-name: !keep channel notes second", "#second") == "written");
  assert(group("changed-name: !fetch channel notes", "#first") == "first");
  assert(group("same-name: !fetch channel notes", "#second") == "second");
  policy("thread 0 notes 0");
  assert(group("same-name: !fetch channel notes", "#first").find("denies storage read") != std::string::npos);
  assert(group("same-name: !fetch channel notes", "#second") == "second");
  policy("thread native monitor 0");
  assert(f.bot.setEventAccess(16));
  const auto failed = f.bot.counters().eventsFailed;
  timeMs += 61000; f.step(800);
  assert(f.bot.counters().eventsFailed > failed);
  policy("thread native monitor 48");
  const auto completed = f.bot.counters().eventsCompleted;
  timeMs += 61000; f.step(800);
  assert(f.bot.counters().eventsCompleted > completed);
  policy("access native default 48");
  const auto queued = f.bot.counters().eventsQueued;
  timeMs += 61000; f.step(800);
  assert(f.bot.counters().eventsQueued == queued);
  assert(f.command(first, "!total") != "0" && f.bot.counters().eventsQueued == queued);
  assert(f.bot.setEventAccess(0) && f.bot.setSharedState(false));
  f.bot.stop(); assert(saveBotRadioPolicy({}));
  puts("PASS native thread policy: two full DM principals, two full channel origins, nickname-independent shared threads, immediate per-context RW edits, native scheduled-thread grant and future event denial");
}
static void command_mesh_experience() {
  BotRadioPolicy originalRadio; BotMeshPolicy originalMesh;
  assert(loadBotRadioPolicy(originalRadio) && loadBotMeshPolicy(originalMesh));
  Fixture f; f.start(); Peer caller, destination, outsider;
  f.learn(caller); f.learn(destination); f.learn(outsider);
  assert(f.command(caller, "!plugins").find("Bundled 30 commands") == 0);
  const auto observed = f.command(caller, "!neighbors");
  assert(observed.find("Seen 1/3") == 0 && observed.find("rx=1b/0h cached=none") != std::string::npos &&
         observed.find("last-hop") != std::string::npos && observed.find("!neighbors 2") != std::string::npos);
  assert(f.command(caller, "!neighbors 3").find("not live adjacency") != std::string::npos);
  assert(f.command(caller, "!neighbors 17").find("Error:") == 0);
  assert(f.command(caller, "!admin bot status").find("not granted") != std::string::npos);
  char key[65];
  for (unsigned i = 0; i < 32; ++i) snprintf(key + 2 * i, 3, "%02x", destination.self_id.pub_key[i]);
  const auto source = std::string(
      "function exchange() mesh.send(mesh.compose{to='") + key + "',text='question'}) "
      "local p=mesh.wait{kind='text',from='" + key + "',prefix='answer:',timeout_ms=3000} "
      "return p.from..':'..p.text end "
      "function delayed() mesh.send(mesh.compose{to='" + key + "',text='question'}) sleep(1000) "
      "return mesh.wait{kind='text',from='" + key + "',timeout_ms=1000}.text end "
      "function follow() local p=mesh.wait{kind='channel',exact='answer:yes',timeout_ms=2000} "
      "if p.authenticated or p.from then return 'BAD AUTH' end return p.nickname..':'..p.text end";
  const auto install = [&] {
    BotWorker::Result result;
    assert(f.bot.stageSource(source.data(), source.size())); f.step();
    assert(f.bot.pollSourceResult(result) && result.ok && f.bot.activateStaged()); f.step();
    assert(f.bot.pollSourceResult(result) && result.ok);
  };
  install();
  assert(f.command(caller, "!exchange").find("not granted") != std::string::npos);
  auto mesh = originalMesh;
  memcpy(mesh.destinations[0], destination.self_id.pub_key, 32);
  assert(f.bot.setMeshPolicy(mesh));
  assert(f.command(caller, "!exchange").find("fresh authenticated direct route") != std::string::npos);
  f.deliver(destination.pathReturn(f.bot.publicKey(), 3, {0x31, 0x32, 0x33})); f.step();
  timeMs += 61000; f.radio.sent.clear();
  f.deliver(caller.command(f.bot.publicKey(), "!exchange")); f.step(30);
  assert(destination.replies(f.bot.publicKey(), f.radio) == std::vector<std::string>{"question"});
  f.deliver(outsider.command(f.bot.publicKey(), "answer:intruder")); f.step(10);
  assert(caller.replies(f.bot.publicKey(), f.radio).empty());
  f.deliver(destination.command(f.bot.publicKey(), "answer:yes", 3)); f.step();
  assert(caller.replies(f.bot.publicKey(), f.radio) == std::vector<std::string>{std::string(key) + ":answer:yes"});
  timeMs += 61000; f.radio.sent.clear();
  f.deliver(caller.command(f.bot.publicKey(), "!delayed")); f.step(30);
  assert(destination.replies(f.bot.publicKey(), f.radio) == std::vector<std::string>{"question"});
  assert(f.bot.setMeshPolicy(originalMesh)); timeMs += 1000; f.step();
  const auto revoked = caller.replies(f.bot.publicKey(), f.radio);
  assert(revoked.size() == 1 && revoked[0].find("grant") != std::string::npos);
  auto radio = originalRadio; strcpy(radio.channel, "#example1"); radio.pathWidth = 3; radio.airtimeMs = 1200;
  uint8_t identity[32]; memcpy(identity, f.bot.publicKey(), 32);
  f.bot.stop(); assert(saveBotRadioPolicy(radio)); f.start(); install();
  strcpy(mesh.name, "Cedar Bot"); memset(mesh.destinations, 0, sizeof(mesh.destinations));
  mesh.channelWait = false; assert(f.bot.setMeshPolicy(mesh));
  const auto group = [&](const char *command) {
    timeMs += 61000; f.radio.sent.clear();
    f.deliver(caller.targetedGroup(f.bot.publicKey(), command)); f.step();
    const auto replies = caller.groupReplies(f.radio);
    assert(replies.size() == 1); return replies[0];
  };
  assert(group("user: !follow").find("owner grant") != std::string::npos);
  mesh.channelWait = true; assert(f.bot.setMeshPolicy(mesh));
  timeMs += 61000; f.radio.sent.clear();
  f.deliver(caller.targetedGroup(f.bot.publicKey(), "user: !follow")); f.step(20);
  f.deliver(outsider.group("owner: answer:yes")); f.step();
  assert(caller.groupReplies(f.radio) == std::vector<std::string>{"owner:answer:yes"});
  timeMs += 61000; f.radio.sent.clear();
  f.deliver(caller.targetedGroup(f.bot.publicKey(), "user: !follow")); f.step(20);
  f.bot.cancelJobs(); f.step();
  auto cancelled = caller.groupReplies(f.radio);
  assert(cancelled.size() == 1 && cancelled[0].find("cancelled") != std::string::npos);
  f.deliver(outsider.group("owner: answer:yes")); f.step();
  assert(caller.groupReplies(f.radio) == cancelled);
  timeMs += 61000; f.radio.sent.clear();
  const char *question = "user: !ping";
  f.deliver(caller.group(question)); f.step(20);
  assert(caller.groupReplies(f.radio).empty());
  uint8_t canonical[64]{}, digest[16];
  queued_tx::put32(canonical, caller.timestamp); memcpy(canonical + 4, question, strlen(question));
  const auto channel = Peer::hashtag("#example1");
  mesh::Utils::sha256(digest, sizeof(digest), canonical, strlen(question) + 4, channel.secret, sizeof(channel.secret));
  char other[64] = "Otherbot: [q:";
  for (unsigned i = 0; i < 4; ++i) snprintf(other + 13 + 2 * i, 3, "%02x", digest[i]);
  strcpy(other + 21, "] Pong");
  f.deliver(outsider.group(other)); f.step(700);
  assert(caller.groupReplies(f.radio).empty() && f.bot.counters().readSuppressed == 1);
  timeMs += 61000; f.radio.sent.clear();
  f.deliver(caller.group("user: !plugins")); f.step(700);
  const auto delayed = caller.groupReplies(f.radio);
  assert(delayed.size() == 1 && delayed[0].find("[q:") == 0 && delayed[0].find("custom 3 commands") != std::string::npos);
  assert(f.bot.setSharedState(true));
  timeMs += 61000; f.radio.sent.clear();
  f.deliver(caller.group("user: !board put example unsafe")); f.step();
  assert(caller.groupReplies(f.radio).back().find("target required") != std::string::npos);
  assert(group("user: !board put example selected") == "Board example created; committed");
  assert(group("user: !board get example") == "selected");
  assert(group("user: !board delete example") == "Board example deleted; committed");
  f.bot.stop(); f.start();
  BotNodeSnapshot node; f.bot.nodeSnapshot(node);
  assert(!strcmp(node.name, "Cedar Bot") && !memcmp(identity, f.bot.publicKey(), 32));
  f.bot.stop(); assert(saveBotEnabled(false) && f.bot.begin(f.mux));
  f.bot.nodeSnapshot(node); assert(!node.enabled && !strcmp(node.name, "Cedar Bot"));
  assert(saveBotEnabled(true));
  assert(f.bot.setMeshPolicy(originalMesh) && f.bot.setSharedState(false) && saveBotRadioPolicy(originalRadio));
  puts("PASS native command UX: discovery/observed caches, granted direct DM reply identity, revocation, channel wait/cancel, target-only writes, jitter/suppression, persisted name and stable identity");
}
struct NativeChat : PeerState, BaseChatMesh {
  uint8_t width = 1;
  bool scoped = false;
  TransportKey region{};
  uint32_t expectedAck = 0;
  uint8_t recipient[32]{};
  unsigned acks = 0, paths = 0, logins = 0;
  std::vector<std::string> messages, commands;
  NativeChat(uint8_t bytes = 1, bool useScope = false)
      : BaseChatMesh(radio, clock, rng, rtc, packets, tables), width(bytes), scoped(useScope) {
    self_id = mesh::LocalIdentity(&rng);
    rtc.setCurrentTime(1800000000);
    TransportKeyStore keys;
    keys.getAutoKeyFor(1, "#beta-lab", region);
    begin();
  }
  int calcRxDelay(float, uint32_t) const override { return 0; }
  void sendFloodScoped(const ContactInfo &, mesh::Packet *packet, uint32_t delay) override {
    if (scoped) {
      const auto code = region.calcTransportCode(packet);
      uint16_t codes[] = {code, code};
      sendFlood(packet, codes, delay, width);
    } else sendFlood(packet, delay, width);
  }
  void onDiscoveredContact(ContactInfo &, bool, uint8_t, const uint8_t *) override {}
  ContactInfo *processAck(const uint8_t *ack) override {
    if (!expectedAck || memcmp(ack, &expectedAck, 4)) return nullptr;
    expectedAck = 0; ++acks;
    return lookupContactByPubKey(recipient, sizeof(recipient));
  }
  void onContactPathUpdated(const ContactInfo &) override { ++paths; }
  void onMessageRecv(const ContactInfo &, mesh::Packet *, uint32_t, const char *text) override {
    messages.emplace_back(text);
  }
  void onCommandDataRecv(const ContactInfo &, mesh::Packet *, uint32_t, const char *text) override {
    commands.emplace_back(text);
  }
  void onSignedMessageRecv(const ContactInfo &, mesh::Packet *, uint32_t, const uint8_t *, const char *) override {}
  uint32_t calcFloodTimeoutMillisFor(uint32_t) const override { return 15000; }
  uint32_t calcDirectTimeoutMillisFor(uint32_t, uint8_t) const override { return 15000; }
  void onSendTimeout() override { assert(false && "Native peer ACK timed out"); }
  void onChannelMessageRecv(const mesh::GroupChannel &, mesh::Packet *, uint32_t, const char *) override {}
  uint8_t onContactRequest(const ContactInfo &, uint32_t, const uint8_t *, uint8_t, uint8_t *) override { return 0; }
  void onContactResponse(const ContactInfo &, const uint8_t *data, uint8_t size) override {
    assert(size >= 13 && data[6] == 1 && data[7] == 3); ++logins;
  }
  int command(const ContactInfo &contact, const char *text, uint8_t type = 0) {
    memcpy(recipient, contact.id.pub_key, 32);
    uint32_t timeout;
    return sendMessage(contact, getRTCClock()->getCurrentTimeUnique(), type, text, expectedAck, timeout);
  }
};
struct NativeRelay : Peer {
  bool scoped;
  TransportKey region{};
  explicit NativeRelay(bool useScope = false) : scoped(useScope) {
    TransportKeyStore keys;
    keys.getAutoKeyFor(1, "#beta-lab", region);
  }
  bool allowPacketForward(const mesh::Packet *packet) override {
    if (!scoped || !packet->isRouteFlood() || packet->getPayloadType() == PAYLOAD_TYPE_ADVERT) return true;
    const auto code = region.calcTransportCode(packet);
    return packet->hasTransportCodes() &&
           (packet->transport_codes[0] == code || packet->transport_codes[1] == code);
  }
  int calcRxDelay(float, uint32_t) const override { return 0; }
  uint32_t getRetransmitDelay(const mesh::Packet *) override { return 0; }
  uint32_t getDirectRetransmitDelay(const mesh::Packet *) override { return 0; }
};
#include "bot_discovery_cases.h"
#include "bot_contact_recovery_cases.h"
static void stock_chat_through_native_relay() {
  for (bool scoped : {false, true}) for (uint8_t width : {1, 2, 3}) {
    Fixture f;
    NativeChat peer(width, scoped);
    NativeRelay relay(scoped);
    assert(f.bot.begin(f.mux));
    size_t botSent = 0, peerSent = 0, relaySent = 0;
    const auto run = [&](unsigned count = 350) {
      while (count--) {
        f.step(1); peer.loop(); relay.loop();
        while (botSent < f.radio.sent.size()) relay.radio.input.push_back(f.radio.sent[botSent++]);
        while (peerSent < peer.radio.sent.size()) relay.radio.input.push_back(peer.radio.sent[peerSent++]);
        while (relaySent < relay.radio.sent.size()) {
          peer.radio.input.push_back(relay.radio.sent[relaySent]);
          f.deliver(relay.radio.sent[relaySent++]);
        }
      }
    };
    auto *advert = peer.createSelfAdvert("disposable-stock-peer");
    assert(advert); peer.sendFlood(advert, 0, width);
    run();
    auto *contact = peer.lookupContactByPubKey(f.bot.publicKey(), 32);
    assert(contact && contact->out_path_len == OUT_PATH_UNKNOWN);
    timeMs += 61000;
    assert(peer.command(*contact, "!mt 1") == MSG_SEND_SENT_FLOOD);
    run(150);
    assert(peer.acks == 1 && peer.messages.empty() && peer.paths);
    assert(contact->out_path_len == uint8_t((width - 1) << 6 | 1));
    assert(!memcmp(contact->out_path, relay.self_id.pub_key, width));
    run(500);
    char observed[7]{};
    for (unsigned i = 0; i < width; ++i)
      snprintf(observed + 2 * i, 3, "%02x", relay.self_id.pub_key[i]);
    assert(peer.messages.size() == 1 &&
           peer.messages[0] == "1 unique paths in 1000 ms; " + std::to_string(width) + ":" + observed);
    timeMs += 61000;
    assert(peer.command(*contact, "!ping") == MSG_SEND_SENT_DIRECT);
    run();
    assert(peer.acks == 2 && peer.messages.size() == 2 && peer.messages.back() == "Pong");
    bool directReply = false;
    for (const auto &wire : f.radio.sent) {
      mesh::Packet packet;
      assert(packet.readFrom(wire.data(), wire.size()));
      if (packet.getPayloadType() == PAYLOAD_TYPE_TXT_MSG && packet.isRouteDirect())
        directReply |= packet.getPathHashSize() == width && packet.getPathHashCount() == 1 &&
                       !memcmp(packet.path, relay.self_id.pub_key, width);
    }
    assert(directReply);
  }
  puts("PASS stock BaseChat: scoped/unscoped relay, 1/2/3-byte reciprocal paths, immediate !mt ACK and direct Pong");
}
static void discovery_through_native_relay() {
  struct Relay : Peer {
    bool allowPacketForward(const mesh::Packet *) override { return true; }
    int calcRxDelay(float, uint32_t) const override { return 0; }
    uint32_t getRetransmitDelay(const mesh::Packet *) override { return 0; }
    uint32_t getDirectRetransmitDelay(const mesh::Packet *) override { return 0; }
  } relay;
  struct PublicPeer : Peer {
    mesh::Identity discovered;
    unsigned adverts = 0;
    void onAdvertRecv(mesh::Packet *packet, const mesh::Identity &id, uint32_t,
                      const uint8_t *data, size_t size) override {
      AdvertDataParser advert(data, size);
      assert(advert.isValid() && advert.hasName() && advert.getType() == ADV_TYPE_CHAT);
      assert(packet->isRouteFlood() && packet->getPathHashCount() == 1);
      discovered = id;
      ++adverts;
    }
  } peer;
  Fixture f;
  assert(f.bot.begin(f.mux));
  size_t botSent = 0, peerSent = 0, relaySent = 0;
  const auto run = [&]() {
    for (unsigned i = 0; i < 200; ++i) {
      f.step(1);
      relay.loop();
      peer.loop();
      // Only the relay connects the two endpoints; no direct discovery or message delivery.
      while (botSent < f.radio.sent.size()) relay.radio.input.push_back(f.radio.sent[botSent++]);
      while (peerSent < peer.radio.sent.size()) relay.radio.input.push_back(peer.radio.sent[peerSent++]);
      while (relaySent < relay.radio.sent.size()) {
        peer.radio.input.push_back(relay.radio.sent[relaySent]);
        f.deliver(relay.radio.sent[relaySent++]);
      }
    }
  };
  run();
  assert(peer.adverts == 1 && peer.discovered.matches(f.bot.publicKey()));
  mesh::Packet discovery;
  assert(discovery.readFrom(relay.radio.sent.front().data(), relay.radio.sent.front().size()));
  assert(discovery.getPathHashSize() == 1 && discovery.getPathHashCount() == 1 &&
         !memcmp(discovery.path, relay.self_id.pub_key, 1));
  peer.radio.sent.push_back(peer.advert());
  run();
  for (uint8_t width : {1, 2, 3}) {
    timeMs += 61000;
    const auto before = relay.radio.sent.size();
    peer.radio.sent.push_back(peer.command(peer.discovered.pub_key, "!ping", width));
    run();
    const auto replies = peer.replies(peer.discovered.pub_key, relay.radio);
    assert(replies.size() == width && replies.back() == "Pong");
    unsigned delivered = 0;
    for (size_t i = before; i < relay.radio.sent.size(); ++i) {
      mesh::Packet packet;
      assert(packet.readFrom(relay.radio.sent[i].data(), relay.radio.sent[i].size()));
      if (packet.getPayloadType() != PAYLOAD_TYPE_TXT_MSG ||
          packet.payload[0] != peer.self_id.pub_key[0] ||
          packet.payload[1] != peer.discovered.pub_key[0]) continue;
      assert(packet.isRouteFlood() && packet.getPathHashCount() == 1 &&
             packet.getPathHashSize() == width);
      ++delivered;
    }
    assert(delivered == 1);
  }
  assert(!f.bot.advertise() && !f.bot.advertise(true));
  timeMs += 900000;
  f.step(1);
  assert(f.bot.advertise());
  run();
  assert(peer.adverts == 2);
  assert(!f.bot.advertise(true));
  timeMs += 900000;
  f.step(1);
  const auto before = f.radio.sent.size();
  assert(f.bot.advertise(true));
  f.step(1);
  assert(f.radio.sent.size() == before + 1);
  mesh::Packet zeroHop;
  assert(zeroHop.readFrom(f.radio.sent.back().data(), f.radio.sent.back().size()));
  assert(zeroHop.getPayloadType() == PAYLOAD_TYPE_ADVERT &&
         zeroHop.isRouteDirect() && zeroHop.path_len == 0);
  puts("bot native relay: signed chat discovery, encrypted Pong via 1/2/3-byte-hash routes, "
       "no direct endpoint link, unchanged advert interval");
}
static void selection_identity() {
  bool enabled = true;
  assert(loadBotEnabled(enabled) && !enabled);
  HardwareRNG rng;
  for (const char *name : {"modem", "repeater", "room", "companion", "observer", "management"}) {
    mesh::LocalIdentity identity;
    assert(loadIdentity(name, identity));
  }
  assert(commitProfileJournal({RoleProfile{5}, 7, 9}));
  const auto before = identity_test::durable;
  {
    Fixture f;
    assert(f.bot.begin(f.mux));
    assert(!f.bot.publicKey());
    assert(psram_test::allocations.empty());
  }
  for (const auto &entry : before)
    assert(identity_test::durable.at(entry.first) == entry.second);
  assert(!identity_test::durable.count({"mc-onchip", "command-bot"}));
  assert(saveBotEnabled(true));
  Bytes original;
  {
    Fixture f; f.start();
    original = identity_test::durable.at({"mc-onchip", "command-bot"});
    CommandBot second;
    assert(!second.begin(f.mux));
    for (const auto &entry : before)
      assert(identity_test::durable.at(entry.first) == entry.second);
  }
  {
    Fixture f; f.start();
    assert(original == identity_test::durable.at({"mc-onchip", "command-bot"}));
    assert(saveBotEnabled(false));
    f.bot.stop();
    assert(f.bot.begin(f.mux));
    RadioDashboard::RoleStatus status;
    f.bot.dashboardStatus(status);
    assert(!status.has_identity && !status.ready && !strcmp(status.state, "disabled"));
    assert(saveBotEnabled(true));
  }
  identity_test::failCommit = true;
  assert(!saveBotEnabled(false));
  identity_test::failCommit = false;
  assert(loadBotEnabled(enabled) && enabled);
  auto &selection = identity_test::durable.at({"mc-onchip", "command-select"});
  selection[4] = 2;
  assert(!loadBotEnabled(enabled) && !enabled);
  selection[4] = 1;
  {
    Fixture f;
    psram_test::failAfter = 0;
    assert(!f.bot.begin(f.mux));
    psram_test::failAfter = -1;
  }
  assert(original == identity_test::durable.at({"mc-onchip", "command-bot"}));
}
static void native_harness_tx_receipts_are_simulated() {
  Fixture::Options options;
  options.captureTxForTest = true;
  options.manualTxCompletionForTest = true;
  Fixture f(options);
  f.start();

  const Bytes packet{0xca, 0xfe, 0x01, 0x02};
  uint32_t job = 0;
  f.radio.airtime = 1000;
  assert(f.existing[0].queueTransmit(packet.data(), packet.size(), 3, 37, 5000,
                                     job));
  f.step();

  bot_native_test::TxFrame frame;
  assert(f.takeTxFrame(frame));
  assert(frame.simulatedRadio && frame.packet == packet);
  assert(frame.token.sourceSlot == f.existing[0].sourceSlot());
  assert(frame.token.sourceGeneration != 0 && frame.token.sourceJob == job);
  assert(frame.priority == 3 && frame.eligibilityDelayMs == 37 &&
         frame.expiryMs == 5000);
  assert(f.radio.active);

  assert(f.completeSimulatedTxForTest(frame.token, 17));
  bot_native_test::TxOutcome outcome;
  assert(f.takeTxOutcomeForTest(outcome));
  assert(outcome.simulated && outcome.token == frame.token &&
         outcome.state == queued_tx::SUCCEEDED && outcome.simulatedRfMs == 17);
  assert(!f.completeSimulatedTxForTest(frame.token, 17));
  assert(!f.takeTxOutcomeForTest(outcome));
}
static void commands_and_packets() {
  static std::string diagnostics;
  diagnostics.clear();
  Fixture f;
  f.bot.setDiagnosticSink([](const char *text) { diagnostics += text; return true; });
  f.start();
  Peer peer; f.learn(peer);
  assert(f.command(peer, "!ping") == "Pong");
  assert(f.command(peer, "!path", 2, {0xa1, 0xa2, 0xb1, 0xb2}) == "2:a1a2b1b2");
  assert(f.command(peer, "!test", 1, {0xab}).find("RSSI=-91.5 dBm SNR=5.25 dB") != std::string::npos);
  assert(f.command(peer, "!trace").find("Error: TRACE") == 0);
  assert(f.command(peer, "!trace", 3, {0xa1, 0xb2, 0xc3}) ==
         "Error: TRACE requires an explicit route or a received flood path (width 1/2)");
  for (const auto &raw : f.radio.sent) {
    mesh::Packet packet; assert(packet.readFrom(raw.data(), raw.size()));
    assert(packet.getPayloadType() != PAYLOAD_TYPE_TRACE);
  }
  timeMs += 61000; f.radio.sent.clear();
  f.deliver(peer.command(f.bot.publicKey(), "!trace 2:a1a2b1b2"));
  f.step();
  bool traced = false;
  for (const auto &raw : f.radio.sent) {
    mesh::Packet p; assert(p.readFrom(raw.data(), raw.size()));
    if (p.getPayloadType() != PAYLOAD_TYPE_TRACE) continue;
    assert(p.getRouteType() == ROUTE_TYPE_DIRECT && p.path_len == 0 &&
           p.payload_len == 13 && p.payload[8] == 1 &&
           !memcmp(p.payload + 9, Bytes{0xa1, 0xa2, 0xb1, 0xb2}.data(), 4));
    traced = true;
  }
  assert(traced);
  auto completeTrace = [&]() {
    for (auto raw = f.radio.sent.rbegin(); raw != f.radio.sent.rend(); ++raw) {
      mesh::Packet packet; assert(packet.readFrom(raw->data(), raw->size()));
      if (packet.getPayloadType() != PAYLOAD_TYPE_TRACE) continue;
      packet.path_len = (packet.payload_len - 9) / (1u << packet.payload[8]);
      memset(packet.path, 20, packet.path_len);
      uint8_t bytes[255];
      const auto size = packet.writeTo(bytes);
      f.deliver(Bytes(bytes, bytes + size)); f.step();
      return;
    }
    assert(false && "Missing native TRACE to return");
  };
  completeTrace();
  assert(peer.replies(f.bot.publicKey(), f.radio).back() == "Trace 2 hops; SNR 5.00 5.00");
  timeMs += 61000; f.radio.sent.clear();
  const std::string wideTrace = "!trace 8:" + std::string(144, 'a');
  f.deliver(peer.command(f.bot.publicKey(), wideTrace.c_str()));
  f.step();
  traced = false;
  for (const auto &raw : f.radio.sent) {
    mesh::Packet p; assert(p.readFrom(raw.data(), raw.size()));
    if (p.getPayloadType() != PAYLOAD_TYPE_TRACE) continue;
    assert(p.getRouteType() == ROUTE_TYPE_DIRECT && p.path_len == 0 &&
           p.payload_len == 81 && p.payload[8] == 3 &&
           std::all_of(p.payload + 9, p.payload + 81, [](uint8_t b) { return b == 0xaa; }));
    traced = true;
  }
  assert(traced);
  completeTrace();
  assert(peer.replies(f.bot.publicKey(), f.radio).back().find("Trace 9 hops; SNR") == 0);
  timeMs += 61000; f.radio.sent.clear();
  f.deliver(peer.command(f.bot.publicKey(), "!trace", 2, {0xa1, 0xa2, 0xb1, 0xb2}));
  f.step();
  traced = false;
  for (const auto &raw : f.radio.sent) {
    mesh::Packet p; assert(p.readFrom(raw.data(), raw.size()));
    if (p.getPayloadType() != PAYLOAD_TYPE_TRACE) continue;
    assert(p.getRouteType() == ROUTE_TYPE_DIRECT && p.path_len == 0 &&
           p.payload[8] == 1 &&
           !memcmp(p.payload + 9, Bytes{0xb1, 0xb2, 0xa1, 0xa2}.data(), 4));
    traced = true;
  }
  assert(traced);
  timeMs += 5001; f.step();
  assert(peer.replies(f.bot.publicKey(), f.radio).back() ==
         "Error: !trace: TRACE timed out; no reply received. Check the route and try again.");
  assert(diagnostics.find("[string \"bot-commands\"]") != std::string::npos);
  assert(diagnostics.find("Native TRACE timed out") != std::string::npos);
  assert(f.command(peer, "!trace", 3, {1, 2, 3}).find("Error: TRACE") == 0);
  timeMs += 61000; f.radio.sent.clear();
  const auto request = peer.command(f.bot.publicKey(), "!ping", 1, {0xa1});
  f.deliver(request); f.step();
  f.deliver(request); f.step();
  assert(peer.replies(f.bot.publicKey(), f.radio).size() == 1);
  assert(f.bot.counters().duplicates);
  f.radio.sent.clear();
  for (const Bytes &malformed : {
           Bytes{}, Bytes{0x09}, Bytes{0x09, 0x01},
           Bytes{uint8_t(PAYLOAD_TYPE_TXT_MSG << 2 | ROUTE_TYPE_FLOOD), 0, 1, 2, 3},
           Bytes{uint8_t(PAYLOAD_TYPE_ACK << 2 | ROUTE_TYPE_DIRECT), 0, 1},
           Bytes{uint8_t(PAYLOAD_TYPE_TRACE << 2 | ROUTE_TYPE_DIRECT), 0, 1, 2, 3},
           Bytes{uint8_t(PAYLOAD_TYPE_TRACE << 2 | ROUTE_TYPE_DIRECT), 0,
                 0, 0, 0, 0, 0, 0, 0, 0, 255},
           Bytes{uint8_t(PAYLOAD_TYPE_ADVERT << 2 | ROUTE_TYPE_FLOOD), 0, 1, 2}}) {
    if (!malformed.empty()) f.deliver(malformed);
    f.step(5);
  }
  auto damaged = peer.command(f.bot.publicKey(), "!ping");
  damaged.back() ^= 0x40;
  f.deliver(damaged); f.step();
  assert(peer.replies(f.bot.publicKey(), f.radio).empty());
  assert(f.bot.counters().malformed >= 2);
  timeMs += 61000; f.radio.sent.clear();
  const auto mt = peer.command(f.bot.publicKey(), "!mt 1", 1, {1});
  for (unsigned path = 1; path <= 9; ++path) {
    Bytes observation = mt;
    observation[2] = path;
    f.deliver(observation); f.step(5);
  }
  assert(peer.replies(f.bot.publicKey(), f.radio).empty());
  timeMs += 1000; f.step();
  const auto replies = peer.replies(f.bot.publicKey(), f.radio);
  assert(replies.size() == 1 && replies[0] ==
         "8 unique paths in 1000 ms; 01 | 02 | 03 | 04 | 05 | 06 | 07 | 08; truncated");
  assert(f.bot.counters().observationsDropped == 1);
  timeMs += 61000; f.radio.sent.clear();
  const auto overflow = peer.command(f.bot.publicKey(), "!mt 1", 1, {0xa1});
  f.deliver(overflow); f.step(2);
  for (unsigned i = 0; i < 20; ++i) f.deliver(overflow);
  f.step(20); timeMs += 1000; f.step();
  const auto truncated = peer.replies(f.bot.publicKey(), f.radio);
  assert(truncated.size() == 1 && truncated[0].find("truncated") != std::string::npos);

  const auto receiveDirect = [&](const char *text) {
    timeMs += 61000; f.radio.sent.clear();
    const auto request = peer.command(f.bot.publicKey(), text, 1, {}, false);
    f.deliver(request); f.step();
    Bytes plain(5);
    queued_tx::put32(plain.data(), peer.timestamp);
    plain.insert(plain.end(), text, text + strlen(text));
    uint8_t expected[4];
    mesh::Utils::sha256(expected, sizeof(expected), plain.data(), plain.size(),
                        peer.self_id.pub_key, 32);
    const auto ackCount = [&]() {
      unsigned count = 0;
      for (const auto &raw : f.radio.sent) {
        mesh::Packet packet; assert(packet.readFrom(raw.data(), raw.size()));
        if (packet.getPayloadType() == PAYLOAD_TYPE_ACK &&
            !memcmp(packet.payload, expected, sizeof(expected))) ++count;
      }
      return count;
    };
    assert(ackCount() == 1);
    const auto responses = peer.replies(f.bot.publicKey(), f.radio);
    const auto malformed = f.bot.counters().malformed;
    f.deliver(request); f.step();
    assert(ackCount() == 1 && peer.replies(f.bot.publicKey(), f.radio) == responses);
    timeMs += 1001;
    f.deliver(request); f.step();
    assert(ackCount() == 2 && peer.replies(f.bot.publicKey(), f.radio) == responses &&
           f.bot.counters().malformed == malformed);
    return responses;
  };
  assert(receiveDirect("!trace 3:abcdef") ==
         std::vector<std::string>{"Error: Use !trace width:hex (width 1,2,4,8)"});
  assert(receiveDirect("!@bad ping") ==
         std::vector<std::string>{"Error: Use !@BOTKEY8 COMMAND or !@FULLKEY COMMAND"});
  assert(receiveDirect("hello").empty());
  assert(receiveDirect("").empty());
  assert(receiveDirect("!unknown-command") ==
         std::vector<std::string>{"Error: unknown command !unknown-command; use !help"});
}
static void collected_path_segments() {
  Fixture f; f.start();
  Peer peer; f.learn(peer);
  const auto request = peer.command(f.bot.publicKey(), "!mt 5", 3, {0xa1, 0xa2, 0xa3, 0xb1, 0xb2, 0xb3});
  f.deliver(request); f.step(10);
  auto other = request;
  const uint8_t second[] = {0xc1, 0xc2, 0xc3, 0xd1, 0xd2, 0xd3};
  memcpy(other.data() + 2, second, sizeof(second));
  f.deliver(other); f.step(10);
  f.deliver(request); f.step(10);
  assert(peer.replies(f.bot.publicKey(), f.radio).empty());
  timeMs += 5000; f.step();
  const auto replies = peer.replies(f.bot.publicKey(), f.radio);
  assert(replies.size() == 1 && replies[0] ==
         "2 unique paths in 5000 ms; a1a2a3,b1b2b3 | c1c2c3,d1d2d3");
  assert(replies[0].size() <= BotReplyLimit && f.bot.counters().observationsDropped == 0);
  puts("PASS !mt 5 native two-path reply: actual three-byte header segments, deduplicated and bounded");
}
static void deferred_receive_metadata() {
  const Bytes unrelated{
      uint8_t(PAYLOAD_TYPE_RAW_CUSTOM << 2 | ROUTE_TYPE_DIRECT), 0, 0x5a};
  for (const bool localInterloper : {false, true}) {
    Fixture f; f.start();
    Peer peer; f.learn(peer);
    f.radio.score = 0; // Native flood receive delay is 60 ms at 10 ms airtime.
    const auto interleave = [&]() {
      if (localInterloper) f.reflect(unrelated);
      else {
        f.deliver(unrelated, -47.25f, 11.5f);
        f.step(1);
      }
    };
    const auto test = peer.command(f.bot.publicKey(), "!test", 1, {0xa1});
    const unsigned scored = f.radio.scored;
    f.deliver(test, -103.5f, -4.25f);
    f.step(1);
    assert(f.radio.scored == scored + 1);
    interleave();
    assert(f.bot.counters().replies == 0);
    timeMs += 100; f.step();
    auto replies = peer.replies(f.bot.publicKey(), f.radio);
    assert(replies.size() == 1 &&
           replies[0] == "Connected; path 1:a1; RSSI=-103.5 dBm SNR=-4.25 dB");

    timeMs += 61000; f.radio.sent.clear();
    const auto mt = peer.command(f.bot.publicKey(), "!mt 1", 1, {0xa1});
    f.deliver(mt); f.step(1);
    interleave();
    timeMs += 100; f.step(1);
    auto secondPath = mt;
    secondPath[2] = 0xb2;
    f.deliver(secondPath); f.step(1);
    interleave();
    timeMs += 100; f.step(1);
    assert(f.bot.counters().duplicates == 1);
    auto reflectedPath = mt;
    reflectedPath[2] = 0xee;
    f.reflect(reflectedPath);
    assert(f.bot.counters().duplicates == 2);
    assert(peer.replies(f.bot.publicKey(), f.radio).empty());
    timeMs += 1000; f.step();
    replies = peer.replies(f.bot.publicKey(), f.radio);
    assert(replies.size() == 1 && replies[0] == "2 unique paths in 1000 ms; 1:a1 | 1:b2");
    assert(f.bot.counters().observationsDropped == 0);

    timeMs += 61000; f.radio.sent.clear();
    f.reflect(peer.command(f.bot.publicKey(), "!test", 1, {0xc3}));
    f.step();
    replies = peer.replies(f.bot.publicKey(), f.radio);
    assert(replies.size() == 1 &&
           replies[0] == "Connected; path 1:c3; local; RSSI/SNR unavailable");
  }
  puts("PASS deferred flood receive metadata survives unrelated RF/local packets; "
       "!test signal and !mt RF-only paths remain packet-specific");
}
static void worker_install_hooks_and_budgets() {
  Fixture f; f.start();
  Peer peer; f.learn(peer);
  const char *custom = "function forecast(text) reply('weather:'..text) end "
                       "command('forecast','text:text:154','Test fixture handler') "
                       "function hello(name) reply('Hello '..name) end "
                       "function broken(text) return json.decode(text) end "
                       "function list_memories() reply('fixture memory') end "
                       "command('read-memories','','Read fixture','list_memories')";
  assert(f.bot.stageSource(custom, strlen(custom)));
  assert(!f.bot.activateStaged());
  f.step();
  BotWorker::Result result;
  assert(f.bot.pollSourceResult(result) && result.ok);
  assert(f.bot.activateStaged()); f.step();
  assert(f.bot.pollSourceResult(result) && result.ok);
  assert(f.command(peer, "!forecast warm") == "weather:warm");
  assert(f.command(peer, "!hello slepp") == "Hello slepp");
  assert(f.command(peer, "!broken invalid") == "Error: !broken: Invalid bounded JSON text");
  assert(f.command(peer, "!read-memories") == "fixture memory");
  assert(f.command(peer, "!ping") == "Pong");
  assert(f.command(peer, "!help hello").find("name:string:159") != std::string::npos);
  assert(f.command(peer, "!hello").find("Error:") == 0);
  const auto failures = f.bot.counters().vmFailures;
  const char *maximum = "function x(text) reply(text..'END') end command('x','text:text:159','Capacity')";
  assert(f.bot.stageSource(maximum, strlen(maximum))); f.step();
  assert(f.bot.pollSourceResult(result) && result.ok);
  assert(f.bot.activateStaged()); f.step();
  assert(f.bot.pollSourceResult(result) && result.ok);
  uint8_t secret[32];
  peer.self_id.calcSharedSecret(secret, f.bot.publicKey());
  const Bytes longestPath(MAX_PATH_SIZE, 0xaa);
  auto *learned = peer.createPathReturn(mesh::Identity(f.bot.publicKey()), secret,
                                        longestPath.data(), 0x60, PAYLOAD_TYPE_RAW_CUSTOM, secret, 0);
  assert(learned);
  learned->header |= ROUTE_TYPE_DIRECT; learned->path_len = 0;
  f.deliver(peer.wire(learned)); f.step();
  const auto maxCommand = "!x " + std::string(BotArgumentsLimit, 'a');
  const auto maxReply = f.command(peer, maxCommand.c_str(), 2, Bytes(MAX_PATH_SIZE, 0xaa));
  assert(maxReply == std::string(BotArgumentsLimit, 'a') + "END" &&
         maxReply.size() == BotReplyLimit);
  assert(std::any_of(f.radio.sent.begin(), f.radio.sent.end(),
                     [](const Bytes &wire) { return wire.size() == 246; }));
  assert(f.bot.stageSource(custom, strlen(custom))); f.step();
  assert(f.bot.pollSourceResult(result) && result.ok);
  assert(f.bot.activateStaged()); f.step();
  assert(f.bot.pollSourceResult(result) && result.ok);
  assert(f.bot.stageSource("while true do end", 17)); f.step();
  assert(f.bot.pollSourceResult(result) && !result.ok);
  assert(f.bot.activateStaged()); f.step();
  assert(f.bot.pollSourceResult(result) && !result.ok);
  assert(f.command(peer, "!forecast still") == "weather:still");
  const char *loop = "function loop() while true do end end command('loop','','Budget failure')";
  assert(f.bot.stageSource(loop, strlen(loop))); f.step();
  assert(f.bot.pollSourceResult(result) && result.ok);
  assert(f.bot.activateStaged()); f.step();
  assert(f.bot.pollSourceResult(result) && result.ok);
  timeMs += 61000; f.radio.sent.clear();
  f.deliver(peer.command(f.bot.publicKey(), "!loop")); f.step();
  const auto errors = peer.replies(f.bot.publicKey(), f.radio);
  assert(errors.size() == 1 && errors[0].find("Error:") == 0 &&
         f.bot.counters().vmFailures == failures + 1);
  assert(f.bot.stageSource(BotDefaultSource, strlen(BotDefaultSource))); f.step();
  assert(f.bot.pollSourceResult(result) && result.ok);
  assert(f.bot.activateStaged()); f.step();
  assert(f.bot.pollSourceResult(result) && result.ok);
  timeMs += 61000; f.radio.sent.clear(); f.radio.airtime = 181;
  f.deliver(peer.command(f.bot.publicKey(), "!ping")); f.step();
  assert(f.radio.sent.size() == 1 && peer.replies(f.bot.publicKey(), f.radio).empty());
  f.radio.airtime = 180;
  assert(f.command(peer, "!ping") == "Pong"); // exact 360 ms including ACK
  timeMs += 61000; f.radio.sent.clear(); f.radio.airtime = 10;
  Peer peers[5];
  for (auto &p : peers) f.learn(p);
  for (auto &p : peers) { f.deliver(p.command(f.bot.publicKey(), "!ping")); f.step(); }
  assert(f.radio.sent.size() == 10);
  assert(!f.bot.counters().globalLimited && !f.bot.counters().notices);
  assert(peers[4].replies(f.bot.publicKey(), f.radio) == std::vector<std::string>{"Pong"});
  f.deliver(peers[0].command(f.bot.publicKey(), "!ping")); f.step();
  assert(f.radio.sent.size() == 12);
}
static void duplicate_acks_and_malformed_paths() {
  Fixture f; f.start();
  Peer peer; f.learn(peer);
  const auto request = peer.command(f.bot.publicKey(), "!ping");
  f.deliver(request); f.step();
  assert(f.bot.counters().replies == 1);
  timeMs += 1100; f.radio.sent.clear();
  f.deliver(request); f.step();
  assert(f.radio.sent.size() == 1 && f.bot.counters().replies == 1);
  mesh::Packet ack;
  assert(ack.readFrom(f.radio.sent[0].data(), f.radio.sent[0].size()) &&
         ack.getPayloadType() == PAYLOAD_TYPE_PATH);
  f.deliver(request); f.step();
  assert(f.radio.sent.size() == 1 && f.bot.counters().duplicates == 2);
  uint8_t secret[32];
  peer.self_id.calcSharedSecret(secret, f.bot.publicKey());
  const uint8_t shortPath[] = {0x95, 0};
  auto *packet = peer.createDatagram(PAYLOAD_TYPE_TXT_MSG, mesh::Identity(f.bot.publicKey()),
                                     secret, shortPath, sizeof(shortPath));
  assert(packet);
  packet->header = PAYLOAD_TYPE_PATH << 2 | ROUTE_TYPE_DIRECT; packet->path_len = 0;
  f.radio.sent.clear();
  f.deliver(peer.wire(packet)); f.step();
  assert(f.radio.sent.empty());
  assert(f.command(peer, "!ping") == "Pong");
  for (const auto &wire : f.radio.sent) {
    mesh::Packet response;
    assert(response.readFrom(wire.data(), wire.size()));
    if (response.getPayloadType() == PAYLOAD_TYPE_TXT_MSG) assert(response.isRouteFlood());
  }
  puts("PASS duplicate admission ACK without re-execution, bounded ACK retries, malformed encrypted PATH rejected before learning");
}
static void spiffs_source_loading() {
  Fixture f; f.start();
  Peer peer; f.learn(peer);
  const std::string source =
      "function custom(text) reply('SPIFFS:'..text) end command('custom','text:text:150','SPIFFS')";
  uint8_t digest[32];
  auto hash = [&](const std::string &text) {
    mesh::Utils::sha256(digest, sizeof(digest),
                        reinterpret_cast<const uint8_t *>(text.data()), text.size());
  };
  auto write = [&](const std::string &text) {
    auto file = SPIFFS.open(BotStagedSourcePath, "w");
    assert(file && file.write(reinterpret_cast<const uint8_t *>(text.data()), text.size()) == text.size());
    file.flush(); file.close();
    hash(text);
  };
  auto stage = [&](size_t size, bool success, const char *error = nullptr) {
    filesystem_test::readCalls = filesystem_test::bytesRead = filesystem_test::largestRead = 0;
    assert(f.bot.stageSourceFile(size, digest));
    f.step();
    BotWorker::Result result;
    assert(f.bot.pollSourceResult(result) &&
           result.operation == BotWorker::Operation::StageFile && result.ok == success);
    assert(filesystem_test::largestRead <= 256 &&
           filesystem_test::bytesRead <= BotSourceLimit + 1);
    if (error) assert(strstr(result.error, error));
    return result;
  };
  auto activate = [&](bool success) {
    assert(f.bot.activateStaged()); f.step();
    BotWorker::Result result;
    assert(f.bot.pollSourceResult(result) && result.ok == success);
  };
  hash(source);
  assert(!f.bot.stageSourceFile(0, digest));
  assert(!f.bot.stageSourceFile(BotSourceLimit + 1, digest));
  assert(!f.bot.stageSourceFile(source.size(), nullptr));
  stage(source.size(), false, "unavailable");
  activate(false);
  write(source);
  stage(source.size(), true);
  // Activation uses the checked RAM snapshot, not a second mutable file read.
  filesystem_test::files[BotStagedSourcePath][0] ^= 1;
  activate(true);
  assert(f.command(peer, "!custom verified") == "SPIFFS:verified");
  stage(source.size(), false, "SHA-256");
  activate(false);
  assert(f.command(peer, "!custom retained") == "SPIFFS:retained");
  write(source);
  filesystem_test::failOpen = true;
  stage(source.size(), false, "unavailable");
  filesystem_test::failOpen = false;
  activate(false);
  filesystem_test::readLimit = 7;
  stage(source.size(), false, "incomplete");
  filesystem_test::readLimit = std::numeric_limits<size_t>::max();
  activate(false);
  filesystem_test::appendOnRead = true;
  stage(source.size(), false, "grew");
  activate(false);
  write(source);
  filesystem_test::readDelayMs = BotSourceReadBudgetMs;
  const auto late = stage(source.size(), false, "deadline");
  filesystem_test::readDelayMs = 0;
  assert(late.sourceReadMs >= BotSourceReadBudgetMs);
  activate(false);
  write(std::string(BotSourceLimit + 1, ' '));
  stage(BotSourceLimit, false, "length");
  assert(filesystem_test::readCalls == 0);
  activate(false);
  const std::string binary("\x04\x22\x4d\x18\0\0", 6); // A codec frame is not source.
  write(binary);
  stage(binary.size(), false, "source text");
  activate(false);
  write("while true do end");
  stage(17, false, "budget");
  activate(false);
  auto full = source;
  full.resize(BotSourceLimit, ' ');
  write(full);
  stage(full.size(), true);
  assert(filesystem_test::bytesRead == BotSourceLimit && filesystem_test::readCalls == 17);
  activate(true);
  assert(f.command(peer, "!custom bounded") == "SPIFFS:bounded");
  assert(SPIFFS.exists(BotStagedSourcePath)); // The core never edits installer storage.
  f.bot.stop(); f.start(); f.learn(peer);
  assert(f.command(peer, "!ping") == "Pong"); // No unauthenticated boot auto-loading.
  stage(full.size(), true); activate(true);
  assert(f.command(peer, "!custom reloaded") == "SPIFFS:reloaded");
  assert(SPIFFS.remove(BotStagedSourcePath));
  assert(f.command(peer, "!custom snapshot") == "SPIFFS:snapshot");
}
static void retained_worker_state_and_timers() {
  Fixture f; f.start();
  Peer first, second; f.learn(first); f.learn(second);
  const char *source =
      "counter=0 "
      "function memorize(note) kv.put('memory',note) return 'committed' end "
      "command('memorize','note:text:100','Save memory') "
      "function list_memories() return kv.get('memory') or 'empty' end "
      "command('read-memories','','Recall','list_memories') "
      "function delayed(name) local sender=ctx.sender.public_key "
      "counter=counter+1 timer.sleep(500) "
      "if ctx.sender.public_key~=sender then return 'WRONG PRINCIPAL' end "
      "return ping()..name..tostring(counter) end";
  auto install = [&]() {
    assert(f.bot.stageSource(source, strlen(source))); f.step();
    BotWorker::Result result;
    assert(f.bot.pollSourceResult(result) && result.ok);
    assert(f.bot.activateStaged()); f.step();
    assert(f.bot.pollSourceResult(result) && result.ok);
  };
  install();
  assert(f.command(first, "!memorize get groceries") == "committed");
  assert(f.command(first, "!read-memories") == "get groceries");
  assert(f.command(second, "!read-memories") == "empty");
  identity_test::failCommit = true;
  assert(f.command(first, "!memorize not committed").find("outcome unknown") != std::string::npos);
  identity_test::failCommit = false;
  assert(f.command(first, "!read-memories") == "get groceries");
  install();
  assert(f.command(first, "!read-memories") == "get groceries");
  f.bot.stop(); f.start(); f.learn(first); f.learn(second); install();
  assert(f.command(first, "!read-memories") == "get groceries");
  timeMs += 61000; f.radio.sent.clear();
  f.deliver(first.command(f.bot.publicKey(), "!delayed A"));
  f.step(30);
  f.deliver(second.command(f.bot.publicKey(), "!delayed B"));
  f.step(30);
  assert(first.replies(f.bot.publicKey(), f.radio).empty());
  assert(second.replies(f.bot.publicKey(), f.radio).empty());
  f.step(280);
  const auto a = first.replies(f.bot.publicKey(), f.radio);
  const auto b = second.replies(f.bot.publicKey(), f.radio);
  assert(a.size() == 1 && a[0] == "PongA2");
  assert(b.size() == 1 && b[0] == "PongB2");
  timeMs += 61000; f.radio.sent.clear();
  f.deliver(first.command(f.bot.publicKey(), "!delayed cancel"));
  f.step(20);
  install();
  f.step(300);
  const auto cancelled = first.replies(f.bot.publicKey(), f.radio);
  assert(cancelled.size() == 1 &&
         cancelled[0] == "Error: !delayed: Runtime code changed; pending operation outcome may be unknown");
  const char *hungry =
      "retained={} function hungry() for i=1,10000 do retained[i]={i,i,i,i} end end";
  assert(f.bot.stageSource(hungry, strlen(hungry))); f.step();
  BotWorker::Result result;
  assert(f.bot.pollSourceResult(result) && result.ok);
  assert(f.bot.activateStaged()); f.step();
  assert(f.bot.pollSourceResult(result) && result.ok);
  assert(f.command(first, "!hungry").find("Error:") == 0);
  assert(f.command(first, "!ping") == "Pong");
  install();
  assert(f.command(first, "!read-memories") == "get groceries");
  puts("PASS retained native jobs: cross-command committed KV, private callers, restart/replacement, "
       "failed commit, shared globals, nested timers, encrypted reply ownership, cancellation and recovery heap");
}

static void durable_store_limits() {
  BotStore store;
  uint8_t bot[32]{0x42};
  std::atomic<uint32_t> generation{9};
  std::atomic<uint32_t> sharedGrant{1};
  std::atomic<bool> shared{false};
  BotIoRequest request{};
  request.token = {9, 1, 1}; request.principal[0] = 1; request.grant = 1;
  request.kind = BotIoRequest::Put;
  strcpy(request.key, "shared-memory"); strcpy(request.value, "one");
  BotIoResult result;
  const auto perform = [&]() { store.perform(bot, request, result, generation, shared, sharedGrant); };
  perform(); assert(result.ok);
  request.kind = BotIoRequest::Get;
  request.scope = BotIoRequest::Conversation; perform(); assert(result.ok && !result.found);
  request.scope = BotIoRequest::Caller;
  request.principal[0] = 2; perform(); assert(result.ok && !result.found);
  request.principal[0] = 1; perform(); assert(result.ok && !strcmp(result.value, "one"));
  request.kind = BotIoRequest::Delete; perform(); assert(result.ok);
  request.kind = BotIoRequest::Get; perform(); assert(result.ok && !result.found);
  request.scope = BotIoRequest::Bot; memset(request.principal, 0, 32);
  request.kind = BotIoRequest::Put; perform(); assert(!result.ok);
  shared = true; perform(); assert(result.ok);
  shared = false; request.kind = BotIoRequest::Get; perform(); assert(!result.ok);
  request.scope = BotIoRequest::Caller; request.principal[0] = 1;
  request.kind = BotIoRequest::Put;
  for (unsigned i = 0; i < 8; ++i) {
    snprintf(request.key, sizeof(request.key), "entry%u", i);
    perform(); assert(result.ok);
  }

  strcpy(request.key, "overflow"); perform(); assert(!result.ok && strstr(result.error, "capacity"));
  request.kind = BotIoRequest::Get; strcpy(request.key, "entry0");
  identity_test::failRead = true; perform(); assert(!result.ok);
  identity_test::failRead = false;
  for (auto &entry : identity_test::durable) if (entry.first.first == "mc-bot-kv") {
    entry.second.back() ^= 1;
    perform(); assert(!result.ok && strstr(result.error, "corrupt"));
    entry.second.back() ^= 1;
    break;
  }
  perform(); assert(result.ok && result.found);
  ++generation; perform(); assert(!result.ok && strstr(result.error, "cancelled"));
}

static void personal_notes() {
  assert(saveBotRadioPolicy({}));
  Fixture f; f.start();
  Peer first, second; f.learn(first); f.learn(second);
  for (unsigned width : {1u, 2u, 3u}) {
    const auto key = "route" + std::to_string(width);
    const auto command = "!remember " + key + " first caller";
    assert(f.command(first, command.c_str(), width) == "Note " + key + " created; committed");
    assert(f.command(second, ("!recall " + key).c_str(), width) == "No note: " + key);
    assert(f.command(first, ("!recall " + key).c_str(), width) == "first caller");
  }
  assert(f.command(first, "!notes") == "Notes (3): [route1] [route2] [route3]");
  assert(f.command(first, "!list-memories route2") == "Notes (1): [route2]");
  assert(f.command(first, "!remember route1 new text") == "Note route1 replaced; committed");
  assert(f.command(second, "!remember route1 private second") == "Note route1 created; committed");
  const char *source =
      "function setnote(text) local ok,state=kv.put('shared',text) return state end "
      "function together() remember('nested','cooperating') return recall('nested') end "
      "function shared_board() kv.put('shared','channel board','channel') return 'committed board' end";
  const auto install = [&] {
    BotWorker::Result done;
    assert(f.bot.stageSource(source, strlen(source))); f.step();
    assert(f.bot.pollSourceResult(done) && done.ok);
    assert(f.bot.activateStaged()); f.step();
    assert(f.bot.pollSourceResult(done) && done.ok);
  };
  install();
  assert(f.command(first, "!setnote script-created") == "created");
  assert(f.command(first, "!recall shared") == "script-created");
  assert(f.command(first, "!together") == "cooperating");
  install();
  assert(f.command(first, "!recall nested") == "cooperating");
  f.bot.stop(); f.start(); f.learn(first); f.learn(second); install();
  assert(f.command(first, "!recall route1") == "new text");
  assert(f.command(second, "!recall route1") == "private second");
  timeMs += 61000; f.radio.sent.clear();
  f.deliver(first.command(f.bot.publicKey(), "!remember simultaneous alpha"));
  f.deliver(second.command(f.bot.publicKey(), "!remember simultaneous beta"));
  f.step(160);
  for (auto *peer : {&first, &second}) {
    const auto replies = peer->replies(f.bot.publicKey(), f.radio);
    assert(replies.size() == 1 && replies[0] == "Note simultaneous created; committed");
  }
  assert(f.command(first, "!recall simultaneous") == "alpha");
  assert(f.command(second, "!recall simultaneous") == "beta");
  identity_test::failCommit = true;
  assert(f.command(first, "!remember route1 uncertain").find("outcome unknown") != std::string::npos);
  identity_test::failCommit = false;
  assert(f.command(first, "!recall route1") == "new text");
  identity_test::afterCommit = [] { identity_test::failRead = true; };
  assert(f.command(first, "!remember route1 readback lost").find("outcome unknown") != std::string::npos);
  identity_test::afterCommit = nullptr; identity_test::failRead = false;
  assert(f.command(first, "!recall route1") == "readback lost");
  identity_test::failRead = true;
  assert(f.command(first, "!notes").find("Error:") == 0);
  identity_test::failRead = false;
  assert(f.command(first, "!forget route1") == "Note route1 deleted; committed");
  assert(f.command(first, "!forget route1") == "No note: route1");
  assert(f.command(second, "!recall route1") == "private second");
  timeMs += 61000; f.radio.sent.clear();
  const auto packet = first.command(f.bot.publicKey(), "!remember once deduplicated");
  f.deliver(packet); f.step(); const auto commits = identity_test::commits;
  f.deliver(packet); f.step();
  const auto replies = first.replies(f.bot.publicKey(), f.radio);
  assert(replies.size() == 1 && replies[0] == "Note once created; committed" &&
         identity_test::commits == commits);
  BotRadioPolicy policy; strcpy(policy.channel, "#example1"); policy.pathWidth = 3;
  f.bot.stop(); assert(saveBotRadioPolicy(policy) && saveBotSharedState(true));
  f.start(); install(); f.learn(first);
  const auto group = [&](const char *text) {
    timeMs += 61000; f.radio.sent.clear();
    f.deliver(first.targetedGroup(f.bot.publicKey(), text, policy.channel)); f.step();
    const auto output = first.groupReplies(f.radio, 3, policy.channel);
    assert(output.size() == 1); return output.front();
  };
  assert(group("owner: !shared_board") == "committed board");
  for (const char *command : {"owner: !remember shared steal", "owner: !recall shared",
                              "owner: !forget shared", "owner: !notes", "owner: !list-memories"}) {
    const auto denied = group(command);
    assert(denied.find("permission not granted: send this command by DM") != std::string::npos);
  }
  assert(f.command(first, "!recall shared") == "script-created");
  assert(saveBotRadioPolicy({}) && saveBotSharedState(false));
  puts("PASS encrypted personal notes: built-in Lua/shared KV, 1/2/3-byte routes, concurrent full-key callers, source/reboot retention, explicit overwrite/delete/unknown, dedup and verified channel rejection");
}

static void retained_native_radio_io() {
  Fixture f; f.start();
  Peer first, second;
  for (unsigned i = 0; first.self_id.pub_key[0] != f.bot.publicKey()[0] && i < 4096; ++i)
    first.self_id = mesh::LocalIdentity(&first.rng);
  assert(first.self_id.pub_key[0] == f.bot.publicKey()[0]);
  f.learn(first); f.learn(second);
  const char *source =
      "function exchange() "
      "local draft=mesh.compose{kind='dm',to=ctx.sender.public_key,text='question'} "
      "local tx=mesh.send(draft) "
      "if not tx.queued or not tx.transmitted or tx.acknowledged then return 'BAD TX' end "
      "local ack=mesh.wait{kind='ack',timeout_ms=1000} "
      "if not ack.acknowledged or ack.authenticated or not ack.queued or not ack.transmitted "
      "or ack.job~=tx.job then return 'BAD ACK' end "
      "local packet=mesh.wait{kind='text',prefix='answer:',timeout_ms=1000} "
      "if not packet.authenticated then return 'BAD SENDER' end "
      "return packet.text end "
      "function timeout() mesh.wait{kind='text',timeout_ms=10} end "
      "function overflow() sleep(80) local p=mesh.wait{kind='text',timeout_ms=100} "
      "if p.truncated then return 'truncated='..p.text end return 'BAD OVERFLOW' end "
      "function sendonly() mesh.send(mesh.compose{text='request'}) return 'sent' end "
      "function testall() local greeting=ping() local result=trace('1:42') sleep(10) "
      "return greeting..'; '..result end "
      "function announce() return advert() end "
      "function echo_wait() mesh.send(mesh.compose{text='answer:reflection'}) "
      "return mesh.wait{kind='text',prefix='answer:',timeout_ms=100}.text end "
      "function holding() return mesh.wait{kind='text',timeout_ms=1000}.text end";
  assert(f.bot.stageSource(source, strlen(source))); f.step();
  BotWorker::Result result;
  assert(f.bot.pollSourceResult(result) && result.ok);
  assert(f.bot.activateStaged()); f.step();
  assert(f.bot.pollSourceResult(result) && result.ok);
  timeMs += 61000; f.radio.sent.clear();
  f.deliver(first.command(f.bot.publicKey(), "!exchange")); f.step(25);
  auto messages = first.replies(f.bot.publicKey(), f.radio);
  assert(messages.size() == 1 && messages[0] == "question");
  const auto ack = first.ackLast(f.bot.publicKey(), f.radio);
  f.deliver(second.command(f.bot.publicKey(), "answer:wrong"));
  f.deliver(first.command(f.bot.publicKey(), "ignore"));
  f.deliver(first.command(f.bot.publicKey(), "answer:correct")); f.step(15);
  f.deliver(ack); f.step(60);
  messages = first.replies(f.bot.publicKey(), f.radio);
  assert(messages.size() == 2 && messages[1] == "answer:correct");
  timeMs += 61000; f.radio.sent.clear();
  f.deliver(first.command(f.bot.publicKey(), "!testall")); f.step(25);
  bool returned = false;
  for (const auto &raw : f.radio.sent) {
    mesh::Packet packet; assert(packet.readFrom(raw.data(), raw.size()));
    if (packet.getPayloadType() != PAYLOAD_TYPE_TRACE) continue;
    assert(packet.payload_len == 10 && packet.payload[9] == 0x42);
    packet.path_len = 1; packet.path[0] = uint8_t(-4);
    uint8_t bytes[255];
    packet.payload[4] ^= 1;
    auto size = packet.writeTo(bytes);
    f.deliver(Bytes(bytes, bytes + size)); f.step(10);
    assert(first.replies(f.bot.publicKey(), f.radio).empty());
    packet.payload[4] ^= 1;
    size = packet.writeTo(bytes);
    f.deliver(Bytes(bytes, bytes + size));
    returned = true; break;
  }
  assert(returned); f.step();
  messages = first.replies(f.bot.publicKey(), f.radio);
  assert(messages.size() == 1 && messages[0] == "Pong; Trace 1 hops; SNR -1.00");
  timeMs += 900001;
  assert(f.command(first, "!announce") == "Advert TX completed");
  assert(f.command(first, "!announce").find("advert unavailable") != std::string::npos);
  assert(f.command(first, "!timeout").find("timed out") != std::string::npos);
  timeMs += 61000; f.radio.sent.clear();
  f.deliver(first.command(f.bot.publicKey(), "!echo_wait")); f.step(20);
  Bytes reflection;
  for (const auto &raw : f.radio.sent) {
    mesh::Packet packet; assert(packet.readFrom(raw.data(), raw.size()));
    if (packet.getPayloadType() == PAYLOAD_TYPE_TXT_MSG) reflection = raw;
  }
  assert(!reflection.empty());
  f.deliver(reflection); f.step(70);
  messages = first.replies(f.bot.publicKey(), f.radio);
  assert(messages.size() == 2 && messages[1].find("timed out") != std::string::npos);
  timeMs += 61000; f.radio.sent.clear();
  f.deliver(first.command(f.bot.publicKey(), "!overflow")); f.step(10);
  for (unsigned i = 0; i < 5; ++i) {
    const auto text = "item" + std::to_string(i);
    f.deliver(first.command(f.bot.publicKey(), text.c_str())); f.step(2);
  }
  f.step(60);
  messages = first.replies(f.bot.publicKey(), f.radio);
  assert(messages.size() == 1 && messages[0] == "truncated=item0");
  f.radio.rejectTx = true;
  timeMs += 61000; f.radio.sent.clear();
  const auto failures = f.bot.counters().vmFailures;
  f.deliver(first.command(f.bot.publicKey(), "!sendonly")); f.step();
  assert(f.bot.counters().vmFailures > failures);
  f.radio.rejectTx = false;
  assert(f.command(first, "!ping") == "Pong");
  for (unsigned i = 0; i <= BotJobLimit; ++i) {
    timeMs += 61000; f.radio.sent.clear();
    f.deliver(first.command(f.bot.publicKey(), "!holding")); f.step(20);
    assert(f.bot.stageSource(source, strlen(source))); f.step();
    assert(f.bot.pollSourceResult(result) && result.ok);
    assert(f.bot.activateStaged()); f.step();
    assert(f.bot.pollSourceResult(result) && result.ok);
    messages = first.replies(f.bot.publicKey(), f.radio);
    assert(messages.size() == 1 &&
           messages[0] == "Error: !holding: Runtime code changed; pending operation outcome may be unknown");
  }
  timeMs += 61000; f.radio.sent.clear();
  f.deliver(first.command(f.bot.publicKey(), "!sendonly")); f.step();
  messages = first.replies(f.bot.publicKey(), f.radio);
  assert(messages.size() == 2 && messages[0] == "request" && messages[1] == "sent");
  puts("PASS native mesh I/O: opaque caller DM, queued/TX/ACK distinction, early filtered receive, "
       "authenticated sender, queue truncation, correlated TRACE, nested timer, advert TX, timeout and TX failure");
}

static void authorized_packet_forwarding() {
  assert(saveBotForwardPolicy({}));
  Fixture f; f.start(); Peer sender, recipient, stranger;
  f.learn(sender); f.learn(recipient); f.learn(stranger);
  const char *source =
      "function relay() local p=mesh.wait{kind='text',timeout_ms=500} "
      "local tx=mesh.forward(p) if not tx.queued or not tx.transmitted or tx.acknowledged then return 'BAD TX' end "
      "return 'forwarded' end "
      "function delayed() local p=mesh.wait{kind='text',timeout_ms=500} sleep(100) mesh.forward(p) return 'bad' end "
      "function twice() local p=mesh.wait{kind='text',timeout_ms=500} mesh.forward(p) mesh.forward(p) return 'bad' end "
      "function overlap(delay) sleep(delay=='long' and 2000 or 1) local p=mesh.wait{kind='text',timeout_ms=30000} "
      "mesh.forward(p) return 'forwarded' end "
      "function replaced() local p=mesh.wait{kind='text'} mesh.wait{kind='text'} mesh.forward(p) return 'bad' end";
  BotWorker::Result result;
  assert(f.bot.stageSource(source, strlen(source))); f.step();
  assert(f.bot.pollSourceResult(result) && result.ok && f.bot.activateStaged()); f.step();
  assert(f.bot.pollSourceResult(result) && result.ok);
  auto start = [&](Peer &caller, const char *command = "!relay") {
    timeMs += 61000; f.radio.sent.clear();
    f.deliver(caller.command(f.bot.publicKey(), command)); f.step(12);
  };
  auto finish = [&](Peer &caller, const char *text, uint8_t width = 1, Bytes path = {}) {
    f.deliver(caller.command(f.bot.publicKey(), text, width, path)); f.step();
    const auto messages = caller.replies(f.bot.publicKey(), f.radio);
    assert(messages.size() == 1);
    return messages.front();
  };
  start(sender);
  assert(finish(sender, "no grant").find("operator pair grant") != std::string::npos);
  assert(recipient.replies(f.bot.publicKey(), f.radio).empty());
  BotForwardPolicy grant;
  memcpy(grant.from, sender.self_id.pub_key, 32); memcpy(grant.to, recipient.self_id.pub_key, 32);
  assert(f.bot.setForwardPolicy(grant) && f.bot.forwardAccess());
  BotForwardPolicy saved;
  assert(loadBotForwardPolicy(saved) && !memcmp(saved.from, grant.from, 32) && !memcmp(saved.to, grant.to, 32));
  start(sender);
  assert(finish(sender, "no route").find("no fresh authenticated direct route") != std::string::npos);
  assert(recipient.replies(f.bot.publicKey(), f.radio).empty());
  std::string attributed = "Fwd ";
  char hex[3];
  for (auto byte : sender.self_id.pub_key) { snprintf(hex, sizeof(hex), "%02x", byte); attributed += hex; }
  attributed += ": hello";
  for (uint8_t width = 1; width <= 3; ++width) {
    Bytes route(width * 2, uint8_t(f.bot.publicKey()[0] ^ 0x5a));
    f.deliver(recipient.pathReturn(f.bot.publicKey(), width, route)); f.step(10);
    start(sender);
    const auto text = sender.command(f.bot.publicKey(), "hello", width, route);
    f.deliver(text); f.deliver(text); f.step();
    assert(sender.replies(f.bot.publicKey(), f.radio) == std::vector<std::string>{"forwarded"});
    assert(recipient.replies(f.bot.publicKey(), f.radio) == std::vector<std::string>{attributed});
    for (const auto &raw : f.radio.sent) {
      mesh::Packet packet; assert(packet.readFrom(raw.data(), raw.size()));
      if (packet.getPayloadType() != PAYLOAD_TYPE_TXT_MSG || packet.payload[0] != recipient.self_id.pub_key[0]) continue;
      assert(packet.isRouteDirect() && packet.getPathHashSize() == width &&
             packet.getPathByteLen() == route.size() && !memcmp(packet.path, route.data(), route.size()));
    }
  }
  start(stranger);
  assert(finish(stranger, "wrong sender").find("operator pair grant") != std::string::npos);
  start(sender);
  assert(finish(sender, "Fwd already relayed").find("eligible copied DM") != std::string::npos);
  assert(recipient.replies(f.bot.publicKey(), f.radio).empty());
  start(sender);
  assert(finish(sender, "loop", 3, Bytes(f.bot.publicKey(), f.bot.publicKey() + 3)).find("eligible copied DM") != std::string::npos);
  start(sender);
  assert(finish(sender, std::string(93, 'a').c_str()).find("attribution exceeds") != std::string::npos);
  start(sender, "!twice");
  assert(finish(sender, "only once").find("eligible copied DM") != std::string::npos);
  assert(recipient.replies(f.bot.publicKey(), f.radio).size() == 1);
  start(sender, "!replaced");
  f.deliver(sender.command(f.bot.publicKey(), "old")); f.step(10);
  assert(finish(sender, "new").find("latest eligible copied DM") != std::string::npos);
  assert(recipient.replies(f.bot.publicKey(), f.radio).empty());
  f.deliver(recipient.pathReturn(f.bot.publicKey(), 1, {})); f.step(10);
  start(sender, "!delayed");
  f.deliver(sender.command(f.bot.publicKey(), "revoked")); f.step(10);
  identity_test::failCommit = true;
  assert(!f.bot.setForwardPolicy({}) && !f.bot.forwardAccess());
  identity_test::failCommit = false;
  assert(f.bot.setForwardPolicy(grant)); f.step();
  auto messages = sender.replies(f.bot.publicKey(), f.radio);
  assert(messages.size() == 1 && messages[0].find("revoked") != std::string::npos);
  assert(recipient.replies(f.bot.publicKey(), f.radio).empty());
  start(sender, "!delayed");
  f.deliver(sender.command(f.bot.publicKey(), "cancelled")); f.step(10);
  assert(f.bot.stageSource(source, strlen(source))); f.step(5);
  assert(f.bot.pollSourceResult(result) && result.ok && f.bot.activateStaged()); f.step();
  assert(f.bot.pollSourceResult(result) && result.ok);
  assert(recipient.replies(f.bot.publicKey(), f.radio).empty());
  f.deliver(recipient.pathReturn(f.bot.publicKey(), 1, {})); f.step(10);
  start(sender, "!overlap long");
  timeMs += 2001; f.step(10);
  timeMs += 28001;
  f.deliver(sender.command(f.bot.publicKey(), "!overlap short")); f.step(10);
  f.deliver(sender.command(f.bot.publicKey(), "same packet")); f.step();
  messages = sender.replies(f.bot.publicKey(), f.radio);
  assert(messages.size() == 2 && recipient.replies(f.bot.publicKey(), f.radio).size() == 1);
  assert(std::count(messages.begin(), messages.end(), "forwarded") == 1);
  assert(std::any_of(messages.begin(), messages.end(), [](const std::string &text) {
    return text.find("already forwarded/attempted") != std::string::npos;
  }));
  f.deliver(recipient.pathReturn(f.bot.publicKey(), 1, Bytes{f.bot.publicKey()[0]})); f.step(10);
  start(sender);
  assert(finish(sender, "loop route").find("route loops") != std::string::npos);
  timeMs += 600001;
  start(sender);
  assert(finish(sender, "stale route").find("no fresh authenticated direct route") != std::string::npos);
  f.deliver(recipient.pathReturn(f.bot.publicKey(), 1, {})); f.step(10);
  f.learn(recipient);
  start(sender);
  const auto reflection = sender.command(f.bot.publicKey(), "local reflection");
  f.reflect(reflection); f.step(260);
  assert(recipient.replies(f.bot.publicKey(), f.radio).empty());
  assert(f.bot.setForwardPolicy(grant));
  f.bot.stop(); assert(f.bot.begin(f.mux)); f.step();
  assert(f.bot.forwardAccess());
  auto own = grant; memcpy(own.to, f.bot.publicKey(), 32);
  assert(!f.bot.setForwardPolicy(own));
  assert(f.bot.setForwardPolicy({}) && !f.bot.forwardAccess());
  identity_test::failRead = true;
  assert(!f.bot.setForwardPolicy(grant) && !f.bot.forwardAccess());
  identity_test::failRead = false;
  assert(loadBotForwardPolicy(saved) && saved.enabled());
  assert(f.bot.setForwardPolicy({}));
  auto &record = identity_test::durable[{"mc-onchip", "bot-forward"}];
  record[0] ^= 1;
  assert(!loadBotForwardPolicy(saved) && !saved.enabled());
  assert(saveBotForwardPolicy({}));
  puts("PASS native forwarding: full-key pair grant, fresh direct 1/2/3-byte routes, attributed re-encryption, "
       "no flood/private-channel fallback, copied/one-shot handles, loop/reflection/duplicate rejection, "
       "failed revoke/regrant fences, source cancellation and persisted policy");
}

static void filtered_packet_continuations() {
  Fixture f; f.start(); Peer first, second;
  f.learn(first); f.learn(second);
  const char *source =
      "function collect() "
      "local a=mesh.wait{kind='text',exact='answer',from=ctx.sender.public_key,path_width=3,route='flood',timeout_ms=500} "
      "local b=mesh.wait{kind='text',exact='early',timeout_ms=500} "
      "local c=mesh.wait{kind='text',exact='last',timeout_ms=500} "
      "if not a.authenticated or not a.path_known or a.path_width~=3 or not a.measured then return 'BAD META' end "
      "return a.text..':'..b.text..':'..c.text end "
      "function split() local tx=mesh.send(mesh.compose{kind='trace',route='2:aabb'}) "
      "if not tx.queued or not tx.transmitted or tx.authenticated then return 'BAD TX' end sleep(80) "
      "local p=mesh.wait{kind='trace',timeout_ms=100} "
      "if not p.correlated or p.authenticated or p.hops~=1 or p.snr[1]~=-1 then return 'BAD TRACE' end return p.text end "
      "function multitrace() return mesh.multitrace('1:42',2) end "
      "function testall() local p=ping() local r=multitrace() sleep(10) return p..'; '..r end "
      "function sendonly() local t=mesh.send(mesh.compose{text='queued request'}) "
      "if not t.queued or not t.transmitted or t.acknowledged then return 'BAD TX' end return 'terminal TX' end "
      "function saturated() sleep(150) return mesh.wait{kind='text',exact='missing',timeout_ms=500}.text end "
      "function advert_ack() local t=mesh.send(mesh.compose{text='ack request'}) mesh.advert() "
      "local a=mesh.wait{kind='ack',timeout_ms=500} "
      "if not a.acknowledged or a.job~=t.job then return 'BAD ACK JOB' end return 'ACK owns DM' end "
      "function once() local a=mesh.wait{kind='text',exact='once',timeout_ms=500} "
      "mesh.send(mesh.compose{text='got one'}) return mesh.wait{kind='text',exact='once',timeout_ms=50}.text end";
  BotWorker::Result result;
  assert(f.bot.stageSource(source, strlen(source))); f.step();
  assert(f.bot.pollSourceResult(result) && result.ok && f.bot.activateStaged()); f.step();
  assert(f.bot.pollSourceResult(result) && result.ok);
  timeMs += 61000; f.radio.sent.clear();
  f.deliver(first.command(f.bot.publicKey(), "!collect")); f.step(10);
  f.deliver(second.command(f.bot.publicKey(), "!collect")); f.step(10);
  f.deliver(first.command(f.bot.publicKey(), "early")); f.step(5);
  f.deliver(second.command(f.bot.publicKey(), "early")); f.step(5);
  f.deliver(first.command(f.bot.publicKey(), "answer:", 3, {1,2,3})); f.step(5);
  f.deliver(first.command(f.bot.publicKey(), "answer", 2, {1,2})); f.step(5);
  f.deliver(second.command(f.bot.publicKey(), "answer", 3, {1,2,3})); f.step(10);
  f.deliver(second.command(f.bot.publicKey(), "last")); f.step(15);
  assert(first.replies(f.bot.publicKey(), f.radio).empty());
  assert(second.replies(f.bot.publicKey(), f.radio) == std::vector<std::string>{"answer:early:last"});
  f.deliver(first.command(f.bot.publicKey(), "answer", 3, {4,5,6})); f.step(10);
  f.deliver(first.command(f.bot.publicKey(), "last")); f.step();
  assert(first.replies(f.bot.publicKey(), f.radio) == std::vector<std::string>{"answer:early:last"});
  timeMs += 61000; f.radio.sent.clear();
  f.deliver(first.command(f.bot.publicKey(), "!once")); f.step(10);
  const auto duplicate = first.command(f.bot.publicKey(), "once");
  f.deliver(duplicate); f.step(15); f.deliver(duplicate); f.step(50);
  const auto messages = first.replies(f.bot.publicKey(), f.radio);
  assert(messages.size() == 2 && messages[0] == "got one" && messages[1].find("timed out") != std::string::npos);
  auto traceReply = [&](const Bytes &raw, bool wrong = false) {
    mesh::Packet packet; assert(packet.readFrom(raw.data(), raw.size()));
    const unsigned width = 1u << packet.payload[8];
    packet.path_len = (packet.payload_len - 9) / width;
    memset(packet.path, uint8_t(-4), packet.path_len);
    if (wrong) packet.payload[4] ^= 1;
    uint8_t wire[255]; const size_t size = packet.writeTo(wire);
    return Bytes(wire, wire + size);
  };
  auto traces = [&]() {
    std::vector<Bytes> result;
    for (const auto &raw : f.radio.sent) {
      mesh::Packet packet; assert(packet.readFrom(raw.data(), raw.size()));
      if (packet.getPayloadType() == PAYLOAD_TYPE_TRACE) result.push_back(raw);
    }
    return result;
  };
  timeMs += 61000; f.radio.sent.clear();
  f.deliver(first.command(f.bot.publicKey(), "!split")); f.step(15);
  auto sent = traces(); assert(sent.size() == 1);
  f.deliver(traceReply(sent[0], true)); f.step(5);
  f.deliver(traceReply(sent[0])); f.deliver(traceReply(sent[0])); f.step(90);
  assert(first.replies(f.bot.publicKey(), f.radio) == std::vector<std::string>{"Trace 1 hops; SNR -1.00"});
  timeMs += 61000; f.radio.sent.clear();
  f.deliver(first.command(f.bot.publicKey(), "!testall")); f.step(15);
  sent = traces(); assert(sent.size() == 1);
  f.deliver(traceReply(sent[0])); f.step(15);
  sent = traces(); assert(sent.size() == 2);
  f.deliver(traceReply(sent[0])); f.step(10);
  assert(first.replies(f.bot.publicKey(), f.radio).empty());
  f.deliver(traceReply(sent[1])); f.step();
  assert(first.replies(f.bot.publicKey(), f.radio) ==
         std::vector<std::string>{"Pong; 1: Trace 1 hops; SNR -1.00 | 2: Trace 1 hops; SNR -1.00"});
  timeMs += 61000; f.radio.sent.clear();
  f.deliver(first.command(f.bot.publicKey(), "!split")); f.step(15);
  sent = traces(); assert(sent.size() == 1); f.step(100);
  const auto timedOut = first.replies(f.bot.publicKey(), f.radio);
  assert(timedOut.size() == 1 && timedOut[0].find("timed out") != std::string::npos);
  f.deliver(traceReply(sent[0])); f.step();
  assert(first.replies(f.bot.publicKey(), f.radio) == timedOut);
  timeMs += 61000; f.radio.sent.clear();
  f.deliver(first.command(f.bot.publicKey(), "!split")); f.step(15);
  sent = traces(); assert(sent.size() == 1);
  assert(f.bot.stageSource(source, strlen(source))); f.step(5);
  assert(f.bot.pollSourceResult(result) && result.ok && f.bot.activateStaged()); f.step(20);
  assert(f.bot.pollSourceResult(result) && result.ok);
  const auto cancelled = first.replies(f.bot.publicKey(), f.radio);
  f.deliver(traceReply(sent[0])); f.step();
  assert(cancelled.size() == 1 &&
         cancelled[0] == "Error: !split: Source changed; pending operation outcome may be unknown" &&
         first.replies(f.bot.publicKey(), f.radio) == cancelled);
  timeMs += 61000; f.radio.sent.clear(); f.radio.airtime = 100; f.radio.holdTx = true;
  f.deliver(first.command(f.bot.publicKey(), "!sendonly")); f.step(20);
  for (const auto &text : first.replies(f.bot.publicKey(), f.radio))
    assert(text != "terminal TX");
  f.radio.holdTx = false; f.step(150); f.radio.airtime = 10;
  assert((first.replies(f.bot.publicKey(), f.radio) == std::vector<std::string>{"queued request", "terminal TX"}));
  timeMs += 61000; f.radio.sent.clear();
  f.deliver(first.command(f.bot.publicKey(), "!saturated")); f.step(5);
  for (unsigned i = 0; i <= BotReceiveDedupLimit; ++i) {
    const auto text = "extra" + std::to_string(i);
    f.deliver(first.command(f.bot.publicKey(), text.c_str())); f.step(2);
  }
  f.step();
  const auto overflow = first.replies(f.bot.publicKey(), f.radio);
  assert(overflow.size() == 1 && overflow[0].find("truncated") != std::string::npos);
  timeMs += 900001; f.radio.sent.clear();
  f.deliver(first.command(f.bot.publicKey(), "!advert_ack")); f.step(25);
  assert(first.replies(f.bot.publicKey(), f.radio) == std::vector<std::string>{"ack request"});
  f.deliver(first.ackLast(f.bot.publicKey(), f.radio)); f.step();
  assert((first.replies(f.bot.publicKey(), f.radio) == std::vector<std::string>{"ack request", "ACK owns DM"}));
  puts("PASS native filtered packets: overlapping callers, preserved unmatched copies, width/exact filters, "
       "dedup after consumption, split early TRACE, sequential multitrace/testall, old-tag rejection and late/cancel fences");
}

static void native_channel_key_configuration() {
  const auto records = identity_test::durable;
  {
    Bytes installed(40);
    memcpy(installed.data(), "BRP\1#example1", 13);
    installed[37] = 3; installed[38] = 1200 & 255; installed[39] = 1200 >> 8;
    identity_test::durable[{"mc-onchip", "bot-radio"}] = installed;
    BotRadioPolicy policy;
    assert(loadBotRadioPolicy(policy) && !strcmp(policy.channel, "#example1") &&
           !policy.channelKeySet && policy.pathWidth == 3 && policy.airtimeMs == 1200);
    Fixture f; f.start(); Peer peer;
    f.deliver(peer.targetedGroup(f.bot.publicKey(), "owner: !ping")); f.step();
    assert(peer.groupReplies(f.radio) == std::vector<std::string>{"Pong"});
    strcpy(policy.channel, "Private Ops");
    policy.channelKeySet = true;
    for (unsigned i = 0; i < 16; ++i) policy.channelKey[i] = uint8_t(0x41 + i);
    assert(saveBotRadioPolicy(policy));
    const auto saved = identity_test::durable.at({"mc-onchip", "bot-radio"});
    assert(saved.size() == 57 && !memcmp(saved.data(), "BRP\2", 4));
    timeMs += 61000; f.radio.sent.clear();
    f.deliver(peer.targetedGroup(f.bot.publicKey(), "owner: !ping", policy.channel, 3, {}, policy.channelKey));
    f.step(); assert(f.radio.sent.empty());
    f.bot.stop(); f.start();
    uint8_t bot[32]; memcpy(bot, f.bot.publicKey(), 32);
    timeMs += 61000; f.radio.sent.clear();
    f.deliver(peer.targetedGroup(bot, "owner: !ping"));
    f.deliver(peer.targetedGroup(bot, "owner: !ping", policy.channel));
    uint8_t wrongKey[16]; memcpy(wrongKey, policy.channelKey, 16); wrongKey[0] ^= 0x80;
    f.deliver(peer.targetedGroup(bot, "owner: !ping", policy.channel, 3, {}, wrongKey));
    f.step(); assert(f.radio.sent.empty());
    f.deliver(peer.targetedGroup(bot, "owner: !ping", policy.channel, 1, {}, policy.channelKey));
    f.step();
    assert(peer.groupReplies(f.radio, 3, policy.channel, policy.channelKey) == std::vector<std::string>{"Pong"});
    BotRadioPolicy loaded;
    assert(loadBotRadioPolicy(loaded) && loaded.channelKeySet &&
           !memcmp(loaded.channelKey, policy.channelKey, 16));
    f.bot.stop(); assert(saveBotRadioPolicy({})); f.start();
    assert(!memcmp(bot, f.bot.publicKey(), 32));
    f.radio.sent.clear();
    f.deliver(peer.targetedGroup(bot, "owner: !ping", policy.channel, 3, {}, policy.channelKey));
    f.step(); assert(f.radio.sent.empty());
    auto invalid = policy; memset(invalid.channelKey, 0, 16);
    assert(!saveBotRadioPolicy(invalid));
    auto corrupt = saved; corrupt[40] = 2;
    identity_test::durable[{"mc-onchip", "bot-radio"}] = corrupt;
    assert(!loadBotRadioPolicy(loaded));
  }
  identity_test::durable = records;
  puts("PASS native channel keys: installed BRP v1 read, checked v2 save, reboot-only application, old/wrong/derived-key rejection, AES128 reply, mixed RX widths and explicit off");
}

static void native_hashtag_commands() {
  BotRadioPolicy policy;
  assert(loadBotRadioPolicy(policy) && !policy.channel[0] && policy.pathWidth == 1);
  {
    Fixture off; off.start(); Peer peer;
    off.deliver(peer.group("owner: !ping")); off.step();
    assert(peer.groupReplies(off.radio).empty());
  }
  strcpy(policy.channel, "#example1"); policy.pathWidth = 3; policy.airtimeMs = 1200;
  assert(saveBotRadioPolicy(policy));
  BotRadioPolicy loaded;
  assert(loadBotRadioPolicy(loaded) && !strcmp(loaded.channel, "#example1") &&
         loaded.pathWidth == 3 && loaded.airtimeMs == 1200);
  strcpy(loaded.channel, "#Bad tag"); assert(!saveBotRadioPolicy(loaded));
  identity_test::failCommit = true;
  assert(!saveBotRadioPolicy({}));
  identity_test::failCommit = false;
  Fixture f; f.start(); Peer peer;
  const auto command = peer.targetedGroup(f.bot.publicKey(), "owner: !ping");
  f.deliver(command); f.step();
  assert(peer.groupReplies(f.radio) == std::vector<std::string>{"Pong"});
  const auto duplicateCount = f.bot.counters().duplicates;
  f.deliver(command); f.step();
  assert(f.bot.counters().duplicates == duplicateCount + 1);
  f.deliver(peer.targetedGroup(f.bot.publicKey(), "different-nickname: !ping")); f.step();
  const auto limited = peer.groupReplies(f.radio);
  assert(limited == std::vector<std::string>({"Pong", "Pong"}));
  const auto source = std::string(
      "function hello(name) reply('Hi '..name) end "
      "function who() if ctx.sender.authenticated or ctx.sender.public_key then return 'WRONG AUTH' end "
      "return ctx.channel.name..':'..ctx.sender.nickname end "
      "function private() return kv.get('private-key') end "
      "function send() local d=mesh.compose{text='private'} mesh.send(d) end "
      "function group_send() mesh.send(mesh.compose{kind='channel',to=ctx.channel.id,text='composed group'}) return 'group complete' end "
      "function group_forward() mesh.forward(mesh.compose{kind='channel',text='leak'}) end "
      "function delayed() sleep(1000) reply('original channel') end "
      "function huge() return '") + std::string(145, 'x') + "' end";
  BotWorker::Result result;
  assert(f.bot.stageSource(source.data(), source.size())); f.step();
  assert(f.bot.pollSourceResult(result) && result.ok);
  assert(f.bot.activateStaged()); f.step();
  assert(f.bot.pollSourceResult(result) && result.ok);
  auto invoke = [&](const char *text) {
    timeMs += 61000; f.radio.sent.clear();
    f.deliver(peer.targetedGroup(f.bot.publicKey(), text)); f.step();
    const auto replies = peer.groupReplies(f.radio);
    assert(replies.size() == 1);
    return replies[0];
  };
  assert(invoke("owner: !hello park") == "Hi park");
  assert(invoke("owner: !who") == "#example1:owner");
  assert(invoke("owner: !private").find("authenticated request authority") != std::string::npos);
  assert(invoke("owner: !send").find("authenticated invocation") != std::string::npos);
  timeMs += 61000; f.radio.sent.clear();
  f.deliver(peer.targetedGroup(f.bot.publicKey(), "owner: !group_send")); f.step();
  assert((peer.groupReplies(f.radio) == std::vector<std::string>{"composed group", "group complete"}));
  assert(invoke("owner: !group_forward").find("Error:") == 0);
  assert(invoke("owner: !remind 1s private") ==
         "Error: !remind permission not granted: send a DM; if unavailable, ask the owner to enable reminders");
  assert(invoke("owner: !reminders") ==
         "Error: !reminders permission not granted: send this command by DM");
  assert(invoke("owner: !huge").find("Error:") == 0);
  timeMs += 61000; f.radio.sent.clear();
  f.deliver(peer.targetedGroup(f.bot.publicKey(), "owner: !delayed")); f.step(20);
  auto changed = policy; strcpy(changed.channel, "#other");
  assert(saveBotRadioPolicy(changed));
  timeMs += 1000; f.step();
  assert(peer.groupReplies(f.radio) == std::vector<std::string>{"original channel"});
  assert(saveBotRadioPolicy(policy));
  timeMs += 61000; f.radio.sent.clear();
  f.deliver(peer.group("owner: !ping", "#other")); f.step();
  auto damaged = peer.group("owner: !ping"); damaged.back() ^= 1;
  f.deliver(damaged); f.step();
  assert(peer.groupReplies(f.radio).empty());
  const auto malformed = f.bot.counters().malformed;
  f.deliver(peer.group("!ping")); f.step();
  damaged = peer.group("owner: !ping"); damaged.pop_back();
  f.deliver(damaged); f.step();
  assert(f.bot.counters().malformed >= malformed + 2);
  assert(peer.groupReplies(f.radio).empty());
  timeMs += 61000; f.radio.sent.clear();
  f.deliver(peer.targetedGroup(f.bot.publicKey(), "visitor: !path", "#example1", 3, {0x12, 0x34, 0x56})); f.step();
  assert(peer.groupReplies(f.radio) == std::vector<std::string>{"3:123456"});
  for (uint8_t width : {1, 2}) {
    timeMs += 61000; f.radio.sent.clear();
    f.deliver(peer.targetedGroup(f.bot.publicKey(), "visitor: !path", "#example1", width, Bytes(width, 0x12))); f.step();
    const std::string expected = width == 1 ? "1:12" : "2:1212";
    assert(peer.groupReplies(f.radio, 3) == std::vector<std::string>{expected});
  }
  assert(invoke("visitor: !trace").find("TRACE requires") != std::string::npos);
  timeMs += 61000; f.radio.sent.clear(); f.radio.airtime = 1201;
  f.deliver(peer.targetedGroup(f.bot.publicKey(), "visitor: !ping")); f.step();
  assert(peer.groupReplies(f.radio).empty());
  f.radio.airtime = 10;
  f.learn(peer);
  assert(f.command(peer, "!ping", 3) == "Pong");
  timeMs += 61000; f.radio.sent.clear();
  f.deliver(peer.command(f.bot.publicKey(), "!ping", 1, {}, false)); f.step();
  assert(peer.replies(f.bot.publicKey(), f.radio) == std::vector<std::string>{"Pong"});
  bool originatedAck = false;
  for (const auto &wire : f.radio.sent) {
    mesh::Packet packet;
    assert(packet.readFrom(wire.data(), wire.size()));
    if (packet.getPayloadType() == PAYLOAD_TYPE_ACK || packet.getPayloadType() == PAYLOAD_TYPE_TXT_MSG) {
      assert(packet.isRouteFlood() && packet.getPathHashSize() == 3);
      originatedAck |= packet.getPayloadType() == PAYLOAD_TYPE_ACK;
    }
  }
  assert(originatedAck);
  assert(saveBotRadioPolicy({}));
  puts("PASS native hashtag commands: disabled default, persistent policy, 3-byte paths, "
       "standard group crypto, named reply, nickname without private KV/DM authority, "
       "dedup, zero channel cooldown, malformed/wrong-channel rejection and bounded airtime");
}

static void native_admission_feedback() {
  BotRadioPolicy policy;
  strcpy(policy.channel, "#example1"); policy.pathWidth = 3; policy.airtimeMs = 1200;
  assert(saveBotRadioPolicy(policy));
  {
    Fixture f; f.start(); Peer peer; f.learn(peer);
    timeMs += 61000; f.radio.sent.clear();
    f.deliver(peer.targetedGroup(f.bot.publicKey(), "owner: !ping")); f.step();
    f.deliver(peer.targetedGroup(f.bot.publicKey(), "owner: !calc 6*7")); f.step();
    auto replies = peer.groupReplies(f.radio);
    assert(replies == std::vector<std::string>({"Pong", "= 42"}));
    assert(f.bot.counters().replies == 2 && !f.bot.counters().notices);
    for (unsigned i = 0; i < 16; ++i) {
      f.deliver(peer.targetedGroup(f.bot.publicKey(), "rotating nickname: !ping")); f.step();
    }
    replies = peer.groupReplies(f.radio);
    assert(replies.size() == 18 && !f.bot.counters().notices);
    for (size_t i = 2; i < replies.size(); ++i) assert(replies[i] == "Pong");
    char status[163];
    f.bot.admissionStatus(status, sizeof(status));
    assert(strstr(status, "Last=none wait-ms=0 active=0") &&
           strstr(status, "sender=0 channel=0 global=0"));
    RadioDashboard::RoleStatus role;
    f.bot.dashboardStatus(role);
    assert(role.ready && !role.fault[0]);
    f.deliver(peer.command(f.bot.publicKey(), "!ping", 3)); f.step();
    assert(peer.replies(f.bot.publicKey(), f.radio) == std::vector<std::string>{"Pong"});
    f.deliver(peer.command(f.bot.publicKey(), "!calc 6*7", 3)); f.step();
    assert(peer.replies(f.bot.publicKey(), f.radio) == std::vector<std::string>({"Pong", "= 42"}));
    assert(!f.bot.counters().senderLimited && !f.bot.counters().notices);
    f.bot.admissionStatus(status, sizeof(status));
    assert(strstr(status, "wait-ms=0") && strstr(status, "active=0"));
  }
  {
    Fixture f; f.start(); Peer peers[4];
    for (auto &peer : peers) f.learn(peer);
    const char *source = "function hold() sleep(30000) return 'done' end";
    assert(f.bot.stageSource(source, strlen(source))); f.step();
    BotWorker::Result result;
    assert(f.bot.pollSourceResult(result) && result.ok);
    assert(f.bot.activateStaged()); f.step();
    assert(f.bot.pollSourceResult(result) && result.ok);
    timeMs += 61000; f.radio.sent.clear();
    for (unsigned i = 0; i < 3; ++i) {
      f.deliver(peers[i].command(f.bot.publicKey(), "!hold", 3)); f.step(20);
    }
    f.deliver(peers[3].command(f.bot.publicKey(), "!hold", 3)); f.step();
    auto replies = peers[3].replies(f.bot.publicKey(), f.radio);
    assert(replies.size() == 1 && replies[0].find("Not run: busy; retry in 5s.") == 0);
    assert(f.bot.counters().busy == 1 && f.bot.counters().notices == 1);
    char status[163];
    f.bot.admissionStatus(status, sizeof(status));
    assert(strstr(status, "Last=busy") && strstr(status, "active=3"));
    f.deliver(peers[3].command(f.bot.publicKey(), "!ping", 3)); f.step();
    replies = peers[3].replies(f.bot.publicKey(), f.radio);
    assert(replies.size() == 2 && replies.back() == "Pong");
    assert(!f.bot.counters().vmFailures);
  }
  {
    Fixture f; f.start(); Peer peer;
    timeMs += 61000; f.radio.sent.clear();
    f.deliver(peer.targetedGroup(f.bot.publicKey(), "owner: !ping")); f.step();
    f.radio.airtime = 1201;
    f.deliver(peer.targetedGroup(f.bot.publicKey(), "owner: !ping")); f.step();
    assert(peer.groupReplies(f.radio) == std::vector<std::string>{"Pong"});
    assert(!f.bot.counters().channelLimited && !f.bot.counters().notices);
  }
  assert(saveBotRadioPolicy({}));
  puts("PASS zero command cooldown: same-sender/channel bursts, no four/minute gate, "
       "busy resource feedback and unchanged airtime admission");
}

static void native_adaptive_admission() {
  BotRadioPolicy policy;
  policy.airtimeMs = 3600;
  strcpy(policy.channel, "#example1");
  policy.pathWidth = 3;
  bool enabled = true;
  assert(loadBotAdaptiveAdmission(enabled) && !enabled);
  assert(saveBotEnabled(true) && saveBotRadioPolicy(policy) && saveBotAdaptiveAdmission(true));
  {
    Fixture f; f.start(); Peer peer; f.learn(peer);
    char status[320];
    f.bot.adaptiveCommand("bot adaptive", status, sizeof(status));
    assert(strstr(status, "saved=1 live=1 metrics=1 congested=0"));
    f.radio.sent.clear();
    f.deliver(peer.command(f.bot.publicKey(), "!ping")); f.step();
    f.deliver(peer.command(f.bot.publicKey(), "!ping")); f.step();
    assert(peer.replies(f.bot.publicKey(), f.radio) == std::vector<std::string>({"Pong", "Pong"}));
    // Four other-role requests congest the one physical queue immediately;
    // local reflections from completed TX must not produce RX load.
    const uint8_t raw[] = {0, 0};
    uint32_t job;
    for (unsigned i = 0; i < 4; ++i)
      assert(f.existing[0].queueTransmit(raw, sizeof(raw), 0, 60000, 0, job));
    f.step(1);
    f.bot.adaptiveCommand("bot adaptive", status, sizeof(status));
    assert(strstr(status, "congested=1") && strstr(status, "allowance-permille=500"));
    f.bot.cancelJobs();
    timeMs += 61000; f.step(30);
    for (unsigned i = 0; i < 20; ++i) { timeMs += 1000; f.step(1); }
    f.bot.adaptiveCommand("bot adaptive", status, sizeof(status));
    assert(strstr(status, "congested=0") && strstr(status, "pending=0"));
    // Measure physical receptions before bounded RX buffering, even when
    // excess frames cannot reach the dispatcher; count once, not once/role.
    f.radio.airtime = 100;
    for (unsigned i = 0; i < 6; ++i) {
      for (unsigned frame = 0; frame < 10; ++frame) f.mux.received(raw, sizeof(raw), -91, 5);
      timeMs += 1000; f.step(1);
    }
    f.bot.adaptiveCommand("bot adaptive", status, sizeof(status));
    assert(strstr(status, "congested=1"));
    f.radio.sent.clear();
    f.setManualTxCompletionForTest(true);
    for (unsigned i = 0; i < 16; ++i) {
      f.deliver(peer.targetedGroup(f.bot.publicKey(), "caller: !ping")); f.step(20);
      bot_native_test::TxFrame frame;
      while (f.takeTxFrame(frame)) {
        assert(f.completeSimulatedTxForTest(frame.token, 100));
        f.setManualTxCompletionForTest(true);
        f.step(20);
      }
    }
    f.setManualTxCompletionForTest(false);
    f.bot.adaptiveCommand("bot adaptive", status, sizeof(status));
    assert(!strstr(status, "denied=0 ") && strstr(status, "pending=0"));
    auto replies = peer.groupReplies(f.radio);
    assert(std::count(replies.begin(), replies.end(), "Pong") < 16);
    assert(!f.bot.counters().senderLimited && !f.bot.counters().channelLimited);
    for (unsigned i = 0; i < 25; ++i) { timeMs += 1000; f.step(1); }
    f.bot.adaptiveCommand("bot adaptive", status, sizeof(status));
    assert(strstr(status, "congested=0"));
    f.radio.sent.clear();
    f.deliver(peer.command(f.bot.publicKey(), "!ping")); f.step();
    assert(peer.replies(f.bot.publicKey(), f.radio) == std::vector<std::string>{"Pong"});
    f.bot.adaptiveCommand("bot adaptive off", status, sizeof(status));
    assert(!strcmp(status, "Saved adaptive admission; reboot required"));
    f.bot.adaptiveCommand("bot adaptive", status, sizeof(status));
    assert(strstr(status, "saved=0 live=1"));
    f.bot.stop(); f.start();
    f.bot.adaptiveCommand("bot adaptive", status, sizeof(status));
    assert(strstr(status, "saved=0 live=0"));
  }
  assert(saveBotAdaptiveAdmission(true));
  {
    Fixture f; f.start(); Peer peer; f.learn(peer);
    f.radio.airtime = 1000;
    f.setManualTxCompletionForTest(true);
    f.deliver(peer.targetedGroup(f.bot.publicKey(), "caller: !ping")); f.step(20);
    bot_native_test::TxFrame frame;
    assert(f.takeTxFrame(frame));
    char status[320], admission[160];
    const auto pending = [&]() {
      f.bot.adaptiveCommand("bot adaptive", status, sizeof(status));
      assert(strstr(status, "live=1") && strstr(status, "pending=1 "));
    };
    const auto sourceResult = [&](bool ok) {
      BotWorker::Result result;
      bool done = false;
      for (unsigned i = 0; i < 100 && !done; ++i) {
        f.step(5);
        done = f.bot.pollSourceResult(result);
      }
      assert(done && result.ok == ok);
    };
    pending();
    f.bot.setCommandAdmission(false);
    f.bot.admissionStatus(admission, sizeof(admission));
#if ONCHIP_BOT_SINGLE_SESSION
    assert(strstr(admission, "pause=source-publication cooldown-ms=0 "));
#else
    assert(!strncmp(admission, "Last=", 5));
#endif
    f.bot.cancelJobs(); f.step(5);
    pending();
    BotNodeSnapshot snapshot;
    f.bot.nodeSnapshot(snapshot);
    const uint32_t activation = snapshot.sourceGeneration;
    const char *rejected = "error('candidate rejected')";
    assert(f.bot.stageSource(rejected, strlen(rejected)));
    sourceResult(false);
    f.bot.nodeSnapshot(snapshot);
    assert(snapshot.sourceGeneration == activation);
    pending();
    const char *candidate = "function fixture_held() return 'not activated' end";
    assert(f.bot.stageSource(candidate, strlen(candidate)));
    sourceResult(true);
    f.bot.admissionStatus(admission, sizeof(admission));
#if ONCHIP_BOT_SINGLE_SESSION
    assert(strstr(admission, "pause=source-unloaded cooldown-ms=0 "));
#endif
    pending();
    assert(f.bot.activateStaged());
    sourceResult(true);
    pending();
    assert(f.bot.stageSource(BotDefaultSource, strlen(BotDefaultSource)));
    sourceResult(true);
    pending();
    assert(f.bot.activateStaged());
    sourceResult(true);
    pending();
    f.bot.setCommandAdmission(true);
    f.bot.admissionStatus(admission, sizeof(admission));
#if ONCHIP_BOT_SINGLE_SESSION
    assert(strstr(admission, "pause=none cooldown-ms=0 "));
    assert(strlen(admission) < sizeof(admission) - 1 && strstr(admission, "busy="));
#endif
    assert(f.completeSimulatedTxForTest(frame.token, 100));
    f.step(5);
    f.bot.adaptiveCommand("bot adaptive", status, sizeof(status));
    assert(strstr(status, "pending=0 "));
    assert(!f.completeSimulatedTxForTest(frame.token, 100));
  }
  for (const auto &bad : std::vector<std::vector<uint8_t>>{
         {'B', 'A', 'D', 1, 0}, {'B', 'A', 'A', 1}, {'B', 'A', 'A', 1, 2}}) {
    identity_test::durable[{"mc-onchip", "bot-adaptive"}] = bad;
    Fixture f; f.start(); Peer peer; f.learn(peer);
    char text[320];
    f.bot.adaptiveCommand("bot adaptive", text, sizeof(text));
    assert(strstr(text, "live=0 policy-fault=1"));
    RadioDashboard::RoleStatus status;
    f.bot.dashboardStatus(status);
    assert(status.ready && strstr(status.fault, "Adaptive policy unreadable"));
    BotNodeSnapshot node;
    f.bot.nodeSnapshot(node);
    assert(node.ready && node.fault);
    identity_test::durable[{"mc-onchip", "bot-adaptive"}] = {'B', 'A', 'A', 1, 0};
    f.bot.adaptiveCommand("bot adaptive", text, sizeof(text));
    assert(strstr(text, "fault retained"));
    f.radio.sent.clear();
    f.deliver(peer.command(f.bot.publicKey(), "!ping")); f.step();
    assert(peer.replies(f.bot.publicKey(), f.radio) == std::vector<std::string>{"Pong"});
    identity_test::durable[{"mc-onchip", "bot-adaptive"}] = bad;
    identity_test::failCommit = true;
    f.bot.adaptiveCommand("bot adaptive off", text, sizeof(text));
    assert(strstr(text, "Error:"));
    identity_test::failCommit = false;
    f.bot.adaptiveCommand("bot adaptive", text, sizeof(text));
    assert(strstr(text, "policy-fault=1"));
    f.bot.adaptiveCommand("bot adaptive off", text, sizeof(text));
    assert(!strcmp(text, "Saved adaptive admission; reboot required"));
    f.bot.adaptiveCommand("bot adaptive", text, sizeof(text));
    assert(strstr(text, "saved=0 live=0") && !strstr(text, "policy-fault"));
    f.bot.dashboardStatus(status);
    f.bot.nodeSnapshot(node);
    assert(status.ready && !status.fault[0] && node.ready && !node.fault);
    f.bot.stop(); f.start();
    f.bot.adaptiveCommand("bot adaptive", text, sizeof(text));
    assert(strstr(text, "saved=0 live=0"));
  }
  assert(saveBotAdaptiveAdmission(false) && saveBotRadioPolicy({}));
  puts("PASS adaptive native bot: opt-in persistence, low load unchanged, competing role queue, RX congestion, zero cooldown, source pause/cancel/rejection/publication/rollback retain reservations until terminal TX, corrupt-policy static operation and checked owner recovery");
}

#ifdef BOT_HOST_RUNNER
static void native_source_api_capacities() {
  assert(saveBotEnabled(true));
  Fixture::Options options;
  options.nativeAdministrationForReplay = true;
  Fixture f(options);
  std::string error;
  if (!f.startReplay(error)) {
    fprintf(stderr, "Source API fixture: %s\n", error.c_str());
    assert(false);
  }
  const std::string api = f.administer("source api");
  const std::string expected =
      "API named-commands-v1 lua=5.5.1 commands=8 arguments=4 source-bytes=4096 runtime=shared-v1 jobs=" +
      std::to_string(BotJobLimit) + " kv=2 sleep=1 mesh=dm,wait,trace,advert rpc=1";
  assert(api == expected && api.size() < 146);
  const std::string atomic = f.administer("source api atomic");
  assert(atomic == "Atomic cas=1 transaction=" + std::to_string(BotTransactionLimit) +
      " keys=distinct scope=single outcome=committed,conflict,unknown,rejected recovery=redo");
  assert(atomic.size() < 146);
  const std::string overrides = f.administer("source api overrides");
  assert(overrides == "Overrides override_command(name,export) call_original(name[,text]) builtin-schema/policy=retained ctx=current-command slots=8");
  assert(overrides.size() < 146);
  for (const char *query : {"source api sources", "source api repeaters", "source api bundled",
                           "source api events"}) {
    const auto response = f.administer(query);
    assert(response.find("Error:") != 0 && response.size() < 146);
  }
  const auto base = f.administer("source hash");
  const char *candidate = "function fresh() return 'new' end";
  uint8_t digest[32]{};
  char hash[65]{}, upload[160]{};
  mesh::Utils::sha256(digest, sizeof(digest), reinterpret_cast<const uint8_t *>(candidate), strlen(candidate));
  for (unsigned i = 0; i < 32; ++i) snprintf(hash + i * 2, 3, "%02x", digest[i]);
  snprintf(upload, sizeof(upload), "source begin %.16s %u %s", hash, unsigned(strlen(candidate)), hash);
  assert(f.administer(upload).find("ACK ") == 0);
  std::string encoded;
  for (const char *p = candidate; *p; ++p) {
    char byte[3]; snprintf(byte, sizeof(byte), "%02x", unsigned(uint8_t(*p)));
    encoded += byte;
  }
  snprintf(upload, sizeof(upload), "source chunk %.16s 0 %s", hash, encoded.c_str());
  assert(f.administer(upload).find("ACK ") == 0);
  snprintf(upload, sizeof(upload), "source commit %.16s %064u", hash, 0u);
  assert(f.administer(upload).find("Error: active Lua sources changed") == 0 &&
         f.administer("source hash") == base);
  assert(f.administer("source cancel") == "Upload cancelled; active source retained");
  printf("PASS source API capacities: jobs=%u transaction=%u, native reply lengths=%zu/%zu\n",
         BotJobLimit, BotTransactionLimit, api.size(), atomic.size());
}
#endif

static void native_channel_storage() {
  BotRadioPolicy policy;
  strcpy(policy.channel, "#example1"); policy.pathWidth = 3;
  assert(saveBotRadioPolicy(policy) && saveBotSharedState(false));
  Fixture f; f.start(); Peer first, second;
  assert(!f.bot.sharedState());
  const char *source =
      "local function tier() if ctx.channel.present then "
      "if not ctx.channel.verified or #ctx.channel.id~=64 then return 'invalid' end "
      "return 'channel' end return 'caller' end "
      "function memorize(note) kv.put('memory',note,tier()) return 'committed' end "
      "command('memorize','note:text:100','Remember') "
      "function list_memories() return kv.get('memory',tier()) or 'empty' end "
      "command('read-memories','','Recall','list_memories')";
  const auto install = [&]() {
    BotWorker::Result result;
    assert(f.bot.stageSource(source, strlen(source))); f.step();
    assert(f.bot.pollSourceResult(result) && result.ok);
    assert(f.bot.activateStaged()); f.step();
    assert(f.bot.pollSourceResult(result) && result.ok);
  };
  install();
  const auto group = [&](Peer &peer, const char *text) {
    timeMs += 61000; f.radio.sent.clear();
    f.deliver(peer.targetedGroup(f.bot.publicKey(), text, policy.channel)); f.step();
    const auto replies = peer.groupReplies(f.radio, 3, policy.channel);
    assert(replies.size() == 1);
    return replies.front();
  };
  assert(group(first, "owner: !memorize denied").find("not granted") != std::string::npos);
  assert(f.bot.setSharedState(true));
  assert(f.bot.sharedState());
  assert(group(first, "owner: !memorize channel note") == "committed");
  assert(group(second, "forged-owner: !read-memories") == "channel note");
  f.learn(first);
  assert(f.command(first, "!read-memories") == "empty");
  assert(f.command(first, "!memorize private note") == "committed");
  assert(group(first, "owner: !read-memories") == "channel note");
  install();
  assert(group(second, "visitor: !read-memories") == "channel note");
  f.bot.stop(); assert(!f.bot.sharedState()); f.start(); install();
  assert(f.bot.sharedState());
  assert(group(second, "any nickname: !read-memories") == "channel note");
  f.bot.stop(); strcpy(policy.channel, "#other"); assert(saveBotRadioPolicy(policy));
  f.start(); install();
  assert(group(first, "owner: !read-memories") == "empty");
  assert(group(first, "owner: !memorize other channel") == "committed");
  f.bot.stop(); strcpy(policy.channel, "#example1"); assert(saveBotRadioPolicy(policy));
  f.start(); install();
  assert(group(first, "changed: !read-memories") == "channel note");
  identity_test::failCommit = true;
  assert(!f.bot.setSharedState(false));
  assert(!f.bot.sharedState());
  identity_test::failCommit = false;
  assert(group(second, "owner: !memorize revoked").find("not granted") != std::string::npos);
  assert(f.bot.setSharedState(false));
  f.learn(first);
  assert(f.command(first, "!read-memories") == "private note");
  assert(saveBotRadioPolicy({}));
  puts("PASS native channel KV: verified key-derived ID, explicit owner grant, nickname-independent sharing, "
       "DM separation, cross-function/source/reboot retention, channel rotation and failed durable revocation");
}

static void native_board() {
  BotRadioPolicy policy;
  strcpy(policy.channel, "#example1");
  assert(saveBotRadioPolicy(policy) && saveBotSharedState(false));
  Fixture f; f.start(); Peer first, second;
  const auto group = [&](Peer &peer, const char *text) {
    timeMs += 61000; f.radio.sent.clear();
    f.deliver(peer.targetedGroup(f.bot.publicKey(), text, policy.channel, policy.pathWidth, Bytes(policy.pathWidth, 0x12)));
    f.step();
    const auto replies = peer.groupReplies(f.radio, policy.pathWidth, policy.channel);
    assert(replies.size() == 1 && replies[0].size() <= BotReplyLimit);
    return replies.front();
  };
  assert(group(first, "owner: !board put plan denied").find("not granted") != std::string::npos);
  assert(f.bot.setSharedState(true));
  assert(group(first, "Alice: !board put plan picnic") == "Board plan created; committed");
  assert(group(second, "not-an-owner: !board get plan") == "picnic");
  assert(group(second, "forged-owner: !board put plan bring lunch") == "Board plan replaced; committed");
  assert(group(first, "Alice: !board list pl") == "Board (1): [plan]");
  f.learn(first);
  assert(f.command(first, "!board get plan").find("permission not granted") != std::string::npos);
  assert(f.command(first, "!remember board:plan private note") == "Note board:plan created; committed");
  assert(group(second, "visitor: !board get plan") == "bring lunch");
  timeMs += 61000; f.radio.sent.clear();
  const auto duplicate = first.targetedGroup(f.bot.publicKey(), "Alice: !board put plan only once", policy.channel, policy.pathWidth);
  f.deliver(duplicate); f.step();
  const auto duplicates = f.bot.counters().duplicates;
  f.deliver(duplicate); f.step();
  assert(f.bot.counters().duplicates == duplicates + 1 &&
         first.groupReplies(f.radio, policy.pathWidth).size() == 1);
  second.timestamp = first.timestamp + 1;
  f.deliver(second.targetedGroup(f.bot.publicKey(), "different-name: !board put plan rate bypass", policy.channel, policy.pathWidth)); f.step();
  const auto limited = first.groupReplies(f.radio, policy.pathWidth);
  assert(limited.size() == 2 && limited.back() == "Board plan replaced; committed");
  assert(group(first, "Alice: !board get plan") == "rate bypass");
  for (uint8_t width : {1, 2, 3}) {
    f.bot.stop(); policy.pathWidth = width; assert(saveBotRadioPolicy(policy)); f.start();
    assert(group(second, "changed-name: !board get plan") == "rate bypass");
    BotWorker::Result result;
    const char *source = "function hello() return 'custom replacement' end";
    assert(f.bot.stageSource(source, strlen(source))); f.step();
    assert(f.bot.pollSourceResult(result) && result.ok);
    assert(f.bot.activateStaged()); f.step();
    assert(f.bot.pollSourceResult(result) && result.ok);
    assert(group(first, "Alice: !board list") == "Board (1): [plan]");
  }
  timeMs += 61000; f.radio.sent.clear();
  f.deliver(first.group("Alice: !board delete plan", "#another-board")); f.step();
  assert(first.groupReplies(f.radio, policy.pathWidth).empty());
  auto corrupt = first.group("Alice: !board delete plan", policy.channel, policy.pathWidth);
  corrupt.back() ^= 1; f.deliver(corrupt); f.step();
  assert(first.groupReplies(f.radio, policy.pathWidth).empty());
  f.bot.stop(); strcpy(policy.channel, "#another-board"); assert(saveBotRadioPolicy(policy)); f.start();
  assert(group(first, "Alice: !board get plan") == "No board entry: plan");
  assert(group(first, "Alice: !board put plan different key") == "Board plan created; committed");
  f.bot.stop(); strcpy(policy.channel, "#example1"); assert(saveBotRadioPolicy(policy)); f.start();
  assert(group(second, "Alice: !board get plan") == "rate bypass");
  identity_test::failCommit = true; assert(!f.bot.setSharedState(false)); identity_test::failCommit = false;
  assert(group(second, "owner: !board delete plan").find("not granted") != std::string::npos);
  assert(f.bot.setSharedState(true));
  assert(group(second, "visitor: !board delete plan") == "Board plan deleted; committed");
  assert(group(first, "Alice: !board get plan") == "No board entry: plan");
  f.learn(first);
  assert(f.command(first, "!recall board:plan") == "private note");
  assert(saveBotRadioPolicy({}) && f.bot.setSharedState(false));
  puts("PASS native board: verified channel-key/nickname sharing, distinct secrets, private notes/DM denial, source/reboot, widths1/2/3, dedup/zero cooldown, malformed crypto and failed durable grant revoke");
}

static void native_repeater_monitor() {
  assert(saveBotEnabled(true) && saveBotEventAccess(0) && saveBotRadioPolicy({}) &&
         saveBotRepeaterPolicy({}));
  Fixture f; f.start();
  beginNetworkClock(true); receiveNetworkTime(1800000000); f.step();
  uint32_t earliest = 0, latest = 0;
  assert(trustedNetworkTime(earliest, latest));
  Peer peer, stranger;
  char key[65]{}, command[160]{}, reply[192]{};
  for (unsigned i = 0; i < 32; ++i) snprintf(key + 2 * i, 3, "%02x", peer.self_id.pub_key[i]);
  snprintf(command, sizeof(command), "add pilot %s 912525000 3:", key);
  f.bot.repeaterCommand(command, reply, sizeof(reply));
  assert(!strncmp(reply, "Saved/applied", 13));
  f.bot.repeaterCommand("config pilot", reply, sizeof(reply));
  assert(strstr(reply, key) && strstr(reply, "frequency=912525000"));
  f.bot.repeaterCommand("storage", reply, sizeof(reply));
  assert(strstr(reply, "nvs-free=") && strstr(reply, "authority=saved"));
  f.bot.repeaterCommand("help", reply, sizeof(reply));
  assert(strlen(reply) <= 162 && strstr(reply, "config ALIAS") && strstr(reply, "storage") &&
         strstr(reply, "discovery SEC") && strstr(reply, "timing ALIAS"));
  f.bot.repeaterCommand("discovery 1800", reply, sizeof(reply));
  assert(!strncmp(reply, "Saved/applied", 13) && strstr(reply, "discovery=1800s"));
  f.bot.repeaterCommand("discovery 59", reply, sizeof(reply));
  assert(!strncmp(reply, "Error:", 6));
  f.bot.repeaterCommand("discovery 900", reply, sizeof(reply));
  assert(!strncmp(reply, "Saved/applied", 13));
  f.bot.repeaterCommand("on", reply, sizeof(reply));
  assert(!strncmp(reply, "Saved/applied", 13));
  const char *source =
      "function fleet_poll() local p=repeater.next() if p then repeater.status(p) end end "
      "events.every(15,'fleet_poll')";
  BotWorker::Result staged;
  assert(f.bot.stageSource(source, strlen(source))); f.step();
  assert(f.bot.pollSourceResult(staged) && staged.ok);
  assert(f.bot.activateStaged()); f.step();
  assert(f.bot.pollSourceResult(staged) && staged.ok);
  assert(f.bot.setEventAccess(16)); f.step();
  f.radio.sent.clear(); timeMs += 16000; f.step();
  uint32_t tag = 0;
  unsigned requests = 0;
  for (const auto &raw : f.radio.sent) {
    mesh::Packet packet;
    assert(packet.readFrom(raw.data(), raw.size()));
    if (packet.getPayloadType() != PAYLOAD_TYPE_REQ) continue;
    assert(packet.isRouteDirect() && packet.getPathHashSize() == 3);
    uint8_t secret[32]{}, plain[MAX_PACKET_PAYLOAD]{};
    peer.self_id.calcSharedSecret(secret, f.bot.publicKey());
    const int size = mesh::Utils::MACThenDecrypt(secret, plain, packet.payload + 2, packet.payload_len - 2);
    assert(size == 16 && plain[4] == 1);
    tag = queued_tx::get32(plain); ++requests;
  }
  assert(requests == 1 && tag);
  const auto response = [&](Peer &sender, uint32_t echoed, bool path, bool malformed = false) {
    uint8_t secret[32]{}, plain[64]{};
    sender.self_id.calcSharedSecret(secret, f.bot.publicKey());
    queued_tx::put32(plain, echoed);
    plain[4] = 0xe3; plain[5] = 0x0e;
    queued_tx::put32(plain + 24, 12345);
    if (malformed) plain[60] = 1;
    mesh::Packet *packet = path ?
        sender.createPathReturn(mesh::Identity(f.bot.publicKey()), secret, plain, 0x80,
                                PAYLOAD_TYPE_RESPONSE, plain, malformed ? 61 : 60) :
        sender.createDatagram(PAYLOAD_TYPE_RESPONSE, mesh::Identity(f.bot.publicKey()), secret,
                              plain, malformed ? 61 : 60);
    assert(packet); packet->header |= ROUTE_TYPE_DIRECT; packet->path_len = 0x80;
    return sender.wire(packet);
  };
  BotRepeaterSnapshot snapshots[BotRepeaterLimit]{};
  assert(f.bot.repeaterSnapshots(snapshots, BotRepeaterLimit) == 1 && !snapshots[0].available);
  f.deliver(response(stranger, tag, false)); f.step();
  f.deliver(response(peer, tag - 1, false)); f.step();
  assert(f.bot.repeaterSnapshots(snapshots, BotRepeaterLimit) == 1 && !snapshots[0].available);
  f.deliver(response(peer, tag, true)); f.step();
  assert(f.bot.repeaterSnapshots(snapshots, BotRepeaterLimit) == 1 && snapshots[0].fresh &&
         snapshots[0].stats.batteryMv == 3811 && snapshots[0].stats.uptimeSeconds == 12345 &&
         snapshots[0].sampledUtc >= tag && snapshots[0].nextPollSeconds);
  f.bot.repeaterCommand("timing pilot", reply, sizeof(reply));
  assert(strlen(reply) <= 162 && strstr(reply, "wait=interval") && strstr(reply, "sampled-utc="));
  assert(f.bot.counters().eventsCompleted && !f.bot.jobsInUse());
  const auto before = f.radio.sent.size();
  timeMs += 16000; f.step();
  assert(f.radio.sent.size() == before);
  timeMs += 310000; receiveNetworkTime(1800000400); f.step();
  assert(f.bot.jobsInUse());
  timeMs += 31000; f.step();
  assert(!f.bot.jobsInUse());
  assert(f.bot.repeaterSnapshots(snapshots, BotRepeaterLimit) == 1 &&
         snapshots[0].available && !snapshots[0].fresh &&
         snapshots[0].error == BotRepeaterError::Timeout && snapshots[0].failures == 1);
  const auto attempts = snapshots[0].attempts;
  timeMs += 31000; f.step();
  assert(f.bot.repeaterSnapshots(snapshots, BotRepeaterLimit) == 1 && snapshots[0].attempts == attempts);
  timeMs += 610000; receiveNetworkTime(1800001000); f.step();
  assert(f.bot.jobsInUse());
  const auto activeAttempts = snapshots[0].attempts;
  const auto latestTag = [&]() {
    uint32_t latestTag = 0;
    for (const auto &raw : f.radio.sent) {
      mesh::Packet packet;
      assert(packet.readFrom(raw.data(), raw.size()));
      if (packet.getPayloadType() != PAYLOAD_TYPE_REQ) continue;
      uint8_t secret[32]{}, plain[MAX_PACKET_PAYLOAD]{};
      peer.self_id.calcSharedSecret(secret, f.bot.publicKey());
      assert(mesh::Utils::MACThenDecrypt(secret, plain, packet.payload + 2,
                                        packet.payload_len - 2) == 16);
      latestTag = queued_tx::get32(plain);
    }
    assert(latestTag);
    return latestTag;
  };
  const auto revokedTag = latestTag();
  assert(f.bot.setEventAccess(0)); f.step();
  f.deliver(response(peer, revokedTag, false)); f.step();
  assert(!f.bot.jobsInUse());
  assert(f.bot.repeaterSnapshots(snapshots, BotRepeaterLimit) == 1 &&
         !snapshots[0].fresh && snapshots[0].attempts > activeAttempts);
  assert(f.bot.setEventAccess(16)); f.step();
  timeMs += 1210000; receiveNetworkTime(1800002600); f.step();
  assert(f.bot.jobsInUse());
  f.deliver(response(peer, latestTag(), false, true)); f.step();
  assert(!f.bot.jobsInUse() && f.bot.repeaterSnapshots(snapshots, BotRepeaterLimit) == 1 &&
         snapshots[0].error == BotRepeaterError::Malformed && !snapshots[0].fresh);
  snprintf(command, sizeof(command), "add pilot %s 910525000 3:", key);
  f.bot.repeaterCommand(command, reply, sizeof(reply));
  assert(!strncmp(reply, "Saved/applied", 13));
  assert(f.bot.setEventAccess(16)); f.step();
  const auto beforeFrequency = f.radio.sent.size();
  timeMs += 310000; f.step();
  assert(f.radio.sent.size() == beforeFrequency && !f.bot.jobsInUse());
  assert(f.bot.repeaterSnapshots(snapshots, BotRepeaterLimit) == 1 &&
         snapshots[0].error == BotRepeaterError::Frequency && !snapshots[0].fresh);
  f.bot.repeaterCommand("off", reply, sizeof(reply));
  assert(!strncmp(reply, "Saved/applied", 13));
  assert(f.bot.setEventAccess(0) && saveBotRepeaterPolicy({}));
  puts("PASS native repeater monitor: autonomous scheduled RF, owner policy, full-key/tag correlation, PATH response, battery/status cache, no burst/retry and timeout backoff");

  f.bot.stop();
  uint32_t floodUtc = 0;
  for (unsigned boot = 0; boot < 2; ++boot) {
    assert(saveBotEventAccess(0));
    Fixture restart; restart.start();
    beginNetworkClock(true); receiveNetworkTime(1800003000 + boot * 100); restart.step();
    if (!boot) {
      snprintf(command, sizeof(command), "add pilot %s 912525000", key);
      restart.bot.repeaterCommand(command, reply, sizeof(reply));
      assert(!strncmp(reply, "Saved/applied", 13));
      restart.bot.repeaterCommand("on", reply, sizeof(reply));
      assert(!strncmp(reply, "Saved/applied", 13));
    }
    assert(restart.bot.stageSource(source, strlen(source))); restart.step();
    assert(restart.bot.pollSourceResult(staged) && staged.ok);
    assert(restart.bot.activateStaged()); restart.step();
    assert(restart.bot.pollSourceResult(staged) && staged.ok);
    assert(restart.bot.setEventAccess(16)); restart.step();
    restart.radio.sent.clear(); timeMs += 16000; restart.step();
    unsigned floods = 0;
    for (const auto &raw : restart.radio.sent) {
      mesh::Packet packet;
      assert(packet.readFrom(raw.data(), raw.size()));
      if (packet.getPayloadType() == PAYLOAD_TYPE_REQ) {
        assert(packet.isRouteFlood()); ++floods;
      }
    }
    assert(floods == (boot ? 0u : 1u));
    BotRepeaterPolicy persisted;
    assert(loadBotRepeaterPolicy(persisted) && persisted.targets[0].lastFloodUtc &&
           persisted.discoverySeconds == 900);
    if (!boot) floodUtc = persisted.targets[0].lastFloodUtc;
    else {
      assert(persisted.targets[0].lastFloodUtc == floodUtc && !restart.bot.jobsInUse());
      assert(restart.bot.repeaterSnapshots(snapshots, BotRepeaterLimit) == 1 &&
             snapshots[0].wait == BotRepeaterWait::Discovery && snapshots[0].nextDiscoverySeconds &&
             snapshots[0].nextPollSeconds >= snapshots[0].nextDiscoverySeconds);
      restart.bot.repeaterCommand("timing pilot", reply, sizeof(reply));
      assert(strstr(reply, "wait=discovery") && strstr(reply, "sampled-utc=0"));
      timeMs += (snapshots[0].nextDiscoverySeconds - 1) * 1000;
      receiveNetworkTime(floodUtc + 899); restart.step();
      assert(!restart.bot.jobsInUse());
      timeMs += 16000; receiveNetworkTime(floodUtc + 915); restart.step();
      assert(restart.bot.jobsInUse() && loadBotRepeaterPolicy(persisted) &&
             persisted.targets[0].lastFloodUtc >= floodUtc + 900);
      unsigned newFloods = 0;
      for (const auto &raw : restart.radio.sent) {
        mesh::Packet packet;
        assert(packet.readFrom(raw.data(), raw.size()));
        if (packet.getPayloadType() == PAYLOAD_TYPE_REQ) ++newFloods;
      }
      assert(newFloods == 1);
    }
    assert(restart.bot.setEventAccess(0)); restart.step();
  }
  assert(saveBotRepeaterPolicy({}));
  puts("PASS native repeater flood: persisted 900-second boundary across restart, visible discovery wait and exactly one new eligible request");

  BotRadioPolicy recoveryPolicy;
  recoveryPolicy.pathWidth = 3;
  assert(saveBotRadioPolicy(recoveryPolicy));
  Fixture recovery; recovery.start();
  uint32_t utc = 1800010000;
  beginNetworkClock(true); receiveNetworkTime(utc); recovery.step();
  snprintf(command, sizeof(command), "add pilot %s 912525000", key);
  recovery.bot.repeaterCommand(command, reply, sizeof(reply));
  assert(!strncmp(reply, "Saved/applied", 13));
  recovery.bot.repeaterCommand("on", reply, sizeof(reply));
  assert(!strncmp(reply, "Saved/applied", 13));
  assert(recovery.bot.stageSource(source, strlen(source))); recovery.step();
  assert(recovery.bot.pollSourceResult(staged) && staged.ok);
  assert(recovery.bot.activateStaged()); recovery.step();
  assert(recovery.bot.pollSourceResult(staged) && staged.ok);
  assert(recovery.bot.setEventAccess(16)); recovery.step();
  const auto advance = [&](uint32_t seconds) {
    timeMs += seconds * 1000; utc += seconds;
    receiveNetworkTime(utc); recovery.step();
  };
  const auto request = [&](bool flood, const Bytes &path, uint32_t seconds = 16) {
    recovery.radio.sent.clear();
    unsigned count = 0; uint32_t requestTag = 0;
    advance(seconds);
    for (const auto &raw : recovery.radio.sent) {
      mesh::Packet packet;
      assert(packet.readFrom(raw.data(), raw.size()));
      if (packet.getPayloadType() != PAYLOAD_TYPE_REQ) continue;
      assert(packet.isRouteFlood() == flood && packet.getPathHashSize() == 3);
      assert(packet.getPathByteLen() == path.size());
      assert(path.empty() || !memcmp(packet.path, path.data(), path.size()));
      uint8_t secret[32]{}, plain[MAX_PACKET_PAYLOAD]{};
      peer.self_id.calcSharedSecret(secret, recovery.bot.publicKey());
      assert(mesh::Utils::MACThenDecrypt(secret, plain, packet.payload + 2,
                                        packet.payload_len - 2) == 16 && plain[4] == 1);
      requestTag = queued_tx::get32(plain); ++count;
    }
    assert(count == 1 && requestTag && recovery.bot.jobsInUse());
    return requestTag;
  };
  const auto routedResponse = [&](Peer &sender, uint32_t echoed, const Bytes &path) {
    uint8_t secret[32]{}, plain[60]{};
    sender.self_id.calcSharedSecret(secret, recovery.bot.publicKey());
    queued_tx::put32(plain, echoed);
    plain[4] = 0xe3; plain[5] = 0x0e; queued_tx::put32(plain + 24, 54321);
    auto *packet = sender.createPathReturn(mesh::Identity(recovery.bot.publicKey()), secret,
        path.data(), uint8_t(0x80 | path.size() / 3), PAYLOAD_TYPE_RESPONSE, plain, sizeof(plain));
    assert(packet); packet->header |= ROUTE_TYPE_DIRECT; packet->path_len = 0x80;
    return sender.wire(packet);
  };
  const Bytes originalPath{0xcc, 0x26, 0x8a}, alternatePath{0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0xf6};
  const auto discoveryTag = request(true, {});
  recovery.bot.repeaterCommand("route pilot", reply, sizeof(reply));
  assert(!strcmp(reply, "pilot route=unknown repair=0"));
  recovery.deliver(routedResponse(peer, discoveryTag, originalPath)); recovery.step();
  assert(!recovery.bot.jobsInUse());
  recovery.bot.repeaterCommand("route pilot", reply, sizeof(reply));
  assert(!strcmp(reply, "pilot route=3:cc268a repair=0"));
  BotRepeaterPolicy discoveredPolicy;
  assert(loadBotRepeaterPolicy(discoveredPolicy));
  const auto discoveryUtc = discoveredPolicy.targets[0].lastFloodUtc;
  for (unsigned failure = 1; failure <= 3; ++failure) {
    request(false, originalPath, failure == 1 ? 317 :
        std::min<uint32_t>(900, 300u << (failure - 1)) + 17);
    advance(31);
    assert(!recovery.bot.jobsInUse());
    assert(recovery.bot.repeaterSnapshots(snapshots, BotRepeaterLimit) == 1 &&
           snapshots[0].available && !snapshots[0].fresh &&
           snapshots[0].error == BotRepeaterError::Timeout &&
           snapshots[0].failures == failure && snapshots[0].attempts == failure + 1);
  }
  recovery.bot.repeaterCommand("route pilot", reply, sizeof(reply));
  assert(!strcmp(reply, "pilot route=3:cc268a repair=1"));
  const auto repairTag = request(true, {}, 917);
  assert(loadBotRepeaterPolicy(discoveredPolicy) &&
         discoveredPolicy.targets[0].lastFloodUtc >= discoveryUtc + 900);
  recovery.deliver(routedResponse(stranger, repairTag, alternatePath)); recovery.step();
  recovery.deliver(routedResponse(peer, discoveryTag, alternatePath)); recovery.step();
  recovery.bot.repeaterCommand("route pilot", reply, sizeof(reply));
  assert(!strcmp(reply, "pilot route=3:cc268a repair=1") && recovery.bot.jobsInUse());
  recovery.deliver(routedResponse(peer, repairTag, alternatePath)); recovery.step();
  assert(!recovery.bot.jobsInUse());
  assert(recovery.bot.repeaterSnapshots(snapshots, BotRepeaterLimit) == 1 &&
         snapshots[0].fresh && snapshots[0].error == BotRepeaterError::None &&
         snapshots[0].stats.uptimeSeconds == 54321 && snapshots[0].attempts == 5);
  const auto beforeRead = recovery.radio.sent.size();
  recovery.bot.repeaterCommand("route pilot", reply, sizeof(reply));
  assert(!strcmp(reply, "pilot route=3:a1b2c3d4e5f6 repair=0") &&
         recovery.radio.sent.size() == beforeRead);
  const auto recoveredTag = request(false, alternatePath, 317);
  recovery.deliver(routedResponse(peer, recoveredTag, alternatePath)); recovery.step();
  assert(recovery.bot.repeaterSnapshots(snapshots, BotRepeaterLimit) == 1 &&
         snapshots[0].fresh && snapshots[0].attempts == 6);
  assert(recovery.bot.setEventAccess(0) && saveBotRepeaterPolicy({}) && saveBotRadioPolicy({}));
  puts("PASS native repeater recovery: learned three-byte hop, bounded direct-failure backoff, configurable flood repair, stale/foreign rejection and alternate multihop recovery");
}
static void event_collector_capacity() {
  for (unsigned mode = 0; mode < 3; ++mode) {
    assert(saveBotEnabled(true) && saveBotEventAccess(0) && saveBotRadioPolicy({}));
    Fixture f; f.start();
    Peer peers[4];
    for (auto &peer : peers) f.learn(peer);
    const char *source = "function startup() sleep(30000) end events.on('startup','startup')";
    assert(f.bot.stageSource(source, strlen(source))); f.step();
    BotWorker::Result staged;
    assert(f.bot.pollSourceResult(staged) && staged.ok);
    assert(f.bot.activateStaged()); f.step();
    assert(f.bot.pollSourceResult(staged) && staged.ok);
    unsigned long eventStarted = timeMs;
    if (mode != 1) {
      assert(f.bot.setEventAccess(1)); f.step();
      assert(f.bot.counters().eventsQueued == 1 && !f.bot.counters().eventsCompleted);
    }
    f.radio.sent.clear();
    bool accepted[4]{};
    unsigned admitted = 0;
    for (unsigned i = 0; i < 4; ++i) {
      const auto before = f.radio.sent.size();
      const auto busyBefore = f.bot.counters().busy;
      f.deliver(peers[i].command(f.bot.publicKey(), "!mt 1", 1, {1}));
      f.step();
      accepted[i] = f.bot.counters().busy == busyBefore;
      if (accepted[i]) ++admitted;
      assert(f.radio.sent.size() == before + (accepted[i] ? 1 : 2));
      unsigned receipts = 0;
      for (size_t j = before; j < f.radio.sent.size(); ++j) {
        mesh::Packet receipt;
        assert(receipt.readFrom(f.radio.sent[j].data(), f.radio.sent[j].size()));
        if (receipt.getPayloadType() == PAYLOAD_TYPE_PATH ||
            receipt.getPayloadType() == PAYLOAD_TYPE_ACK) ++receipts;
      }
      assert(receipts == 1);
      const auto replies = peers[i].replies(f.bot.publicKey(), f.radio);
      if (accepted[i]) assert(replies.empty());
      else assert(replies.size() == 1 && replies.front().find("Not run: busy;") == 0);
    }
    assert(admitted == (mode == 1 ? 4 : 3));
    if (mode == 1) {
      assert(f.bot.setEventAccess(1)); f.step(20);
      assert(!f.bot.counters().eventsQueued && f.bot.counters().eventsDropped);
    } else if (mode == 2) {
      // A stage mailbox blocks worker admission until its result is polled.
      // All already-ACKed collectors expire before that poll in the next loop.
      assert(f.bot.stageSource(source, strlen(source)));
    }
    timeMs += 1001;
    if (mode == 1) eventStarted = timeMs;
    f.step();
    if (mode == 2) assert(f.bot.pollSourceResult(staged) && staged.ok);
    for (unsigned i = 0; i < 4; ++i) {
      const auto replies = peers[i].replies(f.bot.publicKey(), f.radio);
      assert(replies.size() == 1);
      if (accepted[i]) {
        assert(replies.front().size() <= BotReplyLimit);
        assert(replies.front() == (mode == 2 ? "Error: Bot busy; collected command not executed" :
                                               "1 unique paths in 1000 ms; 1:01"));
      }
    }
    assert(f.bot.counters().eventsQueued == 1 && !f.bot.counters().eventsCompleted);
    assert(f.bot.counters().replies == admitted);
    if (mode != 1) {
      f.radio.sent.clear();
      f.deliver(peers[3].command(f.bot.publicKey(), "!ping")); f.step();
      assert((peers[3].replies(f.bot.publicKey(), f.radio) == std::vector<std::string>{"Pong"}));
      assert(!f.bot.counters().eventsCompleted);
    }
    timeMs = eventStarted + 30500; f.step();
    assert(f.bot.counters().eventsQueued == 1 && f.bot.counters().eventsCompleted == 1);
    assert(f.bot.setEventAccess(0));
    assert(f.command(peers[0], "!ping") == "Pong");
  }
  puts("PASS event/collector capacity: startup sleep plus four peers, aggregate ACK reservations in both orders, bounded delayed busy replies, live diagnostics and released slots");
}
static void channel_wait_collector_and_source_fences() {
  BotRadioPolicy originalRadio; BotMeshPolicy originalMesh;
  uint8_t originalEvents = 0;
  assert(loadBotRadioPolicy(originalRadio) && loadBotMeshPolicy(originalMesh) &&
         loadBotEventAccess(originalEvents));
  auto radio = originalRadio; strcpy(radio.channel, "#example1");
  radio.pathWidth = 3; radio.airtimeMs = 3600;
  assert(saveBotRadioPolicy(radio) && saveBotEventAccess(0));
  Fixture f; f.start(); Peer caller, ambient, diagnostic;
  f.learn(diagnostic);
  auto grant = originalMesh; grant.channelWait = true;
  assert(f.bot.setMeshPolicy(grant));
  const auto install = [&](const char *source) {
    BotWorker::Result result;
    assert(f.bot.stageSource(source, strlen(source))); f.step();
    assert(f.bot.pollSourceResult(result) && result.ok && f.bot.activateStaged()); f.step();
    assert(f.bot.pollSourceResult(result) && result.ok);
  };
  install("function listen() sleep(2000) "
          "return mesh.wait{kind='channel',exact='old-answer',timeout_ms=30000}.text end "
          "function _message() sleep(30000) end events.on('message','_message')");
  const unsigned long started = timeMs;
  f.radio.sent.clear();
  f.deliver(caller.targetedGroup(f.bot.publicKey(), "user: !listen")); f.step(40);
  timeMs = started + 2050; f.step(40);
  assert(f.bot.setEventAccess(4));
  f.deliver(ambient.group("ambient: first")); f.step(40);
  assert(f.bot.counters().eventsQueued == 1 && !f.bot.counters().eventsCompleted);
  timeMs = started + 4050;
  f.deliver(caller.targetedGroup(f.bot.publicKey(), "user: !mt 2", "#example1", 3, {1, 2, 3}));
  f.step(40);
  char admission[163];
  f.bot.admissionStatus(admission, sizeof(admission));
  assert(strstr(admission, "active=2") && caller.groupReplies(f.radio).empty());
  f.deliver(diagnostic.command(f.bot.publicKey(), "!ping")); f.step(40);
  assert(diagnostic.replies(f.bot.publicKey(), f.radio) == std::vector<std::string>{"Pong"});
  assert(f.bot.counters().eventsQueued == 1 && !f.bot.counters().eventsCompleted);
  for (unsigned i = 0; i < 5; ++i) {
    const auto noise = "ambient: noise-" + std::to_string(i);
    f.deliver(ambient.group(noise.c_str())); f.step(20);
  }
  const auto bounded = caller.groupReplies(f.radio);
  assert(bounded.size() == 1 && bounded[0].find("Native packet wait truncated") != std::string::npos);
  assert(f.bot.counters().eventsQueued == 1 && f.bot.counters().eventsDropped >= 5 &&
         !f.bot.counters().eventsCompleted);
  timeMs = started + 32600; f.step();
  const auto completed = caller.groupReplies(f.radio);
  assert(completed.size() == 2 && completed.back() == "1 unique paths in 2000 ms; 3:010203");
  assert(f.bot.counters().eventsCompleted == 1 && f.bot.counters().eventsFailed == 0);
  assert(f.bot.setEventAccess(0));

  timeMs += 61000; f.radio.sent.clear();
  f.deliver(caller.targetedGroup(f.bot.publicKey(), "user: !listen")); f.step(40);
  timeMs += 2000; f.step(40);
  BotNodeSnapshot before, after;
  f.bot.nodeSnapshot(before);
  install("function listen() return mesh.wait{kind='channel',exact='new-answer',timeout_ms=1000}.text end");
  f.bot.nodeSnapshot(after);
  assert(after.sourceGeneration != before.sourceGeneration);
  const auto cancelled = caller.groupReplies(f.radio);
  assert(cancelled.size() == 1 && cancelled[0].find("Source changed") != std::string::npos);
  f.deliver(ambient.group("ambient: old-answer")); f.step();
  assert(caller.groupReplies(f.radio) == cancelled);
  f.bot.meshPolicyStatus(admission, sizeof(admission));
  assert(strstr(admission, "channel-wait=1"));
  timeMs += 61000; f.radio.sent.clear();
  f.deliver(caller.targetedGroup(f.bot.publicKey(), "user: !listen")); f.step(20);
  f.deliver(ambient.group("ambient: new-answer")); f.step();
  assert(caller.groupReplies(f.radio) == std::vector<std::string>{"new-answer"});

  timeMs += 61000; f.radio.sent.clear();
  f.deliver(caller.targetedGroup(f.bot.publicKey(), "user: !listen")); f.step(20);
  auto revoked = grant; revoked.channelWait = false;
  identity_test::failCommit = true;
  assert(!f.bot.setMeshPolicy(revoked));
  identity_test::failCommit = false;
  f.step();
  const auto denied = caller.groupReplies(f.radio);
  assert(denied.size() == 1 && denied[0].find("Mesh grant revoked") != std::string::npos);
  f.bot.meshPolicyStatus(admission, sizeof(admission));
  assert(strstr(admission, "channel-wait=0"));
  assert(f.bot.setMeshPolicy(grant));
  f.deliver(ambient.group("ambient: new-answer")); f.step();
  assert(caller.groupReplies(f.radio) == denied);
  assert(f.bot.setEventAccess(originalEvents) && f.bot.setMeshPolicy(originalMesh) &&
         saveBotRadioPolicy(originalRadio));
  puts("PASS channel-wait contention/fences: concurrent selected-channel collector/event/ambient flood, bounded overflow, responsive Pong, source cancellation, fresh-generation grants and failed-persistence revoke");
}
static void native_events() {
  assert(saveBotEnabled(true) && saveBotEventAccess(0));
  BotRadioPolicy policy; strcpy(policy.channel, "#example1"); assert(saveBotRadioPolicy(policy));
  Fixture f; f.start(); Peer peer; f.learn(peer);
  assert(!f.bot.eventAccess() && !f.bot.eventMask());
  assert(f.bot.setEventAccess(15));
  assert(f.bot.eventAccess() == 15 && !f.bot.eventMask());
  assert(f.bot.setEventAccess(0));
  const char *source =
      "seen='none' started=0 connected=0 nodes=0 "
      "function _start() started=started+1 end events.on('startup','_start') "
      "function _wifi() connected=connected+1 end events.on('connectivity','_wifi') "
      "function _node() nodes=nodes+1 end events.on('node_status','_node') "
      "function _message() if ctx.channel.present then seen=ctx.sender.public_key and 'bad' or 'channel' "
      "else seen=ctx.sender.authenticated and 'dm' or 'bad' end end events.on('message','_message') "
      "function inspect() return seen..':'..tostring(started)..':'..tostring(connected)..':'..tostring(nodes) end "
      "function off() events.off('message') return 'off' end";
  assert(f.bot.stageSource(source, strlen(source))); f.step();
  BotWorker::Result stage; assert(f.bot.pollSourceResult(stage) && stage.ok);
  assert(!f.bot.counters().eventsQueued);
  assert(f.bot.activateStaged()); f.step(); assert(f.bot.pollSourceResult(stage) && stage.ok);
  assert(f.command(peer, "!inspect") == "none:0:0:0");
  assert(f.bot.setEventAccess(15));
  assert(f.bot.eventAccess() == 15);
  for (unsigned i = 0; i < 3; ++i) { timeMs += 1100; f.step(); }
  assert(f.bot.counters().eventsCompleted == 3);
  assert(f.command(peer, "!inspect") == "none:1:1:1");
  timeMs += 1100; const auto message = peer.command(f.bot.publicKey(), "ordinary message");
  f.deliver(message); f.step();
  const auto received = f.bot.counters().eventsCompleted;
  timeMs += 1100; f.deliver(message); f.step();
  assert(f.bot.counters().eventsCompleted == received);
  assert(f.command(peer, "!inspect") == "dm:1:1:1");
  timeMs += 1100; f.deliver(peer.group("Owner: ordinary channel")); f.step();
  assert(f.command(peer, "!inspect") == "channel:1:1:1");
  assert(f.command(peer, "!off") == "off"); f.step(); assert(!(f.bot.eventMask() & 4));
  assert(f.bot.eventAccess() == 15);
  identity_test::failCommit = true;
  assert(!f.bot.setEventAccess(0) && !f.bot.eventMask()); identity_test::failCommit = false;
  assert(!f.bot.eventAccess());
  assert(f.bot.setEventAccess(0));
  assert(f.command(peer, "!ping") == "Pong");
  f.bot.stop();
  assert(!f.bot.eventAccess() && !f.bot.eventMask());
  puts("PASS dispatcher subscriptions: default-off owner grants, no staging startup, activate startup, connectivity/node snapshots, authenticated DM vs channel nickname, RF dedup, unsubscribe, failed revoke and public Pong");
}
static void native_diagnostics() {
  assert(saveBotRadioPolicy({}));
  {
    assert(saveBotEnabled(false));
    Fixture off; assert(off.bot.begin(off.mux)); off.bot.loop();
    BotNodeSnapshot snapshot; off.bot.nodeSnapshot(snapshot);
    assert(snapshot.available && !snapshot.enabled && !snapshot.ready && !snapshot.hasIdentity);
  }
  assert(saveBotEnabled(true));
  BotRadioPolicy policy; strcpy(policy.channel, "#example1");
  assert(saveBotRadioPolicy(policy));
  Fixture f; f.start(); Peer first, second; f.learn(first); f.learn(second);
  f.bot.setNodeRoles(0, 0);
  const auto status = f.command(first, "!status");
  assert(status.find("bot=ready") != std::string::npos &&
         status.find("roles selected/ready=0/0") != std::string::npos &&
         status.find("battery=unavailable") != std::string::npos &&
         status.find("WiFi=unavailable") != std::string::npos);
  char key[65];
  mesh::Utils::toHex(key, f.bot.publicKey(), 32);
  std::string expectedKey(key);
  std::transform(expectedKey.begin(), expectedKey.end(), expectedKey.begin(), ::tolower);
  assert(f.command(first, "!about") == "mc-onchip/command-bot; key=" + expectedKey);
  const auto version = f.command(first, "!version");
  assert(version.find("MeshCore d92964352441; Lua 5.5.1; compiled ") == 0 &&
         version.find("image hash unavailable") != std::string::npos);
  assert(f.command(first, "!uptime").find("Uptime snapshot since boot:") == 0);
  RadioDashboard::RoleStatus botStatus; f.bot.dashboardStatus(botStatus);
  mesh::QueuedRadioStats before;
  assert(f.mux.getQueuedRadioStats(botStatus.source_slot, before));
  const auto air = f.command(first, "!air 3");
  const auto counts = "bot=" + std::to_string(before.source_successes) + "/" +
      std::to_string(before.source_failures) + " all=" + std::to_string(before.aggregate_successes) +
      "/" + std::to_string(before.aggregate_failures);
  assert(air.find(counts) != std::string::npos);
  for (uint8_t width : {1, 2, 3}) {
    const auto signal = f.command(first, "!signal", width, Bytes(width, 0x12));
    assert(signal.find("RSSI=-91.5dBm SNR=5.25dB") != std::string::npos &&
           signal.find("path=" + std::to_string(width) + ":") != std::string::npos);
  }
  timeMs += 61000; f.radio.sent.clear();
  f.deliver(first.command(f.bot.publicKey(), "!signal", 1, {0x12}), -101.0f, 1.25f);
  f.deliver(second.command(f.bot.publicKey(), "!signal", 3, {0xab,0xcd,0xef}), -81.0f, 3.75f);
  f.step();
  const auto firstReplies = first.replies(f.bot.publicKey(), f.radio);
  const auto secondReplies = second.replies(f.bot.publicKey(), f.radio);
  assert(firstReplies.size() == 1 && firstReplies[0].find("RSSI=-101dBm SNR=1.25dB; path=1:12") != std::string::npos);
  assert(secondReplies.size() == 1 && secondReplies[0].find("RSSI=-81dBm SNR=3.75dB; path=3:abcdef") != std::string::npos);
  timeMs += 61000; f.radio.sent.clear();
  f.reflect(first.command(f.bot.publicKey(), "!signal")); f.step();
  const auto local = first.replies(f.bot.publicKey(), f.radio);
  assert(local.size() == 1 && local[0].find("local reflection; RF unmeasured") != std::string::npos &&
         local[0].find("RSSI") == std::string::npos);
  for (const char *command : {"!about", "!version", "!uptime", "!status", "!signal", "!air",
                             "!air 2", "!air 3", "!air 4", "!help", "!help signal"}) {
    timeMs += 61000; f.radio.sent.clear();
    const auto text = "nickname: " + std::string(command);
    f.deliver(first.targetedGroup(f.bot.publicKey(), text.c_str(), "#example1", 1)); f.step();
    const auto replies = first.groupReplies(f.radio, 1);
    assert(replies.size() == 1 && replies[0].size() <= BotReplyLimit - 22 &&
           replies[0].find("Error:") != 0);
  }
  timeMs += 61000; f.radio.sent.clear();
  f.deliver(first.group("nickname: !status", "#other", 1));
  auto damaged = first.group("nickname: !status", "#example1", 1); damaged.back() ^= 1;
  f.deliver(damaged); f.step();
  assert(first.groupReplies(f.radio, 1).empty());
  const auto request = first.targetedGroup(f.bot.publicKey(), "nickname: !status", "#example1", 1);
  f.deliver(request); f.step();
  const auto duplicates = f.bot.counters().duplicates;
  f.deliver(request); f.step();
  f.deliver(second.targetedGroup(f.bot.publicKey(), "different-name: !signal", "#example1", 1)); f.step();
  assert(f.bot.counters().duplicates == duplicates + 1 &&
         first.groupReplies(f.radio, 1).size() == 2 &&
         first.groupReplies(f.radio, 1).back().find("RSSI=") != std::string::npos);
  timeMs += 61000; f.radio.sent.clear(); f.radio.airtime = policy.airtimeMs + 1;
  f.deliver(first.targetedGroup(f.bot.publicKey(), "nickname: !air", "#example1", 1)); f.step();
  assert(first.groupReplies(f.radio, 1).empty());
  f.radio.airtime = 10;
  timeMs = UINT32_MAX - 9; f.bot.loop();
  BotNodeSnapshot preWrap, postWrap;
  f.bot.nodeSnapshot(preWrap);
  timeMs = 25; f.bot.loop(); f.bot.nodeSnapshot(postWrap);
  assert(postWrap.uptimeMs - preWrap.uptimeMs == 35 && postWrap.uptimeMs > UINT32_MAX);
  assert(saveBotRadioPolicy({}));
  puts("PASS native diagnostics: actual identity/build/scheduler counters, disabled role, missing sensor/network, packet-local RSSI/SNR, overlapping callers, reflection, channel replies and uptime rollover");
}

static void native_durable_timer_reply() {
  assert(saveBotRadioPolicy({}));
  Fixture f; f.start();
  Peer caller, other; f.learn(caller); f.learn(other);
  beginClocks(); beginNetworkClock(true);
  receiveNetworkTime(1767225600); loopClocks();
  const char *source =
      "function delayed_notice() timer.set('once',2) timer.wait('once') return 'Reminder once' end";
  BotWorker::Result result;
  assert(f.bot.stageSource(source, strlen(source))); f.step();
  assert(f.bot.pollSourceResult(result) && result.ok);
  assert(f.bot.activateStaged()); f.step();
  assert(f.bot.pollSourceResult(result) && result.ok);
  timeMs += 61000; f.radio.sent.clear();
  f.deliver(caller.command(f.bot.publicKey(), "!delayed_notice")); f.step();
  assert(caller.replies(f.bot.publicKey(), f.radio).empty());
  f.deliver(other.command(f.bot.publicKey(), "!ping")); f.step();
  assert(other.replies(f.bot.publicKey(), f.radio) == std::vector<std::string>{"Pong"});
  assert(caller.replies(f.bot.publicKey(), f.radio).empty());
  timeMs += 3000; f.step();
  assert(caller.replies(f.bot.publicKey(), f.radio) == std::vector<std::string>{"Reminder once"});
  f.step(300);
  assert(caller.replies(f.bot.publicKey(), f.radio).size() == 1);
  puts("PASS simulated native RF: durable timer yields, concurrent caller Pong, original principal/route and one budgeted reminder reply");
}
static void autonomous_personal_reminders() {
  const auto clear = [] {
    for (auto it = identity_test::durable.begin(); it != identity_test::durable.end();) {
      if (it->first.first == "mc-bot-remind") it = identity_test::durable.erase(it);
      else ++it;
    }
    for (const char *path : {"/command-bot/reminders-a.bin", "/command-bot/reminders-b.bin"})
      if (SPIFFS.exists(path)) assert(SPIFFS.remove(path));
    assert(saveBotReminderAccess(false));
    beginClocks(); beginNetworkClock(true);
  };
  const auto trust = [](uint32_t epoch) { receiveNetworkTime(epoch); loopClocks(); };
  const auto scheduled = [](const std::string &text, uint32_t &due) {
    unsigned id, deadline;
    if (sscanf(text.c_str(), "#%u pending; due UTC %u", &id, &deadline) != 2) fprintf(stderr, "%s\n", text.c_str());
    assert(sscanf(text.c_str(), "#%u pending; due UTC %u", &id, &deadline) == 2);
    due = deadline; return uint32_t(id);
  };
  const auto reminders = [](const std::vector<std::string> &messages) {
    return std::count_if(messages.begin(), messages.end(), [](const std::string &text) {
      return text.find("Reminder #") == 0;
    });
  };
  clear();
  {
    Fixture f; f.start(); Peer caller, other; f.learn(caller); f.learn(other);
    trust(1767225600);
    assert(f.command(caller, "!remind 2m tea").find("grant") != std::string::npos);
    assert(f.bot.setReminderAccess(true));
    uint32_t due;
    const auto id = scheduled(f.command(caller, "!remind 2m tea after reboot"), due);
    uint8_t identity[32]; memcpy(identity, f.bot.publicKey(), 32);
    f.bot.stop(); timeMs = 1000; beginClocks(); beginNetworkClock(true); f.start();
    assert(!memcmp(identity, f.bot.publicKey(), 32) && f.bot.reminderAccess());
    f.step(600); assert(caller.replies(f.bot.publicKey(), f.radio).empty());
    f.learn(caller); trust(due);
    f.step(600); assert(caller.replies(f.bot.publicKey(), f.radio).empty());
    f.bot.setCommandAdmission(false);
    f.deliver(caller.pathReturn(f.bot.publicKey(), 3, Bytes{0x12,0x34,0x56})); f.step(600);
    assert(caller.replies(f.bot.publicKey(), f.radio).empty());
    f.bot.setCommandAdmission(true); f.step(600);
    auto messages = caller.replies(f.bot.publicKey(), f.radio);
    assert(messages == std::vector<std::string>{"Reminder #" + std::to_string(id) + ": tea after reboot"});
    for (const auto &raw : f.radio.sent) {
      mesh::Packet packet; assert(packet.readFrom(raw.data(), raw.size()));
      if (packet.getPayloadType() == PAYLOAD_TYPE_TXT_MSG)
        assert(packet.isRouteDirect() && packet.getPathHashSize() == 3);
    }
    assert(f.command(caller, "!reminders").find(std::to_string(id) + " sent") != std::string::npos);
    f.bot.stop(); timeMs = 1000; beginClocks(); beginNetworkClock(true); f.start();
    f.learn(caller); f.deliver(caller.pathReturn(f.bot.publicKey(), 1, {}));
    trust(due + 100); f.step(600);
    assert(caller.replies(f.bot.publicKey(), f.radio).empty());
    f.learn(other);
    const auto cancelId = scheduled(f.command(caller, "!remind 10m private note"), due);
    assert(f.command(other, "!reminders") == "No personal reminders");
    const auto cancel = "!cancel " + std::to_string(cancelId);
    assert(f.command(other, cancel.c_str()).find("not found") != std::string::npos);
    assert(f.command(caller, cancel.c_str()).find("cancelled") != std::string::npos);
    trust(due); f.radio.sent.clear(); f.step(600);
    assert(caller.replies(f.bot.publicKey(), f.radio).empty());
  }
  clear();
  {
    Fixture f; f.start(); Peer first, second;
    f.learn(first); f.learn(second); trust(1767225600);
    assert(f.bot.setReminderAccess(true));
    f.deliver(second.pathReturn(f.bot.publicKey(), 2, {0x23,0x45})); f.step(10);
    timeMs += 61000; f.radio.sent.clear();
    f.deliver(first.command(f.bot.publicKey(), "!remind 2s no route")); f.step(40);
    f.deliver(second.command(f.bot.publicKey(), "!remind 2s ready")); f.step(40);
    uint32_t firstDue, secondDue;
    scheduled(first.replies(f.bot.publicKey(), f.radio).front(), firstDue);
    scheduled(second.replies(f.bot.publicKey(), f.radio).front(), secondDue);
    trust(std::max(firstDue, secondDue)); f.step(800);
    assert(reminders(first.replies(f.bot.publicKey(), f.radio)) == 0);
    assert(reminders(second.replies(f.bot.publicKey(), f.radio)) == 1);
    trust(firstDue + BotTimerOverdueSeconds + 1); timeMs += 1000; f.step(600);
    assert(f.command(first, "!reminders").find("overdue") != std::string::npos);
    assert(reminders(first.replies(f.bot.publicKey(), f.radio)) == 0);
  }
  clear();
  {
    Fixture f; f.start(); Peer peer; f.learn(peer); trust(1767225600);
    assert(f.bot.setReminderAccess(true));
    uint32_t due;
    scheduled(f.command(peer, "!remind 2s uncertain claim"), due);
    f.deliver(peer.pathReturn(f.bot.publicKey(), 1, {})); f.step(10);
    f.radio.sent.clear();
    identity_test::afterCommit = [] { identity_test::failRead = true; };
    trust(due); timeMs += 1000; f.step(100);
    identity_test::afterCommit = nullptr; identity_test::failRead = false;
    assert(peer.replies(f.bot.publicKey(), f.radio).empty());
    assert(f.command(peer, "!reminders").find("unknown") != std::string::npos);
    f.bot.stop(); f.start(); f.learn(peer);
    f.deliver(peer.pathReturn(f.bot.publicKey(), 1, {})); trust(due + 70); f.step(600);
    assert(peer.replies(f.bot.publicKey(), f.radio).empty());
  }
  clear();
  {
    Fixture f; f.start(); Peer peer; f.learn(peer); trust(1767225600);
    assert(f.bot.setReminderAccess(true));
    uint32_t due;
    const auto id = scheduled(f.command(peer, "!remind 2s uncertain TX"), due);
    f.deliver(peer.pathReturn(f.bot.publicKey(), 1, {})); f.step(10);
    f.radio.sent.clear(); f.radio.airtime = 100; f.radio.holdTx = true;
    trust(due); timeMs += 1000; f.step(15);
    assert(reminders(peer.replies(f.bot.publicKey(), f.radio)) == 1);
    identity_test::failCommit = true;
    assert(!f.bot.setReminderAccess(false) && !f.bot.reminderAccess());
    identity_test::failCommit = false;
    f.step(10); f.radio.holdTx = false; f.step(100); f.radio.airtime = 10;
    assert(f.command(peer, "!reminders").find("unknown") != std::string::npos);
    assert(f.bot.setReminderAccess(true)); f.radio.sent.clear(); f.step(600);
    assert(peer.replies(f.bot.publicKey(), f.radio).empty());
    const auto cancel = "!cancel " + std::to_string(id);
    assert(f.command(peer, cancel.c_str()).find("unknown") != std::string::npos);
    f.radio.sent.clear();
    const auto id2 = scheduled(f.command(peer, "!remind 2s queue fails"), due);
    f.radio.sent.clear(); f.radio.rejectTx = true; trust(due); timeMs += 1000; f.step(100);
    f.radio.rejectTx = false;
    assert(peer.replies(f.bot.publicKey(), f.radio).empty());
    assert(f.command(peer, "!reminders").find(std::to_string(id2) + " unknown") != std::string::npos);
  }
  assert(saveBotReminderAccess(false));
  puts("PASS autonomous private DM reminders: stock command API, reboot without invocation, clock/source/route gates, "
       "direct 1/2/3-byte routes, caller isolation/cancel, concurrent principals, no-route overdue, "
       "uncertain claim, failed revoke, late/failed TX and no replay");
}

static void native_utilities() {
  assert(saveBotRadioPolicy({}));
  Fixture f; f.start();
  Peer first, second; f.learn(first); f.learn(second);
  assert(f.command(first, "!calc (2+3)*4") == "= 20");
  assert(f.command(first, "!convert 32 F C") == "32 F = 0 C");
  assert(f.command(first, "!roll").find("Roll 1d6: [") == 0);
  assert(f.command(first, "!choose one|two").find("Choice ") == 0);
  assert(f.command(first, "!calc 1/0") == "Error: !calc: Division by zero");
  timeMs += 61000; f.radio.sent.clear();
  f.deliver(first.command(f.bot.publicKey(), "!calc 3*7"));
  f.deliver(second.command(f.bot.publicKey(), "!convert 1 MiB B"));
  f.step(160);
  const auto a = first.replies(f.bot.publicKey(), f.radio), b = second.replies(f.bot.publicKey(), f.radio);
  assert(a.size() == 1 && a[0] == "= 21");
  assert(b.size() == 1 && b[0] == "1 MiB = 1048576 B");
  timeMs += 61000; f.radio.sent.clear();
  f.deliver(first.targetedGroup(f.bot.publicKey(), "owner: !calc 6*7", "#example1")); f.step();
  assert(first.groupReplies(f.radio, 1, "#example1").empty());
  f.bot.stop();
  BotRadioPolicy policy; strcpy(policy.channel, "#example1"); policy.pathWidth = 3;
  assert(saveBotRadioPolicy(policy));
  f.start();
  const auto group = [&](const char *text) {
    timeMs += 61000; f.radio.sent.clear();
    f.deliver(first.targetedGroup(f.bot.publicKey(), text, "#example1")); f.step();
    const auto replies = first.groupReplies(f.radio, 3, "#example1");
    assert(replies.size() == 1); return replies.front();
  };
  assert(group("owner: !calc 6*7") == "= 42");
  assert(group("other: !convert 1 mi km") == "1 mi = 1.609344 km");
  assert(group("any: !roll 2d6").find("Roll 2d6: [") == 0);
  assert(group("visitor: !choose tea|coffee").find("Choice ") == 0);
  assert(group("owner: !remember secret nope").find("permission not granted: send this command by DM") != std::string::npos);
  assert(group("owner: !remind 1m nope") ==
         "Error: !remind permission not granted: send a DM; if unavailable, ask the owner to enable reminders");
  timeMs += 61000; f.radio.sent.clear();
  const auto choice = first.targetedGroup(f.bot.publicKey(), "owner: !choose one|two", "#example1");
  f.deliver(choice); f.step(); f.deliver(choice); f.step();
  assert(first.groupReplies(f.radio, 3, "#example1").size() == 1);
  f.deliver(first.targetedGroup(f.bot.publicKey(), "nickname-changed: !calc 9+9", "#example1")); f.step();
  const auto limited = first.groupReplies(f.radio, 3, "#example1");
  assert(limited.size() == 2 && limited.back() == "= 18");
  const char *override = "function regional_calc() return '= 42' end override_command('calc','regional_calc')";
  BotWorker::Result installed;
  assert(f.bot.stageSource(override, strlen(override))); f.step();
  assert(f.bot.pollSourceResult(installed) && installed.ok && f.bot.activateStaged()); f.step();
  assert(f.bot.pollSourceResult(installed) && installed.ok);
  for (bool targeted : {false, true}) {
    timeMs += 61000; f.radio.sent.clear();
    const auto request = targeted ?
        first.targetedGroup(f.bot.publicKey(), "caller: !calc 6*7", "#example1") :
        first.group("caller: !calc 6*7", "#example1");
    f.bot.setSourceDeploymentState(true, true, true, false, "Lua source startup pending");
    const auto rejected = f.bot.counters().rejected;
    f.deliver(request); f.step();
    assert(f.bot.counters().rejected == rejected + 1 &&
           first.groupReplies(f.radio, 3, "#example1").empty());
    f.bot.setSourceDeploymentState(true, false, true, false, nullptr);
    const auto reflect = [&]() {
      f.reflect(request);
      f.radio.sent.erase(std::remove(f.radio.sent.begin(), f.radio.sent.end(), request),
                         f.radio.sent.end());
      f.step(750);
    };
    reflect();
    assert(first.groupReplies(f.radio, 3, "#example1").empty());
    f.deliver(request); f.step(750);
    const auto received = first.groupReplies(f.radio, 3, "#example1");
    assert(received.size() == 1 &&
           (targeted ? received[0] == "= 42" :
                       received[0].size() == 17 && received[0].substr(13) == "= 42"));
    reflect(); f.deliver(request); f.step();
    assert(first.groupReplies(f.radio, 3, "#example1").size() == 1);
  }
  assert(saveBotRadioPolicy({}));
  puts("PASS encrypted native utilities: two callers, optional verified #example1, no nickname/private authority, zero channel cooldown, packet dedup and channel reflections do not consume RF requests");
}

#ifdef BOT_HOST_RUNNER
static std::atomic<bool> sourceCopyPaused{false}, sourceCopyPauseEntered{false};
static bool sourceLivePauseOnCommit = false, sourceLiveReadFaultOnCommit = false;
static bool sourceResultHeld = false;
static void clearSourceFaults() {
  sourceCopyPaused = false;
  sourceLivePauseOnCommit = sourceLiveReadFaultOnCommit = false;
  filesystem_test::readLimit = std::numeric_limits<size_t>::max();
}
static bool prepareHostRunner() {
  filesystem_test::step = [](const char *checkpoint) {
    if (!strcmp(checkpoint, "file.before-write") && sourceCopyPaused.load()) {
      sourceCopyPauseEntered = true;
      while (sourceCopyPaused.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
      sourceCopyPauseEntered = false;
    }
  };
  identity_test::afterCommit = [] {
    if (sourceLivePauseOnCommit) {
      sourceLivePauseOnCommit = false; sourceCopyPaused = true;
    }
    if (sourceLiveReadFaultOnCommit) {
      sourceLiveReadFaultOnCommit = false; filesystem_test::readLimit = 0;
    }
  };
  for (const char *name : {"modem", "repeater", "room", "companion", "observer", "management"}) {
    mesh::LocalIdentity identity;
    if (!loadIdentity(name, identity)) return false;
  }
  if (!commitProfileJournal({RoleProfile{5}, 7, 9}) || !saveBotEnabled(true) ||
      !saveBotRadioPolicy({})) return false;
  beginClocks();
  beginNetworkClock(true);
  receiveNetworkTime(ONCHIP_CLOCK_BUILD_EPOCH);
  loopClocks();
#ifdef BOT_HOST_RUNNER
  // Network configuration is process-owned, not fixture-owned, and survives
  // fixture restarts. Initialize it before the fixture's allocation baseline.
  if (!botNetworkReload()) return false;
#endif
  return true;
}
static bool sourceResult(Fixture &fixture, BotWorker::Result &result) {
  for (unsigned i = 0; i < 30; ++i) {
    fixture.step(80);
    if (fixture.bot.pollSourceResult(result)) return true;
  }
  return false;
}
static std::string hostSourceSha256(const std::string &source);
static bool stageHostSource(Fixture &fixture, const std::string &source,
                            bool activate, std::string &error) {
  if (source.empty() || source.size() > BotSourceLimit) {
    error = "source must be 1..4096 bytes"; return false;
  }
  if (activate) {
    const auto hash = hostSourceSha256(source), id = hash.substr(0, 16);
    const std::string selector = botSourceIsWasm(source.data(), source.size()) ?
                                 "source wasm " : "source ";
    const auto submit = [&](const std::string &command, const char *expected) {
      const auto reply = fixture.administer((selector + command).c_str());
      if (reply.rfind(expected, 0) == 0) return true;
      error = reply; return false;
    };
    if (!submit("begin " + id + " " + std::to_string(source.size()) + " " + hash, "ACK ")) return false;
    static constexpr char digits[] = "0123456789abcdef";
    for (size_t offset = 0; offset < source.size(); offset += 48) {
      std::string encoded;
      for (size_t i = offset; i < std::min(offset + 48, source.size()); ++i) {
        const auto byte = static_cast<unsigned char>(source[i]);
        encoded += digits[byte >> 4]; encoded += digits[byte & 15];
      }
      if (!submit("chunk " + id + " " + std::to_string(offset / 48) + " " + encoded, "ACK ")) return false;
    }
    if (!submit("commit " + id, "Accepted verification")) return false;
    for (unsigned i = 0; i < 40; ++i) {
      fixture.step(80);
      const auto status = fixture.administer((selector + "status").c_str());
      if (status.find("Error:") != std::string::npos) { error = status; return false; }
      if (status.find("source durably saved and active") != std::string::npos &&
          fixture.bot.sourceReady() &&
          fixture.administer((selector + "hash").c_str()).rfind("SHA256 " + hash + " ", 0) == 0) return true;
    }
    error = "native source activation did not complete: " +
            fixture.administer((selector + "status").c_str());
    return false;
  }
  auto file = SPIFFS.open(BotStagedSourcePath, "w");
  if (!file) {
    error = "native source staging file unavailable"; return false;
  }
  const auto written = file.write(reinterpret_cast<const uint8_t *>(source.data()), source.size());
  file.flush(); file.close();
  if (written != source.size()) {
    error = "native source staging file write failed"; return false;
  }
  uint8_t digest[32];
  mesh::Utils::sha256(digest, sizeof(digest),
                      reinterpret_cast<const uint8_t *>(source.data()), source.size());
  if (!fixture.bot.stageSourceFile(source.size(), digest)) {
    error = "native source staging was not accepted"; return false;
  }
  BotWorker::Result result;
  if (!sourceResult(fixture, result)) {
    error = "native source staging did not complete"; return false;
  }
  if (!result.ok) {
    error = result.error; return false;
  }
  return true;
}
#if ONCHIP_BOT_WASM
static void native_runtime_admission_isolation(const char *modules) {
  assert(prepareHostRunner());
  const auto module = [&](const char *name) {
    std::ifstream file(std::string(modules) + "/" + name + ".wasm", std::ios::binary);
    assert(file);
    return std::string(std::istreambuf_iterator<char>(file), {});
  };
  const std::string lua = "function lkeep() return 'Lua retained' end";
  const auto wasm = module("c-notes");
  Peer owner, stranger;
  for (bool failingWasm : {false, true}) {
    Fixture::Options options; options.nativeAdministrationForReplay = true;
    std::string error;
    {
      Fixture setup(options); assert(setup.startReplay(error));
      assert(stageHostSource(setup, lua, true, error));
      assert(stageHostSource(setup, wasm, true, error));
      std::string key;
      char pair[3];
      for (auto byte : owner.self_id.pub_key) {
        snprintf(pair, sizeof(pair), "%02x", byte); key += pair;
      }
      assert(setup.administer(("trust " + key).c_str()).find("Saved ") == 0);
      setup.learn(owner);
      assert(setup.command(owner, "!wnote retained") == "Note saved");
    }
    // Keep the selected journal and its hash valid, but make guest initialization fail.
    const auto broken = failingWasm ? module("fault-9") : std::string("missing_boot_initializer()");
    nvs_handle_t handle;
    assert(nvs_open("mc-mast-admin", NVS_READWRITE, &handle) == ESP_OK);
    const char *journal = failingWasm ? "wasm-source" : "source";
    size_t size = 0;
    assert(nvs_get_blob(handle, journal, nullptr, &size) == ESP_OK && size == 572);
    Bytes record(size);
    assert(nvs_get_blob(handle, journal, record.data(), &size) == ESP_OK &&
           queued_tx::get32(record.data()) == 1 && record[8] < 3);
    const unsigned slot = record[8];
    assert(broken.size() <= BotSourceLimit);
    queued_tx::put32(record.data() + 12 + slot * 4, uint32_t(broken.size()));
    mesh::Utils::sha256(record.data() + 24 + slot * 32, 32,
                       reinterpret_cast<const uint8_t *>(broken.data()), broken.size());
    assert(nvs_set_blob(handle, journal, record.data(), record.size()) == ESP_OK &&
           nvs_commit(handle) == ESP_OK);
    nvs_close(handle);
    const char *luaPaths[] = {"/command-bot/a.lua", "/command-bot/b.lua", "/command-bot/c.lua"};
    const char *wasmPaths[] = {"/command-bot/wa.lua", "/command-bot/wb.lua", "/command-bot/wc.lua"};
    auto file = SPIFFS.open((failingWasm ? wasmPaths : luaPaths)[slot], "w");
    assert(file && file.write(reinterpret_cast<const uint8_t *>(broken.data()), broken.size()) == broken.size());
    file.flush(); file.close();
    {
      Fixture recovered(options);
      recovered.startReplay(error);
      recovered.step(4000);
      assert(recovered.bot.sourceReady() && !recovered.bot.sourceDeploymentReady());
      const auto status = recovered.administer(failingWasm ? "source wasm status" : "source status");
      printf("Runtime recovery status: %s\n", status.c_str());
      fflush(stdout);
      assert(status.find("Error:") != std::string::npos &&
             status.find(failingWasm ? "instruction limit exceeded" : "missing_boot_initializer") != std::string::npos);
      recovered.learn(owner); recovered.learn(stranger);
      assert(recovered.command(owner, failingWasm ? "!lkeep" : "!wnote") ==
             (failingWasm ? "Lua retained" : "retained"));
      assert(recovered.command(owner, "!ping") == "Pong");
      assert(recovered.command(stranger, "!admin source status").find("permission not granted") != std::string::npos);
      const auto ownerStatus = recovered.command(owner, "!admin source status");
      printf("Authenticated runtime recovery status: %s\n", ownerStatus.c_str());
      fflush(stdout);
      assert(ownerStatus.find("active=") != std::string::npos);
      if (failingWasm) {
        // Wasm source controls retain their existing private native-management path.
        assert(recovered.command(owner, "!admin source wasm status").find("native management CLI/web") != std::string::npos);
        assert(recovered.administer("source wasm remove").find("Accepted verification") == 0);
      } else {
        assert(recovered.command(owner, "!admin source remove").find("Accepted verification") == 0);
      }
      recovered.step(1000);
      assert(recovered.bot.sourceDeploymentReady());
      assert(recovered.command(owner, failingWasm ? "!lkeep" : "!wnote") ==
             (failingWasm ? "Lua retained" : "retained"));
      assert(recovered.command(owner, "!ping") == "Pong");
    }
  }
  puts("PASS failed durable Lua/Wasm initialization: other runtime and native ping stay admitted; authenticated owner recovery works and strangers remain denied");
}
#endif
static std::string runnerText(std::string value) {
  for (char &byte : value) if (byte == '\r' || byte == '\n') byte = ' ';
  return value;
}
class RunnerOutput {
public:
  explicit RunnerOutput(bool jsonl) : jsonl_(jsonl) {}
  RunnerOutput(const RunnerOutput &) = delete;
  RunnerOutput &operator=(const RunnerOutput &) = delete;
  ~RunnerOutput() { restore(); }

  bool begin() {
    if (!jsonl_) return true;
    std::fflush(nullptr);
    stdout_ = dup(STDOUT_FILENO);
    stderr_ = dup(STDERR_FILENO);
    null_ = open("/dev/null", O_WRONLY);
    if (stdout_ < 0 || stderr_ < 0 || null_ < 0) {
      closeDescriptors();
      return false;
    }
    if (dup2(null_, STDOUT_FILENO) < 0) {
      closeDescriptors();
      return false;
    }
    suppressed_ = true;
    if (dup2(null_, STDERR_FILENO) < 0) {
      restore();
      return false;
    }
    return true;
  }

  bool write(FILE *stream, const std::string &text) const {
    if (!jsonl_) {
      return std::fwrite(text.data(), 1, text.size(), stream) == text.size() &&
             std::fflush(stream) == 0;
    }
    return writeAll(stream == stderr ? stderr_ : stdout_, text);
  }

private:
  static bool writeAll(int descriptor, const std::string &text) {
    size_t offset = 0;
    while (offset < text.size()) {
      const auto written = ::write(descriptor, text.data() + offset, text.size() - offset);
      if (written < 0 && errno == EINTR) continue;
      if (written <= 0) return false;
      offset += size_t(written);
    }
    return true;
  }

  void closeDescriptors() {
    if (stdout_ >= 0) close(stdout_);
    if (stderr_ >= 0) close(stderr_);
    if (null_ >= 0) close(null_);
    stdout_ = stderr_ = null_ = -1;
  }

  void restore() {
    if (suppressed_) {
      std::fflush(nullptr);
      if (stdout_ >= 0) dup2(stdout_, STDOUT_FILENO);
      if (stderr_ >= 0) dup2(stderr_, STDERR_FILENO);
      suppressed_ = false;
    }
    closeDescriptors();
  }

  bool jsonl_ = false, suppressed_ = false;
  int stdout_ = -1, stderr_ = -1, null_ = -1;
};
static std::string runnerJsonString(const std::string &value) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string escaped = "\"";
  for (const unsigned char byte : value) {
    if (byte == '"' || byte == '\\') {
      escaped.push_back('\\'); escaped.push_back(char(byte));
    } else if (byte == '\b') escaped += "\\b";
    else if (byte == '\f') escaped += "\\f";
    else if (byte == '\n') escaped += "\\n";
    else if (byte == '\r') escaped += "\\r";
    else if (byte == '\t') escaped += "\\t";
    else if (byte < 32 || byte >= 127) {
      escaped += "\\u00";
      escaped.push_back(digits[byte >> 4]);
      escaped.push_back(digits[byte & 15]);
    } else escaped.push_back(char(byte));
  }
  escaped.push_back('"');
  return escaped;
}
static std::string runnerStringField(const char *name, const std::string &value) {
  return ",\"" + std::string(name) + "\":" + runnerJsonString(value);
}
static std::string runnerNumberField(const char *name, int64_t value) {
  return ",\"" + std::string(name) + "\":" + std::to_string(value);
}
static std::string runnerBoolField(const char *name, bool value) {
  return ",\"" + std::string(name) + "\":" + (value ? "true" : "false");
}
static bool runnerJsonEvent(RunnerOutput &output, const char *type,
                            const std::string &fields = {}) {
  const auto record = "{\"format\":\"meshcore-bot-replay-v1\",\"type\":" +
      runnerJsonString(type) + fields + "}\n";
  return output.write(stdout, record);
}
static bool runnerJsonError(RunnerOutput &output, const char *code,
                            const std::string &message) {
  const auto record = "{\"format\":\"meshcore-bot-replay-v1\",\"type\":\"error\"" +
      runnerStringField("code", code) + runnerStringField("message", message) + "}\n";
  return output.write(stderr, record);
}
static int runnerFailure(RunnerOutput &output, bool jsonl, const char *code,
                         const std::string &message, const std::string &text,
                         int exitCode) {
  if (jsonl) runnerJsonError(output, code, message);
  else output.write(stderr, text + "\n");
  return exitCode;
}
static std::string hostSourceSha256(const std::string &source) {
  uint8_t digest[32]{};
  mesh::Utils::sha256(digest, sizeof(digest),
                      reinterpret_cast<const uint8_t *>(source.data()), source.size());
  static constexpr char digits[] = "0123456789abcdef";
  std::string hex(sizeof(digest) * 2, '0');
  for (size_t i = 0; i < sizeof(digest); ++i) {
    hex[2 * i] = digits[digest[i] >> 4];
    hex[2 * i + 1] = digits[digest[i] & 15];
  }
  return hex;
}
static bool readHostSource(const std::string &path, std::string &source, std::string &error) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    error = "cannot open package"; return false;
  }
  file.seekg(0, std::ios::end);
  const auto end = file.tellg();
  if (end <= 0 || uint64_t(end) > BotSourceLimit) {
    error = "package source must be 1..4096 bytes"; return false;
  }
  source.resize(size_t(end));
  file.seekg(0, std::ios::beg);
  file.read(source.data(), std::streamsize(source.size()));
  if (file.gcount() != std::streamsize(source.size())) {
    error = "package source read incomplete"; source.clear(); return false;
  }
  return true;
}
static int botHostRunner(int argc, char **argv) {
  bool validate = false, jsonl = false, bundled = false;
  std::string sourcePath, replayPath;
  std::string argumentError, textArgumentError;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--validate" && i + 1 < argc) {
      validate = true; sourcePath = argv[++i];
    } else if (arg == "--source" && i + 1 < argc) sourcePath = argv[++i];
    else if (arg == "--bundled") bundled = true;
    else if (arg == "--replay" && i + 1 < argc) replayPath = argv[++i];
    else if (arg == "--format" && i + 1 < argc) {
      const std::string format = argv[++i];
      if (format == "text") jsonl = false;
      else if (format == "jsonl") jsonl = true;
      else {
        argumentError = "unsupported_format";
        textArgumentError = "Replay format must be text or jsonl";
        break;
      }
    }
    else {
      argumentError = "usage";
      textArgumentError = "Usage: bot-host-runner --validate SOURCE | --source SOURCE | --bundled "
                          "[--replay FILE] [--format text|jsonl]";
      break;
    }
  }
  RunnerOutput output(jsonl);
  if (jsonl && !output.begin()) {
    const std::string error =
        "{\"format\":\"meshcore-bot-replay-v1\",\"type\":\"error\","
        "\"code\":\"native_output\",\"message\":\"could not isolate native diagnostics\"}\n";
    std::fprintf(stderr, "%s", error.c_str());
    return 1;
  }
  if (!argumentError.empty())
    return runnerFailure(output, jsonl, argumentError.c_str(), textArgumentError,
                         textArgumentError, 2);
  if (sourcePath.empty() && !bundled) {
    return runnerFailure(output, jsonl, "source_required", "a source package is required",
                         "A source package is required", 2);
  }
  if (bundled && (!sourcePath.empty() || validate))
    return runnerFailure(output, jsonl, "usage", "--bundled cannot be combined with a source",
                         "Select --bundled or --source SOURCE", 2);
  if (validate && !replayPath.empty()) {
    return runnerFailure(output, jsonl, "usage", "--validate cannot be combined with --replay",
                         "Usage: bot-host-runner --validate SOURCE", 2);
  }
  std::string source, error;
  if (!bundled && !readHostSource(sourcePath, source, error)) {
    return runnerFailure(output, jsonl, "source_read", error,
                         "Package " + sourcePath + ": " + error, 2);
  }
  if (!prepareHostRunner()) {
    return runnerFailure(output, jsonl, "runtime_init", "native command-bot initialization failed",
                         "Native command-bot test environment initialization failed", 1);
  }
  const auto makeFixture = [] {
    Fixture::Options options;
    options.nativeAdministrationForReplay = true;
    return std::make_unique<Fixture>(options);
  };
  auto fixture = makeFixture();
  struct SourceFaultGuard { ~SourceFaultGuard() { clearSourceFaults(); } } sourceFaultGuard;
  if (!fixture->startReplay(error))
    return runnerFailure(output, jsonl, "runtime_init", error, error, 1);
  if (!bundled && !stageHostSource(*fixture, source, !validate, error)) {
    return runnerFailure(output, jsonl, validate ? "source_validation" : "source_install",
                         error, std::string("Package ") +
                         (validate ? "rejected: " : "install failed: ") + error, 1);
  }
  if (validate) {
#if ONCHIP_BOT_WASM
    const uint8_t *module = nullptr; size_t moduleSize = 0;
    const bool wasm = botWasmBytes(source.data(), source.size(), module, moduleSize);
#else
    const bool wasm = false;
#endif
    if (jsonl) {
      const std::string fields =
          runnerStringField("runtime", wasm ? "wamr-2.4.1" : "lua-5.5.1") +
          runnerStringField("api", wasm ? "meshcore-v1" : "named-commands-v1") +
          runnerStringField("supervisor", "BotWorker") +
          runnerStringField("command", "CommandBot") +
          runnerNumberField("source_bytes", int64_t(bundled ? strlen(BotDefaultSource) : source.size())) +
          runnerStringField("sha256", hostSourceSha256(bundled ? BotDefaultSource : source));
      if (!runnerJsonEvent(output, "validated", fields))
        return runnerFailure(output, true, "output_write", "cannot write validation record",
                             "Validation output write failed", 1);
    } else {
      std::printf("VALIDATED runtime=%s api=%s supervisor=BotWorker command=CommandBot\n",
                  wasm ? "wamr-2.4.1" : "lua-5.5.1", wasm ? "meshcore-v1" : "named-commands-v1");
    }
    return 0;
  }
  if (jsonl) {
    RadioDashboard::RoleStatus status;
    fixture->bot.dashboardStatus(status);
    const std::string fields =
        runnerStringField("action", "activated") +
        (bundled ? runnerStringField("source", "bundled") : "") +
        runnerNumberField("generation", status.source_generation) +
        runnerNumberField("source_slot", status.source_slot) +
        runnerNumberField("source_bytes", int64_t(bundled ? strlen(BotDefaultSource) : source.size())) +
        runnerStringField("sha256", hostSourceSha256(bundled ? BotDefaultSource : source));
    if (!runnerJsonEvent(output, "source", fields))
      return runnerFailure(output, true, "output_write", "cannot write source record",
                           "Replay output write failed", 1);
  }
  std::map<std::string, std::unique_ptr<Peer>> peers;
  peers["default"] = std::make_unique<Peer>();
  fixture->learn(*peers["default"]);
  std::string selectedPeer = "default", nickname = "developer", channelName;
  bool channelContext = false;
  const auto word = [](const std::string &value) {
    if (value.empty() || value.size() > 24) return false;
    for (unsigned char c : value)
      if (!std::isalnum(c) && c != '_' && c != '-') return false;
    return true;
  };
  const auto rejectDirective = [&](const char *code, const std::string &line) {
    if (jsonl) runnerJsonEvent(output, "reject",
                              runnerStringField("code", code) + runnerStringField("line", line));
    else std::printf("REJECT %s: %s\n", code, runnerText(line).c_str());
  };
  const auto drain = [&] {
    for (const auto &entry : peers) {
      for (const auto &text : entry.second->replies(fixture->bot.publicKey(), fixture->radio)) {
        if (jsonl) runnerJsonEvent(output, "async_reply",
                                  runnerStringField("peer", entry.first) +
                                  runnerStringField("kind", "dm") + runnerStringField("text", text));
        else std::printf("ASYNC %s => %s\n", entry.first.c_str(), runnerText(text).c_str());
      }
    }
    if (!channelName.empty())
      for (const auto &text : peers[selectedPeer]->groupReplies(fixture->radio, 1, channelName.c_str())) {
        if (jsonl) runnerJsonEvent(output, "async_reply",
                                  runnerStringField("kind", "channel") +
                                  runnerStringField("channel", channelName) + runnerStringField("text", text));
        else std::printf("ASYNC %s => %s\n", channelName.c_str(), runnerText(text).c_str());
      }
    fixture->radio.sent.clear();
  };
  std::ifstream replayFile;
  std::istream *replay = &std::cin;
  if (!replayPath.empty()) {
    replayFile.open(replayPath);
    if (!replayFile) {
      return runnerFailure(output, jsonl, "replay_open", "cannot open replay file",
                           "Cannot open replay file " + replayPath, 2);
    }
    replay = &replayFile;
  }
  bool duplicateNext = false;
  std::string line;
  while (std::getline(*replay, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    if (line == "@restart") {
      identity_test::failRead = identity_test::failCommit = false;
      clearSourceFaults();
      sourceResultHeld = false;
      duplicateNext = false;
      fixture.reset();
      fixture = makeFixture();
      if (!fixture->startReplay(error))
        return runnerFailure(output, jsonl, "source_recovery", error, error, 1);
      for (auto &entry : peers) fixture->learn(*entry.second);
      RadioDashboard::RoleStatus status;
      fixture->bot.dashboardStatus(status);
      const auto deployment = fixture->administer("source status");
      bundled = deployment.find(" active=3 ") != std::string::npos;
      if (jsonl) {
        const std::string fields =
            runnerNumberField("generation", status.source_generation) +
            runnerNumberField("source_slot", status.source_slot) +
            runnerStringField("source", bundled ? "bundled" : "journal") +
            runnerStringField("deployment", deployment);
        if (!runnerJsonEvent(output, "restart", fields))
          return runnerFailure(output, true, "output_write", "cannot write restart record",
                               "Replay output write failed", 1);
      } else std::printf("RESTART generation=%u source-slot=%d\n",
                         status.source_generation, status.source_slot);
      continue;
    }
    if (line.rfind("@context ", 0) == 0) {
      std::istringstream fields(line.substr(9));
      std::string kind, alias, nick, extra;
      fields >> kind >> alias;
      if (kind == "channel") fields >> nick;
      if (!word(alias) || (kind != "dm" && kind != "channel") ||
          (kind == "channel" && (channelName.empty() || !word(nick))) || (fields >> extra)) {
        rejectDirective("invalid_context", line); continue;
      }
      if (!peers.count(alias)) {
        peers[alias] = std::make_unique<Peer>(); fixture->learn(*peers[alias]);
      }
      selectedPeer = alias; channelContext = kind == "channel";
      if (channelContext) nickname = nick;
      std::string key;
      char pair[3];
      for (uint8_t byte : peers[alias]->self_id.pub_key) {
        snprintf(pair, sizeof(pair), "%02x", byte); key += pair;
      }
      if (jsonl) runnerJsonEvent(output, "context",
                                runnerStringField("kind", kind) + runnerStringField("peer", alias) +
                                runnerStringField("public_key", key) +
                                runnerBoolField("authenticated", !channelContext) +
                                runnerBoolField("owner", !channelContext &&
                                    fixture->trustedOwner(peers[alias]->self_id.pub_key)));
      else std::printf("CONTEXT %s %s key=%s\n", kind.c_str(), alias.c_str(), key.c_str());
      continue;
    }
    if (line.rfind("@channel ", 0) == 0) {
      BotRadioPolicy policy;
      const auto name = line.substr(9);
      if (!loadBotRadioPolicy(policy) || name.empty() || name.size() >= sizeof(policy.channel) ||
          name[0] != '#' || name.find(' ') != std::string::npos) {
        rejectDirective("invalid_channel", line); continue;
      }
      strcpy(policy.channel, name.c_str()); policy.pathWidth = 1;
      if (!saveBotRadioPolicy(policy)) { rejectDirective("channel_commit", line); continue; }
      clearSourceFaults();
      sourceResultHeld = false;
      drain(); fixture.reset(); fixture = makeFixture();
      if (!fixture->startReplay(error))
        return runnerFailure(output, jsonl, "source_recovery", error, error, 1);
      channelName = name;
      for (auto &entry : peers) fixture->learn(*entry.second);
      fixture->radio.sent.clear();
      if (jsonl) runnerJsonEvent(output, "channel", runnerStringField("name", name) +
                                runnerBoolField("restarted", true));
      else std::printf("CHANNEL %s (native bot restarted)\n", name.c_str());
      continue;
    }
    if (line.rfind("@owner ", 0) == 0) {
      const auto alias = line.substr(7);
      if (alias != "none" && !word(alias)) { rejectDirective("invalid_owner", line); continue; }
      std::string key = "none";
      if (alias != "none") {
        if (!peers.count(alias)) {
          peers[alias] = std::make_unique<Peer>(); fixture->learn(*peers[alias]);
        }
        key.clear();
        char pair[3];
        for (auto byte : peers[alias]->self_id.pub_key) {
          snprintf(pair, sizeof(pair), "%02x", byte); key += pair;
        }
      }
      const auto reply = fixture->administer(("trust " + key).c_str());
      const bool ok = reply.rfind("Saved ", 0) == 0;
      if (jsonl) runnerJsonEvent(output, "owner",
                                runnerStringField("peer", alias) + runnerStringField("public_key", key) +
                                runnerBoolField("ok", ok) + runnerStringField("message", reply));
      else std::printf("OWNER %s ok=%u %s\n", alias.c_str(), ok, reply.c_str());
      continue;
    }
    if (line.rfind("@source ", 0) == 0 || line.rfind("@source-now ", 0) == 0) {
      const bool immediate = line.rfind("@source-now ", 0) == 0;
      const auto operation = line.substr(immediate ? 12 : 8);
      const auto control = operation.rfind("wasm ", 0) == 0 ? operation.substr(5) : operation;
      if (control != "status" && control != "hash" && control != "metadata" && control != "cancel" &&
          control != "retry" && control != "rollback" && control != "remove") {
        rejectDirective("invalid_source_control", line); continue;
      }
      const auto reply = fixture->administer(("source " + operation).c_str());
      if (jsonl) runnerJsonEvent(output, "source_control",
                                runnerStringField("command", operation) +
                                runnerBoolField("ok", reply.rfind("Error:", 0) != 0) +
                                runnerStringField("text", reply));
      else std::printf("SOURCE %s => %s\n", operation.c_str(), reply.c_str());
      if (!immediate) fixture->step(80);
      drain(); continue;
    }
    if (line == "@staging") {
      auto file = SPIFFS.open(BotStagedSourcePath, "r");
      std::string bytes(file ? file.size() : 0, '\0');
      if (file && file.read(reinterpret_cast<uint8_t *>(bytes.data()), bytes.size()) != bytes.size())
        return runnerFailure(output, jsonl, "staging_read", "cannot read staging snapshot",
                             "Cannot read staging snapshot", 1);
      RadioDashboard::RoleStatus status;
      fixture->bot.dashboardStatus(status);
      if (jsonl) runnerJsonEvent(output, "staging",
                                runnerNumberField("source_bytes", bytes.size()) +
                                runnerStringField("sha256", hostSourceSha256(bytes)) +
                                runnerBoolField("ready", status.ready));
      else std::printf("STAGING bytes=%zu sha256=%s\n", bytes.size(), hostSourceSha256(bytes).c_str());
      continue;
    }
    if (line.rfind("@reflect ", 0) == 0) {
      const auto command = line.substr(9);
      if (command.empty() || command[0] != '!') { rejectDirective("invalid_reflection", line); continue; }
      fixture->reflect(peers[selectedPeer]->command(fixture->bot.publicKey(), command.c_str()));
      fixture->step(80);
      if (jsonl) runnerJsonEvent(output, "reflection", runnerStringField("peer", selectedPeer) +
                                runnerStringField("command", command));
      else std::printf("REFLECTION %s %s\n", selectedPeer.c_str(), command.c_str());
      drain(); continue;
    }
    if (line.rfind("@grant ", 0) == 0) {
      std::istringstream fields(line.substr(7));
      std::string name, state, extra;
      fields >> name >> state;
      bool ok = false;
      if ((state == "on" || state == "off") && !(fields >> extra)) {
        const bool enabled = state == "on";
        if (name == "shared") ok = fixture->bot.setSharedState(enabled);
        else if (name == "reminders") ok = fixture->bot.setReminderAccess(enabled);
        else if (name == "home") ok = fixture->bot.setHomeAccess(enabled);
        else if (name == "events") ok = fixture->bot.setEventAccess(enabled ? 15 : 0);
        else if (name == "channel-wait") {
          BotMeshPolicy policy;
          if (loadBotMeshPolicy(policy)) {
            policy.channelWait = enabled; ok = fixture->bot.setMeshPolicy(policy);
          }
        }
      }
      if (jsonl) runnerJsonEvent(output, "grant", runnerStringField("name", name) +
                                runnerBoolField("enabled", state == "on") + runnerBoolField("ok", ok));
      else std::printf("GRANT %s %s ok=%u\n", name.c_str(), state.c_str(), ok);
      continue;
    }
    if (line.rfind("@message ", 0) == 0) {
      std::istringstream fields(line.substr(9));
      std::string kind, alias, text;
      fields >> kind >> alias;
      std::getline(fields, text);
      if (!text.empty() && text[0] == ' ') text.erase(0, 1);
      if (!word(alias) || text.empty() || text.size() > 120 ||
          (kind != "dm" && kind != "channel") ||
          (kind == "channel" && channelName.empty()) || text[0] == '!') {
        rejectDirective("invalid_message", line); continue;
      }
      if (!peers.count(alias)) {
        peers[alias] = std::make_unique<Peer>(); fixture->learn(*peers[alias]);
      }
      fixture->deliver(kind == "dm" ?
          peers[alias]->command(fixture->bot.publicKey(), text.c_str()) :
          peers[alias]->group((alias + ": " + text).c_str(), channelName.c_str(), 1));
      fixture->step(80);
      if (jsonl) runnerJsonEvent(output, "message", runnerStringField("kind", kind) +
                                runnerStringField("peer", alias));
      else std::printf("MESSAGE %s %s\n", kind.c_str(), alias.c_str());
      drain(); continue;
    }
#ifdef BOT_HOST_RUNNER
    if (line.rfind("@network ", 0) == 0) {
      const bool ok = bot_native_test::replayNetwork(line.substr(9), error);
      if (jsonl) runnerJsonEvent(output, "network",
                                runnerBoolField("ok", ok) +
                                runnerNumberField("submissions", bot_native_test::replayNetworkSubmissions()) +
                                (ok ? "" : runnerStringField("message", error)));
      else std::printf("NETWORK ok=%u %s\n", ok, ok ? "" : error.c_str());
      continue;
    }
#endif
    if (line.rfind("@route ", 0) == 0) {
      const auto text = line.substr(7);
      BotPath route;
      bool valid = text.size() >= 2 && text[0] >= '1' && text[0] <= '3' && text[1] == ':';
      if (valid) {
        route.width = text[0] - '0';
        const auto bytes = (text.size() - 2) / 2;
        valid = !(text.size() % 2) && bytes <= sizeof(route.bytes) &&
                bytes % route.width == 0;
        if (valid) {
          route.count = bytes / route.width;
          route.known = true;
          valid = route.valid();
          const auto digit = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
          };
          for (size_t i = 0; valid && i < bytes; ++i) {
            const auto high = digit(text[2 + i * 2]), low = digit(text[3 + i * 2]);
            valid = high >= 0 && low >= 0;
            if (valid) route.bytes[i] = (high << 4) | low;
          }
        }
      }
      if (!valid) {
        rejectDirective("invalid_route", line); continue;
      }
      fixture->deliver(peers[selectedPeer]->pathReturn(fixture->bot.publicKey(), route.width,
                            Bytes(route.bytes, route.bytes + route.size())));
      fixture->step(20);
      if (jsonl) runnerJsonEvent(output, "route", runnerStringField("peer", selectedPeer) +
                                runnerStringField("path", line.substr(7)));
      else std::printf("ROUTE %s %s\n", selectedPeer.c_str(), line.substr(7).c_str());
      continue;
    }
    if (line == "@cancel" || line.rfind("@pump ", 0) == 0) {
      unsigned steps = 80;
      if (line == "@cancel") fixture->bot.cancelJobs();
      else {
        const auto value = line.substr(6);
        try {
          size_t used;
          const auto parsed = std::stoul(value, &used);
          if (!parsed || parsed > 10000 || used != value.size()) throw std::out_of_range("steps");
          steps = unsigned(parsed);
        } catch (const std::exception &) { rejectDirective("invalid_pump", line); continue; }
      }
      fixture->step(steps);
      if (jsonl) runnerJsonEvent(output, line == "@cancel" ? "cancel" : "pump",
                                runnerNumberField("steps", steps));
      else std::printf("%s steps=%u\n", line == "@cancel" ? "CANCEL" : "PUMP", steps);
      drain(); continue;
    }
    if (line.rfind("@clock ", 0) == 0) {
      uint32_t epoch = 0;
      bool valid = false;
      try {
        const auto value = line.substr(7);
        size_t consumed = 0;
        const auto parsed = std::stoul(value, &consumed);
        valid = !value.empty() && consumed == value.size() && parsed <= UINT32_MAX;
        if (valid) epoch = uint32_t(parsed);
      } catch (const std::exception &) {}
      if (!valid) {
        if (jsonl) {
          if (!runnerJsonEvent(output, "reject",
                               runnerStringField("code", "invalid_clock") +
                               runnerStringField("line", line)))
            return runnerFailure(output, true, "output_write", "cannot write reject record",
                                 "Replay output write failed", 1);
        } else std::puts("REJECT invalid @clock epoch");
        continue;
      }
      ClockSnapshot beforeClocks;
      const bool hadBefore = clockSnapshot(beforeClocks);
      receiveNetworkTime(epoch);
      fixture->step(1);
      ClockSnapshot clocks;
      const bool captured = clockSnapshot(clocks);
      const bool accepted = captured && hadBefore && clocks.network_epoch == epoch &&
          clocks.rejected_samples == beforeClocks.rejected_samples;
      uint32_t earliest = 0, latest = 0;
      const bool trusted = accepted && trustedNetworkTime(earliest, latest);
      if (jsonl) {
        const std::string fields =
            runnerNumberField("epoch", epoch) +
            runnerBoolField("accepted", accepted) +
            runnerBoolField("trusted", trusted);
        if (!runnerJsonEvent(output, "clock", fields))
          return runnerFailure(output, true, "output_write", "cannot write clock record",
                               "Replay output write failed", 1);
      } else std::printf("CLOCK epoch=%u accepted=%u trusted=%u\n",
                         epoch, accepted, trusted);
      continue;
    }
    if (line.rfind("@install ", 0) == 0) {
      const auto path = line.substr(9);
      std::string nextSource;
      if (!readHostSource(path, nextSource, error) ||
          !stageHostSource(*fixture, nextSource, true, error)) {
        if (jsonl) {
          const std::string fields =
              runnerBoolField("ok", false) + runnerStringField("message", error);
          if (!runnerJsonEvent(output, "install", fields))
            return runnerFailure(output, true, "output_write", "cannot write install record",
                                 "Replay output write failed", 1);
        } else std::printf("REJECT package update: %s\n", error.c_str());
        continue;
      }
      source = nextSource; bundled = false;
      if (jsonl) {
        RadioDashboard::RoleStatus status;
        fixture->bot.dashboardStatus(status);
        const std::string fields =
            runnerBoolField("ok", true) +
            runnerNumberField("generation", status.source_generation) +
            runnerNumberField("source_slot", status.source_slot) +
            runnerNumberField("source_bytes", int64_t(nextSource.size())) +
            runnerStringField("sha256", hostSourceSha256(nextSource));
        if (!runnerJsonEvent(output, "install", fields))
          return runnerFailure(output, true, "output_write", "cannot write install record",
                               "Replay output write failed", 1);
      } else std::puts("INSTALLED complete source generation");
      continue;
    }
    if (line == "@duplicate") {
      duplicateNext = true; continue;
    }
    if (line.rfind("@fault ", 0) == 0) {
      const auto fault = line.substr(7);
      bool enabled = true, known = true;
      if (fault == "storage") identity_test::failRead = true;
      else if (fault == "commit" || fault == "unknown-commit") identity_test::failCommit = true;
      else if (fault == "source-copy-pause") sourceCopyPaused = true;
      else if (fault == "source-live-pause") sourceLivePauseOnCommit = true;
      else if (fault == "source-live-read") sourceLiveReadFaultOnCommit = true;
      else if (fault == "source-result-held") {
        const char *held = "function fixture_held() return 'not activated' end";
        if (sourceResultHeld || !fixture->bot.stageSource(held, strlen(held)))
          return runnerFailure(output, jsonl, "source_result_hold", "native cold staging was refused",
                               "Native cold staging was refused", 1);
        sourceResultHeld = true;
        fixture->step(80);
      }
      else if (fault == "source-copy-wait") {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!sourceCopyPauseEntered.load() && std::chrono::steady_clock::now() < deadline)
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (!sourceCopyPauseEntered.load()) {
          clearSourceFaults();
          return runnerFailure(output, jsonl, "source_copy_pause", "source copy did not reach pause",
                               "Source copy did not reach pause", 1);
        }
      }
      else if (fault == "off") {
        identity_test::failRead = identity_test::failCommit = false;
        clearSourceFaults();
        if (sourceResultHeld) {
          BotWorker::Result result;
          if (!fixture->bot.pollSourceResult(result) || !result.ok)
            return runnerFailure(output, jsonl, "source_result_release", "native cold staging result unavailable",
                                 "Native cold staging result unavailable", 1);
          sourceResultHeld = false;
        }
        enabled = false;
      } else known = false;
      if (jsonl) {
        const std::string fields = runnerStringField("fault", fault) +
            (known ? runnerBoolField("enabled", enabled) : std::string());
        if (!runnerJsonEvent(output, known ? "fault" : "reject",
                             fields + (known ? "" : runnerStringField("code", "unknown_fault"))))
          return runnerFailure(output, true, "output_write", "cannot write fault record",
                               "Replay output write failed", 1);
      } else if (!known) std::printf("REJECT unknown fault: %s\n", fault.c_str());
      continue;
    }
    if (line.rfind("@advance ", 0) == 0) {
      unsigned long milliseconds = 0;
      bool valid = false;
      try {
        const auto value = line.substr(9);
        size_t consumed = 0;
        milliseconds = std::stoul(value, &consumed);
        ClockSnapshot clocks;
        const bool captured = clockSnapshot(clocks);
        const uint64_t lag = captured ? uint32_t(millis() - clocks.sampled_at_ms) : 0;
        const uint64_t elapsed = captured ? clocks.network_age_ms + lag : UINT64_MAX;
        const uint64_t maximum = uint64_t(UINT32_MAX) - 2;
        valid = !value.empty() && consumed == value.size() && captured &&
            elapsed <= maximum && milliseconds <= maximum - elapsed;
      } catch (const std::exception &) {}
      if (!valid) {
        if (jsonl) {
          if (!runnerJsonEvent(output, "reject",
                               runnerStringField("code", "invalid_advance") +
                               runnerStringField("line", line)))
            return runnerFailure(output, true, "output_write", "cannot write reject record",
                                 "Replay output write failed", 1);
        } else std::puts("REJECT invalid @advance milliseconds");
        continue;
      }
      timeMs += milliseconds;
      fixture->step(80);
      if (jsonl && !runnerJsonEvent(output, "advance",
                                   runnerNumberField("milliseconds", int64_t(milliseconds))))
        return runnerFailure(output, true, "output_write", "cannot write advance record",
                             "Replay output write failed", 1);
      drain();
      continue;
    }
    if (line == "@status") {
      RadioDashboard::RoleStatus status;
      fixture->bot.dashboardStatus(status);
      const auto &counts = fixture->bot.counters();
      if (jsonl) {
        const std::string fields =
            runnerNumberField("generation", status.source_generation) +
            runnerNumberField("source_slot", status.source_slot) +
            runnerNumberField("replies", counts.replies) +
            runnerNumberField("duplicates", counts.duplicates) +
            runnerNumberField("vm_failures", counts.vmFailures) +
            runnerNumberField("rejected", counts.rejected) +
            runnerNumberField("airtime_limited", counts.airtimeLimited) +
            runnerNumberField("busy", counts.busy) +
            runnerNumberField("jobs", fixture->bot.jobsInUse());
        if (!runnerJsonEvent(output, "status", fields))
          return runnerFailure(output, true, "output_write", "cannot write status record",
                               "Replay output write failed", 1);
      } else std::printf("STATUS generation=%u replies=%u duplicates=%u vm-failures=%u\n",
                         status.source_generation, counts.replies, counts.duplicates,
                         counts.vmFailures);
      continue;
    }
    const bool asynchronous = line.rfind("@send ", 0) == 0;
    if (asynchronous) line = line.substr(6);
    if (line.empty() || line[0] != '!') {
      if (jsonl) {
        if (!runnerJsonEvent(output, "reject",
                             runnerStringField("code", "unsupported_line") +
                             runnerStringField("line", line)))
          return runnerFailure(output, true, "output_write", "cannot write reject record",
                               "Replay output write failed", 1);
      } else std::printf("REJECT unsupported replay line: %s\n", runnerText(line).c_str());
      continue;
    }
    BotEvent parsedCommand;
    char parseError[128]{};
    if (!parseBotCommand(line.c_str(), line.size(), parsedCommand,
                         parseError, sizeof(parseError))) {
      if (jsonl) {
        const std::string fields =
            runnerStringField("code", "invalid_native_command") +
            runnerStringField("command", line) +
            runnerStringField("message", parseError);
        if (!runnerJsonEvent(output, "reject", fields))
          return runnerFailure(output, true, "output_write", "cannot write command reject",
                               "Replay output write failed", 1);
      } else std::printf("REJECT invalid native command %s: %s\n",
                         runnerText(line).c_str(), parseError);
      continue;
    }
    try {
      std::string response;
      auto &peer = *peers[selectedPeer];
      const bool duplicate = duplicateNext;
      drain();
      const auto before = fixture->bot.counters();
      const auto request = channelContext ?
          peer.targetedGroup(fixture->bot.publicKey(), (nickname + ": " + line).c_str(),
                             channelName.c_str(), 1) :
          peer.command(fixture->bot.publicKey(), line.c_str());
      fixture->deliver(request); fixture->step();
      if (duplicateNext) {
        duplicateNext = false;
        fixture->deliver(request); fixture->step();
      }
      const auto replies = channelContext ? peer.groupReplies(fixture->radio, 1, channelName.c_str()) :
          peer.replies(fixture->bot.publicKey(), fixture->radio);
      if (replies.empty() && fixture->bot.counters().rejected != before.rejected) {
        char status[160]{};
        fixture->bot.admissionStatus(status, sizeof(status));
        if (jsonl) runnerJsonEvent(output, "reject", runnerStringField("code", "native_admission") +
                                  runnerStringField("command", line) + runnerStringField("message", status));
        else std::printf("REJECT %s: %s\n", line.c_str(), status);
        continue;
      }
      if (asynchronous) {
        if (jsonl) runnerJsonEvent(output, "send", runnerStringField("command", line) +
                                  runnerStringField("peer", selectedPeer) + runnerBoolField("duplicate", duplicate));
        else std::printf("SEND %s\n", line.c_str());
        continue;
      }
      if (replies.empty()) {
        if (jsonl) runnerJsonEvent(output, "pending", runnerStringField("command", line) +
                                  runnerStringField("peer", selectedPeer));
        else std::printf("PENDING %s; use @advance or @pump\n", line.c_str());
        continue;
      }
      if (replies.size() != 1) {
        rejectDirective("unexpected_reply_count", line); drain(); continue;
      }
      response = replies.front();
      fixture->radio.sent.clear();
      if (jsonl) {
        const std::string fields = runnerStringField("command", line) +
            runnerStringField("text", response) + runnerBoolField("duplicate", duplicate);
        if (!runnerJsonEvent(output, "reply", fields))
          return runnerFailure(output, true, "output_write", "cannot write reply record",
                               "Replay output write failed", 1);
      } else std::printf("REPLY %s => %s\n", line.c_str(), runnerText(response).c_str());
    } catch (const std::exception &exception) {
      if (jsonl) {
        const std::string fields = runnerStringField("code", "command_exception") +
            runnerStringField("command", line) +
            runnerStringField("message", exception.what());
        if (!runnerJsonEvent(output, "error", fields))
          return runnerFailure(output, true, "output_write", "cannot write error record",
                               "Replay output write failed", 1);
      } else std::printf("FAULT %s: %s\n", line.c_str(), exception.what());
    }
  }
  identity_test::failRead = identity_test::failCommit = false;
  return 0;
}
#endif

#ifdef ONCHIP_BOT_RUNTIME_TEST
int main(int argc, char **argv) {
  if (argc == 2 && !strcmp(argv[1], "--contact-recovery-test")) {
    native_contact_recovery();
    return 0;
  }
  if (argc == 2 && !strcmp(argv[1], "--channel-policy-regression-test")) {
    assert(saveBotEnabled(true));
    native_channel_key_configuration();
    native_hashtag_commands();
    native_channel_storage();
    native_board();
    native_admission_feedback();
    return 0;
  }
  if (argc == 2 && !strcmp(argv[1], "--channel-policy-test")) {
    native_channel_policy();
    return 0;
  }
  if (argc == 2 && !strcmp(argv[1], "--thread-policy-test")) {
    native_thread_policy();
    return 0;
  }
  if (argc == 2 && !strcmp(argv[1], "--repeater-test")) {
    native_repeater_monitor();
    return 0;
  }
  if (argc == 2 && !strcmp(argv[1], "--adaptive-test")) {
    native_adaptive_admission();
    return 0;
  }
#ifdef BOT_HOST_RUNNER
#if ONCHIP_BOT_WASM
  if (argc == 3 && !strcmp(argv[1], "--runtime-isolation-test")) {
    native_runtime_admission_isolation(argv[2]);
    return 0;
  }
#endif
  if (argc == 2 && !strcmp(argv[1], "--source-api-test")) {
    native_source_api_capacities();
    return 0;
  }
  if (argc > 1) return botHostRunner(argc, argv);
#else
  (void)argc; (void)argv;
#endif
  Radio physical;
  HardwareRNG rng;
  WifiKissMultiplexer mux;
  mux.attachRadio(physical, rng, [](float, float, uint8_t, uint8_t) {}, [](uint8_t) {});
  assert(mux.setInitialConfiguration({912525000, 250000, 7, 5, 2}, true));
  assert(saveRoleProfile({7}) && saveBotEnabled(true));
  mesh::LocalIdentity modem;
  assert(loadIdentity("modem", modem));
  assert(onchip::begin(mux, modem));
  auto pump = [&]() {
    for (unsigned i = 0; i < 150; ++i) {
      timeMs += 10;
      onchip::loop();
      mux.serviceTransmit();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  };
  pump();
  RadioDashboard::RadioStatus dashboard;
  dashboard.wifi_connected = true;
  onchip::dashboardStatus(dashboard, true);
  assert(dashboard.role_count == 7 && dashboard.roles[6].ready);
  for (unsigned i : {0u, 1u, 2u, 5u, 6u}) {
    const auto &expected = dashboard.roles[i];
    RadioDashboard::RoleStatus source;
    assert(localTransmitSource(expected.source_slot,
                               expected.source_generation, source));
    assert(!strcmp(source.role, expected.role) &&
           !memcmp(source.public_key, expected.public_key, 32));
    assert(!localTransmitSource(expected.source_slot,
                                expected.source_generation + 1, source));
  }
  BotNodeSnapshot node;
  commandBotService().nodeSnapshot(node);
  assert(node.available && node.rolesKnown && node.selectedRoles == 7 &&
         node.readyRoles == 7 && node.ready &&
         !memcmp(node.publicKey, dashboard.roles[6].public_key, sizeof(node.publicKey)));
  assert(!setNativeRolePathWidth(4));
  assert(setNativeRolePathWidth(3));
  assert(repeaterPathWidth() == 3 && roomPathWidth() == 3 && companionPathWidth() == 3);
  assert(managementPathWidth() == 3);
  assert(setNativeRolePathWidth(1));
  assert(!strcmp(dashboard.roles[4].role, "bot") &&
         !strcmp(dashboard.roles[6].role, "command-bot"));
  uint8_t managementKey[32];
  memcpy(managementKey, dashboard.roles[5].public_key, sizeof(managementKey));
  for (unsigned i : {0u, 1u, 2u, 4u, 5u, 6u}) {
    assert(dashboard.roles[i].has_identity);
    for (unsigned j : {0u, 1u, 2u, 4u, 5u, 6u})
      if (i != j)
        assert(memcmp(dashboard.roles[i].public_key, dashboard.roles[j].public_key, 32));
  }
  Peer peer;
  const auto advert = peer.advert();
  mux.received(advert.data(), advert.size(), -90, 5);
  pump();
  int sockets[2];
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
  WiFiServer server;
  server.add(WiFiClient(sockets[0]));
  mux.poll(server);
  const uint8_t hello[] = {0xc0, 6, queued_tx::HELLO, 1, 0, 0xc0};
  assert(send(sockets[1], hello, sizeof(hello), 0) == sizeof(hello));
  mux.poll(server);
  uint8_t clientData[2048];
  assert(recv(sockets[1], clientData, sizeof(clientData), MSG_DONTWAIT) > 4);
  assert(mux.clientCount() == 1);
  timeMs += 61000;
  physical.sent.clear();
  const auto ping = peer.command(dashboard.roles[6].public_key, "!ping");
  mux.received(ping.data(), ping.size(), -90, 5);
  pump();
  const auto replies = peer.replies(dashboard.roles[6].public_key, physical);
  assert(replies.size() == 1 && replies[0] == "Pong");
  // The RTOS seam does not run the diagnostic task: its real eight-entry queue
  // stays full, exactly as when the consumer is blocked in USB output.
  serial_test::forbidWrites = true;
  for (unsigned i = 0; i < 10; ++i) {
    physical.sent.clear();
    const auto request = peer.command(dashboard.roles[6].public_key, "!ping");
    mux.received(request.data(), request.size(), -90, 5);
    pump();
    assert(peer.replies(dashboard.roles[6].public_key, physical) == std::vector<std::string>{"Pong"});
  }
  serial_test::forbidWrites = false;
  char diagnosticStatus[163];
  commandBotService().diagnosticStatus(diagnosticStatus, sizeof(diagnosticStatus));
  unsigned queued = 0, dropped = 0;
  assert(sscanf(diagnosticStatus, "Diagnostics queued=%u dropped=%u", &queued, &dropped) == 2);
  assert(queued <= 8 && dropped >= 12 && strstr(diagnosticStatus, "sink=async"));
  assert(commandBotService().counters().lastVm.peakBytes);
  puts("PASS shared diagnostic queue: blocked consumer/full queue drops logs, ten Pong replies and VM stats continue without dispatch Serial writes");
  mux.poll(server);
  assert(mux.clientCount() == 1 &&
         recv(sockets[1], clientData, sizeof(clientData), MSG_DONTWAIT) > 4);
  close(sockets[1]);
  mux.poll(server);
  repeaterStop(); roomStop(); companionStop();
  companionSessions().end();
  stopManagementForTest();
  assert(psram_test::allocations.empty());
  assert(saveRoleProfile({0}));
  assert(onchip::begin(mux, modem));
  pump();
  onchip::dashboardStatus(dashboard, true);
  ClockSnapshot clocks;
  assert(clockSnapshot(clocks) && clocks.network_enabled);
  assert(dashboard.roles[6].ready &&
         !dashboard.roles[0].ready && !dashboard.roles[1].ready &&
         !dashboard.roles[2].ready);
  commandBotService().nodeSnapshot(node);
  assert(node.rolesKnown && node.selectedRoles == 0 && node.readyRoles == 0 && node.ready);
  assert(dashboard.roles[5].has_identity &&
         !memcmp(dashboard.roles[5].public_key, managementKey, sizeof(managementKey)) &&
         dashboard.roles[5].source_slot >= 0 &&
         dashboard.roles[5].source_slot != dashboard.roles[6].source_slot);
  const char source[] =
      "function custom(text) reply('Bot-only:'..text) end command('custom','text:text:150','Bot only')";
  auto stagedFile = SPIFFS.open(BotStagedSourcePath, "w");
  assert(stagedFile &&
         stagedFile.write(reinterpret_cast<const uint8_t *>(source), sizeof(source) - 1) ==
             sizeof(source) - 1);
  stagedFile.flush(); stagedFile.close();
  uint8_t sourceHash[32];
  mesh::Utils::sha256(sourceHash, sizeof(sourceHash),
                      reinterpret_cast<const uint8_t *>(source), sizeof(source) - 1);
  auto &bot = commandBotService();
  assert(bot.stageSourceFile(sizeof(source) - 1, sourceHash));
  pump();
  BotWorker::Result result;
  assert(bot.pollSourceResult(result) && result.ok &&
         result.operation == BotWorker::Operation::StageFile);
  assert(bot.activateStaged());
  pump();
  assert(bot.pollSourceResult(result) && result.ok);
  assert(SPIFFS.remove(BotStagedSourcePath));
  mux.received(advert.data(), advert.size(), -90, 5);
  pump();
  timeMs += 61000;
  physical.sent.clear();
  const auto custom = peer.command(bot.publicKey(), "!custom ready");
  mux.received(custom.data(), custom.size(), -90, 5);
  pump();
  const auto botOnlyReplies = peer.replies(bot.publicKey(), physical);
  assert(botOnlyReplies.size() == 1 && botOnlyReplies[0] == "Bot-only:ready");
  bot.diagnosticStatus(diagnosticStatus, sizeof(diagnosticStatus));
  assert(strstr(diagnosticStatus, "sink=async"));
  onchip::dashboardStatus(dashboard, true);
  assert(!dashboard.roles[0].ready && !dashboard.roles[1].ready &&
         !dashboard.roles[2].ready &&
         !memcmp(dashboard.roles[5].public_key, managementKey, sizeof(managementKey)));
  stopManagementForTest();
  assert(psram_test::allocations.empty());
  assert(saveBotEnabled(false));
  assert(onchip::begin(mux, modem));
  onchip::dashboardStatus(dashboard, true);
  assert(!dashboard.roles[6].ready && !dashboard.roles[6].has_identity &&
         !strcmp(dashboard.roles[6].state, "disabled") && dashboard.roles[4].ready);
  assert(dashboard.roles[5].has_identity && dashboard.roles[5].source_slot >= 0 &&
         !memcmp(dashboard.roles[5].public_key, managementKey, sizeof(managementKey)));
  stopManagementForTest();
  assert(psram_test::allocations.empty());
  puts("bot runtime: separate management identity, repeater-independent SPIFFS activation, "
       "native roles, KISS coexistence and encrypted replies passed");
}
#else
int main(int argc, char **argv) {
  if (argc == 2 && !strcmp(argv[1], "--contact-recovery-test")) {
    native_contact_recovery();
    return 0;
  }
  if (argc == 2 && !strcmp(argv[1], "--channel-policy-regression-test")) {
    assert(saveBotEnabled(true));
    native_channel_key_configuration();
    native_hashtag_commands();
    native_channel_storage();
    native_board();
    native_admission_feedback();
    return 0;
  }
  if (argc == 2 && !strcmp(argv[1], "--channel-policy-test")) {
    native_channel_policy();
    return 0;
  }
  if (argc == 2 && !strcmp(argv[1], "--thread-policy-test")) {
    native_thread_policy();
    return 0;
  }
  if (argc == 2 && !strcmp(argv[1], "--repeater-test")) {
    native_repeater_monitor();
    return 0;
  }
  if (argc == 2 && !strcmp(argv[1], "--adaptive-test")) {
    native_adaptive_admission();
    return 0;
  }
  if (argc == 2 && !strcmp(argv[1], "--native-discovery-test")) {
    assert(saveBotEnabled(true));
    native_bot_discovery();
    native_bot_discovery_limits();
    return 0;
  }
#ifdef BOT_HOST_RUNNER
#if ONCHIP_BOT_WASM
  if (argc == 3 && !strcmp(argv[1], "--runtime-isolation-test")) {
    native_runtime_admission_isolation(argv[2]);
    return 0;
  }
#endif
  if (argc == 2 && !strcmp(argv[1], "--source-api-test")) {
    native_source_api_capacities();
    return 0;
  }
  if (argc > 1) return botHostRunner(argc, argv);
#else
  (void)argc; (void)argv;
#endif
  selection_identity();
  command_mesh_experience();
  native_bot_discovery();
  native_bot_discovery_limits();
  native_harness_tx_receipts_are_simulated();
  stock_chat_through_native_relay();
  discovery_through_native_relay();
  commands_and_packets();
  collected_path_segments();
  deferred_receive_metadata();
  worker_install_hooks_and_budgets();
  duplicate_acks_and_malformed_paths();
  spiffs_source_loading();
  retained_worker_state_and_timers();
  durable_store_limits();
  retained_native_radio_io();
  authorized_packet_forwarding();
  filtered_packet_continuations();
  native_channel_key_configuration();
  native_hashtag_commands();
  native_admission_feedback();
  native_adaptive_admission();
  native_channel_storage();
  native_durable_timer_reply();
  autonomous_personal_reminders();
  personal_notes();
  native_utilities();
  native_board();
  native_diagnostics();
  native_events();
  event_collector_capacity();
  channel_wait_collector_and_source_fences();
  assert(identity_test::handles.empty());
  puts("bot native: persistent identity, selection, crypto packets, commands, worker, budgets and coexistence passed");
}
#endif
