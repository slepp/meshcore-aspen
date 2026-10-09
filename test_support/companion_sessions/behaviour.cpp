// SPDX-License-Identifier: Apache-2.0
#include "CompanionSessions.h"

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

using onchip::CompanionSessions;
using Bytes = std::vector<uint8_t>;
using Clock = std::chrono::steady_clock;

#define REQUIRE(test) do { if (!(test)) throw std::runtime_error(std::string(__func__) + ":" + std::to_string(__LINE__) + ": " #test); } while (0)

static std::atomic<unsigned> sendChunk{0};
static std::atomic<unsigned> blockedPort{0};
static std::atomic<unsigned> shortWrites{0};
static bool failJournalAllocation;
static unsigned journalAllocations;
static size_t journalBytes;

extern "C" void* __real__ZnwmRKSt9nothrow_t(size_t, const std::nothrow_t&) noexcept;
extern "C" void* __wrap__ZnwmRKSt9nothrow_t(size_t size, const std::nothrow_t& tag) noexcept {
  ++journalAllocations;
  journalBytes = size;
  if (failJournalAllocation) return nullptr;
  return __real__ZnwmRKSt9nothrow_t(size, tag);
}

extern "C" ssize_t __real_send(int, const void*, size_t, int);
extern "C" ssize_t __wrap_send(int fd, const void* data, size_t size, int flags) {
  if (flags & MSG_DONTWAIT) {
    sockaddr_in peer{};
    socklen_t length = sizeof(peer);
    if (blockedPort.load() && getpeername(fd, reinterpret_cast<sockaddr*>(&peer), &length) == 0 &&
        ntohs(peer.sin_port) == blockedPort.load()) {
      errno = EAGAIN;
      return -1;
    }
    const unsigned chunk = sendChunk.load();
    if (chunk && size > chunk) { size = chunk; ++shortWrites; }
  }
  return __real_send(fd, data, size, flags);
}

template<class Condition> void until(Condition condition) {
  const auto deadline = Clock::now() + std::chrono::seconds(3);
  while (!condition()) {
    REQUIRE(Clock::now() < deadline);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

class Client {
  int fd;
  void readAll(uint8_t* data, size_t size) {
    while (size) {
      pollfd p{fd, POLLIN, 0};
      REQUIRE(poll(&p, 1, 3000) > 0);
      const int n = recv(fd, data, size, 0);
      REQUIRE(n > 0);
      data += n;
      size -= n;
    }
  }
public:
  explicit Client(uint16_t port) : fd(socket(AF_INET, SOCK_STREAM, 0)) {
    REQUIRE(fd >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    REQUIRE(connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
  }
  ~Client() { disconnect(); }
  void disconnect() { if (fd >= 0) close(fd); fd = -1; }
  unsigned localPort() const {
    sockaddr_in address{};
    socklen_t length = sizeof(address);
    REQUIRE(getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) == 0);
    return ntohs(address.sin_port);
  }
  void raw(const Bytes& bytes) {
    size_t offset = 0;
    while (offset < bytes.size()) {
      const int n = send(fd, bytes.data() + offset, bytes.size() - offset, MSG_NOSIGNAL);
      REQUIRE(n > 0);
      offset += n;
    }
  }
  void command(const Bytes& data) {
    Bytes frame{'<', uint8_t(data.size()), uint8_t(data.size() >> 8)};
    frame.insert(frame.end(), data.begin(), data.end());
    raw(frame);
  }
  Bytes frame() {
    uint8_t header[3];
    readAll(header, sizeof(header));
    REQUIRE(header[0] == '>');
    const size_t size = header[1] | (unsigned(header[2]) << 8);
    REQUIRE(size > 0 && size <= MAX_FRAME_SIZE);
    Bytes result(size);
    readAll(result.data(), result.size());
    return result;
  }
  Bytes response() {
    for (unsigned i = 0; i < 128; ++i) {
      Bytes result = frame();
      if (result[0] < 0x80) return result;
    }
    throw std::runtime_error("too many pushes before response");
  }
  void handshake(uint8_t version = 3) {
    command({22, version});
    REQUIRE(response()[0] == 13);
    command({1, 3, 0, 0, 0, 0, 0, 0});
    REQUIRE(response()[0] == 5);
  }
  Bytes sync() { command({10}); return response(); }
  void closed() {
    pollfd p{fd, POLLIN, 0};
    REQUIRE(poll(&p, 1, 3000) > 0);
    uint8_t byte;
    const int n = recv(fd, &byte, 1, 0);
    REQUIRE(n == 0 || (n < 0 && (errno == ECONNRESET || errno == ENOTCONN)));
  }
};

static Bytes message(uint8_t value, bool channel = false) {
  Bytes frame(channel ? 12 : 17, 0);
  frame[0] = channel ? 17 : 16;
  frame[1] = 12;
  if (channel) {
    frame[5] = 255;
    frame[7] = 1;
  } else {
    frame[4] = 42;
    frame[10] = 255;
    frame[12] = 1;
  }
  frame.back() = value;
  return frame;
}

// A frame-level native-interface fixture, not a mesh or protocol emulator.
// Commands under test produce only the native response boundaries relevant to
// this adapter; the ESP32 integration retains the actual MyMesh implementation.
class NativeFrames {
  CompanionSessions& sessions;
  std::atomic<bool> stop{false};
  std::thread dispatcher;
  std::mutex mutex;
  std::deque<Bytes> incoming, pushes;
  void run() {
    std::deque<Bytes> offline;
    bool contacts = false;
    unsigned contact = 0;
    while (!stop.load()) {
      {
        std::lock_guard<std::mutex> guard(mutex);
        if (!incoming.empty()) {
          offline.push_back(std::move(incoming.front()));
          incoming.pop_front();
          if (sessions.isConnected()) {
            const uint8_t waiting = 0x83;
            sessions.writeFrame(&waiting, 1);
          }
        }
        if (!pushes.empty()) {
          const auto& push = pushes.front();
          sessions.writeFrame(push.data(), push.size());
          pushes.pop_front();
          ++emittedPushes;
        }
      }
      uint8_t data[MAX_FRAME_SIZE];
      const size_t size = sessions.checkRecvFrame(data);
      if (size) {
        switch (data[0]) {
          case 22: {
            version.store(data[1]);
            const uint8_t reply[] = {13, 13, 175, 40};
            sessions.writeFrame(reply, sizeof(reply));
            break;
          }
          case 10:
            if (failCollection.exchange(false)) {
              const uint8_t reply[] = {1, 4};
              sessions.writeFrame(reply, sizeof(reply));
            } else if (offline.empty()) {
              const uint8_t reply = 10;
              sessions.writeFrame(&reply, 1);
            } else {
              auto& frame = offline.front();
              sessions.writeFrame(frame.data(), frame.size());
              offline.pop_front();
            }
            break;
          case 1: {
            Bytes reply(58, 0);
            reply[0] = 5;
            sessions.writeFrame(reply.data(), reply.size());
            break;
          }
          case 4: {
            contacts = true;
            contact = 0;
            const uint8_t reply[] = {2, 30, 0, 0, 0};
            sessions.writeFrame(reply, sizeof(reply));
            break;
          }
          case 5: {
            for (unsigned i = size; i < MAX_FRAME_SIZE; ++i)
              if (data[i]) cleanCommandTail.store(false);
            const uint8_t reply[] = {9, 42, 0, 0, 0};
            sessions.writeFrame(reply, sizeof(reply));
            break;
          }
          case 26: case 27: case 39: case 50: case 52: case 57: {
            if (data[0] == 39 && size == 4) {
              const uint8_t local[] = {0x8b, 0, 42, 43, 44, 45, 46, 47};
              sessions.writeFrame(local, sizeof(local));
              break;
            }
            ++rfCalls;
            if (rejectRF.exchange(false)) {
              const uint8_t error[] = {1, 3};
              sessions.writeFrame(error, sizeof(error));
              break;
            }
            const uint32_t tag = 0x12340000 + rfCalls.load();
            const uint32_t timeout = rfEstimateMs.load();
            uint8_t reply[10] = {6, 0};
            for (unsigned i = 0; i < 4; ++i) {
              reply[2 + i] = tag >> (8 * i);
              reply[6 + i] = timeout >> (8 * i);
            }
            sessions.writeFrame(reply, sizeof(reply));
            break;
          }
          default: {
            const uint8_t reply[] = {1, 1};
            sessions.writeFrame(reply, sizeof(reply));
          }
        }
      } else if (contacts && !pauseContacts.load() && !sessions.isWriteBusy()) {
        if (contact < 30) {
          Bytes reply(148, 0);
          reply[0] = 3;
          reply[1] = ++contact;
          sessions.writeFrame(reply.data(), reply.size());
        } else {
          const uint8_t reply[] = {4, 0, 0, 0, 0};
          sessions.writeFrame(reply, sizeof(reply));
          contacts = false;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
public:
  std::atomic<unsigned> version{0};
  std::atomic<unsigned> emittedPushes{0};
  std::atomic<unsigned> rfCalls{0}, rfEstimateMs{10000};
  std::atomic<bool> pauseContacts{false}, failCollection{false};
  std::atomic<bool> rejectRF{false};
  std::atomic<bool> cleanCommandTail{true};
  explicit NativeFrames(CompanionSessions& value) : sessions(value) {
    dispatcher = std::thread(&NativeFrames::run, this);
  }
  ~NativeFrames() { stop.store(true); dispatcher.join(); }
  void receive(Bytes frame) {
    std::lock_guard<std::mutex> guard(mutex);
    incoming.push_back(std::move(frame));
  }
  void push(Bytes frame) {
    std::lock_guard<std::mutex> guard(mutex);
    pushes.push_back(std::move(frame));
  }
};

void independentReplay() {
  CompanionSessions sessions(0);
  REQUIRE(sessions.begin());
  NativeFrames native(sessions);
  until([&] { return native.version.load() == 3; });
  native.receive(message('A'));
  until([&] { return sessions.stats().collectedMessages == 1; });
  Client first(sessions.port());
  first.command({22, 3});
  REQUIRE(first.frame()[0] == 13);
  first.command({1, 3, 0, 0, 0, 0, 0, 0});
  REQUIRE(first.frame()[0] == 5);
  REQUIRE(first.frame() == Bytes{0x83});
  REQUIRE(first.sync() == message('A'));
  REQUIRE(first.sync() == Bytes{10});
  Client second(sessions.port());
  second.handshake(2);
  auto legacy = second.sync();
  REQUIRE(legacy[0] == 7 && legacy.size() == message('A').size() - 3 && legacy.back() == 'A');
  REQUIRE(native.version.load() == 3);
  native.receive(message('B', true));
  until([&] { return sessions.stats().collectedMessages == 2; });
  REQUIRE(first.sync() == message('B', true));
  legacy = second.sync();
  REQUIRE(legacy[0] == 8 && legacy.back() == 'B');
  REQUIRE(first.sync() == Bytes{10});
  REQUIRE(second.sync() == Bytes{10});
  first.disconnect();
  until([&] { return sessions.stats().connectedClients == 1; });
  Client reconnected(sessions.port());
  reconnected.handshake();
  REQUIRE(reconnected.sync() == message('A'));
  REQUIRE(reconnected.sync() == message('B', true));
  REQUIRE(reconnected.sync() == Bytes{10});
  REQUIRE(!sessions.stats().nativeFault);
}

void contactsOwnershipAndReuse() {
  CompanionSessions sessions(0);
  REQUIRE(sessions.begin());
  NativeFrames native(sessions);
  Client first(sessions.port()), second(sessions.port());
  first.handshake();
  second.handshake();
  native.pauseContacts.store(true);
  first.command({4});
  REQUIRE(first.response()[0] == 2);
  native.receive(message('M'));
  until([&] { return sessions.stats().collectedMessages == 1; });
  REQUIRE(first.frame() == Bytes{0x83});
  REQUIRE(second.frame() == Bytes{0x83});
  REQUIRE(second.sync() == message('M'));
  second.command({5});
  Bytes push(33, 7);
  push[0] = 0x80;
  native.push(push);
  REQUIRE(first.frame() == push);
  REQUIRE(second.frame() == push);
  native.pauseContacts.store(false);
  for (unsigned i = 1; i <= 30; ++i) {
    const auto frame = first.response();
    REQUIRE(frame[0] == 3 && frame[1] == i);
  }
  REQUIRE(first.response()[0] == 4);
  REQUIRE(second.response() == (Bytes{9, 42, 0, 0, 0}));
  native.pauseContacts.store(true);
  first.command({4});
  REQUIRE(first.response()[0] == 2);
  first.disconnect();
  until([&] { return sessions.stats().connectedClients == 1; });
  Client replacement(sessions.port());
  replacement.command({22, 3});
  second.command({5});
  until([&] { return sessions.stats().acceptedClients == 3; });
  native.pauseContacts.store(false);
  REQUIRE(replacement.response()[0] == 13);
  REQUIRE(second.response()[0] == 9);
  REQUIRE(!sessions.stats().nativeFault);
}

void partialIOAndMalformedFrames() {
  sendChunk.store(3);
  shortWrites.store(0);
  CompanionSessions sessions(0);
  REQUIRE(sessions.begin());
  NativeFrames native(sessions);
  Client fragmented(sessions.port()), other(sessions.port());
  fragmented.raw({'<'});
  other.handshake();
  other.command({5});
  REQUIRE(other.response()[0] == 9);
  fragmented.raw({2, 0, 22});
  other.command({5});
  REQUIRE(other.response()[0] == 9);
  fragmented.raw({3});
  REQUIRE(fragmented.response()[0] == 13);
  fragmented.command({1, 3, 0, 0, 0, 0, 0, 0});
  REQUIRE(fragmented.response()[0] == 5);
  REQUIRE(shortWrites.load() > 0);
  fragmented.raw({'<', 0, 0});
  fragmented.closed();
  until([&] { return sessions.stats().malformedFrames == 1; });
  Client oversized(sessions.port());
  oversized.raw({'<', 177, 0});
  oversized.closed();
  until([&] { return sessions.stats().malformedFrames == 2; });
  other.command({0xf0});
  REQUIRE(other.response() == (Bytes{1, 1}));
  Bytes longCommand(MAX_FRAME_SIZE, 0xaa);
  longCommand[0] = 0xf0;
  other.command(longCommand);
  REQUIRE(other.response() == (Bytes{1, 1}));
  other.command({5});
  REQUIRE(other.response()[0] == 9);
  REQUIRE(native.cleanCommandTail.load());
  sendChunk.store(0);
}

void boundedSlowClients() {
  CompanionSessions sessions(0, 80);
  REQUIRE(sessions.begin());
  NativeFrames native(sessions);
  Client stalled(sessions.port());
  blockedPort.store(stalled.localPort());
  stalled.command({22, 3});
  Client responsive(sessions.port());
  responsive.handshake();
  responsive.command({5});
  REQUIRE(responsive.response()[0] == 9);
  stalled.closed();
  until([&] { return sessions.stats().timedOutClients == 1; });
  blockedPort.store(0);
  Client partial(sessions.port());
  partial.raw({'<'});
  partial.closed();
  until([&] { return sessions.stats().timedOutClients == 2; });
  REQUIRE(!sessions.stats().nativeFault);
}

void journalOverflowDisconnectsOnlyUnreadClients() {
  CompanionSessions sessions(0);
  REQUIRE(sessions.begin());
  NativeFrames native(sessions);
  Client slow(sessions.port()), fast(sessions.port());
  slow.handshake();
  fast.handshake();
  for (unsigned i = 1; i <= CompanionSessions::JournalDepth + 1; ++i) {
    native.receive(message(i));
    until([&] { return sessions.stats().collectedMessages == i; });
    REQUIRE(fast.sync() == message(i));
  }
  REQUIRE(slow.frame() == Bytes{0x83});
  slow.closed();
  until([&] { return sessions.stats().connectedClients == 1; });
  REQUIRE(sessions.stats().journalOverruns == 1);
  REQUIRE(fast.sync() == Bytes{10});
  Client replay(sessions.port());
  until([&] { return sessions.stats().connectedClients == 2; });
  native.receive(message(CompanionSessions::JournalDepth + 2));
  until([&] { return sessions.stats().collectedMessages == CompanionSessions::JournalDepth + 2; });
  REQUIRE(fast.sync() == message(CompanionSessions::JournalDepth + 2));
  REQUIRE(sessions.stats().journalOverruns == 1);
  replay.handshake();
  for (unsigned i = 3; i <= CompanionSessions::JournalDepth + 2; ++i)
    REQUIRE(replay.sync() == message(i));
  REQUIRE(replay.sync() == Bytes{10});
}

void outputOverflowAndDisable() {
  CompanionSessions sessions(0);
  REQUIRE(sessions.begin());
  NativeFrames native(sessions);
  Client client(sessions.port());
  client.handshake();
  blockedPort.store(client.localPort());
  for (unsigned i = 0; i < 64; ++i) native.push(Bytes{0x80, uint8_t(i)});
  client.closed();
  until([&] { return sessions.stats().outputOverflows > 0; });
  until([&] { return native.emittedPushes.load() == 64; });
  blockedPort.store(0);
  Client next(sessions.port());
  next.handshake();
  sessions.disable();
  next.closed();
  until([&] { return !sessions.isConnected(); });
  sessions.enable();
  Client enabled(sessions.port());
  enabled.handshake();
  REQUIRE(sessions.isConnected());
}

void boundedAdmissionAndNativeFailure() {
  CompanionSessions sessions(0);
  REQUIRE(sessions.begin());
  NativeFrames native(sessions);
  std::vector<std::unique_ptr<Client>> clients;
  for (unsigned i = 0; i < CompanionSessions::MaxClients; ++i) {
    clients.emplace_back(std::make_unique<Client>(sessions.port()));
    clients.back()->handshake();
  }
  REQUIRE(sessions.stats().connectedClients == CompanionSessions::MaxClients);
  Client excess(sessions.port());
  excess.closed();
  until([&] { return sessions.stats().rejectedClients == 1; });
  native.failCollection.store(true);
  until([&] { return sessions.stats().nativeFault; });
  for (const auto& client : clients) client->closed();
  REQUIRE(sessions.stats().nativeErrors == 1);
}

void disableRetiresCollectionBeforeNativeContactEnd() {
  CompanionSessions sessions(0);
  REQUIRE(sessions.begin());
  uint8_t command[MAX_FRAME_SIZE];
  REQUIRE(sessions.checkRecvFrame(command) == 2 && command[0] == 22);
  const uint8_t device[] = {13, 13, 175, 40};
  sessions.writeFrame(device, sizeof(device));
  Client client(sessions.port());
  client.command({4});
  until([&] {
    const size_t size = sessions.checkRecvFrame(command);
    if (size && command[0] == 10) {
      const uint8_t empty = 10;
      sessions.writeFrame(&empty, 1);
    }
    return size && command[0] == 4;
  });
  const uint8_t start[] = {2, 0, 0, 0, 0};
  sessions.writeFrame(start, sizeof(start));
  const uint8_t waiting = 0x83, empty = 10;
  sessions.writeFrame(&waiting, 1);
  REQUIRE(sessions.checkRecvFrame(command) == 1 && command[0] == 10);
  sessions.writeFrame(&empty, 1);
  sessions.disable();
  REQUIRE(sessions.checkRecvFrame(command) == 0);
  const uint8_t end[] = {4, 0, 0, 0, 0};
  sessions.writeFrame(end, sizeof(end));
  REQUIRE(!sessions.stats().nativeFault);
}

static Bytes request(uint8_t command) {
  const unsigned offset = command == 39 ? 4 : command == 52 ? 2 : 1;
  Bytes data(offset + 32, 0);
  if (command == 50 || command == 57) data.push_back(1);
  data[0] = command;
  for (unsigned i = 0; i < 6; ++i) data[offset + i] = 42 + i;
  return data;
}
static Bytes peerPush(uint8_t code, uint8_t prefix = 42) {
  Bytes data(8, 0);
  data[0] = code;
  for (unsigned i = 0; i < 6; ++i) data[2 + i] = prefix + i;
  return data;
}
static Bytes binaryPush(const Bytes& sent) {
  return Bytes{0x8c, 0, sent[2], sent[3], sent[4], sent[5], 99};
}

void globalRFRequestAdmission() {
  CompanionSessions sessions(0);
  REQUIRE(sessions.begin());
  NativeFrames native(sessions);
  Client a(sessions.port()), b(sessions.port());
  a.handshake(); b.handshake();
  a.command(request(39));
  REQUIRE(a.response()[0] == 6);
  REQUIRE(sessions.stats().rfRequestPending);
  b.command({5});
  REQUIRE(b.response() == (Bytes{9, 42, 0, 0, 0}));
  native.receive(message('R'));
  until([&] { return sessions.stats().collectedMessages == 1; });
  REQUIRE(a.sync() == message('R'));
  REQUIRE(b.sync() == message('R'));
  REQUIRE(sessions.stats().rfRequestPending);
  b.command({39, 0, 0, 0});
  REQUIRE(a.frame()[0] == 0x8b);
  REQUIRE(b.frame()[0] == 0x8b);
  REQUIRE(sessions.stats().rfRequestPending);
  b.command(request(27));
  REQUIRE(b.response() == (Bytes{1, 4}));
  REQUIRE(native.rfCalls.load() == 1);
  native.push(peerPush(0x87));
  REQUIRE(a.frame()[0] == 0x87); REQUIRE(b.frame()[0] == 0x87);
  REQUIRE(sessions.stats().rfRequestPending);
  native.push(peerPush(0x8b, 77));
  REQUIRE(a.frame()[0] == 0x8b); REQUIRE(b.frame()[0] == 0x8b);
  REQUIRE(sessions.stats().rfRequestPending);
  native.push(peerPush(0x8b));
  REQUIRE(a.frame()[0] == 0x8b); REQUIRE(b.frame()[0] == 0x8b);
  REQUIRE(!sessions.stats().rfRequestPending);

  b.command(request(27));
  REQUIRE(b.response()[0] == 6);
  a.command(request(50));
  REQUIRE(a.response() == (Bytes{1, 4}));
  REQUIRE(native.rfCalls.load() == 2);
  native.push(peerPush(0x87));
  REQUIRE(a.frame()[0] == 0x87); REQUIRE(b.frame()[0] == 0x87);
  a.command(request(50));
  auto sent = a.response();
  REQUIRE(sent[0] == 6);
  auto wrong = binaryPush(sent);
  ++wrong[2];
  native.push(wrong);
  REQUIRE(a.frame() == wrong); REQUIRE(b.frame() == wrong);
  REQUIRE(sessions.stats().rfRequestPending);
  native.push(binaryPush(sent));
  REQUIRE(a.frame()[0] == 0x8c); REQUIRE(b.frame()[0] == 0x8c);
  REQUIRE(!sessions.stats().rfRequestPending);

  a.command(request(26));
  REQUIRE(a.response()[0] == 6);
  b.command(request(52));
  REQUIRE(b.response() == (Bytes{1, 4}));
  native.push(peerPush(0x86));
  REQUIRE(a.frame()[0] == 0x86); REQUIRE(b.frame()[0] == 0x86);
  REQUIRE(!sessions.stats().rfRequestPending);
  a.command(request(57));
  sent = a.response();
  REQUIRE(sent[0] == 6);
  a.disconnect();
  until([&] { return sessions.stats().connectedClients == 1; });
  b.command(request(52));
  REQUIRE(b.response() == (Bytes{1, 4}));
  native.push(binaryPush(sent));
  REQUIRE(b.frame()[0] == 0x8c);
  REQUIRE(!sessions.stats().rfRequestPending);

  native.rfEstimateMs.store(40);
  b.command(request(27));
  REQUIRE(b.response()[0] == 6);
  until([&] { return sessions.stats().rfTimeouts == 1; });
  native.rfEstimateMs.store(10000);
  b.command(request(52));
  REQUIRE(b.response()[0] == 6);
  native.push(peerPush(0x8d));
  REQUIRE(b.frame()[0] == 0x8d);
  REQUIRE(!sessions.stats().rfRequestPending);
  native.rejectRF.store(true);
  b.command(request(27));
  REQUIRE(b.response() == (Bytes{1, 3}));
  REQUIRE(!sessions.stats().rfRequestPending);
}

void nativeRestartRetiresClientsAndState() {
  CompanionSessions sessions(0);
  REQUIRE(sessions.begin());
  const uint16_t port = sessions.port();
  auto native = std::make_unique<NativeFrames>(sessions);
  Client a(port), b(port);
  a.handshake(); b.handshake();
  native->receive(message('O'));
  until([&] { return sessions.stats().collectedMessages == 1; });
  REQUIRE(a.frame() == Bytes{0x83});
  REQUIRE(b.frame() == Bytes{0x83});
  a.command(request(27));
  REQUIRE(a.response()[0] == 6);
  native->pauseContacts.store(true);
  b.command({4});
  REQUIRE(b.response()[0] == 2);
  blockedPort.store(a.localPort());
  native->push(Bytes{0x80, 42});
  REQUIRE(b.frame() == (Bytes{0x80, 42}));
  a.command({5});
  b.command({22, 1});
  native.reset();
  REQUIRE(sessions.stats().rfRequestPending);

  sessions.resetNativeSession();
  REQUIRE(!sessions.isEnabled());
  REQUIRE(!sessions.isConnected());
  REQUIRE(!sessions.stats().rfRequestPending);
  REQUIRE(sessions.stats().nativeResets == 1);
  REQUIRE(sessions.stats().collectedMessages == 1);
  REQUIRE(sessions.port() == port);
  uint8_t command[MAX_FRAME_SIZE];
  REQUIRE(sessions.checkRecvFrame(command) == 0);
  Client disabled(port);
  disabled.closed();
  a.closed(); b.closed();
  blockedPort.store(0);

  sessions.enable();
  native = std::make_unique<NativeFrames>(sessions);
  Client nextA(port), nextB(port);
  nextA.handshake(); nextB.handshake();
  REQUIRE(native->version.load() == 3);
  REQUIRE(nextA.sync() == Bytes{10});
  REQUIRE(nextB.sync() == Bytes{10});
  nextA.command(request(27));
  REQUIRE(nextA.response()[0] == 6);
  REQUIRE(native->rfCalls.load() == 1);
  native->receive(message('N'));
  until([&] { return sessions.stats().collectedMessages == 2; });
  REQUIRE(nextA.sync() == message('N'));
  REQUIRE(nextB.sync() == message('N'));

  native->failCollection.store(true);
  until([&] { return sessions.stats().nativeFault; });
  native.reset();
  const auto errors = sessions.stats().nativeErrors;
  sessions.resetNativeSession();
  sessions.enable();
  native = std::make_unique<NativeFrames>(sessions);
  nextA.closed(); nextB.closed();
  Client recovered(port);
  recovered.handshake();
  REQUIRE(recovered.sync() == Bytes{10});
  REQUIRE(!sessions.stats().nativeFault);
  REQUIRE(sessions.stats().nativeErrors == errors);
  REQUIRE(sessions.stats().nativeResets == 2);
}

void networkLossPreservesNativeJournal() {
  CompanionSessions sessions(0);
  REQUIRE(sessions.begin());
  NativeFrames native(sessions);
  Client before(sessions.port());
  before.handshake();
  native.receive(message('B'));
  until([&] { return sessions.stats().collectedMessages == 1; });
  REQUIRE(before.sync() == message('B'));
  sessions.setNetworkAvailable(false);
  REQUIRE(sessions.isEnabled());
  REQUIRE(!sessions.isConnected());
  before.closed();
  Client unavailable(sessions.port());
  unavailable.closed();
  native.receive(message('D'));
  until([&] { return sessions.stats().collectedMessages == 2; });
  sessions.setNetworkAvailable(true);
  Client after(sessions.port());
  after.handshake();
  REQUIRE(after.sync() == message('B'));
  REQUIRE(after.sync() == message('D'));
  REQUIRE(sessions.stats().nativeResets == 0);
  REQUIRE(!sessions.stats().nativeFault);
  sessions.disable();
  sessions.setNetworkAvailable(false);
  sessions.setNetworkAvailable(true);
  REQUIRE(!sessions.isEnabled());
  after.closed();
}

void nativeRestartClearsIncompleteInternalExchange() {
  for (bool collecting : {false, true}) {
    CompanionSessions sessions(0);
    REQUIRE(sessions.begin());
    uint8_t command[MAX_FRAME_SIZE];
    REQUIRE(sessions.checkRecvFrame(command) == 2 && command[0] == 22);
    const uint8_t device[] = {13, 13, 175, 40};
    if (collecting) {
      sessions.writeFrame(device, sizeof(device));
      REQUIRE(sessions.checkRecvFrame(command) == 1 && command[0] == 10);
    }
    sessions.resetNativeSession();
    REQUIRE(sessions.checkRecvFrame(command) == 0);
    sessions.enable();
    REQUIRE(sessions.checkRecvFrame(command) == 2 && command[0] == 22 && command[1] == 3);
    sessions.writeFrame(device, sizeof(device));
    REQUIRE(sessions.checkRecvFrame(command) == 1 && command[0] == 10);
    const uint8_t empty = 10;
    sessions.writeFrame(&empty, 1);
    REQUIRE(!sessions.stats().nativeFault);
  }
}

void journalAllocationAndLifetime() {
  static unsigned errors = 0;
  CompanionSessions sessions(0, 10000, [](const char* message) {
    if (!strcmp(message, "Companion journal/output allocation failed; listener not started"))
      ++errors;
  });
  sessions.resetNativeSession();
  const unsigned before = journalAllocations;
  failJournalAllocation = true;
  REQUIRE(!sessions.begin());
  REQUIRE(journalAllocations == before + 1 && errors == 1);
  REQUIRE(journalBytes == (MAX_FRAME_SIZE + sizeof(uint16_t)) *
      (CompanionSessions::JournalDepth +
       CompanionSessions::MaxClients * CompanionSessions::OutputDepth));
  REQUIRE(!sessions.port() && !sessions.isEnabled());
  failJournalAllocation = false;
  REQUIRE(sessions.begin());
  REQUIRE(journalAllocations == before + 2);
  {
    NativeFrames native(sessions);
    native.receive(message('J'));
    until([&] { return sessions.stats().collectedMessages == 1; });
  }
  sessions.end();
  failJournalAllocation = true;
  REQUIRE(sessions.begin());
  REQUIRE(journalAllocations == before + 2);
  failJournalAllocation = false;
  {
    NativeFrames native(sessions);
    Client restored(sessions.port());
    restored.handshake();
    REQUIRE(restored.sync() == message('J'));
    REQUIRE(restored.sync() == Bytes{10});
  }
}

int main() {
  try {
    journalAllocationAndLifetime();
    independentReplay();
    contactsOwnershipAndReuse();
    partialIOAndMalformedFrames();
    boundedSlowClients();
    journalOverflowDisconnectsOnlyUnreadClients();
    outputOverflowAndDisable();
    boundedAdmissionAndNativeFailure();
    disableRetiresCollectionBeforeNativeContactEnd();
    globalRFRequestAdmission();
    nativeRestartRetiresClientsAndState();
    nativeRestartClearsIncompleteInternalExchange();
    networkLossPreservesNativeJournal();
    std::puts("CompanionSessions: thirteen TCP/session behaviour scenarios passed");
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
}
