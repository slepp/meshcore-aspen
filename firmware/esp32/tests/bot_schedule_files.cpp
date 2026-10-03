// SPDX-License-Identifier: Apache-2.0
#include "bot_schedule_fixture.h"
#include "BotScheduleFiles.h"
#include "Clock.h"
#include <esp_heap_caps.h>
#include <limits>

using namespace onchip;
using namespace schedule_fixture;
static unsigned long now;
unsigned long millis() { return now; }
void delay(unsigned long ms) { now += ms; }
static void faultsOff() {
  identity_test::step = filesystem_test::step = nullptr;
  identity_test::failRead = identity_test::failWrite = identity_test::failCommit = false;
  identity_test::failErase = identity_test::failStats = false;
  identity_test::afterWrite = identity_test::afterCommit = identity_test::afterErase = nullptr;
  identity_test::readHook = nullptr;
  filesystem_test::afterWrite = filesystem_test::afterFlush = nullptr;
  filesystem_test::failOpen = filesystem_test::appendOnRead = false;
  filesystem_test::readLimit = filesystem_test::writeLimit = std::numeric_limits<size_t>::max();
  filesystem_test::readDelayMs = 0;
  now = 0; beginClocks(); beginNetworkClock(true); receiveNetworkTime(Epoch); loopClocks();
}
static void reset() {
  faultsOff(); assert(identity_test::handles.empty());
  identity_test::durable.clear(); identity_test::namespaces.clear(); filesystem_test::files.clear();
  identity_test::entryCapacity = identity_test::peakEntries = 0;
  identity_test::freeEntries = 4096; identity_test::eagerWrites = false;
}
struct Cut {};
static unsigned at, target;
static void checkpoint(const char *) { if (++at == target) throw Cut{}; }
template<class Operation, class Verify> static unsigned cuts(Operation operation, Verify verify) {
  const auto durable = identity_test::durable;
  const auto files = filesystem_test::files;
  unsigned count = 0;
  for (unsigned cut = 0;; ++cut) {
    faultsOff(); identity_test::durable = durable; filesystem_test::files = files;
    at = 0; target = cut; identity_test::step = filesystem_test::step = checkpoint;
    bool interrupted = false;
    try { operation(); } catch (const Cut &) { interrupted = true; }
    if (!cut) count = at; else assert(interrupted);
    faultsOff(); identity_test::handles.clear();
    verify(); assert(identity_test::handles.empty());
    if (cut == count) return count;
  }
}
struct Client {
  BotTimers timers; BotReminders reminders;
  uint8_t bot[32]{1};
  std::atomic<uint32_t> generation{1}, grant{1};
  std::atomic<bool> shared{true}, stopping{false};
  BotIoRequest request; BotIoResult result;
  Client() {
    request.token = {1, 1, 1}; request.grant = 1; request.principal[0] = 7;
    request.delaySeconds = 1; strcpy(request.key, "tea"); strcpy(request.value, "tea");
  }
  void run(bool reminder) {
    if (reminder) reminders.perform(bot, request, result, generation, shared, grant, stopping, 0);
    else timers.perform(bot, request, result, generation, shared, grant);
  }
  BotStore::Snapshot snapshot(bool reminder) {
    BotStore::Snapshot data; data.scope = request.scope; memcpy(data.principal, request.principal, 32);
    char error[128];
    assert(reminder ? reminders.snapshot(bot, data, error, sizeof(error)) :
                      timers.snapshot(bot, data, error, sizeof(error)));
    return data;
  }
};
static void migration() {
  for (bool eager : {false, true}) {
    reset(); identity_test::eagerWrites = eager; seedLegacy();
    identity_test::durable[{"mc-bot-timer", "unrelated"}] = {1, 2};
    const auto count = cuts([] {
      BotTimers timers; BotReminders reminders; verifyLegacy(timers, reminders);
    }, [] {
      BotTimers timers; BotReminders reminders; verifyLegacy(timers, reminders);
      assert(identity_test::durable.at({"mc-bot-timer", "files"}).size() == 72);
      assert(identity_test::durable.at({"mc-bot-remind", "files"}).size() == 72);
      assert(identity_test::durable.at({"mc-bot-timer", "files"})[5] == 0 &&
             identity_test::durable.at({"mc-bot-remind", "files"})[5] == 0);
      assert(!identity_test::durable.count({"mc-bot-timer", "t00"}) &&
             !identity_test::durable.count({"mc-bot-timer", "r01"}) &&
             !identity_test::durable.count({"mc-bot-remind", "r07"}));
      assert((identity_test::durable.at({"mc-bot-timer", "unrelated"}) == std::vector<uint8_t>{1, 2}));
    });
    printf("PASS scheduler migration: %u file/NVS cuts, all 18 deployed records/scopes/full identities/states retained, eager=%u\n", count, eager);
  }
}
static void claimsAndRestore() {
  for (bool reminder : {false, true}) for (bool eager : {false, true}) {
    reset(); identity_test::eagerWrites = eager;
    Client c; c.request.kind = reminder ? BotIoRequest::ReminderSet : BotIoRequest::TimerSet;
    c.run(reminder); assert(c.result.ok && c.result.outcome == BotIoResult::Committed);
    auto backup = c.snapshot(reminder);
    const auto claim = [&] {
      receiveNetworkTime(Epoch + 2); loopClocks();
      if (reminder) {
        BotReminderDispatch dispatch; char error[128];
        assert(c.reminders.next(c.bot, dispatch, c.stopping, error, sizeof(error)));
        dispatch.grant = 1;
        assert(c.reminders.claim(c.bot, dispatch, c.shared, c.grant, c.stopping, error, sizeof(error)));
      } else {
        c.request.kind = BotIoRequest::TimerWait; c.run(false); assert(c.result.ok);
      }
    };
    const auto count = cuts(claim, [&] {
      Client reboot; auto data = reboot.snapshot(reminder);
      const auto *raw = reinterpret_cast<const uint8_t *>(data.entries);
      const unsigned state = raw[reminder ? 84 : 102];
      assert(state == 1 || state == (reminder ? unsigned(BotReminderState::Unknown) : unsigned(BotTimerState::Claimed)));
      if (state != 1) {
        receiveNetworkTime(Epoch + 2); loopClocks();
        if (reminder) {
          BotReminderDispatch dispatch; char error[128];
          assert(!reboot.reminders.next(reboot.bot, dispatch, reboot.stopping, error, sizeof(error)) && !error[0]);
        } else {
          reboot.request.kind = BotIoRequest::TimerWait; reboot.run(false);
          assert(!reboot.result.ok && strstr(reboot.result.error, "already claimed"));
        }
      }
    });
    printf("PASS durable %s claim: %u publication cuts, committed claim never offered again, eager=%u\n",
           reminder ? "reminder" : "timer", count, eager);
    faultsOff();
    Client fresh;
    fresh.request.kind = reminder ? BotIoRequest::ReminderSet : BotIoRequest::TimerSet;
    fresh.run(reminder); assert(fresh.result.ok);
    backup = fresh.snapshot(reminder);
    const auto restoreCount = cuts([&] {
      if (reminder) fresh.reminders.restore(fresh.bot, backup, fresh.result, 1, fresh.generation);
      else fresh.timers.restore(fresh.bot, backup, fresh.result, 1, fresh.generation);
      assert(fresh.result.ok && fresh.result.outcome == BotIoResult::Committed);
    }, [&] {
      Client reboot; auto data = reboot.snapshot(reminder);
      const auto *raw = reinterpret_cast<const uint8_t *>(data.entries);
      const auto *old = reinterpret_cast<const uint8_t *>(backup.entries);
      assert(data.count == backup.count);
      for (unsigned i = 0; i < data.count; ++i) {
        const size_t offset = i * (reminder ? sizeof(Reminder) : sizeof(Timer)) + (reminder ? 84 : 102);
        assert(raw[offset] == old[offset] || (old[offset] == 1 && raw[offset] ==
               (reminder ? uint8_t(BotReminderState::Cancelled) : uint8_t(BotTimerState::Cancelled))));
      }
    });
    printf("PASS no-rearm %s restore: %u publication cuts, eager=%u\n", reminder ? "reminder" : "timer", restoreCount, eager);
  }
}
static void initialAndCapacity() {
  for (bool reminder : {false, true}) for (bool eager : {false, true}) {
    reset(); identity_test::eagerWrites = eager;
    const auto count = cuts([&] {
      Client c; c.request.kind = reminder ? BotIoRequest::ReminderSet : BotIoRequest::TimerSet;
      c.run(reminder); assert(c.result.ok && c.result.outcome == BotIoResult::Committed);
    }, [&] {
      Client reboot; auto data = reboot.snapshot(reminder);
      assert(data.count <= 1);
      if (data.count) assert(reinterpret_cast<const uint8_t *>(data.entries)[reminder ? 84 : 102] == 1);
    });
    printf("PASS initial %s publication: %u initialization/file/authority cuts, eager=%u\n",
           reminder ? "reminder" : "timer", count, eager);
  }
  for (bool reminder : {false, true}) for (bool admitted : {false, true}) {
    reset();
    // Model migration while all other consumers occupy the unchanged 630-entry partition.
    identity_test::entryCapacity = 630;
    identity_test::namespaces = {"mc-onchip", reminder ? "mc-bot-remind" : "mc-bot-timer"};
    if (reminder) seed("mc-bot-remind", "r00", schedule_fixture::reminder(0));
    else seed("mc-bot-timer", "t00", schedule_fixture::timer(0));
    const size_t required = 126 + BotNvsMutationEntries + BotScheduleAuthorityEntries + BotKvBootstrapEntries;
    const size_t fill = 630 - identity_test::usedEntries() - required + !admitted;
    identity_test::durable[{"mc-onchip", "other"}] = std::vector<uint8_t>((fill - 3) * 32);
    const auto before = identity_test::durable;
    Client c; c.request.kind = reminder ? BotIoRequest::ReminderList : BotIoRequest::TimerGet;
    strcpy(c.request.key, "timer0"); c.run(reminder);
    assert(c.result.ok == admitted);
    if (!admitted) assert(before == identity_test::durable && strstr(c.result.error, "headroom"));
    else {
      assert(identity_test::peakEntries <= 630 - 126);
      assert(identity_test::durable.at({reminder ? "mc-bot-remind" : "mc-bot-timer", "files"}).size() == 72);
      const size_t reads = filesystem_test::readCalls;
      c.run(reminder); c.run(reminder); assert(c.result.ok && reads == filesystem_test::readCalls);
    }
  }
  puts("PASS scheduler exact migration metadata budget, unchanged 126-entry GC reserve and zero redundant cached file reads");
}
static void failures() {
  for (bool reminder : {false, true}) {
    reset(); Client c;
    c.request.kind = reminder ? BotIoRequest::ReminderSet : BotIoRequest::TimerSet;
    c.run(reminder); assert(c.result.ok);
    auto before = c.snapshot(reminder);
    const auto files = filesystem_test::files;
    for (unsigned fault = 0; fault < 5; ++fault) {
      faultsOff(); filesystem_test::files = files;
      if (fault == 0) filesystem_test::failOpen = true;
      if (fault == 1) filesystem_test::writeLimit = 17;
      if (fault == 2) filesystem_test::readLimit = 17;
      if (fault == 3) filesystem_test::appendOnRead = true;
      if (fault == 4) identity_test::failStats = true;
      // Replacement can fail after changing the PSRAM bank; retry must reread the committed bank.
      if (reminder) { c.request.kind = BotIoRequest::ReminderCancel; c.request.revision = 1; }
      else c.request.kind = BotIoRequest::TimerCancel;
      c.run(reminder); assert(!c.result.ok);
      faultsOff();
      if (fault == 3) {
        c.request.kind = reminder ? BotIoRequest::ReminderList : BotIoRequest::TimerGet;
        c.run(reminder); assert(!c.result.ok && strstr(c.result.error, "corrupt"));
      }
      filesystem_test::files = files;
      auto after = c.snapshot(reminder); assert(!memcmp(&before, &after, sizeof(before)));
    }
    static std::atomic<uint32_t> *generation;
    generation = &c.generation;
    filesystem_test::afterFlush = [] { ++*generation; };
    if (reminder) c.request.kind = BotIoRequest::ReminderSet;
    else c.request.kind = BotIoRequest::TimerSet;
    c.run(reminder); assert(!c.result.ok && c.result.outcome == BotIoResult::Rejected);
    faultsOff(); c.request.token.generation = c.generation;
    auto after = c.snapshot(reminder); assert(!memcmp(&before, &after, sizeof(before)));
    const char *space = reminder ? "mc-bot-remind" : "mc-bot-timer";
    const unsigned bank = identity_test::durable.at({space, "files"})[4];
    const std::string path = std::string("/command-bot/") + (reminder ? "reminders" : "timers") +
        (bank ? "-b.bin" : "-a.bin");
    filesystem_test::files.at(path).back() ^= 1;
    { Client reboot; reboot.request.kind = reminder ? BotIoRequest::ReminderList : BotIoRequest::TimerGet;
      reboot.run(reminder); assert(!reboot.result.ok && strstr(reboot.result.error, "corrupt")); }
    filesystem_test::files.at(path).back() ^= 1;
    identity_test::durable.erase({space, "files"});
    Client reboot; reboot.request.kind = reminder ? BotIoRequest::ReminderList : BotIoRequest::TimerGet;
    reboot.run(reminder); assert(!reboot.result.ok && strstr(reboot.result.error, "lack authority"));
  }
  puts("PASS scheduler inactive file/short read/write/growth/stats/cancellation failures, dirty-cache retry, corrupt file and missing authority fail closed");
}
static void invalidLegacy() {
  for (bool reminder : {false, true}) for (bool duplicate : {false, true}) {
    reset();
    if (reminder) {
      const auto first = schedule_fixture::reminder(0);
      seed("mc-bot-remind", "r00", first);
      auto second = duplicate ? first : schedule_fixture::reminder(7);
      if (!duplicate) second.digest[31] ^= 1;
      seed("mc-bot-remind", "r07", second);
    } else {
      const auto first = schedule_fixture::timer(0);
      seed("mc-bot-timer", "t00", first);
      auto second = duplicate ? first : schedule_fixture::timer(7);
      if (!duplicate) second.digest[31] ^= 1;
      seed("mc-bot-timer", "t07", second);
    }
    const auto before = identity_test::durable;
    Client c; c.request.kind = reminder ? BotIoRequest::ReminderList : BotIoRequest::TimerGet;
    c.run(reminder);
    assert(!c.result.ok && before == identity_test::durable && filesystem_test::files.empty());
    assert(strstr(c.result.error, "corrupt/duplicate"));
  }
  puts("PASS deployed legacy corruption/duplicate validation completes before any file or metadata publication");
}
static void bootstrapUncertainty() {
  static std::atomic<uint32_t> *generation;
  for (unsigned family = 0; family < 3; ++family)
    for (bool eager : {false, true}) for (unsigned fault = 0; fault < 4; ++fault) {
      reset(); identity_test::eagerWrites = eager;
      Client c; BotStore kv; generation = &c.generation;
      const bool reminder = family == 2;
      c.request.kind = !family ? BotIoRequest::Put : reminder ? BotIoRequest::ReminderSet : BotIoRequest::TimerSet;
      const auto mutate = [&] {
        if (!family) kv.perform(c.bot, c.request, c.result, c.generation, c.shared, c.grant);
        else c.run(reminder);
      };
      if (!fault) identity_test::failCommit = true;
      if (fault == 1) identity_test::afterCommit = [] { identity_test::failRead = true; };
      if (fault == 2) identity_test::afterWrite = [] { ++*generation; };
      if (fault == 3) filesystem_test::failOpen = true;
      mutate();
      assert(!c.result.ok && c.result.outcome == (fault == 3 ? BotIoResult::Rejected : BotIoResult::Unknown) &&
             filesystem_test::files.empty());
      const char *space = !family ? "mc-bot-kv" : reminder ? "mc-bot-remind" : "mc-bot-timer";
      const auto marker = identity_test::durable.find({space, !family ? "txn" : "files"});
      assert((marker != identity_test::durable.end()) == (eager || fault == 1 || fault == 3));
      if (marker != identity_test::durable.end()) {
        assert(marker->second.size() == 48);
        assert(!memcmp(marker->second.data(), !family ? "BKI\1" : reminder ? "BRI\1" : "BTI\1", 4));
      }
      faultsOff();
      const auto snapshot = [&] {
        if (family) return c.snapshot(reminder);
        BotStore::Snapshot data; data.principal[0] = 7; char error[128];
        assert(kv.snapshot(c.bot, data, error, sizeof(error))); return data;
      };
      assert(!snapshot().count);
      c.request.token.generation = c.generation;
      mutate(); assert(c.result.ok && c.result.outcome == BotIoResult::Committed);
      const auto data = snapshot(); assert(data.count == 1);
      if (!family) assert(!strcmp(data.entries[0].value, "tea"));
      else assert(reinterpret_cast<const uint8_t *>(data.entries)[reminder ? 84 : 102] == 1);
    }
  puts("PASS KV/timer/reminder bootstrap: indeterminate commit/readback/cancellation retain UNKNOWN, verified marker plus file failure is REJECTED; recovery does not publish speculative payloads");
}
int main() {
  migration(); initialAndCapacity(); claimsAndRestore(); failures(); invalidLegacy(); bootstrapUncertainty();
  assert(identity_test::handles.empty() && psram_test::allocations.empty());
}
