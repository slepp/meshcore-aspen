// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "SharedRadio.h"
#include <RadioDashboard.h>
#include <QueuedTxProtocol.h>
#include <KissModem.h>
#include <cmath>

namespace onchip {
inline uint32_t nativeTransmitSeconds(const mesh::Dispatcher &dispatcher) {
  uint32_t elapsed;
  return dispatcher.tryGetTotalAirTime(elapsed) ? elapsed / 1000 : UINT32_MAX;
}
class LocalRadio final : public mesh::Radio {
  static constexpr unsigned Capacity = 4;
  nrfmast::RadioPort *port_ = nullptr;
  RadioConfig profile_{};
  struct Transmission {
    uint8_t bytes[255]{};
    uint16_t length = 0;
    uint32_t job = 0, admitted = 0, due = 0, expires = 0, started = 0;
    bool sending = false;
  } pending_[Capacity];
  mesh::QueuedTransmitResult results_[2 * Capacity]{};
  unsigned head_ = 0, count_ = 0, resultHead_ = 0, resultCount_ = 0, outstanding_ = 0;
  uint32_t nextJob_ = 0, sent_ = 0, failed_ = 0, received_ = 0, confirmedRfMs_ = 0;
  uint32_t configGeneration_ = 1, measurementGeneration_ = 0;
  void result(const Transmission &tx, uint8_t state, uint8_t reason, uint32_t now) {
    const bool knownRf = state == queued_tx::SUCCEEDED || (!tx.sending && state == queued_tx::FAILED);
    auto &value = results_[(resultHead_ + resultCount_++) % (2 * Capacity)];
    value = {tx.job, state, reason, uint32_t((tx.sending ? tx.started : now) - tx.admitted),
             knownRf && tx.sending ? uint32_t(now - tx.started) : 0,
             port_->getEstAirtimeFor(tx.length), knownRf};
  }
  void pump() {
    if (!port_ || !count_) return;
    auto &tx = pending_[head_];
    const uint32_t now = millis();
    if (tx.sending) {
      if (!port_->isSendComplete()) return;
      const bool failed = port_->transmissionFailed();
      port_->onSendFinished();
      if (failed) ++failed_;
      else {
        ++sent_;
        confirmedRfMs_ += uint32_t(now - tx.started);
      }
      result(tx, failed ? queued_tx::UNKNOWN : queued_tx::SUCCEEDED,
             failed ? queued_tx::RF_TIMEOUT : queued_tx::NONE, now);
    } else if (int32_t(now - tx.expires) >= 0) {
      ++failed_;
      result(tx, queued_tx::FAILED, queued_tx::EXPIRED, now);
    } else {
      if (int32_t(now - tx.due) < 0 || port_->isReceiving()) return;
      if (port_->startSendRaw(tx.bytes, tx.length)) {
        tx.sending = true; tx.started = now;
        return;
      }
      ++failed_;
      result(tx, queued_tx::FAILED, queued_tx::START_FAILED, now);
    }
    tx = {};
    head_ = (head_ + 1) % Capacity;
    --count_;
  }
public:
  bool attach(nrfmast::RadioPort &port, const RadioConfig &profile) {
    if (port_) return false;
    port_ = &port; profile_ = profile;
    ++measurementGeneration_;
    return true;
  }
  void configure(const RadioConfig &profile) { profile_ = profile; ++configGeneration_; }
  void detach() {
    if (port_) {
      for (unsigned i = 0; i < count_; ++i) {
        auto &tx = pending_[(head_ + i) % Capacity];
        if (tx.sending) {
          if (port_->isSendComplete()) port_->onSendFinished();
          else port_->abortSend();
        }
      }
    }
    port_ = nullptr;
    head_ = count_ = resultHead_ = resultCount_ = outstanding_ = 0;
  }
  bool supportsQueuedTransmit() const override { return true; }
  bool queuedReady() const override { return port_ && NRFMAST_RF_ENABLED; }
  int sourceSlot() const { return port_ ? 2 : -1; }
  RadioConfig configuration() const { return profile_; }
  uint16_t queuedCount() const { return count_; }
  uint32_t receivedAirtimeMs() const { return port_ ? port_->receivedAirtimeMs() : 0; }
  bool hasPendingWork() const { return count_ != 0; }
  bool getQueuedRadioStats(mesh::QueuedRadioStats &value) const override {
    if (!port_) return false;
    value = {};
    value.generation = measurementGeneration_; value.configuration_generation = configGeneration_;
    value.captured_ms = millis(); value.source_rf_ms = confirmedRfMs_;
    value.source_successes = sent_; value.source_failures = failed_;
    port_->aggregateStats(value);
    value.aggregate_queued += count_ - unsigned(count_ && pending_[head_].sending);
    return true;
  }
  bool setQueuedSourcePolicy(float) override { return false; }
  bool queueTransmit(const uint8_t *bytes, int length, uint8_t, uint32_t delay,
                     uint32_t expiry, uint32_t &job) override {
    if (!queuedReady() || !bytes || length < 1 || length > 255 ||
        outstanding_ == Capacity || nextJob_ == UINT32_MAX ||
        delay > queued_tx::MAX_DELAY_MS || expiry > queued_tx::MAX_DELAY_MS) return false;
    auto &tx = pending_[(head_ + count_++) % Capacity];
    tx = {}; memcpy(tx.bytes, bytes, length); tx.length = length;
    tx.job = ++nextJob_; tx.admitted = millis(); tx.due = tx.admitted + delay;
    tx.expires = tx.admitted + (expiry ? expiry : delay + 30000);
    ++outstanding_; job = tx.job;
    result(tx, queued_tx::ACCEPTED, queued_tx::NONE, tx.admitted);
    return true;
  }
  bool pollQueuedResult(mesh::QueuedTransmitResult &value) override {
    pump();
    if (!resultCount_) return false;
    value = results_[resultHead_];
    resultHead_ = (resultHead_ + 1) % (2 * Capacity); --resultCount_;
    if (value.state != queued_tx::ACCEPTED) --outstanding_;
    return true;
  }
  int recvRaw(uint8_t *bytes, int capacity) override {
    pump();
    const int length = port_ ? port_->recvRaw(bytes, capacity) : 0;
    if (length > 0) ++received_;
    return length;
  }
  uint32_t getEstAirtimeFor(int bytes) override { return port_ ? port_->getEstAirtimeFor(bytes) : UINT32_MAX; }
  float packetScore(float snr, int bytes) override { return port_ ? port_->packetScore(snr, bytes) : 0; }
  bool startSendRaw(const uint8_t *, int) override { return false; }
  bool isSendComplete() override { return false; }
  void onSendFinished() override {}
  bool isInRecvMode() const override { return queuedReady(); }
  float getLastRSSI() const override { return port_ ? port_->getLastRSSI() : NAN; }
  float getLastSNR() const override { return port_ ? port_->getLastSNR() : NAN; }
  int getNoiseFloor() const override { return port_ ? port_->getNoiseFloor() : -120; }
  uint32_t getPacketsRecv() const { return received_; }
  uint32_t getPacketsSent() const { return sent_; }
  uint32_t getPacketsRecvErrors() const { return port_ ? port_->rxDrops : 0; }
};
}
