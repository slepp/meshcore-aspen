#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

#include "WifiKissMultiplexer.h"

unsigned long millis() { return 0; }

static std::vector<uint8_t> receiveAvailable(int fd, size_t expected = 0) {
  std::vector<uint8_t> data;
  uint8_t buffer[2048];
  for (int attempt = 0; attempt < 100; ++attempt) {
    const ssize_t count = recv(fd, buffer, sizeof(buffer), MSG_DONTWAIT);
    if (count > 0) data.insert(data.end(), buffer, buffer + count);
    if ((expected == 0 && count <= 0) || (expected > 0 && data.size() >= expected)) break;
    usleep(1000);
  }
  return data;
}

static std::vector<uint8_t> drainInput(WifiKissMultiplexer& mux) {
  std::vector<uint8_t> data;
  while (mux.available() > 0) data.push_back(static_cast<uint8_t>(mux.read()));
  return data;
}

static void testRouting() {
  int first[2];
  int second[2];
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, first) == 0);
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, second) == 0);

  WiFiServer server;
  server.add(WiFiClient(first[0]));
  server.add(WiFiClient(second[0]));
  WifiKissMultiplexer mux;
  mux.poll(server);
  assert(mux.clientCount() == 2);

  const uint8_t ping[] = {0xC0, 0x06, 0x17, 0xC0};
  assert(send(first[1], ping, sizeof(ping), 0) == sizeof(ping));
  mux.poll(server);
  assert(drainInput(mux) == std::vector<uint8_t>(ping, ping + sizeof(ping)));
  assert(send(second[1], ping, sizeof(ping), 0) == sizeof(ping));
  mux.poll(server);
  assert(mux.available() == 0);
  const uint8_t pong[] = {0xC0, 0x06, 0x97, 0xC0};
  mux.write(pong, sizeof(pong));
  assert(receiveAvailable(first[1], sizeof(pong)) ==
         std::vector<uint8_t>(pong, pong + sizeof(pong)));
  assert(receiveAvailable(second[1]).empty());
  assert(drainInput(mux) == std::vector<uint8_t>(ping, ping + sizeof(ping)));
  mux.write(pong, sizeof(pong));
  assert(receiveAvailable(second[1], sizeof(pong)) ==
         std::vector<uint8_t>(pong, pong + sizeof(pong)));
  assert(receiveAvailable(first[1]).empty());

  const uint8_t air_data[] = {0xC0, 0x00, 0x09, 0xAA, 0xC0};
  const uint8_t air_meta[] = {0xC0, 0x06, 0xF9, 0x28, 0xA0, 0xC0};
  mux.write(air_data, sizeof(air_data));
  mux.write(air_meta, sizeof(air_meta));
  std::vector<uint8_t> air_expected(air_data, air_data + sizeof(air_data));
  air_expected.insert(air_expected.end(), air_meta, air_meta + sizeof(air_meta));
  assert(receiveAvailable(first[1], air_expected.size()) == air_expected);
  assert(receiveAvailable(second[1], air_expected.size()) == air_expected);

  const uint8_t data[] = {0xC0, 0x00, 0x3C, 0x01, 0x02, 0xC0};
  assert(send(first[1], data, sizeof(data), 0) == sizeof(data));
  mux.poll(server);
  assert(drainInput(mux) == std::vector<uint8_t>(data, data + sizeof(data)));

  const uint8_t tx_done[] = {0xC0, 0x06, 0xF8, 0x01, 0xC0};
  mux.write(tx_done, sizeof(tx_done));
  assert(receiveAvailable(first[1], sizeof(tx_done)) ==
         std::vector<uint8_t>(tx_done, tx_done + sizeof(tx_done)));

  const uint8_t local_meta[] = {0xC0, 0x06, 0xF9, 0x80, 0x7F, 0xC0};
  std::vector<uint8_t> expected(data, data + sizeof(data));
  expected.insert(expected.end(), local_meta, local_meta + sizeof(local_meta));
  assert(receiveAvailable(second[1], expected.size()) == expected);
  assert(receiveAvailable(first[1]).empty());

  assert(send(second[1], data, sizeof(data), 0) == sizeof(data));
  mux.poll(server);
  assert(drainInput(mux) == std::vector<uint8_t>(data, data + sizeof(data)));
  const uint8_t tx_failed[] = {0xC0, 0x06, 0xF8, 0x00, 0xC0};
  mux.write(tx_failed, sizeof(tx_failed));
  assert(receiveAvailable(second[1], sizeof(tx_failed)) ==
         std::vector<uint8_t>(tx_failed, tx_failed + sizeof(tx_failed)));
  assert(receiveAvailable(first[1]).empty());

  const uint8_t disable_signal[] = {0xC0, 0x06, 0x19, 0x00, 0xC0};
  assert(send(second[1], disable_signal, sizeof(disable_signal), 0) ==
         sizeof(disable_signal));
  mux.poll(server);
  assert(mux.available() == 0);
  const uint8_t disabled[] = {0xC0, 0x06, 0x9A, 0x00, 0xC0};
  assert(receiveAvailable(second[1], sizeof(disabled)) ==
         std::vector<uint8_t>(disabled, disabled + sizeof(disabled)));

  mux.write(air_data, sizeof(air_data));
  mux.write(air_meta, sizeof(air_meta));
  assert(receiveAvailable(first[1], air_expected.size()) == air_expected);
  assert(receiveAvailable(second[1], sizeof(air_data)) ==
         std::vector<uint8_t>(air_data, air_data + sizeof(air_data)));
  assert(send(first[1], data, sizeof(data), 0) == sizeof(data));
  mux.poll(server);
  assert(drainInput(mux) == std::vector<uint8_t>(data, data + sizeof(data)));
  mux.write(tx_done, sizeof(tx_done));
  assert(receiveAvailable(first[1], sizeof(tx_done)) ==
         std::vector<uint8_t>(tx_done, tx_done + sizeof(tx_done)));
  assert(receiveAvailable(second[1], sizeof(data)) ==
         std::vector<uint8_t>(data, data + sizeof(data)));

  std::vector<uint8_t> oversized = {0xC0, 0x00};
  oversized.insert(oversized.end(), 256, 0x01);
  oversized.push_back(0xC0);
  assert(send(first[1], oversized.data(), oversized.size(), 0) ==
         static_cast<ssize_t>(oversized.size()));
  mux.poll(server);
  assert(mux.available() == 0);
  const uint8_t invalid_length[] = {0xC0, 0x06, 0xF1, 0x01, 0xC0};
  assert(receiveAvailable(first[1], sizeof(invalid_length)) ==
         std::vector<uint8_t>(invalid_length,
                              invalid_length + sizeof(invalid_length)));

  close(first[1]);
  close(second[1]);
}

struct Peer {
  WiFiClient socket;
  int remote;

  explicit Peer(WiFiServer& server) {
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    socket = WiFiClient(sockets[0]);
    remote = sockets[1];
    server.add(socket);
  }
  ~Peer() {
    disconnect();
    socket.stop();
  }
  void disconnect() {
    if (remote >= 0) close(remote);
    remote = -1;
  }
  void sendInput(const std::vector<uint8_t>& bytes) {
    assert(send(remote, bytes.data(), bytes.size(), MSG_DONTWAIT) ==
           static_cast<ssize_t>(bytes.size()));
  }
  size_t blockOutput() {
    const int small_buffer = 4096;
    assert(setsockopt(socket.fd(), SOL_SOCKET, SO_SNDBUF,
                      &small_buffer, sizeof(small_buffer)) == 0);
    uint8_t filler[1024] = {};
    size_t total = 0;
    for (;;) {
      const ssize_t count = send(socket.fd(), filler, sizeof(filler), MSG_DONTWAIT);
      if (count < 0) {
        assert(errno == EAGAIN || errno == EWOULDBLOCK);
        return total;
      }
      assert(count > 0);
      total += count;
    }
  }
};

static void writeOutput(WifiKissMultiplexer& mux, const std::vector<uint8_t>& frame) {
  assert(mux.write(frame.data(), frame.size()) == frame.size());
}

static const std::vector<uint8_t> ping = {0xC0, 0x06, 0x17, 0xC0};
static const std::vector<uint8_t> pong = {0xC0, 0x06, 0x97, 0xC0};
static const std::vector<uint8_t> packet = {0xC0, 0x00, 0xDB, 0xDC, 0xDB, 0xDD, 0xC0};
static const std::vector<uint8_t> tx_done = {0xC0, 0x06, 0xF8, 0x01, 0xC0};
static const std::vector<uint8_t> local_meta = {0xC0, 0x06, 0xF9, 0x80, 0x7F, 0xC0};

static void testBackpressureRecoveryAndOverflow() {
  WiFiServer server;
  Peer slow(server);
  Peer fast(server);
  WifiKissMultiplexer mux;
  mux.poll(server);

  // Maximum upstream hardware reply: command + subcommand + 512-byte payload.
  std::vector<uint8_t> reply = {0xC0, 0x06, 0x88};
  for (int i = 0; i < 512; ++i) {
    reply.push_back(0xDB);
    reply.push_back(0xDC);
  }
  reply.push_back(0xC0);

  // Repetition crosses the byte ring's end, including an escaped frame boundary.
  for (int round = 0; round < 3; ++round) {
    const size_t filler = slow.blockOutput();
    std::vector<uint8_t> expected;
    for (int i = 0; i < 2; ++i) {
      writeOutput(mux, reply);
      expected.insert(expected.end(), reply.begin(), reply.end());
      assert(receiveAvailable(fast.remote, reply.size()) == reply);
    }
    mux.flush();
    mux.poll(server);
    assert(mux.clientCount() == 2);
    fast.sendInput(ping);
    mux.poll(server);
    assert(drainInput(mux) == ping);
    writeOutput(mux, pong);
    assert(receiveAvailable(fast.remote, pong.size()) == pong);
    assert(receiveAvailable(slow.remote, filler) == std::vector<uint8_t>(filler, 0));
    std::vector<uint8_t> actual;
    for (int poll = 0; poll < 20 && actual.size() < expected.size(); ++poll) {
      mux.poll(server);
      const auto bytes = receiveAvailable(slow.remote);
      actual.insert(actual.end(), bytes.begin(), bytes.end());
    }
    assert(actual == expected);
  }

  slow.blockOutput();
  for (int i = 0; i < 3; ++i) {
    writeOutput(mux, reply);
    assert(receiveAvailable(fast.remote, reply.size()) == reply);
  }
  assert(mux.clientCount() == 1);
  assert(slow.socket.fd() == -1);
  fast.sendInput(ping);
  mux.poll(server);
  assert(drainInput(mux) == ping);
  writeOutput(mux, pong);
  assert(receiveAvailable(fast.remote, pong.size()) == pong);

  Peer replacement(server);
  mux.poll(server);
  assert(mux.clientCount() == 2);
  assert(receiveAvailable(replacement.remote).empty());
  writeOutput(mux, packet);
  assert(receiveAvailable(replacement.remote, packet.size()) == packet);
  assert(receiveAvailable(fast.remote, packet.size()) == packet);
}

static void testDisconnectedTransactionOwnership() {
  WiFiServer server;
  Peer old(server);
  Peer other(server);
  WifiKissMultiplexer mux;
  mux.poll(server);
  old.sendInput({0xC0, 0x06, 0x19, 0x00, 0xC0});
  mux.poll(server);
  assert(receiveAvailable(old.remote, 5) ==
         std::vector<uint8_t>({0xC0, 0x06, 0x9A, 0x00, 0xC0}));
  old.sendInput(packet);
  mux.poll(server);
  assert(drainInput(mux) == packet);
  for (int i = 0; i < KISS_REQUEST_QUEUE_DEPTH; ++i) old.sendInput(ping);
  mux.poll(server);
  old.disconnect();
  mux.poll(server);
  assert(mux.clientCount() == 1);
  Peer replacement(server);
  mux.poll(server);
  replacement.sendInput(ping);
  other.sendInput(ping);
  mux.poll(server);
  assert(mux.available() == 0);
  assert(receiveAvailable(replacement.remote).empty());
  assert(receiveAvailable(other.remote).empty());

  writeOutput(mux, tx_done);
  auto reflected = packet;
  reflected.insert(reflected.end(), local_meta.begin(), local_meta.end());
  assert(receiveAvailable(replacement.remote, reflected.size()) == reflected);
  assert(receiveAvailable(other.remote, reflected.size()) == reflected);
  for (int i = 0; i < 2; ++i) {
    assert(drainInput(mux) == ping);
    writeOutput(mux, pong);
  }
  assert(receiveAvailable(replacement.remote, pong.size()) == pong);
  assert(receiveAvailable(other.remote, pong.size()) == pong);
  assert(mux.available() == 0);

  // A request selected by available(), but never read, must not reach the modem
  // after its connection dies.
  replacement.sendInput(packet);
  mux.poll(server);
  assert(mux.available() == static_cast<int>(packet.size()));
  replacement.disconnect();
  mux.poll(server);
  other.sendInput(ping);
  mux.poll(server);
  assert(drainInput(mux) == ping);
  writeOutput(mux, pong);
  assert(receiveAvailable(other.remote, pong.size()) == pong);
}

static void testFramingAndInputBudget() {
  WiFiServer server;
  Peer noisy(server);
  Peer other(server);
  WifiKissMultiplexer mux;
  mux.poll(server);
  std::vector<uint8_t> noise(4096, 0x42);
  noise.insert(noise.end(), ping.begin(), ping.end());
  noisy.sendInput(noise);
  other.sendInput(packet);
  mux.poll(server);
  assert(noisy.socket.available() > 0);
  assert(drainInput(mux) == packet);
  writeOutput(mux, tx_done);
  assert(receiveAvailable(other.remote, tx_done.size()) == tx_done);
  auto reflected = packet;
  reflected.insert(reflected.end(), local_meta.begin(), local_meta.end());
  assert(receiveAvailable(noisy.remote, reflected.size()) == reflected);
  for (int i = 0; i < 10; ++i) mux.poll(server);
  assert(drainInput(mux) == ping);
  writeOutput(mux, pong);
  assert(receiveAvailable(noisy.remote, pong.size()) == pong);

  const std::vector<std::vector<uint8_t>> invalid = {
      {0xC0, 0x06, 0x17, 0xDB, 0x01, 0xC0},
      {0xC0, 0x06, 0x17, 0xDB, 0xC0},
      std::vector<uint8_t>(KISS_MAX_ENCODED_FRAME_SIZE + 20, 0x41),
      std::vector<uint8_t>(KISS_MAX_FRAME_SIZE + 1, 0x41),
  };
  for (auto bytes : invalid) {
    if (bytes.front() != 0xC0) {
      bytes.insert(bytes.begin(), 0xC0);
      bytes.push_back(0xC0);
    }
    bytes.insert(bytes.end(), ping.begin(), ping.end());
    noisy.sendInput(bytes);
    for (int i = 0; i < 4; ++i) mux.poll(server);
    assert(drainInput(mux) == ping);
    writeOutput(mux, pong);
    assert(receiveAvailable(noisy.remote, pong.size()) == pong);
    assert(mux.available() == 0);
  }

  noisy.sendInput({0xC0, 0x00, 0xDB});
  mux.poll(server);
  assert(mux.available() == 0);
  noisy.sendInput({0xDC, 0xDB, 0xDD, 0xC0});
  mux.poll(server);
  assert(drainInput(mux) == packet);
  const std::vector<uint8_t> failed = {0xC0, 0x06, 0xF8, 0x00, 0xC0};
  writeOutput(mux, failed);
  assert(receiveAvailable(noisy.remote, failed.size()) == failed);
  assert(receiveAvailable(other.remote).empty());

  // Ordinary settings, unknown commands and nonzero ports do not reply upstream.
  for (const std::vector<uint8_t>& ignored :
       std::vector<std::vector<uint8_t>>{{0xC0, 0x01, 0x02, 0xC0},
                                        {0xC0, 0x16, 0x17, 0xC0},
                                        {0xC0, 0x06, 0xC0},
                                        {0xC0, 0xFF, 0xC0}}) {
    noisy.sendInput(ignored);
    mux.poll(server);
    assert(drainInput(mux) == ignored);
    mux.afterModemLoop();
  }
  noisy.sendInput(ping);
  mux.poll(server);
  assert(drainInput(mux) == ping);
  writeOutput(mux, pong);
  assert(receiveAvailable(noisy.remote, pong.size()) == pong);
}

static void testRequestQueueBoundAndErrors() {
  WiFiServer server;
  Peer source(server);
  Peer observer(server);
  WifiKissMultiplexer mux;
  mux.poll(server);
  source.sendInput(packet);
  mux.poll(server);
  assert(drainInput(mux) == packet);
  for (int i = 0; i < KISS_REQUEST_QUEUE_DEPTH + 1; ++i) source.sendInput(ping);
  mux.poll(server);
  const std::vector<uint8_t> busy = {0xC0, 0x06, 0xF1, 0x07, 0xC0};
  assert(receiveAvailable(source.remote, busy.size()) == busy);
  assert(mux.available() == 0);
  // Rejected DATA does not get reflected and releases transaction ownership.
  writeOutput(mux, busy);
  assert(receiveAvailable(source.remote, busy.size()) == busy);
  assert(receiveAvailable(observer.remote).empty());
  for (int i = 0; i < KISS_REQUEST_QUEUE_DEPTH; ++i) {
    assert(drainInput(mux) == ping);
    writeOutput(mux, pong);
    assert(receiveAvailable(source.remote, pong.size()) == pong);
  }
  assert(mux.available() == 0);
}

static void testDisconnectPrunesMixedQueue() {
  WiFiServer server;
  Peer old(server);
  Peer retained(server);
  WifiKissMultiplexer mux;
  mux.poll(server);
  old.sendInput(packet);
  mux.poll(server);
  assert(mux.read() == KISS_FEND);
  for (int i = 0; i < KISS_REQUEST_QUEUE_DEPTH / 2; ++i) {
    old.sendInput(ping);
    mux.poll(server);
    retained.sendInput({0xC0, 0x01, static_cast<uint8_t>(i), 0xC0});
    mux.poll(server);
  }
  old.disconnect();
  mux.poll(server);
  assert(drainInput(mux) == std::vector<uint8_t>(packet.begin() + 1, packet.end()));
  assert(mux.available() == 0);
  writeOutput(mux, tx_done);
  auto reflected = packet;
  reflected.insert(reflected.end(), local_meta.begin(), local_meta.end());
  assert(receiveAvailable(retained.remote, reflected.size()) == reflected);
  Peer replacement(server);
  mux.poll(server);
  replacement.sendInput(ping);
  mux.poll(server);
  for (int i = 0; i < KISS_REQUEST_QUEUE_DEPTH / 2; ++i) {
    assert(drainInput(mux) ==
           std::vector<uint8_t>({0xC0, 0x01, static_cast<uint8_t>(i), 0xC0}));
    mux.afterModemLoop();
  }
  assert(drainInput(mux) == ping);
  writeOutput(mux, pong);
  assert(receiveAvailable(replacement.remote, pong.size()) == pong);
  assert(receiveAvailable(retained.remote).empty());
  assert(mux.available() == 0);
}

static void testSignalCommandContract() {
  WiFiServer server;
  Peer source(server);
  Peer other(server);
  WifiKissMultiplexer mux;
  mux.poll(server);
  for (const auto& request : std::vector<std::vector<uint8_t>>{
           {0xC0, 0x06, 0x19, 0x00, 0x42, 0xC0},
           {0xC0, 0x06, 0x1A, 0x42, 0xC0}}) {
    source.sendInput(request);
    mux.poll(server);
    assert(mux.available() == 0);
    assert(receiveAvailable(source.remote, 5) ==
           std::vector<uint8_t>({0xC0, 0x06, 0x9A, 0x00, 0xC0}));
    assert(receiveAvailable(other.remote).empty());
  }
  source.sendInput({0xC0, 0x06, 0x19, 0xC0});
  mux.poll(server);
  assert(receiveAvailable(source.remote, 5) ==
         std::vector<uint8_t>({0xC0, 0x06, 0xF1, 0x01, 0xC0}));
  assert(mux.available() == 0);
  source.sendInput({0xC0, 0x06, 0x19, 0x02, 0xC0});
  mux.poll(server);
  assert(receiveAvailable(source.remote, 5) ==
         std::vector<uint8_t>({0xC0, 0x06, 0x9A, 0x01, 0xC0}));
}

int main() {
  testRouting();
  testBackpressureRecoveryAndOverflow();
  testDisconnectedTransactionOwnership();
  testFramingAndInputBudget();
  testRequestQueueBoundAndErrors();
  testDisconnectPrunesMixedQueue();
  testSignalCommandContract();
  std::puts("WiFi KISS multiplexer behavioural tests passed");
}
