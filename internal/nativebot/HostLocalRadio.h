// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <KissModem.h>
#include <Dispatcher.h>
#include <QueuedTxProtocol.h>
#include <RadioDashboard.h>
#include <cmath>
#include <cstring>

#ifndef MESH_QUEUED_RADIO_API
#error "Host bot requires the pinned queued-dispatch patch"
#endif

namespace onchip {
// The process owns this transport; Go alone owns RF scheduling and completion.
bool nativebot_send_tx(uint32_t token, uint8_t priority, uint32_t delay,
                       uint32_t expiry, const uint8_t *packet, size_t length);

inline uint32_t nativeTransmitSeconds(const mesh::Dispatcher &dispatcher) {
  uint32_t rf_ms;
  return dispatcher.tryGetTotalAirTime(rf_ms) ? rf_ms / 1000 : UINT32_MAX;
}

class LocalRadio final : public mesh::Radio {
  static constexpr unsigned Capacity = 13, RxCapacity = 8;
  struct Reception {
    uint8_t data[255]{};
    uint16_t length = 0;
    float rssi = 0, snr = 0;
    bool local = false;
  };
  struct Pending {
    uint32_t token = 0;
    bool accepted = false;
  };
  Reception incoming_[RxCapacity]{};
  Pending pending_[Capacity]{};
  mesh::QueuedTransmitResult results_[2 * Capacity]{};
  uint32_t airtime_[256]{};
  RadioConfig profile_{};
  mesh::QueuedRadioStats stats_{};
  bool stats_valid_ = false;
  unsigned rx_head_ = 0, rx_count_ = 0;
  unsigned result_head_ = 0, result_count_ = 0, outstanding_ = 0;
  uint32_t next_job_ = 0, rx_packets_ = 0, tx_packets_ = 0, rx_dropped_ = 0;
  uint32_t rx_airtime_ = 0;
  float last_rssi_ = 0, last_snr_ = 0;
  int noise_floor_ = -120;
  bool last_local_ = false, attached_ = false;

public:
  bool attachHost(const RadioConfig &profile, const uint32_t *airtime, int noise) {
    if (attached_ || !airtime) return false;
    profile_ = profile;
    std::memcpy(airtime_, airtime, sizeof(airtime_));
    noise_floor_ = noise;
    attached_ = true;
    return true;
  }
  void detach() {
    attached_ = false;
    stats_valid_ = false;
    rx_head_ = rx_count_ = result_head_ = result_count_ = outstanding_ = 0;
    rx_airtime_ = 0;
    for (auto &pending : pending_) pending = {};
    for (auto &result : results_) result = {};
  }
  bool supportsQueuedTransmit() const override { return true; }
  int sourceSlot() const { return attached_ ? 0 : -1; }
  bool queuedReady() const override { return attached_; }
  bool getQueuedRadioStats(mesh::QueuedRadioStats &result) const override {
    if (!attached_ || !stats_valid_ || uint32_t(millis() - stats_.captured_ms) > 3000) return false;
    result = stats_;
    return true;
  }
  void updateStats(const mesh::QueuedRadioStats &stats) {
    stats_ = stats;
    stats_.captured_ms = millis();
    stats_valid_ = true;
  }
  RadioConfig configuration() const { return profile_; }
  uint16_t queuedCount() const { return outstanding_; }
  bool hasPendingWork() const { return outstanding_ != 0; }
  bool hasBufferedEvents() const { return rx_count_ != 0 || result_count_ != 0; }
  bool setQueuedSourcePolicy(float) override { return false; }
  bool queueTransmit(const uint8_t *packet, int size, uint8_t priority,
                     uint32_t delay, uint32_t expiry, uint32_t &job) override {
    if (!attached_ || !packet || size < 1 || size > 255 ||
        outstanding_ == Capacity || next_job_ == UINT32_MAX)
      return false;
    const uint32_t token = next_job_ + 1;
    if (!nativebot_send_tx(token, priority, delay, expiry, packet, size))
      return false;
    next_job_ = token;
    for (auto &pending : pending_)
      if (!pending.token) {
        pending.token = token;
        ++outstanding_;
        job = token;
        return true;
      }
    return false;
  }
  bool completed(uint32_t token, uint8_t state, uint8_t reason, uint32_t queue_ms,
                 uint32_t rf_ms, uint32_t estimate_ms, bool has_rf_ms) {
    Pending *entry = nullptr;
    for (auto &pending : pending_)
      if (pending.token == token) { entry = &pending; break; }
    if (!entry || result_count_ == 2 * Capacity || state > queued_tx::UNKNOWN ||
        reason > queued_tx::NOT_CONFIGURED || (state == queued_tx::ACCEPTED && entry->accepted) ||
        (state != queued_tx::ACCEPTED && !entry->accepted && state != queued_tx::REJECTED) ||
        (state == queued_tx::ACCEPTED && (reason != queued_tx::NONE || has_rf_ms)) ||
        (state == queued_tx::SUCCEEDED && (reason != queued_tx::NONE || !has_rf_ms)))
      return false;
    auto &result = results_[(result_head_ + result_count_++) % (2 * Capacity)];
    result = {token, state, reason, queue_ms, rf_ms, estimate_ms, has_rf_ms};
    if (state == queued_tx::ACCEPTED) entry->accepted = true;
    else {
      if (state == queued_tx::SUCCEEDED) ++tx_packets_;
      *entry = {};
      --outstanding_;
    }
    return true;
  }
  bool pollQueuedResult(mesh::QueuedTransmitResult &result) override {
    if (!result_count_) return false;
    result = results_[result_head_];
    result_head_ = (result_head_ + 1) % (2 * Capacity);
    --result_count_;
    return true;
  }
  bool received(const uint8_t *packet, size_t size, float rssi, float snr, bool local) {
    if (packet && size > 0 && size <= 255 && !local)
      rx_airtime_ += airtime_[size];
    if (!packet || size < 1 || size > 255 || rx_count_ == RxCapacity) {
      ++rx_dropped_;
      return false;
    }
    auto &entry = incoming_[(rx_head_ + rx_count_++) % RxCapacity];
    std::memcpy(entry.data, packet, size);
    entry.length = size;
    entry.rssi = local ? NAN : rssi;
    entry.snr = local ? NAN : snr;
    entry.local = local;
    return true;
  }
  int recvRaw(uint8_t *packet, int capacity) override {
    if (!rx_count_) return 0;
    auto &entry = incoming_[rx_head_];
    const int size = entry.length;
    if (size > capacity) ++rx_dropped_;
    else {
      std::memcpy(packet, entry.data, size);
      if (!entry.local) ++rx_packets_;
    }
    last_rssi_ = entry.rssi;
    last_snr_ = entry.snr;
    last_local_ = entry.local;
    rx_head_ = (rx_head_ + 1) % RxCapacity;
    --rx_count_;
    return size > capacity ? 0 : size;
  }
  bool lastReceiveWasLocal() const override { return last_local_; }
  uint32_t getEstAirtimeFor(int size) override {
    return size >= 0 && size <= 255 ? airtime_[size] : UINT32_MAX;
  }
  float packetScore(float snr, int size) override {
    if (!std::isfinite(snr) || profile_.sf < 7 || profile_.sf > 12 || size < 0 || size > 255) return 0;
    const float minimum = -5.0f - 2.5f * (profile_.sf - 6);
    return std::fmax(0, std::fmin(1, (snr - minimum) / 10 * (1 - size / 256.0f)));
  }
  bool startSendRaw(const uint8_t *, int) override { return false; }
  bool isSendComplete() override { return false; }
  void onSendFinished() override {}
  bool isInRecvMode() const override { return attached_; }
  float getLastRSSI() const override { return last_rssi_; }
  float getLastSNR() const override { return last_snr_; }
  int getNoiseFloor() const override { return noise_floor_; }
  uint32_t getPacketsRecv() const { return rx_packets_; }
  uint32_t getPacketsSent() const { return tx_packets_; }
  uint32_t getPacketsRecvErrors() const { return rx_dropped_; }
  uint32_t receivedAirtimeMs() const { return rx_airtime_; }
};

inline float nativeSNR(const mesh::Radio &radio) {
  return radio.lastReceiveWasLocal() ? -32.0f : radio.getLastSNR();
}
inline float nativeRSSI(const mesh::Radio &radio) {
  return radio.lastReceiveWasLocal() ? 127.0f : radio.getLastRSSI();
}
} // namespace onchip
