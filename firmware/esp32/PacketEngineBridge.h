// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <Packet.h>
#include <Utils.h>
#include <string.h>

namespace mesh {
enum class PacketEngineStage : uint8_t { Relay = 5, PlainReceive = 6, PlainCompose = 7 };

struct PacketOrigin {
  bool engine = false, reflection = false;
};
inline PacketOrigin mergePacketOrigin(PacketOrigin left, PacketOrigin right) {
  return {left.engine || right.engine, left.reflection || right.reflection};
}
class PacketOriginScope {
  PacketOrigin &current_;
  PacketOrigin previous_;
public:
  PacketOriginScope(PacketOrigin &current, PacketOrigin origin)
      : current_(current), previous_(current) { current_ = origin; }
  ~PacketOriginScope() { current_ = previous_; }
  PacketOriginScope(const PacketOriginScope &) = delete;
  PacketOriginScope &operator=(const PacketOriginScope &) = delete;
};
struct PacketEngineInfo {
  PacketEngineStage stage = PacketEngineStage::Relay;
  uint8_t type = 0;
  bool local = false;
  PacketOrigin origin;
  const uint8_t *identity = nullptr;
  float rssi = 0, snr = 0;
};
inline PacketEngineInfo packetEngineInfo(PacketEngineStage stage, const Packet &packet,
                                         const uint8_t *identity) {
  PacketEngineInfo info;
  info.stage = stage;
  info.type = packet.getPayloadType();
  info.local = packet._localReflection;
  info.origin = {packet._engineOrigin, packet._reflectionOrigin};
  info.identity = identity;
  if (!info.local) { info.rssi = packet._rssi; info.snr = packet.getSNR(); }
  return info;
}

struct OriginalPlaintext {
  const uint8_t *bytes = nullptr;
  size_t length = 0;
};
class OriginalPlaintextScope {
  OriginalPlaintext &current_;
  OriginalPlaintext previous_;
  uint8_t original_[MAX_PACKET_PAYLOAD]{};
  size_t length_;
public:
  OriginalPlaintextScope(OriginalPlaintext &current, const uint8_t *data, size_t length)
      : current_(current), previous_(current), length_(length) {
    memcpy(original_, data, length);
    current_ = {};
  }
  void changed(const uint8_t *data, size_t length) {
    if (length != length_ || memcmp(original_, data, length_)) {
      current_.bytes = original_;
      current_.length = length_;
    }
  }
  ~OriginalPlaintextScope() { current_ = previous_; }
};
class OriginalPathExtraScope {
  OriginalPlaintext &current_;
  OriginalPlaintext previous_;
public:
  explicit OriginalPathExtraScope(OriginalPlaintext &current)
      : current_(current), previous_(current) {
    if (!current.bytes || current.length < 2 ||
        !Packet::isValidPathLen(current.bytes[0])) return;
    const size_t prefix = 2 + (current.bytes[0] & 63u) * ((current.bytes[0] >> 6) + 1u);
    if (prefix > current.length) return;
    current_.bytes += prefix;
    current_.length -= prefix;
  }
  ~OriginalPathExtraScope() { current_ = previous_; }
};

inline size_t plaintextTextLength(const uint8_t *data, size_t length, size_t prefix = 5) {
  if (length < prefix) return 0;
  const auto *end = static_cast<const uint8_t *>(memchr(data + prefix, 0, length - prefix));
  return end ? size_t(end - data - prefix) : length - prefix;
}
inline void plaintextTextAck(uint8_t *ack, const uint8_t *data, size_t length,
                             const uint8_t *key, uint8_t *attempt = nullptr) {
  const size_t prefix = length >= 5 && (data[4] >> 2) == 2 ? 9 : 5;
  const size_t size = prefix + plaintextTextLength(data, length, prefix);
  Utils::sha256(ack, 4, data, size <= length ? size : length, key, PUB_KEY_SIZE);
  if (attempt) *attempt = size + 1 < length ? data[size + 1] : 0;
}
} // namespace mesh
