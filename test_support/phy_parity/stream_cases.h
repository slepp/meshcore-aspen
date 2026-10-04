#pragma once

#include <deque>
#include <algorithm>

struct TestUART : Stream {
  std::deque<uint8_t> input;
  std::vector<uint8_t> output;
  size_t room = 65536, short_write = 65536;
  unsigned reads = 0, writes = 0;
  int available() override { return input.size(); }
  int read() override {
    if (input.empty()) return -1;
    const auto byte = input.front();
    input.pop_front();
    ++reads;
    return byte;
  }
  int availableForWrite() override { return room; }
  size_t write(const uint8_t* data, size_t n) override {
    assert(n <= room);
    ++writes;
    n = std::min(n, short_write);
    output.insert(output.end(), data, data + n);
    room -= n;
    return n;
  }
  void flush() override { assert(false && "UART must never wait for a flush"); }
  void feed(const std::vector<uint8_t>& bytes) {
    input.insert(input.end(), bytes.begin(), bytes.end());
  }
  std::vector<std::vector<uint8_t>> take() {
    std::vector<std::vector<uint8_t>> frames;
    std::vector<uint8_t> frame;
    bool escaped = false;
    for (auto byte : output) {
      if (byte == 0xc0) {
        if (!frame.empty()) frames.push_back(frame);
        frame.clear();
        escaped = false;
      } else if (escaped) {
        assert(byte == 0xdc || byte == 0xdd);
        frame.push_back(byte == 0xdc ? 0xc0 : 0xdb);
        escaped = false;
      } else if (byte == 0xdb) escaped = true;
      else frame.push_back(byte);
    }
    assert(frame.empty() && !escaped);
    output.clear();
    return frames;
  }
};

#if KISS_STREAM_ENDPOINT
static void dashboard_stream(WifiKissMultiplexer& mux, uint32_t generation,
                             bool connected, bool negotiated, bool fault,
                             uint32_t overflows) {
  RadioDashboard dashboard;
  RadioDashboard::RadioStatus status;
  mux.dashboardStatus(status);
  assert(status.stream.enabled &&
         status.stream.slot == KISS_MAX_TCP_CLIENTS + KISS_LOCAL_SOURCES &&
         status.stream.generation == generation &&
         status.stream.connected == connected &&
         status.stream.negotiated == negotiated &&
         status.stream.fault == fault &&
         status.stream.output_overflows == overflows);
  assert(dashboard.publish(clock_ms, status));
  RadioDashboard::Snapshot snapshot;
  assert(dashboard.snapshot(snapshot));
  std::vector<char> json(RadioDashboard::JSON_CAPACITY);
  assert(RadioDashboard::formatJSON(snapshot, "test-radio", json.data(), json.size()));
  char expected[200];
  snprintf(expected, sizeof(expected),
           "\"stream\":{\"slot\":%u,\"generation\":%u,\"connected\":%s,"
           "\"negotiated\":%s,\"fault\":%s,\"output_overflows\":%u}",
           KISS_MAX_TCP_CLIENTS + KISS_LOCAL_SOURCES, generation,
           connected ? "true" : "false", negotiated ? "true" : "false",
           fault ? "true" : "false", overflows);
  assert(strstr(json.data(), expected));
}

struct StreamFixture : Fixture {
  TestUART uart;
  uint32_t generation = 0;
  explicit StreamFixture(float factor = 1) : Fixture(factor) {
    dashboard_stream(mux, 0, false, false, false, 0);
    assert(mux.attachStream(uart));
    dashboard_stream(mux, 0, true, false, false, 0);
    renew();
    readback();
  }
  void step() {
    mux.pollStream();
    Fixture::step();
    mux.pollStream();
  }
  void hardwareUART(const std::vector<uint8_t>& bytes, bool run = true) {
    uart.feed(encode(6, bytes));
    if (run) step();
  }
  void renew(bool owner = false) {
    hardwareUART({0x20, 1, static_cast<uint8_t>(owner)});
    const auto frames = uart.take();
    assert(frames.size() == 1 && frames[0].size() == 14 &&
           frames[0][1] == 0xa0 && frames[0][3] == 0);
    const uint32_t next = queued_tx::get32(frames[0].data() + 4);
    assert(next && next != generation);
    generation = next;
    dashboard_stream(mux, generation, true, true, false, mux.streamOutputOverflows());
  }
  std::vector<uint8_t> readback() {
    hardwareUART({0x22, 1, 0});
    const auto frames = uart.take();
    assert(frames.size() == 1 && frames[0].size() == 26 &&
           frames[0][1] == 0xa2 && frames[0][3] == 0);
    return {frames[0].begin() + 8, frames[0].end()};
  }
  void jobUART(uint32_t id, uint8_t priority, uint32_t delay, uint8_t marker,
               bool run = true) {
    std::vector<uint8_t> bytes(20);
    bytes[0] = 0x21; bytes[1] = 1;
    queued_tx::put32(bytes.data() + 2, generation);
    queued_tx::put32(bytes.data() + 6, id);
    bytes[10] = priority;
    queued_tx::put32(bytes.data() + 11, delay);
    queued_tx::put32(bytes.data() + 15, delay + 600000);
    bytes[19] = marker;
    hardwareUART(bytes, run);
  }
  std::vector<uint8_t> stats() {
    hardwareUART({0x24, 1});
    auto frames = uart.take();
    assert(frames.size() == 1 && frames[0].size() == 41 && frames[0][1] == 0xa4);
    return frames[0];
  }
};

static void stream_join_controls_and_ownership() {
  StreamFixture f;
  TestUART other;
  assert(!f.mux.attachStream(other) && !f.mux.streamFaulted());
  f.hardwareUART({0x20, 1, 1});
  auto frames = f.uart.take();
  assert(frames.size() == 1 && frames[0][3] == queued_tx::NOT_OWNER &&
         queued_tx::get32(frames[0].data() + 4) == f.generation);
  f.hardwareUART({0x20, 2, 1});
  frames = f.uart.take();
  assert(frames.size() == 1 && frames[0][3] == queued_tx::INVALID &&
         queued_tx::get32(frames[0].data() + 4) == f.generation);
  f.hardwareUART({0x19, 1});
  assert(f.uart.take() == std::vector<std::vector<uint8_t>>({{6, 0x9a, 1}}));
  auto profile = f.readback();
  std::vector<uint8_t> probe{0x09};
  probe.insert(probe.end(), profile.begin(), profile.begin() + 10);
  const auto changes = applied.changes;
  f.hardwareUART(probe);
  assert(f.uart.take() == std::vector<std::vector<uint8_t>>({{6, 0xf0}}));
  assert(applied.changes == changes);
  f.hardwareUART({0x0f, 42});
  frames = f.uart.take();
  assert(frames.size() == 1 && frames[0][1] == 0x8f &&
         queued_tx::get32(frames[0].data() + 2) == 100);
  assert(receive(f.peers[0]).empty() && receive(f.peers[1]).empty());

  close(f.peers[0]); f.peers[0] = -1; f.step();
  f.renew(true);
  const auto config_generation = queued_tx::get32(f.stats().data() + 3);
  std::vector<uint8_t> config(25);
  config[0] = 0x22; config[1] = 1; config[2] = 1;
  queued_tx::put32(config.data() + 3, config_generation);
  std::copy(profile.begin(), profile.end(), config.begin() + 7);
  config[15] = 8;
  f.hardwareUART(config);
  frames = f.uart.take();
  assert(frames.size() == 1 && frames[0][1] == 0xa2 && frames[0][3] == 0 &&
         queued_tx::get32(frames[0].data() + 4) != config_generation &&
         frames[0][16] == 8 && applied.changes == changes + 1);
  f.renew(false);
  f.hardware(1, {0x20, 1, 1});
  frames = receive(f.peers[1]);
  assert(frames.size() == 1 && frames[0][3] == 0 && frames[0][13] == 1);
  // Successful renewal invalidates the old CONFIG acknowledgement.
  f.jobUART(1, 0, 0, 0xaa);
  assert(event(f.uart.take(), queued_tx::REJECTED, queued_tx::STALE));
  assert(f.radio.transmitted.empty());
  f.readback();
  f.jobUART(2, 0, 0, 0xaa);
  assert(event(f.uart.take(), queued_tx::ACCEPTED));
  assert(f.radio.transmitted.size() == 1);
}

static void stream_dashboard_without_tcp() {
  StreamFixture f;
  for (auto& fd : f.peers) {
    close(fd);
    fd = -1;
  }
  f.step();
  assert(f.mux.clientCount() == 0);
  dashboard_stream(f.mux, f.generation, true, true, false, 0);
}

static void stream_detach_fences_replacement() {
  StreamFixture f;
  f.jobUART(1, 0, 0, 0x51);
  assert(event(f.uart.take(), queued_tx::ACCEPTED));
  f.jobUART(2, 0, 60000, 0x52);
  assert(event(f.uart.take(), queued_tx::ACCEPTED));
  const auto old_generation = f.generation;
  f.mux.detachStream();
  f.mux.detachStream();
  assert(f.mux.isActuallyTransmitting() && f.mux.clientCount() == 2);
  dashboard_stream(f.mux, 0, false, false, false, 0);
  assert(f.mux.attachStream(f.uart));
  f.renew();
  f.readback();
  assert(f.generation != old_generation);
  f.jobUART(1, 0, 60000, 0x53);
  assert(event(f.uart.take(), queued_tx::ACCEPTED));
  f.finish(77);
  assert(!event(f.uart.take(), queued_tx::SUCCEEDED));
  auto stats = f.stats();
  assert(queued_tx::get32(stats.data() + 11) == 77 &&
         queued_tx::get32(stats.data() + 27) == 0);
  assert(f.radio.transmitted == std::vector<std::vector<uint8_t>>{{0x51}});
  clock_ms += 60000;
  f.step();
  assert(f.radio.transmitted == std::vector<std::vector<uint8_t>>({{0x51}, {0x53}}));
  f.finish(33);
  assert(event(f.uart.take(), queued_tx::SUCCEEDED));
  stats = f.stats();
  assert(queued_tx::get32(stats.data() + 11) == 110 &&
         queued_tx::get32(stats.data() + 27) == 33);
}

static void stream_commitment_probe_rejects_defaults() {
  StreamFixture f;
  nvs_test::reset();
  assert(f.mux.setInitialConfiguration({910525000, 62500, 7, 5, 20}));
  const auto profile = f.readback();
  std::vector<uint8_t> probe{0x09};
  probe.insert(probe.end(), profile.begin(), profile.begin() + 10);
  const auto changes = applied.changes;
  f.hardwareUART(probe);
  const auto frames = f.uart.take();
  assert(frames.size() == 1 && frames[0][1] == HW_RESP_ERROR &&
         frames[0][2] == HW_ERR_INVALID_PARAM &&
         applied.changes == changes && f.radio.transmitted.empty());
}

static void stream_renew_fences_inflight_and_clears_output() {
  StreamFixture f;
  constexpr uint8_t stream_slot = KISS_MAX_TCP_CLIENTS + KISS_LOCAL_SOURCES;
  const auto old_generation = f.generation;
  f.jobUART(1, 0, 0, 0x91);
  assert(event(f.uart.take(), queued_tx::ACCEPTED));
  f.jobUART(2, 0, 2000, 0x92);
  assert(event(f.uart.take(), queued_tx::ACCEPTED));
  assert(f.mux.sourceQueuedCount(stream_slot) == 1 &&
         f.mux.sourceTransmitting(stream_slot));
  f.uart.room = 0;
  f.hardwareUART({0x24, 1});
  f.hardwareUART({0x0f, 42}, false);
  f.hardwareUART({0x20, 1, 0});
  f.uart.room = 65536;
  f.step();
  auto frames = f.uart.take();
  assert(frames.size() == 1 && frames[0][1] == 0xa0);
  f.generation = queued_tx::get32(frames[0].data() + 4);
  assert(f.generation != old_generation && f.radio.sending);
  assert(f.mux.sourceQueuedCount(stream_slot) == 0 &&
         !f.mux.sourceTransmitting(stream_slot) && f.mux.isActuallyTransmitting());
  f.readback();
  f.jobUART(1, 0, 10000, 0x93);
  assert(event(f.uart.take(), queued_tx::ACCEPTED));
  assert(f.mux.sourceQueuedCount(stream_slot) == 1 &&
         !f.mux.sourceTransmitting(stream_slot));
  f.finish(77);
  frames = f.uart.take();
  // The new session may observe the old successful RF packet, not its result.
  assert(frames == std::vector<std::vector<uint8_t>>(
      {{0, 0x91}, {6, 0xf9, 0x80, 0x7f}}));
  auto stats = f.stats();
  assert(queued_tx::get32(stats.data() + 11) == 77 &&
         queued_tx::get32(stats.data() + 27) == 0);
  clock_ms += 3000; f.step();
  assert(f.radio.transmitted.size() == 1);
  const auto current = f.generation;
  f.generation = old_generation;
  f.jobUART(3, 0, 0, 0x94);
  frames = f.uart.take();
  assert(event(frames, queued_tx::REJECTED, queued_tx::STALE) &&
         queued_tx::get32(frames[0].data() + 3) == old_generation);
  f.generation = current;
  clock_ms += 10000; f.step(); f.finish(40);
  frames = f.uart.take();
  assert(frames.size() == 1 && event(frames, queued_tx::SUCCEEDED) &&
         queued_tx::get32(frames[0].data() + 3) == current &&
         queued_tx::get32(frames[0].data() + 7) == 1);
  assert(f.radio.transmitted == std::vector<std::vector<uint8_t>>({{0x91}, {0x93}}));
}

static void stream_bounds_partial_writes_and_fault_recovery() {
  StreamFixture f;
  const auto reads = f.uart.reads;
  f.uart.feed(std::vector<uint8_t>(513, 0x11));
  f.mux.pollStream();
  assert(f.uart.reads - reads == 512 && f.uart.available() == 1);
  f.mux.pollStream();
  f.uart.feed(encode(6, {0x17}));
  f.uart.room = 1; f.uart.short_write = 1;
  f.step();
  assert(f.uart.output.size() == 1);
  for (int i = 0; i < 100; ++i) {
    f.uart.room = 1;
    f.mux.pollStream();
  }
  auto frames = f.uart.take();
  assert(frames.size() == 1 && frames[0][1] == 0x97);
  // Exercise wrap of both ring indices while transport writes are short.
  f.uart.short_write = 3;
  for (int i = 0; i < 100; ++i) {
    f.uart.room = 65536;
    f.hardwareUART({0x24, 1});
    for (int j = 0; j < 32; ++j) f.mux.pollStream();
    frames = f.uart.take();
    assert(frames.size() == 1 && frames[0][1] == 0xa4);
  }
  f.uart.short_write = 0;
  f.hardwareUART({0x17});
  assert(f.uart.output.empty() && !f.mux.streamFaulted());
  f.uart.short_write = 65536;
  f.step();
  frames = f.uart.take();
  assert(frames.size() == 1 && frames[0][1] == 0x97);

  f.jobUART(1, 0, 60000, 0xa1);
  f.uart.take();
  const auto sample = f.stats();
  const size_t response_size = encode(sample[0], {sample.begin() + 1, sample.end()}).size();
  f.uart.room = 0;
  unsigned requests = 0;
  for (; requests < 100 && !f.mux.streamFaulted(); ++requests)
    f.hardwareUART({0x24, 1});
  assert(requests == 2060 / response_size + 1);
  assert(f.mux.streamFaulted() && f.mux.streamOutputOverflows() == 1);
  dashboard_stream(f.mux, f.generation, false, false, true, 1);
  assert(!f.mux.hasPendingTransmit() && f.mux.clientCount() == 2);
  f.hardware(1, {0x17});
  frames = receive(f.peers[1]);
  assert(frames.size() == 1 && frames[0][1] == 0x97);
  f.hardwareUART({0x20, 1, 2});
  assert(f.mux.streamFaulted());
  f.uart.room = 65536;
  f.renew();
  f.readback();
  assert(!f.mux.streamFaulted() && f.mux.streamOutputOverflows() == 1);
  assert(f.uart.output.empty());
}

static void stream_rx_reserves_control_and_faults_in_isolation() {
  StreamFixture f;
  f.jobUART(1, 0, 0, 0xd1); f.uart.take();
  f.uart.room = 0;
  const auto raw = encode(0, std::vector<uint8_t>(128, 0xa5));
  const auto metadata = encode(6, {0xf9, 8, 100});
  for (int i = 0; i < 7; ++i) {
    f.mux.write(raw.data(), raw.size());
    f.mux.write(metadata.data(), metadata.size());
  }
  f.finish(50);
  f.hardwareUART({0x24, 1});
  assert(!f.mux.streamFaulted());
  f.uart.room = 65536; f.step();
  auto frames = f.uart.take();
  assert(event(frames, queued_tx::SUCCEEDED));
  unsigned data = 0, signals = 0, stats = 0;
  for (const auto& frame : frames) {
    if (frame[0] == 0) { assert(frame.size() == 129); ++data; }
    if (frame[1] == 0xf9) {
      assert(frame == std::vector<uint8_t>({6, 0xf9, 8, 100}));
      ++signals;
    }
    if (frame[1] == 0xa4) ++stats;
  }
  assert(data == 7 && signals == 7 && stats == 1);
  receive(f.peers[0]); receive(f.peers[1]);
  f.uart.room = 0;
  for (int i = 0; i < 9; ++i) f.mux.write(raw.data(), raw.size());
  assert(f.mux.streamFaulted() && f.mux.streamOutputOverflows() == 1 &&
         f.mux.clientCount() == 2);
  assert(receive(f.peers[1]).size() == 9);
  f.uart.room = 65536; f.renew(); f.readback();
  assert(f.uart.output.empty());
}

#ifdef QUEUED_ADAPTER_TEST
struct UARTPeer : Stream {
  TestUART& uart;
  explicit UARTPeer(TestUART& stream) : uart(stream) {}
  int available() override { return uart.output.size(); }
  int read() override {
    if (uart.output.empty()) return -1;
    const int byte = uart.output.front();
    uart.output.erase(uart.output.begin());
    return byte;
  }
  size_t write(const uint8_t* bytes, size_t length) override {
    uart.input.insert(uart.input.end(), bytes, bytes + length);
    return length;
  }
};

static void stream_actual_remote_negotiation_and_split_submit() {
  StreamFixture f;
  UARTPeer peer(f.uart);
  RemoteKissRadio remote(peer);
  remote.setParams(910.525f, 62.5f, 7, 5);
  remote.setTxPower(20);
  assert(remote.setQueuedSourcePolicy(99));
  remote.begin();
  for (unsigned i = 0; i < 1000 && !remote.queuedReady(); ++i) {
    f.step(); remote.loop(); ++clock_ms;
  }
  assert(remote.queuedReady() && !remote.linkFault() && applied.changes == 1);
  assert(remote.getEstAirtimeFor(0) == 100 && remote.getEstAirtimeFor(255) == 100);
  std::vector<uint8_t> packet(255, 0xc0);
  uint32_t job;
  assert(remote.queueTransmit(packet.data(), packet.size(), 0, 0, 60000, job));
  assert(f.uart.input.size() > 512);
  // pollStream never services RF itself; the full escaped frame needs two reads.
  f.mux.pollStream();
  assert(!f.uart.input.empty() && f.radio.transmitted.empty());
  f.step(); remote.loop();
  mesh::QueuedTransmitResult result;
  assert(remote.pollQueuedResult(result) && result.state == queued_tx::ACCEPTED);
  assert(f.radio.transmitted == std::vector<std::vector<uint8_t>>{packet});
  // A late old-session submission can reuse the new session's job number.
  f.jobUART(job, 0, 0, 0x80);
  remote.loop();
  assert(!remote.pollQueuedResult(result) && remote.queuedReady());
  f.finish(77); remote.loop();
  assert(remote.pollQueuedResult(result) && result.state == queued_tx::SUCCEEDED &&
         result.job == job && result.has_rf_ms && result.rf_ms == 77);
  assert(!remote.pollQueuedResult(result));
  clock_ms += 1000; remote.loop(); f.step(); remote.loop();
  mesh::QueuedRadioStats observed;
  assert(remote.getQueuedRadioStats(observed) && observed.source_rf_ms == 77 &&
         observed.aggregate_rf_ms == 77 && observed.source_successes == 1 &&
         observed.aggregate_queued == 0 && !observed.aggregate_transmitting);
  uint8_t received[255];
  assert(remote.recvRaw(received, sizeof(received)) == 0);
}

static void stream_native_wait_has_no_age_limit() {
  StreamFixture f;
  f.radio.maximum = 100000;
  UARTPeer peer(f.uart);
  RemoteKissRadio remote(peer);
  assert(remote.setQueuedSourcePolicy(99));
  remote.begin();
  for (unsigned i = 0; i < 1000 && !remote.queuedReady(); ++i) {
    f.step(); remote.loop(); ++clock_ms;
  }
  assert(remote.queuedReady());
  const uint8_t bytes[] = {1, 0, 42};
  uint32_t job;
  assert(remote.queueTransmit(bytes, sizeof(bytes), 0, 0, 0, job));
  f.step(); remote.loop();
  mesh::QueuedTransmitResult result;
  assert(remote.pollQueuedResult(result) && result.state == queued_tx::ACCEPTED);
  for (unsigned hour = 0; hour < 24; ++hour) {
    clock_ms += 3600000;
    remote.loop(); f.step(); remote.loop();
    mesh::QueuedRadioStats stats;
    assert(remote.queuedReady() && !remote.linkFault() &&
           remote.getQueuedRadioStats(stats) && stats.aggregate_queued == 1 &&
           !stats.aggregate_transmitting && stats.source_credit_ms == 36000 &&
           stats.source_rf_ms == 0 && !remote.pollQueuedResult(result));
    assert(f.radio.transmitted.empty());
  }
  // The native maximum-packet reserve exceeds the AF99 credit cap. Policy
  // change, not client-side expiry, makes the original job eligible to send.
  assert(remote.setQueuedSourcePolicy(0));
  remote.loop(); f.step(); remote.loop(); f.step(); remote.loop();
  assert(remote.queuedReady());
  clock_ms += 14000; f.step(); remote.loop();
  assert(f.radio.transmitted == std::vector<std::vector<uint8_t>>(
      {std::vector<uint8_t>(bytes, bytes + sizeof(bytes))}));
  f.finish(77); f.step(); remote.loop();
  assert(remote.pollQueuedResult(result) && result.job == job &&
         result.state == queued_tx::SUCCEEDED && result.rf_ms == 77 &&
         result.queue_ms == 24u * 3600000u + 14000u);

  // An explicit caller deadline may precede eligibility; the MCU expires it.
  assert(remote.queueTransmit(bytes, sizeof(bytes), 0, 5000, 1000, job));
  f.step(); remote.loop();
  assert(remote.pollQueuedResult(result) && result.state == queued_tx::ACCEPTED);
  clock_ms += 1000; f.step(); remote.loop();
  assert(remote.pollQueuedResult(result) && result.job == job &&
         result.state == queued_tx::FAILED && result.reason == queued_tx::EXPIRED &&
         result.has_rf_ms && result.rf_ms == 0);
  assert(f.radio.transmitted.size() == 1 && remote.queuedReady());
}
#endif

static void stream_source_and_aggregate_credit() {
  {
    StreamFixture f;
    std::vector<uint8_t> policy(10);
    policy[0] = 0x23; policy[1] = 1;
    queued_tx::put32(policy.data() + 2, f.generation);
    queued_tx::putFloat(policy.data() + 6, 99);
    f.hardwareUART(policy);
    auto frames = f.uart.take();
    assert(frames.size() == 1 && frames[0][1] == 0xa3 && frames[0][3] == 0 &&
           queued_tx::getFloat(frames[0].data() + 4) == 99);
    f.radio.duration = 50000;
    f.jobUART(1, 0, 0, 0xa1); f.uart.take();
    f.finish(50000); f.uart.take();
    f.jobUART(2, 0, 0, 0xa2); f.uart.take();
    assert(f.radio.transmitted.size() == 1);
    f.job(1, 1, 4, 0, 0xa3);
    assert(f.radio.transmitted.size() == 2 && f.radio.transmitted.back()[0] == 0xa3);
    auto stats = f.stats();
    assert(queued_tx::get32(stats.data() + 23) == 0 &&
           queued_tx::get32(stats.data() + 7) > 100);
  }
  {
    StreamFixture f(99);
    f.radio.duration = 50000;
    f.jobUART(1, 0, 0, 0xb1); f.uart.take();
    f.finish(50000); f.uart.take();
    f.renew(); f.readback();
    auto stats = f.stats();
    assert(queued_tx::get32(stats.data() + 7) == 0 &&
           queued_tx::get32(stats.data() + 11) == 50000);
    f.jobUART(1, 0, 0, 0xb2); f.uart.take();
    f.job(1, 1, 4, 0, 0xb3);
    clock_ms += 9900; f.step();
    assert(f.radio.transmitted.size() == 1);
    clock_ms += 100; f.step();
    assert(f.radio.transmitted.size() == 2 && f.radio.transmitted.back()[0] == 0xb2);
  }
}

#if KISS_LOCAL_SOURCES > 0
struct StreamLocalSink : KissLocalSource {
  std::vector<uint8_t> packets;
  std::vector<uint32_t> jobs;
  void received(const uint8_t* p, uint16_t n, float, float, bool local) override {
    assert(n == 1 && local);
    packets.push_back(*p);
  }
  void completed(uint32_t job, uint8_t state, uint8_t reason,
                 uint32_t, uint32_t, uint32_t) override {
    assert(state == queued_tx::SUCCEEDED && reason == 0);
    jobs.push_back(job);
  }
};

static void stream_network_loss_keeps_native_and_uart() {
  StreamFixture f;
  StreamLocalSink local;
  const int slot = f.mux.attachLocal(local);
  assert(slot >= 0);
  const auto generation = f.mux.configurationGeneration();
  const auto tuning_changes = applied.changes;
  f.radio.busy = true;
  f.jobUART(1, 4, 0, 0xc1, false);
  f.job(1, 1, 2, 0, 0xc2, 600000, false);
  const uint8_t packet = 0xc3;
  assert(f.mux.submitLocal(slot, &packet, 1, 1, 0, 0, 600000));
  f.step();
  assert(f.mux.sourceQueuedCount(1) == 1);
  f.mux.disconnectTcpClients();
  assert(f.mux.clientCount() == 0 && f.mux.sourceQueuedCount(1) == 0);
  assert(f.mux.sourceQueuedCount(slot) == 1 && !f.mux.streamFaulted());
  assert(f.mux.configurationGeneration() == generation && applied.changes == tuning_changes);
  f.radio.busy = false;
  clock_ms += 121;
  f.step();
  f.finish(11);
  f.finish(13);
  assert(f.radio.transmitted == std::vector<std::vector<uint8_t>>({{0xc3}, {0xc1}}));
  assert(local.jobs == std::vector<uint32_t>{1});
  int fds[2];
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
  close(f.peers[0]);
  f.peers[0] = fds[1];
  f.server.add(WiFiClient(fds[0]));
  f.step();
  assert(f.mux.clientCount() == 1);
  f.hardware(0, {0x20, 1, 0});
  receive(f.peers[0]);
  f.hardware(0, {0x22, 1, 0});
  const auto replies = receive(f.peers[0]);
  assert(replies.size() == 1 && replies[0][3] == 0);
  assert(f.mux.configurationGeneration() == generation && applied.changes == tuning_changes);
}

static void stream_tcp_local_coexistence() {
  StreamFixture f;
  StreamLocalSink local[KISS_LOCAL_SOURCES], extra;
  int slots[KISS_LOCAL_SOURCES];
  for (int i = 0; i < KISS_LOCAL_SOURCES; ++i) {
    slots[i] = f.mux.attachLocal(local[i]);
    assert(slots[i] == KISS_MAX_TCP_CLIENTS + i);
  }
  assert(f.mux.attachLocal(extra) == -1);
  std::vector<int> peers;
  for (int i = 2; i < KISS_MAX_TCP_CLIENTS; ++i) {
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    f.server.add(WiFiClient(fds[0])); peers.push_back(fds[1]);
  }
  f.step();
  assert(f.mux.clientCount() == KISS_MAX_TCP_CLIENTS);
  f.radio.busy = true;
  f.jobUART(1, 4, 0, 0xc1, false);
  f.job(1, 1, 2, 0, 0xc2, 600000, false);
  const uint8_t packet = 0xc3;
  assert(f.mux.submitLocal(slots[0], &packet, 1, 1, 0, 0, 600000));
  f.step();
  assert(f.radio.transmitted.empty());
  constexpr uint8_t stream_slot = KISS_MAX_TCP_CLIENTS + KISS_LOCAL_SOURCES;
  assert(f.mux.sourceQueuedCount(stream_slot) == 1 &&
         f.mux.sourceQueuedCount(1) == 1 &&
         f.mux.sourceQueuedCount(slots[0]) == 1 &&
         f.mux.sourceQueuedCount(slots[1]) == 0 &&
         !f.mux.sourceTransmitting(slots[0]));
  f.radio.busy = false; clock_ms += 121; f.step();
  assert(f.mux.sourceQueuedCount(slots[0]) == 0 &&
         f.mux.sourceTransmitting(slots[0]) && !f.mux.sourceTransmitting(1));
  f.finish(11);
  assert(!f.mux.sourceTransmitting(slots[0]) && f.mux.sourceTransmitting(1) &&
         f.mux.sourceQueuedCount(1) == 0 && f.mux.sourceQueuedCount(stream_slot) == 1);
  f.finish(12);
  assert(f.mux.sourceTransmitting(stream_slot) && f.mux.sourceQueuedCount(stream_slot) == 0);
  f.finish(13);
  assert(!f.mux.sourceTransmitting(stream_slot));
  assert(f.radio.transmitted == std::vector<std::vector<uint8_t>>(
      {{0xc3}, {0xc2}, {0xc1}}));
  assert(local[0].jobs == std::vector<uint32_t>{1});
  assert(local[0].packets == std::vector<uint8_t>({0xc2, 0xc1}));
  for (int i = 1; i < KISS_LOCAL_SOURCES; ++i)
    assert(local[i].packets == std::vector<uint8_t>({0xc3, 0xc2, 0xc1}));
  auto frames = f.uart.take();
  unsigned reflected = 0, metadata = 0, terminal = 0;
  for (const auto& frame : frames) {
    if (frame[0] == 0) { assert(frame[1] != 0xc1); ++reflected; }
    if (frame[1] == 0xf9) {
      assert(frame == std::vector<uint8_t>({6, 0xf9, 0x80, 0x7f}));
      ++metadata;
    }
    if (frame[1] == 0xfa && frame[11] == queued_tx::SUCCEEDED) ++terminal;
  }
  assert(reflected == 2 && metadata == 2 && terminal == 1);
  const auto stats = f.stats();
  assert(queued_tx::get32(stats.data() + 11) == 36 &&
         queued_tx::get32(stats.data() + 27) == 13);
  for (int fd : peers) close(fd);
}
#endif
#endif

static void stream_endpoint_cases() {
#if KISS_STREAM_ENDPOINT
  stream_join_controls_and_ownership();
  stream_dashboard_without_tcp();
  stream_detach_fences_replacement();
  stream_commitment_probe_rejects_defaults();
  stream_renew_fences_inflight_and_clears_output();
  stream_bounds_partial_writes_and_fault_recovery();
  stream_rx_reserves_control_and_faults_in_isolation();
  stream_source_and_aggregate_credit();
#ifdef QUEUED_ADAPTER_TEST
  stream_actual_remote_negotiation_and_split_submit();
  stream_native_wait_has_no_age_limit();
#endif
#if KISS_LOCAL_SOURCES > 0
  stream_tcp_local_coexistence();
  stream_network_loss_keeps_native_and_uart();
#endif
  puts("UART real modem: renewal, in-flight fencing, controls, partial writes, overflow, budgets and coexistence passed");
#else
  Fixture f;
  TestUART uart;
  assert(!f.mux.attachStream(uart));
  f.mux.pollStream();
  assert(!f.mux.streamFaulted() && f.mux.streamOutputOverflows() == 0 &&
         uart.reads == 0 && uart.writes == 0);
  RadioDashboard::Snapshot snapshot;
  f.mux.dashboardStatus(snapshot.radio);
  assert(!snapshot.radio.stream.enabled);
  std::vector<char> json(RadioDashboard::JSON_CAPACITY);
  assert(RadioDashboard::formatJSON(snapshot, "test-radio", json.data(), json.size()));
  assert(!strstr(json.data(), "\"stream\":"));
  f.job(0, 1, 0, 1000, 0xe1);
  assert(f.mux.sourceQueuedCount(0) == 1 && f.mux.sourceQueuedCount(1) == 0 &&
         !f.mux.sourceTransmitting(0));
  close(f.peers[0]); f.peers[0] = -1; f.step();
  assert(f.mux.sourceQueuedCount(0) == 0 && !f.mux.sourceTransmitting(0));
  assert(f.mux.sourceQueuedCount(UINT8_MAX) == 0 &&
         !f.mux.sourceTransmitting(UINT8_MAX));
#endif
}
