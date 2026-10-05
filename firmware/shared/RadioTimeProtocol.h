// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <cstring>

namespace radio_time {
constexpr uint8_t RequestType = 4, ProtocolVersion = 1;
constexpr size_t RequestSize = 14, ResponseSize = 32;
constexpr uint32_t DeadlineMs = 5000, RefreshMs = 900000, MaxAirtimeMs = 1000;
inline uint32_t get32(const uint8_t *p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
inline void put32(uint8_t *p, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) p[i] = value >> (i * 8);
}
inline bool request(const uint8_t *data, size_t size) {
  if (size < RequestSize || size > 16 || data[4] != RequestType || data[5] != ProtocolVersion) return false;
  for (size_t i = RequestSize; i < size; ++i) if (data[i]) return false;
  return true;
}
inline void response(const uint8_t *request, uint8_t *reply, uint8_t source,
                     uint32_t lower, uint32_t upper, uint32_t age, uint32_t remainingMs) {
  memset(reply, 0, ResponseSize);
  memcpy(reply, request, 6);
  reply[6] = source;
  reply[7] = source ? 0 : 1; // unsynchronized or expired authority
  memcpy(reply + 8, request + 6, 8);
  put32(reply + 16, lower); put32(reply + 20, upper);
  put32(reply + 24, age); put32(reply + 28, remainingMs);
}
class PendingTimeRequest {
  uint8_t provider_[32]{}, nonce_[8]{};
  uint32_t tag_ = 0, started_ = 0;
  bool pending_ = false;
public:
  void begin(const uint8_t *provider, const uint8_t *nonce, uint32_t tag, uint32_t now) {
    memcpy(provider_, provider, 32); memcpy(nonce_, nonce, 8);
    tag_ = tag; started_ = now; pending_ = true;
  }
  void cancel() { pending_ = false; }
  bool pending(uint32_t now) {
    if (uint32_t(now - started_) >= DeadlineMs) pending_ = false;
    return pending_;
  }
  bool accept(const uint8_t *provider, const uint8_t *data, size_t size, bool zeroHopDirect,
              uint32_t now, uint32_t &lower, uint32_t &upper, uint32_t &ttl) {
    lower = upper = ttl = 0;
    if (!pending(now) || !zeroHopDirect || size != ResponseSize ||
        memcmp(provider_, provider, 32) || get32(data) != tag_ ||
        data[4] != RequestType || data[5] != ProtocolVersion ||
        memcmp(data + 8, nonce_, 8)) return false;
    // A correlated error or invalid authority completes the request as well.
    pending_ = false;
    if (data[7] || (data[6] != 1 && data[6] != 2)) return false;
    const uint32_t rtt = uint32_t(now - started_), age = get32(data + 24), remaining = get32(data + 28);
    const uint64_t lo = get32(data + 16), hi = uint64_t(get32(data + 20)) + (rtt + 999) / 1000;
    if (lo < 1715770351u || hi > 4102444800u || hi < lo || hi - lo > 32 ||
        age > 172800 || !remaining || remaining > 172800000u || remaining <= rtt ||
        (data[6] == 2 && age >= 3600)) return false;
    lower = lo; upper = hi;
    ttl = remaining - rtt;
    if (ttl > 3600000u) ttl = 3600000u;
    return true;
  }
};
} // namespace radio_time
