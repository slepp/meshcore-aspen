// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <QueuedTxProtocol.h>
#include <stddef.h>
#include <stdint.h>

namespace nrfmast {
namespace peer {
constexpr uint8_t Version = 1, Slots = 2;
constexpr uint16_t PacketCapacity = 255, FrameCapacity = 280;
constexpr uint16_t EncodedCapacity = 2 * FrameCapacity + 2;
constexpr uint32_t MaxLifetimeMs = 60000, ResponseTimeoutMs = 5000;

struct Result {
  uint32_t job, rfMs;
  uint8_t state, reason;
  bool rfKnown;
  Result(uint32_t id = 0, uint32_t airtime = 0, uint8_t outcome = queued_tx::UNKNOWN,
         uint8_t cause = queued_tx::NONE, bool known = false)
      : job(id), rfMs(airtime), state(outcome), reason(cause), rfKnown(known) {}
};

class Host {
public:
  virtual ~Host() = default;
  // All callbacks run on the dispatch thread and must not re-enter Link.
  // Copy the entire frame or nothing. An accepted frame is sent once, in order.
  virtual bool write(const uint8_t *encoded, uint16_t length) = 0;
  // Admit raw egress without native routing again; copy before returning NONE.
  virtual uint8_t transmit(const uint8_t *, uint16_t length, uint8_t origin,
                           uint32_t delayMs, uint32_t lifetimeMs, uint32_t &job) = 0;
  // Stop unsent peer work. A transmission already on air may finish.
  virtual void closed(const char *reason) = 0;
};

class Link {
  struct Outbound {
    uint32_t job = 0, deadline = 0;
    bool accepted = false, acceptancePending = false, terminal = false;
    Result result;
  } outbound_[Slots];
  struct Inbound {
    uint32_t job = 0, localJob = 0;
    bool acceptancePending = false, terminal = false;
    Result result;
  } inbound_[Slots];
  Host &host_;
  uint32_t session_ = 0, remote_ = 0, nextJob_ = 0, lastInbound_ = 0, helloDeadline_ = 0;
  uint8_t identity_[32]{}, frame_[FrameCapacity]{}, encoded_[EncodedCapacity]{};
  uint16_t frameLength_ = 0;
  bool active_ = false, helloPending_ = false, helloSent_ = false;
  bool inside_ = false, escaped_ = false, discard_ = false;
  const char *error_ = "peer link offline";
  uint32_t malformed_ = 0, stale_ = 0;
  bool send(const uint8_t *, uint16_t length);
  void receive(const uint8_t *, uint16_t length, uint32_t now);
  bool event(uint32_t job, const Result &);
  void fail(const char *);
public:
  explicit Link(Host &host) : host_(host) {}
  Link(const Link &) = delete;
  Link &operator=(const Link &) = delete;
  // Call only after authenticating the pinned BLE peer; use a fresh random nonce.
  bool begin(uint32_t session, const uint8_t virtualIdentity[32], uint32_t now);
  void disconnect() { fail("peer BLE connection lost; transmission outcome may be uncertain"); }
  bool ready() const { return active_ && remote_ && helloSent_; }
  bool active() const { return active_; }
  const char *error() const { return error_; }
  uint32_t malformedFrames() const { return malformed_; }
  uint32_t staleFrames() const { return stale_; }
  uint8_t available() const;
  void feed(const uint8_t *, size_t length, uint32_t now);
  void poll(uint32_t now);
  bool submit(const uint8_t *, uint16_t length, uint8_t origin, uint32_t delayMs,
              uint32_t lifetimeMs, uint32_t now, uint32_t &job);
  bool complete(uint32_t localJob, const Result &);
  bool result(Result &);
};
} // namespace peer
} // namespace nrfmast
