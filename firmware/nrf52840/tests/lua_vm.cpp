// SPDX-License-Identifier: Apache-2.0
#define ONCHIP_BOT_VM_LIBRARY_TEST 1
#include "../../esp32/tests/bot_vm.cpp"
#include "../../esp32/tests/bot_parser_cases.h"
#include <iostream>
#include <iomanip>
#include <cstdlib>
#include <new>
static size_t nothrowBlockLimit = SIZE_MAX, nothrowCalls, failedNothrowCall;
void *operator new(size_t size, const std::nothrow_t &) noexcept {
  if (++nothrowCalls == failedNothrowCall || size > nothrowBlockLimit) return nullptr;
  try { return ::operator new(size); }
  catch (const std::bad_alloc &) { return nullptr; }
}
void operator delete(void *memory, const std::nothrow_t &) noexcept {
  ::operator delete(memory);
}
static size_t physicalLive, physicalPeak;
void onchipBotVmHeapModel(size_t oldSize, size_t newSize) {
  const auto cost = [](size_t size) { return size ? ((size + 7) & ~size_t(7)) + 8 : 0; };
  physicalLive = physicalLive - cost(oldSize) + cost(newSize);
  if (!physicalLive) physicalPeak = 0;
  if (physicalLive > physicalPeak) physicalPeak = physicalLive;
}
static void parserStackValidation() {
  static_assert(LUAI_MAXCCALLS == 32, "Use the staged Pine parser limit");
  BotSession session;
  BotVmStats stats;
  char error[128]{};
  for (bool global : {false, true}) {
    const auto accepted = nestedBotFunctions(28, global);
    assert(session.load(accepted.data(), accepted.size(), 1, stats, error, sizeof(error)));
    assert(session.manifest().count == 1 && session.manifest().find("nesting"));
    const auto nested = nestedBotFunctions(39, global);
    assert(nested.size() <= BotSourceLimit);
    assert(!session.load(nested.data(), nested.size(), 2, stats, error, sizeof(error)) &&
           strstr(error, "C stack overflow") && !session.manifest().count);
    assert(session.load(BotDefaultSource, strlen(BotDefaultSource), 3, stats, error, sizeof(error)));
    assert(session.start(1, event("!ping"), error, sizeof(error)));
    BotSession::Result result;
    assert(session.poll(result) && result.ok && !strcmp(result.action.text, "Pong"));
  }
  puts("Pine parser stack: compact retained 28-level named/global functions accepted, 39-level sources rejected, native commands recover");
}
static void customCapacity() {
  std::ifstream file("../esp32/tests/bot_large.lua");
  assert(file);
  std::string source((std::istreambuf_iterator<char>(file)), {});
  source += " _station=station function station(index) sleep(1) return _station(index) end";
  assert(source.size() <= BotSourceLimit);
  source.append(BotSourceLimit - source.size(), ' ');
  BotSession session;
  BotVmStats stats;
  char error[128]{};
  assert(!session.load(source.data(), source.size(), 1, stats, error, sizeof(error)) &&
         strstr(error, "Lua heap limit"));
  BotVmLimits probe;
  probe.heapBytes = 96 * 1024;
  assert(session.load(source.data(), source.size(), 1, stats, error, sizeof(error), probe));
  printf("Pine dense-catalog constraint: source=%zuB exports=%u rows=32 required_load_peak=%zuB allocator_model_peak=%zuB exceeds_production_cap=%zuB; diagnostic_only_quota=%zuB\n",
         source.size(), session.manifest().count, stats.peakBytes, physicalPeak,
         BotVmLimits{}.heapBytes, probe.heapBytes);
  session.clear();
  file = std::ifstream("../runtime/plugins/examples/lab.lua");
  assert(file);
  source.assign(std::istreambuf_iterator<char>(file), {});
  const bool loaded = session.load(source.data(), source.size(), 2, stats, error, sizeof(error));
  if (!loaded) fprintf(stderr, "Pine persistence/timer package rejected: %s\n", error);
  assert(loaded && session.manifest().count == 6);
  const size_t loadPeak = stats.peakBytes;
  assert(session.start(1, event("!alarm wake 1"), error, sizeof(error)));
  BotIoRequest custom, native;
  assert(session.nextIo(custom) && custom.kind == BotIoRequest::TimerSet);
  assert(session.start(2, event("!recall memo"), error, sizeof(error)));
  assert(session.nextIo(native) && native.kind == BotIoRequest::Get);
  BotSession::Result result;
  assert(!session.poll(result));
  BotIoResult done;
  done.token = custom.token; done.ok = done.timeTrusted = true;
  done.timerState = BotTimerState::Pending; done.deadlineUtc = 2000000001u; done.revision = 1;
  assert(session.complete(done) && session.nextIo(custom) && custom.kind == BotIoRequest::TimerWait);
  done = {};
  done.token = native.token; done.ok = done.found = true;
  strcpy(done.value, "Bench note");
  assert(session.complete(done) && session.poll(result) && result.ok && result.job == 2 &&
         strstr(result.action.text, "Bench note"));
  size_t jobPeak = result.stats.peakBytes;
  done = {}; done.token = custom.token; done.ok = done.timeTrusted = true;
  done.timerState = BotTimerState::Claimed; done.deadlineUtc = 2000000001u; done.revision = 1;
  assert(session.complete(done) && session.poll(result) && result.ok && result.job == 1 &&
         !strcmp(result.action.text, "Timer wake claimed"));
  jobPeak = std::max(jobPeak, size_t(result.stats.peakBytes));
  assert(jobPeak <= BotVmLimits{}.heapBytes);
  printf("Pine retained lab package: source=%zuB exports=%u scoped_KV=1 durable_timers=1 channel_wait=1 load_peak=%zuB concurrent_timer_native_note_peak=%zuB cap=%zuB spare=%zuB allocator_model_peak=%zuB\n",
         source.size(), session.manifest().count, loadPeak, jobPeak,
         BotVmLimits{}.heapBytes, BotVmLimits{}.heapBytes - jobPeak, physicalPeak);
}

static void fragmentedJobBuffers() {
  nothrowBlockLimit = (BotSession::StorageBytes - BotSession::InitializationStorageBytes) / BotJobLimit;
  BotSession session;
  BotVmStats stats;
  char error[128]{};
  assert(session.load(BotDefaultSource, strlen(BotDefaultSource), 1, stats, error, sizeof(error)));
  assert(session.start(1, event("!ping"), error, sizeof(error)));
  assert(session.start(2, event("!ping"), error, sizeof(error)));
  BotSession::Result result;
  assert(session.poll(result) && result.ok && !strcmp(result.action.text, "Pong"));
  assert(session.poll(result) && result.ok && !strcmp(result.action.text, "Pong"));
  session.clear();
  failedNothrowCall = nothrowCalls + 2;
  assert(!session.load(BotDefaultSource, strlen(BotDefaultSource), 2, stats, error, sizeof(error)) &&
         strstr(error, "Lua job buffer") && !session.manifest().count);
  failedNothrowCall = 0;
  assert(session.load(BotDefaultSource, strlen(BotDefaultSource), 3, stats, error, sizeof(error)));
  nothrowBlockLimit = SIZE_MAX;
  puts("Pine fragmented heap: separate job-sized blocks retain both slots; partial allocation failure cleans up and reloads");
}

int main() {
  static_assert(sizeof(void *) == 4 && alignof(double) == 8, "Run Pine validation with -m32 -malign-double");
  static_assert(BotJobLimit == 2 && BotValueLimit == 128 && BotTransactionLimit == 2);
  assert(BotSession::InitializationStorageBytes < BotSession::StorageBytes);
  fragmentedJobBuffers();
  if (std::getenv("PINE_DUMP_REGISTRY")) {
    BotManifest manifest;
    BotVmStats stats;
    char error[128]{};
    assert(BotVm::validate(BotDefaultSource, strlen(BotDefaultSource), stats, error, sizeof(error), {}, &manifest));
    assert(!manifest.moduleCount && !manifest.eventMask);
    std::cout << "// SPDX-License-Identifier: Apache-2.0\n"
                 "// Generated by PINE_DUMP_REGISTRY=1 firmware/nrf52840/.build/native/lua-vm.\n"
                 "// Compact session initialization verifies every entry against the Lua declarations.\n"
                 "#pragma once\n#include \"BotRegistry.h\"\nnamespace onchip {\n"
                 "constexpr BotCommand BotBuiltinCommands[] = {\n";
    for (unsigned i = 0; i < manifest.count; ++i) {
      const auto &command = manifest.commands[i];
      std::cout << "  {BotCommand::" << (command.permission == BotCommand::Public ? "Public" :
          command.permission == BotCommand::Private ? "Private" :
          command.permission == BotCommand::Owner ? "Owner" :
          command.permission == BotCommand::Channel ? "Channel" :
          command.permission == BotCommand::Shared ? "Shared" :
          command.permission == BotCommand::Reminder ? "Reminder" : "Home");
      for (const char *field : {command.name, command.function, command.schema, command.help, command.example})
        std::cout << ", " << std::quoted(field);
      std::cout << "},\n";
    }
    std::cout << "};\nconstexpr BotManifestView BotBuiltinManifest{BotBuiltinCommands,\n"
                 "  sizeof(BotBuiltinCommands) / sizeof(BotBuiltinCommands[0]),\n"
                 "  sizeof(BotBuiltinCommands) / sizeof(BotBuiltinCommands[0])};\n}\n";
    return 0;
  }
  registry();
  commandOverrides();
  boundedJson();
  commandDiscoveryAndMeshGrants();
  subscriptionVm();
  packageModules();
  diagnosticCommands();
  channelBoard();
  utilityCommands();
  personalNotesApi();
  personalReminderApi();
  packetContinuations();
  scopedStorage();
  namedThreadScopes();
  retained();
  manifestLifetimes();
  initializationBudgets();
  parserStackValidation();
  customCapacity();
  BotSession session;
  BotVmStats stats;
  char error[128]{};
  for (const char *body : {
      "http.get('weather')", "http.post('service',{})",
      "rpc.call('service','echo',{})", "rpc.call('home','health',{})"}) {
    const auto source = program(body);
    assert(session.load(source.data(), source.size(), 1, stats, error, sizeof(error)));
    assert(session.start(1, event("!custom"), error, sizeof(error)));
    BotSession::Result result;
    assert(session.poll(result) && !result.ok && strstr(result.error, "unsupported"));
  }
  const auto formatter = program("if rpc.text then return 'present' end");
  assert(session.load(formatter.data(), formatter.size(), 1, stats, error, sizeof(error)));
  assert(session.start(1, event("!custom"), error, sizeof(error)));
  BotSession::Result formatted;
  assert(session.poll(formatted) && formatted.ok && !strcmp(formatted.action.text, "present"));
  assert(session.load(BotDefaultSource, strlen(BotDefaultSource), 2, stats, error, sizeof(error)));
  assert(session.start(1, event("!weather"), error, sizeof(error)));
  BotSession::Result unsupported;
  assert(session.poll(unsupported) && !unsupported.ok && strstr(unsupported.error, "unsupported"));
  printf("Pine profile retained bundled load: peak=%zuB, heap limit=%zuB, session=%zuB, manifest=%zuB\n",
         stats.peakBytes, BotVmLimits{}.heapBytes, BotSession::StorageBytes, sizeof(BotManifest));
  printf("Pine bundled allocator model (8-byte alignment/header): peak=%zuB live=%zuB\n", physicalPeak, physicalLive);
  puts("Pine Lua API: shared typed commands/help/modules/events/jobs/grants/scopes/KV/JSON/utilities/mesh/timers/reminders; explicit network denial");
}
