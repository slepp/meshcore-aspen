// SPDX-License-Identifier: Apache-2.0
#include "PinePeerLink.h"
#include <string.h>

namespace nrfmast {
namespace peer {
namespace {
constexpr uint8_t End = 0xc0, Escape = 0xdb, EscapedEnd = 0xdc, EscapedEscape = 0xdd;
bool terminal(uint8_t state) {
  return state == queued_tx::REJECTED || state == queued_tx::SUCCEEDED ||
         state == queued_tx::FAILED || state == queued_tx::UNKNOWN;
}
bool validResult(const Result &r) {
  return r.state <= queued_tx::UNKNOWN && r.reason <= queued_tx::ENGINE_DROP &&
         (r.state == queued_tx::SUCCEEDED ? r.reason == queued_tx::NONE && r.rfKnown :
          r.state == queued_tx::ACCEPTED ? r.reason == queued_tx::NONE && !r.rfKnown && !r.rfMs :
          r.reason != queued_tx::NONE) &&
         (r.rfKnown || !r.rfMs) &&
         (r.state != queued_tx::UNKNOWN || !r.rfKnown) &&
         (r.state != queued_tx::FAILED || r.rfKnown) &&
         (r.state != queued_tx::REJECTED || (!r.rfKnown && !r.rfMs));
}
}

bool Link::begin(uint32_t session, const uint8_t identity[32], uint32_t now) {
  if (active_ || !session || session == session_ || !identity) return false;
  for (const auto &tx : outbound_) if (tx.job) return false;
  bool nonzero = false;
  for (unsigned i = 0; i < 32; ++i) nonzero |= identity[i] != 0;
  if (!nonzero) return false;
  session_ = session; remote_ = nextJob_ = lastInbound_ = 0;
  memcpy(identity_, identity, sizeof(identity_));
  for (auto &rx : inbound_) rx = {};
  frameLength_ = 0; inside_ = escaped_ = discard_ = false;
  active_ = helloPending_ = true; helloSent_ = false;
  helloDeadline_ = now + ResponseTimeoutMs;
  error_ = "peer identity handshake pending";
  return true;
}

void Link::fail(const char *reason) {
  if (!active_) return;
  active_ = false; error_ = reason;
  for (auto &tx : outbound_) {
    if (!tx.job || tx.terminal) continue;
    tx.result = {tx.job, 0, queued_tx::UNKNOWN, queued_tx::DISCONNECTED, false};
    tx.terminal = true;
  }
  host_.closed(reason);
}

uint8_t Link::available() const {
  if (!ready()) return 0;
  uint8_t count = 0;
  for (const auto &tx : outbound_) if (!tx.job) ++count;
  return count;
}

bool Link::send(const uint8_t *bytes, uint16_t length) {
  uint16_t used = 0;
  encoded_[used++] = End;
  for (uint16_t i = 0; i < length; ++i) {
    const auto byte = bytes[i];
    if (byte == End || byte == Escape) {
      encoded_[used++] = Escape;
      encoded_[used++] = byte == End ? EscapedEnd : EscapedEscape;
    } else encoded_[used++] = byte;
  }
  encoded_[used++] = End;
  return host_.write(encoded_, used);
}

bool Link::event(uint32_t job, const Result &result) {
  uint8_t bytes[21]{queued_tx::EVENT, Version};
  queued_tx::put32(bytes + 2, session_);
  queued_tx::put32(bytes + 6, remote_);
  queued_tx::put32(bytes + 10, job);
  bytes[14] = result.state; bytes[15] = result.reason;
  queued_tx::put32(bytes + 16, result.rfMs); bytes[20] = result.rfKnown;
  return send(bytes, sizeof(bytes));
}

void Link::poll(uint32_t now) {
  if (!active_) return;
  if (!ready() && int32_t(now - helloDeadline_) >= 0) {
    fail("peer identity handshake timed out"); return;
  }
  for (const auto &tx : outbound_) {
    if (tx.job && !tx.terminal && int32_t(now - tx.deadline) >= 0) {
      fail("peer transmit result timed out; reconnect without replaying packets"); return;
    }
  }
  if (helloPending_) {
    uint8_t bytes[39]{queued_tx::HELLO, Version};
    queued_tx::put32(bytes + 2, session_); bytes[6] = Slots;
    memcpy(bytes + 7, identity_, sizeof(identity_));
    if (!send(bytes, sizeof(bytes))) return;
    helloPending_ = false; helloSent_ = true;
    if (remote_) error_ = "peer link ready";
  }
  if (!ready()) return;
  for (auto &rx : inbound_) {
    if (!rx.job) continue;
    if (rx.acceptancePending) {
      Result accepted{rx.job, 0, queued_tx::ACCEPTED, queued_tx::NONE, false};
      if (!event(rx.job, accepted)) return;
      rx.acceptancePending = false;
    }
    if (rx.terminal) {
      if (!event(rx.job, rx.result)) return;
      rx = {};
    }
  }
}

bool Link::submit(const uint8_t *bytes, uint16_t length, uint8_t origin,
                  uint32_t delay, uint32_t lifetime, uint32_t now, uint32_t &job) {
  if (!available() || !bytes || !length || length > PacketCapacity ||
      origin > 3 || !lifetime || lifetime > MaxLifetimeMs || delay >= lifetime ||
      nextJob_ == UINT32_MAX) return false;
  Outbound *slot = nullptr;
  for (auto &tx : outbound_) if (!tx.job) { slot = &tx; break; }
  uint8_t frame[FrameCapacity]{queued_tx::SUBMIT, Version};
  queued_tx::put32(frame + 2, session_); queued_tx::put32(frame + 6, remote_);
  queued_tx::put32(frame + 10, nextJob_ + 1);
  queued_tx::put32(frame + 14, delay); queued_tx::put32(frame + 18, lifetime);
  queued_tx::put16(frame + 22, length); frame[24] = origin;
  memcpy(frame + 25, bytes, length);
  if (!send(frame, length + 25)) return false;
  slot->job = ++nextJob_;
  slot->deadline = now + lifetime + ResponseTimeoutMs;
  job = slot->job;
  return true;
}

bool Link::complete(uint32_t localJob, const Result &result) {
  if (!active_ || !localJob || !terminal(result.state) ||
      result.state == queued_tx::REJECTED || !validResult(result)) return false;
  for (auto &rx : inbound_) {
    if (rx.localJob != localJob || rx.terminal) continue;
    rx.result = result; rx.result.job = rx.job; rx.terminal = true;
    return true;
  }
  return false;
}

bool Link::result(Result &result) {
  for (auto &tx : outbound_) {
    if (!tx.job) continue;
    if (tx.acceptancePending) {
      result = {tx.job, 0, queued_tx::ACCEPTED, queued_tx::NONE, false};
      tx.acceptancePending = false; return true;
    }
    if (tx.terminal) { result = tx.result; tx = {}; return true; }
  }
  return false;
}

void Link::receive(const uint8_t *bytes, uint16_t length, uint32_t) {
  if (!active_) return;
  if (length < 2 || bytes[1] != Version) { ++malformed_; return; }
  if (bytes[0] == queued_tx::HELLO) {
    if (length != 39 || !queued_tx::get32(bytes + 2) || bytes[6] != Slots) {
      ++malformed_; return;
    }
    if (memcmp(bytes + 7, identity_, sizeof(identity_))) {
      fail("peer virtual repeater identity does not match"); return;
    }
    const auto remote = queued_tx::get32(bytes + 2);
    if (remote_ && remote != remote_) {
      fail("peer session changed; reconnect without replaying packets"); return;
    }
    remote_ = remote;
    if (ready()) error_ = "peer link ready";
    return;
  }
  if (length < 14) { ++malformed_; return; }
  if (!ready() || queued_tx::get32(bytes + 2) != remote_ ||
      queued_tx::get32(bytes + 6) != session_) { ++stale_; return; }
  const auto job = queued_tx::get32(bytes + 10);
  if (!job) { ++malformed_; return; }
  if (bytes[0] == queued_tx::EVENT) {
    if (length != 21 || bytes[20] > 1) { ++malformed_; return; }
    const Result result{job, queued_tx::get32(bytes + 16), bytes[14], bytes[15], bytes[20] != 0};
    if (!validResult(result)) { ++malformed_; return; }
    for (auto &tx : outbound_) {
      if (tx.job != job || tx.terminal) continue;
      if (result.state == queued_tx::ACCEPTED) {
        if (!tx.accepted) tx.accepted = tx.acceptancePending = true;
      } else if ((result.state == queued_tx::REJECTED && !tx.accepted) ||
                 (result.state != queued_tx::REJECTED && tx.accepted)) {
        tx.result = result; tx.terminal = true;
      } else { ++malformed_; }
      return;
    }
    ++stale_; return;
  }
  if (bytes[0] != queued_tx::SUBMIT || length < 26) { ++malformed_; return; }
  const auto delay = queued_tx::get32(bytes + 14), lifetime = queued_tx::get32(bytes + 18);
  const auto size = queued_tx::get16(bytes + 22);
  if (!size || size > PacketCapacity || length != size + 25 ||
      bytes[24] > 3 || !lifetime || lifetime > MaxLifetimeMs || delay >= lifetime) {
    ++malformed_; return;
  }
  if (job <= lastInbound_) { ++stale_; return; }
  Inbound *slot = nullptr;
  for (auto &rx : inbound_) if (!rx.job) { slot = &rx; break; }
  if (!slot) {
    fail("peer exceeded transmit credits"); return;
  }
  lastInbound_ = job;
  slot->job = job;
  uint32_t localJob = 0;
  const auto reason = host_.transmit(bytes + 25, size, bytes[24], delay, lifetime, localJob);
  if (reason > queued_tx::ENGINE_DROP || (reason == queued_tx::NONE && !localJob)) {
    fail("peer radio returned an invalid admission result"); return;
  }
  if (reason == queued_tx::NONE && localJob) {
    for (const auto &rx : inbound_) {
      if (&rx != slot && rx.localJob == localJob) {
        fail("peer radio returned a duplicate transmit job"); return;
      }
    }
    slot->localJob = localJob; slot->acceptancePending = true;
  } else {
    slot->terminal = true;
    slot->result = {job, 0, queued_tx::REJECTED, reason, false};
  }
}

void Link::feed(const uint8_t *bytes, size_t length, uint32_t now) {
  if (!bytes || !active_) return;
  for (size_t i = 0; i < length; ++i) {
    const auto byte = bytes[i];
    if (byte == End) {
      if (inside_ && frameLength_ && !discard_ && !escaped_)
        receive(frame_, frameLength_, now);
      else if (inside_ && escaped_ && !discard_) ++malformed_;
      frameLength_ = 0; escaped_ = discard_ = false; inside_ = true;
      if (!active_) return;
      continue;
    }
    if (!inside_ || discard_) continue;
    uint8_t decoded = byte;
    if (escaped_) {
      escaped_ = false;
      if (byte != EscapedEnd && byte != EscapedEscape) {
        discard_ = true; ++malformed_; continue;
      }
      decoded = byte == EscapedEnd ? End : Escape;
    } else if (byte == Escape) { escaped_ = true; continue; }
    if (frameLength_ == FrameCapacity) { discard_ = true; ++malformed_; }
    else frame_[frameLength_++] = decoded;
  }
}
} // namespace peer
} // namespace nrfmast
