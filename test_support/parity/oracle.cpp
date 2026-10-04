#include <Mesh.h>
#include <AES.h>
#include <SHA256.h>
#ifndef PARITY_NO_CAYENNE
#include <CayenneLPP.h>
#endif
#include <helpers/StaticPoolPacketManager.h>
#include <helpers/SimpleMeshTables.h>
#include <helpers/RoutingPolicy.h>
#include <helpers/ClientACL.h>
#include <helpers/RegionMap.h>
#include <helpers/BaseChatMesh.h>
#include <helpers/TxtDataHelpers.h>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cfloat>
#include <deque>
#include <iostream>
#include <string>
#include <vector>
#include "RecordedRNG.h"
#include "kernels.inc"

static std::string scenario;
static void require(bool ok, const char* message) {
  if (!ok) {
    std::cerr << scenario << ": " << message << '\n';
    std::exit(1);
  }
}
static std::string hex(const uint8_t* data, size_t size) {
  static const char chars[] = "0123456789abcdef";
  std::string out;
  for (size_t i = 0; i < size; ++i) {
    out += chars[data[i] >> 4];
    out += chars[data[i] & 15];
  }
  return out;
}
static std::vector<uint8_t> unhex(const char* text) {
  std::vector<uint8_t> out(strlen(text) / 2);
  require(mesh::Utils::fromHex(out.data(), out.size(), text), "invalid fixture hex");
  return out;
}
static void event(const std::string& fields) {
  std::cout << "{\"scenario\":\"" << scenario << "\"," << fields << "}\n";
}
static void passed() { event("\"event\":\"passed\""); }
struct Clock : mesh::MillisecondClock {
  unsigned long now = 1000;
  unsigned long getMillis() override { return now; }
};
struct RTC : mesh::RTCClock {
  uint32_t now = 1700000000;
  uint32_t getCurrentTime() override { return now; }
  void setCurrentTime(uint32_t n) override { now = n; }
};
struct RNG : RecordedRNG {
  std::vector<uint8_t> bytes;
  size_t cursor = 0;
  void random(uint8_t* dest, size_t n) override {
    require(!bytes.empty(), "RNG input exhausted/unconfigured");
    for (size_t i = 0; i < n; ++i) dest[i] = bytes[(cursor++) % bytes.size()];
  }
  void word(uint32_t n) {
    bytes = {uint8_t(n), uint8_t(n >> 8), uint8_t(n >> 16), uint8_t(n >> 24)};
    cursor = 0;
  }
};
struct Radio : mesh::Radio {
  Clock& clock;
  uint32_t airtime = 100;
  bool busy = false, start_ok = true, sending = false, complete = false;
  float score = 1.0f;
  std::deque<std::vector<uint8_t>> rx;
  std::vector<std::vector<uint8_t>> sent;
  unsigned long start = 0;
  explicit Radio(Clock& c) : clock(c) {}
  int recvRaw(uint8_t* dst, int capacity) override {
    if (rx.empty()) return 0;
    auto bytes = rx.front(); rx.pop_front();
    require(bytes.size() <= size_t(capacity), "oversized radio fixture");
    memcpy(dst, bytes.data(), bytes.size());
    return bytes.size();
  }
  uint32_t getEstAirtimeFor(int) override { return airtime; }
  float packetScore(float, int) override { return score; }
  bool startSendRaw(const uint8_t* p, int n) override {
    event("\"event\":\"tx_start\",\"at_ms\":" + std::to_string(clock.now) +
          ",\"accepted\":" + (start_ok ? "true" : "false") + ",\"bytes\":\"" + hex(p, n) + "\"");
    if (!start_ok) return false;
    sent.emplace_back(p, p+n); sending = true; start = clock.now;
    return true;
  }
  bool isSendComplete() override { return complete; }
  void onSendFinished() override {
    event("\"event\":\"tx_end\",\"at_ms\":" + std::to_string(clock.now) +
          ",\"complete\":" + (complete ? "true" : "false"));
    sending = false; complete = false;
  }
  bool isInRecvMode() const override { return !sending; }
  bool isReceiving() override { return busy; }
  float getLastSNR() const override { return 4.5f; }
  float getLastRSSI() const override { return -73; }
};
struct Queue : StaticPoolPacketManager {
  struct Entry { uint8_t priority; uint32_t at; std::string bytes; };
  std::vector<Entry> events;
  std::vector<uint32_t> holds;
  Queue() : StaticPoolPacketManager(32) {}
  void queueOutbound(mesh::Packet* p, uint8_t priority, uint32_t at) override {
    uint8_t raw[MAX_TRANS_UNIT];
    auto size = p->writeTo(raw);
    events.push_back({priority, at, hex(raw, size)});
    event("\"event\":\"enqueue\",\"priority\":" + std::to_string(priority) +
          ",\"eligible_ms\":" + std::to_string(at) + ",\"bytes\":\"" + hex(raw, size) + "\"");
    StaticPoolPacketManager::queueOutbound(p, priority, at);
  }
  void queueInbound(mesh::Packet* p, uint32_t at) override {
    holds.push_back(at);
    event("\"event\":\"rx_hold\",\"eligible_ms\":" + std::to_string(at));
    StaticPoolPacketManager::queueInbound(p, at);
  }
};
class Core : public mesh::Mesh {
public:
  unsigned long window = 1000;
  float airtime_factor = 1;
  bool forward = true;
  bool dispatcher_cad = false;
  int received = 0;
  Core(Radio& r, Clock& c, RNG& rng, RTC& rtc, Queue& q, SimpleMeshTables& tables)
    : Mesh(r, c, rng, rtc, q, tables) {}
  using Mesh::routeRecvPacket;
  using Mesh::getRetransmitDelay;
  using Mesh::getDirectRetransmitDelay;
  uint32_t getCADFailRetryDelay() const override {
    return dispatcher_cad ? Dispatcher::getCADFailRetryDelay() : Mesh::getCADFailRetryDelay();
  }
  uint32_t cadLimit() const { return Dispatcher::getCADFailMaxDuration(); }
  using Dispatcher::calcRxDelay;
  uint16_t errors() const { return _err_flags; }
protected:
  float getAirtimeBudgetFactor() const override { return airtime_factor; }
  unsigned long getDutyCycleWindowMs() const override { return window; }
  bool allowPacketForward(const mesh::Packet*) override { return forward; }
  void onRawDataRecv(mesh::Packet*) override { ++received; }
};
struct Fixture {
  Clock clock; RTC rtc; RNG rng; Radio radio; Queue queue; SimpleMeshTables tables; Core core;
  Fixture() : radio(clock), core(radio, clock, rng, rtc, queue, tables) {
    rng.word(0);
    core.self_id = mesh::LocalIdentity(&rng);
    core.begin();
  }
  mesh::Packet* packet(uint8_t type, uint8_t tag = 0xA5) {
    auto p = core.obtainNewPacket();
    require(p != nullptr, "packet pool empty");
    p->header = type << PH_TYPE_SHIFT; p->payload_len = 1; p->payload[0] = tag;
    return p;
  }
  void step(unsigned long n) { clock.now = n; core.loop(); }
};

static void cryptoVectors() {
  scenario = "crypto-known-answer";
  uint8_t digest[32], encrypted[16], decrypted[16];
  const uint8_t abc[] = {'a','b','c'};
  mesh::Utils::sha256(digest, sizeof(digest), abc, sizeof(abc));
  require(hex(digest, 32) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "FIPS 180-4 SHA256 abc");
  auto key = unhex("000102030405060708090a0b0c0d0e0f");
  auto plain = unhex("00112233445566778899aabbccddeeff");
  AES128 aes;
  aes.setKey(key.data(), key.size());
  aes.encryptBlock(encrypted, plain.data());
  require(hex(encrypted, 16) == "69c4e0d86a7b0430d8cdb78070b4c55a", "FIPS 197 AES128");
  aes.decryptBlock(decrypted, encrypted);
  require(std::equal(plain.begin(), plain.end(), decrypted), "AES decrypt");
  SHA256 sha;
  uint8_t hkey[20]; memset(hkey, 0x0b, sizeof(hkey));
  sha.resetHMAC(hkey, sizeof(hkey));
  sha.update("Hi There", 8);
  sha.finalizeHMAC(hkey, sizeof(hkey), digest, sizeof(digest));
  require(hex(digest, 32) == "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7",
          "RFC4231 HMAC case 1");
  RNG rng;
  rng.bytes = unhex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60");
  mesh::LocalIdentity identity(&rng);
  require(hex(identity.pub_key, 32) == "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
          "RFC8032 test 1 public key");
  uint8_t signature[64], empty = 0;
  identity.sign(signature, &empty, 0);
  require(hex(signature, 64) ==
          "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555f"
          "b8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b",
          "RFC8032 test 1 signature");
  require(identity.verify(signature, &empty, 0), "real Ed25519 verify");
  signature[0] ^= 1;
  require(!identity.verify(signature, &empty, 0), "corrupt signature accepted");
  event("\"event\":\"known_answers\",\"sha256\":\"FIPS180-4\",\"aes\":\"FIPS197\",\"hmac\":\"RFC4231-1\",\"ed25519\":\"RFC8032-7.1-1\"");
  passed();
}

static void paths() {
  scenario = "ordinary-path-encoding";
  Fixture f;
  for (unsigned encoded = 0; encoded < 256; ++encoded) {
    unsigned width = (encoded >> 6) + 1, count = encoded & 63;
    bool expected = width < 4 && width * count <= 64;
    bool valid = mesh::Packet::isValidPathLen(encoded);
    require(valid == expected, "path encoding");
    event("\"event\":\"path_valid\",\"encoded\":" + std::to_string(encoded) +
          ",\"valid\":" + (valid ? "true" : "false"));
    if (!valid) continue;
    mesh::Packet p, parsed;
    p.header = ROUTE_TYPE_TRANSPORT_FLOOD | (PAYLOAD_TYPE_RAW_CUSTOM << PH_TYPE_SHIFT);
    p.path_len = encoded; p.transport_codes[0] = 0x1234; p.transport_codes[1] = 0xabcd;
    std::fill(p.path, p.path + 64, 0x42);
    p.payload[0] = 0x99; p.payload_len = 1;
    uint8_t raw[255];
    auto size = p.writeTo(raw);
    require(f.core.tryParsePacket(&parsed, raw, size), "roundtrip parse");
    require(parsed.path_len == encoded && parsed.payload[0] == 0x99, "roundtrip values");
    if (count) require(!f.core.tryParsePacket(&parsed, raw, 6 + count*width - 1), "truncated path");
  }
  passed();
  scenario = "native-path-overflow-exception";
  mesh::Packet p;
  p.header = ROUTE_TYPE_FLOOD | (PAYLOAD_TYPE_RAW_CUSTOM << PH_TYPE_SHIFT);
  p.setPathHashSizeAndCount(1, 63); p.payload_len = 1; p.payload[0] = 1;
  auto action = f.core.routeRecvPacket(&p);
  require(action != ACTION_RELEASE && p.path_len == 64 && p.getPathHashCount() == 0 &&
          p.getPathHashSize() == 2, "reference overflow exception changed");
  event("\"event\":\"reference_exception\",\"before\":63,\"after\":64,\"safe_host_outcome\":\"reject_forward\"");
  for (uint8_t width : {2,3}) {
    p.header = ROUTE_TYPE_FLOOD;
    p.setPathHashSizeAndCount(width, 64 / width);
    require(f.core.routeRecvPacket(&p) == ACTION_RELEASE, "full multi-byte path forwarded");
  }
  passed();
}

static void priorities() {
  scenario = "originated-priorities";
  Fixture f;
  for (uint8_t width : {1,2,3}) {
    for (uint8_t type : {PAYLOAD_TYPE_PATH, PAYLOAD_TYPE_ADVERT, PAYLOAD_TYPE_RAW_CUSTOM}) {
      f.core.sendFlood(f.packet(type), 25, width);
      auto e = f.queue.events.back();
      require(e.priority == (type == PAYLOAD_TYPE_PATH ? 2 : type == PAYLOAD_TYPE_ADVERT ? 3 : 1),
              "flood priority");
      require(e.at == 1025, "flood eligibility");
    }
  }
  uint8_t path[] = {1,2,3};
  for (uint8_t width : {1,2,3}) {
    for (uint8_t type : {PAYLOAD_TYPE_PATH, PAYLOAD_TYPE_RAW_CUSTOM}) {
      auto packet = f.packet(type);
      f.core.sendDirect(packet, path, uint8_t(((width-1) << 6) | 1), 20);
      require(f.queue.events.back().priority == (type == PAYLOAD_TYPE_PATH ? 1 : 0), "direct priority");
      require(f.queue.events.back().at == 1020 && packet->getPathHashSize() == width &&
              packet->getPathHashCount() == 1, "direct path width/eligibility");
    }
  }
  f.core.sendZeroHop(f.packet(PAYLOAD_TYPE_ADVERT), uint32_t(0));
  require(f.queue.events.back().priority == 0, "zero-hop priority");
  passed();
  scenario = "trace-origin";
  uint8_t trace_path[16];
  for (uint8_t i = 0; i < sizeof(trace_path); ++i) trace_path[i] = 10 + i;
  for (uint8_t flags = 0; flags < 4; ++flags) {
    const uint8_t width = 1 << flags, length = width * 2;
    auto trace = f.core.createTrace(0x01020304, 0x05060708, flags);
    require(trace != nullptr, "trace allocation");
    f.core.sendDirect(trace, trace_path, length, 20);
    require(trace->path_len == 0 && hex(trace->payload, trace->payload_len) ==
            "0403020108070605" + hex(&flags, 1) + hex(trace_path, length),
            "TRACE hash path must live in payload");
    require(f.queue.events.back().priority == 5 && f.queue.events.back().at == 1020,
            "TRACE priority/eligibility");
    event("\"event\":\"trace_inputs\",\"tag\":16909060,\"auth_code\":84281096,"
          "\"flags\":" + std::to_string(flags) + ",\"hash_width\":" + std::to_string(width) +
          ",\"path\":\"" + hex(trace_path, length) + "\",\"delay_ms\":20,\"clock_ms\":1000");
  }
  passed();
  scenario = "eligible-priority-fifo";
  Fixture order;
  order.core.sendPacket(order.packet(PAYLOAD_TYPE_RAW_CUSTOM, 1), 0, 100);
  order.core.sendPacket(order.packet(PAYLOAD_TYPE_RAW_CUSTOM, 2), 3, 0);
  order.core.sendPacket(order.packet(PAYLOAD_TYPE_RAW_CUSTOM, 3), 3, 0);
  auto first = order.queue.getNextOutbound(1000);
  require(first && first->payload[0] == 2, "future job blocks ready work");
  order.queue.free(first);
  auto second = order.queue.getNextOutbound(1000);
  require(second && second->payload[0] == 3, "FIFO tie");
  order.queue.free(second);
  require(!order.queue.getNextOutbound(1099), "early eligibility");
  auto third = order.queue.getNextOutbound(1100);
  require(third && third->payload[0] == 1, "exact eligibility");
  order.queue.free(third);
  passed();
}

static void arithmetic() {
  scenario = "native-delay-kernels";
  Fixture f;
  mesh::Packet p;
  p.payload_len = 11; p.setPathHashSizeAndCount(3, 2);
  simple_repeater_kernel repeater{{0, 0.5f, 0.3f}, &f.radio, &f.rng};
  simple_room_server_kernel room{{0, 0.5f, 0.2f}, &f.radio, &f.rng};
  companion_radio_kernel companion{{0, 0.5f, 0.2f}, &f.radio, &f.rng};
  for (uint32_t airtime : {1, 3, 99, 100, 101, 1000, 16777219}) {
    f.radio.airtime = airtime;
    for (uint32_t random : {0u, 1u, 149u, 150u, 250u, 251u, 0xffffffffu}) {
      f.rng.word(random); auto flood = repeater.getRetransmitDelay(&p);
      auto flood_max = f.rng.last_max;
      f.rng.word(random); auto direct = repeater.getDirectRetransmitDelay(&p);
      auto direct_max = f.rng.last_max;
      f.rng.word(random); auto rd = room.getDirectRetransmitDelay(&p);
      auto room_max = f.rng.last_max;
      f.rng.word(random); auto cd = companion.getDirectRetransmitDelay(&p);
      auto companion_max = f.rng.last_max;
      require(rd == cd, "native room/companion defaults diverge");
      if (airtime == 100 && random == 150) require(direct == 150, "inclusive native direct jitter endpoint");
      if (airtime == 100 && random == 250) require(flood == 250, "inclusive native flood jitter endpoint");
      if (airtime == 100 && random == 251) require(flood == 0, "native modulo boundary");
      event("\"event\":\"delay\",\"airtime_ms\":" + std::to_string(airtime) +
            ",\"rng_u32\":" + std::to_string(random) + ",\"flood_ms\":" + std::to_string(flood) +
            ",\"repeater_direct_ms\":" + std::to_string(direct) +
            ",\"room_direct_ms\":" + std::to_string(rd) + ",\"companion_direct_ms\":" + std::to_string(cd) +
            ",\"rng_min\":" + std::to_string(f.rng.last_min) + ",\"flood_max_exclusive\":" + std::to_string(flood_max) +
            ",\"repeater_direct_max_exclusive\":" + std::to_string(direct_max) +
            ",\"room_direct_max_exclusive\":" + std::to_string(room_max) +
            ",\"companion_direct_max_exclusive\":" + std::to_string(companion_max));
    }
  }
  for (float base : {0.0f, 1.0f, 2.0f, 10.0f}) {
    repeater._prefs.rx_delay_base = base;
    for (float score : {0.0f, 0.25f, 0.85f, 1.0f}) {
      auto delay = repeater.calcRxDelay(score, 100);
      if (base == 0 || base == 1) require(delay == 0, "disabled/identity RX delay");
      event("\"event\":\"rx_delay\",\"base\":" + std::to_string(base) +
            ",\"score\":" + std::to_string(score) + ",\"airtime_ms\":100,\"delay_ms\":" + std::to_string(delay));
    }
  }
  repeater._prefs.rx_delay_base = 10;
  struct RXInput { float score; uint32_t airtime; };
  for (auto input : {RXInput{-0.15f,123}, RXInput{1.85f,1000},
                     RXInput{0,1000}, RXInput{0.5f,1000}, RXInput{1,1000}}) {
    auto delay = repeater.calcRxDelay(input.score, input.airtime);
    event("\"event\":\"rx_delay\",\"base\":10,\"score\":" + std::to_string(input.score) +
          ",\"airtime_ms\":" + std::to_string(input.airtime) + ",\"delay_ms\":" + std::to_string(delay));
    if (input.score == -0.15f) require(delay == 1107, "float32 positive exponent boundary");
    if (input.score == 1.85f) require(delay == -899, "native float pow result before double subtraction");
  }
  passed();
  scenario = "snr-score-kernel";
  for (int sf = 7; sf <= 12; ++sf) {
    for (int len : {2,64,128,255}) {
      float previous = -1;
      for (float snr : {-30.0f,-20.0f,-15.0f,-10.0f,-7.5f,0.0f,10.0f,30.0f}) {
        float score = nativePacketScore(snr, sf, len);
        require(score >= previous && score >= 0 && score <= 1, "SNR score ordering/clamp");
        previous = score;
        event("\"event\":\"score\",\"snr\":" + std::to_string(snr) + ",\"sf\":" + std::to_string(sf) +
              ",\"length\":" + std::to_string(len) + ",\"score\":" + std::to_string(score));
      }
    }
  }
  require(nativePacketScore(-7.5f, 7, 64) == 0, "SF7 threshold");
  require(nativePacketScore(2.5f, 7, 64) == 0.75f, "score known boundary");
  float requested_score = nativePacketScore(-2.5f, 7, 128);
  require(requested_score == 0.25f, "SF7 length128 requested score");
  event("\"event\":\"score\",\"snr\":-2.5,\"sf\":7,\"length\":128,\"score\":" +
        std::to_string(requested_score));
  passed();
  scenario = "core-rng-semantics";
  f.radio.airtime = 101;
  f.rng.word(4);
  require(f.core.getRetransmitDelay(&p) == 208, "core integer arithmetic");
  f.rng.word(3);
  require(f.core.getRetransmitDelay(&p) == 156, "core RNG endpoint");
  require(f.core.getDirectRetransmitDelay(&p) == 0, "core direct default");
  for (uint32_t n : {0,1,2,3}) {
    f.rng.word(n);
    auto delay = f.core.getCADFailRetryDelay();
    event("\"event\":\"cad_retry\",\"rng_u32\":" + std::to_string(n) + ",\"delay_ms\":" + std::to_string(delay));
    require(delay == (n == 0 || n == 3 ? 120u : n == 1 ? 240u : 360u), "CAD jitter");
  }
  passed();
}

static void dispatcher() {
  scenario = "dispatcher-rx-holds";
  Fixture f;
  for (float score : {1.0f,0.7f,0.0f}) {
    f.radio.score = score;
    size_t before = f.queue.holds.size();
    f.radio.rx.push_back({uint8_t(ROUTE_TYPE_FLOOD | (PAYLOAD_TYPE_RAW_CUSTOM << PH_TYPE_SHIFT)), 0, 7});
    f.clock.now += 10; f.core.loop();
    if (score > 0.5f) require(f.queue.holds.size() == before, "below50 RX hold");
    else require(f.queue.holds.size() == before+1, "weak RX not held");
  }
  f.radio.airtime = 10000;
  f.radio.rx.push_back({uint8_t(ROUTE_TYPE_FLOOD | (PAYLOAD_TYPE_RAW_CUSTOM << PH_TYPE_SHIFT)), 0, 8});
  f.clock.now += 10; f.core.loop();
  require(f.queue.holds.back() == f.clock.now+32000, "RX hold cap");
  passed();
  scenario = "dispatcher-budget";
  Fixture budget;
  require(budget.core.getRemainingTxBudget() == 500, "native startup credit");
  budget.core.sendZeroHop(budget.packet(PAYLOAD_TYPE_RAW_CUSTOM), uint32_t(0));
  budget.step(1000);
  require(budget.radio.sent.empty(), "native next_tx strictly after timestamp");
  budget.step(1001);
  require(budget.radio.sent.size() == 1, "start eligible packet");
  budget.radio.complete = true;
  budget.step(1401);
  require(budget.core.getTotalAirTime() == 400 && budget.core.getRemainingTxBudget() == 100,
          "completion accounting/refill cap");
  event("\"event\":\"budget\",\"at_ms\":1401,\"remaining_ms\":100,\"actual_total_ms\":400");
  // Native admission uses half the maximum-packet estimate, not this packet's estimate.
  budget.radio.airtime = 400;
  budget.core.sendZeroHop(budget.packet(PAYLOAD_TYPE_RAW_CUSTOM, 2), uint32_t(0));
  budget.step(1402);
  require(budget.radio.sent.size() == 1, "insufficient budget starts TX");
  budget.step(1602);
  require(budget.radio.sent.size() == 1, "native budget deadline is strict");
  budget.step(1603);
  require(budget.radio.sent.size() == 2, "refilled budget cannot start");
  passed();
  scenario = "dispatcher-cad-failure";
  Fixture cad;
  cad.radio.busy = true;
  cad.core.sendZeroHop(cad.packet(PAYLOAD_TYPE_RAW_CUSTOM), uint32_t(0));
  cad.step(1001);
  require(cad.radio.sent.empty(), "CAD busy ignored");
  cad.step(1122);
  require(cad.radio.sent.empty(), "CAD retry starts while busy");
  cad.step(5002);
  require(cad.radio.sent.size() == 1 && (cad.core.errors() & ERR_EVENT_CAD_TIMEOUT), "native CAD max busy");
  Fixture base_cad;
  base_cad.core.dispatcher_cad = true;
  require(base_cad.core.getCADFailRetryDelay() == 200 && base_cad.core.cadLimit() == 4000,
          "base Dispatcher CAD policy");
  event("\"event\":\"cad_policy\",\"profile\":\"Dispatcher\",\"retry_ms\":200,\"max_busy_ms\":4000");
  base_cad.radio.busy = true;
  base_cad.core.sendZeroHop(base_cad.packet(PAYLOAD_TYPE_RAW_CUSTOM), uint32_t(0));
  base_cad.step(1001);
  base_cad.step(1201);
  require(base_cad.radio.sent.empty(), "base CAD retry equality");
  base_cad.step(1202);
  base_cad.step(5001);
  require(base_cad.radio.sent.empty(), "base CAD forces at exactly4000");
  base_cad.step(5201);
  require(base_cad.radio.sent.empty(), "base CAD pending retry equality");
  base_cad.step(5202);
  require(base_cad.radio.sent.size() == 1 && (base_cad.core.errors() & ERR_EVENT_CAD_TIMEOUT),
          "base CAD does not force after4000");
  passed();
  scenario = "dispatcher-tx-failures";
  Fixture failure;
  failure.radio.start_ok = false;
  failure.core.sendZeroHop(failure.packet(PAYLOAD_TYPE_RAW_CUSTOM), uint32_t(0));
  failure.step(1001);
  require(failure.queue.getFreeCount() == 32 && failure.core.getTotalAirTime() == 0, "start failure ownership");
  failure.radio.start_ok = true;
  failure.core.sendZeroHop(failure.packet(PAYLOAD_TYPE_RAW_CUSTOM, 2), uint32_t(0));
  failure.step(1002);
  failure.step(1153);
  require(!failure.radio.sending && failure.queue.getFreeCount() == 32, "TX timeout releases packet");
  require(failure.core.getTotalAirTime() == 0, "native failed TX charging changed");
  event("\"event\":\"reference_exception\",\"native_failed_airtime_ms\":0,\"host_contract\":\"conservative_failed_occupancy\"");
  passed();
}

static void routing() {
  scenario = "routing-policy-helpers";
  mesh::Packet p;
  for (auto route : {ROUTE_TYPE_FLOOD, ROUTE_TYPE_TRANSPORT_FLOOD}) {
    p.header = route | (PAYLOAD_TYPE_RESPONSE << PH_TYPE_SHIFT);
    for (uint8_t hops : {0,2,3,8}) {
      p.setPathHashSizeAndCount(3, hops);
      auto denied = mesh::isFloodHopLimitExceeded(&p, 8, 3, 2);
      require(denied == (hops >= 8 || (route == ROUTE_TYPE_FLOOD && hops >= 3)), "hop limit policy");
      event("\"event\":\"hop_admission\",\"route\":" + std::to_string(route) + ",\"hops\":" +
            std::to_string(hops) + ",\"denied\":" + (denied ? "true" : "false"));
    }
  }
  for (int flags = 0; flags < 8; ++flags) {
    auto route = mesh::chooseReplyRoute(flags&1, flags&2, flags&4);
    auto scope = mesh::chooseReplyScope(flags&1, flags&2, flags&4);
    event("\"event\":\"reply\",\"request_flags\":" + std::to_string(flags) +
          ",\"route\":" + std::to_string(route) + ",\"scope\":" + std::to_string(scope));
  }
  require(mesh::chooseReplyScope(false, true, true) == mesh::REPLY_SCOPE_NONE, "unscoped mirroring");
  require(mesh::chooseReplyRoute(false, false, true) == mesh::REPLY_ROUTE_DIRECT_OUT_PATH, "stored return route");
  passed();
  scenario = "millis-wrap";
  if (sizeof(unsigned long) != 4) {
    event("\"event\":\"blocked\",\"reason\":\"requires_32_bit_unsigned_long\"");
    return;
  }
  Fixture f;
  f.clock.now = 0xfffffff0UL;
  auto target = f.core.futureMillis(32);
  require(target == 16, "32-bit future wraps");
  f.clock.now = 15; require(!f.core.millisHasNowPassed(target), "wrap early");
  f.clock.now = 16; require(!f.core.millisHasNowPassed(target), "wrap exact");
  f.clock.now = 17; require(f.core.millisHasNowPassed(target), "wrap passed");
  f.clock.now = 0xfffffff0UL;
  f.core.sendPacket(f.packet(PAYLOAD_TYPE_RAW_CUSTOM), 1, 32);
  require(!f.queue.getNextOutbound(0xfffffff5UL), "queue eligible before wrap");
  require(!f.queue.getNextOutbound(15), "queue eligible before wrapped deadline");
  auto due = f.queue.getNextOutbound(16);
  require(due != nullptr, "queue missed exact wrapped deadline");
  f.queue.free(due);
  passed();
}

static void aclVectors() {
  scenario = "client-acl-vectors";
  auto entries = [](ClientACL& acl) {
    std::string result = "[";
    for (int i = 0; i < acl.getNumClients(); ++i) {
      auto c = acl.getClientByIdx(i);
      if (i) result += ",";
      result += "{\"index\":" + std::to_string(i) +
                ",\"public_key\":\"" + hex(c->id.pub_key,32) +
                "\",\"permissions\":" + std::to_string(c->permissions) +
                ",\"last_activity\":" + std::to_string(c->last_activity) +
                ",\"is_admin\":" + (c->isAdmin() ? "true" : "false") + "}";
    }
    return result + "]";
  };
  for (const std::string name : {"raw-permissions","least-active","equal-activity","all-admin-exception"}) {
    Fixture f;
    MemoryFS fs;
    ClientACL acl;
    acl.load(&fs, f.core.self_id);
    bool raw = name == "raw-permissions";
    bool tied = name == "equal-activity";
    bool all_admin = name == "all-admin-exception";
    int count = raw ? 4 : MAX_CLIENTS;
    require(MAX_CLIENTS >= 4, "ACL vectors require at least four slots");
    std::vector<mesh::Identity> identities;
    for (int i = 0; i < count; ++i) {
      f.rng.word(100+i);
      identities.emplace_back(mesh::LocalIdentity(&f.rng));
    }
    if (tied) {
      std::sort(identities.begin(),identities.end(),[](const mesh::Identity& a, const mesh::Identity& b) {
        return memcmp(a.pub_key,b.pub_key,32) > 0;
      });
    }
    for (int i = 0; i < count; ++i) {
      uint8_t permissions = raw ? 0x80+i : all_admin || i == 0 ? 0x83 : 0x80+(i%3);
      auto c = acl.putClient(identities[i], permissions);
      require(c != nullptr && c->permissions == permissions, "raw ACL permission admission");
      require(c->isAdmin() == (raw ? i == 3 : all_admin || i == 0), "upper permission bits alter admin classification");
      c->last_activity = raw ? 10+i : i == 0 ? 0 : tied ? 100 : i == 2 ? 1 : 100+i;
    }
    auto inputs = entries(acl);
    std::string admission = "null";
    int victim = -1;
    if (!raw) {
      f.rng.word(1234);
      mesh::LocalIdentity incoming(&f.rng);
      admission = "{\"public_key\":\"" + hex(incoming.pub_key,32) +
                  "\",\"permissions\":130,\"last_activity\":1000}";
      auto added = acl.putClient(incoming,0x82);
      require(added != nullptr, "native full-table admission");
      added->last_activity = 1000;
      victim = all_admin ? MAX_CLIENTS-1 : tied ? 1 : 2;
      require(acl.getNumClients() == count, "ACL admission changes full capacity");
      for (int i = 0; i < count; ++i) {
        auto c = acl.getClientByIdx(i);
        require(c->id.matches(i == victim ? incoming : identities[i]), "native ACL eviction/member order");
      }
      require(acl.getClient(identities[victim].pub_key,32) == nullptr, "ACL victim remains present");
      auto older_admin = acl.getClient(identities[0].pub_key,32);
      require(older_admin && older_admin->isAdmin(), "older administrator was evicted");
    }
    auto result = entries(acl);
    acl.save(&fs);
    const auto& stored = fs.volume->files.at("/s_contacts");
    require(stored.size() == size_t(count)*136, "native ACL storage record size");
    std::vector<uint8_t> stored_permissions;
    for (int i = 0; i < count; ++i) {
      stored_permissions.push_back(stored[size_t(i)*136+32]);
      require(stored_permissions.back() == acl.getClientByIdx(i)->permissions, "ACL storage masks upper bits");
    }
    ClientACL restored;
    restored.load(&fs,f.core.self_id);
    require(restored.getNumClients() == count, "unfiltered save drops nonzero raw guest permission");
    for (int i = 0; i < count; ++i) {
      auto c = acl.getClientByIdx(i);
      auto loaded = restored.getClientByIdx(i);
      require(loaded->id.matches(c->id) && loaded->permissions == c->permissions &&
              loaded->isAdmin() == c->isAdmin() && loaded->last_activity == 0,
              "ACL persistence/order/transient activity");
    }
    MemoryFS admin_fs;
    acl.save(&admin_fs,[](ClientInfo* c) { return c->isAdmin(); });
    ClientACL admins;
    admins.load(&admin_fs,f.core.self_id);
    int admin_count = 0;
    for (int i = 0; i < count; ++i) {
      auto c = acl.getClientByIdx(i);
      if (!c->isAdmin()) continue;
      auto loaded = admins.getClientByIdx(admin_count++);
      require(loaded->id.matches(c->id) && loaded->permissions == c->permissions,
              "admin filter loses raw permission/order");
    }
    require(admins.getNumClients() == admin_count, "admin persistence filter count");
    event("\"event\":\"acl_case\",\"case\":\"" + name + "\",\"capacity\":" +
          std::to_string(MAX_CLIENTS) + ",\"insertion_api\":\"putClient\",\"input_clients\":" + inputs +
          ",\"admission\":" + admission + ",\"evicted_input_index\":" + std::to_string(victim) +
          ",\"result_clients\":" + result + ",\"save_filter\":\"none\",\"persisted_permissions_hex\":\"" +
          hex(stored_permissions.data(),stored_permissions.size()) + "\",\"reloaded_clients\":" +
          entries(restored) + ",\"admin_filtered_clients\":" + entries(admins) +
          ",\"comparison\":\"" + (all_admin ? "adopted-host-rejection-exception" : "helper-membership-and-order") +
          "\",\"expected_host_admission\":\"" + (all_admin ? "reject-without-mutation" : raw ? "not-requested" : "accept") + "\"");
  }
  passed();
}

static void aclAndRegions() {
  scenario = "acl-storage-eviction";
  Fixture f;
  MemoryFS fs;
  ClientACL acl;
  acl.load(&fs, f.core.self_id);
  std::vector<mesh::Identity> identities;
  for (int i = 0; i < MAX_CLIENTS; ++i) {
    f.rng.word(i+1);
    identities.emplace_back(mesh::LocalIdentity(&f.rng));
    auto c = acl.putClient(identities.back(), PERM_ACL_ADMIN);
    c->last_activity = i+1;
    c->extra.room.sync_since = 1234;
  }
  auto victim = identities.back();
  f.rng.word(1234);
  mesh::LocalIdentity extra(&f.rng);
  require(acl.putClient(extra, PERM_ACL_READ_WRITE) != nullptr, "native admission");
  require(acl.getClient(victim.pub_key, 32) == nullptr, "native all-admin overwrite changed");
  event("\"event\":\"reference_exception\",\"native\":\"last_admin_evicted\",\"safe_host_outcome\":\"reject_admission\"");
  acl.save(&fs, [](ClientInfo* c) { return c->isAdmin(); });
  ClientACL restored;
  restored.load(&fs, f.core.self_id);
  require(restored.getNumClients() == MAX_CLIENTS-1, "admin-only save filter");
  require(restored.getClientByIdx(0)->last_activity == 0 &&
          restored.getClientByIdx(0)->extra.room.sync_since == 1234, "persistent/transient fields");
  event("\"event\":\"restore\",\"admins\":" + std::to_string(restored.getNumClients()) +
        ",\"sync_since\":1234,\"last_activity\":0");
  fs.volume->write_limit = 10;
  acl.save(&fs);
  ClientACL truncated;
  truncated.load(&fs, f.core.self_id);
  require(truncated.getNumClients() == 0, "truncated contact accepted");
  event("\"event\":\"storage_failure\",\"native_save_result\":\"void\",\"restored_contacts\":0");
  passed();
  scenario = "region-keys-storage";
  MemoryFS regionsFS;
  TransportKeyStore keys;
  RegionMap regions(keys);
  auto region = regions.putRegion("TestRegion", 0);
  require(region != nullptr, "region creation");
  require(region->flags & REGION_DENY_FLOOD, "new region must default deny");
  region->flags = 0;
  regions.setDefaultRegion(region);
  regions.setHomeRegion(region);
  TransportKey key;
  require(regions.getTransportKeysFor(*region, &key, 1) == 1, "region key derivation");
  mesh::Packet packet;
  packet.header = ROUTE_TYPE_TRANSPORT_FLOOD | (PAYLOAD_TYPE_RESPONSE << PH_TYPE_SHIFT);
  packet.payload_len = 5; memcpy(packet.payload, "hello", 5);
  auto code = key.calcTransportCode(&packet);
  packet.transport_codes[0] = packet.transport_codes[1] = code;
  require(regions.findMatch(&packet, REGION_DENY_FLOOD) == region, "matching region code");
  region->flags = REGION_DENY_FLOOD;
  require(regions.findMatch(&packet, REGION_DENY_FLOOD) == nullptr, "denied region admitted");
  region->flags = 0;
  require(regions.save(&regionsFS), "save regions");
  RegionMap restoredRegions(keys);
  require(restoredRegions.load(&regionsFS), "load regions");
  require(restoredRegions.getDefaultRegion() && restoredRegions.getHomeRegion(), "scope persistence");
  RegionMap hierarchy(keys);
  auto can = hierarchy.putRegion("can", 0);
  auto ab = hierarchy.putRegion("ab", can->id);
  auto edm = hierarchy.putRegion("edm", ab->id);
  require(can && ab && edm, "can/ab/edm creation");
  can->flags = ab->flags = edm->flags = 0;
  hierarchy.setHomeRegion(ab);
  require(hierarchy.getDefaultRegion() == nullptr, "home unexpectedly changes default scope");
  require(hierarchy.findByName("AB") == nullptr && hierarchy.findByName("#ab") == ab,
          "case-sensitive region names and optional hash");
  TransportKey edmKey;
  require(hierarchy.getTransportKeysFor(*edm, &edmKey, 1) == 1, "edm public key");
  packet.transport_codes[0] = edmKey.calcTransportCode(&packet);
  require(hierarchy.findMatch(&packet, REGION_DENY_FLOOD) == edm, "edm scoped admission");
  edm->flags = REGION_DENY_FLOOD;
  require(hierarchy.findMatch(&packet, REGION_DENY_FLOOD) == nullptr, "ab permission inherited by edm");
  MemoryFS hierarchyFS;
  require(hierarchy.save(&hierarchyFS), "save can/ab/edm");
  RegionMap restoredHierarchy(keys);
  require(restoredHierarchy.load(&hierarchyFS) &&
          !strcmp(restoredHierarchy.getHomeRegion()->name, "ab") &&
          restoredHierarchy.findByName("edm")->parent == restoredHierarchy.findByName("ab")->id &&
          restoredHierarchy.findByName("ab")->parent == restoredHierarchy.findByName("can")->id,
          "home and hierarchy restart persistence");
  event("\"event\":\"region\",\"name\":\"TestRegion\",\"key\":\"" + hex(key.key, 16) +
        "\",\"transport_code\":" + std::to_string(code));
  regionsFS.volume->write_limit = 3;
  require(!regions.save(&regionsFS), "short region write reported success");
  TransportKey known;
  keys.getAutoKeyFor(123, "#test", known);
  packet.header = ROUTE_TYPE_TRANSPORT_FLOOD | (2 << PH_TYPE_SHIFT);
  packet.payload_len = 3;
  packet.payload[0] = 1; packet.payload[1] = 2; packet.payload[2] = 3;
  event("\"event\":\"transport_code\",\"name\":\"#test\",\"key\":\"" + hex(known.key,16) +
        "\",\"payload_type\":2,\"payload\":\"010203\",\"code\":" +
        std::to_string(known.calcTransportCode(&packet)));
  TransportKey private_keys[2] = {known, key};
  require(!keys.saveKeysFor(999, private_keys, 2), "native private keystore save changed");
  require(keys.loadKeysFor(999, private_keys, 2) == 0, "native private keystore load changed");
  event("\"event\":\"reference_limitation\",\"feature\":\"private_multi_key_region\","
        "\"reason\":\"TransportKeyStore_saveKeysFor_is_unimplemented_and_returns_false\"");
  passed();
}

static void wireCryptoAndRelay() {
  scenario = "encrypted-wire-vectors";
  Fixture f;
  f.rng.word(1);
  mesh::LocalIdentity peer(&f.rng);
  uint8_t secret[32], reciprocal[32], decrypted[MAX_PACKET_PAYLOAD];
  f.core.self_id.calcSharedSecret(secret, peer);
  peer.calcSharedSecret(reciprocal, f.core.self_id);
  require(memcmp(secret, reciprocal, 32) == 0, "native ECDH reciprocity");
  const uint8_t plaintext[] = {0x00,0xf1,0x53,0x65,0x00,'h','e','l','l','o'};
  auto packet = f.core.createDatagram(PAYLOAD_TYPE_TXT_MSG, peer, secret, plaintext, sizeof(plaintext));
  require(packet != nullptr, "create encrypted datagram");
  auto count = mesh::Utils::MACThenDecrypt(secret, decrypted, packet->payload+2, packet->payload_len-2);
  require(count >= int(sizeof(plaintext)) && memcmp(decrypted, plaintext, sizeof(plaintext)) == 0, "native decrypt/MAC");
  event("\"event\":\"datagram_inputs\",\"sender_seed\":\"" + std::string(64,'0') +
        "\",\"recipient_seed\":\"0100000001000000010000000100000001000000010000000100000001000000\","
        "\"secret\":\"" + hex(secret,32) + "\",\"plaintext\":\"" + hex(plaintext,sizeof(plaintext)) + "\"");
  uint8_t path[] = {1,2,3,4,5,6};
  f.core.sendDirect(packet, path, 0x82);
  uint8_t raw[255];
  auto len = packet->writeTo(raw);
  event("\"event\":\"encrypted_packet\",\"bytes\":\"" + hex(raw,len) + "\"");
  packet->payload[2] ^= 1;
  require(mesh::Utils::MACThenDecrypt(secret, decrypted, packet->payload+2, packet->payload_len-2) == 0,
          "tampered native MAC accepted");
  packet->payload[2] ^= 1;
  auto advert = f.core.createAdvert(f.core.self_id);
  require(advert != nullptr, "create advert");
  uint8_t signed_message[36];
  memcpy(signed_message, advert->payload, 36);
  require(f.core.self_id.verify(advert->payload+36, signed_message, 36), "native advert signature");
  f.core.sendFlood(advert, uint32_t(0), 3);
  passed();
  scenario = "direct-path-consumption-dedup";
  for (uint8_t width : {1,2,3}) {
    Fixture relay;
    mesh::Packet incoming;
    incoming.header = ROUTE_TYPE_DIRECT | (PAYLOAD_TYPE_RAW_CUSTOM << PH_TYPE_SHIFT);
    incoming.setPathHashSizeAndCount(width, 2);
    memcpy(incoming.path, relay.core.self_id.pub_key, width);
    memset(incoming.path+width, 0x77, width);
    incoming.payload_len = 1; incoming.payload[0] = width;
    uint8_t encoded[255];
    auto size = incoming.writeTo(encoded);
    std::vector<uint8_t> bytes(encoded, encoded+size);
    relay.radio.rx.push_back(bytes);
    relay.step(1001);
    require(relay.queue.events.size() == 1 && relay.queue.events[0].priority == 0, "direct relay priority");
    require(relay.radio.sent.size() == 1, "direct relay not sent");
    mesh::Packet result;
    require(result.readFrom(relay.radio.sent[0].data(), relay.radio.sent[0].size()), "direct relay parse");
    require(result.getPathHashCount() == 1 && result.getPathHashSize() == width &&
            result.path[0] == 0x77, "wrong direct path consumed");
    relay.radio.complete = true;
    relay.step(1010);
    relay.radio.rx.push_back(bytes);
    relay.step(1011);
    require(relay.radio.sent.size() == 1, "direct duplicate retransmitted");
  }
  passed();
}

class Chat : public BaseChatMesh {
public:
  bool overwrite = false;
  int overwritten = 0;
  int messages = 0;
  int signed_messages = 0;
  uint8_t extra_acks = 0;
  Chat(Radio& r, Clock& c, RNG& rng, RTC& rtc, Queue& q, SimpleMeshTables& tables)
    : BaseChatMesh(r,c,rng,rtc,q,tables) {}
protected:
  uint8_t getExtraAckTransmitCount() const override { return extra_acks; }
  bool shouldOverwriteWhenFull() const override { return overwrite; }
  void onContactOverwrite(const uint8_t*) override { ++overwritten; }
  void onDiscoveredContact(ContactInfo&, bool, uint8_t, const uint8_t*) override {}
  ContactInfo* processAck(const uint8_t*) override { return nullptr; }
  void onContactPathUpdated(const ContactInfo&) override {}
  void onMessageRecv(const ContactInfo&, mesh::Packet*, uint32_t, const char*) override { ++messages; }
  void onCommandDataRecv(const ContactInfo&, mesh::Packet*, uint32_t, const char*) override {}
  void onSignedMessageRecv(const ContactInfo&, mesh::Packet*, uint32_t, const uint8_t*, const char*) override {
    ++signed_messages;
  }
  uint32_t calcFloodTimeoutMillisFor(uint32_t) const override { return 0; }
  uint32_t calcDirectTimeoutMillisFor(uint32_t, uint8_t) const override { return 0; }
  void onSendTimeout() override {}
  void onChannelMessageRecv(const mesh::GroupChannel&, mesh::Packet*, uint32_t, const char*) override {}
  uint8_t onContactRequest(const ContactInfo&, uint32_t, const uint8_t*, uint8_t, uint8_t*) override { return 0; }
  void onContactResponse(const ContactInfo&, const uint8_t*, uint8_t) override {}
};

static void baseChat() {
  scenario = "base-chat-contact-capacity";
  Fixture f;
  Chat chat(f.radio,f.clock,f.rng,f.rtc,f.queue,f.tables);
  std::vector<mesh::Identity> ids;
  for (int i = 0; i < MAX_CONTACTS; ++i) {
    f.rng.word(i+1);
    ids.emplace_back(mesh::LocalIdentity(&f.rng));
    ContactInfo c{};
    c.id = ids.back(); c.type = ADV_TYPE_CHAT; c.lastmod = i+1;
    c.flags = i == 0 ? 1 : 0;
    require(chat.addContact(c), "native contact capacity");
  }
  f.rng.word(54321);
  ContactInfo extra{};
  extra.id = mesh::LocalIdentity(&f.rng); extra.type = ADV_TYPE_CHAT; extra.lastmod = 1000;
  require(!chat.addContact(extra), "full contact table accepted without overwrite");
  chat.overwrite = true;
  require(chat.addContact(extra) && chat.overwritten == 1, "overwrite enabled");
  require(chat.lookupContactByPubKey(ids[0].pub_key,32) != nullptr, "favourite overwritten");
  require(chat.lookupContactByPubKey(ids[1].pub_key,32) == nullptr, "oldest non-favourite retained");
  event("\"event\":\"contact_eviction\",\"capacity\":" + std::to_string(MAX_CONTACTS) +
        ",\"favourite_survives\":true,\"oldest_non_favourite_evicted\":true");
  passed();
}

static void baseChatAck() {
  scenario = "base-chat-wire-ack-timing";
  for (int mode = 0; mode < 4; ++mode) {
    Fixture sender, receiver;
    Chat chat(receiver.radio,receiver.clock,receiver.rng,receiver.rtc,receiver.queue,receiver.tables);
    receiver.rng.word(2);
    chat.self_id = mesh::LocalIdentity(&receiver.rng);
    chat.extra_acks = mode == 1 ? 1 : 0;
    chat.begin();
    ContactInfo from{};
    from.id = sender.core.self_id; from.type = ADV_TYPE_CHAT;
    from.out_path_len = mode == 2 ? OUT_PATH_UNKNOWN : 0x81;
    from.out_path[0] = 1; from.out_path[1] = 2; from.out_path[2] = 3;
    require(chat.addContact(from), "add ACK sender");
    uint8_t secret[32];
    sender.core.self_id.calcSharedSecret(secret, chat.self_id);
    const uint8_t plaintext[] = {0,0xf1,0x53,0x65,0,'h','e','l','l','o'};
    auto packet = sender.core.createDatagram(PAYLOAD_TYPE_TXT_MSG, chat.self_id,
                                            secret, plaintext, sizeof(plaintext));
    require(packet != nullptr, "ACK test input");
    packet->header |= mode == 3 ? ROUTE_TYPE_FLOOD : ROUTE_TYPE_DIRECT;
    if (mode == 3) {
      packet->setPathHashSizeAndCount(3,1);
      packet->path[0] = 0x90; packet->path[1] = 0x91; packet->path[2] = 0x92;
    }
    uint8_t raw[255]; auto size = packet->writeTo(raw);
    event("\"event\":\"ack_input\",\"mode\":" + std::to_string(mode) +
          ",\"clock_ms\":1000,\"recipient_rng_word\":2,\"extra_acks\":" +
          std::to_string(chat.extra_acks) + ",\"known_return_path\":" +
          (mode == 2 ? "false" : "true") +
          ",\"return_path_encoded\":" + std::to_string(from.out_path_len) +
          ",\"return_path\":\"" + (mode == 2 ? std::string() : hex(from.out_path,3)) +
          "\",\"sender_seed\":\"" + std::string(64,'0') +
          "\",\"recipient_seed\":\"0200000002000000020000000200000002000000020000000200000002000000\""
          ",\"sender_public_key\":\"" + hex(sender.core.self_id.pub_key,32) +
          "\",\"recipient_public_key\":\"" + hex(chat.self_id.pub_key,32) +
          "\",\"shared_secret\":\"" + hex(secret,32) +
          "\",\"ack_payload_nonce_index\":5,\"bytes\":\"" + hex(raw,size) + "\"");
    receiver.radio.rx.emplace_back(raw, raw+size);
    chat.loop();
    require(chat.messages == 1, "native plaintext receive callback");
    auto& queued = receiver.queue.events;
    require(queued.size() == (mode == 1 ? 2u : 1u), "ACK count");
    require(queued[0].at == 1200, "native ACK200ms eligibility");
    require(queued[0].priority == (mode == 3 ? 2 : mode == 2 ? 1 : 0), "native ACK/PATH priority");
    if (mode == 1) require(queued[1].at == 1500 && queued[1].priority == 0, "extra ACK300ms spacing");
    require(receiver.radio.sent.empty(), "ACK sent before eligibility");
  }
  passed();
}

static void baseChatAckHashes() {
  scenario = "base-chat-ack-hashes";
  for (bool signed_message : {false,true}) {
    std::string previous;
    for (uint8_t attempt : {3,9}) {
      Fixture sender, receiver;
      Chat chat(receiver.radio,receiver.clock,receiver.rng,receiver.rtc,receiver.queue,receiver.tables);
      receiver.rng.word(2);
      chat.self_id = mesh::LocalIdentity(&receiver.rng);
      chat.begin();
      ContactInfo from{};
      from.id = sender.core.self_id; from.type = ADV_TYPE_CHAT; from.out_path_len = 0x81;
      from.out_path[0] = 1; from.out_path[1] = 2; from.out_path[2] = 3;
      require(chat.addContact(from), "add ACK hash sender");
      uint8_t secret[32];
      sender.core.self_id.calcSharedSecret(secret, chat.self_id);
      std::vector<uint8_t> plain{0,0xf1,0x53,0x65,
                                uint8_t(signed_message ? TXT_TYPE_SIGNED_PLAIN << 2 : 0)};
      if (signed_message) plain.insert(plain.end(), {0x11,0x22,0x33,0x44});
      plain.insert(plain.end(), {'h','e','l','l','o',0,attempt});
      auto packet = sender.core.createDatagram(PAYLOAD_TYPE_TXT_MSG, chat.self_id,
                                              secret, plain.data(), plain.size());
      require(packet != nullptr, "ACK hash datagram");
      packet->header |= ROUTE_TYPE_DIRECT;
      uint8_t raw[255]; auto size = packet->writeTo(raw);
      receiver.radio.rx.emplace_back(raw, raw+size);
      chat.loop();
      require(signed_message ? chat.signed_messages == 1 : chat.messages == 1, "ACK hash receive callback");
      require(receiver.queue.events.size() == 1, "ACK hash output count");
      auto& queued = receiver.queue.events[0];
      auto bytes = unhex(queued.bytes.c_str());
      mesh::Packet ack;
      require(ack.readFrom(bytes.data(), bytes.size()) && ack.getPayloadType() == PAYLOAD_TYPE_ACK,
              "ACK hash frame");
      require(ack.payload_len == (signed_message ? 4 : 6) && queued.priority == 0 && queued.at == 1200,
              "ACK hash shape/timing");
      if (!signed_message) require(ack.payload[4] == attempt, "plain extended attempt echo");
      auto hash = hex(ack.payload, 4);
      require(previous.empty() || previous == hash, "extended attempt changes ACK hash");
      previous = hash;
      event("\"event\":\"ack_hash\",\"signed\":" + std::string(signed_message ? "true" : "false") +
            ",\"attempt_tail\":" + std::to_string(attempt) + ",\"clock_ms\":1000,\"recipient_rng_word\":2,"
            "\"plaintext\":\"" + hex(plain.data(), plain.size()) +
            "\",\"sender_public_key\":\"" + hex(sender.core.self_id.pub_key,32) +
            "\",\"recipient_public_key\":\"" + hex(chat.self_id.pub_key,32) +
            "\",\"ack_hash\":\"" + hash + "\",\"ack_payload\":\"" + hex(ack.payload,ack.payload_len) + "\"");
    }
  }
  passed();
}

static void roomBoundaries() {
  scenario = "room-post-copy-boundary";
  for (size_t length : {149u,150u,151u,152u}) {
    std::string input(length, 'a');
    char output[152];
    memset(output, '!', sizeof(output));
    StrHelper::strncpy(output, input.c_str(), 151);
    require(strlen(output) == (length == 149 ? 149 : 150) && output[151] == '!',
            "room storePost buffer-size boundary");
    event("\"event\":\"post_copy\",\"buffer_size\":151,\"input_bytes\":" +
          std::to_string(length) + ",\"retained_bytes\":" + std::to_string(strlen(output)));
  }
  std::string input = std::string(149, 'a') + "\xc3\xa9";
  char output[152] = {};
  StrHelper::strncpy(output, input.c_str(), 151);
  require(strlen(output) == 150 && uint8_t(output[149]) == 0xc3, "native copy is byte-oriented");
  event("\"event\":\"post_copy_utf8\",\"buffer_size\":151,\"input_hex\":\"" +
        hex(reinterpret_cast<const uint8_t*>(input.data()), input.size()) +
        "\",\"retained_hex\":\"" + hex(reinterpret_cast<const uint8_t*>(output),150) + "\"");
  passed();

  scenario = "response-datagram-capacity";
  Fixture f;
  f.rng.word(1);
  mesh::LocalIdentity peer(&f.rng);
  uint8_t secret[32];
  f.core.self_id.calcSharedSecret(secret, peer);
  for (size_t length : {165u,167u,168u,172u}) {
    std::vector<uint8_t> data(length, 0x5a);
    auto before = f.queue.getFreeCount();
    auto packet = f.core.createDatagram(PAYLOAD_TYPE_RESPONSE, peer, secret, data.data(), data.size());
    require((packet != nullptr) == (length <= 167), "response plaintext capacity");
    event("\"event\":\"response_capacity\",\"plaintext_bytes\":" + std::to_string(length) +
          ",\"accepted\":" + (packet ? "true" : "false") +
          ",\"encrypted_payload_bytes\":" + std::to_string(packet ? packet->payload_len : 0));
    if (packet) {
      uint8_t plain[MAX_PACKET_PAYLOAD];
      auto decoded = mesh::Utils::MACThenDecrypt(secret, plain, packet->payload+2, packet->payload_len-2);
      require(decoded >= int(length) && memcmp(plain,data.data(),length) == 0,
              "capacity-boundary encrypted response roundtrip");
      f.queue.free(packet);
    }
    require(f.queue.getFreeCount() == before, "rejected response leaks packet capacity");
  }
  passed();

  scenario = "cayenne-role-telemetry";
#ifdef PARITY_NO_CAYENNE
  event("\"event\":\"blocked\",\"reason\":\"Unchanged non-Arduino CayenneLPPPolyline.cpp does not compile on ELF32: std::max size_t versus unsigned long\"");
#else
  for (uint32_t millivolts : {4020u,4200u}) {
    CayenneLPP lpp(16);
    require(lpp.addVoltage(1, float(millivolts)/1000.0f) == 4, "voltage serialization failed");
    auto bytes = hex(lpp.getBuffer(),lpp.getSize());
    require(bytes == (millivolts == 4020 ? "01740192" : "017401a3"), "native voltage truncation");
    event("\"event\":\"voltage\",\"millivolts\":" + std::to_string(millivolts) +
          ",\"bytes\":\"" + bytes + "\"");
  }
  CayenneLPP lpp(16);
  require(lpp.addTemperature(2,21.3f) == 4, "temperature serialization failed");
  require(hex(lpp.getBuffer(),lpp.getSize()) == "026700d5", "native temperature multiplication");
  event("\"event\":\"temperature\",\"celsius\":21.3,\"bytes\":\"" +
        hex(lpp.getBuffer(),lpp.getSize()) + "\"");
  passed();
#endif
}

static void loopThresholds() {
  scenario = "repeater-loop-kernel";
  Fixture f;
  RepeaterLoopKernel loop;
  loop.self_id = f.core.self_id;
  const uint8_t* limits[] = {max_loop_minimal, max_loop_moderate, max_loop_strict};
  for (uint8_t mode = 1; mode <= 3; ++mode) {
    for (uint8_t width = 1; width <= 3; ++width) {
      int threshold = limits[mode-1][width];
      for (int occurrences : {threshold-1, threshold, threshold+1}) {
        mesh::Packet p;
        p.header = ROUTE_TYPE_FLOOD | (PAYLOAD_TYPE_RESPONSE << PH_TYPE_SHIFT);
        p.setPathHashSizeAndCount(width, occurrences+2);
        for (int i = 0; i < occurrences+2; ++i) {
          memcpy(p.path+i*width, loop.self_id.pub_key, width);
        }
        // Both non-matches share the prefix but differ at the last hash byte.
        p.path[width-1] ^= 0xff;
        p.path[(occurrences+2)*width-1] ^= 0xff;
        bool looped = loop.isLooped(&p, limits[mode-1]);
        require(looped == (occurrences >= threshold), "native complete-hash loop boundary");
        event("\"event\":\"loop\",\"mode\":" + std::to_string(mode) + ",\"width\":" +
              std::to_string(width) + ",\"encoded\":" + std::to_string(p.path_len) +
              ",\"path\":\"" + hex(p.path,p.getPathByteLen()) + "\",\"self_public_key\":\"" +
              hex(loop.self_id.pub_key,32) + "\",\"occurrences\":" + std::to_string(occurrences) +
              ",\"threshold\":" + std::to_string(threshold) + ",\"looped\":" + (looped ? "true" : "false"));
      }
    }
  }
  passed();
}

static void runtimeAirtimeFactor() {
  scenario = "native-runtime-airtime-factor";
  for (float dutycycle : {1.0f,50.0f,100.0f}) {
    float af = nativeDutycycleAF(dutycycle);
    if (dutycycle == 1) require(af == 99, "native dutycycle1 must permitAF99");
    if (dutycycle == 100) require(af == 0, "native dutycycle100 meansAF0");
    event("\"event\":\"dutycycle_af\",\"dutycycle_percent\":" + std::to_string(dutycycle) +
          ",\"airtime_factor\":" + std::to_string(af));
  }
  for (const char* config : {"af 0","af 1","af 9","af 99","af 99.25"}) {
    Fixture f;
    float af = nativeRuntimeAF(config);
    f.core.airtime_factor = af;
    f.core.window = 3600000;
    f.core.begin();
    auto initial = f.core.getRemainingTxBudget();
    if (af == 0) require(initial == 3600000, "native AF0 startup credit");
    if (af == 99) require(initial == 36000, "native AF99 startup credit");
    uint32_t bits;
    memcpy(&bits,&af,sizeof(bits));
    f.core.sendZeroHop(f.packet(PAYLOAD_TYPE_RAW_CUSTOM), uint32_t(0));
    f.step(1001);
    require(f.radio.sent.size() == 1, "runtime AF packet not sent");
    f.radio.complete = true;
    f.step(1101);
    require(f.core.getTotalAirTime() == 100, "runtime AF actual occupancy");
    event("\"event\":\"runtime_af\",\"config\":\"" + std::string(config) + "\",\"airtime_factor\":" +
          std::to_string(af) + ",\"float32_bits\":" + std::to_string(bits) +
          ",\"window_ms\":3600000,\"initial_credit_ms\":" + std::to_string(initial) +
          ",\"after_tx_credit_ms\":" + std::to_string(f.core.getRemainingTxBudget()) +
          ",\"actual_airtime_ms\":100");
  }
  passed();
}

int main() {
  scenario = "abi-profile";
  event("\"event\":\"abi\",\"long_bits\":" + std::to_string(sizeof(long)*8) +
        ",\"float_eval_method\":" + std::to_string(FLT_EVAL_METHOD) +
        ",\"float_pow_result_bits\":" + std::to_string(sizeof(decltype(pow(0.0f,0.0f)))*8) +
        ",\"millis_wrap_evidence\":\"" + (sizeof(long) == 4 ? std::string("eligible") : std::string("blocked")) + "\"");
  cryptoVectors();
  paths();
  priorities();
  arithmetic();
  dispatcher();
  routing();
  aclVectors();
  aclAndRegions();
  wireCryptoAndRelay();
  baseChat();
  baseChatAck();
  baseChatAckHashes();
  roomBoundaries();
  loopThresholds();
  runtimeAirtimeFactor();
  return 0;
}
