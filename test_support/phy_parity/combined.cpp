#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>
#include <nvs.h>
#include "WifiKissMultiplexer.h"
#ifdef QUEUED_ADAPTER_TEST
#include "RemoteKissRadio.h"
#endif

static uint32_t clock_ms;
unsigned long millis() { return clock_ms; }
void delay(unsigned long n) { clock_ms += n; }

struct Radio : mesh::Radio {
  bool busy = false, sending = false, complete = false, start_ok = true;
  bool cad = false;
  int threshold = 0;
  uint32_t duration = 100, maximum = 100, carrier_delay = 0;
  std::vector<std::vector<uint8_t>> transmitted;
  int recvRaw(uint8_t*, int) override { return 0; }
  uint32_t getEstAirtimeFor(int n) override { return n == 255 ? maximum : duration; }
  float packetScore(float, int) override { return 0; }
  bool startSendRaw(const uint8_t* p, int n) override {
    if (!start_ok) return false;
    assert(!sending);
    transmitted.emplace_back(p, p + n);
    sending = true; complete = false;
    return true;
  }
  bool isSendComplete() override { return complete; }
  void onSendFinished() override { sending = false; }
  bool isInRecvMode() const override { return !sending; }
  bool isReceiving() override { clock_ms += carrier_delay; return busy; }
  void setCADEnabled(bool enabled) override { cad = enabled; }
  void triggerNoiseFloorCalibrate(int value) override { threshold = value; }
};
struct Board : mesh::MainBoard {
  uint16_t getBattMilliVolts() override { return 4000; }
  const char* getManufacturerName() const override { return "clocked-test"; }
  void reboot() override { assert(false); }
  uint8_t getStartupReason() const override { return 0; }
};
struct RNG : mesh::RNG {
  uint8_t value = 0;
  void random(uint8_t* p, size_t n) override { memset(p, value, n); }
};
static struct {
  float frequency, bandwidth;
  uint8_t sf, cr, power;
  unsigned changes, power_changes;
} applied;
static void configure(float frequency, float bandwidth, uint8_t sf, uint8_t cr) {
  applied.frequency = frequency; applied.bandwidth = bandwidth;
  applied.sf = sf; applied.cr = cr; ++applied.changes;
}
static void power(uint8_t power) { applied.power = power; ++applied.power_changes; }

static std::vector<uint8_t> encode(uint8_t command, const std::vector<uint8_t>& data) {
  std::vector<uint8_t> wire{0xc0, command};
  for (uint8_t b : data) {
    if (b == 0xc0) { wire.push_back(0xdb); wire.push_back(0xdc); }
    else if (b == 0xdb) { wire.push_back(0xdb); wire.push_back(0xdd); }
    else wire.push_back(b);
  }
  wire.push_back(0xc0);
  return wire;
}
static std::vector<std::vector<uint8_t>> receive(int fd) {
  uint8_t data[8192];
  std::vector<uint8_t> bytes;
  for (;;) {
    auto n = recv(fd, data, sizeof(data), MSG_DONTWAIT);
    if (n <= 0) break;
    bytes.insert(bytes.end(), data, data + n);
  }
  std::vector<std::vector<uint8_t>> frames;
  std::vector<uint8_t> frame;
  bool escaped = false;
  for (uint8_t b : bytes) {
    if (b == 0xc0) {
      if (!frame.empty()) frames.push_back(frame);
      frame.clear(); escaped = false;
    } else if (escaped) {
      assert(b == 0xdc || b == 0xdd);
      frame.push_back(b == 0xdc ? 0xc0 : 0xdb); escaped = false;
    } else if (b == 0xdb) escaped = true;
    else frame.push_back(b);
  }
  assert(frame.empty());
  return frames;
}

struct Fixture {
  Radio radio;
  Board board;
  RNG rng;
  mesh::LocalIdentity identity;
  SensorManager sensors;
  WifiKissMultiplexer mux;
  KissModem modem{mux, identity, rng, radio, board, sensors};
  WiFiServer server;
  int peers[2]{};
  uint32_t generations[2]{};
  explicit Fixture(float aggregate_factor = 1.0f, bool boot_from_store = false,
                   bool require_operator_phy = false) {
    clock_ms = 100;
    applied = {};
    if (!boot_from_store) nvs_test::reset();
    mux.attachRadio(radio, rng, configure, power);
    if (boot_from_store) {
      assert(mux.setInitialConfiguration({910525000, 62500, 7, 5, 20},
                                         require_operator_phy));
    }
    modem.begin();
    for (int i = 0; i < 2; ++i) {
      int fds[2]; assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
      server.add(WiFiClient(fds[0])); peers[i] = fds[1];
    }
    step();
    for (int i = 0; i < 2; ++i) {
      hardware(i, {0x20, 1, static_cast<uint8_t>(i == 0)});
      auto replies = receive(peers[i]);
      assert(replies.size() == 1 && replies[0][1] == 0xa0 && replies[0][3] == 0);
      generations[i] = queued_tx::get32(replies[0].data() + 4);
    }
    std::vector<uint8_t> config(25);
    config[0] = 0x22; config[1] = 1; config[2] = 1;
    queued_tx::put32(config.data() + 7, 910525000);
    queued_tx::put32(config.data() + 11, 62500);
    config[15] = 7; config[16] = 5; config[17] = 20;
    queued_tx::putFloat(config.data() + 18, aggregate_factor);
    hardware(0, boot_from_store ? std::vector<uint8_t>{0x22, 1, 0} : config);
    auto replies = receive(peers[0]);
    assert(replies.size() == 1 && replies[0][1] == 0xa2 && replies[0][3] == 0);
    hardware(1, {0x22, 1, 0});
    replies = receive(peers[1]);
    assert(replies.size() == 1 && replies[0][1] == 0xa2 && replies[0][3] == 0);
  }
  ~Fixture() { for (int fd : peers) close(fd); }
  void step() { mux.poll(server); modem.loop(); mux.afterModemLoop(); }
  void send(int i, uint8_t command, const std::vector<uint8_t>& data, bool run = true) {
    auto wire = encode(command, data);
    assert(::send(peers[i], wire.data(), wire.size(), 0) == static_cast<ssize_t>(wire.size()));
    if (run) step();
  }
  void hardware(int i, const std::vector<uint8_t>& data, bool run = true) { send(i, 6, data, run); }
  void job(int i, uint32_t id, uint8_t priority, uint32_t wait, uint8_t marker,
           uint32_t expiry = 0, bool run = true) {
    std::vector<uint8_t> data(20);
    data[0] = 0x21; data[1] = 1;
    queued_tx::put32(data.data() + 2, generations[i]);
    queued_tx::put32(data.data() + 6, id);
    data[10] = priority;
    queued_tx::put32(data.data() + 11, wait);
    queued_tx::put32(data.data() + 15, expiry);
    data[19] = marker;
    hardware(i, data, run);
  }
  void finish(uint32_t duration = 100) { clock_ms += duration; radio.complete = true; step(); }
};

#ifdef QUEUED_ADAPTER_TEST
struct SocketStream : Stream {
  int fd;
  explicit SocketStream(int descriptor) : fd(descriptor) {}
  int available() override {
    uint8_t byte;
    return ::recv(fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT) > 0 ? 1 : 0;
  }
  int read() override {
    uint8_t byte;
    return ::recv(fd, &byte, 1, MSG_DONTWAIT) == 1 ? byte : -1;
  }
  size_t write(uint8_t byte) override { return write(&byte, 1); }
  size_t write(const uint8_t* data, size_t length) override {
    const auto n = ::send(fd, data, length, MSG_DONTWAIT);
    return n < 0 ? 0 : n;
  }
};

static void actual_queued_adapter() {
  Fixture f;
  SocketStream stream(f.peers[1]);
  RemoteKissRadio remote(stream);
  remote.setParams(910.525f, 62.5f, 7, 5);
  remote.setTxPower(20);
  remote.setQueuedSourcePolicy(99);
  remote.begin();
  for (unsigned i = 0; i < 1000 && !remote.queuedReady(); ++i) {
    f.step(); remote.loop(); ++clock_ms;
  }
  assert(remote.queuedReady() && !remote.lastError());
  assert(remote.getEstAirtimeFor(0) == 100 && remote.getEstAirtimeFor(255) == 100);
  mesh::QueuedRadioStats observed;
  assert(remote.getQueuedRadioStats(observed) &&
         observed.aggregate_credit_ms == 1800000 && observed.source_credit_ms == 36000 &&
         observed.aggregate_rf_ms == 0 && observed.source_rf_ms == 0 &&
         observed.aggregate_queued == 0 && !observed.aggregate_transmitting);
  assert(applied.changes == 1 && applied.power_changes == 1);
  f.job(0, 1, 4, 60000, 0xa1);
  const uint8_t bytes[] = {1, 0, 0xc0, 0xdb};
  uint32_t id;
  assert(remote.queueTransmit(bytes, sizeof(bytes), 0, 0, 5000, id));
  f.step(); remote.loop();
  mesh::QueuedTransmitResult result;
  assert(remote.pollQueuedResult(result) && result.state == queued_tx::ACCEPTED);
  assert(f.radio.transmitted.size() == 1 && f.radio.transmitted[0] ==
         std::vector<uint8_t>(bytes, bytes + sizeof(bytes)));
  f.finish(77); remote.loop();
  assert(remote.pollQueuedResult(result) && result.state == queued_tx::SUCCEEDED &&
         result.job == id && result.has_rf_ms && result.rf_ms == 77);
  clock_ms += 1000; remote.loop(); f.step(); remote.loop();
  assert(remote.getQueuedRadioStats(observed) &&
         observed.source_rf_ms == 77 && observed.aggregate_rf_ms == 77 &&
         observed.source_successes == 1 && observed.aggregate_successes == 1 &&
         observed.source_failures == 0 && observed.aggregate_failures == 0 &&
         observed.source_credit_ms < 36000 &&
         observed.aggregate_queued == 1 && !observed.aggregate_transmitting);
  uint8_t received[255];
  assert(remote.recvRaw(received, sizeof(received)) == 0);
  clock_ms += 60000; f.step(); remote.loop(); f.step(); remote.loop();
  assert(remote.getQueuedRadioStats(observed) && observed.aggregate_transmitting &&
         observed.aggregate_queued == 0 && observed.source_rf_ms == 77 &&
         observed.aggregate_rf_ms == 77);
  f.finish(80); remote.loop();
  assert(remote.recvRaw(received, sizeof(received)) == 1 && received[0] == 0xa1);
  assert(remote.lastReceiveWasLocal() && std::isnan(remote.getLastRSSI()));
  assert(remote.getPacketsRecv() == 0 && applied.changes == 1);
  clock_ms += 1000; remote.loop(); f.step(); remote.loop();
  assert(remote.getQueuedRadioStats(observed) && !observed.aggregate_transmitting &&
         observed.aggregate_rf_ms == 157 && observed.source_rf_ms == 77 &&
         observed.aggregate_successes == 2 && observed.source_successes == 1);
}

static void queued_adapter_requires_committed_profile() {
  Fixture f;
  nvs_test::reset();
  assert(f.mux.setInitialConfiguration({910525000, 62500, 7, 5, 20}));
  const auto initial_changes = applied.changes;
  SocketStream stream(f.peers[1]);
  RemoteKissRadio remote(stream);
  remote.begin();
  for (unsigned i = 0; i < 1000 && !remote.linkFault(); ++i) {
    f.step(); remote.loop(); ++clock_ms;
  }
  assert(remote.linkFault() && !remote.queuedReady());
  assert(applied.changes == initial_changes && f.radio.transmitted.empty());
}
#endif

static bool event(const std::vector<std::vector<uint8_t>>& frames, uint8_t state, uint8_t reason = 0) {
  for (const auto& f : frames)
    if (f.size() == 25 && f[1] == 0xfa && f[11] == state && f[12] == reason) return true;
  return false;
}

#include "stream_cases.h"

static void priority_and_controls() {
  Fixture f;
  f.radio.busy = true;
  f.job(0, 1, 3, 0, 0xa1);
  f.job(1, 1, 0, 0, 0xa2);
  f.hardware(0, {0x17});
  auto frames = receive(f.peers[0]);
  bool pong = false;
  for (auto& frame : frames) if (frame[1] == 0x97) pong = true;
  assert(pong && f.radio.transmitted.empty());
  f.radio.busy = false; clock_ms += 200; f.step();
  assert(f.radio.transmitted == std::vector<std::vector<uint8_t>>{{0xa2}});
  receive(f.peers[0]); receive(f.peers[1]);
  f.finish();
  frames = receive(f.peers[0]);
  assert(frames.size() == 2 && frames[0] == std::vector<uint8_t>({0, 0xa2}) &&
         frames[1] == std::vector<uint8_t>({6, 0xf9, 0x80, 0x7f}));
  assert(event(receive(f.peers[1]), 2));
  assert(f.radio.transmitted.size() == 2 && f.radio.transmitted[1][0] == 0xa1);
}

static void future_fifo_and_expiry() {
  Fixture f;
  f.job(0, 1, 0, 1000, 0xb1);
  f.job(1, 1, 3, 0, 0xb2);
  assert(f.radio.transmitted[0][0] == 0xb2);
  f.job(0, 2, 3, 1000, 0xb3, 500);
  receive(f.peers[0]); receive(f.peers[1]);
  f.finish();
  clock_ms += 500; f.step();
  assert(event(receive(f.peers[0]), 3, 6));
  clock_ms += 500; f.step();
  assert(f.radio.transmitted.size() == 2 && f.radio.transmitted[1][0] == 0xb1);
}

static void measured_budget_unknown_and_legacy() {
  Fixture f;
  f.radio.duration = 1000000;
  f.send(0, 0, {0xcc});
  assert(f.radio.transmitted.size() == 1);
  clock_ms += 1500000;
  f.step();
  auto frames = receive(f.peers[0]);
  assert(frames == std::vector<std::vector<uint8_t>>({{6, 0xf8, 0}}));
  assert(receive(f.peers[1]).empty());
  f.hardware(1, {0x24, 1});
  frames = receive(f.peers[1]);
  assert(frames.size() == 1 && queued_tx::get32(frames[0].data() + 11) == 1500000);
  assert(queued_tx::get32(frames[0].data() + 7) == 300000);
  f.radio.maximum = 1000000;
  f.job(1, 1, 0, 0, 0xdd);
  clock_ms += 31000;
  f.hardware(1, {0x17});
  frames = receive(f.peers[1]);
  bool pong = false;
  for (auto& frame : frames) if (frame[1] == 0x97) pong = true;
  assert(pong && f.radio.transmitted.size() == 1);
}

static void full_queue_reserves_controls_and_fifo() {
  Fixture f;
  f.radio.busy = true;
  for (uint32_t id = 1; id <= KISS_REQUEST_QUEUE_DEPTH; ++id)
    f.job(0, id, 2, 0, static_cast<uint8_t>(id));
  f.job(1, 1, 0, 0, 0xff);
  assert(event(receive(f.peers[1]), 0, 2));
  f.hardware(1, {0x17});
  assert(receive(f.peers[1]) == std::vector<std::vector<uint8_t>>({{6, 0x97}}));
  f.radio.busy = false;
  clock_ms += 200;
  f.step();
  assert(f.radio.transmitted[0][0] == 1);
  f.finish();
  assert(f.radio.transmitted[1][0] == 2);
}

static void configuration_is_owned_and_pending_jobs_are_stable() {
  Fixture f;
  f.job(0, 1, 3, 1000, 0xa0);
  receive(f.peers[0]);
  f.hardware(1, {0x22, 1, 0});
  auto frames = receive(f.peers[1]);
  assert(frames.size() == 1 && frames[0][1] == 0xa2);
  std::vector<uint8_t> change{0x22, 1, 1};
  change.insert(change.end(), frames[0].begin() + 4, frames[0].end());
  change[17] = 21;
  f.hardware(1, change);
  frames = receive(f.peers[1]);
  assert(frames.size() == 1 && frames[0][3] == 4);
  f.hardware(0, change);
  frames = receive(f.peers[0]);
  assert(frames.size() == 1 && frames[0][3] == 5);
  assert(frames[0][18] == 20);
  f.hardware(1, {0x09, 1, 2, 3});
  assert(receive(f.peers[1]) == std::vector<std::vector<uint8_t>>({{6, 0xf1, 2}}));
}

static void read_only_capacity_does_not_claim_ownership() {
  Fixture f;
  f.hardware(1, {queued_tx::CAPACITY, queued_tx::VERSION});
  assert(receive(f.peers[1]) == std::vector<std::vector<uint8_t>>(
      {{6, HW_RESP(queued_tx::CAPACITY), queued_tx::VERSION,
        KISS_MAX_TCP_CLIENTS, KISS_LOCAL_SOURCES}}));
  f.hardware(1, {queued_tx::CAPACITY, 2});
  assert(receive(f.peers[1]) == std::vector<std::vector<uint8_t>>(
      {{6, 0xf1, 2}}));
  f.hardware(0, {queued_tx::CAPACITY, queued_tx::VERSION, 0});
  assert(receive(f.peers[0]) == std::vector<std::vector<uint8_t>>(
      {{6, 0xf1, 2}}));
  f.hardware(1, {queued_tx::CONFIG, queued_tx::VERSION, 0});
  assert(receive(f.peers[1])[0][3] == queued_tx::NONE);
}

static void reconnect_preserves_aggregate_and_old_tx_ownership() {
  Fixture f;
  f.job(0, 1, 0, 0, 0xab);
  receive(f.peers[0]);
  close(f.peers[0]);
  f.peers[0] = -1;
  f.step();
  int fds[2]; assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
  f.server.add(WiFiClient(fds[0]));
  f.peers[0] = fds[1];
  f.step();
  f.hardware(0, {0x20, 1, 1});
  auto frames = receive(f.peers[0]);
  assert(frames.size() == 1 && queued_tx::get32(frames[0].data() + 4) != f.generations[0]);
  f.finish();
  frames = receive(f.peers[0]);
  assert(frames.size() == 2 && frames[0][0] == 0 && frames[1][1] == 0xf9);
  f.step();
  assert(receive(f.peers[0]).empty());
  f.hardware(0, {0x24, 1});
  frames = receive(f.peers[0]);
  assert(queued_tx::get32(frames[0].data() + 7) == 1799900);
  assert(queued_tx::get32(frames[0].data() + 11) == 100);
  assert(queued_tx::get32(frames[0].data() + 27) == 0);
}

static void source_constraints_expiry_and_wrap() {
  Fixture f;
  std::vector<uint8_t> policy(10);
  policy[0] = 0x23; policy[1] = 1;
  queued_tx::put32(policy.data() + 2, f.generations[0]);
  queued_tx::putFloat(policy.data() + 6, 9.0f);
  f.hardware(0, policy);
  receive(f.peers[0]);
  f.radio.maximum = 1000000;
  f.job(0, 1, 0, 0, 0xa1, 1000);
  f.job(1, 1, 4, 0, 0xa2);
  assert(f.radio.transmitted.size() == 1 && f.radio.transmitted[0][0] == 0xa2);
  f.finish();
  clock_ms += 1000; f.step();
  assert(event(receive(f.peers[0]), 3, 6));
  f.radio.maximum = 100;
  clock_ms = 0xfffffff0;
  f.job(1, 2, 0, 32, 0xa3);
  f.step();
  assert(f.radio.transmitted.size() == 1);
  clock_ms = 0x10;
  f.step();
  assert(f.radio.transmitted.size() == 2 && f.radio.transmitted[1][0] == 0xa3);
}

static void native_carrier_retry_and_start_failure() {
  Fixture f;
  f.radio.busy = true;
  f.job(0, 1, 0, 0, 0xa1);
  clock_ms += 4000;
  f.step();
  assert(f.radio.transmitted.empty());
  clock_ms += 200;
  f.step();
  assert(f.radio.transmitted.size() == 1);
  f.finish();
  receive(f.peers[0]); receive(f.peers[1]);
  f.radio.busy = false;
  f.radio.start_ok = false;
  f.job(0, 2, 0, 0, 0xa2);
  assert(event(receive(f.peers[0]), 3, 7));
  assert(receive(f.peers[1]).empty());
}

static void carrier_scan_is_not_rf_airtime() {
  Fixture f;
  f.radio.carrier_delay = 200;
  f.job(0, 1, 0, 0, 0xa1);
  assert(event(receive(f.peers[0]), queued_tx::ACCEPTED));
  f.step();
  assert(f.radio.sending);
  assert(receive(f.peers[0]).empty());
  f.finish(100);
  auto frames = receive(f.peers[0]);
  assert(frames.size() == 1 && event(frames, queued_tx::SUCCEEDED));
  assert(queued_tx::get32(frames[0].data() + 13) == 200);
  assert(queued_tx::get32(frames[0].data() + 17) == 100);
  f.hardware(0, {0x24, 1});
  frames = receive(f.peers[0]);
  assert(queued_tx::get32(frames[0].data() + 11) == 100);
  receive(f.peers[1]);

  f.job(0, 2, 0, 0, 0xa2, 100);
  assert(event(receive(f.peers[0]), queued_tx::FAILED, queued_tx::EXPIRED));
  assert(f.radio.transmitted.size() == 1);
  assert(receive(f.peers[1]).empty());
}

static void blocking_carrier_reselects_and_preserves_retry() {
  {
    Fixture f;
    f.radio.carrier_delay = 200;
    f.job(0, 1, 3, 0, 0xa1, 0, false);
    f.job(0, 2, 0, 100, 0xa2, 0, false);
    f.step();
    assert(f.radio.transmitted.size() == 1 && f.radio.transmitted[0][0] == 0xa2);
  }
  {
    Fixture f;
    f.radio.carrier_delay = 50;
    f.radio.busy = true;
    f.job(0, 1, 0, 0, 0xa1);
    assert(clock_ms == 150);
    clock_ms += 120;
    f.step();
    assert(clock_ms == 270 && f.radio.transmitted.empty());
    ++clock_ms;
    f.step();
    assert(clock_ms == 321 && f.radio.transmitted.empty());
    f.radio.busy = false;
    clock_ms += 120;
    f.step();
    assert(clock_ms == 441 && f.radio.transmitted.empty());
    ++clock_ms;
    f.step();
    assert(clock_ms == 492 && f.radio.transmitted.size() == 1);
  }
}

static void mesh_carrier_retry_is_randomized() {
  const uint32_t retry_ms[] = {120, 240, 360};
  for (uint8_t i = 0; i < 3; ++i) {
    Fixture f;
    f.rng.value = i;
    f.radio.busy = true;
    f.job(0, 1, 0, 0, 0xa1);
    f.radio.busy = false;
    clock_ms += retry_ms[i];
    f.step();
    assert(f.radio.transmitted.empty());
    ++clock_ms;
    f.step();
    assert(f.radio.transmitted.size() == 1);
  }
}

static void changed_profile_requires_fresh_readback() {
  Fixture f;
  f.hardware(0, {0x22, 1, 0});
  auto frames = receive(f.peers[0]);
  std::vector<uint8_t> change{0x22, 1, 1};
  change.insert(change.end(), frames[0].begin() + 4, frames[0].end());
  change[17] = 21;
  f.hardware(0, change);
  frames = receive(f.peers[0]);
  assert(frames[0][3] == 0 && queued_tx::get32(frames[0].data() + 4) == 2);
  f.job(1, 1, 0, 0, 0xaa);
  assert(event(receive(f.peers[1]), 0, 3));
  assert(f.radio.transmitted.empty());
  f.hardware(1, {0x22, 1, 0});
  receive(f.peers[1]);
  f.job(1, 2, 0, 0, 0xbb);
  assert(f.radio.transmitted == std::vector<std::vector<uint8_t>>{{0xbb}});
}

static void physical_service_does_not_require_host_owner() {
  nvs_test::reset();
  Radio radio; Board board; RNG rng; mesh::LocalIdentity identity; SensorManager sensors;
  WifiKissMultiplexer mux; KissModem modem(mux, identity, rng, radio, board, sensors);
  mux.attachRadio(radio, rng, configure, power);
  RadioConfig startup{910525000, 62500, 7, 5, 20};
  assert(mux.setInitialConfiguration(startup));
  modem.begin();
  WiFiServer server;
  int fds[2]; assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
  server.add(WiFiClient(fds[0])); mux.poll(server);
  auto wire = encode(0, {0xaa});
  assert(send(fds[1], wire.data(), wire.size(), 0) == static_cast<ssize_t>(wire.size()));
  mux.poll(server); modem.loop(); mux.afterModemLoop();
  assert(radio.transmitted == std::vector<std::vector<uint8_t>>{{0xaa}});
  clock_ms += 100; radio.complete = true; mux.afterModemLoop();
  assert(receive(fds[1]) == std::vector<std::vector<uint8_t>>({{6, 0xf8, 1}}));
  close(fds[1]);
}

static void native_source_float_is_not_quantized() {
  Fixture f;
  const float factor = 2.0004f;
  uint32_t bits;
  memcpy(&bits, &factor, sizeof(bits));
  std::vector<uint8_t> policy(10);
  policy[0] = 0x23; policy[1] = 1;
  queued_tx::put32(policy.data() + 2, f.generations[0]);
  queued_tx::put32(policy.data() + 6, bits);
  f.hardware(0, policy);
  auto frames = receive(f.peers[0]);
  assert(frames.size() == 1 && frames[0].size() == 8 &&
         frames[0][3] == 0 && queued_tx::get32(frames[0].data() + 4) == bits);
  f.hardware(0, {0x24, 1});
  frames = receive(f.peers[0]);
  const uint32_t credit = queued_tx::get32(frames[0].data() + 23);
  assert(credit < 1200000 && credit > 1199500);
  queued_tx::put32(policy.data() + 6, 0x7fc00000);
  f.hardware(0, policy);
  frames = receive(f.peers[0]);
  assert(frames[0][3] == 1 && queued_tx::get32(frames[0].data() + 4) == bits);
  const float one_percent = 99.0f;
  memcpy(&bits, &one_percent, sizeof(bits));
  queued_tx::put32(policy.data() + 6, bits);
  f.hardware(0, policy);
  frames = receive(f.peers[0]);
  assert(frames[0][3] == 0 && queued_tx::get32(frames[0].data() + 4) == bits);
  f.hardware(0, {0x24, 1});
  frames = receive(f.peers[0]);
  assert(queued_tx::get32(frames[0].data() + 23) == 36000);
}

static void aggregate_runtime_factor_and_zero() {
  {
    Fixture f(2.0004f);
    f.hardware(0, {0x22, 1, 0});
    auto frames = receive(f.peers[0]);
    assert(frames.size() == 1 && frames[0].size() == 26);
    assert(queued_tx::getFloat(frames[0].data() + 19) == 2.0004f);
    std::vector<uint8_t> config(25);
    config[0] = 0x22; config[1] = 1; config[2] = 1;
    queued_tx::put32(config.data() + 3, queued_tx::get32(frames[0].data() + 4));
    memcpy(config.data() + 7, frames[0].data() + 8, queued_tx::PROFILE_SIZE);
    for (uint32_t invalid : {0x7fc00000u, 0x7f800000u, 0xff800000u, 0xbf800000u}) {
      queued_tx::put32(config.data() + 18, invalid);
      f.hardware(0, config);
      frames = receive(f.peers[0]);
      assert(frames.size() == 1 && frames[0][3] == queued_tx::INVALID);
      assert(queued_tx::getFloat(frames[0].data() + 19) == 2.0004f);
    }
  }
  {
    Fixture f(99.0f);
    f.hardware(0, {0x24, 1});
    auto frames = receive(f.peers[0]);
    assert(queued_tx::get32(frames[0].data() + 7) == 36000);
  }
  {
    Fixture f(99.25f);
    f.hardware(0, {0x24, 1});
    auto frames = receive(f.peers[0]);
    assert(queued_tx::get32(frames[0].data() + 7) == 35910);
  }
  {
    Fixture f(0.0f);
    std::vector<uint8_t> policy(10);
    policy[0] = 0x23; policy[1] = 1;
    queued_tx::put32(policy.data() + 2, f.generations[0]);
    queued_tx::putFloat(policy.data() + 6, 0.0f);
    f.hardware(0, policy);
    auto frames = receive(f.peers[0]);
    assert(frames[0][3] == 0 && queued_tx::getFloat(frames[0].data() + 4) == 0);
    clock_ms += queued_tx::WINDOW_MS;
    f.hardware(0, {0x24, 1});
    frames = receive(f.peers[0]);
    assert(queued_tx::get32(frames[0].data() + 7) == 3600000);
    assert(queued_tx::get32(frames[0].data() + 23) == 3600000);
  }
}

static std::vector<uint8_t> readConfigForUpdate(Fixture& f) {
  f.hardware(0, {queued_tx::CONFIG, queued_tx::VERSION, 0});
  const auto frames = receive(f.peers[0]);
  assert(frames.size() == 1 && frames[0].size() == 26 && frames[0][3] == 0);
  std::vector<uint8_t> request(25);
  request[0] = queued_tx::CONFIG; request[1] = queued_tx::VERSION; request[2] = 1;
  memcpy(request.data() + 3, frames[0].data() + 4, 4);
  memcpy(request.data() + 7, frames[0].data() + 8, queued_tx::PROFILE_SIZE);
  return request;
}

static void persistent_operator_profile_survives_reboot() {
  nvs_test::reset();
  {
    Fixture f(1, true);
    assert(nvs_test::store.writes == 0);
    auto request = readConfigForUpdate(f);
    f.hardware(0, request);
    auto frames = receive(f.peers[0]);
    assert(frames[0][3] == 0 && nvs_test::store.commits == 1);

    queued_tx::put32(request.data() + 3, queued_tx::get32(frames[0].data() + 4));
    queued_tx::put32(request.data() + 7, 912525000);
    queued_tx::put32(request.data() + 11, 250000);
    request[17] = 2;
    queued_tx::putFloat(request.data() + 18, 99.25f);
    request[22] = 1;
    queued_tx::put16(request.data() + 23, 15);
    f.hardware(0, request);
    frames = receive(f.peers[0]);
    assert(frames[0][3] == 0 && nvs_test::store.commits == 2);
    assert(std::abs(applied.frequency - 912.525f) < 0.0001f && applied.bandwidth == 250 &&
           applied.sf == 7 && applied.cr == 5 && applied.power == 2);

    queued_tx::put32(request.data() + 3, queued_tx::get32(frames[0].data() + 4));
    f.hardware(0, request);
    frames = receive(f.peers[0]);
    assert(frames[0][3] == 0 && nvs_test::store.writes == 2);
    std::vector<uint8_t> policy(10);
    policy[0] = queued_tx::SOURCE_POLICY; policy[1] = queued_tx::VERSION;
    queued_tx::put32(policy.data() + 2, f.generations[0]);
    queued_tx::putFloat(policy.data() + 6, 0);
    f.hardware(0, policy);
    assert(receive(f.peers[0])[0][3] == 0);
    assert(nvs_test::store.writes == 2);
  }
  {
    Fixture rebooted(1, true);
    const auto restored = readConfigForUpdate(rebooted);
    assert(queued_tx::get32(restored.data() + 7) == 912525000);
    assert(queued_tx::get32(restored.data() + 11) == 250000);
    assert(restored[17] == 2 && queued_tx::getFloat(restored.data() + 18) == 99.25f);
    assert(applied.changes == 1 && std::abs(applied.frequency - 912.525f) < 0.0001f &&
           applied.bandwidth == 250 && applied.power == 2);
    assert(rebooted.radio.cad && rebooted.radio.threshold == 15);
    assert(nvs_test::store.writes == 2);
    rebooted.hardware(0, {queued_tx::STATS, queued_tx::VERSION});
    const auto frames = receive(rebooted.peers[0]);
    assert(queued_tx::get32(frames[0].data() + 7) == 35910);
    assert(queued_tx::get32(frames[0].data() + 23) == 1800000);
    rebooted.job(0, 1, 0, 0, 0xaa);
    assert(rebooted.radio.transmitted.size() == 1);
    receive(rebooted.peers[0]);
    rebooted.finish();
    assert(event(receive(rebooted.peers[0]), queued_tx::SUCCEEDED));
  }
}

static void profile_commit_failure_blocks_tx_until_explicit_recovery() {
  for (int failure = 0; failure < 4; ++failure) {
    Fixture f;
    auto request = readConfigForUpdate(f);
    std::vector<uint8_t> unchanged_radio{HW_CMD_SET_RADIO};
    unchanged_radio.insert(unchanged_radio.end(), request.begin() + 7, request.begin() + 17);
    const std::vector<uint8_t> unchanged_power{HW_CMD_SET_TX_POWER, request[17]};
    queued_tx::put32(request.data() + 7, 912525000);
    nvs_test::store.fail_open = failure == 0;
    nvs_test::store.fail_write = failure == 1;
    nvs_test::store.fail_commit = failure >= 2;
    nvs_test::store.commit_despite_failure = failure == 3;
    const auto changes = applied.changes;
    f.hardware(0, request);
    auto frames = receive(f.peers[0]);
    assert(frames[0][3] == queued_tx::NOT_CONFIGURED);
    assert(queued_tx::get32(frames[0].data() + 8) == 910525000);
    assert(applied.changes == changes);
    f.hardware(0, {queued_tx::CONFIG, queued_tx::VERSION, 0});
    assert(receive(f.peers[0])[0][3] == queued_tx::NOT_CONFIGURED);
    for (const auto& unchanged : {unchanged_radio, unchanged_power}) {
      f.hardware(1, unchanged);
      assert(receive(f.peers[1]) == std::vector<std::vector<uint8_t>>({{6, 0xf1, 2}}));
    }
    f.job(0, 1, 0, 0, 0xaa);
    assert(event(receive(f.peers[0]), queued_tx::REJECTED, queued_tx::NOT_CONFIGURED));
    f.send(1, 0, {0xbb});
    assert(receive(f.peers[1]) == std::vector<std::vector<uint8_t>>({{6, 0xf8, 0}}));
    assert(f.radio.transmitted.empty());

    nvs_test::store.fail_open = nvs_test::store.fail_write = false;
    nvs_test::store.fail_commit = false;
    f.hardware(0, request);
    assert(receive(f.peers[0])[0][3] == 0);
    assert(std::abs(applied.frequency - 912.525f) < 0.0001f);
    f.job(0, 2, 0, 0, 0xcc);
    assert(f.radio.transmitted.size() == 1);
    receive(f.peers[0]);
    f.finish();
    assert(event(receive(f.peers[0]), queued_tx::SUCCEEDED));
  }
}

static void invalid_saved_profile_never_falls_back_to_defaults() {
  { Fixture f; }
  const auto valid = nvs_test::store;
  for (int failure = 0; failure < 6; ++failure) {
    nvs_test::store = valid;
    switch (failure) {
      case 0: nvs_test::store.durable[3] = 2; break;
      case 1: nvs_test::store.durable.pop_back(); break;
      case 2: nvs_test::store.durable.clear(); break;
      case 3: nvs_test::store.fail_read = true; break;
      case 4: nvs_test::store.fail_open = true; break;
      case 5: nvs_test::store.durable[4 + 8] = 0; break;
    }
    Radio radio; RNG rng; WifiKissMultiplexer mux;
    mux.attachRadio(radio, rng, configure, power);
    const auto changes = applied.changes;
    assert(!mux.setInitialConfiguration({910525000, 62500, 7, 5, 20}));
    assert(applied.changes == changes && radio.transmitted.empty());
  }
}

static void legacy_exact_configuration_join_is_a_noop() {
  nvs_test::reset();
  {
    Fixture fresh(1, true);
    const auto defaults = readConfigForUpdate(fresh);
    std::vector<uint8_t> radio{HW_CMD_SET_RADIO};
    radio.insert(radio.end(), defaults.begin() + 7, defaults.begin() + 17);
    for (const auto& request : std::vector<std::vector<uint8_t>>{
             radio, {HW_CMD_SET_TX_POWER, defaults[17]}}) {
      fresh.hardware(1, request);
      assert(receive(fresh.peers[1]) == std::vector<std::vector<uint8_t>>({{6, 0xf1, 2}}));
    }
    assert(nvs_test::store.writes == 0);
  }
  Fixture f;
  f.modem.setRadioCallback(configure);
  f.modem.setTxPowerCallback(power);
  const auto original = readConfigForUpdate(f);
  std::vector<uint8_t> radio{HW_CMD_SET_RADIO};
  radio.insert(radio.end(), original.begin() + 7, original.begin() + 17);
  const std::vector<uint8_t> power{HW_CMD_SET_TX_POWER, original[17]};
  const auto changes = applied.changes;
  const auto power_changes = applied.power_changes;
  const auto writes = nvs_test::store.writes;

  close(f.peers[1]);
  f.peers[1] = -1;
  f.step();
  int fds[2]; assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
  f.server.add(WiFiClient(fds[0]));
  f.peers[1] = fds[1];
  f.step();
  f.job(0, 1, 0, 1000, 0xaa);
  receive(f.peers[0]);
  for (const auto& request : {radio, power}) {
    f.hardware(1, request);
    assert(receive(f.peers[1]) == std::vector<std::vector<uint8_t>>({{6, 0xf0}}));
    assert(receive(f.peers[0]).empty());
  }
  for (size_t i = 1; i < radio.size(); ++i) {
    auto mismatch = radio;
    mismatch[i] ^= 1;
    f.hardware(1, mismatch);
    assert(receive(f.peers[1]) == std::vector<std::vector<uint8_t>>({{6, 0xf1, 2}}));
  }
  auto short_radio = radio; short_radio.pop_back();
  auto long_radio = radio; long_radio.push_back(0);
  for (const auto& rejected : std::vector<std::vector<uint8_t>>{
           short_radio, long_radio, {HW_CMD_SET_TX_POWER},
           {HW_CMD_SET_TX_POWER, static_cast<uint8_t>(original[17] + 1)},
           {HW_CMD_SET_TX_POWER, original[17], 0}, {HW_CMD_REBOOT}}) {
    f.hardware(1, rejected);
    assert(receive(f.peers[1]) == std::vector<std::vector<uint8_t>>({{6, 0xf1, 2}}));
  }
  for (uint8_t timing = 1; timing <= 5; ++timing) {
    f.send(1, timing, {1});
    assert(receive(f.peers[1]) == std::vector<std::vector<uint8_t>>({{6, 0xf1, 2}}));
  }
  f.hardware(1, {queued_tx::HELLO, queued_tx::VERSION, 1});
  auto frames = receive(f.peers[1]);
  assert(frames[0][3] == queued_tx::NOT_OWNER && frames[0][13] == 0);
  assert(readConfigForUpdate(f) == original);
  assert(f.radio.transmitted.empty());
  clock_ms += 1000;
  f.step();
  assert(f.radio.sending && f.radio.transmitted.size() == 1);
  for (const auto& request : {radio, power}) {
    f.hardware(1, request);
    assert(receive(f.peers[1]) == std::vector<std::vector<uint8_t>>({{6, 0xf0}}));
    assert(f.radio.sending);
  }
  assert(applied.changes == changes && applied.power_changes == power_changes);
  assert(nvs_test::store.writes == writes);
  assert(readConfigForUpdate(f) == original);
  f.finish();
  assert(event(receive(f.peers[0]), queued_tx::SUCCEEDED));
  frames = receive(f.peers[1]);
  assert(frames.size() == 2 && frames[0] == std::vector<uint8_t>({0, 0xaa}) &&
         frames[1][1] == HW_RESP_RX_META);
  f.send(1, KISS_CMD_DATA, {0xbb});
  assert(f.radio.sending && f.radio.transmitted.size() == 2);
  assert(receive(f.peers[1]).empty());
  f.finish();
  assert(receive(f.peers[1]) == std::vector<std::vector<uint8_t>>({{6, 0xf8, 1}}));
  frames = receive(f.peers[0]);
  assert(frames.size() == 2 && frames[0] == std::vector<uint8_t>({0, 0xbb}));
  f.hardware(0, {queued_tx::STATS, queued_tx::VERSION});
  frames = receive(f.peers[0]);
  assert(queued_tx::get32(frames[0].data() + 11) == 200);
}

static void dashboard_observes_rf_not_queue_or_reflection() {
  Fixture f;
  RadioDashboard dashboard;
  f.mux.observeWith(dashboard);
  f.job(0, 1, 0, 1000, 0xaa);
  RadioDashboard::RadioStatus status;
  RadioDashboard::Snapshot snapshot;
  f.mux.dashboardStatus(status);
  assert(dashboard.publish(clock_ms, status) && dashboard.snapshot(snapshot));
  assert(snapshot.tx_accepted == 1 && snapshot.event_count == 0 && snapshot.tx_rf_ms == 0);
  assert(snapshot.radio.queued == 1 && snapshot.radio.clients == 2 && snapshot.radio.owner_slot == 0);
  clock_ms += 1000;
  f.step();
  f.finish(100);
  f.radio.start_ok = false;
  f.job(0, 2, 0, 0, 0xbb);
  f.radio.start_ok = true;
  f.job(0, 3, 0, 0, 0xcc);
  clock_ms += 150;
  f.step();
  f.job(0, 4, 0, 1000, 0xdd);
  close(f.peers[0]); f.peers[0] = -1;
  f.step();
  f.mux.dashboardStatus(status);
  assert(dashboard.publish(clock_ms, status) && dashboard.snapshot(snapshot));
  assert(snapshot.tx_accepted == 4 && snapshot.tx_succeeded == 1 &&
         snapshot.tx_failed == 2 && snapshot.tx_unknown == 1);
  assert(snapshot.tx_rf_ms == 250 && snapshot.rx_packets == 0 && snapshot.event_count == 4);
  RadioDashboard::Totals totals;
  assert(dashboard.totals(totals));
  assert(totals.tx_accepted == 4 && totals.tx_succeeded == 1 &&
         totals.tx_failed == 2 && totals.tx_unknown == 1 &&
         totals.tx_rf_ms == 250 && totals.rx_packets == 0);
  assert(snapshot.events[0].rf_ms == 100 && snapshot.events[0].queue_ms == 1000);
  assert(snapshot.events[1].reason == queued_tx::START_FAILED);
  assert(snapshot.events[2].rf_ms == 150 && snapshot.events[2].reason == queued_tx::RF_TIMEOUT);
  assert(snapshot.events[3].reason == queued_tx::DISCONNECTED && snapshot.events[3].rf_ms == 0);
  assert(snapshot.radio.queued == 0 && snapshot.radio.clients == 1 && snapshot.radio.owner_slot == -1);
  for (size_t i = 0; i < snapshot.event_count; ++i) {
    assert(!snapshot.events[i].rx && !snapshot.events[i].has_signal);
    assert(snapshot.events[i].source_generation == f.generations[0]);
  }
}

int main() {
  stream_endpoint_cases();
#ifdef QUEUED_ADAPTER_TEST
  actual_queued_adapter();
  queued_adapter_requires_committed_profile();
#endif
  priority_and_controls();
  future_fifo_and_expiry();
  measured_budget_unknown_and_legacy();
  full_queue_reserves_controls_and_fifo();
  configuration_is_owned_and_pending_jobs_are_stable();
  read_only_capacity_does_not_claim_ownership();
  reconnect_preserves_aggregate_and_old_tx_ownership();
  source_constraints_expiry_and_wrap();
  native_carrier_retry_and_start_failure();
  carrier_scan_is_not_rf_airtime();
  blocking_carrier_reselects_and_preserves_retry();
  mesh_carrier_retry_is_randomized();
  changed_profile_requires_fresh_readback();
  physical_service_does_not_require_host_owner();
  native_source_float_is_not_quantized();
  aggregate_runtime_factor_and_zero();
  persistent_operator_profile_survives_reboot();
  profile_commit_failure_blocks_tx_until_explicit_recovery();
  invalid_saved_profile_never_falls_back_to_defaults();
  legacy_exact_configuration_join_is_a_noop();
  dashboard_observes_rf_not_queue_or_reflection();
  puts("combined real KissModem + multiplexer: priority/FIFO, timing/wrap, controls, budgets, config ownership, reconnect, reflection passed");
  return 0;
}
