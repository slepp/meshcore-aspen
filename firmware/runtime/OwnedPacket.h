// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "PacketPipeline.h"
#include "NativePacketContracts.h"
#include <Mesh.h>
#include <Utils.h>

namespace onchip {
inline packet_engine::Fault composeOwnedPacket(
    const mesh::LocalIdentity &identity, uint32_t nativeTimestamp,
    const packet_engine::ComposeRequest &request, const uint8_t *data, uint16_t length,
    uint8_t *output, uint16_t &capacity, const mesh::GroupChannel *channel = nullptr) {
  using namespace packet_engine;
  if (!identity.matches(request.identity)) return Fault::Unavailable;
  if ((!data && length) || !output || capacity > Capacity ||
      request.kind > uint32_t(ComposeKind::Group) || request.payloadType > 15 ||
      (request.route != ROUTE_TYPE_FLOOD && request.route != ROUTE_TYPE_DIRECT) ||
      request.pathWidth < 1 || request.pathWidth > 3 || request.pathCount > 63 ||
      request.pathCount * request.pathWidth > MAX_PATH_SIZE ||
      (request.route == ROUTE_TYPE_FLOOD && request.pathCount))
    return Fault::Bounds;
  const auto kind = ComposeKind(request.kind);
  const auto type = uint8_t(request.payloadType);
  Metadata envelope;
  envelope.stage = Stage::PlainCompose; envelope.payloadType = type;
  const uint8_t empty = 0;
  if (validateNativePacket(envelope, data ? data : &empty, length, false) != Fault::None)
    return Fault::InvalidPacket;
  uint16_t payloadLength = 0, prefix = 0;
  if (kind == ComposeKind::Advert) {
    if (type != PAYLOAD_TYPE_ADVERT || length > MAX_ADVERT_DATA_SIZE)
      return Fault::InvalidPacket;
    payloadLength = PUB_KEY_SIZE + 4 + SIGNATURE_SIZE + length;
    if (!request.timestamp && !nativeTimestamp) return Fault::Unavailable;
  } else {
    if (kind == ComposeKind::Datagram) {
      if (type != PAYLOAD_TYPE_TXT_MSG && type != PAYLOAD_TYPE_REQ && type != PAYLOAD_TYPE_RESPONSE)
        return Fault::InvalidPacket;
      prefix = 2 * PATH_HASH_SIZE;
    } else if (kind == ComposeKind::Anonymous) {
      if (type != PAYLOAD_TYPE_ANON_REQ) return Fault::InvalidPacket;
      prefix = PATH_HASH_SIZE + PUB_KEY_SIZE;
    } else {
      if (type != PAYLOAD_TYPE_GRP_TXT && type != PAYLOAD_TYPE_GRP_DATA)
        return Fault::InvalidPacket;
      if (!channel) return Fault::Unavailable;
      prefix = PATH_HASH_SIZE;
    }
    if (!length || length > (MAX_PACKET_PAYLOAD - prefix - CIPHER_MAC_SIZE) /
                            CIPHER_BLOCK_SIZE * CIPHER_BLOCK_SIZE)
      return Fault::Bounds;
    payloadLength = prefix + CIPHER_MAC_SIZE +
                    (length + CIPHER_BLOCK_SIZE - 1) / CIPHER_BLOCK_SIZE * CIPHER_BLOCK_SIZE;
  }
  const uint16_t wireLength = 2 + request.pathCount * request.pathWidth + payloadLength;
  if (payloadLength > MAX_PACKET_PAYLOAD || wireLength > capacity || wireLength > Capacity)
    return Fault::Bounds;
  mesh::Packet packet;
  packet.header = (type << PH_TYPE_SHIFT) | request.route;
  packet.setPathHashSizeAndCount(request.pathWidth, request.pathCount);
  if (request.pathCount) memcpy(packet.path, request.path, request.pathCount * request.pathWidth);
  if (kind == ComposeKind::Advert) {
    const uint32_t timestamp = request.timestamp ? request.timestamp : nativeTimestamp;
    uint8_t message[PUB_KEY_SIZE + 4 + MAX_ADVERT_DATA_SIZE]{};
    memcpy(message, identity.pub_key, PUB_KEY_SIZE);
    for (unsigned i = 0; i < 4; ++i) message[PUB_KEY_SIZE + i] = uint8_t(timestamp >> (8 * i));
    if (length) memcpy(message + PUB_KEY_SIZE + 4, data, length);
    memcpy(packet.payload, message, PUB_KEY_SIZE + 4);
    identity.sign(packet.payload + PUB_KEY_SIZE + 4, message, PUB_KEY_SIZE + 4 + length);
    if (length) memcpy(packet.payload + PUB_KEY_SIZE + 4 + SIGNATURE_SIZE, data, length);
  } else {
    uint8_t secret[PUB_KEY_SIZE]{};
    if (kind == ComposeKind::Group) {
      memcpy(secret, channel->secret, sizeof(secret));
      memcpy(packet.payload, channel->hash, PATH_HASH_SIZE);
    } else {
      uint8_t nonzero = 0;
      for (uint8_t byte : request.destination) nonzero |= byte;
      if (!nonzero || identity.matches(request.destination)) return Fault::InvalidPacket;
      identity.calcSharedSecret(secret, request.destination);
      nonzero = 0;
      for (uint8_t byte : secret) nonzero |= byte;
      if (!nonzero) return Fault::InvalidPacket;
      memcpy(packet.payload, request.destination, PATH_HASH_SIZE);
      memcpy(packet.payload + PATH_HASH_SIZE, identity.pub_key,
             kind == ComposeKind::Anonymous ? PUB_KEY_SIZE : PATH_HASH_SIZE);
    }
    const int encrypted = mesh::Utils::encryptThenMAC(secret, packet.payload + prefix, data, length);
    auto *wipe = static_cast<volatile uint8_t *>(secret);
    for (unsigned i = 0; i < sizeof(secret); ++i) wipe[i] = 0;
    if (encrypted != payloadLength - prefix) return Fault::InvalidPacket;
  }
  packet.payload_len = payloadLength;
  const uint16_t written = packet.writeTo(output);
  if (written != wireLength) return Fault::InvalidPacket;
  capacity = written;
  return Fault::None;
}
} // namespace onchip
