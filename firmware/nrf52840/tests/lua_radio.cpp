// SPDX-License-Identifier: Apache-2.0
#include <cstdint>
static uint32_t now = 1000;
inline unsigned long millis() { return now; }
#include "PineLocalRadio.h"
#include "../../runtime/AdaptiveAdmission.h"
#include <helpers/StaticPoolPacketManager.h>
#include <cassert>
#include <deque>
#include <vector>
#include <cstdio>

struct Clock : mesh::MillisecondClock {
  unsigned long getMillis() override { return now; }
};
struct Physical : mesh::Radio {
  std::deque<std::vector<uint8_t>> incoming;
  bool sending = false, complete = true, failStart = false;
  bool completionReported = false;
  uint32_t airtime = 40;
  unsigned finished = 0, completionQueries = 0;
  int recvRaw(uint8_t* bytes, int capacity) override {
    assert(!sending);
    if (incoming.empty()) return 0;
    const auto frame = incoming.front(); incoming.pop_front();
    assert(int(frame.size()) <= capacity);
    memcpy(bytes, frame.data(), frame.size()); return frame.size();
  }
  uint32_t getEstAirtimeFor(int) override { return airtime; }
  float packetScore(float, int) override { return 1; }
  bool startSendRaw(const uint8_t*, int) override {
    assert(!sending);
    if (failStart) return false;
    sending = true; completionReported = false; return true;
  }
  bool isSendComplete() override {
    ++completionQueries;
    const bool ready = sending && complete && !completionReported;
    if (ready) completionReported = true;
    return ready;
  }
  void onSendFinished() override { assert(sending); sending = false; ++finished; }
  bool isInRecvMode() const override { return !sending; }
  float getLastRSSI() const override { return -87; }
  float getLastSNR() const override { return -2; }
};
struct NativeDispatcher : mesh::Dispatcher {
  unsigned logged = 0, failed = 0;
  NativeDispatcher(mesh::Radio& radio, Clock& clock, mesh::PacketManager& packets)
      : Dispatcher(radio, clock, packets) {}
  mesh::DispatcherAction onRecvPacket(mesh::Packet*) override { return ACTION_RELEASE; }
  void logTx(mesh::Packet*, int) override { ++logged; }
  void logTxFail(mesh::Packet*, int) override { ++failed; }
  void enqueue(uint8_t route) {
    auto* packet = obtainNewPacket();
    assert(packet);
    packet->header = route;
    packet->payload[0] = 7;
    packet->payload_len = 1;
    sendPacket(packet, 0);
  }
};
static void stalledNativeDispatcher(bool interleave, bool botRole) {
  for (uint32_t start : {uint32_t(1000), UINT32_MAX - 10000}) {
    for (uint8_t route : {ROUTE_TYPE_DIRECT, ROUTE_TYPE_FLOOD}) {
      now = start;
      Clock clock;
      Physical physical;
      nrfmast::SharedRadio shared(physical, clock, true);
      auto& port = botRole ? shared.bot : shared.repeater;
      auto& other = botRole ? shared.repeater : shared.bot;
      auto canLoop = [&]() { return botRole ? shared.botCanLoop() : shared.repeaterCanLoop(); };
      assert(shared.setAirtimeFactor(0));
      StaticPoolPacketManager packets(2);
      NativeDispatcher dispatcher(port, clock, packets);
      dispatcher.begin();
      physical.complete = false;
      dispatcher.enqueue(route);
      ++now;
      dispatcher.loop();
      assert(physical.sending && packets.getFreeCount() == 1);
      now += 2081;
      shared.poll();
      assert(shared.idle() && port.transmissionFailed());
      const uint8_t bytes[]{ROUTE_TYPE_DIRECT, 0};
      physical.complete = true;
      if (interleave) {
        assert(!port.startSendRaw(bytes, sizeof(bytes)));
        // Another role can transmit before the expired Dispatcher acknowledges its failure.
        assert(other.startSendRaw(bytes, sizeof(bytes)));
        assert(shared.repeaterCanLoop() && shared.botCanLoop());
      }
      assert(canLoop());
      dispatcher.loop();
      printf("Pine stalled Dispatcher: role=%s start=%u route=%u confirmed=%u failed=%u logTx=%u logTxFail=%u sentDirect=%u sentFlood=%u airtime=%lu\n",
             botRole ? "bot" : "repeater", start, route, shared.txSucceeded, shared.txFailed, dispatcher.logged, dispatcher.failed,
             dispatcher.getNumSentDirect(), dispatcher.getNumSentFlood(), dispatcher.getTotalAirTime());
      fflush(stdout);
      assert(dispatcher.logged == 0 && dispatcher.failed == 1 &&
             dispatcher.getNumSentDirect() == 0 && dispatcher.getNumSentFlood() == 0 &&
             dispatcher.getTotalAirTime() == 0 && packets.getFreeCount() == 2);
      if (!interleave) assert(other.startSendRaw(bytes, sizeof(bytes)));
      assert(!canLoop());
      assert(physical.sending && physical.finished == 1 && !shared.idle() &&
             !port.transmissionFailed() && shared.txSucceeded == 0 &&
             shared.txFailed == 1 && shared.aggregateRfMs() == 0 &&
             shared.aggregateActiveMs() == 2081 && !shared.lastTransmitWasConfirmed());
      assert(other.isSendComplete());
      now += 40;
      other.onSendFinished();
      assert(shared.idle() && shared.txSucceeded == 1 && shared.aggregateRfMs() == 40);
      dispatcher.enqueue(route);
      ++now;
      dispatcher.loop();
      now += 2081;
      shared.poll();  // Genuine completion remains latched even if the loop was stalled.
      dispatcher.loop();
      assert(dispatcher.logged == 1 && dispatcher.failed == 1 &&
             dispatcher.getNumSentDirect() == unsigned(route == ROUTE_TYPE_DIRECT) &&
             dispatcher.getNumSentFlood() == unsigned(route == ROUTE_TYPE_FLOOD) &&
             dispatcher.getTotalAirTime() == 2081 && packets.getFreeCount() == 2 &&
             shared.txSucceeded == 2 && shared.txFailed == 1 &&
             shared.aggregateRfMs() == 2121 && physical.finished == 3 && shared.idle());
    }
  }
}
static void receiveObservability() {
  Clock clock;
  Physical physical;
  nrfmast::SharedRadio shared(physical, clock, true);
  onchip::LocalRadio radio;
  assert(radio.receivedAirtimeMs() == 0 && radio.attach(shared.lua, {}));
  for (unsigned i = 1; i <= 3; ++i) {
    physical.incoming.push_back({ROUTE_TYPE_DIRECT, 0, 7});
    shared.poll();
    assert(shared.receivedAirtimeMs() == 40 * i &&
           shared.repeater.receivedAirtimeMs() == 40 * i &&
           shared.bot.receivedAirtimeMs() == 40 * i &&
           radio.receivedAirtimeMs() == 40 * i);
  }
  assert(shared.repeater.rxDrops == 1 && shared.bot.rxDrops == 1 && shared.lua.rxDrops == 1);
  uint8_t bytes[255];
  assert(shared.repeater.recvRaw(bytes, sizeof(bytes)) == 3 &&
         shared.repeater.recvRaw(bytes, sizeof(bytes)) == 3);
  assert(shared.bot.recvRaw(bytes, 1) == 0 && shared.bot.recvRaw(bytes, sizeof(bytes)) == 3);
  assert(radio.recvRaw(bytes, sizeof(bytes)) == 3 &&
         radio.recvRaw(bytes, sizeof(bytes)) == 3 && radio.recvRaw(bytes, sizeof(bytes)) == 0);
  assert(radio.receivedAirtimeMs() == 120);
  physical.incoming.push_back({ROUTE_TYPE_DIRECT});
  shared.poll();
  assert(shared.repeater.rxDrops == 2 && shared.bot.rxDrops == 3 &&
         shared.lua.rxDrops == 2 && radio.getPacketsRecvErrors() == 2);
  physical.incoming.push_back({ROUTE_TYPE_TRANSPORT_DIRECT, 0});
  shared.poll();
  assert(shared.repeater.rxDrops == 3 && shared.bot.rxDrops == 4 &&
         shared.lua.rxDrops == 3 && radio.getPacketsRecvErrors() == 3);
  assert(radio.receivedAirtimeMs() == 200 && shared.lua.recvRaw(bytes, sizeof(bytes)) == 0);
  const uint8_t packet[]{ROUTE_TYPE_DIRECT, 0};
  assert(shared.repeater.startSendRaw(packet, sizeof(packet)));
  now += 40;
  assert(shared.repeater.isSendComplete());
  shared.repeater.onSendFinished();
  shared.poll();
  assert(shared.idle() && shared.rfMs == 40 && radio.receivedAirtimeMs() == 200);
  radio.detach();
  assert(radio.receivedAirtimeMs() == 0 && shared.receivedAirtimeMs() == 200);
}
static void measurementGenerations() {
  now = 1000;
  Clock clock;
  Physical first, replacement;
  nrfmast::SharedRadio oldRadio(first, clock, true), newRadio(replacement, clock, true);
  oldRadio.begin();
  for (unsigned i = 0; i < 10; ++i) {
    first.incoming.push_back({ROUTE_TYPE_DIRECT, 0});
    oldRadio.poll();
  }
  const uint8_t bytes[]{ROUTE_TYPE_DIRECT, 0};
  assert(oldRadio.repeater.startSendRaw(bytes, sizeof(bytes)));
  now += 40;
  assert(oldRadio.repeater.isSendComplete());
  oldRadio.repeater.onSendFinished();
  onchip::LocalRadio radio;
  assert(radio.attach(oldRadio.lua, {}));
  mesh::QueuedRadioStats before, after;
  assert(radio.getQueuedRadioStats(before) && before.generation == 1 &&
         before.aggregate_rf_ms == 40 && radio.receivedAirtimeMs() == 400);
  onchip::AdaptiveAdmission admission;
  admission.configure(true, 3600, now);
  admission.sample(now, before.generation, before.aggregate_rf_ms,
                   radio.receivedAirtimeMs(), before.aggregate_queued);
  uint8_t principal[32]{1};
  uint32_t job = 1;
  assert(admission.reserve(now, principal, &job, 1000, true) == onchip::AdaptiveAdmission::Allowed);
  admission.accepted(&job);
  radio.configure({});
  assert(!radio.attach(newRadio.lua, {}) && radio.getQueuedRadioStats(after) &&
         after.generation == before.generation &&
         after.configuration_generation == before.configuration_generation + 1);
  radio.detach();
  assert(!radio.getQueuedRadioStats(after));
  now += 1000;
  newRadio.begin();
  assert(radio.attach(newRadio.lua, {}) && radio.getQueuedRadioStats(after) &&
         after.generation == before.generation + 1 &&
         after.aggregate_rf_ms == 0 && radio.receivedAirtimeMs() == 0);
  admission.sample(now, after.generation, after.aggregate_rf_ms,
                   radio.receivedAirtimeMs(), after.aggregate_queued);
  assert(admission.loadPermille() == 0 && !admission.congested() &&
         admission.pending() == 1 && !replacement.sending && !newRadio.txStarted);
  admission.finish(now, &job, false, 0);
  admission.sample(now, after.generation, 0, 0, 4);
  uint32_t nextJob = 2;
  assert(admission.pending() == 0 && admission.reserve(now, principal, &nextJob, 1000, true) ==
         onchip::AdaptiveAdmission::Congestion);
  puts("Pine radio generation: successful rebind rebases reset RF/RX counters; failed attach/configuration do not change measurement generation; reservations/unknown charge retained");
}
static void queueObservability() {
  Clock clock;
  Physical physical;
  nrfmast::SharedRadio shared(physical, clock, true);
  StaticPoolPacketManager repeater(2), carrier(2);
  shared.observeQueues(repeater, carrier);
  shared.otherQueued = 255;
  onchip::LocalRadio radio;
  assert(radio.attach(shared.lua, {}));
  mesh::QueuedRadioStats stats;
  assert(radio.getQueuedRadioStats(stats) && stats.aggregate_queued == 0);
  onchip::AdaptiveAdmission admission;
  admission.configure(true, 3600, now);
  admission.sample(now, stats.generation, stats.aggregate_rf_ms,
                   radio.receivedAirtimeMs(), stats.aggregate_queued);
  for (auto *queue : {&repeater, &carrier})
    for (unsigned i = 0; i < 2; ++i)
      queue->queueOutbound(queue->allocNew(), 0, now);
  const uint8_t bytes[]{ROUTE_TYPE_DIRECT, 0};
  uint32_t job;
  assert(radio.queueTransmit(bytes, sizeof(bytes), 0, 100, 0, job) &&
         radio.queueTransmit(bytes, sizeof(bytes), 0, 100, 0, job));
  assert(radio.getQueuedRadioStats(stats) && stats.aggregate_queued == 6 && !stats.aggregate_transmitting);
  admission.sample(now, stats.generation, stats.aggregate_rf_ms,
                   radio.receivedAirtimeMs(), stats.aggregate_queued);
  uint8_t principal[32]{1};
  assert(admission.congested() && admission.scalePermille() == 500 &&
         admission.work(now, principal, true, 2, 0, 40) == onchip::AdaptiveAdmission::Congestion);
  for (auto *queue : {&repeater, &carrier})
    while (auto *packet = queue->getNextOutbound(now)) queue->free(packet);
  mesh::QueuedTransmitResult result;
  now += 100;
  assert(radio.pollQueuedResult(result) && result.state == queued_tx::ACCEPTED && physical.sending);
  assert(radio.getQueuedRadioStats(stats) && stats.aggregate_queued == 1 && stats.aggregate_transmitting);
  physical.complete = false;
  now += 10;
  radio.detach();
  assert(shared.queuedPackets() == 0 && shared.aggregateRfMs() == 0 &&
         shared.aggregateActiveMs() == 10 && !shared.lastTransmitWasConfirmed());
  shared.repeater.aggregateStats(stats);
  assert(stats.aggregate_rf_ms == 0 && stats.aggregate_failures == 1);
}
static void transmitCertainty() {
  Clock clock;
  Physical physical;
  nrfmast::SharedRadio shared(physical, clock, true);
  onchip::LocalRadio radio;
  assert(radio.attach(shared.lua, {}));
  onchip::AdaptiveAdmission admission;
  admission.configure(true, 3600, now);
  admission.sample(now, 1, 0, 0, 4);
  uint8_t principal[32]{1};
  const uint8_t bytes[]{ROUTE_TYPE_DIRECT, 0};
  uint32_t job;
  mesh::QueuedTransmitResult result;
  for (bool expire : {false, true}) {
    assert(admission.reserve(now, principal, &job, 1000, true) == onchip::AdaptiveAdmission::Allowed);
    admission.accepted(&job);
    physical.failStart = !expire;
    physical.airtime = expire ? 75 : 60;
    assert(radio.getEstAirtimeFor(sizeof(bytes)) == physical.airtime);
    assert(radio.queueTransmit(bytes, sizeof(bytes), 0, expire ? 50 : 0, expire ? 20 : 0, job));
    assert(radio.pollQueuedResult(result) && result.state == queued_tx::ACCEPTED && !result.has_rf_ms);
    if (expire) now += 21;
    assert(radio.pollQueuedResult(result) && result.state == queued_tx::FAILED &&
           result.reason == (expire ? queued_tx::EXPIRED : queued_tx::START_FAILED) &&
           result.has_rf_ms && result.rf_ms == 0 && result.estimated_ms == physical.airtime &&
           !physical.sending && shared.rfMs == 0);
    admission.finish(now, &job, result.has_rf_ms, result.rf_ms);
    assert(admission.pending() == 0 &&
           admission.reserve(now, principal, &job, 1800, true) == onchip::AdaptiveAdmission::Allowed);
    admission.released(now, &job);
  }
  physical.failStart = false;
  physical.complete = false;
  assert(admission.reserve(now, principal, &job, 1000, true) == onchip::AdaptiveAdmission::Allowed);
  admission.accepted(&job);
  assert(radio.queueTransmit(bytes, sizeof(bytes), 0, 0, 0, job));
  assert(radio.pollQueuedResult(result) && result.state == queued_tx::ACCEPTED && physical.sending);
  now += 2 * physical.airtime + 2001;
  shared.poll();
  assert(radio.pollQueuedResult(result) && result.state == queued_tx::UNKNOWN &&
         result.reason == queued_tx::RF_TIMEOUT && !result.has_rf_ms &&
         result.rf_ms == 0 && result.estimated_ms == physical.airtime);
  mesh::QueuedRadioStats stats;
  assert(radio.getQueuedRadioStats(stats) && stats.source_rf_ms == 0 && stats.aggregate_rf_ms == 0);
  assert(shared.aggregateActiveMs() == 2 * physical.airtime + 2001);
  admission.finish(now, &job, result.has_rf_ms, result.rf_ms);
  assert(admission.pending() == 0 &&
         admission.reserve(now, principal, &job, 1000, true) == onchip::AdaptiveAdmission::Congestion);
}
static void consumptiveCompletions() {
  for (uint32_t start : {uint32_t(1000), UINT32_MAX - 1000}) {
    now = start;
    Clock clock;
    Physical physical;
    nrfmast::SharedRadio shared(physical, clock, true);
    assert(shared.setAirtimeFactor(0));
    shared.begin();
    onchip::LocalRadio radio;
    assert(radio.attach(shared.lua, {}));
    const uint8_t packet[]{ROUTE_TYPE_DIRECT, 0};
    for (auto* port : {&shared.repeater, &shared.bot, &shared.lua}) {
      const unsigned before = physical.completionQueries;
      assert(port->startSendRaw(packet, sizeof(packet)) && port->isSendComplete());
      assert(port->isSendComplete() && physical.completionQueries == before + 1);
      now += 40;
      port->onSendFinished();
      assert(!port->isSendComplete() && physical.completionQueries == before + 1 &&
            shared.lastTransmitWasConfirmed());
    }
    assert(shared.aggregateRfMs() == 120 && shared.txSucceeded == 3);
    uint32_t job;
    mesh::QueuedTransmitResult result;
    assert(radio.queueTransmit(packet, sizeof(packet), 0, 0, 0, job));
    assert(radio.pollQueuedResult(result) && result.state == queued_tx::ACCEPTED);
    now += 2080;
    const unsigned beforeWatchdog = physical.completionQueries;
    shared.poll();
    assert(!shared.idle() && physical.completionQueries == beforeWatchdog + 1);
    assert(radio.pollQueuedResult(result) && result.state == queued_tx::SUCCEEDED &&
          result.has_rf_ms && result.rf_ms == 2080 &&
          physical.completionQueries == beforeWatchdog + 1);
    assert(shared.txSucceeded == 4 && shared.txFailed == 0 &&
          shared.aggregateRfMs() == 2200 && physical.finished == 4);
    physical.complete = false;
    assert(radio.queueTransmit(packet, sizeof(packet), 0, 0, 0, job));
    assert(radio.pollQueuedResult(result) && result.state == queued_tx::ACCEPTED);
    assert(!shared.lua.isSendComplete());
    now += 2081;
    shared.poll();
    const unsigned afterTimeout = physical.completionQueries;
    assert(shared.idle() && shared.lua.isSendComplete() && shared.lua.transmissionFailed() &&
          physical.completionQueries == afterTimeout);
    assert(radio.pollQueuedResult(result) && result.state == queued_tx::UNKNOWN &&
          !result.has_rf_ms && result.rf_ms == 0 &&
          physical.completionQueries == afterTimeout);
    assert(!shared.lua.transmissionFailed() && !shared.lua.isSendComplete());
    assert(shared.txFailed == 1 && shared.aggregateRfMs() == 2200 &&
          !shared.lastTransmitWasConfirmed());
    physical.complete = true;
    assert(radio.queueTransmit(packet, sizeof(packet), 0, 0, 0, job));
    assert(radio.pollQueuedResult(result) && result.state == queued_tx::ACCEPTED);
    now += 40;
    assert(radio.pollQueuedResult(result) && result.state == queued_tx::SUCCEEDED &&
          result.has_rf_ms && result.rf_ms == 40 && shared.txSucceeded == 5 &&
          shared.aggregateRfMs() == 2240 && physical.finished == 6);
  }
  puts("Pine consumptive completion: first true latched across all three ports/watchdog; timeout terminal survives ownership release; reset/new send and millis rollover");
}
static void detachedLuaRecovery() {
  now = 1000;
  Clock clock;
  Physical physical;
  nrfmast::SharedRadio shared(physical, clock, true);
  assert(shared.setAirtimeFactor(0));
  onchip::LocalRadio radio;
  const uint8_t bytes[]{ROUTE_TYPE_DIRECT, 0};
  uint32_t job;
  mesh::QueuedTransmitResult result;
  assert(radio.attach(shared.lua, {}) && radio.queueTransmit(bytes, sizeof(bytes), 0, 0, 0, job));
  assert(radio.pollQueuedResult(result) && result.state == queued_tx::ACCEPTED);
  physical.complete = false;
  now += 10;
  radio.detach();
  assert(shared.idle() && physical.finished == 1 && shared.txFailed == 1);
  assert(radio.attach(shared.lua, {}) && radio.queueTransmit(bytes, sizeof(bytes), 0, 0, 0, job));
  assert(radio.pollQueuedResult(result) && result.state == queued_tx::ACCEPTED && physical.sending);
  physical.complete = true;
  now += 40;
  assert(radio.pollQueuedResult(result) && result.state == queued_tx::SUCCEEDED &&
         result.has_rf_ms && result.rf_ms == 40 && shared.aggregateRfMs() == 40 &&
         shared.aggregateActiveMs() == 50 && shared.txSucceeded == 1 && shared.txFailed == 1);
}
int main() {
  for (bool botRole : {false, true}) {
    stalledNativeDispatcher(false, botRole);
    stalledNativeDispatcher(true, botRole);
  }
  detachedLuaRecovery();
  consumptiveCompletions();
  receiveObservability();
  measurementGenerations();
  queueObservability();
  transmitCertainty();
  Clock clock;
  Physical physical;
  nrfmast::SharedRadio shared(physical, clock, true);
  onchip::LocalRadio radio;
  assert(radio.attach(shared.lua, {}) && radio.sourceSlot() == 2);
  const uint8_t packet[]{ROUTE_TYPE_DIRECT, 0};
  uint32_t job;
  mesh::QueuedTransmitResult result;
  mesh::QueuedRadioStats stats;
  assert(radio.queueTransmit(packet, sizeof(packet), 0, 0, 0, job));
  assert(radio.pollQueuedResult(result) && result.state == queued_tx::ACCEPTED && physical.sending);
  now += 40;
  assert(radio.pollQueuedResult(result) && result.state == queued_tx::SUCCEEDED &&
         result.has_rf_ms && result.rf_ms == 40);
  assert(shared.idle() && shared.txSucceeded == 1 && shared.rfMs == 40);
  shared.otherQueued = 3;
  assert(radio.getQueuedRadioStats(stats) && stats.source_successes == 1 &&
         stats.aggregate_successes == 1 && stats.aggregate_queued == 3 &&
         stats.source_rf_ms == 40 && stats.aggregate_rf_ms == 40);
  now += 40;
  assert(shared.repeater.startSendRaw(packet, sizeof(packet)));
  now += 40; assert(shared.repeater.isSendComplete()); shared.repeater.onSendFinished();
  assert(radio.getQueuedRadioStats(stats) && stats.source_successes == 1 && stats.aggregate_successes == 2 &&
         stats.source_rf_ms == 40 && stats.aggregate_rf_ms == 80);
  now += 40;
  physical.complete = false;
  assert(radio.queueTransmit(packet, sizeof(packet), 0, 0, 0, job));
  assert(radio.pollQueuedResult(result) && result.state == queued_tx::ACCEPTED);
  now += 2081; shared.poll();
  assert(radio.pollQueuedResult(result) && result.state == queued_tx::UNKNOWN &&
         result.reason == queued_tx::RF_TIMEOUT && !result.has_rf_ms && result.rf_ms == 0);
  assert(shared.idle() && !physical.sending && shared.txFailed == 1);
  assert(shared.aggregateRfMs() == 80 && shared.aggregateActiveMs() == 80 + 2081 &&
         shared.lastTransmitMs() == 2081 && !shared.lastTransmitWasConfirmed());
  assert(radio.getQueuedRadioStats(stats) && stats.source_rf_ms == 40 && stats.aggregate_rf_ms == 80 &&
         stats.source_failures == 1 && stats.aggregate_failures == 1);
  now += 2081;
  assert(radio.queueTransmit(packet, sizeof(packet), 0, 50, 20, job));
  assert(radio.pollQueuedResult(result) && result.state == queued_tx::ACCEPTED && !physical.sending);
  now += 21;
  assert(radio.pollQueuedResult(result) && result.state == queued_tx::FAILED && result.reason == queued_tx::EXPIRED);
  assert(radio.queueTransmit(packet, sizeof(packet), 0, 10, 0, job));
  assert(radio.pollQueuedResult(result) && result.state == queued_tx::ACCEPTED && !physical.sending);
  now += 10;
  uint8_t bytes[255];
  radio.recvRaw(bytes, sizeof(bytes));
  assert(physical.sending);
  radio.detach();
  assert(shared.idle() && !physical.sending && shared.txFailed == 2);
  shared.repeater.aggregateStats(stats);
  assert(stats.aggregate_rf_ms == 80 && stats.aggregate_failures == 2);
  now += 10;
  physical.incoming.push_back({ROUTE_TYPE_DIRECT, 0, 7});
  shared.poll();
  assert(shared.repeater.recvRaw(bytes, sizeof(bytes)) == 3);
  assert(shared.bot.recvRaw(bytes, sizeof(bytes)) == 3);
  assert(shared.lua.recvRaw(bytes, sizeof(bytes)) == 3);
  assert(shared.repeater.startSendRaw(packet, sizeof(packet)));
  physical.complete = true; now += 40;
  assert(shared.repeater.isSendComplete()); shared.repeater.onSendFinished();
  assert(shared.idle());
  shared.repeater.aggregateStats(stats);
  assert(stats.aggregate_rf_ms == 120 && stats.aggregate_successes == 3 && stats.aggregate_failures == 2);
  puts("Pine radio: physical RX airtime before drops, no fanout/TX double count, three-port RF snapshots, zero-expiry admission, delay/expiry, confirmed vs uncertain TX, aggregate stats, detach and repeater recovery");
}
