// SPDX-License-Identifier: Apache-2.0
#include "PacketWasm.h"
#include "BotWasm.h"
#include "WamrRuntime.h"
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
struct TestHost final : Host {
  uint32_t now = 0, step = 0;
  bool accept = true;
  std::vector<Fault> errors;
  std::vector<Emission> sent;
  uint32_t microsNow() override { return now += step; }
  void fault(const char *, const Metadata &, Fault error) override { errors.push_back(error); }
  bool admit(const Metadata &, const Emission *packets, uint8_t count) override {
    if (!accept) return false;
    sent.insert(sent.end(), packets, packets + count); return true;
  }
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
  assert(engine.stats().instructions < 1000);
  const auto loop = source("packet-4"); char error[128]{};
  assert(engine.load(loop.data(), loop.size(), error, sizeof(error)));
  TestHost fullHost; Pipeline fullPipeline(fullHost); attach(fullPipeline, engine, 100000);
  assert(run(fullPipeline, 0, bytes, length) == Decision::Continue);
  assert(fullHost.errors.back() == Fault::Fuel && !fullPipeline.enabled(0) && length == 2);
  puts("PASS packet Wasm trap/bounds/invalid disposition/fuel/deadline/native calls/emission guards rollback and disable");
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
int main(int argc, char **argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  assert(argc == 3); directory = argv[1];
  if (!strcmp(argv[2], "concurrent")) { concurrent_loaders(); return 0; }
  coexistence(!strcmp(argv[2], "packet-first"));
  realloc_accounting(); stages_and_operations(); faults(); initialization_and_profile();
}
