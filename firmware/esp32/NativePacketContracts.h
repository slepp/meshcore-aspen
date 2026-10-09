// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "PacketPipeline.h"
#include <Packet.h>

namespace onchip {
inline bool validNativeWire(const uint8_t *bytes, uint16_t length) {
  if (!bytes || length < 3 || length > MAX_TRANS_UNIT || bytes[0] == 0xff ||
      (bytes[0] >> PH_VER_SHIFT) != PAYLOAD_VER_1) return false;
  const auto route = bytes[0] & PH_ROUTE_MASK;
  const uint16_t offset = (route == ROUTE_TYPE_TRANSPORT_FLOOD ||
                           route == ROUTE_TYPE_TRANSPORT_DIRECT) ? 5 : 1;
  if (offset >= length || !mesh::Packet::isValidPathLen(bytes[offset])) return false;
  const uint16_t path = (bytes[offset] & 63u) * ((bytes[offset] >> 6) + 1u);
  const uint16_t prefix = offset + 1 + path;
  return prefix < length && length - prefix <= MAX_PACKET_PAYLOAD;
}

inline packet_engine::Fault validateNativePacket(
    const packet_engine::Metadata &metadata, const uint8_t *bytes,
    uint16_t length, bool emission) {
  using packet_engine::Fault;
  if (emission || metadata.stage == packet_engine::Stage::Relay)
    return validNativeWire(bytes, length) ? Fault::None : Fault::InvalidPacket;
  if (!bytes || length >= MAX_PACKET_PAYLOAD) return Fault::InvalidPacket;
  if (metadata.payloadType == PAYLOAD_TYPE_ADVERT)
    return length <= MAX_ADVERT_DATA_SIZE ? Fault::None : Fault::InvalidPacket;
  if (!length) return Fault::InvalidPacket;
  switch (metadata.payloadType) {
    case PAYLOAD_TYPE_TXT_MSG: {
      if (length <= 5 || (bytes[4] >> 2) > 2) return Fault::InvalidPacket;
      const uint16_t prefix = (bytes[4] >> 2) == 2 ? 9 : 5;
      if (length <= prefix) return Fault::InvalidPacket;
      break;
    }
    case PAYLOAD_TYPE_REQ:
    case PAYLOAD_TYPE_RESPONSE:
      if (length < 4) return Fault::InvalidPacket;
      break;
    case PAYLOAD_TYPE_ANON_REQ:
      if (length < 5) return Fault::InvalidPacket;
      break;
    case PAYLOAD_TYPE_PATH: {
      if (!mesh::Packet::isValidPathLen(bytes[0])) return Fault::InvalidPacket;
      const uint16_t path = (bytes[0] & 63u) * ((bytes[0] >> 6) + 1u);
      if (path + 2u > length) return Fault::InvalidPacket;
      break;
    }
    case PAYLOAD_TYPE_GRP_TXT:
      if (length <= 5) return Fault::InvalidPacket;
      break;
    case PAYLOAD_TYPE_GRP_DATA:
      if (length < 3 || bytes[2] > length - 3) return Fault::InvalidPacket;
      break;
    default: break;
  }
  return Fault::None;
}
} // namespace onchip
