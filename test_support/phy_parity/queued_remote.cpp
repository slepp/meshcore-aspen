#include "RemoteKissRadio.h"
#include <cassert>
#include <cmath>
#include <cstring>
#include <deque>
#include <helpers/StaticPoolPacketManager.h>
#include <helpers/StatsFormatHelper.h>
#include <helpers/SimpleMeshTables.h>
#include <vector>

static uint32_t clock_ms;
unsigned long millis() { return clock_ms; }
void delay(unsigned long n) { clock_ms += n; }

struct Wire : Stream {
  std::deque<uint8_t> incoming;
  std::vector<uint8_t> outgoing;
  bool short_write = false;
  int available() override { return incoming.size(); }
  int read() override {
    if (incoming.empty())
      return -1;
    uint8_t b = incoming.front();
    incoming.pop_front();
    return b;
  }
  size_t write(uint8_t b) override {
    outgoing.push_back(b);
    return 1;
  }
  size_t write(const uint8_t *p, size_t n) override {
    const size_t written = short_write ? n / 2 : n;
    outgoing.insert(outgoing.end(), p, p + written);
    return written;
  }
  void frame(uint8_t command, const std::vector<uint8_t> &data) {
    incoming.push_back(0xc0);
    incoming.push_back(command);
    for (uint8_t b : data) {
      if (b == 0xc0 || b == 0xdb) {
        incoming.push_back(0xdb);
        incoming.push_back(b == 0xc0 ? 0xdc : 0xdd);
      } else
        incoming.push_back(b);
    }
    incoming.push_back(0xc0);
  }
  void hardware(uint8_t command, const std::vector<uint8_t> &data) {
    std::vector<uint8_t> payload{command};
    payload.insert(payload.end(), data.begin(), data.end());
    frame(6, payload);
  }
  std::vector<uint8_t> take() {
    assert(outgoing.size() >= 3 && outgoing.front() == 0xc0 &&
           outgoing.back() == 0xc0);
    std::vector<uint8_t> decoded;
    bool escaped = false;
    for (size_t i = 1; i + 1 < outgoing.size(); ++i) {
      uint8_t b = outgoing[i];
      if (escaped) {
        decoded.push_back(b == 0xdc ? 0xc0 : 0xdb);
        escaped = false;
      } else if (b == 0xdb)
        escaped = true;
      else {
        assert(b != 0xc0);
        decoded.push_back(b);
      }
    }
    outgoing.clear();
    return decoded;
  }
  std::vector<std::vector<uint8_t>> takeAll() {
    std::vector<std::vector<uint8_t>> frames;
    std::vector<uint8_t> frame;
    bool escaped = false;
    for (const auto byte : outgoing) {
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
    outgoing.clear();
    return frames;
  }
};

static std::vector<uint8_t> profile(uint32_t frequency = 912525000) {
  std::vector<uint8_t> p(24);
  p[0] = 1;
  queued_tx::put32(p.data() + 2, 7);
  queued_tx::put32(p.data() + 6, frequency);
  queued_tx::put32(p.data() + 10, 250000);
  p[14] = 7;
  p[15] = 5;
  p[16] = 2;
  queued_tx::putFloat(p.data() + 17, 1);
  return p;
}
static std::vector<uint8_t> statistics(uint32_t source_rf = 0) {
  std::vector<uint8_t> data(39);
  data[0] = 1;
  queued_tx::put32(data.data() + 1, 7);
  queued_tx::put32(data.data() + 5, 6000);
  queued_tx::put32(data.data() + 9, 1000);
  queued_tx::put32(data.data() + 13, 19);
  queued_tx::put32(data.data() + 17, 2);
  queued_tx::put32(data.data() + 21, 450);
  queued_tx::put32(data.data() + 25, source_rf);
  queued_tx::put32(data.data() + 29, 7);
  queued_tx::put32(data.data() + 33, 1);
  data[37] = 3;
  data[38] = 1;
  return data;
}
static void negotiate(Wire &wire, RemoteKissRadio &radio,
                      uint32_t generation = 10,
                      bool malformed_airtime = false) {
  radio.onLinkConnected();
  assert(wire.take() == std::vector<uint8_t>({6, 0x20, 1, 0}));
  std::vector<uint8_t> hello(12);
  hello[0] = 1;
  hello[10] = 12;
  queued_tx::put32(hello.data() + 2, generation);
  queued_tx::put32(hello.data() + 6, 7);
  wire.hardware(0xa0, hello);
  radio.loop();
  assert(wire.take() == std::vector<uint8_t>({6, 0x22, 1, 0}));
  wire.hardware(0xa2, profile());
  radio.loop();
  auto commit = wire.take();
  assert(commit.size() == 12 && commit[1] == 0x09);
  const auto expected_profile = profile();
  assert(memcmp(commit.data() + 2, expected_profile.data() + 6, 10) == 0);
  wire.hardware(0xf0, {});
  radio.loop();
  auto request = wire.take();
  assert(request.size() == 11 && request[1] == 0x23 &&
         queued_tx::get32(request.data() + 3) == generation);
  std::vector<uint8_t> source{1, 0};
  source.insert(source.end(), request.begin() + 7, request.end());
  wire.hardware(0xa3, source);
  radio.loop();
  assert(wire.take() == std::vector<uint8_t>({6, 0x19, 1}));
  wire.hardware(0x9a, {1});
  radio.loop();
  assert(wire.take() == std::vector<uint8_t>({6, 0x10}));
  wire.hardware(0x90, {0x88, 0xff});
  radio.loop();
  for (unsigned n = 0; n < 256; ++n) {
    assert(!radio.queuedReady());
    assert(radio.getEstAirtimeFor(n) ==
           0); // No partially captured cache escapes.
    assert(wire.take() ==
           std::vector<uint8_t>({6, 0x0f, static_cast<uint8_t>(n)}));
    std::vector<uint8_t> time(4);
    queued_tx::put32(time.data(), 40 + n);
    if (malformed_airtime && n == 100) {
      time.pop_back();
      wire.hardware(0x8f, time);
      radio.loop();
      assert(radio.linkFault() && radio.getEstAirtimeFor(20) == 0);
      return;
    }
    wire.hardware(0x8f, time);
    radio.loop();
  }
  assert(wire.take() == std::vector<uint8_t>({6, 0x22, 1, 0}));
  wire.hardware(0xa2, profile());
  radio.loop();
  assert(!radio.queuedReady());
  assert(wire.take() == std::vector<uint8_t>({6, 0x24, 1}));
  wire.hardware(0xa4, statistics());
  radio.loop();
  assert(radio.queuedReady() && wire.outgoing.empty());
  assert(radio.getEstAirtimeFor(149) == 189 &&
         radio.getEstAirtimeFor(255) == 295);
}
static void event(Wire &wire, uint32_t job, uint8_t state, uint32_t rf,
                  uint32_t generation = 10, uint8_t reason = 0) {
  std::vector<uint8_t> data(23);
  data[0] = 1;
  queued_tx::put32(data.data() + 1, generation);
  queued_tx::put32(data.data() + 5, job);
  data[9] = state;
  data[10] = reason;
  queued_tx::put32(data.data() + 11, 120000);
  queued_tx::put32(data.data() + 15, rf);
  queued_tx::put32(data.data() + 19, 42);
  wire.hardware(0xfa, data);
}

struct Clock : mesh::MillisecondClock {
  unsigned long getMillis() override { return clock_ms; }
};
struct RealChip : mesh::Radio {
  bool sending = false, done = false;
  int recvRaw(uint8_t *, int) override { return 0; }
  uint32_t getEstAirtimeFor(int) override { return 100; }
  float packetScore(float, int) override { return 0; }
  bool startSendRaw(const uint8_t *, int) override {
    sending = true;
    return true;
  }
  bool isSendComplete() override { return done; }
  void onSendFinished() override {
    sending = false;
    done = false;
  }
  bool isInRecvMode() const override { return !sending; }
};
struct Native : mesh::Dispatcher {
  int sent = 0, failed = 0, received = 0, local = 0;
  std::vector<mesh::QueuedTransmitResult> outcomes;
  Native(mesh::Radio &radio, Clock &clock, mesh::PacketManager &pool)
      : Dispatcher(radio, clock, pool) {}
  mesh::DispatcherAction onRecvPacket(mesh::Packet *packet) override {
    ++received;
    if (packet->_localReflection)
      ++local;
    return ACTION_RETRANSMIT_DELAYED(1, 900);
  }
  void logTx(mesh::Packet *, int) override { ++sent; }
  void logTxFail(mesh::Packet *, int) override { ++failed; }
  void logQueuedTxResult(mesh::Packet *,
                         const mesh::QueuedTransmitResult &result) override {
    outcomes.push_back(result);
  }
  float getAirtimeBudgetFactor() const override { return 99; }
};
static mesh::Packet *packet(Native &native, uint8_t marker) {
  auto *p = native.obtainNewPacket();
  assert(p);
  p->header = 1;
  p->path_len = 0;
  p->payload[0] = marker;
  p->payload_len = 1;
  return p;
}

static void authoritative_statistics() {
  clock_ms = 0;
  Wire wire;
  RemoteKissRadio radio(wire);
  Clock clock;
  StaticPoolPacketManager pool(4);
  Native native(radio, clock, pool);
  uint32_t value = 12345;
  mesh::QueuedRadioStats observed;
  observed.source_rf_ms = 9876;
  assert(!native.getQueuedRadioStats(observed) && observed.source_rf_ms == 9876);
  assert(!native.tryGetRemainingTxBudget(value) && value == 12345);
  assert(!native.tryGetTotalAirTime(value) && value == 12345);
  assert(native.getTotalAirTime() == mesh::Dispatcher::UNKNOWN_RADIO_STAT &&
         native.getRemainingTxBudget() == mesh::Dispatcher::UNKNOWN_RADIO_STAT);
  assert(native.getTotalAirTimeSeconds() == UINT32_MAX);
  RealChip signals;
  char json[256];
  StatsFormatHelper::formatRadioStats(json, &radio, signals, value, 3000, false);
  assert(strstr(json, "\"tx_air_secs\":4294967295,\"rx_air_secs\":3"));
  negotiate(wire, radio);
  assert(native.getQueuedRadioStats(observed));
  assert(observed.generation == 10 && observed.configuration_generation == 7 &&
         observed.captured_ms == 0 && observed.aggregate_credit_ms == 6000 &&
         observed.source_credit_ms == 450 && observed.aggregate_rf_ms == 1000 &&
         observed.source_rf_ms == 0 && observed.aggregate_successes == 19 &&
         observed.aggregate_failures == 2 && observed.source_successes == 7 &&
         observed.source_failures == 1 && observed.aggregate_queued == 3 &&
         observed.aggregate_transmitting);
  assert(native.tryGetRemainingTxBudget(value) && value == 450);
  clock_ms = 999; radio.loop();
  assert(wire.outgoing.empty());
  clock_ms = 1000; radio.loop();
  assert(wire.take() == std::vector<uint8_t>({6, 0x24, 1}));
  assert(radio.setQueuedSourcePolicy(0));
  radio.loop();
  assert(wire.outgoing.empty() && !radio.getQueuedRadioStats(observed));
  wire.hardware(0xa4, statistics());
  radio.loop();
  const auto source = wire.take();
  assert(source[1] == 0x23 && queued_tx::getFloat(source.data() + 7) == 0);
  wire.hardware(0xa3, {1, 0, 0, 0, 0, 0});
  radio.loop();
  assert(wire.take() == std::vector<uint8_t>({6, 0x24, 1}) && !radio.queuedReady());
  auto actual = statistics(77);
  queued_tx::put32(actual.data() + 5, 0);
  wire.hardware(0xa4, actual);
  radio.loop();
  assert(radio.queuedReady() && native.tryGetTotalAirTime(value) && value == 77);
  assert(native.getTotalAirTimeSeconds() == 0);
  assert(native.tryGetRemainingTxBudget(value) && value == 0 &&
         native.getRemainingTxBudget() == 0);
  clock_ms = 4999;
  assert(native.getQueuedRadioStats(observed));
  clock_ms = 5000;
  assert(!native.getQueuedRadioStats(observed) &&
         native.getTotalAirTime() == mesh::Dispatcher::UNKNOWN_RADIO_STAT);
  radio.loop();
  assert(wire.take() == std::vector<uint8_t>({6, 0x24, 1}));
  clock_ms += 3000; radio.loop();
  assert(radio.linkFault() && wire.outgoing.empty());

  // Reject malformed, changed-generation, out-of-domain and unsupported stats.
  for (unsigned which = 0; which < 7; ++which) {
    Wire invalid;
    RemoteKissRadio peer(invalid);
    negotiate(invalid, peer);
    uint32_t pending = 0;
    if (which == 2) {
      const uint8_t packet[] = {1, 0, 42};
      assert(peer.queueTransmit(packet, sizeof(packet), 0, 0, 600000, pending));
      invalid.take();
      event(invalid, pending, queued_tx::ACCEPTED, 0);
      peer.loop();
    }
    clock_ms += 1000; peer.loop();
    assert(invalid.take() == std::vector<uint8_t>({6, 0x24, 1}));
    auto bad = statistics();
    switch (which) {
    case 0: bad.pop_back(); break;
    case 1: bad[0] = 2; break;
    case 2: queued_tx::put32(bad.data() + 1, 8); break;
    case 3: queued_tx::put32(bad.data() + 5, queued_tx::WINDOW_MS + 1); break;
    case 4: queued_tx::put32(bad.data() + 21, queued_tx::WINDOW_MS + 1); break;
    case 5: bad[37] = 13; break;
    case 6: bad[38] = 2; break;
    }
    invalid.hardware(0xa4, bad);
    peer.loop();
    assert(peer.linkFault() && !peer.getQueuedRadioStats(observed));
    if (which == 2) {
      mesh::QueuedTransmitResult outcome;
      assert(peer.pollQueuedResult(outcome) && outcome.state == queued_tx::ACCEPTED);
      assert(peer.pollQueuedResult(outcome) && outcome.state == queued_tx::UNKNOWN &&
             !outcome.has_rf_ms && outcome.job == pending);
      assert(!peer.pollQueuedResult(outcome) && invalid.outgoing.empty());
    }
  }
  clock_ms = UINT32_MAX - 500;
  Wire wrapped;
  RemoteKissRadio peer(wrapped);
  negotiate(wrapped, peer);
  clock_ms += 999; peer.loop();
  assert(wrapped.outgoing.empty() && peer.getQueuedRadioStats(observed));
  ++clock_ms; peer.loop();
  assert(wrapped.take() == std::vector<uint8_t>({6, 0x24, 1}));
  wrapped.hardware(0xa4, statistics(UINT32_MAX));
  peer.loop();
  assert(peer.getQueuedRadioStats(observed) && observed.source_rf_ms == UINT32_MAX);
  Native wrapped_native(peer, clock, pool);
  assert(wrapped_native.getTotalAirTimeSeconds() == UINT32_MAX / 1000);
  const bool known = wrapped_native.tryGetTotalAirTime(value);
  StatsFormatHelper::formatRadioStats(json, &peer, signals, value, 3000, known);
  assert(strstr(json, "\"tx_air_secs\":4294967,\"rx_air_secs\":3"));
  // Existing non-queued callers keep the original millisecond-input default.
  StatsFormatHelper::formatRadioStats(json, &signals, signals, 2345, 3000);
  assert(strstr(json, "\"tx_air_secs\":2,\"rx_air_secs\":3"));
  peer.onLinkDisconnected();
  assert(!peer.getQueuedRadioStats(observed));
  clock_ms = 0;
}

static void receive_metadata_adjacency() {
  for (unsigned invalid = 0; invalid < 8; ++invalid) {
    clock_ms = 0;
    Wire wire;
    RemoteKissRadio radio(wire);
    negotiate(wire, radio);
    wire.frame(0, {0xa1});
    switch (invalid) {
    case 0: wire.hardware(0x97, {}); break;
    case 1: wire.frame(0x16, {0xf9, 0x80, 0x7f}); break;
    case 2: wire.frame(1, {3}); break;
    case 3: wire.hardware(0xf9, {0x80}); break;
    case 4: wire.hardware(0xf9, {0x80, 0x7f, 0}); break;
    case 5:
      for (auto b : {0xc0, 6, 0xf9, 0xdb, 0, 0xc0}) wire.incoming.push_back(b);
      break;
    case 6:
      for (auto b : {0xc0, 6, 0xf9, 0x80, 0xdb, 0xc0}) wire.incoming.push_back(b);
      break;
    case 7:
      wire.incoming.push_back(0xc0);
      wire.incoming.insert(wire.incoming.end(), 281, 0x11);
      wire.incoming.push_back(0xc0);
      break;
    }
    wire.hardware(0xf9, {0x80, 0x7f});
    uint8_t packet[255];
    assert(radio.recvRaw(packet, sizeof(packet)) == 0);
    assert(radio.getPacketsRecvErrors() == 1 && radio.lastError() != nullptr &&
           !radio.linkFault() && radio.getPacketsRecv() == 0);
    wire.frame(0, {0xb2});
    wire.hardware(0xf9, {16, 0xa6});
    assert(radio.recvRaw(packet, sizeof(packet)) == 1 && packet[0] == 0xb2);
    assert(!radio.lastReceiveWasLocal() && radio.getLastSNR() == 4 &&
           radio.getLastRSSI() == -90 && radio.getPacketsRecvErrors() == 1);
  }
  {
    Wire wire;
    RemoteKissRadio radio(wire);
    negotiate(wire, radio);
    wire.frame(0, {0xa1});
    wire.frame(0, {0xb2});
    wire.hardware(0xf9, {16, 0xa6});
    uint8_t packet[255];
    assert(radio.recvRaw(packet, sizeof(packet)) == 1 && packet[0] == 0xb2);
    assert(radio.getPacketsRecvErrors() == 1 && radio.getLastRSSI() == -90);
    wire.frame(0, {0xc3});
    wire.hardware(0xf9, {16, 0xa6});
    wire.frame(0, {0xd4});
    wire.hardware(0xf9, {0x80, 0x7f});
    assert(radio.recvRaw(packet, sizeof(packet)) == 1 && packet[0] == 0xc3);
    assert(!radio.lastReceiveWasLocal() && radio.getLastRSSI() == -90 &&
           radio.getPacketsRecvErrors() == 2);
    assert(radio.recvRaw(packet, sizeof(packet)) == 0);
    wire.frame(0, {0xe5});
    wire.hardware(0xf9, {16, 0xa6});
    for (auto b : {0xc0, 6, 0xdb, 0, 0xc0}) wire.incoming.push_back(b);
    wire.hardware(0xf9, {0x80, 0x7f});
    assert(radio.recvRaw(packet, sizeof(packet)) == 1 && packet[0] == 0xe5 &&
           !radio.lastReceiveWasLocal() && radio.getLastRSSI() == -90);
    wire.frame(0, {0xf6});
    radio.loop();
    for (auto b : {0xc0, 6, 0xf9}) wire.incoming.push_back(b);
    assert(radio.recvRaw(packet, sizeof(packet)) == 0);
    for (auto b : {0x80, 0x7f, 0xc0}) wire.incoming.push_back(b);
    assert(radio.recvRaw(packet, sizeof(packet)) == 1 && packet[0] == 0xf6 &&
           radio.lastReceiveWasLocal() && std::isnan(radio.getLastRSSI()));
    wire.frame(0, {0x31});
    wire.hardware(0xf9, {16, 0xa6});
    wire.frame(0, {0x32});
    assert(radio.recvRaw(packet, sizeof(packet)) == 1 && packet[0] == 0x31);
    wire.hardware(0xf9, {0x80, 0x7f});
    assert(radio.recvRaw(packet, sizeof(packet)) == 0);
    wire.frame(0, {0x33});
    wire.hardware(0xf9, {16, 0xa6});
    assert(radio.recvRaw(packet, sizeof(packet)) == 1 && packet[0] == 0x33 &&
           !radio.lastReceiveWasLocal() && radio.getLastRSSI() == -90);
  }
  {
    Wire wire;
    RemoteKissRadio legacy(wire, RemoteKissRadio::Mode::Legacy);
    wire.frame(0, {0xa1});
    wire.hardware(0x97, {});
    wire.hardware(0xf9, {16, 0xa6, 0});
    uint8_t packet[255];
    assert(legacy.recvRaw(packet, sizeof(packet)) == 1 && packet[0] == 0xa1 &&
           legacy.getLastRSSI() == -90 && legacy.getPacketsRecvErrors() == 0);
  }
}

struct MeshRNG : mesh::RNG {
  void random(uint8_t* out, size_t length) override { memset(out, 3, length); }
};
struct MeshRTC : mesh::RTCClock {
  uint32_t getCurrentTime() override { return 100; }
  void setCurrentTime(uint32_t) override {}
};
struct ActualMesh : mesh::Mesh {
  unsigned acks = 0;
  bool last_ack_local = false, reply = false;
  float last_ack_snr = NAN;
  ActualMesh(mesh::Radio& radio, Clock& clock, MeshRNG& rng, MeshRTC& rtc,
             StaticPoolPacketManager& pool, SimpleMeshTables& tables)
      : Mesh(radio, clock, rng, rtc, pool, tables) {
    memset(self_id.pub_key, 0x42, sizeof(self_id.pub_key));
  }
  bool allowPacketForward(const mesh::Packet*) override { return true; }
  uint8_t getExtraAckTransmitCount() const override { return 2; }
  void onAckRecv(mesh::Packet* packet, uint32_t crc) override {
    ++acks;
    last_ack_local = packet->_localReflection;
    last_ack_snr = packet->getSNR();
    if (reply) {
      auto* response = createAck(crc ^ 0x01020304);
      assert(response);
      sendZeroHop(response);
    }
  }
};

static std::vector<uint8_t> ack_packet(bool multipart, bool routed, uint32_t crc) {
  std::vector<uint8_t> bytes{
      static_cast<uint8_t>((multipart ? PAYLOAD_TYPE_MULTIPART : PAYLOAD_TYPE_ACK)
                            << PH_TYPE_SHIFT | ROUTE_TYPE_DIRECT),
      static_cast<uint8_t>(routed ? 1 : 0)};
  if (routed) bytes.push_back(0x42);
  if (multipart) bytes.push_back(0x23);
  for (unsigned shift = 0; shift < 32; shift += 8) bytes.push_back(crc >> shift);
  return bytes;
}

static void actual_mesh_reflection() {
  for (bool multipart : {false, true}) {
    clock_ms = 0;
    Wire wire;
    RemoteKissRadio radio(wire);
    negotiate(wire, radio);
    Clock clock; MeshRNG rng; MeshRTC rtc;
    StaticPoolPacketManager pool(8);
    SimpleMeshTables tables;
    ActualMesh node(radio, clock, rng, rtc, pool, tables);
    node.begin();
    const auto routed = ack_packet(multipart, true, 0x12345678);
    wire.frame(0, routed);
    wire.hardware(0xf9, {0x80, 0x7f});
    node.loop();
    assert(wire.outgoing.empty() && pool.getFreeCount() == 8);
    assert(node.acks == (multipart ? 0u : 1u));
    if (!multipart) assert(node.last_ack_local);

    // The same packet on genuine RF still takes the native ACK forwarding path.
    wire.frame(0, routed);
    wire.hardware(0xf9, {16, 0xb0});
    node.loop();
    auto forwarded = wire.takeAll();
    assert(forwarded.size() == 3 && pool.getFreeCount() == 5);
    for (size_t i = 0; i < forwarded.size(); ++i) {
      const auto& request = forwarded[i];
      assert(request[0] == 6 && request[1] == 0x21 && request[11] == 0);
      assert(request[20] == (i < 2 ? 0x2a : 0x0e) && request[21] == 0);
      const uint32_t id = queued_tx::get32(request.data() + 7);
      event(wire, id, queued_tx::ACCEPTED, 0);
      event(wire, id, queued_tx::SUCCEEDED, 10);
    }
    node.loop();
    assert(pool.getFreeCount() == 8 && node.getNumSentDirect() == 3);

    const auto before = node.acks;
    wire.frame(0, ack_packet(multipart, false, 0x87654321));
    wire.hardware(0xf9, {0x80, 0x7f});
    node.loop();
    assert(node.acks == before + 1 && node.last_ack_local && node.last_ack_snr == 0 &&
           wire.outgoing.empty() && pool.getFreeCount() == 8);
    wire.frame(0, ack_packet(multipart, false, 0xabcddcba));
    wire.hardware(0xf9, {16, 0xb0});
    node.loop();
    assert(node.acks == before + 2 && !node.last_ack_local && node.last_ack_snr == 4 &&
           wire.outgoing.empty());

    // Application-generated replies remain legal; only RF forwarding is blocked.
    node.reply = true;
    wire.frame(0, ack_packet(multipart, false, 0x11223344));
    wire.hardware(0xf9, {0x80, 0x7f});
    node.loop();
    auto generated = wire.take();
    assert(generated[20] == 0x0e && generated[21] == 0 &&
           queued_tx::get32(generated.data() + 22) == (0x11223344 ^ 0x01020304));
  }
}

int main(int argc, char** argv) {
  if (argc == 2 && strcmp(argv[1], "rx-metadata") == 0) {
    receive_metadata_adjacency();
    return 0;
  }
  if (argc == 2 && strcmp(argv[1], "mesh-reflection") == 0) {
    actual_mesh_reflection();
    return 0;
  }
  assert(argc == 1);
  receive_metadata_adjacency();
  actual_mesh_reflection();
  authoritative_statistics();
  {
    RealChip chip;
    Clock clock;
    StaticPoolPacketManager pool(4);
    Native unchanged(chip, clock, pool);
    unchanged.begin();
    assert(!chip.supportsQueuedTransmit());
    uint32_t available;
    assert(unchanged.tryGetRemainingTxBudget(available) &&
           available == unchanged.getRemainingTxBudget());
    unchanged.sendPacket(packet(unchanged, 42), 0, 100);
    clock_ms = 99;
    unchanged.loop();
    assert(!chip.sending && pool.getOutboundTotal() == 1);
    clock_ms = 100;
    unchanged.loop();
    assert(chip.sending);
    clock_ms += 47;
    chip.done = true;
    unchanged.loop();
    assert(unchanged.sent == 1 && unchanged.getTotalAirTime() == 47);
    assert(pool.getFreeCount() == 4);
    clock_ms = 0;
  }
  Wire bad;
  RemoteKissRadio incomplete(bad);
  negotiate(bad, incomplete, 10, true);
  Wire wire;
  RemoteKissRadio radio(wire);
  radio.setParams(912.525f, 250, 7, 5);
  radio.setTxPower(2);
  assert(wire.outgoing
             .empty()); // Setters cache expectations, never mutate the PHY.
  assert(radio.setQueuedSourcePolicy(99));
  negotiate(wire, radio);
  Clock clock;
  StaticPoolPacketManager pool(20);
  Native native(radio, clock, pool);
  native
      .begin(); // A second begin does not renegotiate or reset in-flight state.
  assert(wire.outgoing.empty());
  native.sendPacket(packet(native, 0xaa), 4, 60000);
  auto first = wire.take();
  assert(first[1] == 0x21 && first[11] == 4);
  assert(queued_tx::get32(first.data() + 12) == 60000);
  assert(queued_tx::get32(first.data() + 16) == 0);
  const uint32_t job1 = queued_tx::get32(first.data() + 7);
  native.sendPacket(packet(native, 0xbb), 0, 0);
  auto second = wire.take();
  assert(second[11] == 0 && queued_tx::get32(second.data() + 12) == 0);
  assert(queued_tx::get32(second.data() + 16) == 0);
  const uint32_t job2 = queued_tx::get32(second.data() + 7);
  assert(job1 != job2 && pool.getOutboundTotal() == 0);
  event(wire, job1, 1, 0);
  event(wire, job2, 1, 0);
  native.loop();
  clock_ms = 24 * 60 * 60 * 1000;
  native.loop();
  assert(native.sent == 0 && native.failed == 0 &&
         native.getTotalAirTime() == mesh::Dispatcher::UNKNOWN_RADIO_STAT);
  assert(wire.take() == std::vector<uint8_t>({6, 0x24, 1}));
  wire.hardware(0xa4, statistics());
  native.loop();
  assert(native.getTotalAirTime() == 0);
  auto refresh = [&](uint32_t rf) {
    clock_ms += 1000; native.loop();
    assert(wire.take() == std::vector<uint8_t>({6, 0x24, 1}));
    wire.hardware(0xa4, statistics(rf));
    native.loop();
  };
  event(wire, job2, 2, 47);
  native.loop();
  refresh(47);
  assert(native.sent == 1 && native.getTotalAirTime() == 47);
  event(wire, job1, 4, 72, 10, queued_tx::RF_TIMEOUT);
  native.loop();
  assert(native.outcomes.back().has_rf_ms && native.outcomes.back().rf_ms == 72 &&
         native.outcomes.back().estimated_ms == 42);
  refresh(119);
  assert(native.sent == 1 && native.failed == 1 &&
         native.getTotalAirTime() == 119);
  assert(pool.getFreeCount() == 20 && wire.outgoing.empty());
  native.sendPacket(packet(native, 0xab), 0, 0);
  auto coalesced = wire.take();
  const uint32_t coalesced_job = queued_tx::get32(coalesced.data() + 7);
  event(wire, coalesced_job, queued_tx::ACCEPTED, 0);
  event(wire, coalesced_job, queued_tx::FAILED, 0, 10, queued_tx::START_FAILED);
  const auto before_events = native.outcomes.size();
  native.loop();
  assert(native.outcomes.size() == before_events + 2);
  assert(native.outcomes[before_events].state == queued_tx::ACCEPTED);
  assert(native.outcomes.back().state == queued_tx::FAILED);
  assert(native.failed == 2);

  wire.frame(0, {1, 0, 0xcc});
  wire.hardware(0xf9, {0x80, 0x7f});
  native.loop();
  assert(native.local == 1 && native.received == 1);
  assert(pool.getFreeCount() == 20 && wire.outgoing.empty());
  assert(native.getReceiveAirTime() == 0 && std::isnan(radio.getLastSNR()));

  native.sendPacket(packet(native, 0xdd), 2, 0);
  auto pending = wire.take();
  uint32_t retired = queued_tx::get32(pending.data() + 7);
  radio.onLinkDisconnected();
  native.loop();
  assert(native.failed == 3 && pool.getFreeCount() == 20);
  assert(native.outcomes.back().state == 4 &&
         !native.outcomes.back().has_rf_ms);
  negotiate(wire, radio, 11);
  event(wire, retired, 2, 999, 10);
  native.loop();
  assert(native.sent == 1 && native.getTotalAirTime() == 0);

  native.sendPacket(packet(native, 0xee), 0, 0);
  pending = wire.take();
  clock_ms += 3000;
  native.loop();
  assert(radio.linkFault() && native.failed == 4 && pool.getFreeCount() == 20);
  assert(wire.outgoing.empty()); // Deadline quarantines; never replays.
  negotiate(wire, radio, 12);
  wire.short_write = true;
  native.sendPacket(packet(native, 0xff), 0, 0);
  native.loop();
  assert(native.failed == 5 && pool.getFreeCount() == 20 && radio.linkFault());

  Wire mismatch;
  RemoteKissRadio changed(mismatch);
  negotiate(mismatch, changed);
  changed.onLinkConnected();
  mismatch.take();
  std::vector<uint8_t> hello(12);
  hello[0] = 1;
  hello[10] = 12;
  queued_tx::put32(hello.data() + 2, 12);
  mismatch.hardware(0xa0, hello);
  changed.loop();
  mismatch.take();
  mismatch.hardware(0xa2, profile(913000000));
  changed.loop();
  assert(changed.linkFault() && mismatch.outgoing.empty());
  Wire bounded;
  RemoteKissRadio limited(bounded);
  negotiate(bounded, limited);
  uint32_t id = 0;
  const uint8_t raw[] = {1, 0, 42};
  for (unsigned n = 0; n < 12; ++n) {
    assert(limited.queueTransmit(raw, sizeof(raw), n, 0, 600000, id));
    bounded.take();
  }
  assert(!limited.queueTransmit(raw, sizeof(raw), 0, 0, 600000, id));
  event(bounded, 1, queued_tx::REJECTED, 0, 10, queued_tx::FULL);
  limited.loop();
  mesh::QueuedTransmitResult rejected;
  assert(limited.pollQueuedResult(rejected) &&
         rejected.state == queued_tx::REJECTED);
  assert(limited.queueTransmit(raw, sizeof(raw), 0, 0, 600000, id) && id == 13);
  bounded.take();
  assert(!limited.setQueuedSourcePolicy(NAN) && limited.linkFault());
  puts("Queued native Dispatcher/RemoteKissRadio: priority/delay, actual "
       "airtime, long waits, reflection, reconnect and unknown retirement "
       "passed");
}
