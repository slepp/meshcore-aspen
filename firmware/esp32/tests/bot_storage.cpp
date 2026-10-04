// SPDX-License-Identifier: Apache-2.0
#include "BotStore.h"
#include "BotTimers.h"
#include "BotJournal.h"
#include "BotScheduleFiles.h"
#include "bot_kv_fixture.h"
#include "BotWorker.h"
#include "Clock.h"
#include <nvs.h>
#include <esp_heap_caps.h>
#include <cassert>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

using namespace onchip;
static std::atomic<unsigned long> now{0};
unsigned long millis() { return now; }
void delay(unsigned long ms) { now += ms; }
static constexpr uint32_t Epoch = 1767225600;
static void clockAt(uint32_t epoch = Epoch) {
  receiveNetworkTime(epoch);
  loopClocks();
}
static void reset() {
  assert(identity_test::handles.empty());
  identity_test::durable.clear();
  filesystem_test::files.clear();
  identity_test::failRead = identity_test::failWrite = identity_test::failCommit = false;
  identity_test::eagerWrites = false;
  identity_test::failStats = false; identity_test::freeEntries = 4096;
  identity_test::entryCapacity = identity_test::peakEntries = 0;
  identity_test::namespaces.clear();
  identity_test::afterWrite = identity_test::afterCommit = nullptr;
  identity_test::readHook = nullptr;
  now = 0;
  beginClocks(); beginNetworkClock(true);
}
static void storage() {
  reset();
  BotStore store;
  uint8_t bot[32]{1};
  std::atomic<uint32_t> generation{1}, grant{1};
  std::atomic<bool> shared{true};
  BotIoRequest request;
  request.kind = BotIoRequest::Put; request.token = {1, 1, 1}; request.grant = 1;
  request.scope = BotIoRequest::Channel; request.principal[0] = 7;
  strcpy(request.key, "memory"); strcpy(request.value, "channel memory");
  BotIoResult result;
  const auto run = [&]() { store.perform(bot, request, result, generation, shared, grant); };
  run(); assert(result.ok);
  request.kind = BotIoRequest::Get;
  request.principal[31] = 8; run(); assert(result.ok && !result.found);
  request.principal[31] = 0; request.scope = BotIoRequest::Caller;
  run(); assert(result.ok && !result.found);
  request.scope = BotIoRequest::Conversation; run(); assert(result.ok && !result.found);
  request.scope = BotIoRequest::Channel; bot[1] = 2;
  run(); assert(result.ok && !result.found);
  bot[1] = 0; run(); assert(result.ok && result.found && !strcmp(result.value, "channel memory"));
  shared = false; run(); assert(!result.ok);
  shared = true; ++grant; run(); assert(!result.ok && strstr(result.error, "grant"));
  request.grant = grant; run(); assert(result.ok && result.found);
  request.kind = BotIoRequest::Put;
  identity_test::failWrite = true; run();
  assert(!result.ok && strstr(result.error, "outcome unknown"));
  identity_test::failWrite = false;
  identity_test::afterCommit = [] { identity_test::failRead = true; };
  strcpy(request.value, "committed readback lost"); run();
  assert(!result.ok && strstr(result.error, "outcome unknown"));
  identity_test::afterCommit = nullptr; identity_test::failRead = false;
  request.kind = BotIoRequest::Get; run();
  assert(result.ok && !strcmp(result.value, "committed readback lost"));
  identity_test::readHook = [](const char *key) { if (!strcmp(key, "txn")) now += 2000; };
  run(); assert(!result.ok && strstr(result.error, "read cancelled/deadline"));
  identity_test::readHook = nullptr;
  static std::atomic<uint32_t> *cancelGeneration;
  cancelGeneration = &generation;
  identity_test::afterWrite = [] { ++*cancelGeneration; };
  request.kind = BotIoRequest::Put;
  run(); assert(!result.ok && strstr(result.error, "cancelled during write; outcome unknown"));
  identity_test::afterWrite = nullptr;
  request.token.generation = generation;
  for (auto scope : {BotIoRequest::Caller, BotIoRequest::Conversation}) {
    request.scope = scope; request.kind = BotIoRequest::Put;
    for (unsigned i = 0; i < 8; ++i) {
      snprintf(request.key, sizeof(request.key), "key%u", i); run(); assert(result.ok);
    }
    strcpy(request.key, "overflow"); run(); assert(!result.ok && strstr(result.error, "capacity"));
  }
  puts("PASS KV scopes: full channel/bot identities, DM isolation, per-scope quotas, grant epochs and readback uncertainty");
}
static void listStorage() {
  reset();
  BotStore store;
  uint8_t bot[32]{1};
  std::atomic<uint32_t> generation{1}, grant{1};
  std::atomic<bool> shared{true};
  BotIoRequest request; request.token = {1,1,1}; request.grant = 1; request.principal[0] = 7;
  BotIoResult result;
  const auto run = [&] { store.perform(bot, request, result, generation, shared, grant); };
  request.kind = BotIoRequest::List;
  run(); assert(result.ok && !result.found && !result.keys.count);
  for (const char *key : {"zebra", "alpha", "beta", "alphabet"}) {
    request.kind = BotIoRequest::Put; strcpy(request.key, key); strcpy(request.value, "private value");
    run(); assert(result.ok && !result.found);
    run(); assert(result.ok && result.found);
  }
  const auto saved = identity_test::durable;
  request.kind = BotIoRequest::List; request.key[0] = 0;
  run(); assert(result.ok && result.found && result.keys.count == 4 && !result.truncated);
  assert(!strcmp(result.keys.keys[0], "alpha") && !strcmp(result.keys.keys[1], "alphabet") &&
         !strcmp(result.keys.keys[2], "beta") && !strcmp(result.keys.keys[3], "zebra") && !result.value[0]);
  strcpy(request.key, "alph"); run();
  assert(result.ok && result.keys.count == 2 && !strcmp(result.keys.keys[1], "alphabet") &&
         !result.keys.keys[2][0]);
  strcpy(request.key, "absent"); run(); assert(result.ok && !result.keys.count);
  request.key[0] = 0; request.principal[31] = 1; run(); assert(result.ok && !result.keys.count);
  request.principal[31] = 0; bot[31] = 1; run(); assert(result.ok && !result.keys.count); bot[31] = 0;
  request.scope = BotIoRequest::Conversation; run(); assert(result.ok && !result.keys.count);
  request.scope = BotIoRequest::Channel; run(); assert(result.ok && !result.keys.count);
  request.scope = BotIoRequest::Caller;
  assert(identity_test::durable == saved);
  {
    BotStore reboot;
    ++generation; request.token.generation = generation;
    reboot.perform(bot, request, result, generation, shared, grant);
    assert(result.ok && result.keys.count == 4 && !strcmp(result.keys.keys[0], "alpha"));
  }
  request.kind = BotIoRequest::Delete; strcpy(request.key, "beta"); run();
  assert(result.ok && result.found);
  const auto commits = identity_test::commits;
  run(); assert(result.ok && !result.found && identity_test::commits == commits);
  request.kind = BotIoRequest::List; request.key[0] = 0; run();
  assert(result.ok && result.keys.count == 3);
  for (unsigned i = 0; i < 5; ++i) {
    request.kind = BotIoRequest::Put;
    const auto key = std::string(31, 'm') + char('a' + i);
    strcpy(request.key, key.c_str()); memset(request.value, 'x', BotValueLimit); request.value[BotValueLimit] = 0;
    run(); assert(result.ok);
  }
  strcpy(request.key, "ninth"); run(); assert(!result.ok && strstr(result.error, "capacity"));
  request.kind = BotIoRequest::List; request.key[0] = 0; run();
  assert(result.ok && result.keys.count == BotKeysPerScope && !result.truncated &&
         strlen(result.keys.keys[2]) == BotKeyLimit);
  strcpy(request.key, std::string(31, 'm').c_str()); run();
  assert(result.ok && result.keys.count == 5);
  identity_test::failRead = true; run(); assert(!result.ok && strstr(result.error, "unavailable"));
  identity_test::failRead = false;
  identity_test::readHook = [](const char *key) { if (!strcmp(key, "txn")) now += 2000; };
  run(); assert(!result.ok && strstr(result.error, "deadline")); identity_test::readHook = nullptr;
  static std::atomic<uint32_t> *changing;
  changing = &generation;
  identity_test::readHook = [](const char *key) { if (!strcmp(key, "txn")) ++*changing; };
  run(); assert(!result.ok && strstr(result.error, "cancelled")); identity_test::readHook = nullptr;
  request.token.generation = generation;
  request.key[0] = 0; request.scope = BotIoRequest::Bot; memset(request.principal, 0, 32);
  request.kind = BotIoRequest::Put; strcpy(request.key, "owner"); strcpy(request.value, "reserved"); run(); assert(result.ok);
  request.kind = BotIoRequest::List; request.key[0] = 0; run();
  assert(result.ok && result.keys.count == 1 && !strcmp(result.keys.keys[0], "owner"));
  shared = false; run(); assert(!result.ok && strstr(result.error, "grant"));
  shared = true; ++grant; run(); assert(!result.ok && strstr(result.error, "grant"));
  request.grant = grant;
  request.scope = BotIoRequest::Channel; request.principal[0] = 5;
  request.kind = BotIoRequest::Put; strcpy(request.key, "board"); run(); assert(result.ok);
  request.kind = BotIoRequest::List; request.key[0] = 0; run();
  assert(result.ok && result.keys.count == 1 && !strcmp(result.keys.keys[0], "board"));
  request.principal[31] = 2; run(); assert(result.ok && result.keys.count == 0);
  request.principal[31] = 0;
  changing = &grant;
  identity_test::readHook = [](const char *key) { if (!strcmp(key, "txn")) ++*changing; };
  run(); assert(!result.ok && strstr(result.error, "cancelled")); identity_test::readHook = nullptr;
  request.grant = grant;
  memset(request.key, 'x', sizeof(request.key)); run(); assert(!result.ok && strstr(result.error, "bounded"));
  request.key[0] = 0;
  const auto empty = kv_fixture::read(9);
  kv_fixture::replace(9, kv_fixture::read(8));
  run(); assert(!result.ok && strstr(result.error, "duplicate"));
  kv_fixture::replace(9, empty);
  auto &raw = kv_fixture::file(0);
  raw.back() ^= 1; run(); assert(!result.ok && strstr(result.error, "corrupt")); raw.back() ^= 1;
  puts("PASS KV list: sorted bounded snapshot/prefix, full-key/bot/scope isolation, source/reboot, quotas, grant/read/deadline/cancel failures and duplicate journal rejection");
}
static void durableTimers() {
  reset();
  BotTimers timers;
  uint8_t bot[32]{1};
  std::atomic<uint32_t> generation{1}, grant{1};
  std::atomic<bool> shared{true};
  BotIoRequest request;
  request.token = {1, 1, 1}; request.grant = 1; request.principal[0] = 7;
  request.kind = BotIoRequest::TimerSet; request.delaySeconds = 5;
  strcpy(request.key, "named");
  BotIoResult result;
  const auto run = [&]() { timers.perform(bot, request, result, generation, shared, grant); };
  run(); assert(!result.ok && strstr(result.error, "SNTP"));
  companionClock().setCurrentTime(Epoch + 100);
  run(); assert(!result.ok && identity_test::durable.empty());
  clockAt();
  request.delaySeconds = BotTimerMaximumSeconds + 1; run(); assert(!result.ok);
  request.delaySeconds = 5; run();
  assert(result.ok && result.timerState == BotTimerState::Pending && result.deadlineUtc == Epoch + 6);
  const auto firstRevision = result.revision;
  request.kind = BotIoRequest::TimerWait; run();
  assert(result.ok && result.pending && result.revision == firstRevision);
  request.kind = BotIoRequest::TimerGet; request.principal[31] = 8;
  run(); assert(result.ok && result.timerState == BotTimerState::Missing);
  request.principal[31] = 0; request.scope = BotIoRequest::Conversation;
  run(); assert(result.ok && result.timerState == BotTimerState::Missing);
  request.scope = BotIoRequest::Caller; bot[1] = 2;
  run(); assert(result.ok && !result.found); bot[1] = 0;
  identity_test::readHook = [](const char *key) { if (!strcmp(key, "files")) now += BotScheduleRecoveryBudgetMs; };
  run(); assert(!result.ok && strstr(result.error, "read deadline"));
  identity_test::readHook = nullptr;
  beginClocks(); beginNetworkClock(true);
  run(); assert(result.ok && result.found && !result.timeTrusted);
  request.kind = BotIoRequest::TimerWait;
  run(); assert(result.ok && result.pending && !result.timeTrusted);
  clockAt(Epoch + 6);
  run(); assert(result.ok && !result.pending && result.timerState == BotTimerState::Claimed);
  run(); assert(!result.ok && strstr(result.error, "already claimed"));
  request.kind = BotIoRequest::TimerCancel; run();
  assert(result.ok && result.timerState == BotTimerState::Claimed);
  request.kind = BotIoRequest::TimerSet; run();
  assert(result.ok && result.replaced && result.revision > firstRevision);
  request.revision = result.revision; request.kind = BotIoRequest::TimerWait; run();
  assert(result.pending);
  request.kind = BotIoRequest::TimerSet; run(); assert(result.ok);
  request.kind = BotIoRequest::TimerWait; run();
  assert(!result.ok && strstr(result.error, "replaced"));
  request.revision = 0; request.kind = BotIoRequest::TimerCancel; run();
  assert(result.ok && result.timerState == BotTimerState::Cancelled);
  request.kind = BotIoRequest::TimerWait; run(); assert(!result.ok && strstr(result.error, "cancelled"));
  request.kind = BotIoRequest::TimerSet; run(); assert(result.ok);
  clockAt(result.deadlineUtc + BotTimerOverdueSeconds + 1);
  request.kind = BotIoRequest::TimerWait; run();
  assert(!result.ok && result.timerState == BotTimerState::Overdue);
  request.kind = BotIoRequest::TimerGet; run(); assert(result.ok && result.timerState == BotTimerState::Overdue);
  request.kind = BotIoRequest::TimerSet; run(); assert(result.ok);
  clockAt(result.deadlineUtc);
  request.kind = BotIoRequest::TimerWait;
  identity_test::afterCommit = [] { identity_test::failRead = true; };
  run(); assert(!result.ok && strstr(result.error, "outcome unknown"));
  identity_test::afterCommit = nullptr; identity_test::failRead = false;
  run(); assert(!result.ok && strstr(result.error, "already claimed"));
  request.kind = BotIoRequest::TimerSet;
  identity_test::failCommit = true;
  run(); assert(!result.ok && strstr(result.error, "outcome unknown"));
  identity_test::failCommit = false; request.kind = BotIoRequest::TimerGet;
  run(); assert(result.ok && result.timerState == BotTimerState::Claimed);
  request.kind = BotIoRequest::TimerSet;
  run(); assert(result.ok);
  clockAt(result.deadlineUtc);
  request.kind = BotIoRequest::TimerWait;
  identity_test::afterCommit = [] { clockAt(Epoch + 10000); };
  run(); assert(!result.ok && strstr(result.error, "clock no longer"));
  identity_test::afterCommit = nullptr;
  run(); assert(!result.ok && strstr(result.error, "already claimed"));
  request.kind = BotIoRequest::TimerSet;
  identity_test::afterCommit = [] { now += 2000; };
  run(); assert(!result.ok && strstr(result.error, "committed after deadline"));
  identity_test::afterCommit = nullptr;
  clockAt();
  request.kind = BotIoRequest::TimerGet; run(); assert(result.ok && result.found);
  ++generation; run(); assert(!result.ok && strstr(result.error, "cancelled"));
  assert(identity_test::handles.empty());
  puts("PASS durable deadlines: reboot/unsynced hold, one-consumer claim, replace/cancel, overdue and uncertain commit");
}
static void timerLimits() {
  reset(); clockAt();
  BotTimers timers;
  uint8_t bot[32]{1};
  std::atomic<uint32_t> generation{1}, grant{1};
  std::atomic<bool> shared{true};
  BotIoRequest request;
  request.token = {1, 1, 1}; request.grant = 1;
  request.kind = BotIoRequest::TimerSet; request.scope = BotIoRequest::Channel;
  request.delaySeconds = 5;
  BotIoResult result;
  const auto run = [&]() { timers.perform(bot, request, result, generation, shared, grant); };
  for (unsigned caller = 0; caller < 4; ++caller) {
    request.principal[0] = caller;
    for (unsigned n = 0; n < 2; ++n) {
      snprintf(request.key, sizeof(request.key), "timer%u", n); run(); assert(result.ok);
    }
    strcpy(request.key, "extra"); run(); assert(!result.ok && strstr(result.error, "quota"));
  }
  request.principal[0] = 5; run(); assert(!result.ok && strstr(result.error, "capacity"));
  request.principal[0] = 0; strcpy(request.key, "timer0");
  request.kind = BotIoRequest::TimerCancel; run(); assert(result.ok);
  const auto cancelledRevision = result.revision;
  request.principal[0] = 5; strcpy(request.key, "reused"); request.kind = BotIoRequest::TimerSet;
  run(); assert(result.ok && result.revision > cancelledRevision);
  ++grant; request.kind = BotIoRequest::TimerGet;
  run(); assert(!result.ok && strstr(result.error, "grant"));
  request.grant = grant;
  auto &raw = identity_test::durable.begin()->second;
  raw.back() ^= 1; run(); assert(!result.ok && strstr(result.error, "corrupt"));
  raw.back() ^= 1;
  puts("PASS timer quotas, channel grants, terminal-slot reuse with monotonic revisions and corrupt-journal denial");
}
static void reservedCapacity() {
  reset(); clockAt();
  BotStore store; BotTimers timers;
  uint8_t bot[32]{1};
  std::atomic<uint32_t> generation{1}, grant{1};
  std::atomic<bool> shared{true};
  BotIoRequest request; request.token = {1,1,1}; request.grant = 1;
  request.kind = BotIoRequest::Put; strcpy(request.value, "public");
  BotIoResult result;
  for (unsigned principal = 1; principal <= 2; ++principal) {
    request.principal[0] = principal;
    for (auto scope : {BotIoRequest::Caller, BotIoRequest::Conversation}) {
      request.scope = scope;
      for (unsigned i = 0; i < 8; ++i) {
        snprintf(request.key, sizeof(request.key), "key%u", i);
        store.perform(bot, request, result, generation, shared, grant); assert(result.ok);
      }
    }
  }
  request.principal[0] = 3;
  store.perform(bot, request, result, generation, shared, grant); assert(!result.ok);
  request.principal[0] = 1;
  identity_test::freeEntries = BotKvPublicEntries - 1;
  store.perform(bot, request, result, generation, shared, grant);
  assert(!result.ok && strstr(result.error, "headroom"));
  identity_test::freeEntries++;
  store.perform(bot, request, result, generation, shared, grant); assert(result.ok);
  identity_test::failStats = true;
  store.perform(bot, request, result, generation, shared, grant);
  assert(!result.ok && strstr(result.error, "unavailable"));
  identity_test::failStats = false; identity_test::freeEntries--;
  kv_fixture::Record legacy[32];
  for (unsigned i = 0; i < 32; ++i) legacy[i] = kv_fixture::read(i);
  memset(request.principal, 0, 32); request.scope = BotIoRequest::Bot;
  for (unsigned i = 0; i < 8; ++i) {
    snprintf(request.key, sizeof(request.key), "owner%u", i);
    store.perform(bot, request, result, generation, shared, grant); assert(result.ok);
  }
  for (unsigned i = 0; i < 32; ++i) {
    const auto entry = kv_fixture::read(i);
    assert(!memcmp(&entry, &legacy[i], sizeof(entry)));
  }
  request.kind = BotIoRequest::List; request.key[0] = 0;
  store.perform(bot, request, result, generation, shared, grant);
  assert(result.ok && result.keys.count == BotKeysPerScope && !strcmp(result.keys.keys[7], "owner7"));
  request.scope = BotIoRequest::Caller; request.principal[0] = 1;
  store.perform(bot, request, result, generation, shared, grant);
  assert(result.ok && result.keys.count == BotKeysPerScope && !strcmp(result.keys.keys[7], "key7"));
  request.principal[0] = 3;
  store.perform(bot, request, result, generation, shared, grant);
  assert(result.ok && !result.keys.count);
  identity_test::freeEntries = 4096;
  request.kind = BotIoRequest::TimerSet; request.delaySeconds = 86400;
  request.scope = BotIoRequest::Caller;
  for (unsigned caller = 1; caller <= 4; ++caller) {
    request.principal[0] = caller;
    for (unsigned i = 0; i < 2; ++i) {
      snprintf(request.key, sizeof(request.key), "t%u", i);
      timers.perform(bot, request, result, generation, shared, grant); assert(result.ok);
    }
  }
  request.principal[0] = 5;
  timers.perform(bot, request, result, generation, shared, grant); assert(!result.ok);
  identity_test::freeEntries = BotCoreNvsReserveEntries;
  timers.perform(bot, request, result, generation, shared, grant);
  assert(!result.ok && strstr(result.error, "headroom"));
  request.scope = BotIoRequest::Bot; memset(request.principal, 0, 32);
  identity_test::freeEntries = BotCoreNvsReserveEntries;
  for (unsigned i = 0; i < 2; ++i) {
    snprintf(request.key, sizeof(request.key), "owner%u", i);
    timers.perform(bot, request, result, generation, shared, grant); assert(result.ok);
  }
  shared = false;
  timers.perform(bot, request, result, generation, shared, grant); assert(!result.ok);
  request.kind = BotIoRequest::Put;
  store.perform(bot, request, result, generation, shared, grant); assert(!result.ok);
  identity_test::freeEntries = 4096;
  puts("PASS core reserve: full legacy public KV32/timer8 cannot consume bot-global KV8/timer2; legacy records preserved");
}
static void reminderJournal() {
  reset();
  uint8_t bot[32]{1};
  std::atomic<uint32_t> generation{1}, grant{1};
  std::atomic<bool> enabled{true}, stopping{false};
  BotReminders reminders;
  BotIoRequest request; request.token = {1,1,1}; request.grant = 1; request.principal[0] = 9;
  request.kind = BotIoRequest::ReminderSet; request.delaySeconds = 5; strcpy(request.value, "tea");
  BotIoResult result;
  const auto run = [&] { reminders.perform(bot, request, result, generation, enabled, grant, stopping, 0); };
  run(); assert(!result.ok && strstr(result.error, "SNTP"));
  clockAt(); enabled = false; run(); assert(!result.ok);
  enabled = true; identity_test::freeEntries = BotCoreNvsReserveEntries;
  run(); assert(!result.ok && strstr(result.error, "headroom"));
  identity_test::freeEntries = 4096;
  run(); assert(result.ok && result.revision == 1);
  const uint32_t id = result.revision, due = result.deadlineUtc;
  request.kind = BotIoRequest::ReminderCancel; request.revision = id; request.principal[31] = 1;
  run(); assert(!result.ok && strstr(result.error, "not found"));
  request.kind = BotIoRequest::ReminderList; run(); assert(result.ok && !strcmp(result.value, "No personal reminders"));
  request.principal[31] = 0; run(); assert(result.ok && strstr(result.value, "1 pending"));
  BotReminderDispatch dispatch;
  char error[128]{};
  assert(!reminders.next(bot, dispatch, stopping, error, sizeof(error)) && !error[0]);
  identity_test::readHook = [](const char *key) { if (!strcmp(key, "files")) now += BotScheduleRecoveryBudgetMs; };
  request.kind = BotIoRequest::ReminderList; run();
  assert(!result.ok && strstr(result.error, "deadline"));
  identity_test::readHook = nullptr; clockAt();
  beginClocks(); beginNetworkClock(true);
  {
    BotReminders reboot;
    assert(!reboot.next(bot, dispatch, stopping, error, sizeof(error)) && !error[0]);
    clockAt(due);
    assert(reboot.next(bot, dispatch, stopping, error, sizeof(error)));
    assert(dispatch.id == id && dispatch.sourceGeneration == 1 && !strcmp(dispatch.text, "tea"));
    dispatch.grant = grant;
    identity_test::failCommit = true;
    assert(!reboot.claim(bot, dispatch, enabled, grant, stopping, error, sizeof(error)));
    identity_test::failCommit = false;
    run(); assert(result.ok && strstr(result.value, "pending"));
    identity_test::afterCommit = [] { identity_test::failRead = true; };
    assert(!reboot.claim(bot, dispatch, enabled, grant, stopping, error, sizeof(error)));
    identity_test::afterCommit = nullptr; identity_test::failRead = false;
  }
  assert(!reminders.next(bot, dispatch, stopping, error, sizeof(error)) && !error[0]);
  run(); assert(result.ok && strstr(result.value, "unknown"));
  request.kind = BotIoRequest::ReminderCancel; run();
  assert(result.ok && result.reminderState == BotReminderState::Unknown);
  request.kind = BotIoRequest::ReminderSet; run(); assert(result.ok);
  const auto second = result.revision;
  clockAt(result.deadlineUtc);
  assert(reminders.next(bot, dispatch, stopping, error, sizeof(error)));
  dispatch.grant = grant;
  request.kind = BotIoRequest::ReminderCancel; request.revision = second;
  run(); assert(result.ok && result.reminderState == BotReminderState::Cancelled);
  assert(!reminders.claim(bot, dispatch, enabled, grant, stopping, error, sizeof(error)));
  request.kind = BotIoRequest::ReminderSet; run(); assert(result.ok);
  clockAt(result.deadlineUtc);
  assert(reminders.next(bot, dispatch, stopping, error, sizeof(error))); dispatch.grant = grant;
  ++grant;
  assert(!reminders.claim(bot, dispatch, enabled, grant, stopping, error, sizeof(error)));
  dispatch.grant = grant;
  beginClocks(); beginNetworkClock(true);
  assert(!reminders.claim(bot, dispatch, enabled, grant, stopping, error, sizeof(error)));
  clockAt(dispatch.due);
  assert(reminders.claim(bot, dispatch, enabled, grant, stopping, error, sizeof(error)));
  assert(!reminders.claim(bot, dispatch, enabled, grant, stopping, error, sizeof(error)));
  dispatch.outcome = BotReminderState::Sent;
  identity_test::afterCommit = [] { identity_test::failRead = true; };
  assert(!reminders.complete(bot, dispatch, stopping, error, sizeof(error)));
  identity_test::afterCommit = nullptr; identity_test::failRead = false;
  request.kind = BotIoRequest::ReminderList; run(); assert(result.ok && strstr(result.value, "sent"));
  assert(!reminders.complete(bot, dispatch, stopping, error, sizeof(error)));
  request.kind = BotIoRequest::ReminderSet; request.grant = grant;
  run(); assert(result.ok);
  clockAt(result.deadlineUtc + BotTimerOverdueSeconds + 1);
  assert(!reminders.next(bot, dispatch, stopping, error, sizeof(error)) && !error[0]);
  request.kind = BotIoRequest::ReminderList; run(); assert(result.ok && strstr(result.value, "overdue"));
  for (auto &entry : identity_test::durable) if (entry.first.first == "mc-bot-remind") {
    entry.second.back() ^= 1;
    run(); assert(!result.ok && strstr(result.error, "corrupt"));
    entry.second.back() ^= 1; break;
  }
  request.kind = BotIoRequest::ReminderSet;
  request.delaySeconds = 86400;
  for (unsigned caller = 1; caller <= 4; ++caller) {
    request.principal[0] = caller;
    for (unsigned i = 0; i < 2; ++i) { run(); assert(result.ok); }
    run(); assert(!result.ok && strstr(result.error, "quota"));
  }
  request.principal[0] = 5; run(); assert(!result.ok && strstr(result.error, "capacity"));
  puts("PASS reminder journal: private identities, reboot, lost clock, durable claim/readback uncertainty, cancel/grant fences, TX outcomes and quotas");
}
static BotWorker::Result poll(BotWorker &worker) {
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  BotWorker::Result result;
  while (std::chrono::steady_clock::now() < until) {
    if (worker.poll(result)) return result;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  assert(false && "worker did not complete");
  return result;
}
static BotEvent event(const char *command, bool channel = false) {
  BotEvent e;
  char error[128];
  assert(parseBotCommand(command, strlen(command), e, error, sizeof(error)));
  e.authenticated = !channel; e.sender[0] = 1;
  if (channel) {
    e.sender[0] = 0;
    e.channelVerified = e.sharedState = e.targeted = true; e.channelId[0] = 7;
    strcpy(e.channel, "#fixture"); strcpy(e.nickname, "untrusted");
  }
  return e;
}
static void workerTimers() {
  reset(); clockAt();
  uint8_t bot[32]{1};
  BotWorker worker;
  assert(worker.begin(bot));
  const char *source =
      "function arm(name) return timer.set(name,5).state end "
      "function await(name) timer.wait(name) return 'reminder '..name end "
      "function timer_status(name) return timer.get(name).state end "
      "function disarm(name) return timer.cancel(name).state end "
      "function delayed() sleep(10) kv.put('memory','bad','channel') return 'bad' end "
      "function lookup() return kv.get('memory','channel') or 'empty' end";
  const auto install = [&]() {
    assert(worker.stage(source, strlen(source)) && poll(worker).ok);
    assert(worker.activate() && poll(worker).ok);
  };
  install();
  assert(worker.invoke(event("!arm tea"), 1) && poll(worker).ok);
  assert(worker.invoke(event("!await tea"), 2));
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  assert(worker.invoke(event("!ping"), 3));
  auto result = poll(worker);
  assert(result.ok && result.job == 3 && !strcmp(result.action.text, "Pong"));
  now += 6000; loopClocks();
  result = poll(worker);
  assert(result.ok && result.job == 2 && !strcmp(result.action.text, "reminder tea"));
  assert(!worker.poll(result));
  assert(worker.invoke(event("!await tea"), 4));
  result = poll(worker); assert(!result.ok && strstr(result.error, "claimed"));
  assert(worker.invoke(event("!arm reboot"), 5) && poll(worker).ok);
  worker.stop(); now = 0; beginClocks(); beginNetworkClock(true);
  assert(worker.begin(bot)); install();
  assert(worker.invoke(event("!timer_status reboot"), 6));
  result = poll(worker); assert(result.ok && !strcmp(result.action.text, "pending"));
  assert(worker.invoke(event("!await reboot"), 7));
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  assert(!worker.poll(result));
  clockAt(Epoch + 12);
  now += 250; loopClocks();
  result = poll(worker); assert(result.ok && !strcmp(result.action.text, "reminder reboot"));
  assert(worker.invoke(event("!arm cancel"), 11) && poll(worker).ok);
  assert(worker.invoke(event("!await cancel"), 12));
  assert(worker.invoke(event("!disarm cancel"), 13));
  result = poll(worker);
  assert(result.ok && result.job == 13 && !strcmp(result.action.text, "cancelled"));
  now += 250; loopClocks();
  result = poll(worker); assert(!result.ok && result.job == 12 && strstr(result.error, "cancelled"));
  assert(worker.invoke(event("!arm source"), 14) && poll(worker).ok);
  assert(worker.invoke(event("!await source"), 15));
  assert(worker.stage(source, strlen(source)));
  result = poll(worker);
  assert(result.ok && result.operation == BotWorker::Operation::Stage);
  assert(worker.activate());
  bool activated = false, cancelled = false;
  for (unsigned i = 0; i < 2; ++i) {
    result = poll(worker);
    if (result.operation == BotWorker::Operation::Activate) activated = result.ok;
    else cancelled = !result.ok && result.job == 15;
  }
  assert(activated && cancelled);
  assert(worker.invoke(event("!timer_status source"), 16));
  result = poll(worker); assert(result.ok && !strcmp(result.action.text, "pending"));
  worker.setSharedState(true);
  assert(worker.invoke(event("!delayed", true), 9));
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  worker.setSharedState(false); worker.setSharedState(true);
  now += 10; loopClocks();
  result = poll(worker); assert(!result.ok && strstr(result.error, "grant"));
  assert(worker.invoke(event("!lookup", true), 10));
  result = poll(worker); assert(result.ok && !strcmp(result.action.text, "empty"));
  assert(worker.invoke(event("!arm stopping"), 17) && poll(worker).ok);
  assert(worker.invoke(event("!await stopping"), 18));
  assert(worker.invoke(event("!ping"), 19));
  result = poll(worker); assert(result.ok && result.job == 19);
  worker.stop();
  assert(worker.begin(bot)); install();
  assert(worker.invoke(event("!timer_status stopping"), 20));
  result = poll(worker); assert(result.ok && !strcmp(result.action.text, "pending"));
  worker.stop();
  assert(psram_test::allocations.empty());
  puts("PASS native yielding durable timer worker: concurrent Pong, reboot rebind, single completion, "
       "cancel/source/stop fences and revoked channel epoch");
}
static void timerClockSuspension() {
  reset(); clockAt();
  uint8_t bot[32]{1};
  BotWorker worker;
  assert(worker.begin(bot));
  const char *source =
      "function hold() timer.set('long',86400) timer.wait('long') return 'fired' end "
      "function timer_status() return timer.get('long').state end";
  assert(worker.stage(source, strlen(source)) && poll(worker).ok);
  assert(worker.activate() && poll(worker).ok);
  assert(worker.invoke(event("!hold"), 1));
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  now += 5000;
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  BotWorker::Result result;
  assert(!worker.poll(result));
  clockAt(Epoch + 5); now += 250; loopClocks();
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  assert(!worker.poll(result));
  assert(worker.invoke(event("!ping"), 2));
  result = poll(worker); assert(result.ok && result.job == 2);
  now += 5000;
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  now += BotTimerClockSuspendMs;
  result = poll(worker); assert(!result.ok && strstr(result.error, "suspension/lifetime"));
  clockAt(Epoch + 310);
  assert(worker.invoke(event("!timer_status"), 3));
  result = poll(worker); assert(result.ok && !strcmp(result.action.text, "pending"));
  assert(worker.invoke(event("!hold"), 4));
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  now += BotTimerWaitLifetimeMs;
  clockAt(Epoch + 310 + BotTimerWaitLifetimeMs / 1000);
  result = poll(worker); assert(!result.ok && strstr(result.error, "suspension/lifetime"));
  worker.stop();
  puts("PASS timer.wait: publication stalls suspend, concurrent Pong survives, bounded clock grace and hard lifetime leave journal pending");
}
static void workerNotes() {
  reset();
  uint8_t bot[32]{1};
  BotWorker worker;
  assert(worker.begin(bot));
  const char *source =
      "function put(value) local ok,state=kv.put('shared',value) return state end "
      "function enumerate() local r=kv.list() return tostring(r.count)..':'..(r.keys[1] or '-') end "
      "function together() remember('cooperate','same tier') return recall('cooperate') end";
  const auto install = [&] {
    assert(worker.stage(source, strlen(source)) && poll(worker).ok);
    assert(worker.activate() && poll(worker).ok);
  };
  install();
  assert(worker.invoke(event("!put first"), 1));
  assert(worker.invoke(event("!enumerate"), 2));
  bool put = false, list = false;
  for (unsigned i = 0; i < 2; ++i) {
    const auto done = poll(worker); assert(done.ok);
    if (done.job == 1) { put = true; assert(!strcmp(done.action.text, "created")); }
    else {
      list = true; assert(done.job == 2 &&
        (!strcmp(done.action.text, "0:-") || !strcmp(done.action.text, "1:shared")));
    }
  }
  assert(put && list);
  auto other = event("!enumerate"); other.sender[31] = 2;
  assert(worker.invoke(other, 3)); auto result = poll(worker);
  assert(result.ok && !strcmp(result.action.text, "0:-"));
  assert(worker.invoke(event("!notes"), 4));
  result = poll(worker); assert(result.ok && !strcmp(result.action.text, "Notes (1): [shared]"));
  assert(worker.invoke(event("!put overwritten"), 5));
  result = poll(worker); assert(result.ok && !strcmp(result.action.text, "replaced"));
  assert(worker.invoke(event("!together"), 6));
  result = poll(worker); assert(result.ok && !strcmp(result.action.text, "same tier"));
  assert(worker.invoke(event("!list-memories coop"), 7));
  result = poll(worker); assert(result.ok && !strcmp(result.action.text, "Notes (1): [cooperate]"));
  install();
  assert(worker.invoke(event("!recall shared"), 8)); result = poll(worker);
  assert(result.ok && !strcmp(result.action.text, "overwritten"));
  worker.stop(); assert(worker.begin(bot)); install();
  assert(worker.invoke(event("!notes"), 9)); result = poll(worker);
  assert(result.ok && !strcmp(result.action.text, "Notes (2): [cooperate] [shared]"));
  worker.stop();
  puts("PASS yielding notes: concurrent put/list serialized snapshots, two full-key callers, nested built-ins/custom API and source/reboot persistence");
}
static void legacyList() {
  reset();
  BotStore store;
  uint8_t bot[32]{1};
  std::atomic<uint32_t> generation{1}, grant{1};
  std::atomic<bool> shared{true};
  BotIoRequest request; request.token = {1,1,1}; request.grant = 1; request.scope = BotIoRequest::Bot;
  BotIoResult result;
  const auto run = [&] { store.perform(bot, request, result, generation, shared, grant); };
  kv_fixture::seed(0, kv_fixture::record(1, BotIoRequest::Bot, 0, "legacy", "old-format"));
  request.kind = BotIoRequest::Put; strcpy(request.value, "old-format");
  strcpy(request.key, "reserved"); run(); assert(result.ok);
  const auto durable = identity_test::durable;
  const auto reserved = kv_fixture::read(32), original = kv_fixture::read(0);
  request.kind = BotIoRequest::List; request.key[0] = 0; run();
  assert(result.ok && result.keys.count == 2 && !strcmp(result.keys.keys[0], "legacy") &&
         !strcmp(result.keys.keys[1], "reserved") && identity_test::durable == durable);
  request.kind = BotIoRequest::Put; strcpy(request.key, "legacy"); strcpy(request.value, "updated");
  run(); assert(result.ok && result.found);
  const auto afterReserved = kv_fixture::read(32), updated = kv_fixture::read(0);
  assert(identity_test::durable.size() == durable.size() &&
         !memcmp(&reserved, &afterReserved, sizeof(reserved)) &&
         memcmp(&original, &updated, sizeof(original)));
  puts("PASS legacy bot-global KV listing spans unchanged public/reserve record formats and updates original slot");
}
static void workerBoard() {
  reset();
  uint8_t bot[32]{1};
  BotWorker worker;
  assert(worker.begin(bot));
  const char *source =
      "function rawput(key,value) kv.put(key,value,'channel') return 'raw committed' end "
      "function rawget(key) return kv.get(key,'channel') or 'raw absent' end";
  const auto install = [&] {
    assert(worker.stage(source, strlen(source)) && poll(worker).ok);
    assert(worker.activate() && poll(worker).ok);
  };
  install();
  unsigned job = 0;
  const auto run = [&](const char *command, unsigned channel = 1, const char *name = "Alice") {
    auto e = event(command, channel != 0);
    e.channelId[31] = channel;
    strcpy(e.nickname, name);
    assert(worker.invoke(e, ++job));
    return poll(worker);
  };
  auto result = run("!board put plan initial");
  assert(!result.ok && strstr(result.error, "not granted"));
  assert(worker.setSharedState(true));
  result = run("!rawput plan unrelated"); assert(result.ok);
  result = run("!board put plan initial");
  assert(result.ok && !strcmp(result.action.text, "Board plan created; committed"));
  result = run("!board get plan", 1, "Bob");
  assert(result.ok && !strcmp(result.action.text, "initial"));
  result = run("!rawget plan"); assert(result.ok && !strcmp(result.action.text, "unrelated"));
  result = run("!board list"); assert(result.ok && !strcmp(result.action.text, "Board (1): [plan]"));
  result = run("!board get plan", 2); assert(result.ok && !strcmp(result.action.text, "No board entry: plan"));
  result = run("!board put plan second secret", 2); assert(result.ok);
  result = run("!board put plan private attempt", 0);
  assert(!result.ok && strstr(result.error, "permission not granted"));
  result = run("!remember plan personal", 0); assert(result.ok);
  result = run("!board get plan"); assert(result.ok && !strcmp(result.action.text, "initial"));
  result = run("!recall plan", 0); assert(result.ok && !strcmp(result.action.text, "personal"));
  auto a = event("!board put race alpha", true), b = event("!board put race beta", true);
  a.channelId[31] = b.channelId[31] = 1; strcpy(a.nickname, "Alice"); strcpy(b.nickname, "Bob");
  assert(worker.invoke(a, ++job) && worker.invoke(b, ++job));
  unsigned created = 0, replaced = 0;
  for (unsigned i = 0; i < 2; ++i) {
    result = poll(worker); assert(result.ok);
    created += strstr(result.action.text, "created; committed") != nullptr;
    replaced += strstr(result.action.text, "replaced; committed") != nullptr;
  }
  assert(created == 1 && replaced == 1);
  result = run("!board get race");
  assert(result.ok && (!strcmp(result.action.text, "alpha") || !strcmp(result.action.text, "beta")));
  a = event("!board put snapshot value", true); b = event("!board list", true);
  a.channelId[31] = b.channelId[31] = 1;
  const unsigned writeJob = ++job;
  assert(worker.invoke(a, writeJob) && worker.invoke(b, ++job));
  for (unsigned i = 0; i < 2; ++i) {
    result = poll(worker); assert(result.ok);
    if (result.job != writeJob)
      assert(!strcmp(result.action.text, "Board (2): [plan] [race]") ||
             !strcmp(result.action.text, "Board (3): [plan] [race] [snapshot]"));
  }
  result = run("!board delete snapshot"); assert(result.ok);
  install();
  worker.stop(); assert(worker.begin(bot)); install(); assert(worker.setSharedState(true));
  result = run("!board get plan"); assert(result.ok && !strcmp(result.action.text, "initial"));
  result = run("!board get plan", 2); assert(result.ok && !strcmp(result.action.text, "second secret"));
  identity_test::failCommit = true;
  result = run("!board put plan failed");
  assert(!result.ok && strstr(result.error, "outcome unknown"));
  identity_test::failCommit = false;
  result = run("!board get plan"); assert(result.ok && !strcmp(result.action.text, "initial"));
  static BotWorker *current;
  current = &worker;
  identity_test::readHook = [](const char *key) {
    if (!strcmp(key, "txn")) {
      identity_test::readHook = nullptr;
      assert(current->setSharedState(false) && current->setSharedState(true));
    }
  };
  result = run("!board put plan revoked");
  assert(!result.ok && strstr(result.error, "revoked"));
  result = run("!board get plan"); assert(result.ok && !strcmp(result.action.text, "initial"));
  identity_test::afterCommit = [] {
    assert(current->setSharedState(false) && current->setSharedState(true));
  };
  result = run("!board put plan commit then revoke");
  assert(!result.ok && strstr(result.error, "may have committed"));
  identity_test::afterCommit = nullptr;
  result = run("!board get plan"); assert(result.ok && !strcmp(result.action.text, "commit then revoke"));
  identity_test::afterCommit = [] { identity_test::failRead = true; };
  result = run("!board delete plan"); assert(!result.ok && strstr(result.error, "outcome unknown"));
  identity_test::afterCommit = nullptr; identity_test::failRead = false;
  result = run("!board get plan"); assert(result.ok && !strcmp(result.action.text, "No board entry: plan"));
  result = run("!rawput plan still-unrelated"); assert(result.ok);
  for (unsigned i = 0; i < 6; ++i) {
    const auto command = "!board put k" + std::to_string(i) + " value";
    result = run(command.c_str()); assert(result.ok);
  }
  result = run("!board put overflow value"); assert(!result.ok && strstr(result.error, "capacity"));
  result = run("!board delete k0"); assert(result.ok);
  result = run("!board put reused value"); assert(result.ok);
  static std::atomic<bool> entered, release;
  entered = release = false;
  identity_test::readHook = [](const char *key) {
    if (!strcmp(key, "txn")) {
      entered = true;
      while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  };
  a = event("!board put race cancelled", true); a.channelId[31] = 1;
  const unsigned cancelledJob = ++job;
  assert(worker.invoke(a, cancelledJob));
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (!entered && std::chrono::steady_clock::now() < until)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  assert(entered);
  assert(worker.stage(source, strlen(source)) && poll(worker).ok);
  assert(worker.activate());
  bool activated = false, cancelled = false;
  for (unsigned i = 0; i < 2; ++i) {
    result = poll(worker);
    if (result.operation == BotWorker::Operation::Activate) activated = result.ok;
    else cancelled = result.job == cancelledJob && !result.ok;
  }
  assert(activated && cancelled); release = true;
  result = run("!board get race");
  assert(result.ok && strcmp(result.action.text, "cancelled"));
  worker.stop(); identity_test::readHook = nullptr;
  puts("PASS durable board worker: namespace/DM/full-channel isolation, concurrent serialized writers, reboot/source, quota/reuse, before/after-commit grant revoke, unknown deletion and cancelled in-flight write");
}
static void boardSlots() {
  reset();
  BotStore store;
  uint8_t bot[32]{1};
  std::atomic<uint32_t> generation{1}, grant{1};
  std::atomic<bool> shared{true};
  BotIoRequest request; request.token = {1,1,1}; request.grant = 1;
  request.scope = BotIoRequest::Channel; request.kind = BotIoRequest::Put;
  strcpy(request.value, "board entry");
  BotIoResult result;
  const auto run = [&] { store.perform(bot, request, result, generation, shared, grant); };
  for (unsigned channel = 1; channel <= 4; ++channel) {
    request.principal[31] = channel;
    for (unsigned key = 0; key < 8; ++key) {
      snprintf(request.key, sizeof(request.key), "board:k%u", key); run(); assert(result.ok);
    }
  }
  request.principal[31] = 5; run(); assert(!result.ok && strstr(result.error, "capacity"));
  request.principal[31] = 1; request.kind = BotIoRequest::Delete; run(); assert(result.ok);
  request.principal[31] = 5; request.kind = BotIoRequest::Put; run(); assert(result.ok);
  request.scope = BotIoRequest::Bot; memset(request.principal, 0, 32);
  strcpy(request.key, "owner"); run(); assert(result.ok);
  unsigned used = 0;
  for (unsigned slot = 0; slot < 40; ++slot) used += kv_fixture::read(slot).entry.used;
  assert(used == 33 && identity_test::durable.size() == 1 && kv_fixture::authority().size() == 200);
  puts("PASS board slots: four shared channel quotas fill public32, no fifth-channel bypass, tombstone reuse and protected bot reserve");
}
static void configuredNvs() {
  identity_test::entryCapacity = 630;
  const auto seed = [](const char *space, const char *key, size_t size) {
    nvs_handle_t handle;
    assert(nvs_open(space, NVS_READWRITE, &handle) == ESP_OK);
    std::vector<uint8_t> data(size);
    assert(nvs_set_blob(handle, key, data.data(), data.size()) == ESP_OK);
    assert(nvs_commit(handle) == ESP_OK);
    nvs_close(handle);
  };
  for (const char *name : {"modem", "management", "repeater", "room", "companion", "observer", "command-bot"})
    seed("mc-onchip", name, 64);
  seed("mc-onchip", "role-profile", 28); seed("mc-onchip", "origin-path", 5);
  for (const char *name : {"command-select", "bot-shared-kv", "bot-home", "bot-reminders", "bot-events"})
    seed("mc-onchip", name, 5);
  seed("mc-onchip", "bot-radio", 40); seed("mc-onchip", "bot-forward", 68);
  seed("mc-mast-admin", "settings", 140); seed("mc-mast-admin", "replay", 148);
  seed("mc-mast-admin", "owner-replay", 40); seed("mc-mast-admin", "source", 572);
  seed("phy", "cal_data", 1904); seed("phy", "cal_mac", 6);
  seed("phy", "cal_version", 4); // Conservatively model the SDK integer as a blob.
  seed("mesh-phy", "profile", 22);
  for (const char *space : {"mc-bot-kv", "mc-bot-timer", "mc-bot-remind"}) {
    nvs_handle_t handle;
    assert(nvs_open(space, NVS_READWRITE, &handle) == ESP_OK);
    nvs_close(handle);
  }
  nvs_stats_t stats{};
  assert(nvs_get_stats(nullptr, &stats) == ESP_OK);
  assert(stats.total_entries == 630 && stats.used_entries == 176 && stats.free_entries == 454);
  assert(stats.namespace_count == 7);
}
static void physicalScalarCapacity() {
  reset(); clockAt(); configuredNvs();
  BotStore store; BotTimers timers; BotReminders reminders;
  uint8_t bot[32]{1};
  std::atomic<uint32_t> generation{1}, grant{1};
  std::atomic<bool> enabled{true}, stopping{false};
  BotIoRequest request; request.token = {1,1,1}; request.grant = 1;
  request.kind = BotIoRequest::Put;
  strcpy(request.value, "persistent note");
  BotIoResult result;
  for (unsigned owner = 1; owner <= 4; ++owner) {
    request.principal[0] = owner;
    for (unsigned i = 0; i < 8; ++i) {
      snprintf(request.key, sizeof(request.key), "note%u", i);
      store.perform(bot, request, result, generation, enabled, grant);
      assert(result.ok);
    }
    request.kind = BotIoRequest::List; request.key[0] = 0;
    store.perform(bot, request, result, generation, enabled, grant);
    assert(result.ok && result.keys.count == 8);
    request.kind = BotIoRequest::Put;
  }
  request.scope = BotIoRequest::Bot; request.principal[0] = 0;
  for (unsigned i = 0; i < 8; ++i) {
    snprintf(request.key, sizeof(request.key), "owner%u", i);
    store.perform(bot, request, result, generation, enabled, grant); assert(result.ok);
  }
  request.scope = BotIoRequest::Caller; request.principal[0] = 4;
  request.kind = BotIoRequest::ReminderSet; request.delaySeconds = 60;
  for (unsigned i = 0; i < 2; ++i) {
    reminders.perform(bot, request, result, generation, enabled, grant, stopping, 0);
    assert(result.ok);
  }
  request.kind = BotIoRequest::TimerSet; strcpy(request.key, "clock");
  timers.perform(bot, request, result, generation, enabled, grant); assert(result.ok);
  nvs_stats_t stats{}; assert(nvs_get_stats(nullptr, &stats) == ESP_OK);
  assert(stats.used_entries == 195 && stats.free_entries == 435);
  assert(kv_fixture::authority().size() == 200 && filesystem_test::files.size() <= 14);
  // Other NVS consumers can still spend headroom. KV metadata fits below the
  // unchanged public scheduler floor, without allocating another payload blob.
  identity_test::durable[{"mc-onchip", "other-sdk-data"}] = std::vector<uint8_t>((128 - 3) * 32);
  request.kind = BotIoRequest::Put; strcpy(request.key, "note0"); strcpy(request.value, "updated");
  store.perform(bot, request, result, generation, enabled, grant); assert(result.ok);
  assert(nvs_get_stats(nullptr, &stats) == ESP_OK && stats.free_entries == 307);
  const auto before = identity_test::durable;
  request.kind = BotIoRequest::ReminderSet; request.principal[0] = 3;
  reminders.perform(bot, request, result, generation, enabled, grant, stopping, 0);
  assert(!result.ok && strstr(result.error, "headroom"));
  request.kind = BotIoRequest::TimerSet;
  timers.perform(bot, request, result, generation, enabled, grant);
  assert(!result.ok && strstr(result.error, "headroom") && before == identity_test::durable);
  assert(identity_test::peakEntries <= 630 - 126);
  puts("PASS physical KV capacity: all 32 public + 8 reserved keys use nine NVS entries; timer/reminder floor 308 and GC reserve 126 unchanged");
}
static void physicalAtomicCapacity() {
  uint8_t bot[32]{1};
  std::atomic<uint32_t> generation{1}, grant{1};
  std::atomic<bool> shared{true};
  const auto limitFree = [](size_t free) {
    const size_t entries = 630 - identity_test::usedEntries() - free;
    assert(entries > 16);
    identity_test::durable[{"mc-onchip", "other-sdk-data"}] =
        std::vector<uint8_t>((entries - 3) * 32);
    assert(630 - identity_test::usedEntries() == free);
  };
  for (unsigned count = 1; count <= 4; ++count) for (bool botScope : {false, true}) {
    const unsigned required = botScope ? BotKvRecoveryEntries + BotKvBootstrapEntries : BotKvPublicEntries;
    for (bool admitted : {false, true}) {
      reset(); configuredNvs(); limitFree(required - !admitted);
      BotStore store; BotIoResult result;
      BotIoRequest request; request.token = {1,1,1}; request.grant = 1;
      request.scope = botScope ? BotIoRequest::Bot : BotIoRequest::Caller;
      request.principal[0] = botScope ? 0 : 7;
      request.kind = count == 1 ? BotIoRequest::Cas : BotIoRequest::Transaction;
      request.mutations = count;
      for (unsigned i = 0; i < count; ++i) {
        snprintf(request.mutation[i].key, sizeof(request.mutation[i].key), "key%u", i);
        strcpy(request.mutation[i].value, "value");
        request.mutation[i].compare = true;
      }
      const auto before = identity_test::durable;
      const auto commits = identity_test::commits;
      store.perform(bot, request, result, generation, shared, grant);
      assert(result.ok == admitted);
      if (!admitted) {
        assert(result.outcome == BotIoResult::Rejected && strstr(result.error, "not published"));
        assert(before == identity_test::durable && commits == identity_test::commits);
      } else {
        assert(result.outcome == BotIoResult::Committed);
        assert(kv_fixture::authority().size() == 200);
      }
    }
  }
  BotStore::Snapshot backup;
  memcpy(backup.magic, "BKD\1", 4); memcpy(backup.bot, bot, 32);
  backup.principal[0] = 7; backup.count = 8;
  for (unsigned i = 0; i < 8; ++i) {
    snprintf(backup.entries[i].key, sizeof(backup.entries[i].key), "key%u", i);
    strcpy(backup.entries[i].value, "restored");
  }
  mesh::Utils::sha256(backup.digest, 32, reinterpret_cast<const uint8_t *>(&backup),
                      offsetof(BotStore::Snapshot, digest));
  for (unsigned cut = 0; cut <= 1; ++cut) {
    reset(); configuredNvs(); limitFree(BotKvPublicEntries - !cut);
    BotStore store; BotIoResult result;
    static unsigned committed, failAt;
    committed = 0; failAt = cut ? 2 : 0;
    identity_test::afterCommit = [] { if (++committed == failAt) identity_test::failRead = true; };
    const auto before = identity_test::durable;
    store.restore(bot, backup, result, 1, generation);
    assert(!result.ok);
    identity_test::afterCommit = nullptr; identity_test::failRead = false;
    if (!cut) {
      assert(result.outcome == BotIoResult::Rejected && strstr(result.error, "196 free, 197 required"));
      assert(before == identity_test::durable && !committed);
    } else {
      assert(result.outcome == BotIoResult::Unknown);
      BotStore reboot;
      BotStore::Snapshot actual; actual.principal[0] = 7;
      char error[128]{};
      assert(reboot.snapshot(bot, actual, error, sizeof(error)));
      assert(!memcmp(&backup, &actual, sizeof(actual)));
      assert(kv_fixture::authority().size() == 200);
      assert(identity_test::peakEntries <= 630 - 126);
    }
  }
  puts("PASS physical atomic budget: 1..4-key transactions and eight-key restore share one nine-entry authority; exact public/bot thresholds and unknown publication recover");
}
static void transactions() {
  for (bool eager : {false, true}) for (unsigned cut = 0; cut <= 1; ++cut) {
    reset(); identity_test::eagerWrites = eager;
    uint8_t bot[32]{1};
    std::atomic<uint32_t> generation{1}, grant{1};
    std::atomic<bool> shared{true};
    BotIoRequest request; request.token = {1,1,1}; request.grant = 1;
    request.kind = BotIoRequest::Transaction; request.mutations = 2;
    request.principal[0] = 7;
    strcpy(request.mutation[0].key, "left"); strcpy(request.mutation[0].value, "one");
    strcpy(request.mutation[1].key, "right"); strcpy(request.mutation[1].value, "two");
    request.mutation[0].compare = request.mutation[1].compare = true;
    static unsigned commits, failAt;
    commits = 0; failAt = cut ? 2 : 0;
    identity_test::afterCommit = [] { if (++commits == failAt) identity_test::failRead = true; };
    BotIoResult result;
    {
      BotStore store;
      store.perform(bot, request, result, generation, shared, grant);
      assert(cut ? (!result.ok && result.outcome == BotIoResult::Unknown) :
                    (result.ok && result.outcome == BotIoResult::Committed));
    }
    identity_test::afterCommit = nullptr; identity_test::failRead = false;
    BotStore reboot;
    const auto run = [&] { reboot.perform(bot, request, result, generation, shared, grant); };
    request.kind = BotIoRequest::Get; strcpy(request.key, "left"); run();
    assert(result.ok && result.found && !strcmp(result.value, "one"));
    strcpy(request.key, "right"); run();
    assert(result.ok && result.found && !strcmp(result.value, "two"));
    request.kind = BotIoRequest::Transaction; request.key[0] = 0;
    const auto before = identity_test::durable;
    run(); assert(result.ok && result.outcome == BotIoResult::Conflict && identity_test::durable == before);
    request.mutation[0].present = request.mutation[1].present = true;
    strcpy(request.mutation[0].expected, "one"); strcpy(request.mutation[1].expected, "two");
    request.mutation[0].value[0] = 0; request.mutation[1].remove = true;
    run(); assert(result.ok && result.outcome == BotIoResult::Committed);
    request.kind = BotIoRequest::Cas; request.mutations = 1; request.mutation[0].expected[0] = 0;
    strcpy(request.mutation[0].value, "from empty");
    run(); assert(result.ok && result.outcome == BotIoResult::Committed);
    request.mutation[0].present = false; run();
    assert(result.ok && result.outcome == BotIoResult::Conflict);
    request.mutations = 2; run(); assert(!result.ok && result.outcome == BotIoResult::Rejected);
    request.mutations = 1; request.principal[31] = 9;
    run(); assert(result.ok && result.outcome == BotIoResult::Committed);
    identity_test::freeEntries = BotKvPublicEntries - 1;
    request.mutation[0].compare = false; request.kind = BotIoRequest::Transaction;
    run(); assert(!result.ok && result.outcome == BotIoResult::Rejected && strstr(result.error, "headroom"));
    identity_test::freeEntries = 4096;
    identity_test::durable.at({"mc-bot-kv", "txn"}).back() ^= 1;
    request.kind = BotIoRequest::Get; strcpy(request.key, "left"); run();
    assert(!result.ok && strstr(result.error, "storage blocked"));
    assert(identity_test::handles.empty());
  }
  puts("PASS CAS/transactions: comparison in worker, absence vs empty, conflict no writes, scoped atomic redo at every commit cut, eager NVS and headroom denial");
}
static void workerRecoveryReserve() {
  reset(); clockAt(); configuredNvs();
  uint8_t bot[32]{1};
  std::atomic<uint32_t> generation{1};
  BotStore::Snapshot backup;
  memcpy(backup.magic, "BKD\1", 4); memcpy(backup.bot, bot, 32);
  backup.principal[0] = 7; backup.count = 8;
  for (unsigned i = 0; i < 8; ++i) {
    snprintf(backup.entries[i].key, sizeof(backup.entries[i].key), "key%u", i);
    strcpy(backup.entries[i].value, "atomic");
  }
  mesh::Utils::sha256(backup.digest, 32, reinterpret_cast<const uint8_t *>(&backup), offsetof(BotStore::Snapshot, digest));
  identity_test::durable[{"mc-onchip", "other-sdk-data"}] = std::vector<uint8_t>((257 - 3) * 32);
  assert(630 - identity_test::usedEntries() == BotKvPublicEntries);
  static unsigned commits;
  commits = 0;
  identity_test::afterCommit = [] { if (++commits == 2) identity_test::failRead = true; };
  {
    BotStore store; BotIoResult result;
    store.restore(bot, backup, result, 1, generation);
    assert(!result.ok && result.outcome == BotIoResult::Unknown);
  }
  identity_test::afterCommit = nullptr;
  BotWorker worker; assert(worker.begin(bot)); assert(worker.setSharedState(true));
  const char *source = "function go() return timer.set('after',60,'bot').state end";
  assert(worker.stage(source, strlen(source)) && poll(worker).ok);
  assert(worker.activate() && poll(worker).ok);
  auto timer = event("!go"); timer.sharedState = true;
  assert(worker.invoke(timer, 1));
  auto result = poll(worker); assert(!result.ok && strstr(result.error, "KV recovery"));
  assert(identity_test::durable.find({"mc-bot-timer", "r00"}) == identity_test::durable.end());
  assert(worker.invoke(event("!ping"), 2));
  result = poll(worker); assert(result.ok && !strcmp(result.action.text, "Pong"));
  identity_test::failRead = false;
  assert(worker.invoke(timer, 3));
  result = poll(worker); assert(result.ok && !strcmp(result.action.text, "pending"));
  assert(kv_fixture::authority().size() == 200);
  assert(identity_test::durable.at({"mc-bot-timer", "files"}).size() == 72);
  assert(identity_test::peakEntries <= 630 - 126);
  worker.stop();
  puts("PASS worker recovery reserve: late interrupted maximum KV restore fences bot-global timers, Pong stays live, replay drains before scheduler writes");
}
static void workerMigrationDeadline() {
  for (bool timeout : {false, true}) {
    reset(); clockAt();
    kv_fixture::seed(0, kv_fixture::record(1, BotIoRequest::Caller, 1, "note", "retained"));
    kv_fixture::seed(39, kv_fixture::record(2, BotIoRequest::Bot, 0, "old-bot", "retained"));
    filesystem_test::readDelayMs = timeout ? 2000 : 500;
    uint8_t bot[32]{1};
    BotWorker worker; assert(worker.begin(bot));
    const char *source =
        "function arm() return timer.set('after',60).state end "
        "function readnote() return kv.get('note') end";
    assert(worker.stage(source, strlen(source)) && poll(worker).ok);
    assert(worker.activate() && poll(worker).ok);
    assert(worker.invoke(event("!arm"), 1));
    auto result = poll(worker);
    if (timeout) {
      assert(!result.ok && strstr(result.error, "deadline"));
      assert(identity_test::durable.count({"mc-bot-kv", "v00"}));
      assert(!identity_test::durable.count({"mc-bot-timer", "t00"}));
    } else {
      assert(result.ok && !strcmp(result.action.text, "pending"));
      assert(now >= 2500 && now < BotKvRecoveryBudgetMs);
    }
    filesystem_test::readDelayMs = 0;
    clockAt();
    assert(worker.invoke(event("!arm"), 2));
    result = poll(worker); assert(result.ok && !strcmp(result.action.text, "pending"));
    assert(worker.invoke(event("!readnote"), 3));
    result = poll(worker); assert(result.ok && !strcmp(result.action.text, "retained"));
    worker.stop();
    assert(identity_test::durable.at({"mc-bot-timer", "files"}).size() == 72);
    assert(kv_fixture::authority()[5] == 0);
    assert(!identity_test::durable.count({"mc-bot-kv", "v00"}));
    assert(!strcmp(kv_fixture::read(39).entry.value, "retained"));
  }
  puts("PASS worker migration deadline: slow migration admits timers, timeout fences scheduler writes, same-worker retry preserves both bot identities");
}
static void workerTransactions() {
  reset();
  uint8_t bot[32]{1};
  BotWorker worker; assert(worker.begin(bot));
  const char *source =
      "function claim(v) return kv.cas('claim',false,v).status end "
      "function swap() return kv.transaction({{key='a',expect=false,value='x'},"
      "{key='b',expect=false,value='y'}}).status end";
  assert(worker.stage(source, strlen(source)) && poll(worker).ok);
  assert(worker.activate() && poll(worker).ok);
  assert(worker.invoke(event("!claim first"), 1) && worker.invoke(event("!claim second"), 2));
  unsigned committed = 0, conflict = 0;
  for (unsigned i = 0; i < 2; ++i) {
    auto result = poll(worker); assert(result.ok);
    committed += !strcmp(result.action.text, "committed"); conflict += !strcmp(result.action.text, "conflict");
  }
  assert(committed == 1 && conflict == 1);
  assert(worker.invoke(event("!swap"), 3));
  auto result = poll(worker); assert(result.ok && !strcmp(result.action.text, "committed"));
  assert(worker.invoke(event("!swap"), 4));
  result = poll(worker); assert(result.ok && !strcmp(result.action.text, "conflict"));
  worker.stop();
}
static void backups() {
  reset();
  uint8_t bot[32]{1};
  std::atomic<uint32_t> generation{1}, grant{1};
  std::atomic<bool> shared{true};
  BotStore store;
  BotIoRequest request; request.token = {1,1,1}; request.grant = 1;
  request.kind = BotIoRequest::Put; request.principal[0] = 7;
  BotIoResult result;
  const auto run = [&] { store.perform(bot, request, result, generation, shared, grant); };
  for (unsigned i = 0; i < BotKeysPerScope; ++i) {
    snprintf(request.key, sizeof(request.key), "key%u", i);
    snprintf(request.value, sizeof(request.value), "original%u", i);
    run(); assert(result.ok);
  }
  BotStore::Snapshot backup; backup.principal[0] = 7;
  char error[128]{};
  assert(store.snapshot(bot, backup, error, sizeof(error)) && backup.count == 8);
  auto damaged = backup; damaged.entries[0].value[0] ^= 1;
  assert(!BotStore::validSnapshot(damaged, bot, error, sizeof(error)));
  bot[31] = 1; assert(!BotStore::validSnapshot(backup, bot, error, sizeof(error))); bot[31] = 0;
  request.principal[31] = 8; strcpy(request.key, "other"); strcpy(request.value, "untouched"); run(); assert(result.ok);
  request.principal[31] = 0;
  for (unsigned i = 0; i < BotKeysPerScope; ++i) {
    snprintf(request.key, sizeof(request.key), "key%u", i);
    request.kind = BotIoRequest::Delete; run(); assert(result.ok);
    snprintf(request.key, sizeof(request.key), "new%u", i);
    request.kind = BotIoRequest::Put; run(); assert(result.ok);
  }
  const auto changed = identity_test::durable;
  const auto changedFiles = filesystem_test::files;
  for (unsigned cut = 1; cut <= 1; ++cut) {
    identity_test::durable = changed;
    filesystem_test::files = changedFiles;
    static unsigned commits, failAt;
    commits = 0; failAt = cut;
    identity_test::afterCommit = [] { if (++commits == failAt) identity_test::failRead = true; };
    store.restore(bot, backup, result, 1, generation);
    assert(!result.ok && result.outcome == BotIoResult::Unknown);
    identity_test::afterCommit = nullptr; identity_test::failRead = false;
    BotStore reboot;
    BotStore::Snapshot restored; restored.principal[0] = 7;
    assert(reboot.snapshot(bot, restored, error, sizeof(error)) && !memcmp(&restored, &backup, sizeof(backup)));
    request.kind = BotIoRequest::Get; request.principal[31] = 8; strcpy(request.key, "other");
    run(); assert(result.ok && !strcmp(result.value, "untouched")); request.principal[31] = 0;
  }
  ++generation;
  store.restore(bot, backup, result, 1, generation);
  assert(!result.ok && result.outcome == BotIoResult::Rejected);
  puts("PASS owner KV snapshot: canonical 2422-byte identity/scope/hash, atomic eight-key replacement, uncertain authority commit, other-principal preservation");
}
static void scheduleBackups() {
  for (bool reminder : {false, true}) {
    reset(); clockAt(); configuredNvs();
    uint8_t bot[32]{1};
    std::atomic<uint32_t> generation{1}, grant{1};
    std::atomic<bool> enabled{true}, stopping{false};
    BotTimers timers; BotReminders reminders;
    BotIoRequest request; request.token = {1,1,1}; request.grant = 1;
    request.principal[0] = 7; request.delaySeconds = 5;
    strcpy(request.value, "private work");
    BotIoResult result;
    const auto run = [&] {
      if (reminder) reminders.perform(bot, request, result, generation, enabled, grant, stopping, 0);
      else timers.perform(bot, request, result, generation, enabled, grant);
    };
    const auto snapshot = [&](BotStore::Snapshot &data) {
      data.scope = BotIoRequest::Caller; data.principal[0] = 7;
      char error[128]{};
      assert(reminder ? reminders.snapshot(bot, data, error, sizeof(error)) :
                        timers.snapshot(bot, data, error, sizeof(error)));
    };
    const auto restore = [&](const BotStore::Snapshot &data) {
      if (reminder) reminders.restore(bot, data, result, 1, generation);
      else timers.restore(bot, data, result, 1, generation);
    };
    const auto read32 = [](const uint8_t *bytes) {
      uint32_t value; memcpy(&value, bytes, sizeof(value)); return value;
    };
    uint32_t ids[2]{};
    for (unsigned i = 0; i < 2; ++i) {
      request.kind = reminder ? BotIoRequest::ReminderSet : BotIoRequest::TimerSet;
      snprintf(request.key, sizeof(request.key), "clock%u", i); run();
      assert(result.ok); ids[i] = result.revision;
    }
    BotStore::Snapshot backup; snapshot(backup); assert(backup.count == 2);
    const auto initial = identity_test::durable;
    const auto initialFiles = filesystem_test::files;
    auto corrupt = backup; corrupt.entries[0].key[0] ^= 1;
    restore(corrupt); assert(!result.ok && result.outcome == BotIoResult::Rejected && initial == identity_test::durable);
    corrupt = backup; corrupt.bot[31] = 2;
    mesh::Utils::sha256(corrupt.digest, 32, reinterpret_cast<const uint8_t *>(&corrupt), offsetof(BotStore::Snapshot, digest));
    restore(corrupt); assert(!result.ok && initial == identity_test::durable);
    const size_t paddingEntries = 630 - identity_test::usedEntries() - 307;
    identity_test::durable[{"mc-onchip", "other-sdk-data"}] =
        std::vector<uint8_t>((paddingEntries - 3) * 32);
    const auto full = identity_test::durable;
    restore(backup);
    assert(!result.ok && result.outcome == BotIoResult::Rejected && strstr(result.error, "307 free, 308 required"));
    assert(full == identity_test::durable);
    identity_test::durable = initial;
    corrupt = backup;
    auto *overflow = reinterpret_cast<uint8_t *>(corrupt.entries);
    const uint32_t maximumRevision = UINT32_MAX;
    memcpy(overflow + (reminder ? 72 : 108), &maximumRevision, sizeof(maximumRevision));
    mesh::Utils::sha256(overflow + (reminder ? 244 : 144) - 32, 32, overflow, (reminder ? 244 : 144) - 32);
    mesh::Utils::sha256(corrupt.digest, 32, reinterpret_cast<const uint8_t *>(&corrupt), offsetof(BotStore::Snapshot, digest));
    restore(corrupt);
    assert(!result.ok && strstr(result.error, "revision space exhausted") && initial == identity_test::durable);
    for (unsigned cut = 0; cut <= 1; ++cut) {
      identity_test::durable = initial;
      filesystem_test::files = initialFiles;
      static unsigned writes, failAt;
      writes = 0; failAt = cut;
      identity_test::afterCommit = [] { if (++writes == failAt) identity_test::failRead = true; };
      restore(backup);
      assert(cut ? (!result.ok && result.outcome == BotIoResult::Unknown) :
                   (result.ok && result.outcome == BotIoResult::Committed));
      identity_test::afterCommit = nullptr; identity_test::failRead = false;
      BotStore::Snapshot partial; snapshot(partial);
      assert(partial.count == 2);
      const auto *bytes = reinterpret_cast<const uint8_t *>(partial.entries);
      const size_t size = reminder ? 244 : 144;
      for (unsigned i = 0; i < partial.count; ++i) {
        const auto *record = bytes + i * size;
        assert(record[reminder ? 84 : 102] == (reminder ? uint8_t(BotReminderState::Cancelled) : uint8_t(BotTimerState::Cancelled)));
        assert(read32(record + (reminder ? 72 : 108)) > ids[1]);
        if (reminder) {
          assert(read32(record + 68) == ids[i] && read32(record + 80) == 1);
        }
      }
      restore(backup); assert(result.ok);
      snapshot(partial); assert(partial.count == 2);
      request.kind = reminder ? BotIoRequest::ReminderSet : BotIoRequest::TimerSet;
      strcpy(request.key, "fresh"); request.revision = 0; run();
      assert(result.ok && result.revision > read32(reinterpret_cast<const uint8_t *>(partial.entries) + (reminder ? 72 : 108)));
    }
    identity_test::durable = initial;
    filesystem_test::files = initialFiles;
    request.kind = reminder ? BotIoRequest::ReminderCancel : BotIoRequest::TimerCancel;
    request.revision = ids[0]; strcpy(request.key, "clock0"); run(); assert(result.ok);
    now += 6000; clockAt(Epoch + 6);
    if (reminder) {
      BotReminderDispatch dispatch; char error[128]{};
      assert(reminders.next(bot, dispatch, stopping, error, sizeof(error)));
      dispatch.generation = 1; dispatch.grant = 1; dispatch.approved = true;
      assert(reminders.claim(bot, dispatch, enabled, grant, stopping, error, sizeof(error)));
    } else {
      request.kind = BotIoRequest::TimerWait; request.revision = 0;
      strcpy(request.key, "clock1"); run(); assert(result.ok && result.timerState == BotTimerState::Claimed);
    }
    restore(backup); assert(result.ok);
    BotStore::Snapshot terminal; snapshot(terminal);
    const auto *bytes = reinterpret_cast<const uint8_t *>(terminal.entries);
    assert(bytes[reminder ? 84 : 102] == (reminder ? uint8_t(BotReminderState::Cancelled) : uint8_t(BotTimerState::Cancelled)));
    assert(bytes[(reminder ? 244 : 144) + (reminder ? 84 : 102)] ==
           (reminder ? uint8_t(BotReminderState::Unknown) : uint8_t(BotTimerState::Claimed)));
    BotStore::Snapshot other; other.principal[0] = 9;
    char error[128]{};
    assert(reminder ? reminders.snapshot(bot, other, error, sizeof(error)) : timers.snapshot(bot, other, error, sizeof(error)));
    assert(!other.count);
    // Reuse terminal slots through the product API, then try the obsolete file.
    for (unsigned i = 0; i < 10; ++i) {
      request.kind = reminder ? BotIoRequest::ReminderSet : BotIoRequest::TimerSet;
      request.revision = 0; snprintf(request.key, sizeof(request.key), "new%u", i); run(); assert(result.ok);
      request.revision = result.revision;
      request.kind = reminder ? BotIoRequest::ReminderCancel : BotIoRequest::TimerCancel; run(); assert(result.ok);
    }
    const auto reused = identity_test::durable;
    restore(backup); assert(!result.ok && result.outcome == BotIoResult::Rejected && reused == identity_test::durable);
    assert(strstr(result.error, "capacity"));
    ++generation;
    restore(backup); assert(!result.ok && strstr(result.error, "cancelled") && reused == identity_test::durable);
  }
  puts("PASS scheduler backups: private checked snapshots, explicit non-rearming imports, stable IDs/source generations, monotonic revisions, retained terminal claims, every merge cut and reused-slot stale-backup denial");
}
static void workerDataKind(const char *kind, bool blockedRecovery = false) {
  reset();
  uint8_t bot[32]{1};
  BotWorker worker; assert(worker.begin(bot));
  char reply[163]{}, command[163]{}, status[163]{};
  const auto execute = [&](const char *text) { worker.dataCommand(text, reply, sizeof(reply)); };
  execute("help"); assert(strlen(reply) <= 146);
  const auto wait = [&] {
    for (unsigned i = 0; i < 5000; ++i) {
      execute("status");
      if (strncmp(reply, "BUSY", 4) && strncmp(reply, "PENDING", 7)) return;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(false);
  };
  snprintf(command, sizeof(command), "export %s caller 0700000000000000000000000000000000000000000000000000000000000000", kind);
  execute(command);
  assert(!strncmp(reply, "PENDING", 7)); wait(); strcpy(status, reply);
  char hash[65], id[17];
  assert(sscanf(status, "EXPORTED %64s %16s", hash, id) == 2);
  std::string chunks[51];
  snprintf(command, sizeof(command), "read %s 4294967296", id); execute(command);
  assert(!strncmp(reply, "Error:", 6));
  for (unsigned i = 0; i < 51; ++i) {
    snprintf(command, sizeof(command), "read %s %u", id, i); execute(command);
    assert(!strncmp(reply, "DATA ", 5)); chunks[i] = reply + 5;
  }
  snprintf(command, sizeof(command), "begin %s %s", id, hash); execute(command); assert(!strncmp(reply, "UPLOADING", 9));
  snprintf(command, sizeof(command), "chunk %s 4294967296 %s", id, chunks[0].c_str()); execute(command);
  assert(!strncmp(reply, "Error:", 6));
  snprintf(command, sizeof(command), "stage %s", id); execute(command); assert(!strncmp(reply, "Error:", 6));
  for (unsigned i = 0; i < 51; ++i) {
    snprintf(command, sizeof(command), "chunk %s %u %s", id, i, chunks[i].c_str()); execute(command);
    assert(!strncmp(reply, "RECEIVED", 8));
  }
  snprintf(command, sizeof(command), "stage %s", id); execute(command); wait(); assert(!strncmp(reply, "STAGED ", 7));
  if (blockedRecovery) identity_test::durable[{"mc-bot-kv", "txn"}] = std::vector<uint8_t>(48);
  snprintf(command, sizeof(command), "restore %s", id);
  if (strcmp(kind, "kv")) { execute(command); assert(!strncmp(reply, "Error:", 6)); strcat(command, " no-rearm"); }
  execute(command); wait();
  if (blockedRecovery) {
    assert(!strncmp(reply, "REJECTED", 8) && strstr(reply, "KV recovery"));
    worker.stop(); return;
  }
  assert(!strncmp(reply, "COMMITTED ", 10));
  const auto committed = identity_test::commits;
  execute(command); assert(!strncmp(reply, "COMMITTED ", 10) && identity_test::commits == committed);
  worker.stop(); assert(worker.begin(bot)); execute("status"); assert(!strncmp(reply, "EMPTY", 5)); worker.stop();
}
static void workerData() {
  for (const char *kind : {"kv", "timers", "reminders"}) workerDataKind(kind);
  workerDataKind("timers", true);
  puts("PASS management data worker: all three families, checked chunks/staging, explicit non-rearming scheduler consent, commit/no retry replay, reboot clears staging");
}
static void workerEvents() {
  reset();
  uint8_t bot[32]{1};
  BotWorker worker; assert(worker.begin(bot));
  const char *source =
      "function _start() sleep(100) kv.put('event','done','bot') end events.on('startup','_start') "
      "function wait() sleep(100) return 'command' end";
  assert(worker.stage(source, strlen(source)) && poll(worker).ok);
  assert(worker.activate() && poll(worker).ok);
  BotEvent e; e.kind = BotEvent::Startup; e.sharedState = true;
  assert(!worker.invoke(e, 1) && worker.setEventAccess(1) && worker.setSharedState(true));
  assert(worker.invoke(e, 1));
  for (unsigned i = 0; i < 20; ++i) assert(!worker.invoke(e, 10 + i));
  assert(worker.invoke(event("!wait"), 2) && worker.invoke(event("!wait"), 3));
  assert(!worker.invoke(event("!wait"), 4));
  assert(worker.invoke(event("!ping"), 4));
  auto result = poll(worker); assert(result.ok && result.job == 4 && !strcmp(result.action.text, "Pong"));
  assert(worker.setEventAccess(0)); result = poll(worker);
  assert(!result.ok && result.operation == BotWorker::Operation::Event);
  now += 100;
  assert(poll(worker).ok && poll(worker).ok);
  assert(identity_test::durable.empty());
  assert(worker.setEventAccess(1) && worker.invoke(e, 5));
  std::this_thread::sleep_for(std::chrono::milliseconds(10)); now += 100;
  result = poll(worker); assert(result.ok && result.operation == BotWorker::Operation::Event &&
      result.action.kind == BotAction::None);
  for (unsigned i = 0; i < 5; ++i) {
    assert(worker.invoke(e, 10 + i));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    assert(worker.setEventAccess(0));
    result = poll(worker); assert(!result.ok);
    assert(worker.setEventAccess(1));
  }
  assert(worker.invoke(event("!wait"), 20));
  std::this_thread::sleep_for(std::chrono::milliseconds(10)); now += 100;
  result = poll(worker); assert(result.ok && !strcmp(result.action.text, "command"));
  worker.stop(); assert(worker.begin(bot)); assert(!worker.eventMask()); worker.stop();
  puts("PASS event worker: default-off grants, one event slot, bounded flood, reserved public Pong, revoke-before-write, yielding persistence and reboot fences");
}
int main() {
  physicalScalarCapacity();
  physicalAtomicCapacity();
  workerEvents();
  transactions();
  workerTransactions();
  workerRecoveryReserve();
  workerMigrationDeadline();
  backups();
  scheduleBackups();
  workerData();
  storage();
  listStorage();
  durableTimers();
  timerLimits();
  workerTimers();
  reservedCapacity();
  reminderJournal();
  timerClockSuspension();
  workerNotes();
  legacyList();
  workerBoard();
  boardSlots();
  assert(identity_test::handles.empty() && psram_test::allocations.empty());
}
