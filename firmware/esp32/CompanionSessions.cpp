// SPDX-License-Identifier: Apache-2.0
#include "CompanionSessions.h"

#include <errno.h>
#include <fcntl.h>
#include <new>
#include <string.h>
#include <unistd.h>

#if defined(COMPANION_SESSIONS_HOST)
#include <arpa/inet.h>
#include <chrono>
#include <cstdio>
#include <netinet/tcp.h>
#include <sys/socket.h>
#else
#include "RoleStorage.h"
#include <lwip/sockets.h>
#include <lwip/tcp.h>
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace onchip {
namespace {
constexpr uint8_t AppStart = 1, SyncMessage = 10, DeviceQuery = 22;
constexpr uint8_t ContactsStart = 2, Contact = 3, ContactsEnd = 4;
constexpr uint8_t SelfInfo = 5, NoMessages = 10, DeviceInfo = 13;
constexpr uint8_t MessageWaiting = 0x83;

bool messageFrame(uint8_t code) {
  return code == 7 || code == 8 || code == 16 || code == 17 || code == 27;
}
bool wouldBlock() { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR; }
bool nonblocking(int fd) {
  const int flags = fcntl(fd, F_GETFL, 0);
  return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}
bool expired(uint32_t current, uint32_t start, uint32_t duration) {
  return uint32_t(current - start) >= duration;
}
uint32_t little32(const uint8_t* data) {
  return uint32_t(data[0]) | uint32_t(data[1]) << 8 |
         uint32_t(data[2]) << 16 | uint32_t(data[3]) << 24;
}
// Only these native command forms call clearPendingReqs(). Local telemetry and
// malformed requests still go to the native handler without acquiring a lease.
uint8_t rfReplyFor(const uint8_t* data, size_t size, unsigned& keyOffset) {
  keyOffset = 1;
  switch (data[0]) {
    case 26: return size >= 33 ? 0x85 : 0;
    case 27: return size >= 33 ? 0x87 : 0;
    case 39: keyOffset = 4; return size >= 36 ? 0x8b : 0;
    case 50: case 57: return size >= 34 ? 0x8c : 0;
    case 52: keyOffset = 2; return size >= 34 && data[1] == 0 ? 0x8d : 0;
    default: return 0;
  }
}
}

CompanionSessions::CompanionSessions(uint16_t port, uint32_t ioTimeoutMs,
                                     DiagnosticHandler handler)
    : requestedPort(port), timeoutMs(ioTimeoutMs), diagnostic(handler) {}

CompanionSessions::~CompanionSessions() {
  end();
#if defined(COMPANION_SESSIONS_HOST)
  delete journal;
#else
  releaseRoleStorage(journal);
#endif
}

void CompanionSessions::lock() const {
#if defined(COMPANION_SESSIONS_HOST)
  mutex.lock();
#else
  portENTER_CRITICAL(&mutex);
#endif
}
void CompanionSessions::unlock() const {
#if defined(COMPANION_SESSIONS_HOST)
  mutex.unlock();
#else
  portEXIT_CRITICAL(&mutex);
#endif
}
uint32_t CompanionSessions::now() {
#if defined(COMPANION_SESSIONS_HOST)
  return uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
#else
  return millis();
#endif
}

uint16_t CompanionSessions::port() const { Guard guard(*this); return boundPort; }
CompanionSessions::Stats CompanionSessions::stats() const {
  Guard guard(*this);
  Stats result = counters;
  result.rfRequestPending = rfPending;
  for (const auto& client : clients)
    if (client.active && !client.closing) ++result.connectedClients;
  return result;
}
void CompanionSessions::enable() { Guard guard(*this); enabled = true; collecting = true; }
void CompanionSessions::setNetworkAvailable(bool available) {
  Guard guard(*this);
  networkAvailable = available;
  if (!available)
    for (auto& client : clients)
      if (client.active) client.closing = true;
}
void CompanionSessions::disable() {
  Guard guard(*this);
  enabled = false;
  for (unsigned i = 0; i < MaxClients; ++i)
    if (clients[i].active) clients[i].closing = true;
}
void CompanionSessions::resetNativeSession() {
  Guard guard(*this);
  enabled = false;
  for (auto& client : clients) {
    if (client.active) client.closing = true;
    client.pending = client.appStarted = client.notified = client.cursorStarted = false;
    client.command = Frame{};
    for (auto& frame : client.output) frame = Frame{};
    client.head = client.count = client.version = 0;
    client.cursor = 0;
  }
  if (journal)
    for (auto& frame : journal->frames) frame = Frame{};
  sequence = 0;
  nextClient = 0;
  operation = Operation::None;
  requester = contactsOwner = Owner{};
  currentCommand = currentRFReply = rfReply = 0;
  memset(currentRFPeer, 0, sizeof(currentRFPeer));
  memset(rfPeer, 0, sizeof(rfPeer));
  localTelemetryPending = rfPending = false;
  rfTag = rfStarted = rfDuration = 0;
  contactsActive = configured = configureReplied = false;
  collecting = collectReplied = false;
  collectedInTurn = 0;
  lastPoll = contactsSince = 0;
  counters.nativeFault = false;
  ++counters.nativeResets;
  diagnostics |= NativeReset;
}
bool CompanionSessions::isEnabled() const { Guard guard(*this); return enabled; }
bool CompanionSessions::isConnected() const {
  Guard guard(*this);
  for (const auto& client : clients)
    if (enabled && client.active && !client.closing) return true;
  return false;
}
bool CompanionSessions::owns(Owner owner) const {
  return owner.slot >= 0 && clients[owner.slot].active && !clients[owner.slot].closing &&
         clients[owner.slot].generation == owner.generation;
}
bool CompanionSessions::isWriteBusy() const {
  Guard guard(*this);
  return enabled && contactsActive && owns(contactsOwner) &&
      clients[contactsOwner.slot].count >= OutputDepth - 1;
}

void CompanionSessions::drop(unsigned slot, Diagnostic reason) {
  auto& client = clients[slot];
  if (!client.active || client.closing) return;
  client.closing = true;
  ++counters.droppedClients;
  diagnostics |= reason;
  switch (reason) {
    case BadFrame: ++counters.malformedFrames; break;
    case Timeout: ++counters.timedOutClients; break;
    case OutputFull: ++counters.outputOverflows; break;
    case JournalFull: ++counters.journalOverruns; break;
    default: break;
  }
}
void CompanionSessions::nativeFailure() {
  counters.nativeFault = true;
  ++counters.nativeErrors;
  diagnostics |= NativeFailure;
  for (unsigned i = 0; i < MaxClients; ++i) drop(i, NativeFailure);
}
bool CompanionSessions::enqueue(unsigned slot, const uint8_t* data, size_t size) {
  auto& client = clients[slot];
  if (!client.active || client.closing) return false;
  if (client.count == OutputDepth) { drop(slot, OutputFull); return false; }
  auto& frame = client.output[(client.head + client.count) % OutputDepth];
  frame.size = size;
  memcpy(frame.data, data, size);
  ++client.count;
  return true;
}
uint64_t CompanionSessions::oldest() const {
  return sequence > JournalDepth ? sequence - JournalDepth : 0;
}
void CompanionSessions::notify(unsigned slot) {
  auto& client = clients[slot];
  if (client.appStarted && !client.notified && client.cursor < sequence &&
      enqueue(slot, &MessageWaiting, 1)) client.notified = true;
}
void CompanionSessions::publish(const uint8_t* data, size_t size) {
  if (sequence >= JournalDepth) {
    const uint64_t evicted = oldest();
    for (unsigned i = 0; i < MaxClients; ++i)
      if (clients[i].active && clients[i].cursorStarted && clients[i].cursor <= evicted)
        drop(i, JournalFull);
  }
  auto& frame = journal->frames[sequence % JournalDepth];
  frame.size = size;
  memcpy(frame.data, data, size);
  ++sequence;
  ++counters.collectedMessages;
  for (unsigned i = 0; i < MaxClients; ++i) notify(i);
}
void CompanionSessions::sync(unsigned slot) {
  auto& client = clients[slot];
  client.pending = false;
  if (!client.cursorStarted) {
    client.cursor = oldest();
    client.cursorStarted = true;
  }
  if (client.cursor < oldest()) { drop(slot, JournalFull); return; }
  if (client.cursor == sequence) {
    enqueue(slot, &NoMessages, 1);
    client.notified = false;
    return;
  }
  const auto& frame = journal->frames[client.cursor % JournalDepth];
  Frame reply = frame;
  if (client.version < 3 && (reply.data[0] == 16 || reply.data[0] == 17)) {
    if (reply.size < 4) { nativeFailure(); return; }
    reply.data[0] = reply.data[0] == 16 ? 7 : 8;
    memmove(reply.data + 1, reply.data + 4, reply.size - 4);
    reply.size -= 3;
  }
  if (enqueue(slot, reply.data, reply.size)) {
    ++client.cursor;
    ++counters.replayedMessages;
    if (client.cursor == sequence) client.notified = false;
  }
}

bool CompanionSessions::matchesRFReply(const uint8_t* data, size_t size) const {
  if (!rfPending || (data[0] != rfReply && !(rfReply == 0x85 && data[0] == 0x86)))
    return false;
  if (rfReply == 0x8c) return size >= 6 && little32(data + 2) == rfTag;
  return size >= 8 && memcmp(data + 2, rfPeer, sizeof(rfPeer)) == 0;
}

size_t CompanionSessions::writeFrame(const uint8_t* data, size_t size) {
  Guard guard(*this);
  if (!size || size > MAX_FRAME_SIZE) { nativeFailure(); return 0; }
  const uint8_t code = data[0];
  if (code == MessageWaiting) {
    collecting = true;
    return size;
  }
  if (code >= 0x80) {
    const bool localTelemetry = code == 0x8b && localTelemetryPending;
    if (localTelemetry) localTelemetryPending = false;
    if (!localTelemetry && matchesRFReply(data, size)) rfPending = false;
    if (enabled)
      for (unsigned i = 0; i < MaxClients; ++i) enqueue(i, data, size);
    return size;
  }
  localTelemetryPending = false;
  if (operation == Operation::Configure) {
    if (code != DeviceInfo) { nativeFailure(); return 0; }
    configured = configureReplied = true;
    return size;
  }
  if (operation == Operation::Collect) {
    collectReplied = true;
    if (messageFrame(code)) {
      publish(data, size);
    } else if (code == NoMessages) {
      collecting = false;
    } else {
      nativeFailure();
      return 0;
    }
    return size;
  }
  Owner owner = requester;
  if (contactsActive && (code == Contact || code == ContactsEnd)) owner = contactsOwner;
  if (code == ContactsStart && operation == Operation::Client) {
    contactsOwner = requester;
    contactsActive = true;
    contactsSince = now();
  }
  if (code == ContactsEnd) contactsActive = false;
  if (owner.slot < 0) { nativeFailure(); return 0; }
  if (operation == Operation::Client && currentRFReply && code == 6) {
    if (size != 10) { nativeFailure(); return 0; }
    rfReply = currentRFReply;
    memcpy(rfPeer, currentRFPeer, sizeof(rfPeer));
    rfTag = little32(data + 2);
    rfDuration = little32(data + 6);
    rfStarted = now();
    rfPending = true;
  }
  if (enabled && owns(owner)) {
    if (!enqueue(owner.slot, data, size)) return 0;
    if (code == SelfInfo && currentCommand == AppStart) {
      auto& client = clients[owner.slot];
      if (!client.cursorStarted) {
        client.cursor = oldest();
        client.cursorStarted = true;
      }
      client.appStarted = true;
      client.notified = false;
      notify(owner.slot);
    }
  }
  return size;
}

size_t CompanionSessions::checkRecvFrame(uint8_t* data) {
  Guard guard(*this);
  if (counters.nativeFault) return 0;
  if ((operation == Operation::Configure && !configureReplied) ||
      (operation == Operation::Collect && !collectReplied)) {
    nativeFailure();
    return 0;
  }
  const bool justCollected = operation == Operation::Collect;
  operation = Operation::None;
  requester = Owner{};
  currentCommand = 0;
  currentRFReply = 0;
  localTelemetryPending = false;
  if (!enabled) return 0;
  const uint32_t current = now();
  if (rfPending && expired(current, rfStarted, rfDuration)) {
    rfPending = false;
    ++counters.rfTimeouts;
    diagnostics |= RFTimeout;
  }
  if (contactsActive && expired(current, contactsSince, timeoutMs) && owns(contactsOwner))
    drop(contactsOwner.slot, Timeout);
  if (!configured) {
    operation = Operation::Configure;
    configureReplied = false;
    data[0] = DeviceQuery;
    data[1] = 3;
    return 2;
  }
  if (!justCollected && expired(current, lastPoll, 100)) { collecting = true; lastPoll = current; }
  if (collecting && collectedInTurn < 4) {
    ++collectedInTurn;
    operation = Operation::Collect;
    collectReplied = false;
    data[0] = SyncMessage;
    return 1;
  }
  collectedInTurn = 0;
  for (unsigned offset = 0; offset < MaxClients; ++offset) {
    const unsigned i = (nextClient + offset) % MaxClients;
    auto& client = clients[i];
    if (!client.active || client.closing || !client.pending || client.count >= OutputDepth - 2) continue;
    if (client.command.data[0] == SyncMessage) {
      if (client.cursor == sequence && collecting) continue;
      sync(i);
      nextClient = (i + 1) % MaxClients;
      return 0;
    }
    unsigned keyOffset;
    const uint8_t expectedRFReply = rfReplyFor(client.command.data, client.command.size, keyOffset);
    if (expectedRFReply && rfPending) {
      const uint8_t busy[] = {1, 4};
      client.pending = false;
      enqueue(i, busy, sizeof(busy));
      ++counters.rfBusyRejections;
      nextClient = (i + 1) % MaxClients;
      return 0;
    }
    if (contactsActive) continue;
    const size_t size = client.command.size;
    memcpy(data, client.command.data, size);
    // Some native commands inspect fixed offsets; never expose another
    // client's previous command through an otherwise short input frame.
    memset(data + size, 0, MAX_FRAME_SIZE - size);
    client.pending = false;
    requester.slot = i;
    requester.generation = client.generation;
    nextClient = (i + 1) % MaxClients;
    currentCommand = data[0];
    currentRFReply = expectedRFReply;
    if (expectedRFReply) memcpy(currentRFPeer, data + keyOffset, sizeof(currentRFPeer));
    localTelemetryPending = currentCommand == 39 && size == 4;
    operation = Operation::Client;
    if (data[0] == DeviceQuery && size >= 2) {
      client.version = data[1];
      data[1] = 3;
    }
    return size;
  }
  return 0;
}

bool CompanionSessions::begin() {
  {
    Guard guard(*this);
    if (running) return true;
  }
  if (!timeoutMs || timeoutMs > 60000) return false;
  // Allocate after PSRAM initialization; end/begin retains the same history.
  if (!journal) {
#if defined(COMPANION_SESSIONS_HOST)
    journal = new (std::nothrow) Journal;
#else
    journal = allocateRoleStorage<Journal>("companion message journal");
#endif
    if (!journal) {
      if (diagnostic)
        diagnostic("Companion message journal allocation failed; listener not started");
      return false;
    }
  }
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return false;
  int yes = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(requestedPort);
#if defined(COMPANION_SESSIONS_HOST)
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
#else
  address.sin_addr.s_addr = htonl(INADDR_ANY);
#endif
  if (!nonblocking(fd) || bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 ||
      listen(fd, MaxClients) < 0) {
    close(fd);
    return false;
  }
  socklen_t length = sizeof(address);
  if (getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) < 0) {
    close(fd);
    return false;
  }
  {
    Guard guard(*this);
    listener = fd;
    boundPort = ntohs(address.sin_port);
    enabled = running = true;
    stopping = false;
    collecting = true;
  }
#if defined(COMPANION_SESSIONS_HOST)
  worker = std::thread(&CompanionSessions::networkLoop, this);
#else
  if (xTaskCreate(taskEntry, "companion-tcp", 4096, this, 1, &worker) != pdPASS) {
    close(fd);
    Guard guard(*this);
    listener = -1;
    enabled = running = false;
    boundPort = 0;
    return false;
  }
#endif
  return true;
}

void CompanionSessions::end() {
  {
    Guard guard(*this);
    stopping = true;
    enabled = false;
  }
#if defined(COMPANION_SESSIONS_HOST)
  if (worker.joinable()) worker.join();
#else
  for (;;) {
    { Guard guard(*this); if (!running) break; }
    vTaskDelay(1);
  }
#endif
}
void CompanionSessions::taskEntry(void* argument) {
  static_cast<CompanionSessions*>(argument)->networkLoop();
#if defined(ARDUINO_ARCH_ESP32) && !defined(COMPANION_SESSIONS_HOST)
  vTaskDelete(nullptr);
#endif
}
void CompanionSessions::closeSocket(unsigned slot) {
  auto& socket = sockets[slot];
  if (socket.fd >= 0) close(socket.fd);
  socket.fd = -1;
  socket.received = socket.expected = socket.size = socket.written = 0;
  Guard guard(*this);
  auto& client = clients[slot];
  client.active = client.closing = client.pending = false;
  client.count = client.head = 0;
}
void CompanionSessions::networkLoop() {
  for (;;) {
    { Guard guard(*this); if (stopping) break; }
    networkPass();
    reportDiagnostics();
#if defined(COMPANION_SESSIONS_HOST)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
#else
    vTaskDelay(1);
#endif
  }
  reportDiagnostics();
  for (unsigned i = 0; i < MaxClients; ++i) closeSocket(i);
  close(listener);
  Guard guard(*this);
  listener = -1;
  boundPort = 0;
  running = false;
}

void CompanionSessions::networkPass() {
  const uint32_t current = now();
  for (unsigned i = 0; i < MaxClients; ++i) {
    auto& peer = sockets[i];
    if (peer.fd < 0) continue;
    bool closing, pending;
    {
      Guard guard(*this);
      closing = clients[i].closing || !enabled || !networkAvailable;
      pending = clients[i].pending;
      if (!closing && !peer.size && clients[i].count) {
        auto& client = clients[i];
        const auto& frame = client.output[client.head];
        peer.output[0] = '>';
        peer.output[1] = frame.size;
        peer.output[2] = frame.size >> 8;
        memcpy(peer.output + 3, frame.data, frame.size);
        peer.size = frame.size + 3;
        peer.written = 0;
        peer.outputSince = current;
        client.head = (client.head + 1) % OutputDepth;
        --client.count;
      }
    }
    if (closing) { closeSocket(i); continue; }
    if ((peer.size && expired(current, peer.outputSince, timeoutMs)) ||
        (peer.received && expired(current, peer.inputSince, timeoutMs))) {
      Guard guard(*this); drop(i, Timeout); continue;
    }
    if (peer.size) {
      const int n = send(peer.fd, peer.output + peer.written, peer.size - peer.written,
                         MSG_DONTWAIT | MSG_NOSIGNAL);
      if (n > 0) {
        peer.written += n;
        if (peer.written == peer.size) peer.size = peer.written = 0;
      } else if (!n || !wouldBlock()) {
        Guard guard(*this); drop(i, SocketFailure); continue;
      }
    }
    if (pending) {
      uint8_t peek;
      const int n = recv(peer.fd, &peek, 1, MSG_PEEK | MSG_DONTWAIT);
      if (!n || (n < 0 && !wouldBlock())) { Guard guard(*this); drop(i, SocketFailure); }
      continue;
    }
    const unsigned wanted = peer.received < 3 ? 3 : peer.expected;
    const int n = recv(peer.fd, peer.input + peer.received, wanted - peer.received, MSG_DONTWAIT);
    if (n < 0 && wouldBlock()) continue;
    if (n <= 0) { Guard guard(*this); drop(i, SocketFailure); continue; }
    if (!peer.received) peer.inputSince = current;
    peer.received += n;
    if (peer.input[0] != '<') { Guard guard(*this); drop(i, BadFrame); continue; }
    if (peer.received == 3 && !peer.expected) {
      const unsigned size = peer.input[1] | (unsigned(peer.input[2]) << 8);
      if (!size || size > MAX_FRAME_SIZE) { Guard guard(*this); drop(i, BadFrame); continue; }
      peer.expected = size + 3;
    }
    if (peer.expected && peer.received == peer.expected) {
      Guard guard(*this);
      auto& client = clients[i];
      if (!client.active || client.closing || !enabled) continue;
      client.command.size = peer.expected - 3;
      memcpy(client.command.data, peer.input + 3, client.command.size);
      client.pending = true;
      peer.received = peer.expected = 0;
    }
  }
  for (unsigned budget = 0; budget < MaxClients; ++budget) {
    const int fd = accept(listener, nullptr, nullptr);
    if (fd < 0) {
      if (!wouldBlock()) { Guard guard(*this); diagnostics |= SocketFailure; }
      break;
    }
    if (!nonblocking(fd)) {
      close(fd);
      Guard guard(*this); ++counters.rejectedClients; diagnostics |= SocketFailure;
      continue;
    }
    int yes = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    int slot = -1;
    {
      Guard guard(*this);
      if (enabled && networkAvailable && !counters.nativeFault) {
        for (unsigned i = 0; i < MaxClients; ++i) {
          if (clients[i].active) continue;
          auto& client = clients[i];
          ++client.generation;
          client.active = true;
          client.closing = client.pending = client.appStarted = client.notified = client.cursorStarted = false;
          client.head = client.count = 0;
          client.version = 0;
          client.cursor = oldest();
          collecting = true;
          ++counters.acceptedClients;
          slot = i;
          break;
        }
      }
      if (slot < 0) { ++counters.rejectedClients; diagnostics |= TooManyClients; }
    }
    if (slot < 0) close(fd);
    else sockets[slot].fd = fd;
  }
}

void CompanionSessions::reportDiagnostics() {
  const uint32_t current = now();
  unsigned bits;
  {
    Guard guard(*this);
    if (!diagnostics || (!stopping && !expired(current, lastDiagnostic, 1000))) return;
    bits = diagnostics;
    diagnostics = 0;
    lastDiagnostic = current;
  }
  const struct { unsigned bit; const char* message; } messages[] = {
    {SocketFailure, "Companion TCP socket closed or failed"},
    {BadFrame, "Companion TCP rejected malformed framing"},
    {Timeout, "Companion TCP disconnected a stalled client"},
    {OutputFull, "Companion TCP disconnected a client after output overflow"},
    {JournalFull, "Companion TCP disconnected an unread-journal laggard"},
    {NativeFailure, "Companion native frame bridge failed closed"},
    {TooManyClients, "Companion TCP rejected an excess or unavailable session"},
    {RFTimeout, "Companion RF admission lease reached its native SENT timeout; queued RF work is not cancelled"},
    {NativeReset, "Companion native session reset; previous clients retired"}
  };
  for (const auto& entry : messages) {
    if (!(bits & entry.bit)) continue;
    if (diagnostic) diagnostic(entry.message);
#if defined(COMPANION_SESSIONS_HOST)
    else std::fprintf(stderr, "%s\n", entry.message);
#else
    else Serial.println(entry.message);
#endif
  }
}

}
