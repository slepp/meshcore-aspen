// SPDX-License-Identifier: Apache-2.0
#include "PacketLua.h"
#include "PacketWasm.h"
#include "support/PacketServiceHost.h"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
using namespace onchip;
using namespace packet_engine;
static uint64_t clockTime = 0, clockStep = 0;
uint64_t onchipBotVmTestClock() {
  if (clockStep) return clockTime += clockStep;
  return std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}
struct TestHost final : PacketServiceHost {};
static const char *basic = R"lua(
local count = 0
return function()
  local info = packet.info()
  assert(info.version == 1 and info.source == 3 and info.destination == 4)
  assert(info.payload_type == 5 and info.generation == 6 and info.job == 7)
  assert(info.rssi == -101 and info.snr_quarter_db == -9 and #info.identity == 32)
  assert(info.capacity == 255)
  local bytes = packet.read(0, 2)
  assert(#bytes == 2)
  assert(packet.write(1, "\x42") == 1)
  assert(packet.replace("\x70\x42\x43") == 3)
  count = count + 1
  return packet.CONTINUE
end
)lua";
static Metadata metadata(Stage stage = Stage::Receive) {
  Metadata m;
  m.stage = stage; m.source = 3; m.destination = 4; m.payloadType = 5;
  m.generation = 6; m.job = 7; m.rssi = -101; m.snrQuarterDb = -9; m.identity[31] = 0xa5;
  return m;
}
static void load(PacketLua &engine, const char *program = basic) {
  char error[128]{};
  const bool ok = engine.load(program, strlen(program), error, sizeof(error));
  if (!ok) fprintf(stderr, "Lua load: %s\n", error);
  assert(ok && engine.loaded() && !engine.stats().error[0]);
  assert(engine.stats().sourceBytes == strlen(program) && engine.stats().heapLimit == 65536 &&
         engine.stats().liveBytes > 0 && engine.stats().peakBytes <= 65536);
}
static void attach(Pipeline &pipeline, Engine &engine, const char *name = "lua", uint32_t fuel = 10000, uint32_t us = 20000) {
  assert(pipeline.attach(engine, {name, 255, fuel, us}) == Registration::Attached);
}
static Decision run(Pipeline &pipeline, uint8_t (&bytes)[Capacity], uint16_t &length, Metadata m = metadata()) {
  memset(bytes, 0, sizeof(bytes)); bytes[1] = 2; length = 2;
  return pipeline.process(m, bytes, length, Capacity);
}
static void stages_and_operations() {
  PacketLua engine; load(engine);
  TestHost host; Pipeline pipeline(host); attach(pipeline, engine);
  uint8_t bytes[Capacity]; uint16_t length;
  for (unsigned stage = 0; stage < 8; ++stage) {
    assert(run(pipeline, bytes, length, metadata(Stage(stage))) == Decision::Continue);
    assert(length == 3 && bytes[0] == 0x70 && bytes[1] == 0x42 && bytes[2] == 0x43);
  }
  load(engine, R"lua(
    return function()
      packet.replace("\x70\x42\x43")
      packet.emit("\x70\x42\x43", 1, 12, 23)
      packet.emit("\x70\x42\x43", 2, 13, 24)
      return packet.DROP
    end
  )lua");
  assert(run(pipeline, bytes, length) == Decision::Drop && length == 2 && !bytes[0]);
  assert(host.sent.size() == 2 && host.sent[0].priority == 1 && host.sent[0].delayMs == 12 &&
         host.sent[0].expiryMs == 23 && host.sent[1].priority == 2 && host.sent[1].delayMs == 13);
  host.sent.clear(); host.accept = false;
  assert(run(pipeline, bytes, length) == Decision::Continue && length == 2 && !bytes[0]);
  assert(host.sent.empty() && host.errors.back() == Fault::EmissionRejected && pipeline.enabled(0));
  load(engine, "return function() packet.replace(''); return 0 end");
  assert(run(pipeline, bytes, length, metadata(Stage::PlainCompose)) == Decision::Continue && !length);
  load(engine, "local n=0; return function() n=n+1; packet.replace(n%2==1 and 'one' or 'two'); return 0 end");
  assert(run(pipeline, bytes, length) == Decision::Continue && !memcmp(bytes, "one", 3));
  assert(run(pipeline, bytes, length) == Decision::Continue && !memcmp(bytes, "two", 3));
  puts("PASS packet Lua eight stages, metadata, binary read/write/replace/drop/emissions and retained state");
}
static void faults() {
  struct Case { const char *body; Fault fault; };
  for (const auto &c : {
       Case{"packet.emit('new'); local x=nil; return x.missing", Fault::Execution},
       {"while true do end", Fault::Fuel},
       {"packet.read(0, 4); return 0", Fault::Bounds},
       {"packet.write(3, 'x'); return 0", Fault::Bounds},
       {"return 256", Fault::InvalidDecision},
       {"return 0.5", Fault::InvalidDecision},
       {"return '0'", Fault::InvalidDecision},
       {"return nil", Fault::InvalidDecision},
       {"for i=1,65 do packet.info() end; return 0", Fault::Fuel},
       {"packet.replace(''); return 0", Fault::Bounds},
       {"for i=1,3 do packet.emit('new') end; return 0", Fault::EmissionLimit},
       {"packet.emit('new', 256); return 0", Fault::Bounds},
       {"packet.emit('new', 1, -1); return 0", Fault::Bounds},
       {"packet.write(65536, 'x'); return 0", Fault::Bounds},
       {"packet.replace('new'); local s='x'; while true do s=s..s end", Fault::Execution}}) {
    const std::string program = std::string("return function() packet.write(1,'Z'); ") + c.body + " end";
    PacketLua engine; load(engine, program.c_str());
    TestHost host; Pipeline pipeline(host); attach(pipeline, engine, "lua", 10000);
    uint8_t bytes[Capacity]; uint16_t length;
    assert(run(pipeline, bytes, length) == Decision::Continue);
    assert(!bytes[0] && bytes[1] == 2 && length == 2 && host.sent.empty());
    assert(!pipeline.enabled(0) && pipeline.faults(0) == 1 && host.errors.back() == c.fault);
    assert(engine.stats().error[0]);
    load(engine); assert(pipeline.enable(0, true));
    assert(run(pipeline, bytes, length) == Decision::Continue && length == 3);
  }
  for (unsigned origin = 0; origin < 3; ++origin) {
    PacketLua engine; load(engine, "return function() packet.emit('new'); return 0 end");
    TestHost host; Pipeline pipeline(host); attach(pipeline, engine);
    auto m = metadata(); m.local = origin == 0; m.engineOrigin = origin == 1; m.reflectionOrigin = origin == 2;
    uint8_t bytes[Capacity]; uint16_t length;
    assert(run(pipeline, bytes, length, m) == Decision::Continue && length == 2);
    assert(host.errors.back() == Fault::EmissionOrigin && !pipeline.enabled(0) && host.sent.empty());
  }
  PacketLua engine; load(engine, "return function() packet.emit('new'); while true do end end");
  TestHost host; Pipeline pipeline(host); attach(pipeline, engine, "lua", 100000, 200);
  host.now = UINT32_MAX - 50; host.step = 1;
  uint8_t bytes[Capacity]; uint16_t length;
  assert(run(pipeline, bytes, length) == Decision::Continue && length == 2 && host.sent.empty());
  assert(host.errors.back() == Fault::Deadline && !pipeline.enabled(0) && engine.stats().instructions < 1000);
  puts("PASS packet Lua runtime/heap/bounds/fuel/deadline/native call/emission faults rollback and disable");
}
static void services() {
  const char *body = R"lua(
      local sys = packet.system()
      assert(sys.version == 1 and sys.uptime_ms == 123 and sys.unix_time == 1791500000)
      assert(sys.free_internal_bytes == 456 and sys.free_psram_bytes == 789 and sys.flags == 13)
      assert(sys.enabled_roles == 15 and sys.ready_roles == 7 and sys.generation == 21)
      assert(sys.phy.frequency_hz == 910525000 and sys.phy.bandwidth_hz == 62500)
      assert(sys.phy.spreading_factor == 7 and sys.phy.coding_rate == 5 and sys.phy.tx_power == 22)
      sys.phy.tx_power = 2
      assert(packet.set_phy(sys.phy, sys.generation, false) == 28)
      local wire = packet.compose({kind=packet.ADVERT, payload_type=4,
        route=packet.DIRECT, path_width=2, path_count=1, path="ab",
        identity=packet.info().identity, timestamp=sys.unix_time}, "xy")
      assert(wire == "\x12\x41abxy")
      packet.emit(wire)
    )lua";
  for (unsigned mode = 0; mode < 6; ++mode) {
    const std::string program = std::string("return function() packet.replace('new'); ") + body +
                                (mode == 3   ? " error_missing() "
                                 : mode == 4 ? " packet.set_phy(sys.phy, sys.generation) "
                                             : "") +
                                " return 0 end";
    PacketLua engine;
    load(engine, program.c_str());
    TestHost host;
    host.accept = mode != 2;
    host.available = mode != 5;
    Pipeline pipeline(host);
    assert(pipeline.attach(engine, {"services", 255, 10000, 20000, mode == 1 ? 0u : AllCapabilities}) ==
           Registration::Attached);
    uint8_t bytes[Capacity];
    uint16_t length;
    assert(run(pipeline, bytes, length) == Decision::Continue);
    if (mode == 0) {
      assert(length == 3 && !memcmp(bytes, "new", 3) && host.controls == 1 && host.changed.phy.txPower == 2 &&
             host.changed.generation == 21 && !host.changed.persist && host.sent.size() == 1 &&
             host.sent[0].length == 6 && host.composed == 1);
    } else {
      assert(length == 2 && !bytes[0] && bytes[1] == 2 && !host.controls && host.sent.empty());
      const Fault expected[] = {Fault::None,      Fault::Permission,   Fault::EmissionRejected,
                                Fault::Execution, Fault::ControlLimit, Fault::Unavailable};
      assert(host.errors.back() == expected[mode] && pipeline.enabled(0) == (mode == 2));
    }
  }
  for (const char *body :
       {"packet.set_phy({frequency_hz=1,bandwidth_hz=62500,spreading_factor=7,coding_rate=5,tx_power=2},21)",
        "packet.set_phy({frequency_hz=910525000,bandwidth_hz=62500,spreading_factor=7,coding_rate=5,tx_power=2},21,1)",
        "packet.compose({kind=0,payload_type=4,identity='short'}, 'x')",
        "packet.compose({kind=0,payload_type=4,identity=packet.info().identity,path_width=3,path_count=63},'x')"}) {
    PacketLua engine;
    const std::string program = std::string("return function() ") + body + "; return 0 end";
    load(engine, program.c_str());
    TestHost host;
    Pipeline pipeline(host);
    assert(pipeline.attach(engine, {"services", 255, 10000, 20000, AllCapabilities}) == Registration::Attached);
    uint8_t bytes[Capacity];
    uint16_t length;
    run(pipeline, bytes, length);
    assert(!pipeline.enabled(0) && host.errors.back() == Fault::Bounds && !host.controls && host.sent.empty());
  }
  puts("PASS packet Lua system/PHY/owned-compose records, grants, bounds and transactional controls/emissions");
  for (const uint32_t capabilities : {uint32_t(ReadSystem), uint32_t(ReadSystem | WritePhy)}) {
    const std::string program = std::string("return function() ") + body + " return 0 end";
    PacketLua engine;
    load(engine, program.c_str());
    TestHost host;
    Pipeline pipeline(host);
    assert(pipeline.attach(engine, {"grants", 255, 10000, 20000, capabilities}) == Registration::Attached);
    uint8_t bytes[Capacity];
    uint16_t length;
    run(pipeline, bytes, length);
    assert(host.errors.back() == Fault::Permission && !pipeline.enabled(0) && host.sent.empty() && !host.controls &&
           !host.composed);
  }
}
static void initialization() {
  char error[128]{}; PacketLua engine;
  for (const char *text : {"while true do end", "packet.info(); return function() return 0 end",
       "return 0", "return packet.read", "return function("}) {
    assert(!engine.load(text, strlen(text), error, sizeof(error), 65536, 500));
    assert(!engine.loaded() && error[0]);
  }
  const char binary[] = {'r', '\0', 'x'};
  assert(!engine.load(binary, sizeof(binary), error, sizeof(error)));
  assert(!engine.load("\x1bLua", 4, error, sizeof(error)));
  std::string large(PacketLuaSourceLimit + 1, 'x');
  assert(!engine.load(large.data(), large.size(), error, sizeof(error)));
  assert(!engine.load(basic, strlen(basic), error, sizeof(error), 8191));
  clockTime = 100; clockStep = 10000;
  assert(!engine.load(basic, strlen(basic), error, sizeof(error)));
  clockStep = 0; load(engine); engine.clear();
  assert(!engine.loaded() && !engine.stats().liveBytes);
  puts("PASS packet Lua source-only parsing, initialization budgets, active import guards and released heap");
}
static void comparison(const char *wasmPath) {
  std::ifstream input(wasmPath, std::ios::binary); assert(input);
  std::vector<uint8_t> source{std::istreambuf_iterator<char>(input), {}};
  PacketWasm wasm; char error[128]{};
  assert(wasm.load(source.data(), source.size(), error, sizeof(error)));
  PacketLua lua; load(lua);
  TestHost host; Pipeline pipeline(host);
  attach(pipeline, wasm, "wasm"); attach(pipeline, lua);
  uint8_t bytes[Capacity]; uint16_t length;
  assert(run(pipeline, bytes, length) == Decision::Continue && length == 3);
  load(lua, "return function() packet.emit('new'); local x=nil; return x.missing end");
  assert(run(pipeline, bytes, length) == Decision::Continue);
  assert(length == 2 && !bytes[0] && bytes[1] == 2 && host.sent.empty() &&
         pipeline.enabled(0) && !pipeline.enabled(1));
  load(lua);
  const auto benchmark = [&](Engine &engine, const char *name) {
    TestHost h; Pipeline p(h); attach(p, engine, name);
    const auto started = std::chrono::steady_clock::now();
    constexpr unsigned iterations = 10000;
    for (unsigned i = 0; i < iterations; ++i) {
      assert(run(p, bytes, length) == Decision::Continue && length == 3 && bytes[0] == 0x70);
      assert(p.enabled(0));
    }
    return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - started).count() / iterations;
  };
  const double wasmUs = benchmark(wasm, "wasm"), luaUs = benchmark(lua, "lua");
  printf("HOST packet edit mean: Wasm=%.2fus/op Lua=%.2fus/op (10000 each); "
         "Wasm session=%u linear=%u stack=%u shared pool=%u; "
         "Lua session=%u live=%u peak=%u cap=%u; last fuel Wasm=%u Lua=%u\n",
         wasmUs, luaUs, wasm.stats().sessionBytes, wasm.stats().linearBytes, wasm.stats().stackBytes,
         wasm.stats().poolBytes, lua.stats().sessionBytes, lua.stats().liveBytes,
         lua.stats().peakBytes, lua.stats().heapLimit, wasm.stats().instructions, lua.stats().instructions);
  puts("PASS mixed Wasm/Lua invocation rollback and bounded repeated packet processing");
}
int main(int argc, char **argv) {
  assert(argc == 2); setvbuf(stdout, nullptr, _IONBF, 0);
  stages_and_operations(); faults(); initialization(); services(); comparison(argv[1]);
}
