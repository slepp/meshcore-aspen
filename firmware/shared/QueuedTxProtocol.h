#pragma once

#include <stdint.h>
#include <string.h>

#ifndef KISS_MAX_TCP_CLIENTS
#define KISS_MAX_TCP_CLIENTS 8
#endif
#ifndef KISS_REQUEST_QUEUE_DEPTH
#define KISS_REQUEST_QUEUE_DEPTH 12
#endif

namespace queued_tx {
constexpr uint8_t VERSION = 1;
constexpr uint8_t HELLO = 0x20;
constexpr uint8_t SUBMIT = 0x21;
constexpr uint8_t CONFIG = 0x22;
constexpr uint8_t SOURCE_POLICY = 0x23;
constexpr uint8_t STATS = 0x24;
constexpr uint8_t CAPACITY = 0x25;
constexpr uint8_t SESSION_PORTS = 4; // Includes the physical port 0.
constexpr uint8_t ROLE_PRESENCE = 0x26;
constexpr uint8_t ROLE_KEY_SIZE = 32;
constexpr uint8_t ROLE_COUNT = 4; // v1 role IDs, not a limit on instances.
constexpr uint8_t NATIVE_ROLE_PRESENT = 1u << 0;
constexpr uint8_t TCP_ROLE_PRESENT = 1u << 1;
constexpr uint8_t NATIVE_KEY_PRESENT = 1u << 2;
constexpr uint8_t TCP_KEY_PRESENT = 1u << 3;
constexpr uint8_t EVENT = 0xFA;
constexpr uint8_t LEGACY_PRIORITY = 4;
constexpr uint32_t MAX_DELAY_MS = 0x3FFFFFFF;
constexpr uint32_t WINDOW_MS = 3600000;
constexpr uint8_t PROFILE_SIZE = 18;
enum State : uint8_t { REJECTED, ACCEPTED, SUCCEEDED, FAILED, UNKNOWN };
enum Reason : uint8_t {
  NONE,
  INVALID,
  FULL,
  STALE,
  NOT_OWNER,
  BUSY,
  EXPIRED,
  START_FAILED,
  RF_TIMEOUT,
  DISCONNECTED,
  NOT_CONFIGURED
};
inline uint16_t get16(const uint8_t *p) {
  return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}
inline uint32_t get32(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}
inline void put16(uint8_t *p, uint16_t n) {
  p[0] = n;
  p[1] = n >> 8;
}
inline void put32(uint8_t *p, uint32_t n) {
  p[0] = n;
  p[1] = n >> 8;
  p[2] = n >> 16;
  p[3] = n >> 24;
}
inline float getFloat(const uint8_t *p) {
  const uint32_t bits = get32(p);
  float value;
  static_assert(sizeof(value) == sizeof(bits), "32-bit float required");
  memcpy(&value, &bits, sizeof(value));
  return value;
}
inline void putFloat(uint8_t *p, float value) {
  uint32_t bits;
  memcpy(&bits, &value, sizeof(bits));
  put32(p, bits);
}
} // namespace queued_tx
