// SPDX-License-Identifier: Apache-2.0
#include <cerrno>
#include <algorithm>
#include <arpa/inet.h>
#include <cassert>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

static int tcpPair(int domain, int type, int protocol, int pair[2]) {
  assert(domain == AF_UNIX && type == SOCK_STREAM && protocol == 0);
  const int listener = socket(AF_INET, SOCK_STREAM, 0);
  assert(listener >= 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  assert(bind(listener, reinterpret_cast<sockaddr*>(&address),
              sizeof(address)) == 0);
  assert(listen(listener, 1) == 0);
  socklen_t length = sizeof(address);
  assert(getsockname(listener, reinterpret_cast<sockaddr*>(&address),
                     &length) == 0);
  pair[1] = socket(AF_INET, SOCK_STREAM, 0);
  assert(pair[1] >= 0);
  assert(connect(pair[1], reinterpret_cast<sockaddr*>(&address),
                 sizeof(address)) == 0);
  pair[0] = accept(listener, nullptr, nullptr);
  assert(pair[0] >= 0);
  const int nodelay = 1;
  assert(setsockopt(pair[0], IPPROTO_TCP, TCP_NODELAY, &nodelay,
                    sizeof(nodelay)) == 0);
  assert(setsockopt(pair[1], IPPROTO_TCP, TCP_NODELAY, &nodelay,
                    sizeof(nodelay)) == 0);
  close(listener);
  return 0;
}

#define socketpair tcpPair
#define main existing_phy_tests
#include "combined.cpp"
#undef main
#undef socketpair

struct SessionPeer {
  WiFiClient socket;
  int remote;

  explicit SessionPeer(WiFiServer& server) {
    int pair[2];
    assert(tcpPair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    socket = WiFiClient(pair[0]);
    remote = pair[1];
    server.add(socket);
  }
  ~SessionPeer() {
    close(remote);
    socket.stop();
  }
  void sendInput(const std::vector<uint8_t>& data) {
    assert(send(remote, data.data(), data.size(), 0) ==
           static_cast<ssize_t>(data.size()));
  }
  size_t blockOutput() {
    const int capacity = 4096;
    assert(setsockopt(socket.fd(), SOL_SOCKET, SO_SNDBUF,
                      &capacity, sizeof(capacity)) == 0);
    assert(setsockopt(remote, SOL_SOCKET, SO_RCVBUF,
                      &capacity, sizeof(capacity)) == 0);
    uint8_t filler[1024]{};
    size_t total = 0;
    for (;;) {
      const ssize_t count = send(socket.fd(), filler, sizeof(filler),
                                 MSG_DONTWAIT);
      if (count < 0) {
        assert(errno == EAGAIN || errno == EWOULDBLOCK);
        return total;
      }
      assert(count > 0);
      total += count;
    }
  }
};

static std::vector<std::vector<uint8_t>> request(
    Fixture& f, int peer, uint8_t port, const std::vector<uint8_t>& payload) {
  const auto wire = encode((port << 4) | KISS_CMD_SETHARDWARE, payload);
  assert(send(f.peers[peer], wire.data(), wire.size(), 0) ==
         static_cast<ssize_t>(wire.size()));
  f.step();
  return receive(f.peers[peer]);
}

static uint32_t hello(Fixture& f, uint8_t port, uint8_t owner = 0) {
  auto frames = request(f, 0, port, {queued_tx::HELLO, 1, owner});
  assert(frames.size() == 1 && frames[0].size() == 14 &&
         frames[0][0] == (port << 4 | 6) && frames[0][1] == 0xa0 &&
         frames[0][3] == queued_tx::NONE);
  const uint32_t generation = queued_tx::get32(frames[0].data() + 4);
  assert(generation != 0);
  return generation;
}

static std::vector<uint8_t> submitPacket(uint32_t generation, uint32_t id,
                                         uint8_t marker) {
  std::vector<uint8_t> packet(20);
  packet[0] = queued_tx::SUBMIT;
  packet[1] = queued_tx::VERSION;
  queued_tx::put32(packet.data() + 2, generation);
  queued_tx::put32(packet.data() + 6, id);
  packet[10] = 1;
  packet[19] = marker;
  return packet;
}

static bool hasEvent(const std::vector<std::vector<uint8_t>>& frames,
                     uint8_t port, uint32_t generation, uint32_t id,
                     uint8_t state, uint8_t reason = 0) {
  for (const auto& frame : frames)
    if (frame.size() == 25 && frame[0] == (port << 4 | 6) &&
        frame[1] == queued_tx::EVENT &&
        queued_tx::get32(frame.data() + 3) == generation &&
        queued_tx::get32(frame.data() + 7) == id &&
        frame[11] == state && frame[12] == reason)
      return true;
  return false;
}

static void capacityAndIsolation(bool verifyQuietKeepalive = false) {
  nvs_test::reset();
  Radio provisionedRadio;
  RNG provisionedRng;
  WifiKissMultiplexer provisioner;
  provisioner.attachRadio(provisionedRadio, provisionedRng, configure, power);
  assert(provisioner.setInitialConfiguration({910525000, 62500, 7, 5, 20},
                                             true));
  Fixture f(1.0f, true, true);
  auto legacyHello = request(f, 1, 0, {queued_tx::HELLO, 1, 0});
  assert(legacyHello.size() == 1 && legacyHello[0].size() == 14 &&
         legacyHello[0][0] == 6 && legacyHello[0][1] == 0xa0 &&
         legacyHello[0][2] == 1 && legacyHello[0][3] == 0 &&
         queued_tx::get32(legacyHello[0].data() + 4) == f.generations[1] &&
         queued_tx::get32(legacyHello[0].data() + 8) == 1 &&
         legacyHello[0][12] == KISS_REQUEST_QUEUE_DEPTH &&
         legacyHello[0][13] == 0);
  assert(request(f, 0, 0, {queued_tx::CAPACITY, 1}) ==
         std::vector<std::vector<uint8_t>>({{6, 0xa5, 1, 4, 4}}));
  assert(request(f, 0, 0, {queued_tx::CAPACITY, 1, 2}) ==
         std::vector<std::vector<uint8_t>>({{6, 0xf1, HW_ERR_INVALID_PARAM}}));
  assert(request(f, 0, 0, {queued_tx::CAPACITY, 1, 1}) ==
         std::vector<std::vector<uint8_t>>({{6, 0xa5, 1, 4, 4, 4, 1}}));
  assert(f.mux.clientCount() == 2);
  if (verifyQuietKeepalive) {
    sleep(80);
    f.step();
    assert(f.mux.clientCount() == 2);
    assert(request(f, 0, 0, {queued_tx::CAPACITY, 1}) ==
           std::vector<std::vector<uint8_t>>({{6, 0xa5, 1, 4, 4}}));
  }
  int additional[2];
  for (int& peer : additional) {
    int pair[2];
    assert(tcpPair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    f.server.add(WiFiClient(pair[0]));
    peer = pair[1];
    f.step();
    auto wire = encode(6, {queued_tx::HELLO, 1, 0});
    assert(send(peer, wire.data(), wire.size(), 0) ==
           static_cast<ssize_t>(wire.size()));
    f.step();
    auto frames = receive(peer);
    assert(frames.size() == 1 && frames[0].size() == 14 &&
           frames[0][3] == 0);
  }
  assert(f.mux.clientCount() == 4);
  for (int peer : additional) close(peer);
  f.step();
  assert(f.mux.clientCount() == 2);
  assert(request(f, 1, 0, {queued_tx::CAPACITY, 1}) ==
         std::vector<std::vector<uint8_t>>({{6, 0xa5, 1, 4, 4}}));
  assert(request(f, 0, 0, {queued_tx::CAPACITY, 1}) ==
         std::vector<std::vector<uint8_t>>({{6, 0xa5, 1, 4, 4}}));
  assert(request(f, 1, 0, {queued_tx::CAPACITY, 1, 1}) ==
         std::vector<std::vector<uint8_t>>({{6, 0xa5, 1, 4, 4, 4, 0}}));
  assert(request(f, 0, 1, {queued_tx::CONFIG, 1, 0}) ==
         std::vector<std::vector<uint8_t>>({{0x16, 0xf1, HW_ERR_INVALID_PARAM}}));
  // KissModem.Connect sends this preamble before the first child HELLO.
  assert(request(f, 0, 1, {HW_CMD_SET_SIGNAL_REPORT, 1}) ==
         std::vector<std::vector<uint8_t>>({{0x16, 0x9a, 1}}));
  assert(request(f, 0, 1, {HW_CMD_GET_SIGNAL_REPORT}) ==
         std::vector<std::vector<uint8_t>>({{0x16, 0x9a, 1}}));
  assert(request(f, 0, 2, {HW_CMD_SET_SIGNAL_REPORT, 0}) ==
         std::vector<std::vector<uint8_t>>({{0x26, 0x9a, 0}}));
  assert(request(f, 0, 2, {HW_CMD_GET_SIGNAL_REPORT}) ==
         std::vector<std::vector<uint8_t>>({{0x26, 0x9a, 0}}));
  auto denied = request(f, 0, 1, {queued_tx::HELLO, 1, 1});
  assert(denied.size() == 1 && denied[0][3] == queued_tx::NOT_OWNER);
  uint32_t generations[4] = {f.generations[0]};
  for (uint8_t port = 1; port < 4; ++port) {
    generations[port] = hello(f, port);
    for (uint8_t previous = 0; previous < port; ++previous)
      assert(generations[port] != generations[previous]);
    auto config = request(f, 0, port, {queued_tx::CONFIG, 1, 0});
    assert(config.size() == 1 && config[0].size() == 26 &&
           config[0][0] == (port << 4 | 6) && config[0][1] == 0xa2 &&
           config[0][3] == queued_tx::NONE);
  }
  assert(request(f, 0, 1, {HW_CMD_GET_SIGNAL_REPORT}) ==
         std::vector<std::vector<uint8_t>>({{0x16, 0x9a, 1}}));
  assert(request(f, 0, 2, {HW_CMD_GET_SIGNAL_REPORT}) ==
         std::vector<std::vector<uint8_t>>({{0x26, 0x9a, 0}}));
  assert(request(f, 0, 0, {queued_tx::CAPACITY, 1, 1}) ==
         std::vector<std::vector<uint8_t>>({{6, 0xa5, 1, 4, 4, 4, 1}}));
  assert(request(f, 0, 1, {queued_tx::STATS, 1})[0][0] == 0x16);
  assert(request(f, 0, 2, {0x17}) ==
         std::vector<std::vector<uint8_t>>({{0x26, 0x97}}));
  assert(request(f, 1, 0, {0x17}) ==
         std::vector<std::vector<uint8_t>>({{6, 0x97}}));
  auto deniedConfig = request(f, 0, 1,
                              {queued_tx::CONFIG, 1, 1, 1, 0, 0, 0});
  assert(deniedConfig.size() == 1 && deniedConfig[0][3] == queued_tx::INVALID);

  std::vector<uint8_t> policy{queued_tx::SOURCE_POLICY, 1};
  policy.resize(10);
  queued_tx::put32(policy.data() + 2, generations[1]);
  queued_tx::putFloat(policy.data() + 6, 99.0f);
  auto response = request(f, 0, 1, policy);
  assert(response.size() == 1 && response[0][0] == 0x16 &&
         response[0][1] == 0xa3 && response[0][3] == 0 &&
         queued_tx::getFloat(response[0].data() + 4) == 99.0f);
  auto port1Stats = request(f, 0, 1, {queued_tx::STATS, 1});
  auto port2Stats = request(f, 0, 2, {queued_tx::STATS, 1});
  assert(port1Stats.size() == 1 && port2Stats.size() == 1 &&
         queued_tx::get32(port1Stats[0].data() + 23) <
             queued_tx::get32(port2Stats[0].data() + 23));

  std::vector<uint8_t> role{queued_tx::ROLE_PRESENCE, 1, 1};
  role.resize(35);
  role[3] = 0x42;
  auto presence = request(f, 0, 1, role);
  assert(presence.size() == 1 && presence[0][1] == 0xa6 &&
         presence[0][9] == 0);
  presence = request(f, 0, 2, role);
  assert(presence.size() == 1 &&
         presence[0][9] == (queued_tx::TCP_ROLE_PRESENT |
                            queued_tx::TCP_KEY_PRESENT));
  presence = request(f, 1, 0, role);
  assert(presence.size() == 1 &&
         presence[0][9] == (queued_tx::TCP_ROLE_PRESENT |
                            queued_tx::TCP_KEY_PRESENT));

  RadioDashboard::Snapshot snapshot{};
  f.mux.dashboardStatus(snapshot.radio);
  assert(snapshot.radio.clients == 2 && snapshot.radio.session.connected &&
         snapshot.radio.session.physical_slot == 0 &&
         snapshot.radio.session.port[0].generation == generations[1]);
  char json[RadioDashboard::JSON_CAPACITY];
  assert(RadioDashboard::formatJSON(snapshot, "mast", json, sizeof(json)));
  assert(strstr(json, "\"session\":{\"physical_slot\":0,\"ports\":[") &&
         strstr(json, "\"port\":3,\"connected\":true"));

  const auto disable = request(f, 0, 2, {HW_CMD_SET_SIGNAL_REPORT, 0});
  assert(disable == std::vector<std::vector<uint8_t>>({{0x26, 0x9a, 0}}));
  const auto data = encode(0, {0x44, 0xc0, 0xdb});
  const auto meta = encode(6, {HW_RESP_RX_META, 3, 0xa0});
  f.mux.write(data.data(), data.size());
  f.mux.write(meta.data(), meta.size());
  auto frames = receive(f.peers[0]);
  for (uint8_t port = 0; port < 4; ++port) {
    std::vector<std::vector<uint8_t>> received;
    for (const auto& frame : frames)
      if ((frame[0] >> 4) == port) received.push_back(frame);
    assert(received.size() == (port == 2 ? 1u : 2u));
    assert(received[0] == std::vector<uint8_t>(
        {(uint8_t)(port << 4), 0x44, 0xc0, 0xdb}));
    if (port != 2)
      assert(received[1] ==
             std::vector<uint8_t>({(uint8_t)(port << 4 | 6),
                                   HW_RESP_RX_META, 3, 0xa0}));
  }
  assert(receive(f.peers[1]) ==
         std::vector<std::vector<uint8_t>>({{0, 0x44, 0xc0, 0xdb},
                                             {6, HW_RESP_RX_META, 3, 0xa0}}));

  f.radio.busy = true;
  assert(hasEvent(request(f, 0, 1, submitPacket(generations[1], 1, 0x51)),
                  1, generations[1], 1, queued_tx::ACCEPTED));
  assert(hasEvent(request(f, 0, 2, submitPacket(generations[2], 1, 0x52)),
                  2, generations[2], 1, queued_tx::ACCEPTED));
  f.radio.busy = false;
  clock_ms += 500;
  f.step();
  assert(f.radio.transmitted.size() == 1 &&
         f.radio.transmitted[0] == std::vector<uint8_t>{0x51});
  f.finish();
  frames = receive(f.peers[0]);
  assert(hasEvent(frames, 1, generations[1], 1, queued_tx::SUCCEEDED));
  for (const auto& frame : frames)
    assert(frame != std::vector<uint8_t>({0x10, 0x51}));
  bool reflected = false;
  for (const auto& frame : frames)
    if (frame == std::vector<uint8_t>({0x20, 0x51})) reflected = true;
  assert(reflected);
  assert(receive(f.peers[1])[0] == std::vector<uint8_t>({0, 0x51}));
  f.finish();
  frames = receive(f.peers[0]);
  assert(hasEvent(frames, 2, generations[2], 1, queued_tx::SUCCEEDED));
  assert(f.radio.transmitted.size() == 2 &&
         f.radio.transmitted[1] == std::vector<uint8_t>{0x52});
  receive(f.peers[1]);

  f.radio.busy = true;
  assert(hasEvent(request(f, 0, 3, submitPacket(generations[3], 1, 0x53)),
                  3, generations[3], 1, queued_tx::ACCEPTED));
  const uint32_t renewed = hello(f, 3);
  assert(renewed != generations[3]);
  f.radio.busy = false;
  clock_ms += 500;
  f.step();
  assert(f.radio.transmitted.size() == 2);
  assert(request(f, 0, 3, {queued_tx::CONFIG, 1, 0})[0][3] == 0);
  assert(hasEvent(request(f, 0, 3, submitPacket(generations[3], 2, 0x54)),
                  3, generations[3], 2, queued_tx::REJECTED,
                  queued_tx::STALE));
  assert(hasEvent(request(f, 0, 3, submitPacket(renewed, 1, 0x55)),
                  3, renewed, 1, queued_tx::ACCEPTED));
  assert(f.radio.sending);
  close(f.peers[0]);
  f.peers[0] = -1;
  f.step();
  assert(f.mux.clientCount() == 1);
  f.finish();
  assert(f.radio.transmitted.size() == 3 &&
         f.radio.transmitted[2] == std::vector<uint8_t>{0x55});
  frames = receive(f.peers[1]);
  bool directReflection = false;
  for (const auto& frame : frames)
    if (frame == std::vector<uint8_t>({0, 0x55})) directReflection = true;
  assert(directReflection);
  int reconnect[2];
  assert(tcpPair(AF_UNIX, SOCK_STREAM, 0, reconnect) == 0);
  f.peers[0] = reconnect[1];
  f.server.add(WiFiClient(reconnect[0]));
  f.step();
  const auto nextOwner = request(f, 0, 0, {queued_tx::HELLO, 1, 1});
  assert(nextOwner.size() == 1 && nextOwner[0][3] == 0 &&
         queued_tx::get32(nextOwner[0].data() + 4) != generations[0]);
  assert(request(f, 0, 0, {queued_tx::CAPACITY, 1, 1}) ==
         std::vector<std::vector<uint8_t>>({{6, 0xa5, 1, 4, 4, 4, 1}}));
  assert(hello(f, 3) != renewed);
  assert(receive(f.peers[0]).empty());
}

static void outputPressureAndReconnect() {
  nvs_test::reset();
  Radio radio;
  Board board;
  RNG rng;
  mesh::LocalIdentity identity;
  SensorManager sensors;
  WifiKissMultiplexer mux;
  KissModem modem{mux, identity, rng, radio, board, sensors};
  WiFiServer server;
  mux.attachRadio(radio, rng, configure, power);
  assert(mux.setInitialConfiguration({910525000, 62500, 7, 5, 20}, true));
  modem.begin();
  SessionPeer session(server);
  SessionPeer direct(server);
  auto step = [&]() { mux.poll(server); modem.loop(); mux.afterModemLoop(); };
  auto command = [&](SessionPeer& peer, uint8_t port,
                     const std::vector<uint8_t>& payload) {
    peer.sendInput(encode((port << 4) | 6, payload));
    step();
    return receive(peer.remote);
  };
  step();
  auto owner = command(session, 0, {queued_tx::HELLO, 1, 1});
  const uint32_t firstOwner = queued_tx::get32(owner[0].data() + 4);
  auto socketOption = [](SessionPeer& peer, int level, int option) {
    int value = -1;
    socklen_t length = sizeof(value);
    assert(getsockopt(peer.socket.fd(), level, option, &value, &length) == 0 &&
           length == sizeof(value));
    return value;
  };
  assert(socketOption(session, SOL_SOCKET, SO_KEEPALIVE) == 0);
  assert(socketOption(direct, SOL_SOCKET, SO_KEEPALIVE) == 0);
  assert(command(session, 0, {queued_tx::CAPACITY, 1, 1}) ==
         std::vector<std::vector<uint8_t>>({{6, 0xa5, 1, 4, 4, 4, 1}}));
  assert(socketOption(session, SOL_SOCKET, SO_KEEPALIVE) == 1);
  assert(socketOption(session, IPPROTO_TCP, TCP_KEEPIDLE) == 45);
  assert(socketOption(session, IPPROTO_TCP, TCP_KEEPINTVL) == 10);
  assert(socketOption(session, IPPROTO_TCP, TCP_KEEPCNT) == 3);
  assert(socketOption(direct, SOL_SOCKET, SO_KEEPALIVE) == 0);
  assert(command(session, 1, {HW_CMD_SET_SIGNAL_REPORT, 1}) ==
         std::vector<std::vector<uint8_t>>({{0x16, 0x9a, 1}}));
  auto child = command(session, 1, {queued_tx::HELLO, 1, 0});
  const uint32_t firstChild = queued_tx::get32(child[0].data() + 4);
  assert(command(session, 1, {queued_tx::CONFIG, 1, 0})[0][3] == 0);

  // Child replies fill more than half the physical ring while the peer stalls.
  // Port 0 must still have room for its own CONFIG reply.
  const size_t filler = session.blockOutput();
  for (int i = 0; i < 73; ++i) {
    session.sendInput(encode(0x16, {queued_tx::CONFIG, 1, 0}));
    step();
    assert(mux.clientCount() == 2);
  }
  session.sendInput(encode(6, {queued_tx::CONFIG, 1, 0}));
  step();
  assert(mux.clientCount() == 2);
  size_t unread = filler;
  uint8_t bytes[2048];
  while (unread) {
    const ssize_t n = recv(session.remote, bytes, std::min(unread, sizeof(bytes)), 0);
    assert(n > 0);
    for (ssize_t i = 0; i < n; ++i) assert(bytes[i] == 0);
    unread -= n;
  }
  std::vector<uint8_t> wire;
  for (int attempts = 0; attempts < 200; ++attempts) {
    step();
    for (;;) {
      const ssize_t n = recv(session.remote, bytes, sizeof(bytes), MSG_DONTWAIT);
      if (n <= 0) break;
      wire.insert(wire.end(), bytes, bytes + n);
    }
    if (std::count(wire.begin(), wire.end(), KISS_FEND) >= 2 * 74)
      break;
  }
  assert(std::count(wire.begin(), wire.end(), KISS_FEND) == 2 * 74);
  const std::vector<uint8_t> ownerReply{KISS_FEND, 6, 0xa2, 1, 0};
  assert(std::search(wire.begin(), wire.end(), ownerReply.begin(),
                     ownerReply.end()) != wire.end());
  assert(mux.clientCount() == 2);

  radio.busy = true;
  assert(hasEvent(command(session, 1, submitPacket(firstChild, 1, 0x61)),
                  1, firstChild, 1, queued_tx::ACCEPTED));

  session.blockOutput();
  bool retired = false;
  for (int i = 0; i < 3000 && !retired; ++i) {
    session.sendInput(encode(0x16, {queued_tx::STATS, 1}));
    step();
    retired = mux.clientCount() == 1;
  }
  assert(retired && radio.transmitted.empty());
  assert(command(direct, 0, {queued_tx::HELLO, 1, 0})[0][3] == 0);
  SessionPeer replacement(server);
  step();
  owner = command(replacement, 0, {queued_tx::HELLO, 1, 1});
  assert(queued_tx::get32(owner[0].data() + 4) != firstOwner);
  assert(command(replacement, 0, {queued_tx::CAPACITY, 1, 1})[0] ==
         std::vector<uint8_t>({6, 0xa5, 1, 4, 4, 4, 1}));
  assert(command(replacement, 1, {HW_CMD_SET_SIGNAL_REPORT, 1}) ==
         std::vector<std::vector<uint8_t>>({{0x16, 0x9a, 1}}));
  child = command(replacement, 1, {queued_tx::HELLO, 1, 0});
  const uint32_t nextChild = queued_tx::get32(child[0].data() + 4);
  assert(nextChild != firstChild);
  assert(command(replacement, 1, {queued_tx::CONFIG, 1, 0})[0][3] == 0);
  assert(hasEvent(command(replacement, 1,
                          submitPacket(firstChild, 2, 0x62)),
                  1, firstChild, 2, queued_tx::REJECTED, queued_tx::STALE));
  assert(hasEvent(command(replacement, 1, submitPacket(nextChild, 1, 0x63)),
                  1, nextChild, 1, queued_tx::ACCEPTED));
  radio.busy = false;
  clock_ms += 500;
  step();
  assert(radio.transmitted ==
         std::vector<std::vector<uint8_t>>({{0x63}}));
  clock_ms += 100;
  radio.complete = true;
  step();
  assert(hasEvent(receive(replacement.remote), 1, nextChild, 1,
                  queued_tx::SUCCEEDED));
}

static void serveGo() {
  nvs_test::reset();
  Radio radio;
  Board board;
  RNG rng;
  mesh::LocalIdentity identity;
  SensorManager sensors;
  WifiKissMultiplexer mux;
  KissModem modem{mux, identity, rng, radio, board, sensors};
  WiFiServer server;
  mux.attachRadio(radio, rng, configure, power);
  assert(mux.setInitialConfiguration({910525000, 62500, 7, 5, 20}, true));
  modem.begin();
  const int listener = socket(AF_INET, SOCK_STREAM, 0);
  assert(listener >= 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  assert(bind(listener, reinterpret_cast<sockaddr*>(&address),
              sizeof(address)) == 0);
  assert(listen(listener, 8) == 0);
  socklen_t length = sizeof(address);
  assert(getsockname(listener, reinterpret_cast<sockaddr*>(&address),
                     &length) == 0);
  assert(fcntl(listener, F_SETFL, O_NONBLOCK) == 0);
  printf("MKISS_PORT=%u\n", ntohs(address.sin_port));
  fflush(stdout);
  for (;;) {
    const int fd = accept(listener, nullptr, nullptr);
    if (fd >= 0)
      server.add(WiFiClient(fd));
    else
      assert(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR);
    mux.poll(server);
    modem.loop();
    mux.afterModemLoop();
    usleep(1000);
  }
}

int main(int argc, char** argv) {
  if (argc == 2 && strcmp(argv[1], "--serve") == 0) serveGo();
  if (argc == 2 && strcmp(argv[1], "--keepalive") == 0) {
    capacityAndIsolation(true);
    puts("MKISS quiet TCP keepalive and session lifecycle passed");
    return 0;
  }
  capacityAndIsolation();
  outputPressureAndReconnect();
  puts("MKISS v1 capacity, isolation, RX, TX and reconnect passed");
}
