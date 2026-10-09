// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "PacketPipeline.h"
#include <stddef.h>

namespace onchip {
constexpr size_t PacketLuaSourceLimit = 16384;
struct PacketLuaStats {
  uint32_t sourceBytes = 0, sessionBytes = 0, heapLimit = 0, liveBytes = 0, peakBytes = 0;
  uint32_t instructions = 0, nativeCalls = 0, parserSteps = 0;
  uint64_t loadUs = 0, invokeUs = 0;
  char error[128]{};
};
class PacketLua final : public packet_engine::Engine {
  struct Impl;
  Impl *impl_ = nullptr;
  PacketLuaStats stats_;
public:
  PacketLua() = default;
  PacketLua(const PacketLua &) = delete;
  PacketLua &operator=(const PacketLua &) = delete;
  ~PacketLua() override;
  bool load(const char *, size_t, char *, size_t, uint32_t heapBytes = 65536,
            uint32_t initFuel = 100000, uint32_t initUs = 20000);
  void clear();
  bool loaded() const { return impl_ != nullptr; }
  const PacketLuaStats &stats() const { return stats_; }
  packet_engine::Decision process(const packet_engine::Metadata &, packet_engine::Call &) override;
};
} // namespace onchip
