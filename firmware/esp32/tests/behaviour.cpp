// SPDX-License-Identifier: Apache-2.0
// Reuse the existing physical arbiter fixtures and their independent wire
// vectors.
#define main upstream_mux_tests
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type"
#include "../../../test_support/phy_parity/combined.cpp"
#pragma GCC diagnostic pop
#undef main
#include "../CommandPolicy.h"
#include "../LocalRadio.h"
#include "../NativePacketHost.h"
#include <helpers/StatsFormatHelper.h>
#include <helpers/StaticPoolPacketManager.h>

struct StatsClock : mesh::MillisecondClock {
  unsigned long getMillis() override { return 123456; }
};

struct StatsDispatcher : mesh::Dispatcher {
  StatsDispatcher(mesh::Radio &radio, StatsClock &clock,
                  mesh::PacketManager &pool)
      : Dispatcher(radio, clock, pool) {}
  mesh::DispatcherAction onRecvPacket(mesh::Packet *) override {
    return ACTION_RELEASE;
  }
};

static void operator_phy_boot() {
  nvs_test::reset();
  applied = {};
  Radio radio;
  RNG rng;
  const RadioConfig staging{912525000, 250000, 7, 5, 2};
  WifiKissMultiplexer first;
  first.attachRadio(radio, rng, configure, power);
  assert(first.setInitialConfiguration(staging, true));
  const std::vector<uint8_t> record = {
      0x4d, 0x43, 0x50, 0x01, 0xc8, 0x06, 0x64, 0x36, 0x90, 0xd0, 0x03,
      0x00, 7,    5,    2,    0,    0,    0x80, 0x3f, 1,    0,    0};
  assert(nvs_test::store.durable == record && nvs_test::store.writes == 1 &&
         nvs_test::store.commits == 1 && first.localReady());
  assert(applied.frequency == 912.525f && applied.bandwidth == 250 &&
         applied.sf == 7 && applied.cr == 5 && applied.power == 2);
  nvs_test::store.durable[17] = 0x40;
  nvs_test::store.durable[18] =
      0x40; // Saved factor 3, not a second PHY authority.
  nvs_test::store.durable[19] = 1;
  WifiKissMultiplexer restored;
  restored.attachRadio(radio, rng, configure, power);
  assert(restored.setInitialConfiguration(staging, true));
  RadioDashboard::RadioStatus status;
  restored.dashboardStatus(status);
  assert(status.committed && status.credit_ms == 900000 && radio.cad &&
         nvs_test::store.writes == 1);
  const auto saved = nvs_test::store.durable;
  const unsigned changes = applied.changes, powers = applied.power_changes;
  const RadioConfig conflicts[] = {{915000000, 250000, 7, 5, 2},
                                   {912525000, 125000, 7, 5, 2},
                                   {912525000, 250000, 8, 5, 2},
                                   {912525000, 250000, 7, 6, 2},
                                   {912525000, 250000, 7, 5, 3}};
  for (const auto &conflict : conflicts) {
    WifiKissMultiplexer refused;
    refused.attachRadio(radio, rng, configure, power);
    assert(!refused.setInitialConfiguration(conflict, true) &&
           !refused.localReady());
    assert(applied.changes == changes && applied.power_changes == powers &&
           nvs_test::store.durable == saved && nvs_test::store.writes == 1);
  }
  nvs_test::store.durable[0] = 0;
  WifiKissMultiplexer corrupt;
  corrupt.attachRadio(radio, rng, configure, power);
  assert(!corrupt.setInitialConfiguration(staging, true) &&
         !corrupt.localReady());
  assert(applied.changes == changes && nvs_test::store.writes == 1);
  nvs_test::reset();
  nvs_test::store.fail_commit = true;
  WifiKissMultiplexer failed;
  failed.attachRadio(radio, rng, configure, power);
  assert(!failed.setInitialConfiguration(staging, true) &&
         !failed.localReady());
  assert(nvs_test::store.commits == 1 && nvs_test::store.durable.empty() &&
         applied.changes == changes);
  nvs_test::reset();
  WifiKissMultiplexer legacy;
  legacy.attachRadio(radio, rng, configure, power);
  assert(legacy.setInitialConfiguration(staging));
  legacy.dashboardStatus(status);
  assert(!status.committed && nvs_test::store.writes == 0);
}

static void uart_receive_without_tcp() {
  const uint8_t slot = KISS_MAX_TCP_CLIENTS + KISS_LOCAL_SOURCES;
  {
    Fixture f;
    TestUART uart;
    mesh::QueuedRadioStats sample;
    sample.generation = 91;
    assert(f.mux.attachStream(uart));
    assert(!f.mux.getQueuedRadioStats(slot, sample) && sample.generation == 91);
  }
  StreamFixture f;
  for (int fd : f.peers)
    assert(shutdown(fd, SHUT_RDWR) == 0);
  f.step();
  assert(f.mux.clientCount() == 0);
  f.uart.output.clear();
  const uint8_t packet[] = {0xc0, 0xdb, 0x42};
  f.mux.received(packet, sizeof(packet), -97, 3.25f);
  f.modem.onPacketReceived(13, -97, packet, sizeof(packet));
  f.step();
  const std::vector<uint8_t> expected = {0xc0, 0x00, 0xdb, 0xdc, 0xdb,
                                         0xdd, 0x42, 0xc0, 0xc0, 0x06,
                                         0xf9, 0x0d, 0x9f, 0xc0};
  assert(f.uart.output == expected);
  mesh::QueuedRadioStats sample;
  assert(f.mux.getQueuedRadioStats(slot, sample) &&
         sample.generation == f.generation && sample.aggregate_rf_ms == 0);
  const uint32_t previousGeneration = sample.generation;
  f.uart.output.clear();
  f.renew();
  assert(f.mux.getQueuedRadioStats(slot, sample) &&
         sample.generation != previousGeneration && sample.source_rf_ms == 0);
}

static void stream_role_presence_does_not_fault() {
  StreamFixture f;
  std::vector<uint8_t> claim{queued_tx::ROLE_PRESENCE, 1, 1};
  claim.resize(3 + queued_tx::ROLE_KEY_SIZE);
  claim[3] = 0x51;
  f.hardwareUART(claim);
  const auto frames = f.uart.take();
  assert(frames.size() == 1 && frames[0].size() == 10 &&
         frames[0][1] == 0xa6 && frames[0][2] == 1 &&
         frames[0][3] == queued_tx::INVALID &&
         queued_tx::get32(frames[0].data() + 4) == f.generation &&
         frames[0][8] == 0 && frames[0][9] == 0);
  f.mux.pollStream();
  assert(!f.mux.streamFaulted());
  assert(f.readback().size() == queued_tx::PROFILE_SIZE);
}

static void embedded_sources() {
  Fixture f;
  onchip::LocalRadio repeater, room, companion, management, extra;
  static StaticPoolPacketManager statsPool(1);
  StatsClock statsClock;
  StatsDispatcher repeaterNode(repeater, statsClock, statsPool);
  StatsDispatcher roomNode(room, statsClock, statsPool);
  uint32_t unavailable = 73;
  mesh::QueuedRadioStats sample;
  sample.generation = 91;
  assert(!repeater.getQueuedRadioStats(sample) && sample.generation == 91);
  assert(!f.mux.getQueuedRadioStats(UINT8_MAX, sample) &&
         sample.generation == 91);
  assert(!repeaterNode.tryGetTotalAirTime(unavailable) && unavailable == 73);
  assert(!repeaterNode.tryGetRemainingTxBudget(unavailable) &&
         unavailable == 73);
  assert(repeaterNode.getTotalAirTime() == UINT32_MAX &&
         repeaterNode.getRemainingTxBudget() == UINT32_MAX);
  assert(repeaterNode.getTotalAirTimeSeconds() == UINT32_MAX);
  assert(repeater.attach(f.mux) && room.attach(f.mux) &&
         companion.attach(f.mux) && management.attach(f.mux));
  assert(!extra.attach(f.mux));
  assert(repeater.queuedCount() == 0 && !repeater.hasPendingWork());
  assert(f.mux.clientCount() == 2);
  assert(repeater.setQueuedSourcePolicy(99) && room.setQueuedSourcePolicy(0));
  assert(!room.setQueuedSourcePolicy(NAN));
  assert(repeater.getQueuedRadioStats(sample) && sample.generation != 0 &&
         sample.configuration_generation != 0 &&
         sample.captured_ms == clock_ms && sample.source_credit_ms == 36000 &&
         sample.aggregate_credit_ms == 1800000);
  assert(repeaterNode.getRemainingTxBudget() == 36000);
  const uint8_t low[] = {0x11, 0}, high[] = {0x22, 0};
  uint32_t lowJob, highJob;
  assert(repeater.queueTransmit(low, sizeof(low), 5, 100, 2000, lowJob));
  assert(room.queueTransmit(high, sizeof(high), 0, 0, 2000, highJob));
  assert(repeater.queuedCount() == 1 && room.queuedCount() == 1 &&
         companion.queuedCount() == 0);
  char stats[128];
  StatsFormatHelper::formatCoreStats(stats, f.board, statsClock, 5,
                                    repeater.queuedCount());
  assert(strcmp(stats, "{\"battery_mv\":4000,\"uptime_secs\":123,"
                       "\"errors\":5,\"queue_len\":1}") == 0);
  StatsFormatHelper::formatCoreStats(stats, f.board, statsClock, 5, &statsPool);
  assert(strcmp(stats, "{\"battery_mv\":4000,\"uptime_secs\":123,"
                       "\"errors\":5,\"queue_len\":0}") == 0);
  f.mux.serviceTransmit();
  assert(room.getQueuedRadioStats(sample) && sample.aggregate_queued == 1 &&
         sample.aggregate_transmitting && sample.source_rf_ms == 0);
  assert(room.queuedCount() == 0 && room.hasPendingWork() &&
         repeater.queuedCount() == 1);
  assert(f.radio.transmitted.size() == 1 && f.radio.transmitted[0][0] == 0x22);
  uint8_t packet[255];
  assert(repeater.recvRaw(packet, sizeof(packet)) ==
         0); // admission isn't reception
  f.finish(37);
  assert(room.getQueuedRadioStats(sample) && sample.source_rf_ms == 37 &&
         sample.aggregate_rf_ms == 37 && sample.source_successes == 1 &&
         sample.aggregate_successes == 1 && sample.source_failures == 0 &&
         sample.aggregate_failures == 0 && sample.captured_ms == clock_ms &&
         sample.aggregate_queued == 1 && !sample.aggregate_transmitting);
  assert(roomNode.getTotalAirTime() == 37 &&
         roomNode.getRemainingTxBudget() == 1799963);
  assert(repeaterNode.getTotalAirTime() == 0 &&
         repeaterNode.getRemainingTxBudget() == 36000);
  receive(f.peers[0]);
  f.hardware(0, {0x24, 1});
  const auto wire = receive(f.peers[0]);
  assert(wire.size() == 1 && wire[0].size() == 41 && wire[0][1] == 0xa4 &&
         queued_tx::get32(wire[0].data() + 7) == 1799963 && wire[0][11] == 37 &&
         wire[0][12] == 0 && wire[0][39] == 1 && wire[0][40] == 0);
  assert(f.mux.getQueuedRadioStats(0, sample) &&
         sample.generation == f.generations[0] && sample.source_rf_ms == 0 &&
         sample.aggregate_rf_ms == 37);
  assert(!room.hasPendingWork() && repeater.hasPendingWork());
  mesh::QueuedTransmitResult result;
  assert(room.pollQueuedResult(result) && result.job == highJob &&
         result.state == queued_tx::ACCEPTED && !result.has_rf_ms &&
         result.rf_ms == 0);
  assert(room.pollQueuedResult(result) && result.job == highJob &&
         result.state == 2 && result.rf_ms == 37 && result.has_rf_ms);
  assert(!room.pollQueuedResult(result));
  assert(repeater.recvRaw(packet, sizeof(packet)) == 2 && packet[0] == 0x22 &&
         repeater.lastReceiveWasLocal() && repeater.getPacketsRecv() == 0);
  assert(std::isnan(repeater.getLastRSSI()) &&
         std::isnan(repeater.getLastSNR()));
  assert(onchip::nativeSNR(repeater) == -32 &&
         onchip::nativeRSSI(repeater) == 127);
  assert(repeater.receivedAirtimeMs() == 0 && room.receivedAirtimeMs() == 0 &&
         companion.receivedAirtimeMs() == 0);
  assert(room.recvRaw(packet, sizeof(packet)) == 0);
  assert(companion.recvRaw(packet, sizeof(packet)) == 2);
  clock_ms += 100;
  f.step();
  assert(repeater.queuedCount() == 0 && repeater.hasPendingWork());
  f.finish(24);
  assert(repeater.getQueuedRadioStats(sample) && sample.source_rf_ms == 24 &&
         sample.aggregate_rf_ms == 61 && sample.source_successes == 1 &&
         sample.aggregate_successes == 2);
  const uint32_t observedCredit = sample.aggregate_credit_ms;
  clock_ms += 50;
  assert(repeater.getQueuedRadioStats(sample) &&
         sample.aggregate_credit_ms == observedCredit &&
         sample.captured_ms == clock_ms);
  assert(repeaterNode.getTotalAirTime() == 24 &&
         roomNode.getTotalAirTime() == 37);
  assert(!repeater.hasPendingWork());
  assert(repeater.pollQueuedResult(result) &&
         result.state == queued_tx::ACCEPTED && !result.has_rf_ms);
  assert(repeater.pollQueuedResult(result) && result.rf_ms == 24);
  while (room.recvRaw(packet, sizeof(packet))) {
  }
  while (companion.recvRaw(packet, sizeof(packet))) {
  }
  f.radio.start_ok = false;
  assert(repeater.queueTransmit(low, 2, 0, 0, 5000, lowJob));
  f.step();
  assert(repeater.pollQueuedResult(result) &&
         result.state == queued_tx::ACCEPTED);
  assert(repeater.pollQueuedResult(result) && result.state == 3 &&
         result.reason == queued_tx::START_FAILED && result.rf_ms == 0 &&
         result.has_rf_ms);
  assert(repeater.getQueuedRadioStats(sample) && sample.aggregate_rf_ms == 61 &&
         sample.source_rf_ms == 24 && sample.source_failures == 1 &&
         sample.aggregate_failures == 1);
  assert(room.recvRaw(packet, sizeof(packet)) == 0); // failure cannot reflect
  f.mux.received(high, 2, -91, 4.25f);
  assert(repeater.receivedAirtimeMs() == f.radio.getEstAirtimeFor(2) &&
         room.receivedAirtimeMs() == repeater.receivedAirtimeMs() &&
         companion.receivedAirtimeMs() == repeater.receivedAirtimeMs());
  assert(room.recvRaw(packet, sizeof(packet)) == 2 &&
         !room.lastReceiveWasLocal() && room.getLastRSSI() == -91 &&
         room.getLastSNR() == 4.25f);
  // Bounded shared queue admission also includes pending external jobs.
  f.radio.start_ok = true;
  f.job(0, 1, 0, 60000, 0xee);
  unsigned admitted = 0;
  while (repeater.queueTransmit(low, 2, 1, 60000, 0, lowJob))
    ++admitted;
  assert(admitted == KISS_REQUEST_QUEUE_DEPTH - 1);
  clock_ms += 60000;
  f.step();
  f.finish(1); // The higher-priority external job completes first.
  for (unsigned i = 0; i < admitted; ++i) {
    assert(f.radio.sending);
    f.finish(1);
  }
  unsigned acceptances = 0, terminals = 0;
  while (repeater.pollQueuedResult(result)) {
    if (result.state == queued_tx::ACCEPTED)
      ++acceptances;
    else {
      assert(result.state == queued_tx::SUCCEEDED && result.rf_ms == 1);
      ++terminals;
    }
  }
  assert(acceptances == admitted && terminals == admitted);
  while (room.recvRaw(packet, sizeof(packet))) {
  }
  assert(repeater.queueTransmit(low, 2, 0, 0, 1000, lowJob));
  f.step();
  clock_ms += 150;
  f.step();
  assert(repeater.pollQueuedResult(result) &&
         result.state == queued_tx::ACCEPTED && !result.has_rf_ms);
  assert(repeater.pollQueuedResult(result) &&
         result.state == queued_tx::UNKNOWN &&
         result.reason == queued_tx::RF_TIMEOUT && result.rf_ms == 150 &&
         result.has_rf_ms);
  assert(!repeater.pollQueuedResult(result));
  assert(room.recvRaw(packet, sizeof(packet)) == 0);
  assert(repeater.getQueuedRadioStats(sample) &&
         sample.source_rf_ms == 24 + admitted + 150 &&
         sample.aggregate_rf_ms == 61 + admitted + 1 + 150 &&
         sample.source_failures == 2 && sample.aggregate_failures == 2 &&
         sample.source_successes == 1 + admitted &&
         sample.aggregate_successes == 3 + admitted);
  assert(repeaterNode.getTotalAirTime() == sample.source_rf_ms);
  const uint32_t previousGeneration = sample.generation;
  f.mux.detachLocal(KISS_MAX_TCP_CLIENTS);
  assert(!repeater.getQueuedRadioStats(sample) &&
         sample.generation == previousGeneration);
  assert(repeaterNode.getTotalAirTime() == UINT32_MAX);
  onchip::LocalRadio replacement;
  assert(replacement.attach(f.mux) && replacement.getQueuedRadioStats(sample) &&
         sample.generation != previousGeneration && sample.source_rf_ms == 0 &&
         sample.source_successes == 0 && sample.source_failures == 0 &&
         sample.aggregate_rf_ms == 61 + admitted + 1 + 150);
  StatsDispatcher replacementNode(replacement, statsClock, statsPool);
  f.radio.maximum = f.radio.duration = 2500;
  assert(replacement.queueTransmit(low, 2, 0, 0, 5000, lowJob));
  f.step();
  f.finish(2345);
  assert(replacementNode.getTotalAirTime() == 2345 &&
         replacementNode.getTotalAirTimeSeconds() == 2);
  uint32_t txMillis = 0;
  bool known = replacementNode.tryGetTotalAirTime(txMillis);
  assert(known && txMillis == 2345);
  StatsFormatHelper::formatRadioStats(stats, &replacement, f.radio, txMillis, 0,
                                    known);
  assert(strcmp(stats, "{\"noise_floor\":0,\"last_rssi\":0,\"last_snr\":0.00,"
                       "\"tx_air_secs\":2,\"rx_air_secs\":0}") == 0);
  f.mux.detachLocal(KISS_MAX_TCP_CLIENTS);
  known = replacementNode.tryGetTotalAirTime(txMillis);
  assert(!known && replacementNode.getTotalAirTimeSeconds() == UINT32_MAX);
  StatsFormatHelper::formatRadioStats(stats, &replacement, f.radio, txMillis, 0,
                                    known);
  assert(strcmp(stats, "{\"noise_floor\":0,\"last_rssi\":0,\"last_snr\":0.00,"
                       "\"tx_air_secs\":4294967295,\"rx_air_secs\":0}") == 0);
}

struct PacketHost final : packet_engine::Host {
  std::vector<packet_engine::Fault> errors;
  uint32_t microsNow() override { return clock_ms * 1000; }
  void fault(const char *, const packet_engine::Metadata &, packet_engine::Fault fault) override {
    errors.push_back(fault);
  }
  bool admit(const packet_engine::Metadata &, const packet_engine::Emission *, uint8_t) override {
    assert(false);
    return false;
  }
};
struct PacketEngine final : packet_engine::Engine {
  std::vector<packet_engine::Metadata> seen;
  packet_engine::Stage change = packet_engine::Stage::Receive;
  packet_engine::Stage drop = packet_engine::Stage::PlainReceive;
  packet_engine::Stage fail = packet_engine::Stage::PlainReceive;
  uint8_t target = UINT8_MAX;
  packet_engine::Decision process(const packet_engine::Metadata &metadata,
                                 packet_engine::Call &call) override {
    using namespace packet_engine;
    seen.push_back(metadata);
    if (metadata.stage == change) {
      const uint8_t byte = 0x61 + uint8_t(change);
      assert(call.write(0, &byte, 1));
    }
    if (metadata.stage == fail) return Decision::Failed;
    if (metadata.stage == drop && (target == UINT8_MAX || target == metadata.destination))
      return Decision::Drop;
    return Decision::Continue;
  }
};
static void packet_pipeline_fanout_and_transmit() {
  using namespace packet_engine;
  Fixture f;
  onchip::LocalRadio first, second;
  assert(first.attach(f.mux) && second.attach(f.mux));
  PacketHost host;
  Pipeline pipeline(host);
  PacketEngine engine;
  assert(pipeline.attach(engine, {"test", 0x3f, 100, 1000}) == Registration::Attached);
  f.mux.packetPipeline(&pipeline);
  uint8_t raw[KISS_MAX_PACKET_SIZE] = {0x11, 0x22};
  uint16_t length = 2;
  assert(f.mux.receiveRaw(raw, length, sizeof(raw), -91, 4.25f));
  assert(length == 2 && raw[0] == 0x61 && engine.seen.size() == 3 &&
         engine.seen[0].stage == Stage::Receive && engine.seen[0].rssi == -91 &&
         engine.seen[0].snrQuarterDb == 17 && !engine.seen[0].local);
  f.modem.onPacketReceived(17, -91, raw, length);
  f.step();
  for (int peer : f.peers) {
    const auto frames = receive(peer);
    assert(frames.size() == 2 && frames[0] == std::vector<uint8_t>({0, 0x61, 0x22}));
  }
  for (auto *local : {&first, &second})
    assert(local->recvRaw(raw, sizeof(raw)) == 2 && raw[0] == 0x61 &&
           !local->lastReceiveWasLocal());
  engine.drop = Stage::LocalDelivery;
  engine.target = second.sourceSlot();
  raw[0] = 0x11;
  length = 2;
  assert(f.mux.receiveRaw(raw, length, sizeof(raw), -91, 4.25f));
  assert(first.recvRaw(raw, sizeof(raw)) == 2 && second.recvRaw(raw, sizeof(raw)) == 0);
  engine.drop = Stage::Receive;
  engine.target = UINT8_MAX;
  raw[0] = 0x11;
  length = 2;
  assert(!f.mux.receiveRaw(raw, length, sizeof(raw), -91, 4.25f));
  assert(raw[0] == 0x11 && first.recvRaw(raw, sizeof(raw)) == 0);

  engine.drop = Stage::PlainReceive;
  engine.change = Stage::Admission;
  uint32_t job;
  const uint8_t outgoing[] = {0x31, 0x32};
  assert(first.queueTransmit(outgoing, sizeof(outgoing), 0, 0, 1000, job));
  engine.change = Stage::Transmit;
  f.step();
  assert(f.radio.transmitted.size() == 1 && f.radio.transmitted[0][0] == 0x64);
  engine.change = Stage::Reflection;
  f.finish(10);
  assert(first.recvRaw(raw, sizeof(raw)) == 0);
  assert(second.recvRaw(raw, sizeof(raw)) == 2 && raw[0] == 0x65 &&
         second.lastReceiveWasLocal());
  assert(f.radio.transmitted[0][0] == 0x64);
  for (int peer : f.peers) {
    const auto frames = receive(peer);
    assert(frames.size() == 2 && frames[0] == std::vector<uint8_t>({0, 0x65, 0x32}));
  }
  mesh::QueuedTransmitResult result;
  assert(first.pollQueuedResult(result) && result.state == queued_tx::ACCEPTED);
  assert(first.pollQueuedResult(result) && result.state == queued_tx::SUCCEEDED);
  engine.drop = Stage::Admission;
  assert(first.queueTransmit(outgoing, sizeof(outgoing), 0, 0, 1000, job));
  assert(first.pollQueuedResult(result) && result.state == queued_tx::ACCEPTED);
  assert(first.pollQueuedResult(result) && result.state == queued_tx::REJECTED &&
         result.reason == queued_tx::ENGINE_DROP);
  f.step();
  assert(f.radio.transmitted.size() == 1);
  engine.drop = Stage::Transmit;
  assert(first.queueTransmit(outgoing, sizeof(outgoing), 0, 0, 1000, job));
  f.step();
  assert(first.pollQueuedResult(result) && result.state == queued_tx::ACCEPTED);
  assert(first.pollQueuedResult(result) && result.state == queued_tx::FAILED &&
         result.reason == queued_tx::ENGINE_DROP);
  assert(f.radio.transmitted.size() == 1 && !f.radio.sending);

  engine.drop = Stage::PlainReceive;
  engine.change = engine.fail = Stage::Receive;
  raw[0] = 0x11;
  length = 2;
  assert(f.mux.receiveRaw(raw, length, sizeof(raw), -91, 4.25f));
  assert(raw[0] == 0x11 && !pipeline.enabled(0) &&
         host.errors.back() == Fault::Execution);
  assert(first.recvRaw(raw, sizeof(raw)) == 2 && raw[0] == 0x11);
  assert(second.recvRaw(raw, sizeof(raw)) == 2 && raw[0] == 0x11);
  f.mux.packetPipeline(nullptr);
}

static uint32_t packetClock() { return clock_ms * 1000; }
static std::vector<packet_engine::Fault> packetErrors;
static void packetReport(const char *, const packet_engine::Metadata &, packet_engine::Fault fault) {
  packetErrors.push_back(fault);
}
struct EmittingEngine final : packet_engine::Engine {
  unsigned origins = 0, reflections = 0;
  packet_engine::Decision process(const packet_engine::Metadata &metadata,
                                 packet_engine::Call &call) override {
    using namespace packet_engine;
    if (metadata.engineOrigin) ++origins;
    if (metadata.local) ++reflections;
    if (metadata.stage == Stage::Receive) {
      const uint8_t bytes[] = {0x51, 0x52};
      assert(call.emit(bytes, sizeof(bytes)));
      assert(call.emit(bytes, sizeof(bytes)));
      const uint8_t change = 0x41;
      assert(call.write(0, &change, 1));
    }
    return Decision::Continue;
  }
};
static void packet_pipeline_scheduler_emissions() {
  using namespace packet_engine;
  Fixture f;
  packetErrors.clear();
  onchip::NativePacketHost host(f.mux, packetClock, packetReport);
  onchip::NativePacketHost missingClock(f.mux, nullptr, packetReport);
  assert(!missingClock.begin());
  assert(host.begin());
  onchip::NativePacketHost conflict(f.mux, packetClock, packetReport);
  assert(!conflict.begin());
  EmittingEngine engine;
  assert(host.pipeline().attach(engine, {"emit", 0x3f, 100, 1000}) == Registration::Attached);
  onchip::LocalRadio local;
  assert(local.attach(f.mux));
  uint8_t raw[KISS_MAX_PACKET_SIZE] = {0x11, 0x22};
  uint16_t length = 2;
  assert(f.mux.receiveRaw(raw, length, sizeof(raw), -91, 4.25f) && raw[0] == 0x41);
  mesh::QueuedRadioStats stats;
  assert(f.mux.getQueuedRadioStats(f.mux.engineSourceSlot(), stats) &&
         stats.aggregate_queued == 2 && stats.source_rf_ms == 0);
  assert(local.recvRaw(raw, sizeof(raw)) == 2 && raw[0] == 0x41 &&
         !local.lastReceiveWasLocal());
  f.step();
  assert(f.radio.transmitted.size() == 1 &&
         f.radio.transmitted[0] == std::vector<uint8_t>({0x51, 0x52}));
  f.finish(10);
  assert(f.radio.transmitted.size() == 2 && engine.origins == 4 &&
         engine.reflections == 2);
  f.finish(20);
  assert(f.mux.getQueuedRadioStats(f.mux.engineSourceSlot(), stats) &&
         stats.aggregate_queued == 0 && stats.source_rf_ms == 30 &&
         stats.aggregate_rf_ms == 30 && stats.source_successes == 2 &&
         stats.source_credit_ms == queued_tx::WINDOW_MS / 2 - 20);
  assert(f.radio.transmitted.size() == 2 && packetErrors.empty());
  while (local.recvRaw(raw, sizeof(raw))) assert(local.lastReceiveWasLocal());
  // A two-packet effect cannot partially occupy the final queue entry.
  for (unsigned i = 0; i < KISS_REQUEST_QUEUE_DEPTH - 1; ++i)
    f.job(0, i + 1, 4, 60000, 0x61);
  raw[0] = 0x11;
  length = 2;
  assert(f.mux.receiveRaw(raw, length, sizeof(raw), -91, 4.25f) && raw[0] == 0x11);
  assert(host.status().faults == 1 && host.status().lastFault == Fault::EmissionRejected &&
         host.pipeline().enabled(0));
  assert(f.mux.getQueuedRadioStats(f.mux.engineSourceSlot(), stats) &&
         stats.aggregate_queued == KISS_REQUEST_QUEUE_DEPTH - 1);
  host.stop();
  assert(!f.mux.getQueuedRadioStats(f.mux.engineSourceSlot(), stats));
  raw[0] = 0x11;
  length = 2;
  assert(f.mux.receiveRaw(raw, length, sizeof(raw), -91, 4.25f) && raw[0] == 0x11);
}

int main() {
  operator_phy_boot();
  assert(onchip::clearsAdminPassword("password "));
  assert(!onchip::clearsAdminPassword("password configured"));
  assert(!onchip::clearsAdminPassword("set guest.password "));
  uart_receive_without_tcp();
  stream_role_presence_does_not_fault();
  embedded_sources();
  packet_pipeline_fanout_and_transmit();
  packet_pipeline_scheduler_emissions();
  for (auto command : {"clkreboot",
                       "set radio 900,250,7,5", "set tx 22", "clear stats"})
    assert(onchip::unsafeCLI(command));
  for (auto command : {"get name", "advert", "set name hello", "set repeat on",
                       "reboot", "set prv.key deadbeef", "erase"})
    assert(!onchip::unsafeCLI(command));
  puts("On-chip local arbiter and command boundaries passed");
}
