// SPDX-License-Identifier: Apache-2.0
#include "PacketWasm.h"
#include "PacketReports.h"
#include <algorithm>
#include <cmath>
#include "BotWasm.h"
#include "WamrRuntime.h"
#include "support/PacketServiceHost.h"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>
using namespace onchip;
using namespace packet_engine;
static uint64_t clockTime = 0, clockStep = 0;
uint64_t onchipBotVmTestClock() {
  if (clockStep) return clockTime += clockStep;
  return std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}
struct TestHost final : PacketServiceHost {
  Reports reports;
  TestHost() { assert(reports.begin()); }
  void beginReports() override { reports.start(); }
  bool report(uint8_t slot, const void *data, size_t size) override { return reports.stage(slot, data, size); }
  void commitReports() override { reports.commit(); }
  void clearReport(uint8_t slot) override { reports.clear(slot); }
};
static std::string directory;
static void poolStats(const char *phase) {
  mem_alloc_info_t info{};
  assert(wasm_runtime_get_mem_alloc_info(&info));
  if (info.total_free_size > info.total_size || info.highmark_size > info.total_size)
    fprintf(stderr, "invalid pool %s total=%u free=%u high-water=%u\n", phase,
            info.total_size, info.total_free_size, info.highmark_size);
  assert(info.total_free_size <= info.total_size && info.highmark_size <= info.total_size);
}
static std::vector<uint8_t> source(const char *name) {
  std::ifstream input(directory + "/" + name + ".wasm", std::ios::binary);
  assert(input);
  return {std::istreambuf_iterator<char>(input), {}};
}
static void load(PacketWasm &engine) {
  const auto bytes = source("packet-0"); char error[128]{};
  const bool ok = engine.load(bytes.data(), bytes.size(), error, sizeof(error));
  if (!ok) fprintf(stderr, "packet load: %s\n", error);
  assert(ok && engine.loaded() && !engine.stats().error[0]);
  assert(engine.stats().sourceBytes == bytes.size() && engine.stats().linearBytes == 65536 &&
         engine.stats().stackBytes == 8192 && engine.stats().poolBytes == 1024 * 1024);
}
static Metadata metadata(Stage stage = Stage::Receive) {
  Metadata m;
  m.stage = stage; m.source = 3; m.destination = 4; m.payloadType = 5;
  m.generation = 6; m.job = 7; m.rssi = -101; m.snrQuarterDb = -9; m.identity[31] = 0xa5;
  return m;
}
static void attach(Pipeline &pipeline, PacketWasm &engine, uint32_t fuel = 10000, uint32_t us = 20000) {
  assert(pipeline.attach(engine, {"wasm", 255, fuel, us}) == Registration::Attached);
}
static Decision run(Pipeline &pipeline, uint8_t mode, uint8_t (&bytes)[Capacity], uint16_t &length,
                    Metadata m = metadata()) {
  memset(bytes, 0, sizeof(bytes)); bytes[0] = mode; bytes[1] = 2; length = 2;
  return pipeline.process(m, bytes, length, Capacity);
}
static void stages_and_operations() {
  PacketWasm engine; load(engine);
  poolStats("stages load");
  TestHost host; Pipeline pipeline(host); attach(pipeline, engine);
  uint8_t bytes[Capacity]; uint16_t length;
  for (unsigned stage = 0; stage < 8; ++stage) {
    auto m = metadata(Stage(stage)); m.authenticated = stage >= 6;
    assert(run(pipeline, 0, bytes, length, m) == Decision::Continue);
    assert(length == 3 && bytes[0] == 0x70 && bytes[1] == 0x42 && bytes[2] == 0x43);
  }
  assert(run(pipeline, 1, bytes, length) == Decision::Drop && length == 2 && bytes[0] == 1);
  assert(host.sent.size() == 2 && host.sent[0].length == 3 && host.sent[0].priority == 1 &&
         host.sent[0].delayMs == 12 && host.sent[0].expiryMs == 23 &&
         host.sent[1].priority == 2 && host.sent[1].delayMs == 13 && host.sent[1].expiryMs == 24);
  host.sent.clear(); host.accept = false;
  assert(run(pipeline, 1, bytes, length) == Decision::Continue && bytes[0] == 1 && length == 2);
  assert(host.sent.empty() && host.errors.back() == Fault::EmissionRejected && pipeline.enabled(0));
  assert(run(pipeline, 8, bytes, length) == Decision::Continue && length == 3);
  assert(run(pipeline, 15, bytes, length) == Decision::Continue && length == 3);
  assert(run(pipeline, 9, bytes, length, metadata(Stage::PlainCompose)) == Decision::Continue && !length);
  assert(run(pipeline, 16, bytes, length) == Decision::Continue && bytes[1] == 1);
  assert(run(pipeline, 16, bytes, length) == Decision::Continue && bytes[1] == 2);
  printf("PASS packet Wasm eight stages, copied metadata, read/write/replace/drop/emissions/persistent state; "
         "guest=%u session=%u linear=%u stack=%u shared-pool=%u pool-high-water=%u\n",
         engine.stats().sourceBytes, engine.stats().sessionBytes, engine.stats().linearBytes,
         engine.stats().stackBytes, engine.stats().poolBytes, engine.stats().poolHighWaterBytes);
}
static void faults() {
  struct Case { uint8_t mode; Fault fault; };
  for (const auto &c : {Case{2, Fault::Execution}, {3, Fault::Fuel}, {4, Fault::Bounds},
                       {5, Fault::Bounds}, {6, Fault::InvalidDecision}, {7, Fault::Fuel},
                       {9, Fault::Bounds}, {10, Fault::EmissionLimit}, {11, Fault::Bounds},
                       {12, Fault::Bounds}, {14, Fault::Bounds}}) {
    PacketWasm engine; load(engine);
    TestHost host; Pipeline pipeline(host); attach(pipeline, engine, c.mode == 3 ? 500 : 10000);
    uint8_t bytes[Capacity]; uint16_t length;
    assert(run(pipeline, c.mode, bytes, length) == Decision::Continue);
    assert(bytes[0] == c.mode && bytes[1] == 2 && length == 2 && host.sent.empty());
    assert(!pipeline.enabled(0) && pipeline.faults(0) == 1 && host.errors.back() == c.fault);
    assert(run(pipeline, 0, bytes, length) == Decision::Continue && length == 2 && bytes[0] == 0);
    load(engine); assert(pipeline.enable(0, true));
    assert(run(pipeline, 0, bytes, length) == Decision::Continue && length == 3);
  }
  for (unsigned origin = 0; origin < 3; ++origin) {
    PacketWasm engine; load(engine);
    TestHost host; Pipeline pipeline(host); attach(pipeline, engine);
    auto m = metadata(); m.local = origin == 0; m.engineOrigin = origin == 1; m.reflectionOrigin = origin == 2;
    uint8_t bytes[Capacity]; uint16_t length;
    assert(run(pipeline, 1, bytes, length, m) == Decision::Continue && length == 2 && bytes[0] == 1);
    assert(host.errors.back() == Fault::EmissionOrigin && !pipeline.enabled(0) && host.sent.empty());
  }
  PacketWasm engine; load(engine);
  TestHost host; Pipeline pipeline(host); attach(pipeline, engine, 100000, 200);
  host.now = UINT32_MAX - 50; host.step = 1;
  uint8_t bytes[Capacity]; uint16_t length;
  assert(run(pipeline, 13, bytes, length) == Decision::Continue);
  assert(length == 2 && bytes[0] == 13 && host.sent.empty() &&
         host.errors.back() == Fault::Deadline && !pipeline.enabled(0));
  assert(engine.stats().instructions <= 64 * 200);
  const auto loop = source("packet-4"); char error[128]{};
  assert(engine.load(loop.data(), loop.size(), error, sizeof(error)));
  TestHost fullHost; Pipeline fullPipeline(fullHost); attach(fullPipeline, engine, 100000);
  assert(run(fullPipeline, 0, bytes, length) == Decision::Continue);
  assert(fullHost.errors.back() == Fault::Fuel && !fullPipeline.enabled(0) && length == 2);
  puts("PASS packet Wasm trap/bounds/invalid disposition/fuel/deadline/native calls/emission guards rollback and disable");
}
static void services() {
  for (unsigned mode = 0; mode < 9; ++mode) {
    PacketWasm engine;
    load(engine);
    TestHost host;
    host.accept = mode != 2;
    host.available = mode != 5;
    Pipeline pipeline(host);
    assert(pipeline.attach(engine, {"services", 255, 10000, 20000, mode == 1 ? 0u : AllCapabilities}) ==
           Registration::Attached);
    uint8_t bytes[Capacity];
    uint16_t length;
    const uint8_t guestMode = mode == 3 ? 18 : mode == 4 ? 19 : mode >= 6 ? mode + 14 : 17;
    assert(run(pipeline, guestMode, bytes, length) == Decision::Continue);
    if (mode == 0) {
      assert(length == 3 && bytes[0] == 0x70 && host.controls == 1 && host.changed.phy.txPower == 2 &&
             host.changed.generation == 21 && !host.changed.persist && host.sent.size() == 1 &&
             host.sent[0].length == 6 && host.composed == 1);
    } else {
      assert(length == 2 && bytes[0] == guestMode && bytes[1] == 2 && !host.controls && host.sent.empty());
      const Fault expected[] = {Fault::None,      Fault::Permission,   Fault::EmissionRejected,
                                Fault::Execution, Fault::ControlLimit, Fault::Unavailable,
                                Fault::Bounds,    Fault::Bounds,       Fault::Bounds};
      assert(host.errors.back() == expected[mode] && pipeline.enabled(0) == (mode == 2));
    }
  }
  puts("PASS packet Wasm system/PHY/owned-compose ABI copies, grants, bounds and transactional controls/emissions");
  for (const uint32_t capabilities : {uint32_t(ReadSystem), uint32_t(ReadSystem | WritePhy)}) {
    PacketWasm engine;
    load(engine);
    TestHost host;
    Pipeline pipeline(host);
    assert(pipeline.attach(engine, {"grants", 255, 10000, 20000, capabilities}) == Registration::Attached);
    uint8_t bytes[Capacity];
    uint16_t length;
    run(pipeline, 17, bytes, length);
    assert(host.errors.back() == Fault::Permission && !pipeline.enabled(0) && host.sent.empty() && !host.controls &&
           !host.composed);
  }
}
static void initialization_and_profile() {
  char error[128]{};
  PacketWasm engine;
  for (const char *name : {"packet-1", "packet-2", "packet-3", "c-arithmetic"}) {
    const auto bytes = source(name);
    assert(!engine.load(bytes.data(), bytes.size(), error, sizeof(error), 500));
    assert(!engine.loaded() && error[0] && engine.stats().error[0]);
  }
  const auto bytes = source("packet-0");
  assert(!engine.load(bytes.data(), bytes.size(), error, sizeof(error), 0));
  assert(!engine.load(bytes.data(), bytes.size(), error, sizeof(error), 100001));
  assert(!engine.load(bytes.data(), bytes.size(), error, sizeof(error), 1000, 20001));
  std::vector<uint8_t> oversized(PacketWasmSourceLimit + 1);
  assert(!engine.load(oversized.data(), oversized.size(), error, sizeof(error)));
  clockTime = 100; clockStep = 10000;
  assert(!engine.load(bytes.data(), bytes.size(), error, sizeof(error)));
  assert(strstr(error, "time budget"));
  clockStep = 0;
  load(engine);
  size_t codeEnd = 8;
  const auto leb = [&](size_t &offset) {
    uint32_t value = 0;
    for (unsigned shift = 0; shift < 35; shift += 7) {
      assert(offset < bytes.size());
      const uint8_t byte = bytes[offset++];
      value |= uint32_t(byte & 127) << shift;
      if (!(byte & 128)) return value;
    }
    assert(false); return uint32_t(0);
  };
  while (codeEnd < bytes.size()) {
    const auto section = bytes[codeEnd++];
    const auto size = leb(codeEnd); codeEnd += size;
    assert(codeEnd <= bytes.size());
    if (section == 10) break;
  }
  // Clang can leave an optional target-features section after the code.
  assert(engine.load(bytes.data(), codeEnd, error, sizeof(error)));
  for (size_t cut = 0; cut < codeEnd; ++cut) {
    assert(!engine.load(bytes.data(), cut, error, sizeof(error)));
    assert(!engine.loaded());
  }
  load(engine);
  puts("PASS packet Wasm initialization fuel/time/ABI/import guards and truncated source rejection");
}
static void coexistence(bool packetFirst) {
  PacketWasm engine;
  BotWasmSession bot;
  BotVmStats stats; char error[128]{};
  auto bytes = source("c-arithmetic");
  if (packetFirst) load(engine);
  assert(bot.load(reinterpret_cast<const char *>(bytes.data()), bytes.size(), 1, stats, error, sizeof(error), {}));
  if (!packetFirst) load(engine);
  poolStats("coexist loaded");
  TestHost host; Pipeline pipeline(host); attach(pipeline, engine);
  for (unsigned i = 0; i < 5; ++i) {
    uint8_t packet[Capacity]; uint16_t length;
    assert(run(pipeline, 16, packet, length) == Decision::Continue && packet[1] == i + 1);
    BotEvent event; event.authenticated = true; event.replyLimit = BotReplyLimit;
    strcpy(event.name, "wadd"); strcpy(event.arguments, "7 8");
    assert(bot.start(i + 1, event, error, sizeof(error)));
    BotSession::Result result;
    assert(bot.poll(result) && result.ok && !strcmp(result.action.text, "15"));
  }
  printf("PASS packet and command Wasm share one runtime, %s initialization order\n",
         packetFirst ? "packet-first" : "bot-first");
  poolStats("coexist done");
  bot.clear(); poolStats("bot cleared");
  engine.clear(); poolStats("packet cleared");
}
static void realloc_accounting() {
  mem_alloc_info_t before{}, grown{}, after{};
  assert(wasm_runtime_get_mem_alloc_info(&before));
  auto *bytes = static_cast<uint8_t *>(wasm_runtime_malloc(8)); assert(bytes);
  bytes[0] = 0xa5;
  auto *larger = static_cast<uint8_t *>(wasm_runtime_realloc(bytes, 2048)); assert(larger);
  assert(larger[0] == 0xa5);
  assert(wasm_runtime_get_mem_alloc_info(&grown));
  assert(before.total_free_size - grown.total_free_size >= 2048);
  wasm_runtime_free(larger);
  assert(wasm_runtime_get_mem_alloc_info(&after));
  assert(after.total_free_size == before.total_free_size && after.highmark_size <= after.total_size);
  puts("PASS WAMR pool realloc growth/free accounting remains bounded");
}
static void concurrent_loaders() {
  const auto packetSource = source("packet-0"), botSource = source("c-arithmetic");
  std::thread packet([&] {
    PacketWasm engine; char error[128]{};
    for (unsigned i = 0; i < 50; ++i) {
      assert(engine.load(packetSource.data(), packetSource.size(), error, sizeof(error)));
      TestHost host; Pipeline pipeline(host); attach(pipeline, engine);
      uint8_t bytes[Capacity]; uint16_t length;
      assert(run(pipeline, 0, bytes, length) == Decision::Continue && length == 3);
      engine.clear();
    }
  });
  std::thread command([&] {
    BotWasmSession bot; BotVmStats stats; char error[128]{};
    for (unsigned i = 0; i < 50; ++i) {
      assert(bot.load(reinterpret_cast<const char *>(botSource.data()), botSource.size(),
                      i + 1, stats, error, sizeof(error), {}));
      BotEvent event; event.authenticated = true; event.replyLimit = BotReplyLimit;
      strcpy(event.name, "wadd"); strcpy(event.arguments, "7 8");
      assert(bot.start(1, event, error, sizeof(error)));
      BotSession::Result result;
      assert(bot.poll(result) && result.ok && !strcmp(result.action.text, "15"));
      bot.clear();
    }
  });
  packet.join(); command.join();
  poolStats("concurrent loaders done");
  puts("PASS shared WAMR concurrent first registration, reload/unload and independent guest execution");
}
static void repeater_snr() {
  PacketWasm engine; const auto guest = source("repeater-snr"); char error[128]{};
  assert(engine.load(guest.data(), guest.size(), error, sizeof(error)));
  TestHost host; Pipeline pipeline(host);
  assert(pipeline.attach(engine, {"snr", 1, 100000, 20000, ReadSystem | PublishReport}) == Registration::Attached);
  uint32_t maxFuel = 0;
  const auto receive = [&](unsigned hop, int snr, unsigned width = 3, unsigned route = 1,
                           unsigned flags = 0) {
    uint8_t bytes[Capacity]{};
    bytes[0] = route; const unsigned offset = route == 0 ? 5 : 1;
    bytes[offset] = ((width - 1) << 6) | 2;
    for (unsigned i = 0; i < width; ++i) {
      bytes[offset + 1 + i] = 0xa5;
      bytes[offset + 1 + width + i] = uint8_t(hop >> (8 * (width - i - 1)));
    }
    uint16_t length = offset + 2 + 2 * width; bytes[length - 1] = 0x42;
    const std::vector<uint8_t> original(bytes, bytes + length);
    auto m = metadata(); m.snrQuarterDb = snr; m.rssi = -100 + snr;
    m.local = flags == 1; m.reflectionOrigin = flags == 2; m.engineOrigin = flags == 3;
    const auto result = pipeline.process(m, bytes, length, Capacity);
    if (!pipeline.enabled(0)) fprintf(stderr, "SNR fault=%u fuel=%u error=%s\n",
      unsigned(host.errors.back()), engine.stats().instructions, engine.stats().error);
    assert(result == Decision::Continue && pipeline.enabled(0));
    assert(length == original.size() && !memcmp(bytes, original.data(), length) && host.sent.empty());
    maxFuel = std::max(maxFuel, engine.stats().instructions);
  };
  mp_report report{};
  for (unsigned flags = 0; flags < 4; ++flags) receive(0xd12600, 4, 3, flags ? 1 : 2, flags);
  assert(!host.reports.read(0, report));
  std::vector<int> samples;
  for (int value : {-12, 4, 20, 36}) { receive(0xd12600, value); samples.push_back(value); }
  assert(host.reports.read(0, report) && report.row_count == 1 && !strcmp(report.rows[0].label, "D12600"));
  assert(report.rows[0].values[0] == 4 && report.rows[0].values[2] == 3 && report.rows[0].values[3] == 9);
  for (unsigned i = 0; i < 300; ++i) { int value = int(i % 80) - 40; samples.push_back(value); receive(0xd12600, value); }
  samples.erase(samples.begin(), samples.end() - 256);
  double sum = 0; for (int value : samples) sum += value;
  std::sort(samples.begin(), samples.end());
  assert(host.reports.read(0, report) && report.rows[0].values[0] == 256 && report.rows[0].values[1] == 304);
  assert(report.rows[0].values[2] == sum / 1024 && report.rows[0].values[3] == samples[(256 * 95 + 99) / 100 - 1] / 4.0);
  receive(0xd1, 8, 1); receive(0xd126, 12, 2, 0);
  for (unsigned i = 0; i < 24; ++i) for (unsigned j = 0; j < 256 + i; ++j) receive(0x100000 + i, int(j % 80) - 40);
  assert(host.reports.read(0, report) && report.row_count == 16 && !strcmp(report.rows[0].label, "D12600"));
  for (unsigned i = 1; i < report.row_count; ++i) assert(report.rows[i-1].values[1] >= report.rows[i].values[1]);
  std::ifstream input("../runtime/plugins/examples/snr.lua"); assert(input);
  const std::string plugin{std::istreambuf_iterator<char>(input), {}};
  BotSession bot; BotVmStats stats;
  assert(bot.load(plugin.data(), plugin.size(), 1, stats, error, sizeof(error)));
  unsigned id = 0;
  for (const char *command : {"!snr", "!snr D12600", "!snr FFFFFF"}) {
    BotEvent e{}; assert(parseBotCommand(command, strlen(command), e, error, sizeof(error)));
    e.authenticated = true; e.node.uptimeMs = 2123;
    assert(bot.start(++id, e, error, sizeof(error)));
    BotIoRequest request; assert(bot.nextIo(request) && request.kind == BotIoRequest::Inspect && !strcmp(request.key, "packet_report"));
    BotIoResult completion{}; completion.token = request.token; completion.ok = completion.found = true; completion.packetReport = report;
    assert(bot.complete(completion)); BotSession::Result result;
    assert(bot.poll(result));
    if (!result.ok) fprintf(stderr, "snr bot: %s\n", result.error);
    assert(result.ok && strstr(result.action.text, id == 1 ? "SNR avg/P95" : id == 2 ? "256 samples" : "No SNR samples"));
  }
  struct Failing : Engine { Decision process(const Metadata &, Call &) override { return Decision::Failed; } } failing;
  assert(pipeline.attach(failing, {"failure", 1, 1000, 20000}) == Registration::Attached);
  const auto before = report;
  uint8_t packet[Capacity] = {1, 0x81, 0xd1, 0x26, 0, 0x42}; uint16_t length = 6;
  pipeline.process(metadata(), packet, length, Capacity);
  assert(host.reports.read(0, report) && !memcmp(&before, &report, sizeof(report)) && !pipeline.enabled(1));
  assert(pipeline.enable(1, false));
  assert(pipeline.enable(0, false) && !host.reports.read(0, report));
  printf("PASS repeater SNR real Wasm ring/P95/final-hop widths/24 peers, shared bot report/!snr, max fuel=%u\n", maxFuel);
}
int main(int argc, char **argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  assert(argc == 3); directory = argv[1];
  if (!strcmp(argv[2], "concurrent")) { concurrent_loaders(); return 0; }
  coexistence(!strcmp(argv[2], "packet-first"));
  repeater_snr(); realloc_accounting(); stages_and_operations(); faults(); initialization_and_profile(); services();
}
