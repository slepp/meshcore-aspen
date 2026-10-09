// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "PacketPipeline.h"
#include <cassert>
#include <vector>

struct PacketServiceHost : packet_engine::Host {
  uint32_t now = 0, step = 0, composed = 0, controls = 0;
  bool accept = true, available = true;
  packet_engine::PhyChange changed;
  std::vector<packet_engine::Fault> errors;
  std::vector<packet_engine::Emission> sent;
  uint32_t microsNow() override { return now += step; }
  void fault(const char *, const packet_engine::Metadata &, packet_engine::Fault error) override {
    errors.push_back(error);
  }
  bool admit(const packet_engine::Metadata &, const packet_engine::Emission *packets, uint8_t count) override {
    if (!accept) return false;
    sent.insert(sent.end(), packets, packets + count); return true;
  }
  bool system(packet_engine::SystemInfo &info) override {
    if (!available) return false;
    info = {};
    info.uptimeMs = 123; info.unixTime = 1791500000;
    info.freeInternalBytes = 456; info.freePsramBytes = 789;
    info.enabledRoles = 15; info.readyRoles = 7;
    info.phy = {910525000, 62500, 7, 5, 22};
    info.generation = 21; info.flags = 13; return true;
  }
  packet_engine::Fault compose(const packet_engine::ComposeRequest &request,
      const uint8_t *data, uint16_t length, uint8_t *out, uint16_t &capacity) override {
    using namespace packet_engine;
    if (!available || request.identity[31] != 0xa5) return Fault::Unavailable;
    assert(request.kind == uint32_t(ComposeKind::Advert) && request.payloadType == 4 &&
           request.route == 2 && request.pathWidth == 2 && request.pathCount == 1 &&
           request.path[0] == 0x61 && request.path[1] == 0x62 && request.timestamp == 1791500000);
    if (capacity < length + 4) return Fault::Bounds;
    out[0] = 0x12; out[1] = 0x41; memcpy(out + 2, request.path, 2);
    memcpy(out + 4, data, length); capacity = length + 4; ++composed;
    return Fault::None;
  }
  packet_engine::Fault commit(const packet_engine::Metadata &metadata, const packet_engine::Emission *packets,
                             uint8_t count, const packet_engine::PhyChange *phy) override {
    using packet_engine::Fault;
    if (phy && phy->generation != 21) return Fault::ControlRejected;
    if (count && !admit(metadata, packets, count)) return Fault::EmissionRejected;
    if (!accept) return Fault::ControlRejected;
    if (phy) { changed = *phy; ++controls; }
    return Fault::None;
  }
};
