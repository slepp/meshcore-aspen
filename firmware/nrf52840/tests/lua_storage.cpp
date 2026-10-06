// SPDX-License-Identifier: Apache-2.0
#include "PineFilesystem.h"
#include "platform/nvs.h"
#include "BotWorker.h"
#include "BotVm.h"
#include "BotStore.h"
#include "BotTimers.h"
#include "BotReminders.h"
#include "BotSettings.h"
#include "Clock.h"
#include "../../esp32/tests/bot_parser_cases.h"
#include <Utils.h>
#include <cassert>
#include <algorithm>
#include <vector>
#include <string>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <cstdlib>
#include <new>

extern "C" void *pvPortMalloc(size_t size) { return std::malloc(size); }
extern "C" void vPortFree(void *pointer) { std::free(pointer); }
static std::atomic<unsigned> failVmAllocationAfter{0};
static std::atomic<bool> failSourceAllocation{false};
void *operator new[](size_t size, const std::nothrow_t &) noexcept {
  if (failSourceAllocation.exchange(false)) return nullptr;
  try { return ::operator new[](size); }
  catch (const std::bad_alloc &) { return nullptr; }
}
void operator delete[](void *pointer, const std::nothrow_t &) noexcept {
  ::operator delete[](pointer);
}
extern "C" void *__real_calloc(size_t count, size_t size);
extern "C" void *__wrap_calloc(size_t count, size_t size) {
  if (count == 1 && size == onchip::BotSession::InitializationStorageBytes) {
    unsigned remaining = failVmAllocationAfter.load();
    if (remaining && failVmAllocationAfter.compare_exchange_strong(remaining, remaining - 1) && remaining == 1)
      return nullptr;
  }
  return __real_calloc(count, size);
}

namespace {
using namespace onchip;
std::atomic<uint32_t> utc{2000000000u};
struct Nor : nrfmast::NoteFlash {
  std::vector<uint8_t> bytes = std::vector<uint8_t>(0x200000, 0xff);
  int writes = 0, cut = -1;
  bool powered = true;
  bool begin() override { return powered; }
  bool partitionFree(uint32_t, uint32_t) override { return false; }
  bool retiredPartition(uint32_t, uint32_t) override { return false; }
  bool read(uint32_t at, void *out, size_t size) override {
    assert(at + size <= nrfmast::PineFilesystem::Bytes);
    if (!powered) return false;
    memcpy(out, bytes.data() + at, size); return true;
  }
  bool program(uint32_t at, const void *in, size_t size) override {
    assert(at + size <= nrfmast::PineFilesystem::Bytes);
    if (!powered) return false;
    const bool fault = ++writes == cut;
    const auto *data = static_cast<const uint8_t *>(in);
    for (size_t i = 0; i < (fault ? size / 2 : size); ++i) {
      assert((bytes[at + i] & data[i]) == data[i]);
      bytes[at + i] &= data[i];
    }
    if (fault) powered = false;
    return !fault;
  }
  bool erase(uint32_t at) override {
    assert(at % 4096 == 0 && at + 4096 <= nrfmast::PineFilesystem::Bytes);
    if (!powered) return false;
    const bool fault = ++writes == cut;
    std::fill(bytes.begin() + at, bytes.begin() + at + (fault ? 2048 : 4096), 0xff);
    if (fault) powered = false;
    return !fault;
  }
};
BotWorker::Result wait(BotWorker &worker) {
  const uint32_t deadline = millis() + 5000;
  BotWorker::Result result;
  while (!worker.poll(result)) { assert(int32_t(millis() - deadline) < 0); delay(1); }
  return result;
}
BotEvent commandEvent(const char *command) {
  BotEvent event;
  char error[128]{};
  assert(parseBotCommand(command, strlen(command), event, error, sizeof(error)));
  event.authenticated = true; event.sender[0] = 7; event.timestamp = utc;
  return event;
}
void invoke(BotWorker &worker, const char *command, unsigned job, const char *expected) {
  const auto event = commandEvent(command);
  assert(worker.invoke(event, job));
  const auto result = wait(worker);
  if (!result.ok) fprintf(stderr, "Pine worker invocation failed: %s\n", result.error);
  assert(result.ok && !strcmp(result.action.text, expected));
}
void metadataCuts(Nor &flash) {
  nvs_handle_t handle, second;
  assert(nvs_open("test", NVS_READWRITE, &handle) == ESP_OK);
  assert(nvs_open("test", NVS_READWRITE, &second) == ESP_ERR_INVALID_STATE);
  const char old[] = "old committed", next[] = "new committed";
  assert(nvs_set_blob(handle, "value", old, sizeof(old)) == ESP_OK && nvs_commit(handle) == ESP_OK);
  nvs_close(handle);
  const auto initial = flash.bytes;
  bool sawFailure = false, sawSuccess = false;
  for (int cut = 1; cut <= 24; ++cut) {
    assert(nrfmast::botFilesystem.end());
    flash.bytes = initial; flash.powered = true; flash.cut = -1;
    assert(nrfmast::botFilesystem.begin(flash, false));
    assert(nvs_open("test", NVS_READWRITE, &handle) == ESP_OK);
    flash.writes = 0; flash.cut = cut;
    bool ok = nvs_set_blob(handle, "value", next, sizeof(next)) == ESP_OK;
    if (ok) ok = nvs_commit(handle) == ESP_OK;
    sawSuccess |= ok; sawFailure |= !ok;
    nvs_close(handle);
    assert(nrfmast::botFilesystem.end());
    flash.powered = true; flash.cut = -1;
    assert(nrfmast::botFilesystem.begin(flash, false));
    assert(nvs_open("test", NVS_READONLY, &handle) == ESP_OK);
    char value[32]{}; size_t size = sizeof(value);
    assert(nvs_get_blob(handle, "value", value, &size) == ESP_OK);
    assert(!strcmp(value, old) || !strcmp(value, next));
    if (ok) assert(!strcmp(value, next));
    nvs_close(handle);
  }
  assert(sawSuccess && sawFailure);
  assert(!nrfmast::botFilesystem.open("/../notes", "w"));
  assert(!nrfmast::botFilesystem.open("/command-bot/../../notes", "w"));
  assert(std::all_of(flash.bytes.begin() + nrfmast::PineFilesystem::Bytes, flash.bytes.end(),
                     [](uint8_t byte) { return byte == 0xff; }));
  puts("Pine metadata: real LittleFS replacement, partial program/erase cuts, remount, writer exclusion, partition bounds");
}
void adaptiveSettings(Nor &flash) {
  bool enabled = true;
  assert(loadBotAdaptiveAdmission(enabled) && !enabled);
  assert(saveBotAdaptiveAdmission(true));
  assert(nrfmast::botFilesystem.end() && nrfmast::botFilesystem.begin(flash, false));
  assert(loadBotAdaptiveAdmission(enabled) && enabled);
  nvs_handle_t handle;
  assert(nvs_open("mc-onchip", NVS_READWRITE, &handle) == ESP_OK);
  const uint8_t corrupt[]{'B', 'A', 'A', 1, 2};
  assert(nvs_set_blob(handle, "bot-adaptive", corrupt, sizeof(corrupt)) == ESP_OK &&
         nvs_commit(handle) == ESP_OK);
  nvs_close(handle);
  assert(!loadBotAdaptiveAdmission(enabled));
  assert(saveBotAdaptiveAdmission(false) && loadBotAdaptiveAdmission(enabled) && !enabled);
  flash.writes = 0; flash.cut = 1;
  assert(!saveBotAdaptiveAdmission(true));
  assert(nrfmast::botFilesystem.end());
  flash.powered = true; flash.cut = -1;
  assert(nrfmast::botFilesystem.begin(flash, false));
  assert(loadBotAdaptiveAdmission(enabled) && !enabled);
  puts("Pine adaptive setting: default off, shared metadata persistence/readback, explicit corrupt/write failure and recovery");
}
void workerSources() {
  uint8_t key[32]{1};
  BotWorker worker;
  assert(worker.begin(key));
  const char *source =
      "function old() return 'old' end "
      "function put() kv.put('memo','saved') return 'written' end "
      "function get() return kv.get('memo') end "
      "function linger() mesh.wait{kind='text',timeout_ms=30000} return 'done' end";
  assert(worker.stage(source, strlen(source)) && wait(worker).ok);
  assert(worker.activate() && wait(worker).ok);
  invoke(worker, "!old", 1, "old");
  invoke(worker, "!put", 2, "written");
  const auto oldGeneration = worker.generation();
  const auto oldSourceGeneration = worker.sourceGeneration();
  failSourceAllocation = true;
  const char *replacement = "function replacement() return 'new' end";
  assert(worker.stage(replacement, strlen(replacement)));
  const auto allocationFailure = wait(worker);
  assert(!allocationFailure.ok && strstr(allocationFailure.error, "source recovery buffer unavailable") &&
         !worker.sourceSuspended() && worker.generation() == oldGeneration &&
         worker.sourceGeneration() == oldSourceGeneration);
  invoke(worker, "!old", 900, "old");
  BotIoRequest pending[2];
  unsigned nextPendingJob = 20;
  const auto suspend = [&] {
    for (unsigned i = 0; i < 2; ++i) {
      auto event = commandEvent(i ? "!admin bot status" : "!linger");
      event.owner = i != 0;
      assert(worker.invoke(event, nextPendingJob++));
    }
    const uint32_t deadline = millis() + 5000;
    for (auto &request : pending) {
      while (!worker.pollRadio(request)) { assert(int32_t(millis() - deadline) < 0); delay(1); }
      assert((request.kind == BotIoRequest::Wait || request.kind == BotIoRequest::Admin) &&
             request.token.generation == oldGeneration);
    }
    assert(pending[0].kind != pending[1].kind);
  };
  const auto cancelled = [&](bool staging) {
    unsigned seen = 0;
    BotWorker::Result staged;
    for (unsigned i = 0; i < (staging ? 3u : 2u); ++i) {
      const auto result = wait(worker);
      if (result.operation == BotWorker::Operation::Stage) {
        assert(staging); staged = result;
      } else {
        assert(result.operation == BotWorker::Operation::Invoke && !result.ok && result.error[0]);
        assert(result.job == pending[0].token.job || result.job == pending[1].token.job);
        seen |= result.job == pending[0].token.job ? 1u : 2u;
      }
    }
    assert(seen == 3);
    for (const auto &request : pending) {
      BotIoResult stale; stale.token = request.token; stale.ok = true;
      // Providers may retire already-running I/O, but must not revive cancelled jobs.
      worker.completeRadio(stale);
      assert(!worker.completeRadio(stale));
    }
    return staged;
  };
  suspend();
  worker.cancelJobs();
  cancelled(false);
  assert(worker.generation() == oldGeneration);
  suspend();
  assert(worker.stage("function broken(", 16));
  const auto rejected = cancelled(true);
  assert(!rejected.ok && rejected.error[0] && !worker.sourceSuspended());
  const auto restoredEpoch = worker.generation();
  assert(restoredEpoch != oldGeneration && worker.sourceGeneration() == oldSourceGeneration);
  invoke(worker, "!ping", 13, "Pong");
  invoke(worker, "!get", 3, "saved");
  const char *fresh = "function fresh() return 'new' end";
  assert(worker.stage(fresh, strlen(fresh)) && wait(worker).ok && worker.sourceSuspended());
  BotEvent event;
  assert(!worker.canInvoke(event));
  assert(worker.recoverSource() && wait(worker).ok && !worker.sourceSuspended());
  assert(worker.generation() != restoredEpoch && worker.sourceGeneration() == oldSourceGeneration);
  invoke(worker, "!old", 4, "old");
  assert(worker.stage(fresh, strlen(fresh)) && wait(worker).ok);
  char error[128]{};
  const auto publication = worker.reserveSourcePublication(&worker, false, error, sizeof(error));
  assert(publication && worker.sourcePublicationCurrent(publication));
  assert(!worker.stage(source, strlen(source)));
  assert(worker.activate(publication) && wait(worker).ok);
  worker.releaseSourcePublication(publication);
  assert(worker.generation() != oldGeneration);
  assert(worker.sourceGeneration() != oldSourceGeneration);
  for (const auto &request : pending) {
    BotIoResult stale; stale.token = request.token; stale.ok = true;
    assert(!worker.completeRadio(stale));
  }
  invoke(worker, "!fresh", 5, "new");
  for (const char *bad : {"while true do end",
                         "local t={} for i=1,10000 do t[i]={i,i,i,i} end function bad() end"}) {
    assert(worker.stage(bad, strlen(bad)) && !wait(worker).ok);
    assert(!worker.sourceSuspended());
    invoke(worker, "!fresh", 6, "new");
  }
  worker.stop();
  assert(worker.begin(key) && worker.stage(source, strlen(source)) && wait(worker).ok);
  assert(worker.activate() && wait(worker).ok);
  invoke(worker, "!get", 7, "saved");
  worker.stop();
  puts("Pine worker: suspended-job cancel/replacement fences, one-live-VM malformed/runaway/OOM recovery, native commands, publication and persistent KV restart");
}
void bundledSourceRecovery() {
  uint8_t key[32]{2};
  BotWorker worker;
  assert(worker.begin(key));
  assert(worker.stage(BotDefaultSource, strlen(BotDefaultSource)) && wait(worker).ok);
  assert(worker.activate() && wait(worker).ok);
  assert(worker.stage("function broken(", 16) && !wait(worker).ok && !worker.sourceSuspended());
  invoke(worker, "!ping", 910, "Pong");
  const char *replacement = "function replacement() return 'new' end";
  assert(worker.stage(replacement, strlen(replacement)) && wait(worker).ok);
  assert(worker.recoverSource() && wait(worker).ok && !worker.sourceSuspended());
  invoke(worker, "!ping", 911, "Pong");
  assert(worker.stage(replacement, strlen(replacement)) && wait(worker).ok);
  assert(worker.activate() && wait(worker).ok);
  invoke(worker, "!replacement", 912, "new");
  worker.stop();
  puts("Pine worker: bundled recovery reloads read-only source without a retained heap copy; custom replacement remains recoverable");
}
void failedSourceRecovery() {
  uint8_t key[32]{1};
  BotWorker worker;
  assert(worker.begin(key));
  const char *source =
      "count=0 function old() count=count+1 return tostring(count) end "
      "function state() return tostring(count) end "
      "function linger() mesh.wait{kind='text',timeout_ms=30000} count=count+100 return 'awake' end";
  assert(worker.stage(source, strlen(source)) && wait(worker).ok);
  assert(worker.activate() && wait(worker).ok);
  invoke(worker, "!old", 301, "1");
  const auto oldEpoch = worker.generation();
  const auto oldSourceGeneration = worker.sourceGeneration();
  assert(worker.invoke(commandEvent("!linger"), 302));
  BotIoRequest oldRequest;
  const uint32_t deadline = millis() + 5000;
  while (!worker.pollRadio(oldRequest)) { assert(int32_t(millis() - deadline) < 0); delay(1); }
  failVmAllocationAfter = 2;
  assert(worker.stage("function broken(", 16));
  bool rejected = false, cancelled = false;
  for (unsigned i = 0; i < 2; ++i) {
    const auto result = wait(worker);
    assert(!result.ok);
    if (result.operation == BotWorker::Operation::Stage) {
      assert(strstr(result.error, "prior Lua reload failed") &&
             strstr(result.error, "Retained VM storage unavailable"));
      rejected = true;
    } else {
      assert(result.operation == BotWorker::Operation::Invoke && result.job == 302);
      cancelled = true;
    }
  }
  assert(rejected && cancelled && worker.sourceSuspended() &&
         worker.generation() != oldEpoch && !worker.canInvoke(commandEvent("!ping")));
  failVmAllocationAfter = 1;
  assert(worker.recoverSource());
  const auto failed = wait(worker);
  assert(!failed.ok && strstr(failed.error, "Retained VM storage unavailable") && worker.sourceSuspended());
  assert(worker.recoverSource() && wait(worker).ok && !worker.sourceSuspended());
  assert(worker.sourceGeneration() == oldSourceGeneration);
  invoke(worker, "!state", 303, "0");
  assert(worker.invoke(commandEvent("!linger"), 302));
  BotIoRequest newRequest;
  const uint32_t nextDeadline = millis() + 5000;
  while (!worker.pollRadio(newRequest)) { assert(int32_t(millis() - nextDeadline) < 0); delay(1); }
  assert(newRequest.token.job == oldRequest.token.job &&
         newRequest.token.operation == oldRequest.token.operation &&
         newRequest.token.generation != oldRequest.token.generation);
  BotIoResult late;
  late.token = oldRequest.token; late.ok = true; late.packet.id = 1; late.packet.authenticated = true;
  strcpy(late.value, "stale");
  worker.completeRadio(late);
  assert(!worker.completeRadio(late));
  invoke(worker, "!ping", 304, "Pong");
  late.token = newRequest.token; strcpy(late.value, "current");
  assert(worker.completeRadio(late));
  const auto resumed = wait(worker);
  assert(resumed.ok && resumed.job == 302 && !strcmp(resumed.action.text, "awake"));
  invoke(worker, "!state", 305, "100");
  worker.stop();
  puts("Pine recovery: prior code/new runtime epoch, globals reset, failed restore/retry errors and same-job late RF completion fences");
}
void sourceParserValidation() {
  uint8_t key[32]{1};
  BotWorker worker;
  assert(worker.begin(key));
  const char source[] = "function prior() return 'retained' end";
  assert(worker.stage(source, strlen(source)) && wait(worker).ok);
  assert(worker.activate() && wait(worker).ok);
  const auto sourceGeneration = worker.sourceGeneration();
  for (bool global : {false, true}) {
    const auto nested = nestedBotFunctions(39, global);
    assert(worker.stage(nested.data(), nested.size()));
    const auto rejected = wait(worker);
    assert(!rejected.ok && strstr(rejected.error, "C stack overflow") &&
           !worker.sourceSuspended() && worker.sourceGeneration() == sourceGeneration);
    assert(worker.activate() && !wait(worker).ok);
    invoke(worker, "!prior", global ? 702 : 700, "retained");
    invoke(worker, "!ping", global ? 703 : 701, "Pong");
  }
  worker.stop();
  puts("Pine source parser: 39-level named/global rejection restores prior code and native command admission");
}
void deadlines() {
  uint8_t bot[32]{1};
  std::atomic<uint32_t> generation{1}, grant{1};
  std::atomic<bool> enabled{true}, stopping{false};
  BotIoRequest request;
  request.token = {1, 1, 1}; request.principal[0] = 7;
  request.kind = BotIoRequest::TimerSet; request.delaySeconds = 1; strcpy(request.key, "wake");
  BotIoResult result;
  {
    BotTimers timers;
    timers.perform(bot, request, result, generation, enabled, grant);
    assert(result.ok && result.timerState == BotTimerState::Pending);
    ++utc; request.kind = BotIoRequest::TimerWait;
    timers.perform(bot, request, result, generation, enabled, grant);
    assert(result.ok && result.timerState == BotTimerState::Claimed);
  }
  {
    BotTimers restarted;
    request.kind = BotIoRequest::TimerGet;
    restarted.perform(bot, request, result, generation, enabled, grant);
    assert(result.ok && result.timerState == BotTimerState::Claimed && !result.pending);
  }
  BotReminderDispatch dispatch;
  request.kind = BotIoRequest::ReminderSet; request.delaySeconds = 1; request.grant = 1;
  strcpy(request.value, "remember");
  char error[128]{};
  {
    BotReminders reminders;
    reminders.perform(bot, request, result, generation, enabled, grant, stopping, 0);
    assert(result.ok && result.reminderState == BotReminderState::Pending);
    ++utc;
    assert(reminders.next(bot, dispatch, stopping, error, sizeof(error)));
    dispatch.grant = grant;
    assert(reminders.claim(bot, dispatch, enabled, grant, stopping, error, sizeof(error)));
  }
  {
    BotReminders restarted;
    assert(!restarted.next(bot, dispatch, stopping, error, sizeof(error)));
  }
  puts("Pine deadlines: durable timer claim and reminder unknown claim survive restart without rearm");
}
void readFailure(Nor &flash) {
  auto file = nrfmast::botFilesystem.open("/command-bot/read-fault", "w");
  uint8_t bytes[1024]{};
  assert(file && file.write(bytes, sizeof(bytes)) == sizeof(bytes));
  file.flush(); file.close();
  assert(nrfmast::botFilesystem.end());
  assert(nrfmast::botFilesystem.begin(flash, false));
  file = nrfmast::botFilesystem.open("/command-bot/read-fault", "r");
  assert(file);
  flash.powered = false;
  assert(file.read(bytes, sizeof(bytes)) != sizeof(bytes));
  assert(!nrfmast::botFilesystem.ready());
  nvs_handle_t handle;
  assert(nvs_open("test", NVS_READONLY, &handle) == ESP_FAIL);
  file.close();
  assert(nrfmast::botFilesystem.end());
  flash.powered = true;
  assert(nrfmast::botFilesystem.begin(flash, false));
  file = nrfmast::botFilesystem.open("/command-bot/read-fault", "r");
  assert(file && file.read(bytes, sizeof(bytes)) == sizeof(bytes));
  file.close();
  assert(nvs_open("fault", NVS_READWRITE, &handle) == ESP_OK);
  assert(nvs_set_blob(handle, "value", bytes, sizeof(bytes)) == ESP_OK && nvs_commit(handle) == ESP_OK);
  nvs_close(handle);
  assert(nrfmast::botFilesystem.end() && nrfmast::botFilesystem.begin(flash, false));
  assert(nvs_open("fault", NVS_READONLY, &handle) == ESP_OK);
  flash.powered = false;
  size_t size = sizeof(bytes);
  assert(nvs_get_blob(handle, "value", bytes, &size) == ESP_FAIL);
  assert(!nrfmast::botFilesystem.ready());
  nvs_close(handle);
  assert(nrfmast::botFilesystem.end());
  flash.powered = true;
  assert(nrfmast::botFilesystem.begin(flash, false));
  puts("Pine filesystem: failed reads block metadata access; remount retains committed files");
}
}
namespace onchip {
bool trustedNetworkTime(uint32_t &early, uint32_t &late, const char **reason) {
  early = late = utc; if (reason) *reason = nullptr; return early != 0;
}
}
int main() {
  Nor flash;
  assert(nrfmast::botFilesystem.begin(flash, true));
  assert(nrfmast::botFilesystem.mkdir("/metadata/inventory"));
  auto inventoryFile = nrfmast::botFilesystem.open("/metadata/inventory/record", "w");
  const uint8_t inventoryBytes[] = {1, 2, 3};
  assert(inventoryFile && inventoryFile.write(inventoryBytes, sizeof(inventoryBytes)) == sizeof(inventoryBytes));
  inventoryFile.close();
  unsigned inventoried = 0;
  assert(nrfmast::botFilesystem.visit("/metadata", [](const char *path, void *context) {
    assert(!strcmp(path, "/metadata/inventory/record"));
    ++*static_cast<unsigned *>(context);
    return true;
  }, &inventoried));
  assert(inventoried == 1);
  assert(!nrfmast::botFilesystem.visit("/metadata", [](const char *, void *) { return false; }, nullptr));
  assert(nrfmast::botFilesystem.remove("/metadata/inventory/record"));
  assert(nrfmast::botFilesystem.remove("/metadata/inventory"));
  assert(nrfmast::botFilesystem.totalBytes() == nrfmast::PineFilesystem::Bytes);
  assert(nrfmast::botFilesystem.usedBytes() < nrfmast::botFilesystem.totalBytes());
  metadataCuts(flash);
  adaptiveSettings(flash);
  workerSources();
  bundledSourceRecovery();
  failedSourceRecovery();
  sourceParserValidation();
  deadlines();
  readFailure(flash);
  assert(nrfmast::botFilesystem.end());
}
