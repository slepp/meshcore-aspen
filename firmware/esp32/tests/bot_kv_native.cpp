// SPDX-License-Identifier: Apache-2.0
#include "BotWorker.h"
#include "Clock.h"
#include <SPIFFS.h>
#include <Utils.h>
#include <nvs.h>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>

using namespace onchip;
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
  assert(false && "Native durable worker timed out");
  return result;
}
static void invoke(BotWorker &worker, unsigned principal, const char *command, const char *expected) {
  BotEvent event;
  char error[128];
  assert(parseBotCommand(command, strlen(command), event, error, sizeof(error)));
  event.authenticated = event.sharedState = true; event.sender[0] = principal;
  assert(worker.invoke(event, principal));
  const auto result = poll(worker);
  if (!result.ok || strcmp(result.action.text, expected))
    fprintf(stderr, "Native KV command %s: %s (%s)\n", command, result.error, result.action.text);
  assert(result.ok && !strcmp(result.action.text, expected));
}
int main(int argc, char **argv) {
  assert(argc == 2 && argv[1][0] == '/');
  const std::string root = std::string(argv[1]) + "/kv-native-" + std::to_string(getpid());
  const auto nvs = root + "/nvs", spiffs = root + "/spiffs";
  assert(mkdir(root.c_str(), 0700) == 0 && mkdir(nvs.c_str(), 0700) == 0 &&
         mkdir(spiffs.c_str(), 0700) == 0);
  const auto init = [&] {
    assert(native_nvs_init(nvs.c_str()) == ESP_OK && native_spiffs_init(spiffs.c_str()));
    beginClocks(); beginNetworkClock(true);
  };
  init();
  nvs_handle_t handle;
  assert(nvs_open("mc-bot-kv", NVS_READWRITE, &handle) == ESP_OK);
  uint8_t legacy[392]{};
  memcpy(legacy, "BKV\1", 4); legacy[4] = legacy[36] = legacy[38] = 1;
  strcpy(reinterpret_cast<char *>(legacy + 70), "key0");
  strcpy(reinterpret_cast<char *>(legacy + 103), "legacy");
  mesh::Utils::sha256(legacy + 360, 32, legacy, 360);
  assert(nvs_set_blob(handle, "v00", legacy, sizeof(legacy)) == ESP_OK && nvs_commit(handle) == ESP_OK);
  nvs_close(handle);
  const char *source =
      "function save() for i=0,7 do kv.put('key'..i,'value'..i) end return 'saved' end "
      "function countkeys() return tostring(kv.list().count) end "
      "function readnote() return kv.get('key0') or 'absent' end "
      "function own() for i=0,7 do kv.put('owner'..i,'reserved','bot') end return 'owned' end "
      "function owner_read() return kv.get('owner0','bot') or 'absent' end "
      "function swap() return kv.transaction({{key='key0',expect='value0',value='swapped'},"
      "{key='key1',expect='value1',value='also-swapped'}}).status end";
  uint8_t hash[32]; mesh::Utils::sha256(hash, sizeof(hash), reinterpret_cast<const uint8_t *>(source), strlen(source));
  auto file = SPIFFS.open(BotStagedSourcePath, "w");
  assert(file && file.write(reinterpret_cast<const uint8_t *>(source), strlen(source)) == strlen(source));
  file.flush(); file.close(); assert(!native_spiffs_faulted());
  uint8_t bot[32]{1};
  const auto startWorker = [&](BotWorker &worker) {
    assert(worker.begin(bot) && worker.setSharedState(true));
    assert(worker.stageFile(strlen(source), hash));
    const auto staged = poll(worker);
    if (!staged.ok) fprintf(stderr, "Native source stage: %s\n", staged.error);
    assert(staged.ok);
    assert(worker.activate() && poll(worker).ok);
  };
  {
    BotWorker worker; startWorker(worker);
    invoke(worker, 1, "!readnote", "legacy");
    assert(nvs_open("mc-bot-kv", NVS_READONLY, &handle) == ESP_OK);
    size_t size = 0;
    assert(nvs_get_blob(handle, "v00", nullptr, &size) == ESP_ERR_NVS_NOT_FOUND);
    assert(nvs_get_blob(handle, "txn", nullptr, &size) == ESP_OK && size == 200);
    nvs_close(handle);
    for (unsigned principal = 1; principal <= 4; ++principal) {
      invoke(worker, principal, "!save", "saved");
      invoke(worker, principal, "!countkeys", "8");
    }
    invoke(worker, 1, "!own", "owned");
    invoke(worker, 1, "!swap", "committed");
    invoke(worker, 1, "!swap", "conflict");
    const char *replacement = "function readnote() return 'changed:'..kv.get('key0') end";
    assert(worker.stage(replacement, strlen(replacement)) && poll(worker).ok);
    assert(worker.activate() && poll(worker).ok);
    invoke(worker, 1, "!readnote", "changed:swapped");
    worker.stop();
  }
  nvs_stats_t stats; assert(nvs_get_stats(nullptr, &stats) == ESP_OK && stats.used_entries == 10);
  native_spiffs_shutdown(); native_nvs_shutdown();
  init();
  {
    BotWorker worker; startWorker(worker);
    invoke(worker, 1, "!readnote", "swapped");
    for (unsigned principal = 2; principal <= 4; ++principal) invoke(worker, principal, "!readnote", "value0");
    invoke(worker, 1, "!owner_read", "reserved");
    worker.stop();
    bot[0] = 2; startWorker(worker);
    invoke(worker, 1, "!readnote", "absent");
    invoke(worker, 1, "!owner_read", "absent");
    worker.stop();
    bot[0] = 1; startWorker(worker);
    invoke(worker, 1, "!readnote", "swapped");
    invoke(worker, 1, "!owner_read", "reserved");
    worker.stop();
  }
  native_spiffs_shutdown(); native_nvs_shutdown();
  std::filesystem::remove_all(root);
  puts("PASS actual POSIX worker: legacy migration, all 40 KV slots, CAS/transaction, source replacement/file reload, adapter restart and identity isolation");
}
