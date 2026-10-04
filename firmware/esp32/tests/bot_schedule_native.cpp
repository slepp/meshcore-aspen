// SPDX-License-Identifier: Apache-2.0
#include "bot_schedule_fixture.h"
#include "BotWorker.h"
#include "Clock.h"
#include <SPIFFS.h>
#include <chrono>
#include <filesystem>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>

using namespace onchip;
using namespace schedule_fixture;
static const auto started = std::chrono::steady_clock::now();
unsigned long millis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
}
void delay(unsigned long ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
static BotWorker::Result poll(BotWorker &worker) {
  BotWorker::Result result;
  for (unsigned i = 0; i < 5000; ++i) {
    if (worker.poll(result)) return result;
    delay(1);
  }
  assert(false && "Native scheduler worker timeout"); return result;
}
static void invoke(BotWorker &worker, const char *command, const char *expected, bool contains = false) {
  BotEvent event; char error[128];
  assert(parseBotCommand(command, strlen(command), event, error, sizeof(error)));
  event.authenticated = true; event.sender[0] = 7;
  assert(worker.invoke(event, 1));
  const auto result = poll(worker);
  assert(result.ok && (contains ? strstr(result.action.text, expected) != nullptr : !strcmp(result.action.text, expected)));
}
int main(int argc, char **argv) {
  assert(argc == 2 && argv[1][0] == '/');
  const std::string root = std::string(argv[1]) + "/schedule-native-" + std::to_string(getpid());
  const auto nvs = root + "/nvs", spiffs = root + "/spiffs";
  assert(mkdir(root.c_str(), 0700) == 0 && mkdir(nvs.c_str(), 0700) == 0 && mkdir(spiffs.c_str(), 0700) == 0);
  const auto init = [&] {
    assert(native_nvs_init(nvs.c_str()) == ESP_OK && native_spiffs_init(spiffs.c_str()));
    beginClocks(); beginNetworkClock(true); receiveNetworkTime(Epoch); loopClocks();
  };
  init(); seedLegacy();
  uint8_t bot[32]{1};
  std::atomic<uint32_t> generation{1}, grant{1};
  std::atomic<bool> enabled{true}, stopping{false};
  BotStore::Snapshot timerBackup, reminderBackup;
  char error[128];
  {
    BotTimers timers; BotReminders reminders;
    verifyLegacy(timers, reminders);
    timerBackup.principal[0] = reminderBackup.principal[0] = 7;
    assert(timers.snapshot(bot, timerBackup, error, sizeof(error)) && timerBackup.count == 1);
    assert(reminders.snapshot(bot, reminderBackup, error, sizeof(error)) && reminderBackup.count == 1);
    BotIoRequest request; request.kind = BotIoRequest::TimerWait; request.token = {1, 1, 1};
    request.principal[0] = 7; strcpy(request.key, "timer0");
    BotIoResult result;
    timers.perform(bot, request, result, generation, enabled, grant);
    assert(result.ok && result.timerState == BotTimerState::Claimed);
    BotReminderDispatch dispatch;
    assert(reminders.next(bot, dispatch, stopping, error, sizeof(error)) && dispatch.id == 1);
    dispatch.grant = 1;
    assert(reminders.claim(bot, dispatch, enabled, grant, stopping, error, sizeof(error)));
    dispatch.outcome = BotReminderState::Sent;
    assert(reminders.complete(bot, dispatch, stopping, error, sizeof(error)));
    assert(!reminders.claim(bot, dispatch, enabled, grant, stopping, error, sizeof(error)));
    timers.restore(bot, timerBackup, result, 1, generation);
    assert(result.ok && result.outcome == BotIoResult::Committed);
    reminders.restore(bot, reminderBackup, result, 1, generation);
    assert(result.ok && result.outcome == BotIoResult::Committed);
  }
  nvs_stats_t stats;
  assert(nvs_get_stats(nullptr, &stats) == ESP_OK && stats.used_entries == 12);
  native_spiffs_shutdown(); native_nvs_shutdown(); init();
  const char *source =
      "function timerstate() return timer.get('timer0').state end "
      "function personal() return reminder.list() end";
  const auto startWorker = [&](BotWorker &worker) {
    assert(worker.begin(bot));
    assert(worker.stage(source, strlen(source)) && poll(worker).ok);
    assert(worker.activate() && poll(worker).ok);
  };
  {
    BotWorker worker; startWorker(worker);
    invoke(worker, "!timerstate", "claimed");
    invoke(worker, "!personal", "1 sent", true);
    worker.stop();
    // The native rekey path stops the old worker and opens this same persistent
    // state directory with the new full identity; data ownership is unchanged.
    bot[31] = 9; startWorker(worker);
    invoke(worker, "!timerstate", "missing");
    invoke(worker, "!personal", "No personal reminders");
    worker.stop();
    bot[31] = 0; startWorker(worker);
    invoke(worker, "!timerstate", "claimed");
    invoke(worker, "!personal", "1 sent", true);
    worker.stop();
  }
  {
    BotTimers timers; BotReminders reminders;
    BotIoResult result;
    timers.restore(bot, timerBackup, result, 1, generation); assert(result.ok);
    reminders.restore(bot, reminderBackup, result, 1, generation); assert(result.ok);
    BotIoRequest request; request.kind = BotIoRequest::TimerWait; request.token = {1, 1, 1};
    request.principal[0] = 7; strcpy(request.key, "timer0");
    timers.perform(bot, request, result, generation, enabled, grant);
    assert(!result.ok && result.timerState == BotTimerState::Claimed);
    BotReminderDispatch dispatch;
    assert(!reminders.next(bot, dispatch, stopping, error, sizeof(error)) && !error[0]);
  }
  assert(nvs_get_stats(nullptr, &stats) == ESP_OK && stats.used_entries == 12);
  native_spiffs_shutdown(); native_nvs_shutdown();
  for (const char *bank : {"a", "b"})
    assert(unlink((spiffs + "/spiffs.timers-" + bank + ".bin").c_str()) == 0);
  init();
  {
    BotTimers timers; BotIoResult result;
    BotIoRequest request; request.kind = BotIoRequest::TimerGet; request.token = {1, 1, 1};
    request.principal[0] = 7; strcpy(request.key, "timer0");
    timers.perform(bot, request, result, generation, enabled, grant);
    assert(!result.ok && strstr(result.error, "file corrupt/unavailable"));
  }
  native_spiffs_shutdown(); native_nvs_shutdown();
  std::filesystem::remove_all(root);
  puts("PASS actual POSIX scheduler: all legacy slots, commit/fsync and adapter restart, worker full-key rekey isolation, BTD1/BRD1 restore cannot replay claimed/sent effects, missing active file blocks recovery");
}
