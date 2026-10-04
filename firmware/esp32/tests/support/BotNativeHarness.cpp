// SPDX-License-Identifier: Apache-2.0
#include "BotNativeHarness.h"
#include "Runtime.h"
#ifdef BOT_HOST_RUNNER
#include "Management.h"
#endif
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstring>
#include <esp_heap_caps.h>
#include <thread>

namespace bot_native_test {

BotNativeHarness::BotNativeHarness() : BotNativeHarness(Options{}) {}

int Radio::recvRaw(uint8_t *data, int capacity) {
  if (input.empty())
    return 0;
  auto packet = std::move(input.front());
  input.pop_front();
  assert(packet.size() <= size_t(capacity));
  memcpy(data, packet.data(), packet.size());
  return int(packet.size());
}

bool Radio::startSendRaw(const uint8_t *data, int size) {
  if (rejectTx)
    return false;
  assert(!active);
  active = true;
  transmitStartedAt = uint32_t(millis());
  sent.emplace_back(data, data + size);
  if (started_)
    started_(startedContext_, data, size);
  return true;
}

BotNativeHarness::BotNativeHarness(Options options)
    : options_(options), observer_(*this) {
  if (options_.manualTxCompletionForTest)
    options_.captureTxForTest = true;
  if (options_.storage) {
    installStorageDriverForTest(options_.storage.get());
    installedStorageDriver_ = true;
  }
  for (const auto &entry : psram_test::allocations)
    retainedAllocations_.push_back(entry);
  radio.setStarted(radioStarted, this);
  mux.attachRadio(
      radio, rng, [](float, float, uint8_t, uint8_t) {}, [](uint8_t) {});
  mux.observePackets(observer_);
  assert(mux.setInitialConfiguration({912525000, 250000, 7, 5, 2}, true));
  for (unsigned i = 0; i < 4; ++i) {
#ifdef BOT_HOST_RUNNER
    if (i == 3 && options_.nativeAdministrationForReplay) continue;
#endif
    assert(existing[i].attach(mux));
  }
}

BotNativeHarness::~BotNativeHarness() {
  bot.stop();
#ifdef BOT_HOST_RUNNER
  if (management_) {
    management_->stop();
    management_.reset();
    assert(onchip::bindCommandBotForReplay(nullptr));
  }
#endif
  for (auto &source : existing)
    source.detach();
  assert(psram_test::allocations.size() == retainedAllocations_.size());
  for (const auto &entry : retainedAllocations_)
    assert(psram_test::allocations.count(entry.first) &&
           psram_test::allocations.at(entry.first) == entry.second);
  if (installedStorageDriver_)
    resetStorageDriverForTest();
}

void BotNativeHarness::installStorageDriverForTest(
    const StorageDriver *storage) {
  identity_test::driver = storage ? storage->nvs.get() : nullptr;
  SPIFFS.setDriver(storage ? storage->spiffs : nullptr);
}

void BotNativeHarness::resetStorageDriverForTest() {
  identity_test::driver = nullptr;
  SPIFFS.useMemoryDriver();
}

void BotNativeHarness::step(unsigned count) {
  while (count--) {
    timeMs += 2;
    onchip::loopClocks();
    bot.loop();
#ifdef BOT_HOST_RUNNER
    if (management_) management_->loop();
#endif
    mux.serviceTransmit();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

void BotNativeHarness::start() {
  assert(bot.begin(mux));
  step();
  RadioDashboard::RoleStatus status;
  bot.dashboardStatus(status);
  assert(status.ready && !strcmp(status.role, "command-bot"));
  assert(status.source_generation != 0);
  assert(status.source_slot == KISS_MAX_TCP_CLIENTS + 4);
  timeMs += 61000;
  radio.sent.clear();
  txFrames_.clear();
  txOutcomes_.clear();
  setManualTxCompletionForTest(options_.manualTxCompletionForTest);
}

#ifdef BOT_HOST_RUNNER
std::string BotNativeHarness::administer(const char *command) {
  if (!management_) return "Error: native administration unavailable";
  onchip::MastAdmin::Reply reply;
  management_->admin().execute(command, reply);
  return reply.text;
}

bool BotNativeHarness::trustedOwner(const uint8_t key[32]) const {
  auto *admin = onchip::MastAdmin::service();
  return management_ && admin == &management_->admin() && admin->ready() && admin->trusted(key);
}

bool BotNativeHarness::startReplay(std::string &error) {
  if (!options_.nativeAdministrationForReplay || management_ || onchip::MastAdmin::service() ||
      !onchip::bindCommandBotForReplay(&bot)) {
    error = "native replay bot binding unavailable"; return false;
  }
  management_ = std::make_unique<onchip::Management>();
  if (!management_->begin(mux, "") || !management_->admin().ready() || !bot.begin(mux)) {
    error = "native Management/MastAdmin or command-bot initialization failed"; return false;
  }
  for (unsigned i = 0; i < 40; ++i) {
    step(80);
    const auto source = administer("source status");
    if (source.find("Error:") != std::string::npos) {
      error = source; return false;
    }
    if (!bot.sourceReady()) continue;
    if (source.find("bundled source") == std::string::npos &&
        source.find("source durably saved and active") == std::string::npos) continue;
    timeMs += 61000;
    radio.sent.clear();
    txFrames_.clear();
    txOutcomes_.clear();
    setManualTxCompletionForTest(options_.manualTxCompletionForTest);
    return true;
  }
  error = "native source journal recovery did not complete: " + administer("source status");
  return false;
}
#endif

void BotNativeHarness::ingest(const std::vector<uint8_t> &bytes, float rssi,
                              float snr) {
  mux.received(bytes.data(), bytes.size(), rssi, snr);
}

void BotNativeHarness::reflect(const std::vector<uint8_t> &bytes) {
  uint32_t job;
  assert(existing[0].queueTransmit(bytes.data(), bytes.size(), 0, 0, 0, job));
  step(3);
  mesh::QueuedTransmitResult result;
  assert(existing[0].pollQueuedResult(result) && result.job == job &&
         result.state == queued_tx::ACCEPTED);
  assert(existing[0].pollQueuedResult(result) && result.job == job &&
         result.state == queued_tx::SUCCEEDED);
  assert(!existing[0].pollQueuedResult(result));
}

bool BotNativeHarness::takeTxFrame(TxFrame &frame) {
  if (!options_.captureTxForTest || txFrames_.empty())
    return false;
  frame = std::move(txFrames_.front());
  txFrames_.pop_front();
  return true;
}

bool BotNativeHarness::completeSimulatedTxForTest(const TxToken &token,
                                                  uint32_t airtimeMs) {
  if (!options_.manualTxCompletionForTest || !radio.active || !radio.holdTx ||
      !activeTxTokenValid_ || !(activeTxToken_ == token))
    return false;
  const uint32_t elapsed =
      static_cast<uint32_t>(millis()) - radio.transmitStartedAt;
  const uint32_t minimumStep = 2;
  if (airtimeMs > elapsed + minimumStep)
    timeMs += airtimeMs - elapsed - minimumStep;
  simulatedAirtimeMs_ = airtimeMs;
  radio.holdTx = false;
  step(1);
  return true;
}

bool BotNativeHarness::takeTxOutcomeForTest(TxOutcome &outcome) {
  if (txOutcomes_.empty())
    return false;
  outcome = txOutcomes_.front();
  txOutcomes_.pop_front();
  return true;
}

void BotNativeHarness::setManualTxCompletionForTest(bool enabled) {
  options_.manualTxCompletionForTest = enabled;
  if (enabled)
    options_.captureTxForTest = true;
  radio.holdTx = enabled;
}

void BotNativeHarness::radioStarted(void *context, const uint8_t *packet,
                                    int length) {
  static_cast<BotNativeHarness *>(context)->onTransmitStarted(
      packet, static_cast<size_t>(length));
}

void BotNativeHarness::onTransmitStarting(const uint8_t *packet,
                                          uint16_t length, const TxToken &token,
                                          uint8_t priority,
                                          uint32_t eligibilityDelayMs,
                                          uint32_t eligibleAtMs,
                                          uint32_t expiryMs) {
  if (!options_.captureTxForTest)
    return;
  TxFrame frame;
  frame.token = token;
  frame.priority = priority;
  frame.eligibilityDelayMs = eligibilityDelayMs;
  frame.eligibleAtMs = eligibleAtMs;
  frame.expiryMs = expiryMs;
  frame.packet.assign(packet, packet + length);
  pendingTx_ = std::move(frame);
}

void BotNativeHarness::onTransmitStarted(const uint8_t *packet, size_t length) {
  if (!options_.captureTxForTest)
    return;
  assert(pendingTx_ && pendingTx_->packet.size() == length &&
         !memcmp(pendingTx_->packet.data(), packet, length));
  activeTxToken_ = pendingTx_->token;
  activeTxTokenValid_ = true;
  txFrames_.push_back(std::move(*pendingTx_));
  pendingTx_.reset();
}

void BotNativeHarness::onTransmitCompleted(const TxToken &token, uint8_t state,
                                           uint8_t reason, uint32_t queueMs,
                                           uint32_t rfMs,
                                           uint32_t estimatedMs) {
  if (pendingTx_ && pendingTx_->token == token)
    pendingTx_.reset();
  if (!options_.captureTxForTest || state == queued_tx::ACCEPTED)
    return;
  txOutcomes_.push_back({token, state, reason, queueMs,
                         simulatedAirtimeMs_.value_or(rfMs), estimatedMs,
                         true});
  if (activeTxTokenValid_ && activeTxToken_ == token) {
    activeTxTokenValid_ = false;
    simulatedAirtimeMs_.reset();
  }
}

void TxObserver::transmitStarting(const uint8_t *packet, uint16_t length,
                                  uint8_t sourceSlot, uint32_t sourceGeneration,
                                  uint32_t sourceJob, uint8_t priority,
                                  uint32_t eligibilityDelayMs,
                                  uint32_t eligibleAtMs, uint32_t expiryMs) {
  harness_.onTransmitStarting(
      packet, length, {sourceSlot, sourceGeneration, sourceJob}, priority,
      eligibilityDelayMs, eligibleAtMs, expiryMs);
}

void TxObserver::transmitCompleted(const uint8_t *, uint16_t,
                                   uint8_t sourceSlot,
                                   uint32_t sourceGeneration,
                                   uint32_t sourceJob, uint8_t state,
                                   uint8_t reason, uint32_t queueMs,
                                   uint32_t rfMs, uint32_t estimatedMs) {
  harness_.onTransmitCompleted({sourceSlot, sourceGeneration, sourceJob}, state,
                               reason, queueMs, rfMs, estimatedMs);
}

} // namespace bot_native_test
