// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "WifiKissMultiplexer.h"

namespace onchip {
class NativePacketHost final : public packet_engine::Host {
public:
  using Clock = uint32_t (*)();
  using Report = void (*)(const char *, const packet_engine::Metadata &, packet_engine::Fault);
  struct Status {
    uint32_t faults = 0;
    char engine[32]{};
    packet_engine::Metadata packet;
    packet_engine::Fault lastFault = packet_engine::Fault::None;
  };
private:
  WifiKissMultiplexer &mux_;
  Clock clock_;
  Report report_;
  packet_engine::Pipeline pipeline_;
  Status status_;
  bool running_ = false;
public:
  NativePacketHost(WifiKissMultiplexer &mux, Clock clock, Report report)
      : mux_(mux), clock_(clock), report_(report), pipeline_(*this) {}
  ~NativePacketHost() override { stop(); }
  bool begin(float airtimeFactor = 1) {
    if (running_ || !clock_ || !report_) {
      Serial.println("Packet engine host already running or clock/fault reporter missing");
      return false;
    }
    if (!mux_.beginEngineSource(airtimeFactor)) return false;
    mux_.packetPipeline(&pipeline_);
    running_ = true;
    return true;
  }
  void stop() {
    if (!running_) return;
    mux_.packetPipeline(nullptr);
    mux_.stopEngineSource();
    running_ = false;
  }
  packet_engine::Pipeline &pipeline() { return pipeline_; }
  const Status &status() const { return status_; }
  uint32_t microsNow() override { return clock_ ? clock_() : 0; }
  void fault(const char *engine, const packet_engine::Metadata &metadata,
             packet_engine::Fault fault) override {
    ++status_.faults;
    const auto length = strnlen(engine, sizeof(status_.engine) - 1);
    memcpy(status_.engine, engine, length);
    status_.engine[length] = 0;
    status_.packet = metadata;
    status_.lastFault = fault;
    if (report_) report_(engine, metadata, fault);
    else Serial.println(packet_engine::faultText(fault));
  }
  bool admit(const packet_engine::Metadata &, const packet_engine::Emission *packets,
             uint8_t count) override {
    return running_ && mux_.admitEnginePackets(packets, count);
  }
};
} // namespace onchip
