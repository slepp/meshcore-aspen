// SPDX-License-Identifier: Apache-2.0
#include "PinePeerLink.h"
#include <assert.h>
#include <deque>
#include <string.h>
#include <vector>

using namespace nrfmast::peer;
struct Radio : Host {
  std::deque<std::vector<uint8_t>> wire;
  std::vector<uint8_t> packet;
  uint32_t nextJob = 0, delay = 0, lifetime = 0;
  uint8_t reason = queued_tx::NONE, origin = 0;
  bool writable = true;
  unsigned submitted = 0, closedCount = 0;
  const char *failure = nullptr;
  bool write(const uint8_t *bytes, uint16_t length) override {
    if (!writable) return false;
    wire.emplace_back(bytes, bytes + length); return true;
  }
  uint8_t transmit(const uint8_t *bytes, uint16_t length, uint8_t tag,
                   uint32_t wait, uint32_t ttl, uint32_t &job) override {
    ++submitted;
    if (reason) return reason;
    packet.assign(bytes, bytes + length);
    origin = tag; delay = wait; lifetime = ttl; job = ++nextJob;
    return queued_tx::NONE;
  }
  void closed(const char *text) override { ++closedCount; failure = text; }
};
static uint8_t identity[32]{1, 2, 3};
static void flush(Radio &radio, Link &target, uint32_t now = 0, size_t fragment = 20) {
  while (!radio.wire.empty()) {
    auto frame = radio.wire.front(); radio.wire.pop_front();
    for (size_t offset = 0; offset < frame.size(); offset += fragment) {
      const auto size = frame.size() - offset < fragment ? frame.size() - offset : fragment;
      target.feed(frame.data() + offset, size, now);
    }
  }
}
struct Pair {
  Radio a, b;
  Link left{a}, right{b};
  explicit Pair(uint32_t now = 0) {
    assert(left.begin(0xc0db0123, identity, now));
    assert(right.begin(0xdbabcdef, identity, now));
    left.poll(now); right.poll(now);
    flush(a, right, now, 1); flush(b, left, now, 1);
    assert(left.ready() && right.ready());
  }
};
static void resultsAndCredits() {
  Pair p;
  uint8_t bytes[PacketCapacity];
  for (unsigned i = 0; i < sizeof(bytes); ++i) bytes[i] = i;
  uint32_t job = 77, second = 0;
  assert(p.left.submit(bytes, sizeof(bytes), 3, 17, 30000, 0, job));
  assert(job == 1 && p.left.available() == 1);
  auto duplicate = p.a.wire.front();
  assert(p.left.submit(bytes, sizeof(bytes), 0, 0, 30000, 0, second));
  assert(second == 2 && !p.left.available());
  assert(!p.left.submit(bytes, 1, 0, 0, 30000, 0, second));
  flush(p.a, p.right, 0, 1);
  assert(p.b.submitted == 2 && p.b.packet.size() == sizeof(bytes));
  assert(!memcmp(bytes, p.b.packet.data(), sizeof(bytes)));
  p.right.feed(duplicate.data(), duplicate.size(), 0);
  assert(p.b.submitted == 2 && p.right.staleFrames() == 1);
  p.b.writable = false;
  p.right.poll(0);
  Result result;
  assert(!p.left.result(result));
  assert(p.right.complete(1, {0, 123, queued_tx::SUCCEEDED, queued_tx::NONE, true}));
  assert(!p.right.complete(1, {0, 123, queued_tx::SUCCEEDED, queued_tx::NONE, true}));
  assert(p.right.complete(2, {0, 0, queued_tx::UNKNOWN, queued_tx::RF_TIMEOUT, false}));
  p.b.writable = true; p.right.poll(0); flush(p.b, p.left, 0, 7);
  assert(p.left.result(result) && result.job == 1 && result.state == queued_tx::ACCEPTED);
  assert(p.left.available() == 0);
  assert(p.left.result(result) && result.job == 1 && result.state == queued_tx::SUCCEEDED);
  assert(result.rfKnown && result.rfMs == 123 && p.left.available() == 1);
  assert(p.left.result(result) && result.job == 2 && result.state == queued_tx::ACCEPTED);
  assert(p.left.result(result) && result.state == queued_tx::UNKNOWN && !result.rfKnown);
  assert(!p.left.result(result) && p.left.available() == Slots);
}
static void admissionAndPressure() {
  Pair p;
  uint8_t packet = 0xdb;
  uint32_t job = 99;
  p.a.writable = false;
  assert(!p.left.submit(&packet, 1, 0, 0, 30000, 0, job));
  assert(job == 99 && p.left.available() == Slots);
  p.a.writable = true; p.b.reason = queued_tx::FULL;
  assert(p.left.submit(&packet, 1, 2, 20, 400, 0, job) && job == 1);
  flush(p.a, p.right);
  p.right.poll(0); flush(p.b, p.left);
  Result result;
  assert(p.left.result(result) && result.state == queued_tx::REJECTED &&
         result.reason == queued_tx::FULL && !result.rfKnown);
  assert(!p.left.result(result));
  p.b.reason = queued_tx::NONE;
  assert(p.left.submit(&packet, 1, 2, 20, 400, 0, job));
  flush(p.a, p.right);
  assert(p.b.delay == 20 && p.b.lifetime == 400 && p.b.origin == 2);
  assert(!p.right.complete(2, {0, 0, queued_tx::SUCCEEDED, queued_tx::NONE, false}));
  assert(!p.right.complete(2, {0, 1, queued_tx::UNKNOWN, queued_tx::RF_TIMEOUT, true}));
  assert(!p.right.complete(2, {0, 0, queued_tx::FAILED, queued_tx::START_FAILED, false}));
  assert(p.right.complete(1, {0, 0, queued_tx::FAILED, queued_tx::START_FAILED, true}));
  p.right.poll(0); flush(p.b, p.left);
  assert(p.left.result(result) && result.state == queued_tx::ACCEPTED);
  assert(p.left.result(result) && result.state == queued_tx::FAILED && result.rfKnown);
}
static void disconnectAndTimeout() {
  Pair p(0xfffffff0);
  uint8_t packet = 1;
  uint32_t job = 0;
  assert(p.left.submit(&packet, 1, 0, 0, 100, 0xfffffff0, job));
  flush(p.a, p.right);
  p.right.poll(0xfffffff0); flush(p.b, p.left);
  p.left.poll(uint32_t(0xfffffff0u + 5099u));
  assert(p.left.ready());
  p.left.poll(uint32_t(0xfffffff0u + 5100u));
  assert(!p.left.active() && p.a.closedCount == 1);
  p.left.disconnect(); assert(p.a.closedCount == 1);
  assert(!p.left.begin(555, identity, 0));
  Result result;
  assert(p.left.result(result) && result.state == queued_tx::ACCEPTED);
  assert(p.left.result(result) && result.state == queued_tx::UNKNOWN);
  assert(p.left.begin(555, identity, 0));
  p.left.poll(0); flush(p.a, p.right);
  assert(!p.right.active() && strstr(p.b.failure, "session changed"));
  p.left.disconnect();
  assert(p.left.begin(888, identity, 0));
  assert(p.right.begin(777, identity, 0));
  p.left.poll(0); p.right.poll(0); flush(p.a, p.right); flush(p.b, p.left);
  assert(p.left.ready() && p.right.ready());
  assert(!p.left.result(result));
}
static void invalidFramesAndResync() {
  Pair p;
  const uint8_t badEscape[]{0xc0, 1, 0xdb, 4, 0xc0};
  p.right.feed(badEscape, sizeof(badEscape), 0);
  const uint8_t danglingEscape[]{0xc0, 1, 0xdb, 0xc0};
  p.right.feed(danglingEscape, sizeof(danglingEscape), 0);
  std::vector<uint8_t> large(FrameCapacity + 4, 0xaa);
  large.front() = large.back() = 0xc0;
  p.right.feed(large.data(), large.size(), 0);
  assert(p.right.malformedFrames() == 3);
  uint8_t packet = 1;
  uint32_t job = 0;
  assert(p.left.submit(&packet, 1, 0, 0, 100, 0, job));
  auto old = p.a.wire.front();
  flush(p.a, p.right);
  assert(p.b.submitted == 1);
  p.right.disconnect();
  assert(p.right.begin(42, identity, 0)); p.right.poll(0);
  p.right.feed(old.data(), old.size(), 0);
  assert(p.b.submitted == 1 && p.right.staleFrames() == 1);
  assert(!p.left.submit(&packet, 0, 0, 0, 100, 0, job));
  assert(!p.left.submit(&packet, 1, 4, 0, 100, 0, job));
  assert(!p.left.submit(&packet, 1, 0, 100, 100, 0, job));
  assert(!p.left.submit(&packet, 1, 0, 0, MaxLifetimeMs + 1, 0, job));
}
static void identityHandshake() {
  Radio a, b; Link left(a), right(b);
  uint8_t other[32]{9}, zero[32]{};
  assert(!left.begin(0, identity, 0));
  assert(!left.begin(1, zero, 0));
  assert(left.begin(1, identity, 0) && right.begin(2, other, 0));
  left.poll(0); flush(a, right);
  assert(!right.active() && strstr(b.failure, "identity"));
  left.poll(ResponseTimeoutMs);
  assert(!left.active() && strstr(a.failure, "timed out"));
}
int main() {
  static_assert(sizeof(Link) <= 1536, "Pine peer link exceeds its static RAM budget");
  resultsAndCredits(); admissionAndPressure(); disconnectAndTimeout();
  invalidFramesAndResync(); identityHandshake();
}
