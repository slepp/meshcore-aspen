// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CommandBot.h"
#include <SPIFFS.h>
#include <atomic>
#include <cassert>
#include <deque>
#include <memory>
#include <nvs.h>
#include <optional>
#include <string>
#include <utility>
#include <vector>

extern std::atomic<unsigned long> timeMs;
#ifdef BOT_HOST_RUNNER
namespace onchip { class Management; }
#endif

namespace bot_native_test {

struct StorageDriver {
  // Durable adapters must commit NVS records and rename files atomically.
  std::shared_ptr<identity_test::NvsDriver> nvs;
  std::shared_ptr<filesystem_test::StorageDriver> spiffs;
};

struct TxToken {
  uint8_t sourceSlot = 0;
  uint32_t sourceGeneration = 0;
  uint32_t sourceJob = 0;

  bool operator==(const TxToken &other) const {
    return sourceSlot == other.sourceSlot &&
           sourceGeneration == other.sourceGeneration &&
           sourceJob == other.sourceJob;
  }
};

struct TxFrame {
  TxToken token{};
  uint8_t priority = 0;
  uint32_t eligibilityDelayMs = 0;
  uint32_t eligibleAtMs = 0;
  uint32_t expiryMs = 0;
  std::vector<uint8_t> packet;
  bool simulatedRadio = true;
};

struct TxOutcome {
  TxToken token{};
  uint8_t state = queued_tx::UNKNOWN;
  uint8_t reason = 0;
  uint32_t queueMs = 0;
  uint32_t simulatedRfMs = 0;
  uint32_t estimatedMs = 0;
  bool simulated = true;
};

class Radio final : public mesh::Radio {
public:
  using Started = void (*)(void *, const uint8_t *, int);

  std::vector<std::vector<uint8_t>> sent;
  std::deque<std::vector<uint8_t>> input;
  bool active = false, rejectTx = false, holdTx = false;
  uint32_t airtime = 10;
  float score = 1;
  unsigned scored = 0;
  uint32_t transmitStartedAt = 0;

  void setStarted(Started callback, void *context) {
    started_ = callback;
    startedContext_ = context;
  }
  int recvRaw(uint8_t *data, int capacity) override;
  uint32_t getEstAirtimeFor(int) override { return airtime; }
  float packetScore(float, int) override {
    ++scored;
    return score;
  }
  bool startSendRaw(const uint8_t *data, int size) override;
  bool isSendComplete() override { return active && !holdTx; }
  void onSendFinished() override { active = false; }
  bool isInRecvMode() const override { return !active; }

private:
  Started started_ = nullptr;
  void *startedContext_ = nullptr;
};

class BotNativeHarness;

class TxObserver final : public KissPacketObserver {
public:
  explicit TxObserver(BotNativeHarness &harness) : harness_(harness) {}
  void packet(const uint8_t *, uint16_t, bool, uint8_t, float, float) override {
  }
  void transmitStarting(const uint8_t *packet, uint16_t length,
                        uint8_t sourceSlot, uint32_t sourceGeneration,
                        uint32_t sourceJob, uint8_t priority,
                        uint32_t eligibilityDelayMs, uint32_t eligibleAtMs,
                        uint32_t expiryMs) override;
  void transmitCompleted(const uint8_t *packet, uint16_t length,
                         uint8_t sourceSlot, uint32_t sourceGeneration,
                         uint32_t sourceJob, uint8_t state, uint8_t reason,
                         uint32_t queueMs, uint32_t rfMs,
                         uint32_t estimatedMs) override;

private:
  BotNativeHarness &harness_;
};

class BotNativeHarness {
public:
  struct Options {
    bool captureTxForTest = false;
    bool manualTxCompletionForTest = false;
    std::shared_ptr<StorageDriver> storage;
#ifdef BOT_HOST_RUNNER
    bool nativeAdministrationForReplay = false;
#endif
  };

  Radio radio;
  onchip::HardwareRNG rng;
  WifiKissMultiplexer mux;
  onchip::LocalRadio existing[4];
  onchip::CommandBot bot;

  BotNativeHarness();
  explicit BotNativeHarness(Options options);
  ~BotNativeHarness();
  BotNativeHarness(const BotNativeHarness &) = delete;
  BotNativeHarness &operator=(const BotNativeHarness &) = delete;

  void step(unsigned count = 80);
  void start();
#ifdef BOT_HOST_RUNNER
  bool startReplay(std::string &error);
  std::string administer(const char *command);
  bool trustedOwner(const uint8_t key[32]) const;
#endif
  void ingest(const std::vector<uint8_t> &bytes, float rssi = -91.5f,
              float snr = 5.25f);
  void deliver(const std::vector<uint8_t> &bytes, float rssi = -91.5f,
               float snr = 5.25f) {
    ingest(bytes, rssi, snr);
  }
  void reflect(const std::vector<uint8_t> &bytes);

  bool takeTxFrame(TxFrame &frame);
  bool completeSimulatedTxForTest(const TxToken &token,
                                  uint32_t simulatedAirtimeMs);
  bool takeTxOutcomeForTest(TxOutcome &outcome);
  void setManualTxCompletionForTest(bool enabled);

  template <class Peer> void learn(Peer &peer) {
    ingest(peer.advert());
    step(15);
  }
  template <class Peer>
  std::string command(Peer &peer, const char *text, uint8_t width = 1,
                      std::vector<uint8_t> path = {}) {
    timeMs += 61000;
    radio.sent.clear();
    ingest(peer.command(bot.publicKey(), text, width, path));
    step();
    const auto replies = peer.replies(bot.publicKey(), radio);
    assert(replies.size() == 1);
    return replies.front();
  }

private:
  friend class TxObserver;
  static void installStorageDriverForTest(const StorageDriver *storage);
  static void resetStorageDriverForTest();
  static void radioStarted(void *context, const uint8_t *packet, int length);
  void onTransmitStarting(const uint8_t *packet, uint16_t length,
                          const TxToken &token, uint8_t priority,
                          uint32_t eligibilityDelayMs, uint32_t eligibleAtMs,
                          uint32_t expiryMs);
  void onTransmitStarted(const uint8_t *packet, size_t length);
  void onTransmitCompleted(const TxToken &token, uint8_t state, uint8_t reason,
                           uint32_t queueMs, uint32_t rfMs,
                           uint32_t estimatedMs);

  Options options_;
  TxObserver observer_;
  std::optional<TxFrame> pendingTx_;
  std::deque<TxFrame> txFrames_;
  std::deque<TxOutcome> txOutcomes_;
  TxToken activeTxToken_{};
  bool activeTxTokenValid_ = false;
  std::optional<uint32_t> simulatedAirtimeMs_;
  bool installedStorageDriver_ = false;
  std::vector<std::pair<void *, size_t>> retainedAllocations_;
#ifdef BOT_HOST_RUNNER
  std::unique_ptr<onchip::Management> management_;
#endif
};

} // namespace bot_native_test
