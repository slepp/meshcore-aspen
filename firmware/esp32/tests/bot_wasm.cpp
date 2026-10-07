// SPDX-License-Identifier: Apache-2.0
#include "BotVm.h"
#include "BotWasm.h"
#include "wasm_export.h"
#include <cassert>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
using namespace onchip;
static_assert(std::is_same<decltype(std::declval<const BotWasmSession &>().manifest()),
                           const BotManifestView &>::value,
              "Wasm manifest access must not require an owned empty command array");
static uint64_t fakeTime = 0, fakeStep = 0;
uint64_t onchipBotVmTestClock() {
  if (fakeStep) return fakeTime += fakeStep;
  return std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}
static std::string folder;
static std::string bytes(const std::string &name) {
  std::ifstream input(folder + "/" + name + ".wasm", std::ios::binary);
  assert(input); return {std::istreambuf_iterator<char>(input), {}};
}
static BotEvent event(const char *command) {
  BotEvent e;
  char error[128]{};
  assert(parseBotCommand(command, strlen(command), e, error, sizeof(error)));
  e.authenticated = true; e.sender[0] = 1;
  return e;
}
static void load(BotSession &vm, const std::string &source, BotVmLimits limits = {}) {
  char error[128]{};
  BotVmStats stats;
  const bool ok = vm.load(source.data(), source.size(), 7, stats, error, sizeof(error), limits);
  if (!ok) fprintf(stderr, "load failed: %s\n", error);
  assert(ok);
}
static BotSession::Result run(BotSession &vm, const BotEvent &e) {
  char error[128]{};
  assert(vm.start(1, e, error, sizeof(error)));
  BotIoRequest request; BotIoResult completion;
  while (vm.nextIo(request)) {
    completion.token = request.token; completion.ok = true;
    assert(vm.complete(completion));
  }
  BotSession::Result result;
  assert(vm.poll(result)); return result;
}
static void emptyManifest() {
  BotWasmSession vm;
  const auto verify = [&]() {
    const auto &manifest = vm.manifest();
    assert(!manifest.commands && !manifest.capacity && !manifest.count);
    assert(!manifest.eventMask && !manifest.moduleCount && !manifest.find("wadd"));
    const BotManifest copied{manifest};
    assert(!copied.count && !copied.find("wadd"));
  };
  verify();
  const auto source = bytes("c-arithmetic");
  BotVmStats stats;
  char error[128]{};
  assert(vm.load(source.data(), source.size(), 1, stats, error, sizeof(error), {}));
  assert(vm.manifest().count == 1 && vm.manifest().find("wadd"));
  vm.clear();
  verify();
  assert(!vm.load("invalid", 7, 2, stats, error, sizeof(error), {}));
  verify();
  puts("PASS Wasm empty manifest is a zero-capacity view across initial, loaded, cleared and rejected states");
}
static void initializationBudgets() {
  BotSession vm;
  BotVmStats stats;
  BotVmLimits limits;
  char error[128]{};
  const auto source = bytes("c-arithmetic");
  fakeTime = 1000; fakeStep = 1;
  assert(vm.load(source.data(), source.size(), 1, stats, error, sizeof(error)));
  const auto initTicks = stats.initUs;
  assert(initTicks > 0 && initTicks < 30000);
  fakeStep = 30000 / initTicks;
  assert(vm.load(source.data(), source.size(), 2, stats, error, sizeof(error)));
  assert(stats.initUs > limits.wallUs && stats.initUs < limits.initWallUs);
  fakeStep = 60000 / initTicks;
  assert(!vm.load(source.data(), source.size(), 3, stats, error, sizeof(error)));
  assert(strstr(error, "initialization wall deadline") && stats.initUs >= limits.initWallUs);
  fakeStep = 1;
  load(vm, bytes("fault-0"));
  fakeStep = 1000;
  const auto failed = run(vm, event("!wfault"));
  assert(!failed.ok && strstr(failed.error, "invocation wall deadline"));
  assert(failed.stats.invokeUs >= limits.wallUs && failed.stats.instructions < limits.instructions);
  fakeStep = 1;
  load(vm, bytes("c-notes"));
  assert(vm.start(4, event("!wnote durable"), error, sizeof(error)));
  BotIoRequest request;
  assert(vm.nextIo(request) && request.kind == BotIoRequest::Put);
  fakeTime += 1000000;
  BotIoResult completion; completion.token = request.token; completion.ok = true;
  BotSession::Result result;
  assert(vm.complete(completion) && vm.poll(result) && result.ok);
  assert(result.stats.invokeUs < limits.wallUs && result.stats.loadUs == 0 && result.stats.initUs == 0);
  fakeStep = 0;
  puts("PASS Wasm separate 50ms Init/20ms invocation budgets, unchanged fuel and uncharged suspended I/O");
}
static void homeArguments() {
  BotSession vm; load(vm, bytes("c-rpc-args"));
  struct Case { const char *operation; std::string json, value; bool valid; };
  const Case cases[] = {
    {"echo", "{\"text\":\"hello\"}", "hello", true},
    {"echo", " { \"\\u0074ext\" : \"a\\\"b\\\\c\\/d\" } ", "a\"b\\c/d", true},
    {"echo", "{\"text\":\"caf\\u00e9 \\ud83d\\ude80\"}", u8"café 🚀", true},
    {"echo", u8"{\"text\":\"café 🚀\"}", u8"café 🚀", true},
    {"echo", "{\"text\":\"\\b\\f\\n\\r\\t\"}", "\b\f\n\r\t", true},
    {"health", " \t{ \r\n} ", "", true},
    {"weather", "{\"place\":\"Paris\"}", "Paris", true},
    {"weather", "{\"place\":\"" + std::string(80, 'x') + "\"}", std::string(80, 'x'), true},
    {"weather", "{\"place\":\"" + std::string(81, 'x') + "\"}", "", false},
    {"echo", "{}", "", false},
    {"echo", "[]", "", false},
    {"echo", "{\"text\":true}", "", false},
    {"echo", "{\"text\":42}", "", false},
    {"echo", "{\"text\":null}", "", false},
    {"echo", "{\"text\":{}}", "", false},
    {"echo", "{\"text\":\"\"}", "", false},
    {"echo", "{\"text\":\"a\",\"text\":\"b\"}", "", false},
    {"echo", "{\"text\":\"a\",\"extra\":1}", "", false},
    {"echo", "{\"place\":\"Paris\"}", "", false},
    {"echo", "{\"text\":\"a\",}", "", false},
    {"echo", "{\"text\":\"a\"} {}", "", false},
    {"echo", "{\"text\":\"a\"", "", false},
    {"echo", "{\"text\":\"\\q\"}", "", false},
    {"echo", "{\"text\":\"\\u123\"}", "", false},
    {"echo", "{\"text\":\"\\u0000\"}", "", false},
    {"echo", "{\"text\":\"\\ud800\"}", "", false},
    {"echo", "{\"text\":\"\\ud800\\u1234\"}", "", false},
    {"echo", "{\"text\":\"\\udc00\"}", "", false},
    {"echo", "{\"text\":\"\xc0\xaf\"}", "", false},
    {"echo", "{\"text\":\"\xed\xa0\x80\"}", "", false},
    {"echo", "{\"text\":\"\xf4\x90\x80\x80\"}", "", false},
    {"echo", "{\"text\":\"\xe2\"}", "", false},
    {"echo", "{\"text\":\"raw\nnewline\"}", "", false},
    {"weather", "{\"text\":\"Paris\"}", "", false},
    {"health", "{\"text\":\"a\"}", "", false},
  };
  unsigned id = 100;
  for (const auto &test : cases) {
    auto e = event("!warg_echo");
    snprintf(e.name, sizeof(e.name), "warg_%s", test.operation);
    assert(test.json.size() < sizeof(e.arguments));
    memcpy(e.arguments, test.json.c_str(), test.json.size() + 1);
    e.homeAccess = true; e.homeGrant = 42;
    char error[128]{};
    assert(vm.start(id++, e, error, sizeof(error)));
    BotIoRequest request;
    const bool requested = vm.nextIo(request);
    if (requested != test.valid) fprintf(stderr, "home %s %s: admission mismatch\n", test.operation, test.json.c_str());
    assert(requested == test.valid);
    if (requested) {
      assert(!strcmp(request.value, test.value.c_str()) && !strcmp(request.json, test.json.c_str()));
      BotIoResult completion; completion.token = request.token; completion.ok = true;
      strcpy(completion.rpcCode, "ok"); assert(vm.complete(completion));
    }
    BotSession::Result result; assert(vm.poll(result) && result.ok == test.valid);
    if (!test.valid) assert(strstr(result.error, "Home RPC"));
  }
  for (unsigned count : {256, 257}) {
    const auto command = "!warg_repeat " + std::to_string(count);
    auto e = event(command.c_str()); e.homeAccess = true; e.homeGrant = 42;
    char error[128]{}; assert(vm.start(id++, e, error, sizeof(error)));
    BotIoRequest request; const bool valid = count == BotValueLimit;
    assert(vm.nextIo(request) == valid);
    if (valid) {
      assert(strlen(request.value) == count);
      BotIoResult completion; completion.token = request.token; completion.ok = true;
      assert(vm.complete(completion));
    }
    BotSession::Result result; assert(vm.poll(result) && result.ok == valid);
    if (!valid) assert(strstr(result.error, "Home RPC"));
  }
  puts("PASS Wasm configured-home strict arguments, UTF-8/Unicode escapes, native bounds and same-session recovery");
}
static void namedThreads() {
  BotSession vm; load(vm, bytes("contract-8"));
  auto e = event("!wcontract"); e.sender[31] = 9;
  char error[128]{};
  assert(vm.start(71, e, error, sizeof(error)));
  BotIoRequest request; unsigned phase = 0;
  const BotIoRequest::Kind kinds[] = {BotIoRequest::Get, BotIoRequest::Put, BotIoRequest::Cas,
      BotIoRequest::Transaction, BotIoRequest::List, BotIoRequest::TimerSet,
      BotIoRequest::TimerGet, BotIoRequest::TimerCancel};
  while (vm.nextIo(request)) {
    assert(phase < 8 && request.kind == kinds[phase] && request.scope == BotIoRequest::CallerThread &&
           !memcmp(request.principal, e.sender, 32));
    if (phase == 2 || phase == 3) {
      assert(!strcmp(request.key, "notes/") && !strcmp(request.mutation[0].key, "notes/a"));
      if (phase == 3) assert(!strcmp(request.mutation[1].key, "notes/b"));
    } else assert(!strcmp(request.key, phase == 4 ? "notes/" : phase >= 5 ? "notes/wake" : "notes/key"));
    BotIoResult completion; completion.ok = true; completion.token = request.token;
    completion.outcome = BotIoResult::Committed; completion.timerState = BotTimerState::Pending;
    assert(vm.complete(completion)); ++phase;
  }
  BotSession::Result result;
  assert(phase == 8 && vm.poll(result) && result.ok && !strcmp(result.action.text, "thread complete"));
  strcpy(e.threadRules[0].name, "notes"); e.threadRules[0].access = 0;
  assert(vm.start(72, e, error, sizeof(error)) && vm.poll(result) && !result.ok && !vm.nextIo(request));
  e.threadRules[0] = {};
  load(vm, bytes("contract-9"));
  assert(vm.start(73, e, error, sizeof(error)) && vm.poll(result) && !result.ok &&
         strstr(result.error, "mix thread") && !vm.nextIo(request));
  puts("PASS Wasm additive thread scopes and SDK key builder: all 8 KV/timer operations, full origin, native RW denial and mixed-thread atomic rejection");
}
int main(int argc, char **argv) {
  assert(argc == 2); folder = argv[1];
  emptyManifest();
  initializationBudgets();
  homeArguments();
  namedThreads();
  for (const char *language : {"c", "rust"}) {
    BotSession arithmetic;
    load(arithmetic, bytes(std::string(language) + "-arithmetic"));
    auto result = run(arithmetic, event(language[0] == 'c' ? "!wadd 40 2" : "!radd 40 2"));
    if (!result.ok) fprintf(stderr, "%s\n", result.error);
    assert(result.ok && !strcmp(result.action.text, "42"));
    BotSession notes;
    load(notes, bytes(std::string(language) + "-notes"));
    char error[128]{};
    assert(notes.start(2, event(language[0] == 'c' ? "!wnote durable" : "!rnote durable"), error, sizeof(error)));
    BotIoRequest request;
    assert(notes.nextIo(request) && request.kind == BotIoRequest::Put &&
           request.scope == BotIoRequest::Caller && request.principal[0] == 1 &&
           !strcmp(request.value, "durable"));
    BotIoResult io; io.token = request.token; io.ok = true;
    assert(notes.complete(io)); assert(!notes.complete(io));
    assert(notes.poll(result) && result.ok);
    BotSession rpc;
    load(rpc, bytes(std::string(language) + "-rpc"));
    auto e = event(language[0] == 'c' ? "!wecho hello" : "!recho hello");
    e.homeAccess = true; e.homeGrant = 42;
    assert(rpc.start(3, e, error, sizeof(error)));
    assert(rpc.nextIo(request) && request.kind == BotIoRequest::Rpc &&
           request.grant == 42 && !strcmp(request.value, "hello"));
    io.token = request.token; strcpy(io.value, "hello");
    assert(rpc.complete(io) && rpc.poll(result) && result.ok && !strcmp(result.action.text, "hello"));
    assert(notes.start(4, event(language[0] == 'c' ? "!wnote pending" : "!rnote pending"), error, sizeof(error)));
    assert(notes.nextIo(request)); io.token = request.token;
    notes.cancel(); assert(!notes.complete(io)); assert(notes.poll(result) && !result.ok);
    e = event(language[0] == 'c' ? "!wnote unauthorized" : "!rnote unauthorized");
    e.authenticated = false;
    assert(!notes.start(5, e, error, sizeof(error)));
  }
  for (unsigned fault = 0; fault < 9; ++fault) {
    BotSession vm;
    load(vm, bytes("fault-" + std::to_string(fault)));
    auto started = std::chrono::steady_clock::now();
    BotSession::Result result;
    if (fault == 7) {
      char error[128]{};
      assert(vm.start(1, event("!wfault"), error, sizeof(error)));
      BotIoRequest request; BotIoResult io;
      unsigned count = 0;
      while (vm.nextIo(request)) { ++count; io.token = request.token; io.ok = true; assert(vm.complete(io)); }
      assert(count == BotIoLimit && vm.poll(result) && !result.ok);
    } else {
      result = run(vm, event("!wfault"));
      if (fault == 4) assert(result.ok && !strcmp(result.action.text, "growth rejected"));
      else assert(!result.ok && result.error[0] && result.action.kind == BotAction::None);
    }
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    assert(ms < 100);
    printf("fault=%u terminated=%ldms instructions=%u error=%s\n", fault, long(ms), result.stats.instructions, result.error);
    BotSession lua;
    load(lua, "function healthy() reply('Lua alive') end");
    assert(run(lua, event("!healthy")).ok);
  }
  for (unsigned contract = 0; contract < 8; ++contract) {
      BotSession vm;
      load(vm, bytes("contract-" + std::to_string(contract)));
      auto e = event("!wcontract");
      e.reminderAccess = e.forwardAccess = e.homeAccess = true;
      e.owner = true;
      e.reminderGrant = 17; e.forwardGrant = 19; e.homeGrant = 21; e.meshGrant = 23;
      char error[128]{};
      assert(vm.start(41, e, error, sizeof(error)));
      BotIoRequest request;
      unsigned step = 0;
      while (vm.nextIo(request)) {
        assert(request.token.operation & 0x80000000u);
        BotIoResult result; result.token = request.token; result.ok = true;
        if (contract == 0) {
          assert(request.kind == (step ? BotIoRequest::Transaction : BotIoRequest::Cas));
          assert(request.mutations == (step ? 2 : 1) && request.principal[0] == 1);
          assert(!strcmp(request.mutation[0].value, "one") && request.mutation[0].compare);
          result.outcome = step ? BotIoResult::Unknown : BotIoResult::Committed;
        } else if (contract == 1) {
          assert(request.kind == (step == 0 ? BotIoRequest::ReminderSet :
                                  step == 1 ? BotIoRequest::ReminderList : BotIoRequest::ReminderCancel));
          assert(request.principal[0] == 1);
          if (!step) assert(request.delaySeconds == 30 && request.grant == 17 && !strcmp(request.value, "later"));
          if (step == 2) assert(request.revision == 37);
          result.revision = 37; result.reminderState = BotReminderState::Cancelled;
        } else if (contract == 2) {
          assert(request.kind == (step == 0 ? BotIoRequest::Send :
                                  step == 1 ? BotIoRequest::Wait : BotIoRequest::Forward));
          if (step == 0) assert(request.principal[0] == 1 && request.grant == 23 && !strcmp(request.value, "hello"));
          if (step == 1) { result.packet.id = 99; result.packet.forwardable = result.packet.authenticated = true; }
          if (step == 2) assert(request.packetId == 99 && request.grant == 19);
          result.transmitted = false;
        } else if (contract == 3) {
          assert(request.kind == BotIoRequest::Utility && !strcmp(request.value, "6*7"));
          strcpy(result.value, "= 42");
        } else if (contract == 4) {
          assert(request.kind == BotIoRequest::HttpPost && request.grant == 21 &&
                 !strcmp(request.key, "echo") && !strcmp(request.json, "{\"value\":42}") && request.principal[0] == 1);
          memset(result.json, ' ', 2048); result.json[2048] = 0;
          strcpy(result.rpcCode, "unknown"); result.networkSubmitted = true; result.ok = false;
        } else if (contract == 5) {
          assert(request.kind == BotIoRequest::Rpc && request.grant == 21 &&
                 !strcmp(request.endpoint, "home") && !strcmp(request.key, "echo") &&
                 !strcmp(request.json, "{\"text\":\"hello\"}") &&
                 !strcmp(request.value, "hello") && request.principal[0] == 1);
          strcpy(result.json, "{\"value\":42}"); strcpy(result.rpcCode, "ok");
        } else if (contract == 6) {
          assert(request.kind == (step == 0 ? BotIoRequest::Trace : step == 1 ? BotIoRequest::Inspect :
                                  step == 2 ? BotIoRequest::Advert : BotIoRequest::Admin));
          if (!step) {
            assert(request.routeSize == 2 && request.routeWidth == 1 && request.route[0] == 0xab);
            result.trace.width = 1; result.trace.count = 2;
          } else if (step == 1) assert(!strcmp(request.key, "neighbors") && request.revision == 2);
          else if (step == 3) assert(!strcmp(request.value, "status"));
        } else {
          assert(request.kind == BotIoRequest::Utility && request.delaySeconds == step + 1);
          if (!step) assert(!strcmp(request.value, "100") && !strcmp(request.key, "C") && !strcmp(request.endpoint, "F"));
          strcpy(result.value, "copied utility result");
        }
        auto malformed = result; memset(malformed.json, 'x', sizeof(malformed.json));
        assert(!vm.complete(malformed));
        assert(vm.complete(result)); assert(!vm.complete(result));
        ++step;
      }
      BotSession::Result result;
      assert(vm.poll(result) && result.ok);
      const char *expected[] = {"unknown", "cancelled", "unknown", "= 42", "unknown", "ok",
                                "mesh complete", "utilities complete"};
      assert(!strcmp(result.action.text, expected[contract]));
      e.authenticated = false;
      assert(vm.start(42, e, error, sizeof(error)) && vm.poll(result) && !result.ok);
      assert(!vm.nextIo(request));
      printf("PASS shared ABI contract=%u operations=%u; copied outcome and native permission denial\n", contract, step);
    }
    {
      BotSession vm;
      BotVmLimits limits; limits.instructions = 100000000; limits.wallUs = 5000;
      load(vm, bytes("fault-0"), limits);
      const auto started = std::chrono::steady_clock::now();
      auto result = run(vm, event("!wfault"));
      assert(!result.ok && strstr(result.error, "deadline"));
      assert(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(100));
      printf("PASS real wall deadline interruption at %llu us/%u instructions\n",
             (unsigned long long)result.stats.elapsedUs, result.stats.instructions);
    }
  {
    BotSession events;
    load(events, bytes("c-events"));
    assert(events.subscriptions() == 9);
    std::atomic<uint32_t> epoch{8}; events.setEventEpoch(&epoch);
    BotEvent e; e.kind = BotEvent::Startup; e.eventEpoch = 8;
    e.node.uptimeMs = (uint64_t(1) << 40) + 2; e.sharedState = true; e.sharedGrant = 19;
    char error[128]{};
    assert(events.start(21, e, error, sizeof(error)));
    BotIoRequest request; BotIoResult io;
    assert(events.nextIo(request) && request.kind == BotIoRequest::Sleep && request.eventEpoch == 8);
    io.token = request.token; io.ok = true;
    assert(events.complete(io));
    assert(events.nextIo(request) && request.kind == BotIoRequest::Get &&
           request.scope == BotIoRequest::Bot && request.grant == 19);
    io.token = request.token; assert(events.complete(io));
    BotSession::Result result; assert(events.poll(result) && result.ok);
    assert(events.start(22, e, error, sizeof(error)));
    assert(events.nextIo(request)); io.token = request.token;
    epoch = 9;
    assert(events.complete(io) && events.poll(result) && !result.ok);
    assert(!events.nextIo(request) && !events.complete(io));
    puts("PASS Wasm typed u64 time, native shared scope and revoked event continuation");
  }
  BotSession vm;
  BotVmStats stats; char error[128]{};
  const auto source = bytes("c-arithmetic");
  BotManifest manifest;
  assert(BotVm::validate(source.data(), source.size(), stats, error, sizeof(error), {}, &manifest));
  assert(manifest.find("wadd"));
  BotAction action;
  assert(BotVm::invoke(source.data(), source.size(), event("!wadd 20 22"), action, stats, error, sizeof(error)));
  assert(!strcmp(action.text, "42"));
  assert(BotVm::invoke(source.data(), source.size(), event("!ping"), action, stats, error, sizeof(error)));
  assert(!strcmp(action.text, "Pong"));
  const auto notes = bytes("c-notes");
  assert(!BotVm::invoke(notes.data(), notes.size(), event("!wnote pending"), action, stats, error, sizeof(error)));
  assert(strstr(error, "retained BotSession"));
  const auto initLoop = bytes("fault-9");
  error[0] = 0;
  assert(!vm.load(initLoop.data(), initLoop.size(), 1, stats, error, sizeof(error)));
  assert(strstr(error, "instruction") || strstr(error, "deadline"));
  printf("PASS runaway initialization trapped after %u instructions: %s\n", stats.instructions, error);
  BotVmLimits small; small.heapBytes = 65536;
  assert(!vm.load(source.data(), source.size(), 1, stats, error, sizeof(error), small));
  auto bad = source; bad[0] = 'A';
  assert(!vm.load(bad.data(), bad.size(), 1, stats, error, sizeof(error)));
  auto import = source;
  auto at = import.find("meshcore_v1"); assert(at != std::string::npos); import[at] = 'x';
  assert(!vm.load(import.data(), import.size(), 1, stats, error, sizeof(error)));
  auto version = source; version[4] = 2;
  assert(!BotVm::validate(version.data(), version.size(), stats, error, sizeof(error)));
  assert(strstr(error, "Wasm") && !stats.wasmLinearBytes);
  BotSession healthy;
  load(healthy, source);
  std::vector<std::unique_ptr<BotSession>> allocated;
  bool exhausted = false;
  for (unsigned i = 0; i < 128; ++i) {
    auto candidate = std::make_unique<BotSession>();
    error[0] = 0;
    if (!candidate->load(source.data(), source.size(), 9, stats, error, sizeof(error))) {
      assert(error[0]); exhausted = true; break;
    }
    allocated.push_back(std::move(candidate));
  }
  assert(exhausted);
  const auto retained = run(healthy, event("!wadd 1 2"));
  assert(retained.ok);
  allocated.clear();
  printf("pool exhaustion leaves loaded arithmetic operational; session=%uB linear=%uB pool=%uB\n",
         retained.stats.wasmSessionBytes, retained.stats.wasmLinearBytes, retained.stats.wasmPoolBytes);
  mem_alloc_info_t memory{};
  assert(wasm_runtime_get_mem_alloc_info(&memory));
  printf("WAMR pool total=%u free=%u highmark=%u; PASS C/Rust arithmetic/notes/RPC, Lua coexistence, faults/stale handles\n",
         memory.total_size, memory.total_free_size, memory.highmark_size);
}
