// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <helpers/BaseSerialInterface.h>

#if defined(COMPANION_SESSIONS_HOST)
#include <mutex>
#include <thread>
#elif defined(ARDUINO_ARCH_ESP32)
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#else
#error "CompanionSessions requires ESP32 or COMPANION_SESSIONS_HOST"
#endif

namespace onchip {

// The native mesh remains the only protocol handler. Call its loop from one
// dispatch task; this adapter's separate task owns every socket operation.
// Keep native checkSerialInterface and offline-queue methods unchanged.
// A reader starts at its first AppStart/sync and replays retained RAM history,
// not a durable/exactly-once cursor. Unread eviction disconnects active readers.
// Native RF queries share one admission lease until a matching push or SENT's
// estimated timeout. Expiry does not cancel queued RF work; native legacy
// peer-prefix replies cannot disambiguate late same-peer requests.
class CompanionSessions final : public BaseSerialInterface {
public:
#if defined(ONCHIP_COMPANION_MAX_CLIENTS)
  static constexpr unsigned MaxClients = ONCHIP_COMPANION_MAX_CLIENTS;
#else
  static constexpr unsigned MaxClients = 2;
#endif
  static_assert(MaxClients >= 1 && MaxClients <= 4, "Companion client limit must be 1..4");
  static constexpr unsigned OutputDepth = 16;
  static constexpr unsigned JournalDepth = 64;

  struct Stats {
    uint32_t acceptedClients = 0;
    uint32_t rejectedClients = 0;
    uint32_t droppedClients = 0;
    uint32_t malformedFrames = 0;
    uint32_t timedOutClients = 0;
    uint32_t outputOverflows = 0;
    uint32_t journalOverruns = 0;
    uint32_t nativeErrors = 0;
    uint32_t collectedMessages = 0;
    uint32_t replayedMessages = 0;
    uint32_t rfBusyRejections = 0;
    uint32_t rfTimeouts = 0;
    uint32_t nativeResets = 0;
    unsigned connectedClients = 0;
    bool nativeFault = false;
    bool rfRequestPending = false;
  };
  // Invoked on the network task; custom handlers must not block.
  using DiagnosticHandler = void (*)(const char*);

  explicit CompanionSessions(uint16_t port = 5000, uint32_t ioTimeoutMs = 10000,
                             DiagnosticHandler diagnostic = nullptr);
  ~CompanionSessions();
  CompanionSessions(const CompanionSessions&) = delete;
  CompanionSessions& operator=(const CompanionSessions&) = delete;

  // Initialize the network stack before begin(). Attach through the native
  // startInterface(). Replacing the native mesh requires resetNativeSession().
  // The owner serializes begin/end; these lifecycle operations are not
  // radio-dispatch callbacks.
  bool begin();
  void end();
  // No socket I/O on the dispatch task, and no native identity/journal reset.
  void setNetworkAvailable(bool available);
  // Dispatch task only, after the old handler returns and its source detaches.
  // No old native callbacks may follow. Retires clients without socket I/O or
  // waiting for the worker; stays disabled until the new startInterface/enable.
  void resetNativeSession();
  uint16_t port() const;
  Stats stats() const;

  void enable() override;
  void disable() override;
  bool isEnabled() const override;
  bool isConnected() const override;
  bool isWriteBusy() const override;
  void loop() override {}
  size_t writeFrame(const uint8_t data[], size_t size) override;
  size_t checkRecvFrame(uint8_t data[]) override;

private:
  struct Frame {
    uint16_t size = 0;
    uint8_t data[MAX_FRAME_SIZE]{};
  };
  struct Owner {
    int slot = -1;
    uint32_t generation = 0;
  };
  struct Session {
    Frame command;
    Frame output[OutputDepth];
    unsigned head = 0, count = 0;
    uint32_t generation = 0;
    uint64_t cursor = 0;
    uint8_t version = 0;
    bool active = false, closing = false, pending = false;
    bool appStarted = false, notified = false, cursorStarted = false;
  };
  struct Socket {
    int fd = -1;
    uint8_t input[MAX_FRAME_SIZE + 3]{};
    uint8_t output[MAX_FRAME_SIZE + 3]{};
    unsigned received = 0, expected = 0, size = 0, written = 0;
    uint32_t inputSince = 0, outputSince = 0;
  };
  enum class Operation { None, Client, Configure, Collect };
  enum Diagnostic : unsigned {
    SocketFailure = 1, BadFrame = 2, Timeout = 4, OutputFull = 8,
    JournalFull = 16, NativeFailure = 32, TooManyClients = 64, RFTimeout = 128,
    NativeReset = 256
  };

  class Guard {
    const CompanionSessions& owner;
  public:
    explicit Guard(const CompanionSessions& value) : owner(value) { owner.lock(); }
    ~Guard() { owner.unlock(); }
  };
  void lock() const;
  void unlock() const;
  static uint32_t now();
  static void taskEntry(void* argument);
  void networkLoop();
  void networkPass();
  void closeSocket(unsigned slot);
  void reportDiagnostics();

  bool owns(Owner owner) const;
  bool enqueue(unsigned slot, const uint8_t* data, size_t size);
  void drop(unsigned slot, Diagnostic reason);
  void nativeFailure();
  void publish(const uint8_t* data, size_t size);
  void notify(unsigned slot);
  void sync(unsigned slot);
  bool matchesRFReply(const uint8_t* data, size_t size) const;
  uint64_t oldest() const;

#if defined(COMPANION_SESSIONS_HOST)
  mutable std::mutex mutex;
  std::thread worker;
#else
  mutable portMUX_TYPE mutex = portMUX_INITIALIZER_UNLOCKED;
  TaskHandle_t worker = nullptr;
#endif
  const uint16_t requestedPort;
  const uint32_t timeoutMs;
  DiagnosticHandler diagnostic;
  uint16_t boundPort = 0;
  int listener = -1;
  bool enabled = false, stopping = false, running = false, networkAvailable = true;
  Session clients[MaxClients];
  Socket sockets[MaxClients];
  Frame journal[JournalDepth];
  uint64_t sequence = 0;
  unsigned nextClient = 0;
  Operation operation = Operation::None;
  Owner requester, contactsOwner;
  uint8_t currentCommand = 0;
  uint8_t currentRFReply = 0, currentRFPeer[6]{};
  bool localTelemetryPending = false, rfPending = false;
  uint8_t rfReply = 0, rfPeer[6]{};
  uint32_t rfTag = 0, rfStarted = 0, rfDuration = 0;
  bool contactsActive = false, configured = false, configureReplied = false;
  bool collecting = true, collectReplied = false;
  unsigned collectedInTurn = 0;
  uint32_t lastPoll = 0, contactsSince = 0, lastDiagnostic = 0;
  unsigned diagnostics = 0;
  Stats counters;
};

}
