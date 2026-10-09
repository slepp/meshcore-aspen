// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "PacketPipeline.h"
#include <stddef.h>

namespace onchip {
constexpr size_t PacketWasmSourceLimit = 16384;
struct PacketWasmStats {
  uint32_t sourceBytes = 0, sessionBytes = 0, linearBytes = 0, stackBytes = 0;
  uint32_t poolBytes = 0, poolHighWaterBytes = 0;
  uint32_t instructions = 0, nativeCalls = 0;
  uint64_t loadUs = 0, initUs = 0, invokeUs = 0;
  char error[128]{};
};
class PacketWasm final : public packet_engine::Engine {
  struct Impl;
  Impl *impl_ = nullptr;
  PacketWasmStats stats_;
public:
  PacketWasm() = default;
  PacketWasm(const PacketWasm &) = delete;
  PacketWasm &operator=(const PacketWasm &) = delete;
  ~PacketWasm() override;
  bool load(const uint8_t *, size_t, char *, size_t,
            uint32_t initFuel = 100000, uint32_t initUs = 20000);
  void clear();
  bool loaded() const { return impl_ != nullptr; }
  const PacketWasmStats &stats() const { return stats_; }
  packet_engine::Decision process(const packet_engine::Metadata &, packet_engine::Call &) override;
};
} // namespace onchip
