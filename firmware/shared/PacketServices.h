// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>

namespace packet_engine {
enum Capability : uint32_t { ReadSystem = 1, WritePhy = 2, ComposeOwned = 4 };
constexpr uint32_t AllCapabilities = ReadSystem | WritePhy | ComposeOwned;
struct Phy {
  uint32_t frequencyHz = 0, bandwidthHz = 0, spreadingFactor = 0,
           codingRate = 0, txPower = 0;
};
struct PhyChange {
  Phy phy;
  uint32_t generation = 0, persist = 0;
};
enum SystemFlag : uint32_t { RadioReady = 1, Transmitting = 2, TrustedTime = 4, HeapMeasured = 8 };
struct SystemInfo {
  uint32_t version = 1, uptimeMs = 0, unixTime = 0, freeInternalBytes = 0,
           freePsramBytes = 0, enabledRoles = 0, readyRoles = 0;
  Phy phy;
  uint32_t generation = 0, flags = 0;
};
enum class ComposeKind : uint32_t { Advert, Datagram, Anonymous, Group };
struct ComposeRequest {
  uint32_t kind = 0, payloadType = 0, route = 0, pathWidth = 1, pathCount = 0,
           channel = 0, timestamp = 0;
  uint8_t identity[32]{}, destination[32]{}, path[64]{};
};
static_assert(sizeof(Phy) == 20 && sizeof(PhyChange) == 28 &&
              sizeof(SystemInfo) == 56 && sizeof(ComposeRequest) == 156,
              "Packet service ABI uses fixed-width copied records");
} // namespace packet_engine
