// SPDX-License-Identifier: Apache-2.0
#include "CompanionSessions.h"
#include "Runtime.h"
#include "Management.h"
#include "ScopedFS.h"
#include <SPIFFS.h>
#include <Utils.h>
#include <arpa/inet.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <deque>
#include <esp_heap_caps.h>
#include <helpers/SimpleMeshTables.h>
#include <helpers/ContactInfo.h>
#include <nvs.h>
#include <sys/socket.h>
#include <target.h>
#include <thread>
#include <unistd.h>

namespace peer_pool {
#define ONCHIP_PACKET_CAPACITY 16
#include "support/PacketPool.h"
#include "support/PacketPool.inc"
#undef ONCHIP_PACKET_CAPACITY
} // namespace peer_pool

static std::atomic<unsigned long> now{1000};
unsigned long millis() { return now.load(); }
void delay(unsigned long amount) { now += amount; }
static uint32_t entropy = 123456789;
void randomSeed(long seed) { entropy = seed; }
long random(long low, long high) {
  entropy ^= entropy << 13;
  entropy ^= entropy >> 17;
  entropy ^= entropy << 5;
  return low + entropy % (high - low);
}
void esp_fill_random(void *buffer, size_t size) {
  auto bytes = static_cast<uint8_t *>(buffer);
  while (size--)
    *bytes++ = random(0, 256);
}
LifecycleTestBoard board;
SensorManager sensors;
VolatileRTCClock rtc_clock;
namespace onchip {
LocalRadio &repeaterRadio();
LocalRadio &roomRadio();
LocalRadio &companionRadio();
} // namespace onchip
using Bytes = std::vector<uint8_t>;
using onchip::LifecycleAction;
using onchip::Role;
using onchip::RolePhase;

struct TestRadio : mesh::Radio {
  bool transmitting = false, complete = true;
  uint32_t airtime = 10;
  std::deque<Bytes> incoming, sent;
  int recvRaw(uint8_t *data, int capacity) override {
    if (incoming.empty())
      return 0;
    auto bytes = incoming.front();
    incoming.pop_front();
    assert(bytes.size() <= size_t(capacity));
    memcpy(data, bytes.data(), bytes.size());
    return bytes.size();
  }
  uint32_t getEstAirtimeFor(int) override { return airtime; }
  float packetScore(float, int) override { return 1; }
  bool startSendRaw(const uint8_t *data, int size) override {
    assert(!transmitting);
    transmitting = true;
    sent.emplace_back(data, data + size);
    return true;
  }
  bool isSendComplete() override { return transmitting && complete; }
  void onSendFinished() override { transmitting = false; }
  bool isInRecvMode() const override { return !transmitting; }
  float getLastRSSI() const override { return -90; }
  float getLastSNR() const override { return 5; }
};

static const onchip::RoleCallbacks callbacks[] = {
    {onchip::repeaterBegin, onchip::repeaterFlush, onchip::repeaterStop,
     onchip::repeaterEraseStep, onchip::repeaterLoop, nullptr},
    {onchip::roomBegin, onchip::roomFlush, onchip::roomStop,
     onchip::roomEraseStep, onchip::roomLoop, nullptr},
    {onchip::companionBegin, onchip::companionFlush, onchip::companionStop,
     onchip::companionEraseStep, onchip::companionLoop,
     onchip::companionRequestFailed}};

struct Fixture {
  TestRadio radio;
  onchip::HardwareRNG rng;
  WifiKissMultiplexer mux;
  onchip::Management *management = nullptr;
  explicit Fixture(bool psramAvailable = true,
                   onchip::RoleProfile selected = {}) {
    onchip::RoleProfile profile;
    assert(onchip::loadRoleProfile(profile));
    assert(profile.enabled == selected.enabled);
    mux.attachRadio(
        radio, rng, [](float, float, uint8_t, uint8_t) {}, [](uint8_t) {});
    assert(mux.setInitialConfiguration({912525000, 250000, 7, 5, 2}, true));
    if (profile.has(Role::Companion)) {
      assert(onchip::companionSessions().begin());
      onchip::companionSessions().disable();
    }
    onchip::beginClocks(profile);
    assert(psram_test::allocations.empty());
    psram_test::failAfter = psramAvailable ? -1 : 0;
    onchip::beginLifecycles(mux, callbacks, profile);
    psram_test::failAfter = -1;
    for (unsigned i = 0; i < 3; ++i)
      assert(onchip::rolePhase(Role(i)) ==
             (!profile.has(Role(i)) ? RolePhase::Disabled
              : psramAvailable ? RolePhase::Running : RolePhase::Failed));
  }
  ~Fixture() {
    for (const auto &callback : callbacks)
      callback.stop();
    onchip::companionSessions().end();
    assert(psram_test::allocations.empty());
  }
  void step(unsigned count = 1) {
    while (count--) {
      now += 20;
      onchip::loopClocks();
      if (management) management->loop();
      onchip::loopLifecycles();
      mux.serviceTransmit();
    }
  }
};

static Bytes stored(const char *name) {
  return identity_test::durable.at({"mc-onchip", name});
}

struct PeerState {
  TestRadio radio;
  ArduinoMillis clock;
  onchip::HardwareRNG rng;
  VolatileRTCClock rtc;
  SimpleMeshTables tables;
  peer_pool::StaticPoolPacketManager packets{16};
};
struct Peer : PeerState, mesh::Mesh {
  mesh::Identity server;
  std::vector<Bytes> responses, text;
  std::vector<uint32_t> acknowledgements;
  uint32_t timestamp = 1800000000;
  Peer() : Mesh(radio, clock, rng, rtc, packets, tables) {
    self_id = mesh::LocalIdentity(&rng);
    begin();
  }
  bool allowPacketForward(const mesh::Packet *) override { return false; }
  void onAckRecv(mesh::Packet *, uint32_t ack) override {
    acknowledgements.push_back(ack);
  }
  int searchPeersByHash(const uint8_t *hash) override {
    return server.isHashMatch(hash) ? 1 : 0;
  }
  void getPeerSharedSecret(uint8_t *secret, int) override {
    self_id.calcSharedSecret(secret, server.pub_key);
  }
  void onPeerDataRecv(mesh::Packet *, uint8_t type, int, const uint8_t *,
                      uint8_t *data, size_t size) override {
    if (type == PAYLOAD_TYPE_RESPONSE)
      responses.emplace_back(data, data + size);
    if (type == PAYLOAD_TYPE_TXT_MSG)
      text.emplace_back(data, data + size);
  }
  bool onPeerPathRecv(mesh::Packet *, int, const uint8_t *, uint8_t *, uint8_t,
                      uint8_t type, uint8_t *data, uint8_t size) override {
    onPeerDataRecv(nullptr, type, 0, nullptr, data, size);
    return false;
  }
  void target(Role role) {
    const auto bytes = stored(onchip::roleName(role));
    mesh::LocalIdentity identity;
    identity.readFrom(bytes.data() + (bytes.size() == 64 ? 0 : 5), 64);
    server = mesh::Identity(identity.pub_key);
  }
  void pump(Fixture &f, unsigned count = 100) {
    while (count--) {
      f.step();
      while (!f.radio.sent.empty()) {
        radio.incoming.push_back(f.radio.sent.front());
        f.radio.sent.pop_front();
      }
      rtc.tick();
      loop();
    }
  }
  void send(Fixture &f, const Bytes &data, bool anonymous) {
    uint8_t secret[32], wire[255];
    self_id.calcSharedSecret(secret, server.pub_key);
    auto packet =
        anonymous ? createAnonDatagram(PAYLOAD_TYPE_ANON_REQ, self_id, server,
                                       secret, data.data(), data.size())
                  : createDatagram(PAYLOAD_TYPE_TXT_MSG, server, secret,
                                   data.data(), data.size());
    assert(packet);
    packet->header = (packet->header & ~PH_ROUTE_MASK) | ROUTE_TYPE_DIRECT;
    packet->path_len = 0;
    const auto size = packet->writeTo(wire);
    packets.free(packet);
    f.mux.received(wire, size, -90, 5);
    pump(f);
  }
  bool login(Fixture &f, Role role, const char *password, int permission) {
    responses.clear();
    Bytes request(role == Role::Room ? 8 : 4, 0);
    queued_tx::put32(request.data(), ++timestamp);
    request.insert(request.end(), password, password + strlen(password) + 1);
    send(f, request, true);
    if (permission < 0)
      return responses.empty();
    if (responses.size() != 1)
      return false;
    const auto &reply = responses.back();
    // Native 13-byte login body is AES-padded; permission/status offsets are
    // wire contracts.
    return reply.size() >= 13 && reply[4] == 0 && reply[5] == 0 &&
           reply[6] == (permission == 3 ? 1 : 0) && reply[7] == permission;
  }
  std::string command(Fixture &f, const std::string &command) {
    text.clear();
    Bytes data(5);
    queued_tx::put32(data.data(), ++timestamp);
    data[4] = 4; // TXT_TYPE_CLI_DATA, attempt zero.
    data.insert(data.end(), command.begin(), command.end());
    send(f, data, false);
    if (text.empty())
      return {};
    const auto &reply = text.back();
    assert(reply.size() > 5 && reply[4] == 4);
    const auto first = reply.begin() + 5;
    return {first, std::find(first, reply.end(), 0)};
  }
};

static void authenticated_wire_lifecycle() {
  Fixture f;
  for (Role role : {Role::Repeater, Role::Room}) {
    Peer peer;
    peer.target(role);
    const auto original = stored(onchip::roleName(role));
    mesh::LocalIdentity replacement(&f.rng);
    uint8_t privateKey[64];
    char hex[129], publicHex[65];
    replacement.writeTo(privateKey, sizeof(privateKey));
    mesh::Utils::toHex(hex, privateKey, sizeof(privateKey));
    mesh::Utils::toHex(publicHex, replacement.pub_key, 32);
    const std::string stage = std::string("set prv.key ") + hex;
    assert(peer.command(f, stage).empty());
    assert(stored(onchip::roleName(role)) == original);
    assert(peer.login(f, role, "wrong-password", -1));
    if (role == Role::Room) {
      assert(peer.login(f, role, "test-room", 2));
      assert(peer.command(f, "reboot").empty());
      assert(peer.command(f, stage).empty());
      assert(stored("room") == original);
    }
    assert(peer.login(f, role, "test-admin", 3));
    assert(peer.command(f, "  ab|set radio 900,125,12,8") ==
           "ab|Error: unsupported on shared device");
    assert(f.mux.currentConfiguration().freq_hz == 912525000);
    assert(peer.command(f, "  ab|password ") ==
           "ab|Error: administrator password must not be empty");
    assert(peer.command(f, "erase").find("OK") == std::string::npos);
    assert(peer.command(f, "get prv.key").find("> ") == std::string::npos);
    assert(peer.command(f, "set prv.key deadbeef") == "Error, bad key");
    assert(stored(onchip::roleName(role)) == original);
    identity_test::failCommit = true;
    assert(peer.command(f, "  ab|" + stage) ==
           "ab|Error: identity staging failed");
    assert(stored(onchip::roleName(role)) == original);
    identity_test::failCommit = false;
    assert(peer.command(f, "  ab|" + stage) ==
           std::string("ab|OK, reboot to apply! New pubkey: ") + publicHex);
    const auto staged = stored(onchip::roleName(role));
    assert(staged.size() == 133 &&
           !memcmp(staged.data() + 5, original.data(), 64));
    assert(peer.command(f, "set name persisted-role").find("OK") !=
           std::string::npos);
    assert(peer.command(f, "get name") == "> persisted-role");
    auto &radio =
        role == Role::Repeater ? onchip::repeaterRadio() : onchip::roomRadio();
    auto &sibling =
        role == Role::Repeater ? onchip::roomRadio() : onchip::repeaterRadio();
    mesh::QueuedRadioStats before, after, other;
    assert(radio.getQueuedRadioStats(before) &&
           sibling.getQueuedRadioStats(other));
    peer.command(f, "  ab|reboot");
    assert(onchip::rolePhase(role) == RolePhase::Running);
    assert(radio.getQueuedRadioStats(after) &&
           before.generation != after.generation);
    assert(sibling.getQueuedRadioStats(after) &&
           after.generation == other.generation);
    assert(stored(onchip::roleName(role)) ==
           Bytes(privateKey, privateKey + 64));
    peer.target(role);
    // The durable ACL is loaded and recalculates its secret using the new
    // identity.
    assert(peer.command(f, "get name") == "> persisted-role");
    assert(peer.login(f, role, "", 3));
  }
}

template <class Condition> static void until(Fixture &f, Condition condition) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (!condition()) {
    assert(std::chrono::steady_clock::now() < end);
    f.step();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}
class Client {
  int fd;
  void read(Fixture &f, uint8_t *data, size_t size) {
    size_t offset = 0;
    until(f, [&] {
      const int n = recv(fd, data + offset, size - offset, MSG_DONTWAIT);
      if (n > 0)
        offset += n;
      else
        assert(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
      return offset == size;
    });
  }

public:
  std::vector<Bytes> pushes;
  Client() : fd(socket(AF_INET, SOCK_STREAM, 0)) {
    assert(fd >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(onchip::companionSessions().port());
    assert(connect(fd, reinterpret_cast<sockaddr *>(&address),
                   sizeof(address)) == 0);
  }
  ~Client() { close(fd); }
  void command(const Bytes &data) {
    Bytes wire{'<', uint8_t(data.size()), uint8_t(data.size() >> 8)};
    wire.insert(wire.end(), data.begin(), data.end());
    assert(send(fd, wire.data(), wire.size(), MSG_NOSIGNAL) ==
           ssize_t(wire.size()));
  }
  Bytes response(Fixture &f) {
    for (unsigned i = 0; i < 128; ++i) {
      uint8_t header[3];
      read(f, header, 3);
      assert(header[0] == '>');
      Bytes bytes(header[1] | (unsigned(header[2]) << 8));
      assert(!bytes.empty() && bytes.size() <= MAX_FRAME_SIZE);
      read(f, bytes.data(), bytes.size());
      if (bytes[0] < 0x80)
        return bytes;
      pushes.push_back(bytes);
    }
    assert(false && "Unbounded unsolicited companion output");
    return {};
  }
  void handshake(Fixture &f) {
    command({22, 3});
    assert(response(f)[0] == 13);
    command({1, 0, 0, 0, 0, 0, 0, 0});
    assert(response(f)[0] == 5);
  }
  bool closed() {
    uint8_t buffer[256];
    const int n = recv(fd, buffer, sizeof(buffer), MSG_DONTWAIT);
    if (n > 0)
      return false;
    return n == 0 || errno == ECONNRESET;
  }
};

static void native_room_clock_replay(bool deliberateFuture, bool syncNative = true) {
  rtc_clock.setCurrentTime(3078062800);
  Fixture f;
  Client client;
  client.handshake(f);
  client.command({5});
  auto initialTime = client.response(f);
  assert(initialTime[0] == 9 &&
         queued_tx::get32(initialTime.data() + 1) < 1790265409u);
  Peer recipient;
  recipient.target(Role::Room);
  assert(recipient.login(f, Role::Room, "test-room", 2));
  Bytes contact(148, 0);
  contact[0] = 9;
  memcpy(contact.data() + 1, recipient.server.pub_key, 32);
  contact[33] = 3; // Native room advert type, direct zero-hop path.
  memcpy(contact.data() + 100, "local-room", 10);
  queued_tx::put32(contact.data() + 144, 3078062905);
  client.command(contact);
  assert(client.response(f) == Bytes{0});
  const auto identities = identity_test::durable;
  client.command({19, 'r', 'e', 'b', 'o', 'o', 't'});
  until(f, [&] {
    return client.closed() &&
           onchip::rolePhase(Role::Companion) == RolePhase::Running;
  });
  Client replacement;
  replacement.handshake(f);
  replacement.command({5});
  initialTime = replacement.response(f);
  assert(initialTime[0] == 9 &&
         queued_tx::get32(initialTime.data() + 1) < 1790265409u);
  assert(identity_test::durable == identities);
  Bytes query{30};
  query.insert(query.end(), recipient.server.pub_key, recipient.server.pub_key + 32);
  replacement.command(query);
  const auto retained = replacement.response(f);
  assert(retained[0] == 3 && retained.size() >= 148);
  assert(queued_tx::get32(retained.data() + 144) == 3078062905u);
  Bytes setTime(5);
  setTime[0] = 6;
  queued_tx::put32(setTime.data() + 1, deliberateFuture ? 3078062800u : 1790265400u);
  if (syncNative) {
    replacement.command(setTime);
    assert(replacement.response(f) == Bytes{0});
  }
  Bytes login{26};
  login.insert(login.end(), recipient.server.pub_key,
               recipient.server.pub_key + 32);
  const char password[] = "test-room";
  login.insert(login.end(), password, password + sizeof(password));
  replacement.command(login);
  assert(replacement.response(f)[0] == 6);
  recipient.pump(f, 500);
  replacement.command({5});
  const auto clock = replacement.response(f);
  assert(clock[0] == 9);
  assert(std::any_of(replacement.pushes.begin(), replacement.pushes.end(),
                     [](const Bytes &p) { return p[0] == 0x85; }));
  const auto post = [&](uint32_t timestamp, const char *message) {
    recipient.text.clear();
    recipient.acknowledgements.clear();
    Bytes command(13);
    command[0] = 2;
    queued_tx::put32(command.data() + 3, timestamp);
    memcpy(command.data() + 7, recipient.server.pub_key, 6);
    command.insert(command.end(), message, message + strlen(message));
    replacement.command(command);
    const auto sent = replacement.response(f);
    assert(sent.size() == 10 && sent[0] == 6);
    const uint32_t ack = queued_tx::get32(sent.data() + 2);
    recipient.pump(f, 1000);
    return std::find(recipient.acknowledgements.begin(),
                     recipient.acknowledgements.end(), ack) !=
           recipient.acknowledgements.end();
  };
  assert(post(1790265409, "wall-clock-post") == !deliberateFuture);
  if (deliberateFuture) {
    assert(recipient.text.empty());
    assert(post(queued_tx::get32(clock.data() + 1) + 60, "newer-clock"));
  }
  assert(std::any_of(recipient.text.begin(), recipient.text.end(),
                     [deliberateFuture](const Bytes &p) {
                       const std::string text(p.begin(), p.end());
                       return p.size() > 9 && (p[4] >> 2) == 2 &&
                           text.find(deliberateFuture ? "newer-clock" : "wall-clock-post") !=
                               std::string::npos;
                     }));
  // A fresh payload below the accepted member timestamp must still be rejected.
  assert(!post(1790265300, "replayed-post"));
  assert(std::none_of(recipient.text.begin(), recipient.text.end(),
                     [](const Bytes &p) {
                       return std::string(p.begin(), p.end()).find("replayed-post") !=
                           std::string::npos;
                     }));
  puts(deliberateFuture
       ? "PASS deliberate native future-time replay rejection remains intact"
       : "PASS corrupt ESP RTC and persisted future contact do not poison native "
         "login; wall-clock post ACK and signed delivery succeed");
}

static void clock_authority() {
  const auto savedMillis = now.load();
  now = 0xfffffff0u;
  onchip::RoleClock clock(1767225600);
  now = 0x000003d8u;
  assert(clock.getCurrentTime() == 1767225601u);
  const auto unique = clock.getCurrentTimeUnique();
  clock.synchronize(1767225500);
  assert(clock.getCurrentTime() == 1767225601u &&
         clock.getCurrentTimeUnique() > unique);
  now = savedMillis;
  onchip::beginClocks();
  onchip::ClockSnapshot snapshot;
  assert(onchip::clockSnapshot(snapshot));
  assert(snapshot.build_epoch == 1767225600u);
  for (const auto &role : snapshot.roles)
    assert(role.source == onchip::ClockSource::Build && !role.synchronized);
  onchip::receiveNetworkTime(3078062800u);
  onchip::receiveNetworkTime(1790265409u); // bounded mailbox: newest wins
  assert(onchip::roomClock().getCurrentTime() == 1767225600u);
  onchip::loopClocks();
  assert(onchip::clockSnapshot(snapshot));
  for (const auto &role : snapshot.roles)
    assert(role.epoch == 1790265409u &&
           role.source == onchip::ClockSource::Network && role.synchronized);
  const auto previous = onchip::companionClock().getCurrentTimeUnique();
  onchip::receiveNetworkTime(1790260000u);
  onchip::loopClocks();
  assert(onchip::companionClock().getCurrentTime() == 1790265409u &&
         onchip::companionClock().getCurrentTimeUnique() > previous);
  assert(onchip::clockSnapshot(snapshot) && !snapshot.roles[2].synchronized);
  onchip::receiveNetworkTime(1715770351u);
  onchip::loopClocks();
  assert(onchip::clockSnapshot(snapshot) && snapshot.rejected_samples == 1);
  char json[768], shortBuffer[8];
  assert(onchip::formatClockJSON(snapshot, json, sizeof(json)) > 0);
  assert(strstr(json, "\"last_sntp_epoch\":1790260000"));
  assert(strstr(json, "\"source\":\"sntp\",\"synchronized\":false"));
  assert(!onchip::formatClockJSON(snapshot, shortBuffer, sizeof(shortBuffer)));
  onchip::receiveNetworkTime(1790265410u);
  onchip::loopClocks();
  now += 7201000;
  onchip::loopClocks();
  assert(onchip::clockSnapshot(snapshot) &&
         snapshot.network_age_seconds == 7201 && !snapshot.roles[0].synchronized);
  now = savedMillis;
}

static void companion_channel_configuration() {
  const auto files = filesystem_test::files;
  {
    Fixture f; Client client; client.handshake(f);
    uint8_t key[16], fingerprint[8];
    char name[32];
    for (unsigned i = 0; i < sizeof(key); ++i) key[i] = uint8_t(0x71 + i);
    assert(onchip::companionSetChannel(7, "private-fixture", key));
    assert(onchip::companionChannelInfo(7, name, fingerprint) && !strcmp(name, "private-fixture"));
    Peer peer;
    const auto receive = [&](const uint8_t *secret) {
      mesh::GroupChannel channel{};
      memcpy(channel.secret, secret, 16);
      mesh::Utils::sha256(channel.hash, sizeof(channel.hash), secret, 16);
      Bytes data(5);
      queued_tx::put32(data.data(), ++peer.timestamp);
      const char text[] = "fixture: private message";
      data.insert(data.end(), text, text + sizeof(text));
      auto packet = peer.createGroupDatagram(PAYLOAD_TYPE_GRP_TXT, channel, data.data(), data.size());
      assert(packet);
      packet->header |= ROUTE_TYPE_FLOOD; packet->path_len = 0;
      uint8_t wire[255]; const auto size = packet->writeTo(wire);
      peer.packets.free(packet);
      f.mux.received(wire, size, -91, 6); f.step(100);
      client.command({10});
      return client.response(f);
    };
    const auto message = receive(key);
    assert(message[0] == 8 || message[0] == 17);
    uint8_t wrong[16]; memcpy(wrong, key, sizeof(wrong)); wrong[0] ^= 0x80;
    assert(receive(wrong) == Bytes{10});
    client.command({3, 0, 7, 0, 0, 0, 0, 'h', 'i'});
    assert(client.response(f) == Bytes{0});
    filesystem_test::failOpen = true;
    assert(!onchip::companionSetChannel(7, "", wrong));
    Bytes command(50, 0);
    command[0] = 32; command[1] = 7;
    memcpy(command.data() + 2, "private-fixture", 15);
    memcpy(command.data() + 34, key, 16);
    client.command(command);
    assert(client.response(f)[0] == 1);
    filesystem_test::failOpen = false;
    assert(onchip::companionChannelInfo(7, name, fingerprint) && !strcmp(name, "private-fixture"));
    uint8_t empty[16]{};
    assert(onchip::companionSetChannel(7, "", empty));
    assert(receive(key) == Bytes{10} && receive(empty) == Bytes{10});
    client.command({3, 0, 7, 0, 0, 0, 0, 'h', 'i'});
    assert(client.response(f)[0] == 1);
    client.command({62, 7, 0xff, 1, 0, 'x'});
    assert(client.response(f)[0] == 1);
    assert(onchip::requestLifecycle(Role::Companion, LifecycleAction::Reboot));
    until(f, [&] { return client.closed() && onchip::rolePhase(Role::Companion) == RolePhase::Running; });
    assert(onchip::companionChannelInfo(7, name, fingerprint) && !name[0]);
  }
  filesystem_test::files = files;
  puts("PASS native companion channels: configured PSK RX/TX, wrong-key rejection, checked save failure, disabled text/data TX and zero/old-key RX, persisted off after restart");
}

static void companion_wire_lifecycle() {
  Fixture f;
  Client first, second;
  first.handshake(f);
  second.handshake(f);
  assert(onchip::companionSessions().stats().connectedClients == 2);
  const auto identity = stored("companion"), repeater = stored("repeater"),
             room = stored("room");
  const auto profile = identity_test::durable.at({"mesh-phy", "profile"});
  mesh::QueuedRadioStats before, after, sibling;
  assert(onchip::companionRadio().getQueuedRadioStats(before));
  assert(onchip::roomRadio().getQueuedRadioStats(sibling));
  first.command({19, 'r', 'e', 'b', 'o', 'o'});
  assert(first.response(f)[0] == 1);
  assert(onchip::companionRadio().getQueuedRadioStats(after) &&
         after.generation == before.generation);
  filesystem_test::failOpen = true;
  first.command({19, 'r', 'e', 'b', 'o', 'o', 't'});
  assert(first.response(f)[0] == 1);
  assert(onchip::companionRadio().getQueuedRadioStats(after) &&
         after.generation == before.generation);
  filesystem_test::failOpen = false;
  first.command({8, 'r', 'e', 't', 'a', 'i', 'n'});
  assert(first.response(f) == Bytes{0});
  Bytes channel(50, 0);
  channel[0] = 32;
  channel[1] = 1;
  memcpy(channel.data() + 2, "retained-channel", 16);
  std::fill(channel.begin() + 34, channel.end(), 0x42);
  first.command(channel);
  assert(first.response(f) == Bytes{0});
  mesh::LocalIdentity contactIdentity(&f.rng);
  Bytes contact(148, 0);
  contact[0] = 9;
  memcpy(contact.data() + 1, contactIdentity.pub_key, 32);
  contact[33] = 1;
  contact[35] = 0xff;
  memcpy(contact.data() + 100, "retained-contact", 16);
  queued_tx::put32(contact.data() + 144, 1700000000);
  first.command(contact);
  assert(first.response(f) == Bytes{0});
  f.radio.complete = false;
  f.radio.airtime = 60000;
  first.command({7, 1});
  assert(first.response(f) == Bytes{0});
  until(f, [&] { return f.mux.isActuallyTransmitting(); });
  assert(onchip::companionRadio().hasPendingWork());
  first.command({19, 'r', 'e', 'b', 'o', 'o', 't'});
  until(f, [&] {
    return first.closed() && second.closed() &&
           onchip::rolePhase(Role::Companion) == RolePhase::Running;
  });
  assert(onchip::companionRadio().getQueuedRadioStats(after) &&
         after.generation != before.generation);
  assert(f.mux.isActuallyTransmitting() &&
         !onchip::companionRadio().hasPendingWork());
  assert(onchip::roomRadio().getQueuedRadioStats(before) &&
         before.generation == sibling.generation);
  f.radio.complete = true;
  f.step(2);
  f.radio.airtime = 10;
  assert(onchip::companionRadio().getQueuedRadioStats(after) &&
         after.source_rf_ms == 0);
  assert(after.aggregate_rf_ms > 0 &&
         onchip::companionPackets().getFreeCount() == 16);
  mesh::QueuedTransmitResult stale;
  assert(!onchip::companionRadio().pollQueuedResult(stale));
  assert(stored("companion") == identity);
  Client replacement, observer;
  replacement.handshake(f);
  observer.handshake(f);
  replacement.command({1, 0, 0, 0, 0, 0, 0, 0});
  const auto self = replacement.response(f);
  const Bytes name{'r', 'e', 't', 'a', 'i', 'n'};
  assert(std::search(self.begin(), self.end(), name.begin(), name.end()) !=
         self.end());
  replacement.command({31, 1});
  const auto channelReply = replacement.response(f);
  assert(channelReply.size() == 50 && channelReply[0] == 18 &&
         channelReply[1] == 1);
  assert(!memcmp(channelReply.data() + 2, "retained-channel", 16));
  assert(std::equal(channelReply.begin() + 34, channelReply.end(),
                    channel.begin() + 34));
  Bytes contactQuery{30};
  contactQuery.insert(contactQuery.end(), contactIdentity.pub_key,
                      contactIdentity.pub_key + 32);
  replacement.command(contactQuery);
  const auto contactReply = replacement.response(f);
  assert(contactReply.size() >= 132 && contactReply[0] == 3);
  assert(std::equal(contactReply.begin() + 1, contactReply.begin() + 132,
                    contact.begin() + 1));
  filesystem_test::files["/companion/owned"] = {1, 2, 3};
  filesystem_test::files["/companionish/keep"] = {4, 5, 6};
  const auto files = filesystem_test::files;
  filesystem_test::failRemove = true;
  replacement.command({51, 'r', 'e', 's', 'e', 't'});
  until(f, [&] {
    return onchip::rolePhase(Role::Companion) == RolePhase::Failed;
  });
  assert(stored("companion").size() == 133 && stored("companion")[4] == 3);
  assert(onchip::rolePhase(Role::Room) == RolePhase::Running &&
         onchip::rolePhase(Role::Repeater) == RolePhase::Running);
  until(f, [&] { return replacement.closed() && observer.closed(); });
  filesystem_test::failRemove = false;
  // Simulate a power loss: only the durable identity marker drives recovery.
  for (const auto &callback : callbacks)
    callback.stop();
  onchip::beginLifecycles(f.mux, callbacks, {});
  for (unsigned i = 0;
       i < 100 && onchip::rolePhase(Role::Companion) != RolePhase::Running; ++i)
    f.step();
  assert(onchip::rolePhase(Role::Companion) == RolePhase::Running);
  assert(stored("companion").size() == 64 && stored("companion") != identity);
  assert(stored("repeater") == repeater && stored("room") == room);
  assert(identity_test::durable.at({"mesh-phy", "profile"}) == profile);
  for (const auto &entry : files) {
    if (entry.first.compare(0, 11, "/companion/") != 0)
      assert(filesystem_test::files.at(entry.first) == entry.second);
  }
  assert(!filesystem_test::files.count("/companion/owned"));
  Client fresh;
  fresh.handshake(f);
  fresh.command(contactQuery);
  assert(fresh.response(f)[0] == 1);
  fresh.command({31, 1});
  const auto resetChannel = fresh.response(f);
  assert(resetChannel.size() == 50 && resetChannel[0] == 18 &&
         resetChannel[1] == 1 && resetChannel[2] == 0);
  assert(std::all_of(resetChannel.begin() + 34, resetChannel.end(),
                     [](uint8_t byte) { return byte == 0; }));
}

static void companion_contact_capacity() {
  static_assert(MAX_CONTACTS == 256, "Exercise the combined production contact capacity");
  const auto files = filesystem_test::files;
  std::vector<Bytes> contacts;
  Bytes legacy;
  onchip::HardwareRNG rng;
  for (unsigned index = 0; index <= MAX_CONTACTS; ++index) {
    mesh::LocalIdentity identity(&rng);
    Bytes frame(148, 0);
    frame[0] = 9;
    memcpy(frame.data() + 1, identity.pub_key, 32);
    frame[33] = 1;
    frame[34] = index & 1;
    frame[35] = 0x82; // Two three-byte path hashes.
    for (unsigned j = 0; j < 6; ++j) frame[36 + j] = index + j;
    snprintf(reinterpret_cast<char *>(frame.data() + 100), 32, "retained-%03u", index);
    queued_tx::put32(frame.data() + 132, 1700000000u + index);
    queued_tx::put32(frame.data() + 136, 53546000u + index);
    queued_tx::put32(frame.data() + 140, uint32_t(-113493800));
    queued_tx::put32(frame.data() + 144, 1700001000u + index);
    contacts.push_back(frame);
    if (index < 64) {
      // Existing contacts3 records are 152 bytes, independent of table capacity.
      legacy.insert(legacy.end(), frame.begin() + 1, frame.begin() + 33);
      legacy.insert(legacy.end(), frame.begin() + 100, frame.begin() + 132);
      legacy.insert(legacy.end(), {frame[33], frame[34], 0, 1, 0, 0, 0, frame[35]});
      legacy.insert(legacy.end(), frame.begin() + 132, frame.begin() + 136);
      legacy.insert(legacy.end(), frame.begin() + 36, frame.begin() + 100);
      legacy.insert(legacy.end(), frame.begin() + 144, frame.end());
      legacy.insert(legacy.end(), frame.begin() + 136, frame.begin() + 144);
    }
  }
  assert(legacy.size() == 64 * 152);
  filesystem_test::files["/companion/contacts3"] = legacy;
  {
    Fixture f;
    const auto identities = identity_test::durable;
    const auto psramBytes = [&]() {
      const auto packet = reinterpret_cast<uintptr_t>(&onchip::companionPackets());
      for (const auto &allocation : psram_test::allocations) {
        const auto start = reinterpret_cast<uintptr_t>(allocation.first);
        if (packet >= start && packet < start + allocation.second) {
          assert(allocation.second > (MAX_CONTACTS + 8) * (sizeof(ContactInfo) + sizeof(int)));
          return allocation.second;
        }
      }
      assert(false && "Companion object and embedded contact arrays must be in PSRAM");
      return size_t(0);
    };
    const auto allocation = psramBytes();
    Client client;
    client.handshake(f);
    client.command({22, 3});
    const auto info = client.response(f);
    assert(info.size() >= 4 && info[0] == 13 && info[2] == 128 && info[3] == MAX_GROUP_CHANNELS);
    const auto query = [&](Client &connection, unsigned index) {
      Bytes request{30};
      request.insert(request.end(), contacts[index].begin() + 1, contacts[index].begin() + 33);
      connection.command(request);
      const auto reply = connection.response(f);
      assert(reply.size() == 148 && reply[0] == 3 &&
             std::equal(reply.begin() + 1, reply.end(), contacts[index].begin() + 1));
    };
    for (unsigned i = 0; i < 64; ++i) query(client, i);
    assert(onchip::companionFlush() && filesystem_test::files.at("/companion/contacts3") == legacy);
    for (unsigned i = 64; i < MAX_CONTACTS; ++i) {
      client.command(contacts[i]);
      assert(client.response(f) == Bytes{0});
    }
    query(client, 64); query(client, 255);
    client.command(contacts[MAX_CONTACTS]);
    assert(client.response(f) == (Bytes{1, 3})); // Native table-full error; no eviction.
    memcpy(contacts[255].data() + 100, "updated-contact", 16);
    client.command(contacts[255]);
    assert(client.response(f) == Bytes{0});
    assert(onchip::companionFlush());
    const auto saved = filesystem_test::files.at("/companion/contacts3");
    assert(saved.size() == MAX_CONTACTS * 152 &&
           std::equal(legacy.begin(), legacy.end(), saved.begin()));
    client.command({19, 'r', 'e', 'b', 'o', 'o', 't'});
    until(f, [&] { return client.closed() && onchip::rolePhase(Role::Companion) == RolePhase::Running; });
    assert(psramBytes() == allocation && identity_test::durable == identities);
    Client restarted;
    restarted.handshake(f);
    restarted.command({4});
    const auto start = restarted.response(f);
    assert(start.size() == 5 && start[0] == 2 && queued_tx::get32(start.data() + 1) == MAX_CONTACTS);
    for (unsigned i = 0; i < MAX_CONTACTS; ++i) {
      const auto reply = restarted.response(f);
      assert(reply.size() == 148 && reply[0] == 3 &&
             std::equal(reply.begin() + 1, reply.end(), contacts[i].begin() + 1));
    }
    assert(restarted.response(f)[0] == 4);
    restarted.command(contacts[MAX_CONTACTS]);
    assert(restarted.response(f) == (Bytes{1, 3}));
    const auto attempts = psram_test::attempts;
    psram_test::failAfter = 0;
    assert(onchip::requestLifecycle(Role::Companion, LifecycleAction::Reboot));
    f.step(2);
    assert(onchip::rolePhase(Role::Companion) == RolePhase::Failed &&
           psram_test::attempts == attempts + 1 && psram_test::allocations.size() == 2);
    assert(filesystem_test::files.at("/companion/contacts3") == saved);
    psram_test::failAfter = -1;
    f.step(260);
    assert(onchip::rolePhase(Role::Companion) == RolePhase::Running && psramBytes() == allocation);
    until(f, [&] { return restarted.closed(); });
    Client recovered;
    recovered.handshake(f);
    for (unsigned i = 0; i < MAX_CONTACTS; ++i) query(recovered, i);
    assert(onchip::companionFlush() && filesystem_test::files.at("/companion/contacts3") == saved);
    assert(identity_test::durable == identities);
    printf("PASS companion 256 contacts: info=128, legacy64 retained, >64 add/update, full error3/no eviction, "
           "SPIFFS/reboot/PSRAM-failure recovery; ContactInfo=%zu index=%zu delta=%zu PSRAM bytes; storage=%zu\n",
           sizeof(ContactInfo), sizeof(int), 192 * (sizeof(ContactInfo) + sizeof(int)), allocation);
  }
  filesystem_test::files = files;
}

static void bounded_erasure() {
  for (unsigned i = 0; i < 20; ++i) {
    filesystem_test::files["/other/" + std::to_string(i)] = {1};
    filesystem_test::files["/room/" + std::to_string(i)] = {2};
  }
  filesystem_test::files["/roommate/keep"] = {3};
  onchip::ScopedErase eraser(SPIFFS, "/room");
  bool done = false;
  for (unsigned passes = 0; !done; ++passes) {
    assert(passes < 1000);
    const auto count = filesystem_test::files.size();
    const unsigned reads = filesystem_test::directoryReads;
    assert(eraser.step(done));
    assert(filesystem_test::directoryReads - reads <= 8);
    assert(count - filesystem_test::files.size() <= 1);
  }
  assert(filesystem_test::files.size() == 21);
  assert(filesystem_test::files.at("/roommate/keep") == Bytes{3});
  filesystem_test::files.clear();
}

static void identity_failures() {
  onchip::HardwareRNG rng;
  mesh::LocalIdentity initial, candidate(&rng), output(&rng);
  assert(onchip::loadIdentity("repeater", initial));
  const auto legacy = stored("repeater");
  assert(legacy.size() == 64);
  assert(onchip::stageIdentity(Role::Repeater, candidate));
  const auto staged = stored("repeater");
  assert(staged.size() == 133 && staged[0] == 'M' && staged[1] == 'C' &&
         staged[2] == 'I' && staged[3] == 1 && staged[4] == 1);
  assert(!memcmp(staged.data() + 5, legacy.data(), 64));
  identity_test::durable[{"mc-onchip", "repeater"}][4] = 0;
  assert(!onchip::loadIdentity("repeater", output));
  identity_test::durable[{"mc-onchip", "repeater"}] = staged;
  const auto unchanged = output;
  identity_test::failCommit = true;
  assert(!onchip::loadIdentity("repeater", output));
  assert(!memcmp(output.pub_key, unchanged.pub_key, 32) &&
         stored("repeater") == staged);
  identity_test::failCommit = false;
  assert(onchip::loadIdentity("repeater", output));
  assert(!memcmp(output.pub_key, candidate.pub_key, 32) &&
         stored("repeater").size() == 64);
  identity_test::failWrite = true;
  assert(!onchip::stageIdentity(Role::Repeater, initial));
  assert(stored("repeater") == Bytes(staged.begin() + 69, staged.end()));
  identity_test::failWrite = false;
  auto corrupt = stored("repeater");
  identity_test::durable[{"mc-onchip", "repeater"}] = {1, 2, 3};
  assert(!onchip::loadIdentity("repeater", output));
  identity_test::durable[{"mc-onchip", "repeater"}] = corrupt;
  assert(onchip::prepareIdentityReset(Role::Repeater));
  const auto reset = stored("repeater");
  assert(reset[4] == 3);
  assert(onchip::prepareIdentityReset(Role::Repeater) &&
         stored("repeater") == reset);
  assert(!onchip::loadIdentity("repeater", output));
  assert(onchip::finishIdentityReset(Role::Repeater));
  assert(stored("repeater") == Bytes(reset.begin() + 69, reset.end()));
  identity_test::durable.clear();
  assert(identity_test::handles.empty());
}

static void identity_imports() {
  using onchip::IdentityChange;
  const auto baseline = identity_test::durable;
  onchip::HardwareRNG rng;
  mesh::LocalIdentity initial, replacement(&rng), other(&rng), loaded;
  uint8_t raw[PRV_KEY_SIZE], different[PRV_KEY_SIZE], publicKey[32]{};
  replacement.writeTo(raw, sizeof(raw)); other.writeTo(different, sizeof(different));
  assert(onchip::loadIdentity("command-bot", initial));
  const auto original = identity_test::durable;
  const auto originalEntries = identity_test::usedEntries();
  uint8_t invalid[PRV_KEY_SIZE]{};
  assert(onchip::importIdentity("command-bot", invalid, publicKey) == IdentityChange::Rejected);
  assert(identity_test::durable == original);
  assert(onchip::importIdentity("command-bot", raw, publicKey) == IdentityChange::Staged);
  assert(identity_test::usedEntries() == originalEntries + 3);
  assert(!memcmp(publicKey, replacement.pub_key, 32));
  assert(onchip::identityPublicKey("command-bot", publicKey) &&
         !memcmp(publicKey, initial.pub_key, 32));
  const auto staged = identity_test::durable;
  const auto commits = identity_test::commits;
  assert(onchip::importIdentity("command-bot", raw, publicKey) == IdentityChange::Staged);
  assert(identity_test::commits == commits);
  assert(onchip::importIdentity("command-bot", different, publicKey) == IdentityChange::Conflict);
  assert(identity_test::durable == staged);
  assert(onchip::cancelIdentityChange("command-bot", publicKey) == IdentityChange::Active);
  assert(!memcmp(publicKey, initial.pub_key, 32) && identity_test::durable == original);
  assert(onchip::cancelIdentityChange("command-bot", publicKey) == IdentityChange::Active);
  for (unsigned cut = 0; cut < 4; ++cut) {
    identity_test::durable = original;
    identity_test::failWrite = cut == 0;
    identity_test::failCommit = cut == 1 || cut == 2;
    identity_test::eagerWrites = cut == 2;
    identity_test::afterCommit = cut == 3 ? +[] { identity_test::failRead = true; } : nullptr;
    assert(onchip::importIdentity("command-bot", raw, publicKey) == IdentityChange::Unknown);
    identity_test::failWrite = identity_test::failCommit = identity_test::failRead = false;
    identity_test::eagerWrites = false; identity_test::afterCommit = nullptr;
    assert(onchip::identityPublicKey("command-bot", publicKey) &&
           !memcmp(publicKey, initial.pub_key, 32));
    assert(onchip::loadIdentity("command-bot", loaded));
    assert(!memcmp(loaded.pub_key, cut >= 2 ? replacement.pub_key : initial.pub_key, 32));
  }
  identity_test::durable = staged;
  identity_test::afterCommit = +[] { identity_test::failRead = true; };
  assert(onchip::cancelIdentityChange("command-bot", publicKey) == IdentityChange::Unknown);
  identity_test::afterCommit = nullptr; identity_test::failRead = false;
  assert(onchip::cancelIdentityChange("command-bot", publicKey) == IdentityChange::Active);
  assert(identity_test::durable == original);
  assert(onchip::importIdentity("command-bot", raw, publicKey) == IdentityChange::Staged);
  assert(onchip::loadIdentity("command-bot", loaded));
  const auto activated = identity_test::durable;
  assert(onchip::importIdentity("command-bot", raw, publicKey) == IdentityChange::Active);
  assert(identity_test::durable == activated && !memcmp(publicKey, replacement.pub_key, 32));
  assert(onchip::loadIdentity("room", initial) && onchip::prepareIdentityReset(Role::Room));
  const auto resetting = identity_test::durable;
  assert(onchip::importIdentity("room", raw, publicKey) == IdentityChange::Conflict);
  assert(onchip::cancelIdentityChange("room", publicKey) == IdentityChange::Conflict);
  assert(identity_test::durable == resetting);

  identity_test::durable.clear();
  const char *targets[] = {"repeater", "room", "companion", "management", "command-bot"};
  const char *identities[] = {"modem", "repeater", "room", "companion", "observer", "management", "command-bot"};
  for (const char *name : identities) assert(onchip::loadIdentity(name, loaded));
  const auto distinct = identity_test::durable;
  for (const char *target : targets) for (const char *otherRole : identities) {
    if (!strcmp(target, otherRole)) continue;
    for (bool pending : {false, true}) {
      identity_test::durable = distinct;
      Bytes candidate = distinct.at({"mc-onchip", otherRole});
      if (pending) {
        Bytes journal{'M', 'C', 'I', 1, 1};
        journal.insert(journal.end(), candidate.begin(), candidate.end());
        journal.insert(journal.end(), raw, raw + sizeof(raw));
        identity_test::durable[{"mc-onchip", otherRole}] = journal;
        candidate.assign(raw, raw + sizeof(raw));
      }
      const auto before = identity_test::durable;
      const auto commitsBefore = identity_test::commits;
      for (bool alternatePrivate : {false, true}) {
        // The signing nonce half may differ while deriving the same public key.
        if (alternatePrivate) candidate.back() ^= 1;
        assert(mesh::LocalIdentity::validatePrivateKey(candidate.data()));
        memset(publicKey, 0xa5, sizeof(publicKey));
        assert(onchip::importIdentity(target, candidate.data(), publicKey) == IdentityChange::Duplicate);
        for (auto byte : publicKey) assert(byte == 0xa5);
        assert(identity_test::durable == before && identity_test::commits == commitsBefore);
        assert(identity_test::handles.empty());
      }
    }
  }
  identity_test::durable = distinct;
  identity_test::durable[{"mc-onchip", "observer"}] = {1, 2, 3};
  const auto corruptOther = identity_test::durable;
  assert(onchip::importIdentity("command-bot", raw, publicKey) == IdentityChange::Rejected);
  assert(identity_test::durable == corruptOther);
  identity_test::durable = distinct;
  identity_test::readHook = +[](const char *key) {
    if (!strcmp(key, "room")) identity_test::failRead = true;
  };
  assert(onchip::importIdentity("command-bot", raw, publicKey) == IdentityChange::Rejected);
  identity_test::readHook = nullptr; identity_test::failRead = false;
  assert(identity_test::durable == distinct && identity_test::handles.empty());
  identity_test::durable = baseline;
  puts("PASS identity import journal: native private validation, same-key retry, conflicting pending denial, cancellation, every write/readback cut, activation/reset fences, cross-role active/pending public-key duplicates and unreadable-role denial");
}

static void reconstruction() {
  Fixture f;
  assert(psram_test::allocations.size() == 3);
  assert(psram_test::contains(&onchip::repeaterPackets()));
  assert(psram_test::contains(&onchip::roomPackets()));
  assert(psram_test::contains(&onchip::companionPackets()));
  assert(!psram_test::contains(&onchip::repeaterRadio()));
  assert(!psram_test::contains(&onchip::companionSessions()));
  const auto identities = identity_test::durable;
  mesh::QueuedRadioStats previous, after, room;
  assert(onchip::roomRadio().getQueuedRadioStats(room));
  for (Role role : {Role::Repeater, Role::Room, Role::Companion}) {
    auto &radio = role == Role::Repeater ? onchip::repeaterRadio()
                  : role == Role::Room   ? onchip::roomRadio()
                                         : onchip::companionRadio();
    auto &sibling =
        role == Role::Room ? onchip::repeaterRadio() : onchip::roomRadio();
    assert(sibling.getQueuedRadioStats(room));
    for (unsigned i = 0; i < 100; ++i) {
      assert(radio.getQueuedRadioStats(previous));
      assert(onchip::requestLifecycle(role, LifecycleAction::Reboot));
      assert(!onchip::requestLifecycle(role, LifecycleAction::Reset));
      f.step(2);
      assert(onchip::rolePhase(role) == RolePhase::Running);
      assert(radio.getQueuedRadioStats(after));
      assert(previous.generation != after.generation);
      assert(psram_test::allocations.size() == 3);
      assert(onchip::repeaterPackets().getFreeCount() >= 31);
      assert(onchip::roomPackets().getFreeCount() >= 31);
      assert(onchip::companionPackets().getFreeCount() == 16);
      assert(sibling.getQueuedRadioStats(after) &&
             after.generation == room.generation);
    }
  }
  assert(identity_test::durable == identities);
  filesystem_test::writeLimit = 0;
  assert(onchip::repeaterRadio().getQueuedRadioStats(previous));
  assert(onchip::requestLifecycle(Role::Repeater, LifecycleAction::Reboot));
  f.step();
  assert(onchip::rolePhase(Role::Repeater) == RolePhase::Running);
  assert(onchip::repeaterRadio().getQueuedRadioStats(after) &&
         previous.generation == after.generation);
  filesystem_test::writeLimit = SIZE_MAX;
}

static void role_storage_failures() {
  Fixture f;
  const auto identities = identity_test::durable;
  for (Role role : {Role::Repeater, Role::Room, Role::Companion}) {
    auto &radio = role == Role::Repeater ? onchip::repeaterRadio()
                  : role == Role::Room ? onchip::roomRadio()
                                       : onchip::companionRadio();
    mesh::QueuedRadioStats previous, current;
    assert(radio.getQueuedRadioStats(previous));
    const auto attempts = psram_test::attempts;
    const auto released = psram_test::released;
    psram_test::failAfter = 0;
    assert(onchip::requestLifecycle(role, LifecycleAction::Reboot));
    f.step(2);
    assert(onchip::rolePhase(role) == RolePhase::Failed);
    assert(psram_test::allocations.size() == 2 &&
           psram_test::released == released + 1 &&
           psram_test::attempts == attempts + 1);
    RadioDashboard::RoleStatus status;
    onchip::roleStatus(role, status);
    assert(!status.ready && !status.has_identity &&
           !strcmp(status.state, "fault") && status.fault[0]);
    assert(radio.sourceSlot() == -1 && !radio.queuedReady());
    for (unsigned i = 0; i < 3; ++i)
      if (Role(i) != role)
        assert(onchip::rolePhase(Role(i)) == RolePhase::Running);
    assert(identity_test::durable == identities);
    // No repeated allocation attempt before the existing bounded retry.
    f.step(100);
    assert(psram_test::attempts == attempts + 1);
    psram_test::failAfter = -1;
    f.step(160);
    assert(onchip::rolePhase(role) == RolePhase::Running &&
           psram_test::allocations.size() == 3);
    assert(radio.getQueuedRadioStats(current) &&
           current.generation != previous.generation);

    const auto &callback = callbacks[unsigned(role)];
    callback.stop();
    assert(psram_test::allocations.size() == 2);
    onchip::LocalRadio occupied, reserved;
    assert(occupied.attach(f.mux) && reserved.attach(f.mux));
    assert(!callback.start(f.mux)); // Allocation succeeds, mux admission fails.
    assert(psram_test::allocations.size() == 2 && radio.sourceSlot() == -1);
    occupied.detach();
    reserved.detach();
    identity_test::failRead = true;
    assert(!callback.start(f.mux)); // Native object exists when identity fails.
    identity_test::failRead = false;
    assert(psram_test::allocations.size() == 2 && radio.sourceSlot() == -1);
    callback.stop(); // Cleanup is safe after partial construction/failure.
    assert(callback.start(f.mux));
    assert(psram_test::allocations.size() == 3);
    assert(identity_test::durable == identities);
  }
  puts("PASS PSRAM-only native allocations, failed admission/identity cleanup, "
       "bounded exhaustion recovery and unchanged identity/PHY state");
}

static void missing_psram_boot() {
  const auto identities = identity_test::durable;
  Fixture f(false);
  assert(psram_test::allocations.empty());
  assert(identity_test::durable == identities);
  const auto attempts = psram_test::attempts;
  f.step(100);
  assert(f.radio.sent.empty() && psram_test::attempts == attempts);
  for (unsigned i = 0; i < 3; ++i) {
    RadioDashboard::RoleStatus status;
    onchip::roleStatus(Role(i), status);
    assert(!status.ready && !status.has_identity && status.source_slot == -1);
  }
  f.step(160);
  assert(psram_test::allocations.size() == 3);
  for (unsigned i = 0; i < 3; ++i)
    assert(onchip::rolePhase(Role(i)) == RolePhase::Running);
  assert(identity_test::durable == identities);
}

#include "observer_cases.h"

static void role_profile_storage() {
  using onchip::RoleProfile;
  identity_test::durable.erase({"mc-onchip", "role-profile"});
  RoleProfile loaded{0};
  const unsigned commits = identity_test::commits;
  assert(onchip::loadRoleProfile(loaded) && loaded.enabled == RoleProfile::All);
  assert(identity_test::commits == commits);
  assert(!onchip::saveRoleProfile({0x80}) && identity_test::commits == commits);
  assert(onchip::saveRoleProfile({RoleProfile::Repeater | RoleProfile::Observer}));
  const auto original = stored("role-profile");
  assert(original.size() == 12);
  assert(onchip::loadRoleProfile(loaded) &&
         loaded.enabled == (RoleProfile::Repeater | RoleProfile::Observer));
  identity_test::failWrite = true;
  assert(!onchip::saveRoleProfile({0}) && stored("role-profile") == original);
  identity_test::failWrite = false;
  identity_test::failCommit = true;
  assert(!onchip::saveRoleProfile({0}) && stored("role-profile") == original);
  identity_test::failCommit = false;
  identity_test::failRead = true;
  assert(!onchip::loadRoleProfile(loaded) && loaded.enabled == 0);
  identity_test::failRead = false;
  auto &record = identity_test::durable.at({"mc-onchip", "role-profile"});
  for (size_t offset : {size_t(0), size_t(4), size_t(5), size_t(6),
                        size_t(11)}) {
    record = original;
    record[offset] ^= offset == 5 ? 0x80 : 1;
    loaded.enabled = RoleProfile::All;
    assert(!onchip::loadRoleProfile(loaded) && loaded.enabled == 0);
  }
  // A self-consistent checksum must not make unknown roles or versions valid.
  for (size_t offset : {size_t(4), size_t(5), size_t(6)}) {
    record = original;
    record[offset] = offset == 5 ? 0x80 : 2;
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < 8; ++i)
      hash = (hash ^ record[i]) * 16777619u;
    for (unsigned i = 0; i < 4; ++i)
      record[8 + i] = uint8_t(hash >> (8 * i));
    assert(!onchip::loadRoleProfile(loaded) && loaded.enabled == 0);
  }
  record = {1, 2, 3};
  assert(!onchip::loadRoleProfile(loaded) && loaded.enabled == 0);
  record = original;
  record.push_back(0);
  assert(!onchip::loadRoleProfile(loaded) && loaded.enabled == 0);
  record = original;
  assert(identity_test::handles.empty());
}

static void role_profile_combinations() {
  using onchip::RoleProfile;
  const uint8_t masks[] = {
      uint8_t(RoleProfile::Repeater | RoleProfile::Companion),
      uint8_t(RoleProfile::Repeater | RoleProfile::Room),
      uint8_t(RoleProfile::Repeater | RoleProfile::Observer),
      RoleProfile::Repeater, RoleProfile::Room, RoleProfile::Companion,
      RoleProfile::Observer, 0};
  auto retainedRoom = stored("room");
  auto retainedCompanion = stored("companion");
  auto retainedObserver = stored("observer");
  const auto file = filesystem_test::files.find("/companion/retained-profile");
  assert(file == filesystem_test::files.end());
  filesystem_test::files["/companion/retained-profile"] = {42};
  for (uint8_t mask : masks) {
    assert(onchip::saveRoleProfile({mask}));
    onchip::Observer active;
    {
      Fixture f(true, {mask});
      assert((onchip::companionSessions().port() != 0) ==
             bool(mask & RoleProfile::Companion));
      unsigned count = 0;
      for (unsigned i = 0; i < 3; ++i) {
        const Role role = Role(i);
        RadioDashboard::RoleStatus status;
        onchip::roleStatus(role, status);
        const bool active = mask & (1u << i);
        assert(active == (onchip::rolePhase(role) == RolePhase::Running));
        assert(status.ready == active && status.has_identity == active);
        assert((status.source_slot >= 0) == active);
        if (!active) {
          assert(!strcmp(status.state, "disabled") && !status.fault[0]);
          assert(!onchip::requestLifecycle(role, LifecycleAction::Reboot));
          assert(!onchip::requestLifecycle(role, LifecycleAction::Reset));
        }
        count += active;
      }
      assert(psram_test::allocations.size() == count);
      onchip::ClockSnapshot clocks;
      assert(onchip::clockSnapshot(clocks));
      assert(clocks.network_enabled == bool(mask & (RoleProfile::Repeater |
                                                    RoleProfile::Room |
                                                    RoleProfile::Companion)));
      for (unsigned i = 0; i < 3; ++i)
        assert(clocks.roles[i].enabled == bool(mask & (1u << i)) &&
               (clocks.roles[i].enabled || !clocks.roles[i].epoch));
      if (!(mask & RoleProfile::Room))
        assert(stored("room") == retainedRoom);
      if (!(mask & RoleProfile::Companion))
        assert(stored("companion") == retainedCompanion);
      if (!(mask & RoleProfile::Observer))
        assert(stored("observer") == retainedObserver);
      if (mask & RoleProfile::Observer) {
        assert(active.begin(f.mux));
        RadioDashboard::RoleStatus observerStatus;
        active.dashboardStatus(observerStatus);
        assert(observerStatus.has_identity &&
               !strcmp(observerStatus.state, "connecting"));
      }
      assert(filesystem_test::files.at("/companion/retained-profile") == Bytes{42});
      // Saving during a running boot does not change any active lifecycle.
      assert(onchip::saveRoleProfile({0}));
      f.step(2);
      for (unsigned i = 0; i < 3; ++i)
        assert(onchip::rolePhase(Role(i)) ==
               (mask & (1u << i) ? RolePhase::Running : RolePhase::Disabled));
    }
    if (mask & RoleProfile::Observer)
      onchip::ObserverTest::close(active);
    assert(onchip::companionSessions().port() == 0);
  }
  filesystem_test::files.erase("/companion/retained-profile");
  assert(onchip::saveRoleProfile({RoleProfile::All}));
  puts("PASS boot profile combinations, disabled source/clock/port and storage");
}

static void role_profile_pending_and_runtime() {
  using onchip::RoleProfile;
  onchip::HardwareRNG rng;
  mesh::LocalIdentity roomReplacement(&rng), companionReplacement(&rng);
  assert(onchip::stageIdentity(Role::Room, roomReplacement));
  assert(onchip::stageIdentity(Role::Companion, companionReplacement));
  const auto roomPending = stored("room");
  const auto companionPending = stored("companion");
  const auto observerBefore = stored("observer");
  filesystem_test::files["/companion/profile-preserved"] = {7, 8};
  mesh::LocalIdentity observerReplacement(&rng);
  Bytes observerPending(133);
  memcpy(observerPending.data(), "MCI\1", 4);
  observerPending[4] = 1;
  memcpy(observerPending.data() + 5, observerBefore.data(), 64);
  observerReplacement.writeTo(observerPending.data() + 69, 64);
  identity_test::durable[{"mc-onchip", "observer"}] = observerPending;

  assert(onchip::saveRoleProfile({RoleProfile::Repeater}));
  {
    TestRadio radio;
    WifiKissMultiplexer mux;
    mux.attachRadio(
        radio, rng, [](float, float, uint8_t, uint8_t) {}, [](uint8_t) {});
    assert(mux.setInitialConfiguration({912525000, 250000, 7, 5, 2}, true));
    mesh::LocalIdentity modem;
    assert(onchip::loadIdentity("modem", modem));
    const auto validProfile = stored("role-profile");
    identity_test::durable[{"mc-onchip", "role-profile"}][4] = 2;
    assert(!onchip::begin(mux, modem) && psram_test::allocations.empty() &&
           onchip::companionSessions().port() == 0);
    identity_test::durable[{"mc-onchip", "role-profile"}] = validProfile;
    identity_test::failRead = true;
    assert(!onchip::begin(mux, modem) && psram_test::allocations.empty() &&
           onchip::companionSessions().port() == 0);
    identity_test::failRead = false;
    const auto savedRepeater = stored("repeater");
    identity_test::durable[{"mc-onchip", "repeater"}] = stored("modem");
    assert(onchip::begin(mux, modem));
    WiFiServer server;
    auto tryClaim = [&](uint8_t role, const uint8_t *key,
                        bool saveOverlappingProfile = false) {
      int sockets[2];
      assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
      server.add(WiFiClient(sockets[0]));
      mux.poll(server);
      auto sendCommand = [&](const Bytes &payload) {
        Bytes wire{0xc0, 6};
        for (uint8_t byte : payload) {
          if (byte == 0xc0 || byte == 0xdb) {
            wire.push_back(0xdb);
            wire.push_back(byte == 0xc0 ? 0xdc : 0xdd);
          } else
            wire.push_back(byte);
        }
        wire.push_back(0xc0);
        assert(send(sockets[1], wire.data(), wire.size(), 0) ==
               ssize_t(wire.size()));
        mux.poll(server);
        uint8_t encoded[128];
        const int length = recv(sockets[1], encoded, sizeof(encoded), MSG_DONTWAIT);
        assert(length >= 4 && encoded[0] == 0xc0 && encoded[length - 1] == 0xc0);
        Bytes answer;
        for (int j = 1; j < length - 1; ++j) {
          uint8_t byte = encoded[j];
          if (byte == 0xdb) {
            assert(++j < length - 1);
            byte = encoded[j] == 0xdc ? 0xc0 : 0xdb;
          }
          answer.push_back(byte);
        }
        return answer;
      };
      const auto hello = sendCommand({queued_tx::HELLO, 1, 0});
      assert(hello[1] == 0xa0 && hello[3] == queued_tx::NONE);
      Bytes payload{queued_tx::ROLE_PRESENCE, 1, role};
      payload.insert(payload.end(), key, key + 32);
      const auto answer = sendCommand(payload);
      assert(answer.size() == 10 && answer[1] == 0xa6 &&
             answer[3] == queued_tx::NONE &&
             queued_tx::get32(answer.data() + 4) ==
                 queued_tx::get32(hello.data() + 4) &&
             answer[8] == RoleProfile::Repeater);
      if (saveOverlappingProfile) {
        assert(onchip::saveRoleProfile(
            {RoleProfile::Repeater | RoleProfile::Room}));
        RoleProfile saved;
        assert(onchip::loadRoleProfile(saved) &&
               saved.enabled == (RoleProfile::Repeater | RoleProfile::Room) &&
               onchip::rolePhase(Role::Room) == RolePhase::Disabled &&
               mux.clientCount() == 1);
      }
      close(sockets[1]);
      mux.poll(server);
      return answer[9];
    };
    const auto activeRepeater = stored("repeater");
    mesh::LocalIdentity repeater;
    repeater.readFrom(activeRepeater.data(), 64);
    assert(!memcmp(repeater.pub_key, modem.pub_key, 32));
    assert(tryClaim(0, roomReplacement.pub_key, true) ==
           queued_tx::NATIVE_ROLE_PRESENT);
    assert(tryClaim(1, repeater.pub_key) ==
           queued_tx::NATIVE_KEY_PRESENT);
    assert(tryClaim(1, modem.pub_key) ==
           queued_tx::NATIVE_KEY_PRESENT);
    assert(tryClaim(1, roomReplacement.pub_key) == 0);
    RadioDashboard::RadioStatus status;
    mux.dashboardStatus(status);
    status.wifi_connected = true;
    onchip::dashboardStatus(status, true);
#if defined(MESHCORE_ONCHIP_BOT) && MESHCORE_ONCHIP_BOT
    assert(status.role_count == 7 && status.roles[0].has_identity);
#else
    assert(status.role_count == 6 && status.roles[0].has_identity);
#endif
    assert(!memcmp(status.roles[0].public_key, modem.pub_key, 32));
    for (unsigned i : {1u, 2u, 3u}) {
      const auto &role = status.roles[i];
      assert(!strcmp(role.state, "disabled") && !role.ready &&
             !role.has_identity && role.source_slot == -1 &&
             !role.source_generation && !role.fault[0]);
    }
    assert(!strcmp(status.roles[4].role, "bot") &&
           status.roles[4].has_identity && status.roles[4].ready);
    assert(!strcmp(status.roles[5].role, "management") &&
           status.roles[5].has_identity &&
           !strcmp(status.roles[5].state, "unprovisioned") &&
           !status.roles[5].ready && status.roles[5].source_slot >= 0);
    assert(tryClaim(1, status.roles[5].public_key) ==
           queued_tx::NATIVE_KEY_PRESENT);
    assert(onchip::companionSessions().port() == 0);
    assert(stored("room") == roomPending &&
           stored("companion") == companionPending &&
           stored("observer") == observerPending);
    assert(filesystem_test::files.at("/companion/profile-preserved") ==
           (Bytes{7, 8}));
    for (const auto &callback : callbacks)
      callback.stop();
    onchip::stopManagementForTest();
    identity_test::durable[{"mc-onchip", "repeater"}] = savedRepeater;
    assert(psram_test::allocations.empty());
  }
  assert(onchip::saveRoleProfile({RoleProfile::Room | RoleProfile::Companion |
                                  RoleProfile::Observer}));
  onchip::Observer observer;
  {
    Fixture f(true, {uint8_t(RoleProfile::Room | RoleProfile::Companion |
                              RoleProfile::Observer)});
    assert(stored("room").size() == 64 &&
           stored("companion").size() == 64);
    assert(!memcmp(stored("room").data(), roomPending.data() + 69, 64));
    assert(!memcmp(stored("companion").data(), companionPending.data() + 69,
                   64));
    assert(observer.begin(f.mux));
    RadioDashboard::RoleStatus status;
    observer.dashboardStatus(status);
    assert(status.has_identity &&
           !memcmp(status.public_key, observerReplacement.pub_key, 32));
    assert(stored("observer").size() == 64 &&
           !memcmp(stored("observer").data(), observerPending.data() + 69, 64));
    assert(filesystem_test::files.at("/companion/profile-preserved") ==
           (Bytes{7, 8}));
  }
  onchip::ObserverTest::close(observer);
  filesystem_test::files.erase("/companion/profile-preserved");
  assert(onchip::saveRoleProfile({RoleProfile::All}));
  puts("PASS disabled pending identities survive boot and activate only on reenable");
}

static void management_rf() {
  using onchip::RoleProfile;
  assert(RoleProfile{0}.bootableWith("", "") &&
         RoleProfile{RoleProfile::Observer}.bootableWith("", "") &&
         !RoleProfile{RoleProfile::Repeater}.bootableWith("", "") &&
         !RoleProfile{RoleProfile::Room}.bootableWith("", "") &&
         !RoleProfile{RoleProfile::Companion}.bootableWith("", "") &&
         RoleProfile{RoleProfile::Companion}.bootableWith("admin", "") &&
         !RoleProfile{0}.bootableWith("1234567890123456", ""));
  assert(onchip::saveRoleProfile({0}));
  onchip::HardwareRNG rng;
  mesh::LocalIdentity operatorIdentity(&rng);
  char operatorHex[65];
  mesh::Utils::toHex(operatorHex, operatorIdentity.pub_key, 32);
  const auto originalRoom = stored("room");
  const auto originalModem = stored("modem");
  uint8_t managementKey[32]{};
  Bytes signedRequest(129);
  memcpy(signedRequest.data(), "MCORE-ROLE-RF-V1", 16);
  auto put64 = [](uint8_t *data, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) data[i] = value >> (8 * i);
  };
  auto get64 = [](const uint8_t *data) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= uint64_t(data[i]) << (8 * i);
    return value;
  };
  auto receipt = [&](const Bytes &wire, Peer &sender, uint64_t generation,
                     uint64_t nonce, uint8_t result, bool relayed = false,
                     bool flood = true) {
    mesh::Packet response;
    assert(response.readFrom(wire.data(), wire.size()));
    assert(response.getPayloadType() == PAYLOAD_TYPE_RESPONSE &&
           response.isRouteFlood() == flood &&
           response.path_len == 0);
    assert(response.payload_len == 180 &&
           response.payload[0] == sender.self_id.pub_key[0] &&
           response.payload[1] == managementKey[0]);
    uint8_t secret[32], plain[184];
    sender.self_id.calcSharedSecret(secret, sender.server);
    assert(mesh::Utils::MACThenDecrypt(secret, plain, response.payload + 2,
                                       response.payload_len - 2) == 176);
    assert(!memcmp(plain, "MCORE-ROLE-RCPT-V1", 18) &&
           !memcmp(plain + 18, managementKey, 32) &&
           !memcmp(plain + 50, sender.self_id.pub_key, 32));
    uint64_t signedGeneration = 0, signedNonce = 0;
    for (unsigned i = 0; i < 8; ++i) {
      signedGeneration |= uint64_t(plain[82 + i]) << (8 * i);
      signedNonce |= uint64_t(plain[90 + i]) << (8 * i);
    }
    assert(signedGeneration == generation && signedNonce == nonce &&
           plain[98] == result && !memcmp(plain + 163, Bytes(13).data(), 13) &&
           mesh::Identity(managementKey).verify(plain + 99, plain, 99));
    Bytes delivered = wire;
    if (relayed) {
      delivered[1] = 1;
      delivered.insert(delivered.begin() + 2, 0x56);
    }
    sender.radio.incoming.push_back(delivered);
    now += 20;
    sender.loop();
    assert(sender.responses.size() == 1 &&
           sender.responses.front() == Bytes(plain, plain + 176));
    sender.responses.clear();
  };
  {
    Fixture f(true, {0});
    onchip::Management receiver;
    assert(receiver.begin(f.mux, operatorHex));
    f.management = &receiver;
    memcpy(managementKey, receiver.publicKey(), 32);
    RadioDashboard::RoleStatus status;
    receiver.dashboardStatus(status);
    assert(status.ready && status.source_slot >= 0 &&
           !memcmp(status.public_key, managementKey, 32));
    Peer sender;
    sender.server = mesh::Identity(managementKey);
    memcpy(signedRequest.data() + 16, managementKey, 32);
    put64(signedRequest.data() + 48, 1);
    put64(signedRequest.data() + 56, 42);
    signedRequest[64] = RoleProfile::Room;
    auto sendRequest = [&](Bytes request, bool flood = true, int expected = 0,
                           bool scoped = false) {
      uint8_t secret[32];
      sender.self_id.calcSharedSecret(secret, sender.server);
      auto *packet = sender.createAnonDatagram(PAYLOAD_TYPE_ANON_REQ,
          sender.self_id, sender.server, secret, request.data(), request.size());
      assert(packet);
      assert(packet->payload_len == 179);
      if (scoped) {
        uint16_t codes[2] = {0x1234, 0x5678};
        sender.sendFlood(packet, codes, 0, 1);
      } else if (flood) sender.sendFlood(packet);
      else sender.sendZeroHop(packet);
      now += 20;
      sender.loop();
      sender.loop();
      assert(!sender.radio.sent.empty());
      auto wire = sender.radio.sent.front();
      sender.radio.sent.pop_front();
      assert(wire.size() == (scoped ? 185u : 181u) &&
             (wire[0] & PH_ROUTE_MASK) ==
             (scoped ? ROUTE_TYPE_TRANSPORT_FLOOD :
              flood ? ROUTE_TYPE_FLOOD : ROUTE_TYPE_DIRECT));
      assert((wire[0] >> PH_TYPE_SHIFT & PH_TYPE_MASK) == PAYLOAD_TYPE_ANON_REQ);
      const size_t payloadOffset = scoped ? 6 : 2;
      if (scoped)
        assert(wire[1] == 0x34 && wire[2] == 0x12 &&
               wire[3] == 0x78 && wire[4] == 0x56);
      assert(wire[payloadOffset] == managementKey[0] &&
             !memcmp(wire.data() + payloadOffset + 1,
                     sender.self_id.pub_key, 32));
      uint8_t decoded[MAX_PACKET_PAYLOAD];
      assert(mesh::Utils::MACThenDecrypt(secret, decoded,
                                         wire.data() + payloadOffset + 33,
                                         wire.size() - payloadOffset - 33) == 144);
      assert(!memcmp(decoded, request.data(), request.size()));
      f.mux.received(wire.data(), wire.size(), -90, 5);
      f.step(80);
      if (expected) {
        assert(f.radio.sent.size() == 1);
        receipt(f.radio.sent.front(), sender, get64(request.data() + 48),
                get64(request.data() + 56), expected, false, flood);
        f.radio.sent.pop_front();
      } else {
        assert(f.radio.sent.empty());
      }
    };
    auto sign = [&] {
      operatorIdentity.sign(signedRequest.data() + 65,
                            signedRequest.data(), 65);
    };
    sign();
    assert(mesh::Identity(operatorIdentity.pub_key).verify(
        signedRequest.data() + 65, signedRequest.data(), 65));
    const auto baseline = stored("role-profile");
    const auto commitsBeforeInvalid = identity_test::commits;
    auto bad = signedRequest;
    bad[65] ^= 1;
    sendRequest(bad);
    assert(stored("role-profile") == baseline);
    bad = signedRequest;
    bad[16] ^= 1;
    sendRequest(bad);
    assert(stored("role-profile") == baseline);
    bad = signedRequest;
    bad[64] = 0x80;
    sendRequest(bad);
    assert(stored("role-profile") == baseline);
    bad = signedRequest;
    bad.push_back(1);
    sendRequest(bad);
    auto scopedRequest = signedRequest;
    put64(scopedRequest.data() + 56, 99);
    operatorIdentity.sign(scopedRequest.data() + 65,
                          scopedRequest.data(), 65);
    scopedRequest[65] ^= 1;
    sendRequest(scopedRequest, true, 0, true);
    assert(stored("role-profile") == baseline &&
           identity_test::commits == commitsBeforeInvalid &&
           stored("room") == originalRoom &&
           stored("modem") == originalModem);
    identity_test::failCommit = true;
    sendRequest(signedRequest, true, 3);
    identity_test::failCommit = false;
    assert(stored("role-profile") == baseline);
    receiver.dashboardStatus(status);
    assert(!status.ready && !strcmp(status.state, "fault"));
    now += 1200;
    auto later = signedRequest;
    put64(later.data() + 48, 2);
    put64(later.data() + 56, 43);
    operatorIdentity.sign(later.data() + 65, later.data(), 65);
    sendRequest(later);
    assert(stored("role-profile") == baseline);
    f.management = nullptr;
    receiver.stop();
  }
  {
    Fixture f(true, {0});
    onchip::Management receiver;
    assert(receiver.begin(f.mux, operatorHex));
    f.management = &receiver;
    Peer sender;
    sender.server = mesh::Identity(managementKey);
    auto send = [&](const Bytes &request, int expected, unsigned steps = 80,
                    bool relayed = false, bool flood = true) {
      sender.self_id = mesh::LocalIdentity(&rng);
      uint8_t secret[32];
      sender.self_id.calcSharedSecret(secret, sender.server);
      auto *packet = sender.createAnonDatagram(PAYLOAD_TYPE_ANON_REQ,
          sender.self_id, sender.server, secret, request.data(), request.size());
      assert(packet);
      if (flood) sender.sendFlood(packet);
      else sender.sendZeroHop(packet);
      now += 20;
      sender.loop();
      sender.loop();
      assert(!sender.radio.sent.empty());
      auto wire = sender.radio.sent.front();
      sender.radio.sent.pop_front();
      if (relayed) {
        assert(wire[1] == 0);
        wire[1] = 1;
        wire.insert(wire.begin() + 2, 0x56);
      }
      f.mux.received(wire.data(), wire.size(), -90, 5);
      f.step(steps);
      if (expected) {
        assert(f.radio.sent.size() == 1);
        receipt(f.radio.sent.front(), sender, get64(request.data() + 48),
                get64(request.data() + 56), expected, relayed, flood);
        f.radio.sent.pop_front();
      } else {
        assert(f.radio.sent.empty());
      }
    };
    send(signedRequest, 1, 80, true);
    onchip::ProfileJournal journal;
    assert(onchip::loadProfileJournal(journal) &&
           journal.generation == 1 && journal.nonce == 42 &&
           journal.profile.enabled == RoleProfile::Room);
    const auto saved = stored("role-profile");
    const auto committed = identity_test::commits;
    assert(!onchip::saveRoleProfile({0}));
    now += 1200;
    send(signedRequest, 2, 2);
    assert(identity_test::commits == committed && stored("role-profile") == saved);
    now += 1200;
    send(signedRequest, 2, 2, false, false);
    assert(identity_test::commits == committed && stored("role-profile") == saved);
    send(signedRequest, 0, 2); // An authenticated burst is rate limited.
    assert(identity_test::commits == committed);
    now += 1200;
    auto stale = signedRequest;
    put64(stale.data() + 56, 44);
    operatorIdentity.sign(stale.data() + 65, stale.data(), 65);
    send(stale, 3);
    assert(identity_test::commits == committed && stored("role-profile") == saved);
    auto conflicting = signedRequest;
    conflicting[64] = 0;
    operatorIdentity.sign(conflicting.data() + 65, conflicting.data(), 65);
    send(conflicting, 3);
    assert(identity_test::commits == committed && stored("role-profile") == saved);
    put64(signedRequest.data() + 48, 2);
    put64(signedRequest.data() + 56, 43);
    signedRequest[64] = 0;
    operatorIdentity.sign(signedRequest.data() + 65, signedRequest.data(), 65);
    now += 1200;
    send(signedRequest, 1);
    assert(onchip::loadProfileJournal(journal) &&
           journal.generation == 2 && journal.nonce == 43 &&
           journal.profile.enabled == 0);
    RadioDashboard::RoleStatus status;
    receiver.dashboardStatus(status);
    assert(status.profile_generation == 2 && status.ready);
    f.management = nullptr;
    receiver.stop();
  }
  {
    Fixture f(true, {0});
    onchip::Management receiver;
    assert(receiver.begin(f.mux, ""));
    assert(!memcmp(receiver.publicKey(), managementKey, 32));
    RadioDashboard::RoleStatus status;
    receiver.dashboardStatus(status);
    assert(status.profile_generation == 2);
#if defined(MESHCORE_MAST_ADMIN) && MESHCORE_MAST_ADMIN
    assert(status.ready && !strcmp(status.state, "running"));
#else
    assert(!status.ready && !strcmp(status.state, "unprovisioned"));
#endif
    receiver.stop();
  }
  assert(stored("room") == originalRoom && stored("modem") == originalModem);
  onchip::RoleProfile boot;
  assert(onchip::loadRoleProfile(boot) && boot.enabled == 0);
  auto &record = identity_test::durable.at({"mc-onchip", "role-profile"});
  const auto validJournal = record;
  for (size_t offset : {size_t(4), size_t(8), size_t(16), size_t(27)}) {
    record = validJournal;
    record[offset] ^= 1;
    boot.enabled = RoleProfile::All;
    assert(!onchip::loadRoleProfile(boot) && boot.enabled == 0);
  }
  record = validJournal;
  std::fill(record.begin() + 8, record.begin() + 16, 0);
  uint32_t checksum = 2166136261u;
  for (unsigned i = 0; i < 24; ++i)
    checksum = (checksum ^ record[i]) * 16777619u;
  for (unsigned i = 0; i < 4; ++i)
    record[24 + i] = checksum >> (8 * i);
  assert(!onchip::loadRoleProfile(boot) && boot.enabled == 0);
  record = validJournal;
  {
    Fixture f(true, {0});
    onchip::Management receiver;
    const auto identity = stored("management");
    identity_test::durable[{"mc-onchip", "management"}] = {1, 2};
    assert(!receiver.begin(f.mux, operatorHex));
    assert(psram_test::allocations.empty());
    identity_test::durable[{"mc-onchip", "management"}] = identity;
    psram_test::failAfter = 0;
    assert(!receiver.begin(f.mux, operatorHex));
    psram_test::failAfter = -1;
    assert(psram_test::allocations.empty());
    assert(receiver.begin(f.mux, operatorHex));
    receiver.stop();
  }
  puts("PASS routed signed receipts, malformed/target/signature/replay, NVS "
       "failure seal, disabled-role RF access and boot journal");
}

#include "management_query_cases.h"

static void management_with_all_roles() {
  assert(onchip::saveRoleProfile({onchip::RoleProfile::All}));
  Fixture f;
  onchip::Management receiver;
  assert(receiver.begin(f.mux, ""));
  assert(psram_test::allocations.size() == 4);
  RadioDashboard::RoleStatus status;
  receiver.dashboardStatus(status);
  assert(status.has_identity && status.source_slot == 7 &&
         !strcmp(status.state, "unprovisioned"));
  mesh::QueuedRadioStats stats;
  assert(f.mux.getQueuedRadioStats(status.source_slot, stats));
  assert(!receiver.publicKey() ||
         memcmp(receiver.publicKey(), stored("modem").data(), 32));
  receiver.stop();
}

static void python_signed_interop(const char *path, const char *receiptPath,
                                  const char *queryPath, const char *statusPath) {
  std::ifstream stream(path, std::ios::binary);
  assert(stream.is_open());
  Bytes vector(std::istreambuf_iterator<char>{stream},
               std::istreambuf_iterator<char>{});
  assert(vector.size() == 64 + 32 + 32 + 64 + 32 + 181);
  std::ifstream queryStream(queryPath, std::ios::binary);
  assert(queryStream.is_open());
  Bytes queryVector(std::istreambuf_iterator<char>{queryStream},
                    std::istreambuf_iterator<char>{});
  assert(queryVector.size() == 64 + 32 + 32 + 64 + 32 + 165 &&
         std::equal(queryVector.begin(), queryVector.begin() + 224,
                    vector.begin()));
  const auto original = stored("management");
  identity_test::durable[{"mc-onchip", "management"}] =
      Bytes(vector.begin(), vector.begin() + 64);
  char operatorHex[65];
  mesh::Utils::toHex(operatorHex, vector.data() + 64, 32);
  mesh::LocalIdentity managementIdentity;
  assert(onchip::loadIdentity("management", managementIdentity));
  assert(!memcmp(managementIdentity.pub_key, vector.data() + 96, 32));
  mesh::LocalIdentity sender;
  sender.readFrom(vector.data() + 128, 64);
  const uint8_t *packet = vector.data() + 224;
  assert(packet[0] == 0x1d && packet[1] == 0x80 &&
         packet[2] == managementIdentity.pub_key[0] &&
         !memcmp(packet + 3, sender.pub_key, 32));
  uint8_t secret[32], decoded[MAX_PACKET_PAYLOAD];
  managementIdentity.calcSharedSecret(secret, packet + 3);
  assert(mesh::Utils::MACThenDecrypt(secret, decoded, packet + 35, 146) == 144);
  assert(!memcmp(decoded, "MCORE-ROLE-RF-V1", 16) &&
         !memcmp(decoded + 16, managementIdentity.pub_key, 32) &&
         mesh::Identity(vector.data() + 64).verify(decoded + 65, decoded, 65));
  const uint8_t *queryPacket = queryVector.data() + 224;
  assert(queryPacket[0] == 0x1d && queryPacket[1] == 0x80 &&
         queryPacket[2] == managementIdentity.pub_key[0] &&
         !memcmp(queryPacket + 3, sender.pub_key, 32));
  assert(mesh::Utils::MACThenDecrypt(secret, decoded, queryPacket + 35, 130) ==
             128 &&
         !memcmp(decoded, "MCORE-ROLE-QRY-V1", 17) &&
         !memcmp(decoded + 17, managementIdentity.pub_key, 32) &&
         mesh::Identity(vector.data() + 64).verify(decoded + 57, decoded, 57));
  {
    Fixture f(true, {0});
    onchip::Management receiver;
    assert(receiver.begin(f.mux, operatorHex));
    f.management = &receiver;
    f.mux.received(queryPacket, 165, -90, 5);
    f.step(2);
    assert(f.radio.sent.size() == 1);
    mesh::Packet status;
    assert(status.readFrom(f.radio.sent.front().data(),
                           f.radio.sent.front().size()) &&
           status.isRouteFlood() &&
           status.getPathHashSize() == 3 &&
           status.getPayloadType() == PAYLOAD_TYPE_RESPONSE &&
           status.payload_len == 180);
    std::ofstream statusFile(statusPath, std::ios::binary);
    assert(statusFile.is_open());
    statusFile.write(
        reinterpret_cast<const char *>(f.radio.sent.front().data()),
        f.radio.sent.front().size());
    assert(statusFile.good());
    f.radio.sent.pop_front();
    now += 1200;
    f.mux.received(packet, 181, -90, 5);
    f.step(2);
    onchip::ProfileJournal saved;
    assert(onchip::loadProfileJournal(saved) && saved.generation == 3 &&
           saved.nonce == 0x12345678 &&
           saved.profile.enabled == onchip::RoleProfile::Companion);
    assert(f.radio.sent.size() == 1);
    mesh::Packet response;
    assert(response.readFrom(f.radio.sent.front().data(), f.radio.sent.front().size()));
    assert(response.isRouteFlood() && response.getPayloadType() == PAYLOAD_TYPE_RESPONSE &&
           response.getPathHashSize() == 3 &&
           response.payload_len == 180);
    std::ofstream receiptFile(receiptPath, std::ios::binary);
    assert(receiptFile.is_open());
    receiptFile.write(reinterpret_cast<const char *>(f.radio.sent.front().data()),
                      f.radio.sent.front().size());
    assert(receiptFile.good());
    f.management = nullptr;
    receiver.stop();
  }
  identity_test::durable[{"mc-onchip", "management"}] = original;
  puts("PASS Python sender packet decrypted and authenticated by pinned native MeshCore");
}

int main(int argc, char **argv) {
  if (argc == 2 && !strcmp(argv[1], "--observer-public-test")) {
    public_observer_service();
    return 0;
  }
  assert(argc == 6);
#ifdef ONCHIP_SIGNED_ONLY_TEST
  for (const char *name : {"room", "modem", "management"}) {
    mesh::LocalIdentity identity;
    assert(onchip::loadIdentity(name, identity));
  }
  management_rf();
  management_status_queries();
  python_signed_interop(argv[2], argv[3], argv[4], argv[5]);
  puts("PASS beta-enabled signed role/status compatibility including fail-closed replay storage");
#else
  bounded_erasure();
  identity_failures();
  identity_imports();
  clock_authority();
  native_room_clock_replay(true);
  native_room_clock_replay(false);
  native_room_clock_replay(false, false);
  reconstruction();
  role_storage_failures();
  missing_psram_boot();
  authenticated_wire_lifecycle();
  companion_channel_configuration();
  companion_wire_lifecycle();
  companion_contact_capacity();
  observer_and_dashboard(argv[1]);
  role_profile_storage();
  role_profile_combinations();
  role_profile_pending_and_runtime();
  management_with_all_roles();
  management_rf();
  management_status_queries();
  python_signed_interop(argv[2], argv[3], argv[4], argv[5]);
  puts("PASS native lifecycle storage, reconstruction, encrypted "
       "administration and TCP reset");
#endif
}
