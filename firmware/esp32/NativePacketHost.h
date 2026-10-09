// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "WifiKissMultiplexer.h"

namespace onchip {
class NativePacketHost final : public packet_engine::Host {
public:
  using Clock = uint32_t (*)();
  using Report = void (*)(const char *, const packet_engine::Metadata &, packet_engine::Fault);
  using Snapshot = bool (*)(packet_engine::SystemInfo &);
  using Compose = packet_engine::Fault (*)(const packet_engine::ComposeRequest &,
      const uint8_t *, uint16_t, uint8_t *, uint16_t &);
  enum class PhyState : uint8_t { Idle, Pending, Applied, Stale, Expired, Failed, Cancelled };
  static const char *phyStateText(PhyState state) {
    switch (state) {
      case PhyState::Idle: return "idle";
      case PhyState::Pending: return "pending";
      case PhyState::Applied: return "applied";
      case PhyState::Stale: return "stale";
      case PhyState::Expired: return "expired";
      case PhyState::Failed: return "failed";
      case PhyState::Cancelled: return "cancelled";
    }
    return "invalid";
  }
  struct Status {
    uint32_t faults = 0;
    char engine[32]{};
    packet_engine::Metadata packet;
    packet_engine::Fault lastFault = packet_engine::Fault::None;
    PhyState phy = PhyState::Idle;
    uint32_t phyAccepted = 0, phyApplied = 0;
  };
private:
  WifiKissMultiplexer &mux_;
  Clock clock_;
  Report report_;
  Snapshot snapshot_;
  Compose compose_;
  packet_engine::Pipeline pipeline_;
  Status status_;
  bool running_ = false;
  packet_engine::PhyChange pendingPhy_;
  packet_engine::Metadata phyPacket_;
  uint32_t phyStarted_ = 0;
public:
  NativePacketHost(WifiKissMultiplexer &mux, Clock clock, Report report,
                   Snapshot snapshot = nullptr, Compose compose = nullptr)
      : mux_(mux), clock_(clock), report_(report), snapshot_(snapshot),
        compose_(compose), pipeline_(*this) {}
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
    if (status_.phy == PhyState::Pending) {
      status_.phy = PhyState::Cancelled;
      fault("PHY controller", phyPacket_, packet_engine::Fault::ControlRejected);
    }
    running_ = false;
  }
  packet_engine::Pipeline &pipeline() { return pipeline_; }
  const Status &status() const { return status_; }
  // Dispatch task, outside packet callbacks. Applying a PHY is attempted once.
  void service() {
    if (!running_ || status_.phy != PhyState::Pending) return;
    if (pendingPhy_.generation != mux_.configurationGeneration()) {
      status_.phy = PhyState::Stale;
    } else if (uint32_t(microsNow() - phyStarted_) >= 5000000) {
      status_.phy = PhyState::Expired;
    } else if (!mux_.localReady()) {
      status_.phy = PhyState::Failed;
    } else if (mux_.hasPendingTransmit() || mux_.isActuallyTransmitting() ||
               mux_.physicalRadio().isReceiving()) {
      return;
    } else {
      const auto &p = pendingPhy_.phy;
      const RadioConfig config{p.frequencyHz, p.bandwidthHz, uint8_t(p.spreadingFactor),
                               uint8_t(p.codingRate), uint8_t(p.txPower)};
      status_.phy = mux_.applyMastConfiguration(config, pendingPhy_.persist != 0) ?
                    PhyState::Applied : PhyState::Failed;
      if (status_.phy == PhyState::Applied) { ++status_.phyApplied; return; }
    }
    fault("PHY controller", phyPacket_, packet_engine::Fault::ControlRejected);
  }
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
  bool system(packet_engine::SystemInfo &info) override {
    if (!running_ || !snapshot_ || !snapshot_(info)) return false;
    const auto phy = mux_.currentConfiguration();
    info.phy = {phy.freq_hz, phy.bw_hz, phy.sf, phy.cr, phy.tx_power};
    info.generation = mux_.configurationGeneration();
    info.flags |= (mux_.localReady() ? packet_engine::RadioReady : 0) |
                  (mux_.isActuallyTransmitting() ? packet_engine::Transmitting : 0);
    return true;
  }
  packet_engine::Fault compose(const packet_engine::ComposeRequest &request,
      const uint8_t *data, uint16_t length, uint8_t *output, uint16_t &capacity) override {
    return running_ && compose_ ? compose_(request, data, length, output, capacity) :
                                 packet_engine::Fault::Unavailable;
  }
  packet_engine::Fault commit(const packet_engine::Metadata &metadata, const packet_engine::Emission *packets,
                             uint8_t count, const packet_engine::PhyChange *phy) override {
    if (!running_) return phy ? packet_engine::Fault::ControlRejected : packet_engine::Fault::EmissionRejected;
    if (phy && (status_.phy == PhyState::Pending || !mux_.localReady() ||
        phy->generation != mux_.configurationGeneration())) return packet_engine::Fault::ControlRejected;
    if (count && !mux_.admitEnginePackets(packets, count)) return packet_engine::Fault::EmissionRejected;
    if (phy) {
      pendingPhy_ = *phy; phyPacket_ = metadata; phyStarted_ = microsNow();
      status_.phy = PhyState::Pending; ++status_.phyAccepted;
    }
    return packet_engine::Fault::None;
  }
};
} // namespace onchip
