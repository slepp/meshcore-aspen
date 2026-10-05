// SPDX-License-Identifier: Apache-2.0
#pragma once
#if defined(MESHCORE_HOST_BOT_SOURCE) && MESHCORE_HOST_BOT_SOURCE
#include <HostLocalRadio.h>
#elif defined(NRF52_PLATFORM)
#include "PineLocalRadio.h"
#else
#include "WifiKissMultiplexer.h"
#include <Dispatcher.h>
#include <cmath>

#ifndef MESH_QUEUED_RADIO_API
#error "Apply queued-dispatch.patch before building on-chip roles"
#endif

namespace onchip {
class LocalRadio final : public mesh::Radio, public KissLocalSource {
  static constexpr unsigned CAPACITY = KISS_REQUEST_QUEUE_DEPTH + 1;
  WifiKissMultiplexer *mux = nullptr;
  int slot = -1;
  uint32_t nextJob = 0;
  unsigned outstanding = 0;
  struct Reception {
    uint8_t data[256];
    uint16_t length;
    float rssi, snr;
    bool local;
  };
  Reception incoming[8]{};
  unsigned rxHead = 0, rxCount = 0;
  mesh::QueuedTransmitResult results[2 * CAPACITY]{};
  unsigned resultHead = 0, resultCount = 0;
  float lastRSSI = 0, lastSNR = 0;
  bool lastLocal = false;
  uint32_t rxPackets = 0, txPackets = 0, rxDropped = 0, rxAirtime = 0;

public:
  bool attach(WifiKissMultiplexer &arbiter) {
    if (mux)
      return false;
    const int assigned = arbiter.attachLocal(*this);
    if (assigned < 0) return false;
    mux = &arbiter;
    slot = assigned;
    return true;
  }
  void detach() {
    if (slot >= 0) mux->detachLocal(slot);
    mux = nullptr;
    slot = -1;
    outstanding = resultHead = resultCount = rxHead = rxCount = 0;
    rxPackets = txPackets = rxDropped = rxAirtime = 0;
    lastRSSI = lastSNR = 0;
    lastLocal = false;
    memset(incoming, 0, sizeof(incoming));
    for (auto &result : results) result = {};
  }
  bool supportsQueuedTransmit() const override { return true; }
  int sourceSlot() const { return slot; }
  bool queuedReady() const override { return slot >= 0 && mux->localReady(); }
  bool getQueuedRadioStats(mesh::QueuedRadioStats &stats) const override {
    return slot >= 0 && mux->getQueuedRadioStats(slot, stats);
  }
  RadioConfig configuration() const { return mux->currentConfiguration(); }
  const WifiKissMultiplexer *sharedRadio() const { return mux; }
  mesh::Radio &physical() const { return mux->physicalRadio(); }
  uint16_t queuedCount() const {
    return slot >= 0 ? mux->sourceQueuedCount(slot) : 0;
  }
  bool hasPendingWork() const {
    return slot >= 0 && (queuedCount() != 0 || mux->sourceTransmitting(slot));
  }
  bool setQueuedSourcePolicy(float f) override {
    return slot >= 0 && mux->setLocalPolicy(slot, f);
  }
  bool queueTransmit(const uint8_t *p, int n, uint8_t priority, uint32_t delay,
                     uint32_t expiry, uint32_t &job) override {
    if (!queuedReady() || outstanding >= CAPACITY || n <= 0 || n > 255)
      return false;
    if (nextJob == UINT32_MAX)
      return false;
    const uint32_t id = ++nextJob;
    ++outstanding;
    auto &accepted = results[(resultHead + resultCount++) % (2 * CAPACITY)];
    accepted = {};
    accepted.job = id;
    accepted.state = queued_tx::ACCEPTED;
    if (!mux->submitLocal(slot, p, n, id, priority, delay, expiry)) {
      --resultCount;
      --outstanding;
      return false;
    }
    job = id;
    return true;
  }
  void completed(uint32_t job, uint8_t state, uint8_t reason, uint32_t queue,
                 uint32_t rf, uint32_t estimate) override {
    // Each transfer reserves separate admission and terminal result slots.
    auto &r = results[(resultHead + resultCount++) % (2 * CAPACITY)];
    r.job = job;
    r.state = state;
    r.reason = reason;
    r.queue_ms = queue;
    r.rf_ms = rf;
    r.estimated_ms = estimate;
    r.has_rf_ms =
        state != queued_tx::ACCEPTED && reason != queued_tx::DISCONNECTED;
    if (state == queued_tx::SUCCEEDED)
      ++txPackets;
  }
  bool pollQueuedResult(mesh::QueuedTransmitResult &result) override {
    if (!resultCount)
      return false;
    result = results[resultHead];
    resultHead = (resultHead + 1) % (2 * CAPACITY);
    --resultCount;
    if (result.state != queued_tx::ACCEPTED)
      --outstanding;
    return true;
  }
  void received(const uint8_t *p, uint16_t n, float rssi, float snr,
                bool local) override {
    if (!local && n > 0 && n <= 255)
      rxAirtime += getEstAirtimeFor(n);
    if (n > 255 || rxCount == 8) {
      ++rxDropped;
      return;
    }
    auto &r = incoming[(rxHead + rxCount++) % 8];
    memcpy(r.data, p, n);
    r.length = n;
    r.rssi = local ? NAN : rssi;
    r.snr = local ? NAN : snr;
    r.local = local;
  }
  int recvRaw(uint8_t *p, int capacity) override {
    if (!rxCount)
      return 0;
    auto &r = incoming[rxHead];
    int n = r.length;
    if (n > capacity) {
      n = 0;
      ++rxDropped;
    } else {
      memcpy(p, r.data, n);
      if (!r.local)
        ++rxPackets;
    }
    lastRSSI = r.rssi;
    lastSNR = r.snr;
    lastLocal = r.local;
    rxHead = (rxHead + 1) % 8;
    --rxCount;
    return n;
  }
  bool lastReceiveWasLocal() const override { return lastLocal; }
  uint32_t getEstAirtimeFor(int n) override {
    return mux->physicalRadio().getEstAirtimeFor(n);
  }
  float packetScore(float snr, int n) override {
    return mux->physicalRadio().packetScore(snr, n);
  }
  bool startSendRaw(const uint8_t *, int) override { return false; }
  bool isSendComplete() override { return false; }
  void onSendFinished() override {}
  bool isInRecvMode() const override { return queuedReady(); }
  float getLastRSSI() const override { return lastRSSI; }
  float getLastSNR() const override { return lastSNR; }
  int getNoiseFloor() const override {
    return mux->physicalRadio().getNoiseFloor();
  }
  uint32_t getPacketsRecv() const { return rxPackets; }
  uint32_t getPacketsSent() const { return txPackets; }
  uint32_t getPacketsRecvErrors() const { return rxDropped; }
  uint32_t receivedAirtimeMs() const { return rxAirtime; }
};

// Native byte-oriented receive notifications use the explicit unmeasured
// marker.
inline float nativeSNR(const mesh::Radio &radio) {
  return radio.lastReceiveWasLocal() ? -32.0f : radio.getLastSNR();
}
inline float nativeRSSI(const mesh::Radio &radio) {
  return radio.lastReceiveWasLocal() ? 127.0f : radio.getLastRSSI();
}
} // namespace onchip
#endif
