#include <Arduino.h>
#include <utility/debug.h>
#include <cmath>
#include <inttypes.h>
#include "MyMesh.h"
#include "CommandBot.h"
#include "RuntimeConfig.h"
#include "SharedRadio.h"
#include "NoteStore.h"
#include "PineQspiFlash.h"
#include "NoteDiagnostics.h"
#include "CompanionInterface.h"
#include "PineFieldUpdate.h"
#include <helpers/sensors/GpsTime.h>
#include <helpers/nrf52/SerialBLEInterface.h>
#if NRFMAST_PRODUCTION_LUA
#include "PineAdmin.h"
#include "onchip/CommandBot.h"
#include "PineFilesystem.h"
#include "PineNotesMigration.h"
#include "PineRuntimePlatform.h"
#include "platform/nvs.h"
static bool luaAdminCommand(const char*, char*);
#endif

static bool statsCommand(const char*, char*);

#ifndef NRFMAST_RF_ENABLED
#error "Select the nrfmast_rx or nrfmast environment"
#endif

class Repeater : public MyMesh {
protected:
  uint32_t getCADFailMaxDuration() const override { return UINT32_MAX; }
  void logTxFail(mesh::Packet* packet, int length) override {
    MyMesh::logTxFail(packet, length);
    Serial.println("repeater: transmit failed (RF is disabled in nrfmast_rx)");
  }
public:
  nrfmast::CompanionRadioInfo activeProfile = {};
  void onRadioProfile(float frequency, float bandwidth, uint8_t sf, uint8_t cr) override {
    activeProfile.frequency = frequency;
    activeProfile.bandwidth = bandwidth;
    activeProfile.sf = sf;
    activeProfile.cr = cr;
  }
  nrfmast::RuntimeConfig* runtimeConfig = nullptr;
  unsigned queuedCount() const { return _mgr->getOutboundTotal(); }
  using MyMesh::MyMesh;
  const mesh::PacketManager& packetManager() const { return *_mgr; }
  void handleCommand(uint32_t senderTimestamp, char* text, char* reply) override {
    char* body = text;
    while (*body == ' ') ++body;
    char* output = reply;
    if (strlen(body) > 4 && body[2] == '|') {
      memcpy(output, body, 3);
      output += 3;
      body += 3;
    }
    if (nrfmast::fieldUpdateCommand(senderTimestamp, body, output)) return;
    if (nrfmast::fieldUpdatePending()) {
      strcpy(output, "Error: BLE firmware update armed; use stop ota or wait for restart"); return;
    }
    // CommonCLI uses prefix matching, so fence these prefixes as well.
    if (!strncmp(body, "poweroff", 8) || !strncmp(body, "shutdown", 8)) {
      strcpy(output, "Error: SYSTEMOFF strands Pine; use reboot or disconnect local power"); return;
    }
    if (statsCommand(body, output)) return;
    if (runtimeConfig && !strcmp(body, "bot time.fetch") &&
        runtimeConfig->handleCommand(senderTimestamp, body, output)) return;
#if NRFMAST_PRODUCTION_LUA
    if (luaAdminCommand(body, output)) return;
#endif
    if (runtimeConfig && runtimeConfig->handleCommand(senderTimestamp, body, output)) return;
    MyMesh::handleCommand(senderTimestamp, text, reply);
#if NRFMAST_PRODUCTION_LUA
    if ((!strcmp(body, "clock sync") || !strncmp(body, "time ", 5)) &&
        !strncmp(output, "OK - clock set:", 15))
      nrfmast::trustLuaTime(rtc_clock.getCurrentTime());
#endif
  }
};

static ArduinoMillis clockMillis;
static StdRNG rng;
static nrfmast::SharedRadio sharedRadio(radio_driver, clockMillis, NRFMAST_RF_ENABLED != 0);
static SimpleMeshTables repeaterTables, botTables;
static StaticPoolPacketManager botPackets(8);
static Repeater repeater(board, sharedRadio.repeater, clockMillis, rng, rtc_clock, repeaterTables);
static nrfmast::CommandBot bot(sharedRadio.bot, clockMillis, rng, rtc_clock, botPackets, botTables,
                              NRFMAST_RF_ENABLED != 0);
static nrfmast::RuntimeConfig runtimeConfig(InternalFS, repeater.self_id, bot);
static nrfmast::PineQspiFlash noteFlash;
#if NRFMAST_PRODUCTION_LUA
static nrfmast::NoteJournal* noteJournal;
static nrfmast::NoteStore* notes;
static bool originalNotesReady, originalVolumeRetired;
static unsigned originalNoteCount, originalPrincipalCount, originalJournalSlots;
static bool loadOriginalNotes() {
  if (notes) return notes->ready();
  noteJournal = new (std::nothrow) nrfmast::NoteJournal(noteFlash);
  if (noteJournal) notes = new (std::nothrow) nrfmast::NoteStore(InternalFS, clockMillis, noteJournal);
  if (!notes) { delete noteJournal; noteJournal = nullptr; return false; }
  originalNotesReady = notes->begin();
  originalVolumeRetired = notes->externalVolumeRetired();
  originalNoteCount = notes->countNotes(); originalPrincipalCount = notes->countPrincipals();
  originalJournalSlots = noteJournal->usedSlots();
  return originalNotesReady;
}
static void releaseOriginalNotes() {
  bot.clearNotes();
  delete notes; notes = nullptr;
  delete noteJournal; noteJournal = nullptr;
}
#else
static nrfmast::NoteJournal noteJournalStorage(noteFlash);
static nrfmast::NoteStore notesStorage(InternalFS, clockMillis, &noteJournalStorage);
static auto* noteJournal = &noteJournalStorage;
static auto* notes = &notesStorage;
#endif
static SerialBLEInterface ble;
static bool bleStarted;
static uint32_t activeBlePin;
static void radioInfo(nrfmast::CompanionRadioInfo& info) {
  info = repeater.activeProfile;
  info.power = repeater.getNodePrefs()->tx_power_dbm;
  info.maxPower = 22;  // SX1262 output limit; BLE cannot change the shared TX setting.
  info.airtimeFactor = sharedRadio.getAirtimeFactor();
  info.lastRssi = sharedRadio.bot.getLastRSSI();
  info.lastSnr = sharedRadio.bot.getLastSNR();
  info.physicalReceived = radio_driver.getPacketsRecv();
  info.physicalSent = radio_driver.getPacketsSent();
  info.physicalErrors = radio_driver.getPacketsRecvErrors();
  info.noiseFloor = radio_driver.getNoiseFloor();
}
static uint16_t batteryRead() { return board.getBattMilliVolts(); }
static int countBlock(void* count, lfs_block_t) {
  ++*static_cast<uint32_t*>(count);
  return 0;
}
static void storageRead(uint32_t& used, uint32_t& total) {
  const auto* filesystem = InternalFS._getFS();
  uint32_t count = 0;
  const int result = lfs_traverse(InternalFS._getFS(), countBlock, &count);
  used = result < 0 ? 0 : (count * filesystem->cfg->block_size) / 1024;
  total = (filesystem->cfg->block_count * filesystem->cfg->block_size) / 1024;
}
static nrfmast::CompanionInterface companion(bot, runtimeConfig, ble, radioInfo, batteryRead, storageRead);
static int heapLow = INT32_MAX;
static char command[160];
static size_t commandLength;
static bool commandOverflow;
namespace nrfmast {
extern FieldUpdateWindow fieldWindow;
static bool updatePreparing, cancelUpdate;
static uint32_t cancelAt;
bool fieldUpdatePending() { return updatePreparing || cancelUpdate || fieldWindow.active(millis()); }
bool fieldUpdateCommand(uint32_t, const char* text, char* reply) {
  if (!strcmp(text, "get update") || !strcmp(text, "help ota")) {
    snprintf(reply, 160, "BLE-DFU=%u legacy app-only arm-ms=120000 boot=0x%06lx boot-timeout=none; start ota confirm; stop ota",
             unsigned(NRFMAST_BLE_DFU), (unsigned long)bootloaderVersion); return true;
  }
  if (!strncmp(text, "start ota", 9)) {
#if NRFMAST_BLE_DFU
    if (strcmp(text, "start ota confirm")) {
      strcpy(reply, "WARNING: app-only BLE DFU has no rollback/bootloader timeout; start ota confirm"); return true;
    }
    if (!runtimeConfig.ble.getPin()) {
      strcpy(reply, "Error: configure BLE PIN locally with set bot.ble.pin before arming"); return true;
    }
    if (bleStarted && activeBlePin != runtimeConfig.ble.getPin()) {
      strcpy(reply, "Error: saved BLE PIN differs from active PIN; reboot before arming DFU"); return true;
    }
    if (fieldUpdatePending()) { strcpy(reply, "Error: BLE DFU already armed; deadline not extended"); return true; }
    updatePreparing = true;
    strcpy(reply, "OK: BLE DFU preparing; pair with saved PIN within 120s; bot paused; abandoned arm restarts Pine");
#else
    strcpy(reply, "Error: this image declares BLE DFU disabled");
#endif
    return true;
  }
  if (!strncmp(text, "stop ota", 8)) {
    if (strcmp(text, "stop ota")) strcpy(reply, "Error: use stop ota");
    else {
      cancelUpdate = fieldUpdatePending();
      if (cancelUpdate) {
        fieldWindow.revoke(); updatePreparing = false; cancelAt = millis() + 3000;
      }
      strcpy(reply, cancelUpdate ? "OK: BLE DFU cancelled; Pine restarts" : "OK: BLE DFU not armed");
    }
    return true;
  }
  return false;
}
void fieldUpdateLoop() {
  if ((cancelUpdate && int32_t(millis() - cancelAt) >= 0) || fieldWindow.expired(millis())) {
    ble.disconnect();
    NVIC_SystemReset();
  }
  if (!updatePreparing || cancelUpdate) return;
#if NRFMAST_PRODUCTION_LUA
  onchip::MastAdmin::service()->stop();
  onchip::commandBotService().stop();
#endif
  if (!bleStarted) {
    char name[32]; strcpy(name, bot.getName());
    ble.begin("MeshCore-", name, runtimeConfig.ble.getPin());
    activeBlePin = runtimeConfig.ble.getPin();
    Bluefruit.Periph.clearBonds();
    ble.enable(); bleStarted = true;
  }
  fieldWindow.arm(millis(), NRFMAST_BLE_DFU != 0, runtimeConfig.ble.getPin() != 0, true);
  updatePreparing = false;
  Serial.println("BLE DFU: armed 120s; saved PIN required; bootloader transfer has no deadline/rollback");
}
}
#if NRFMAST_PRODUCTION_LUA
static bool luaStarted = false;
static constexpr char LuaMarker[] = "Pine Lua filesystem v1\n";
namespace nrfmast {
bool saveLuaCarrierName(const char* name) {
  if (!runtimeConfig.setBotName(name)) return false;
  if (bleStarted) {
    char bleName[48];
    snprintf(bleName, sizeof(bleName), "MeshCore-%s", name);
    Bluefruit.setName(bleName);
  }
  return true;
}
}
static bool preserveLuaBotName() {
  nvs_handle_t handle;
  auto status = nvs_open("mc-onchip", NVS_READONLY, &handle);
  if (status == ESP_OK) {
    size_t size = 0;
    status = nvs_get_blob(handle, "bot-mesh", nullptr, &size);
    nvs_close(handle);
    if (status == ESP_OK) return true;
  }
  if (status != ESP_ERR_NVS_NOT_FOUND) return false;
  onchip::BotMeshPolicy policy;
  snprintf(policy.name, sizeof(policy.name), "%s", bot.getName());
  return onchip::saveBotMeshPolicy(policy);
}
static bool prepareLuaFilesystem() {
  if (!notes || !notes->ready() || !notes->externalVolumeRetired()) {
    Serial.println("Lua storage: native notes must be ready and the old external volume retired; use bot notes provision");
    return false;
  }
  noteFlash.authorizeFilesystem(true);
  const bool present = InternalFS.exists("/pine-lua-v1");
  if (present) {
    char marker[sizeof(LuaMarker)]{};
    auto file = InternalFS.open("/pine-lua-v1");
    if (!file || file.size() != sizeof(LuaMarker) - 1 ||
        file.read(marker, sizeof(LuaMarker) - 1) != sizeof(LuaMarker) - 1 ||
        strcmp(marker, LuaMarker)) {
      Serial.println("Lua storage: ownership marker corrupt; filesystem was not formatted");
      return false;
    }
  } else Serial.println("Lua storage: initializing retired QSPI 0x000000..0x180000; original notes/identities preserved");
  if (!nrfmast::botFilesystem.begin(noteFlash, !present)) {
    Serial.println("Lua storage: mount failed; existing files retained; use bot lua provision for recovery warning");
    return false;
  }
  if (!present) {
    auto file = InternalFS.open("/pine-lua-v1", FILE_O_WRITE);
    if (!file || file.write(reinterpret_cast<const uint8_t*>(LuaMarker), sizeof(LuaMarker) - 1) != sizeof(LuaMarker) - 1)
      return false;
    file.flush(); file.close();
    auto check = InternalFS.open("/pine-lua-v1");
    char marker[sizeof(LuaMarker)]{};
    if (!check || check.size() != sizeof(LuaMarker) - 1 ||
        check.read(marker, sizeof(LuaMarker) - 1) != sizeof(LuaMarker) - 1 || strcmp(marker, LuaMarker)) return false;
  }
  char error[128]{};
  if (!nrfmast::migrateLuaNotes(*notes, error, sizeof(error))) {
    Serial.printf("Lua notes: %s; original notes retained; native repeater administration remains available\n", error);
    return false;
  }
  return true;
}
static bool luaAdminCommand(const char* text, char* reply) {
  char nameCommand[48];
  if (!strncmp(text, "set bot.name ", 13)) {
    if (strlen(text + 13) > 31) {
      strcpy(reply, "Error: bot name requires 1-31 printable ASCII bytes without edge spaces"); return true;
    }
    snprintf(nameCommand, sizeof(nameCommand), "bot name %s", text + 13);
    text = nameCommand;
  }
  if (!strncmp(text, "bot key", 7) || !strncmp(text, "bot ble", 7)) return false;
  if (!strcmp(text, "bot notes provision") || !strcmp(text, nrfmast::NOTE_PROVISION_CONFIRM)) {
    if (!strcmp(text, "bot notes provision")) {
      snprintf(reply, 160, "WARNING: bot notes provision retire-external migrate-internal confirm retires old QSPI files; preserves identities/current notes");
    } else {
      if (!notes && originalNotesReady && originalVolumeRetired) {
        strcpy(reply, "Original notes already provisioned; Lua uses its separate partition; identities retained");
      } else {
        snprintf(reply, 160, "%s", notes && notes->provisionRetiredVolume() ?
            "Original notes/replay migrated; identities retained; use bot lua provision" :
            notes ? notes->error() : "Error: original notes allocation unavailable");
        if (notes) {
          originalNotesReady = notes->ready();
          originalVolumeRetired = notes->externalVolumeRetired();
        }
      }
    }
    return true;
  }
  if (!strcmp(text, "bot lua provision erase-lua confirm")) {
    if (!originalNotesReady || !originalVolumeRetired) {
      strcpy(reply, "Error: provision original notes first with bot notes provision"); return true;
    }
    onchip::MastAdmin::service()->stop();
    onchip::commandBotService().stop();
    luaStarted = false;
    if (!loadOriginalNotes()) {
      strcpy(reply, "Error: original notes reload failed; Lua files and identities retained"); return true;
    }
    noteFlash.authorizeFilesystem(true);
    if (!nrfmast::botFilesystem.end() ||
        (InternalFS.exists("/pine-lua-v1") && !InternalFS.remove("/pine-lua-v1"))) {
      strcpy(reply, "Error: Lua filesystem recovery failed; original notes/identities retained"); return true;
    }
    if (!prepareLuaFilesystem()) {
      strcpy(reply, "Error: Lua notes migration/marker failed; original notes/identities retained"); return true;
    }
    releaseOriginalNotes();
    strcpy(reply, "Lua source/KV/timers/grants erased; original notes restored; identities retained; reboot to start Lua");
    return true;
  }
  if (strcmp(text, "bot lua provision") == 0) {
    strcpy(reply, "WARNING: bot lua provision erase-lua confirm erases Lua source/KV/timers/grants only; identities and original note journal retained");
    return true;
  }
  if (!strncmp(text, "bot ", 4) || !strncmp(text, "source ", 7) ||
      !strcmp(text, "bot") || !strcmp(text, "source") ||
      !strcmp(text, "help bot") || !strcmp(text, "help source")) {
    onchip::MastAdmin::Reply result;
    onchip::MastAdmin::service()->execute(text, result, 0, onchip::MastAdmin::Transport::NativeEncrypted);
    snprintf(reply, 160, "%s", result.text);
    return true;
  }
  return false;
}
#endif

[[noreturn]] static void halt(const char* reason) {
  Serial.println(reason);
  while (true) delay(1000);
}

static void loadIdentity(IdentityStore& store, const char* name, mesh::LocalIdentity& identity) {
  if (store.load(name, identity)) return;
  // Do not silently replace a corrupt existing identity.
  char path[40];
  snprintf(path, sizeof(path), "/%s.id", name);
  if (InternalFS.exists(path)) halt("identity: existing record cannot be read; restore it before booting");
#if NRFMAST_REQUIRE_IDENTITIES
  halt("identity: required Pine record is missing; restore the existing key; no replacement generated");
#endif
  identity = radio_new_identity();
  while (identity.pub_key[0] == 0 || identity.pub_key[0] == 0xFF) identity = radio_new_identity();
  if (!store.save(name, identity)) halt("identity: save failed; firmware halted");
  mesh::LocalIdentity saved;
  if (!store.load(name, saved) || !saved.matches(identity)) halt("identity: readback failed; firmware halted");
}

static uint64_t statsUptime() {
  static uint32_t previous = 0;
  static uint64_t total = 0;
  const uint32_t current = millis();
  total += uint32_t(current - previous);
  previous = current;
  return total;
}
struct StatsFormatter {
  char* reply;
  template<typename... Args> void operator()(const char* pattern, Args... values) const {
    const int size = snprintf(reply, 160, pattern, values...);
    if (size < 0 || size > 130) strcpy(reply, "Error: stats response exceeds encrypted CLI capacity");
  }
};
static bool statsCommand(const char* command, char* reply) {
  if (strcmp(command, "stats") && strcmp(command, "get stats") &&
      strcmp(command, "help stats") && strncmp(command, "stats ", 6)) return false;
  const char* topic = !strcmp(command, "help stats") ? "help" :
      !strncmp(command, "stats ", 6) ? command + 6 : "radio";
  const StatsFormatter format{reply};
  if (!strcmp(topic, "help")) {
    strcpy(reply, "stats [radio|signal|tx|airtime|admission|sensors|memory|bot|vm]; key=value schema=1; units in keys; counters since boot");
  } else if (!strcmp(topic, "sensors")) {
    char battery[12] = "unavailable", temperature[24] = "unavailable";
    const auto mv = board.getBattMilliVolts();
    const auto celsius = board.getMCUTemperature();
    if (mv) snprintf(battery, sizeof(battery), "%u", unsigned(mv));
    if (std::isfinite(celsius)) {
      const int size = snprintf(temperature, sizeof(temperature), "%.2f", double(celsius));
      if (size < 0 || size_t(size) >= sizeof(temperature)) {
        strcpy(reply, "Error: MCU temperature exceeds stats representation"); return true;
      }
    }
    format("schema=1 scope=device battery_mv=%s mcu_temp_c=%s", battery, temperature);
  } else if (!strcmp(topic, "memory")) {
    const int free = dbgHeapFree();
    if (free < heapLow) heapLow = free;
    format("schema=1 scope=device heap_free_bytes=%d heap_total_bytes=%d heap_sampled_min_bytes=%d",
           free, dbgHeapTotal(), heapLow);
  } else if (!strcmp(topic, "radio")) {
    unsigned queued = sharedRadio.queuedPackets();
#if NRFMAST_PRODUCTION_LUA
    if (luaStarted) {
      mesh::QueuedRadioStats stats;
      if (!onchip::commandBotService().radioStatistics(stats)) {
        strcpy(reply, "Error: Lua/shared-radio queue snapshot unavailable"); return true;
      }
      queued = stats.aggregate_queued;
    }
#endif
    format("schema=1 scope=modem rx_packets=%lu rx_errors=%lu queued=%u transmitting=%u",
           (unsigned long)radio_driver.getPacketsRecv(), (unsigned long)radio_driver.getPacketsRecvErrors(),
           queued, unsigned(!sharedRadio.idle()));
  } else if (!strcmp(topic, "signal")) {
    if (!radio_driver.getPacketsRecv()) {
      strcpy(reply, "Error: RF packet signal unavailable before first reception"); return true;
    }
    char noise[16] = "unavailable";
    if (radio_driver.getNoiseFloor()) snprintf(noise, sizeof(noise), "%d", radio_driver.getNoiseFloor());
    format("schema=1 scope=modem last_rssi_dbm=%.2f last_snr_db=%.2f noise_floor_dbm=%s",
           double(radio_driver.getLastRSSI()), double(radio_driver.getLastSNR()), noise);
  } else if (!strcmp(topic, "tx")) {
    format("schema=1 scope=modem tx_confirmed=%lu tx_unconfirmed=%lu",
           (unsigned long)sharedRadio.txSucceeded, (unsigned long)sharedRadio.txFailed);
  } else if (!strcmp(topic, "airtime")) {
    format("schema=1 scope=modem tx_rf_ms=%lu rx_estimated_ms=%lu",
           (unsigned long)sharedRadio.aggregateRfMs(), (unsigned long)sharedRadio.receivedAirtimeMs());
  } else if (!strcmp(topic, "admission")) {
    format("schema=1 scope=modem uptime_ms=%" PRIu64 " tx_started=%lu tx_rejected=%lu",
           statsUptime(), (unsigned long)sharedRadio.txStarted, (unsigned long)sharedRadio.txRejected);
#if NRFMAST_PRODUCTION_LUA
  } else if (!strcmp(topic, "bot") || !strcmp(topic, "vm")) {
    auto &service = onchip::commandBotService();
    if (!luaStarted) { strcpy(reply, "Error: command bot is not running"); return true; }
    const auto &s = service.counters();
    if (!strcmp(topic, "bot"))
      format("schema=1 scope=bot jobs=%u replies=%lu rejected=%lu vm_failures=%lu",
             service.jobsInUse(), (unsigned long)s.replies, (unsigned long)s.rejected, (unsigned long)s.vmFailures);
    else if (s.lastVm.peakBytes)
      format("schema=1 scope=last_vm peak_bytes=%u instructions=%lu elapsed_us=%" PRIu64 " stack_free_bytes=%lu",
             unsigned(s.lastVm.peakBytes), (unsigned long)s.lastVm.instructions, s.lastVm.elapsedUs,
             (unsigned long)s.lastVm.stackHighWaterBytes);
    else strcpy(reply, "Error: Lua execution measurements unavailable before first execution");
#endif
  } else strcpy(reply, "Error: unknown stats topic; use stats help");
  return true;
}
static void reportMemory() {
  int free = dbgHeapFree();
  if (free < heapLow) heapLow = free;
  Serial.printf("mem heap_total=%d heap_used=%d heap_free=%d sampled_low=%d stack_free=%lu rf=%u tx=%lu rejected=%lu rx_drop=%lu/%lu\n",
                dbgHeapTotal(), dbgHeapUsed(), free, heapLow,
                static_cast<unsigned long>(uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t)),
                NRFMAST_RF_ENABLED,
                static_cast<unsigned long>(sharedRadio.txStarted),
                static_cast<unsigned long>(sharedRadio.txRejected),
                static_cast<unsigned long>(sharedRadio.repeater.rxDrops),
                static_cast<unsigned long>(sharedRadio.bot.rxDrops));
}

static void runCommand() {
  char updateReply[160]{};
  if (nrfmast::fieldUpdateCommand(0, command, updateReply)) {
    Serial.println(updateReply); return;
  }
  if (nrfmast::fieldUpdatePending()) {
    Serial.println("Error: BLE firmware update armed; use stop ota or wait for restart"); return;
  }
  if (strcmp(command, "mem") == 0) {
    reportMemory();
  } else if (strcmp(command, "bot state") == 0) {
    uint32_t used, total;
    storageRead(used, total);
#if NRFMAST_PRODUCTION_LUA
    Serial.printf("bot state notes=%s notes_used=%u principal_used=%u notes_slots=%u notes_guard=%s BLE_active=%u inbox_drops=%lu\n",
                  originalNotesReady ? "migrated-original" : "unavailable", originalNoteCount,
                  originalPrincipalCount, originalJournalSlots, noteFlash.partitionStatus().label(),
                  bleStarted, (unsigned long)companion.inboxDrops);
    Serial.printf("Lua ready=%u storage_used_kb=%lu storage_total_kb=%lu; bot status|limits|memory|data\n",
                  onchip::commandBotService().sourceReady(), (unsigned long)used, (unsigned long)total);
#else
    nrfmast::printNoteState(Serial, *notes, used, total, bleStarted, companion.inboxDrops,
                          noteJournal->usedSlots(), noteFlash.jedecId(), noteFlash.partitionStatus());
#endif
  } else if (strcmp(command, "bot notes reset confirm") == 0) {
#if NRFMAST_PRODUCTION_LUA
    Serial.println("Error: Lua notes live in KV; use !forget or bot data; original note journal retained");
#else
    if (notes->reset())
      Serial.println("bot: all personal notes and replay watermarks erased; identities unchanged");
    else Serial.printf("bot: %s; notes disabled; identities unchanged\n", notes->error());
#endif
  } else if (strcmp(command, "bot notes provision") == 0 ||
             strcmp(command, nrfmast::NOTE_PROVISION_CONFIRM) == 0) {
#if NRFMAST_PRODUCTION_LUA
    char reply[160]{};
    luaAdminCommand(command, reply);
    Serial.println(reply);
#else
    nrfmast::printNoteProvisionWarning(Serial);
    if (strcmp(command, nrfmast::NOTE_PROVISION_CONFIRM) == 0) {
      if (notes->provisionRetiredVolume())
        Serial.println("bot: external volume retired; notes/replay migrated and ready; current identities/configuration unchanged");
      else Serial.printf("bot: %s; provisioning not acknowledged; inspect bot state before retry\n", notes->error());
    } else Serial.printf("bot: notes=%s; %s\n", notes->ready() ? "ready" : "unavailable",
                         notes->ready() ? "current notes unchanged" : notes->error());
#endif
  } else if (strcmp(command, "bot notes volume") == 0) {
    if (!notes || notes->externalVolumeRetired()) {
      Serial.println("notes volume: old external filesystem retired; legacy inventory disabled; use bot state");
    } else if (noteFlash.jedecId() != 0x856015) {
      Serial.println("notes volume: expected QSPI chip unavailable; no filesystem access");
    } else {
      nrfmast::NoteVolumeStatus status;
      nrfmast::inspectNoteVolume(noteFlash, [](void*, const nrfmast::NoteVolumeEntry& entry) {
        nrfmast::printNoteVolumeEntry(Serial, entry);
      }, nullptr, status);
      nrfmast::printNoteVolumeStatus(Serial, status);
    }
  } else if (strcmp(command, "ids") == 0) {
    char repeaterKey[PUB_KEY_SIZE * 2 + 1], botKey[PUB_KEY_SIZE * 2 + 1];
    mesh::Utils::toHex(repeaterKey, repeater.self_id.pub_key, PUB_KEY_SIZE);
    mesh::Utils::toHex(botKey, bot.self_id.pub_key, PUB_KEY_SIZE);
    Serial.printf("ids repeater=%s bot=%s\n", repeaterKey, botKey);
  } else if (strcmp(command, "bot advert") == 0) {
#if NRFMAST_PRODUCTION_LUA
    char reply[160]{};
    luaAdminCommand(command, reply); Serial.println(reply);
#else
    bot.advertise();
#endif
  } else {
    char reply[160] = {};
    repeater.handleCommand(0, command, reply);
    if (reply[0]) Serial.println(reply);
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  board.begin();
  uint32_t radioBackoff = 1000;
  while (!radio_init()) {
    Serial.printf("SX1262 initialization failed; retry in %lums; saved state unchanged\n", (unsigned long)radioBackoff);
    delay(radioBackoff);
    radioBackoff = radioBackoff < 15000 ? radioBackoff * 2 : 30000;
  }
  rng.begin(radio_driver.getRngSeed());
  if (!InternalFS.Adafruit_LittleFS::begin())
    halt("InternalFS mount failed; storage was not formatted; restore the existing Pine state");
  IdentityStore store(InternalFS, "");
  loadIdentity(store, "_main", repeater.self_id);
  loadIdentity(store, "_nrfbot", bot.self_id);
#if !NRFMAST_PRODUCTION_LUA
  bot.bindAdaptiveRadio(sharedRadio);
#endif
  sharedRadio.observeQueues(repeater.packetManager(), botPackets);
  if (!runtimeConfig.begin()) halt("bot: saved runtime configuration cannot be read; firmware halted");
  repeater.runtimeConfig = &runtimeConfig;
  if (repeater.self_id.matches(bot.self_id))
    Serial.println("warning: bot and repeater have the same identity; replace the bot record for separate addressing");
  sensors.begin();
#if ENV_INCLUDE_GPS
  meshcore::gpsTimeHandler() = [](uint32_t utc) {
#if NRFMAST_PRODUCTION_LUA
    nrfmast::trustLuaTime(utc, nrfmast::LuaTimeSource::Gps);
#endif
  };
#endif
  repeater.begin(&InternalFS);
#if ENV_INCLUDE_GPS
  repeater.getNodePrefs()->gps_enabled = 1;
  sensors.setSettingValue("gps", "1");
#endif
  // The upstream login treats an empty admin password as a valid credential.
  // Replace the initial empty default before processing any RF input.
  auto* prefs = repeater.getNodePrefs();
  if (!prefs->password[0]) {
    mesh::LocalIdentity entropy = radio_new_identity();
    char randomHex[PUB_KEY_SIZE * 2 + 1];
    mesh::Utils::toHex(randomHex, entropy.pub_key, PUB_KEY_SIZE);
    memcpy(prefs->password, randomHex, 15);
    prefs->password[15] = 0;
    repeater.savePrefs();
    NodePrefs saved;
    File file = InternalFS.open("/prefs.json");
    if (!file || !saved.loadSerial(file) || strcmp(saved.password, prefs->password) != 0)
      halt("admin: initial password save/readback failed; firmware halted");
    file.close();
    Serial.println("admin: use password or setperm through the native serial CLI before remote use");
  }
  bot.begin(NRFMAST_RF_ENABLED != 0 && !NRFMAST_PRODUCTION_LUA);
#if NRFMAST_PRODUCTION_LUA
  const bool notesReady = loadOriginalNotes();
#else
  const bool notesReady = notes->begin();
#endif
  if (!notesReady) Serial.printf("bot: %s; notes disabled; RF roles continue\n",
                                notes ? notes->error() : "original notes allocation unavailable");
  Serial.printf("notes QSPI JEDEC=%06lx region=0x180000..0x200000 ready=%u\n",
                static_cast<unsigned long>(noteFlash.jedecId()), notesReady);
  if (notes) bot.setNotes(*notes);
  if (runtimeConfig.ble.isEnabled()) {
    char name[32];
    strcpy(name, bot.getName());
    ble.begin("MeshCore-", name, runtimeConfig.ble.getPin());
    activeBlePin = runtimeConfig.ble.getPin();
    // A saved PIN change must not leave previously bonded clients authorized.
    Bluefruit.Periph.clearBonds();
    ble.enable();
    bot.setCompanion(companion);
    bleStarted = true;
    Serial.println("BLE: bot identity companion enabled; pair with configured PIN; shared radio read-only");
  }
  if (!sharedRadio.setAirtimeFactor(prefs->airtime_factor))
    Serial.println("radio: invalid saved airtime factor; retaining 1.0 until corrected");
#if NRFMAST_PRODUCTION_LUA
  if (prepareLuaFilesystem() && preserveLuaBotName()) {
    releaseOriginalNotes();
    nrfmast::setLuaIdentity(bot.self_id);
    const auto &profile = repeater.activeProfile;
    const RadioConfig configuration{uint32_t(profile.frequency * 1000000), uint32_t(profile.bandwidth * 1000),
                                    profile.sf, profile.cr, uint8_t(prefs->tx_power_dbm)};
    luaStarted = onchip::commandBotService().begin(sharedRadio.lua, configuration);
    if (!onchip::MastAdmin::service()->begin())
      Serial.println("Lua administration: source/owner journal requires recovery; native repeater administration remains available");
  } else Serial.println("Lua startup: original notes or saved Lua name unavailable; inspect bot state and Lua filesystem; native repeater remains available");
#endif
  Serial.printf("nrfmast: RF %s; native repeater + separate command bot\n",
                NRFMAST_RF_ENABLED ? "enabled" : "DISABLED");
  reportMemory();
#if NRFMAST_LUA_PROBE
  extern void pineLuaProbeLink();
  pineLuaProbeLink();
#endif
  board.onBootComplete();
}

void loop() {
#if NRFMAST_PRODUCTION_LUA
  nrfmast::pollLuaTime();
#endif
  nrfmast::fieldUpdateLoop();
  statsUptime();
  static bool badAirtime;
  bool valid = sharedRadio.setAirtimeFactor(repeater.getNodePrefs()->airtime_factor);
  if (!valid && !badAirtime) Serial.println("radio: invalid airtime factor; retaining last valid value");
  badAirtime = !valid;
  sharedRadio.poll();
#if NRFMAST_PRODUCTION_LUA
  if (luaStarted && !nrfmast::fieldUpdatePending()) {
    const auto &profile = repeater.activeProfile;
    static RadioConfig current{};
    const RadioConfig configuration{uint32_t(profile.frequency * 1000000), uint32_t(profile.bandwidth * 1000),
                                    profile.sf, profile.cr, uint8_t(repeater.getNodePrefs()->tx_power_dbm)};
    if (current.freq_hz != configuration.freq_hz || current.bw_hz != configuration.bw_hz ||
        current.sf != configuration.sf || current.cr != configuration.cr || current.tx_power != configuration.tx_power) {
      current = configuration;
      onchip::commandBotService().configureRadio(current);
    }
    const char* name = onchip::commandBotService().botName();
    if (name && strcmp(name, bot.getName()) && bot.setName(name) && bleStarted) {
      char bleName[48];
      snprintf(bleName, sizeof(bleName), "MeshCore-%s", name);
      Bluefruit.setName(bleName);
    }
    onchip::commandBotService().loop();
  }
  if (!nrfmast::fieldUpdatePending()) onchip::MastAdmin::service()->loop();
#endif
  static bool botFirst;
  if (!nrfmast::fieldUpdatePending() && botFirst && sharedRadio.botCanLoop()) bot.loop();
  if (sharedRadio.repeaterCanLoop()) repeater.loop();
  if (!nrfmast::fieldUpdatePending() && !botFirst && sharedRadio.botCanLoop()) bot.loop();
  botFirst = !botFirst;
  if (bleStarted && sharedRadio.idle() && !nrfmast::fieldUpdatePending()) companion.loop();
  sensors.loop();
  rtc_clock.tick();
  int free = dbgHeapFree();
  if (free < heapLow) heapLow = free;
  while (sharedRadio.idle() && Serial.available()) {
    int c = Serial.read();
    if (c == '\r' || c == '\n') {
      if (commandOverflow) Serial.println("CLI: command exceeds 159 bytes");
      else if (commandLength) runCommand();
      commandLength = 0;
      commandOverflow = false;
      memset(command, 0, sizeof(command));
    } else if (!commandOverflow) {
      if (commandLength == sizeof(command) - 1) commandOverflow = true;
      else { command[commandLength++] = c; command[commandLength] = 0; }
    }
  }
}
