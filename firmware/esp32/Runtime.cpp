// SPDX-License-Identifier: Apache-2.0
#include "Runtime.h"
#if MESHCORE_NODE_BACKUP
#include "NodeBackup.h"
#include "EspNodeBackup.h"
#endif
#include "Capacity.h"
#include "CompanionSessions.h"
#include "Config.h"
#include "Observer.h"
#include "Management.h"
#include "ServiceName.h"
#include "Syslog.h"
#if defined(ARDUINO_ARCH_ESP32) && MESHCORE_ONCHIP_BOT
#include "EspPacketPrograms.h"
#endif
#if defined(MESHCORE_CLOUD_ROOM) && MESHCORE_CLOUD_ROOM
#include "CloudRoomService.h"
#endif
#if defined(MESHCORE_ONCHIP_BOT) && MESHCORE_ONCHIP_BOT
#include "CommandBot.h"
#endif
#include <atomic>
#include <algorithm>
#include <freertos/queue.h>
#include <freertos/task.h>
#ifdef ARDUINO_ARCH_ESP32
#include <esp_heap_caps.h>
#include <esp_system.h>
#endif

namespace onchip {
static_assert(CompanionSessions::MaxClients == ONCHIP_COMPANION_MAX_CLIENTS,
              "Companion server and combined capacity profile disagree");
struct Diagnostic {
  char message[160];
  bool companion;
  bool remote;
};
#if defined(MESHCORE_ONCHIP_BOT) && MESHCORE_ONCHIP_BOT
static_assert(sizeof(Diagnostic::message) >= CommandBot::DiagnosticCapacity,
              "Command diagnostic must fit without truncation");
#endif
static_assert(sizeof(Diagnostic) == 162, "Diagnostics queue record budget changed");
static std::atomic<QueueHandle_t> diagnosticQueue{nullptr};
static std::atomic<uint32_t> droppedDiagnostics{0};
static std::atomic<uint32_t> usbDropped{0}, completedDiagnostics{0};
#ifdef ARDUINO_ARCH_ESP32
static StaticQueue_t diagnosticQueueControl;
static uint8_t *diagnosticQueueStorage;
static TaskHandle_t diagnosticTask;
static uint32_t loopPeakUs = 0, loopGapPeakUs = 0, previousLoopStart = 0;
#endif
static Observer observer;
static Management management;
#if defined(MESHCORE_ONCHIP_BOT) && MESHCORE_ONCHIP_BOT
static CommandBot commandBot;
#ifdef BOT_HOST_RUNNER
static CommandBot *replayCommandBot;
bool bindCommandBotForReplay(CommandBot *bot) {
  if (bot && replayCommandBot && replayCommandBot != bot) return false;
  replayCommandBot = bot;
  return true;
}
CommandBot &commandBotService() { return replayCommandBot ? *replayCommandBot : commandBot; }
#else
CommandBot &commandBotService() { return commandBot; }
#endif
#endif
static RadioDashboard::RoleStatus botStatus;
static RoleProfile bootProfile;
static WifiKissMultiplexer *roleMux;
static bool observerActive;
bool packetSystemSnapshot(packet_engine::SystemInfo &info) {
  info = {};
  info.uptimeMs = millis();
  uint32_t lower = 0, upper = 0;
  if (trustedNetworkTime(lower, upper)) {
    info.unixTime = lower; info.flags |= packet_engine::TrustedTime;
  }
#ifdef ARDUINO_ARCH_ESP32
  info.freeInternalBytes = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  info.freePsramBytes = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  info.flags |= packet_engine::HeapMeasured;
#endif
  info.enabledRoles = bootProfile.enabled;
  for (unsigned i = 0; i < 3; ++i) {
    RadioDashboard::RoleStatus status;
    roleStatus(static_cast<Role>(i), status);
    if (status.ready && rolePhase(static_cast<Role>(i)) == RolePhase::Running)
      info.readyRoles |= 1u << i;
  }
  if (observerActive) {
    RadioDashboard::RoleStatus status;
    observer.dashboardStatus(status);
    if (status.ready) info.readyRoles |= RoleProfile::Observer;
  }
  return roleMux != nullptr;
}
packet_engine::Fault repeaterComposePacket(const packet_engine::ComposeRequest &,
    const uint8_t *, uint16_t, uint8_t *, uint16_t &);
packet_engine::Fault roomComposePacket(const packet_engine::ComposeRequest &,
    const uint8_t *, uint16_t, uint8_t *, uint16_t &);
packet_engine::Fault companionComposePacket(const packet_engine::ComposeRequest &,
    const uint8_t *, uint16_t, uint8_t *, uint16_t &);
packet_engine::Fault nativeRoleComposePacket(Role role, const packet_engine::ComposeRequest &request,
    const uint8_t *data, uint16_t length, uint8_t *output, uint16_t &capacity) {
  using packet_engine::Fault;
  using Composer = Fault (*)(const packet_engine::ComposeRequest &, const uint8_t *,
                            uint16_t, uint8_t *, uint16_t &);
  const Composer compose[] = {repeaterComposePacket, roomComposePacket, companionComposePacket};
  if (unsigned(role) >= 3 || lifecycleBusy(role) || rolePhase(role) != RolePhase::Running)
    return Fault::Unavailable;
  RadioDashboard::RoleStatus status;
  roleStatus(role, status);
  if (!status.ready || !status.has_identity || memcmp(status.public_key, request.identity, 32))
    return Fault::Unavailable;
  return compose[unsigned(role)](request, data, length, output, capacity);
}
packet_engine::Fault packetComposeOwned(const packet_engine::ComposeRequest &request,
    const uint8_t *data, uint16_t length, uint8_t *output, uint16_t &capacity) {
  using packet_engine::Fault;
  if (!roleMux) return Fault::Unavailable;
  const auto *key = management.publicKey();
  if (key && !memcmp(key, request.identity, 32))
    return management.composePacket(request, data, length, output, capacity);
#if defined(MESHCORE_ONCHIP_BOT) && MESHCORE_ONCHIP_BOT
  key = commandBot.publicKey();
  if (key && !memcmp(key, request.identity, 32))
    return commandBot.composePacket(request, data, length, output, capacity);
#endif
  for (unsigned i = 0; i < 3; ++i) {
    const auto role = static_cast<Role>(i);
    RadioDashboard::RoleStatus status;
    roleStatus(role, status);
    if (rolePhase(role) == RolePhase::Running && status.ready && status.has_identity &&
        !memcmp(status.public_key, request.identity, 32))
      return nativeRoleComposePacket(role, request, data, length, output, capacity);
  }
  return Fault::Unavailable;
}
bool sharedRadioReadCommand(const LocalRadio &radio, const char *command, char *reply, size_t capacity) {
  const bool cad = !strcmp(command, "get cad");
  const bool threshold = !strcmp(command, "get int.thresh");
  const bool agc = !strcmp(command, "get agc.reset.interval");
  const bool rxBoost = !strcmp(command, "get rxboost");
  if (!cad && !threshold && !agc && !rxBoost) return false;
  const auto *mux = radio.sharedRadio();
  if (!mux) {
    snprintf(reply, capacity, "Error: shared-radio settings unavailable");
  } else if (cad) {
    snprintf(reply, capacity, "> %s", mux->cadEnabled() ? "on" : "off");
  } else if (threshold) {
    snprintf(reply, capacity, "> %d", mux->interferenceThreshold());
  } else if (agc) {
    snprintf(reply, capacity, "> %u", unsigned(mux->agcResetIntervalSeconds()));
  } else if (!mux->rxBoostAvailable()) {
    snprintf(reply, capacity, "Error: shared-radio RX boost unavailable");
  } else {
    snprintf(reply, capacity, "> %s", mux->rxBoostEnabled() ? "on" : "off");
  }
  return true;
}
void observerStatistics(char *reply, size_t capacity) {
  if (observerActive) observer.statistics(reply, capacity);
  else snprintf(reply, capacity, "Error: observer is not running");
}
bool observerToken(const char *audience, char *output, size_t capacity, char *error, size_t errorCapacity) {
  if (!observerActive) {
    snprintf(error, errorCapacity, "Observer role unavailable; enable it and inspect mqtt status");
    return false;
  }
  return observer.mintToken(audience, output, capacity, error, errorCapacity);
}
bool repeaterName(char name[32]);
bool roomName(char name[32]);
bool companionName(char name[32]);
bool repeaterSetName(const char *name);
bool roomSetName(const char *name);
bool companionSetName(const char *name);
RolePasswordUpdate repeaterSetPassword(const char *password);
RolePasswordUpdate roomSetPassword(const char *password);
bool repeaterAdvertise(bool zeroHop);
bool roomAdvertise(bool zeroHop);
bool companionAdvertise(bool zeroHop);

bool kissName(char name[32]) {
  if (!botStatus.has_identity) return loadServiceName(NamedService::Kiss, name);
  strcpy(name, botStatus.name);
  return true;
}
bool setKissName(const char *name) {
  if (!saveServiceName(NamedService::Kiss, name)) return false;
  strcpy(botStatus.name, name);
  return true;
}
bool nativeRoleAdvertise(Role role, bool zeroHop) {
  if (lifecycleBusy(role)) return false;
  switch (role) {
  case Role::Repeater: return repeaterAdvertise(zeroHop);
  case Role::Room: return roomAdvertise(zeroHop);
  case Role::Companion: return companionAdvertise(zeroHop);
  }
  return false;
}

bool nativeRoleName(Role role, char name[32]) {
  if (lifecycleBusy(role)) return false;
  switch (role) {
  case Role::Repeater: return repeaterName(name);
  case Role::Room: return roomName(name);
  case Role::Companion: return companionName(name);
  }
  return false;
}
bool setNativeRoleName(Role role, const char *name) {
  if (!validRuntimeName(name) || lifecycleBusy(role)) return false;
  switch (role) {
  case Role::Repeater: return repeaterSetName(name);
  case Role::Room: return roomSetName(name);
  case Role::Companion: return companionSetName(name);
  }
  return false;
}
RolePasswordUpdate setNativeRolePassword(Role role, const char *password) {
  if (!password || !password[0] || strlen(password) > 15 || lifecycleBusy(role))
    return RolePasswordUpdate::Unavailable;
  for (const auto *p = reinterpret_cast<const unsigned char *>(password); *p; ++p)
    if (*p < 32 || *p > 126) return RolePasswordUpdate::Unavailable;
  switch (role) {
  case Role::Repeater: return repeaterSetPassword(password);
  case Role::Room: return roomSetPassword(password);
  default: return RolePasswordUpdate::Unavailable;
  }
}

struct NativePresenceKeys {
  static constexpr uint8_t Capacity = 3 + queued_tx::ROLE_COUNT
#if defined(MESHCORE_CLOUD_ROOM) && MESHCORE_CLOUD_ROOM
    + cloudroom::AliasLimit
#endif
    ;
  uint8_t keys[Capacity][queued_tx::ROLE_KEY_SIZE]{};
  uint8_t count = 0;
  bool add(const uint8_t *key) {
    if (count == Capacity) {
      Serial.println("On-chip native identity report capacity exceeded");
      return false;
    }
    memcpy(keys[count++], key, queued_tx::ROLE_KEY_SIZE);
    return true;
  }
};

static bool refreshRolePresence() {
  if (!roleMux) return true;
  NativePresenceKeys keys;
  const uint8_t *managementKey = management.publicKey();
  if (!managementKey || !keys.add(botStatus.public_key) ||
      !keys.add(managementKey))
    return false;
#if defined(MESHCORE_ONCHIP_BOT) && MESHCORE_ONCHIP_BOT
  if (commandBot.publicKey() && !keys.add(commandBot.publicKey())) return false;
#endif
  uint8_t active = 0;
#if defined(MESHCORE_ONCHIP_BOT) && MESHCORE_ONCHIP_BOT
  uint8_t ready = 0;
#endif
  for (uint8_t i = 0; i < 3; ++i) {
    RadioDashboard::RoleStatus status;
    roleStatus(static_cast<Role>(i), status);
    if (rolePhase(static_cast<Role>(i)) != RolePhase::Running ||
        !status.has_identity || status.source_slot < 0)
      continue;
    active |= 1u << i;
#if defined(MESHCORE_ONCHIP_BOT) && MESHCORE_ONCHIP_BOT
    if (status.ready && !(i == 2 && companionSessions().stats().nativeFault))
      ready |= 1u << i;
#endif
    if (!keys.add(status.public_key)) return false;
  }
  if (observerActive) {
    RadioDashboard::RoleStatus status;
    observer.dashboardStatus(status);
    if (status.has_identity) {
      active |= RoleProfile::Observer;
#if defined(MESHCORE_ONCHIP_BOT) && MESHCORE_ONCHIP_BOT
      if (status.ready) ready |= RoleProfile::Observer;
#endif
      if (!keys.add(status.public_key)) return false;
    }
  }
#if defined(MESHCORE_CLOUD_ROOM) && MESHCORE_CLOUD_ROOM
  for (unsigned alias = 0; alias < cloudRoomAliases(); ++alias)
    if (!keys.add(cloudRoomPublicKey(alias))) return false;
#endif
  if (!roleMux->setNativeRolePresence(bootProfile.enabled, &keys.keys[0][0],
                                      keys.count))
    return false;
  roleMux->setActiveRolePresence(active);
#if defined(MESHCORE_ONCHIP_BOT) && MESHCORE_ONCHIP_BOT
  commandBot.setNodeRoles(bootProfile.enabled, ready);
#endif
  return true;
}

static bool queueDiagnostic(const char *message, bool companion, bool remote = false) {
  Diagnostic entry{};
  const int size = message ? snprintf(entry.message, sizeof(entry.message), "%s", message) : -1;
  if (size <= 0 || size_t(size) >= sizeof(entry.message)) {
    ++droppedDiagnostics;
    return false;
  }
  entry.companion = companion;
  entry.remote = remote;
  const auto queue = diagnosticQueue.load();
  if (!queue || xQueueSend(queue, &entry, 0) != pdTRUE) {
    droppedDiagnostics.fetch_add(1);
    return false;
  }
  return true;
}
static void companionDiagnostic(const char *message) {
  queueDiagnostic(message, true, true);
}
bool diagnosticEvent(const char *message) {
#if defined(ARDUINO_ARCH_ESP32) && (!defined(MESHCORE_MAST_ADMIN) || !MESHCORE_MAST_ADMIN)
  if (!diagnosticQueue && message && *message) {
    Serial.println(message);
    return true;
  }
#endif
  return queueDiagnostic(message, false, true);
}
void diagnosticLoopSample(uint32_t started, uint32_t finished) {
#ifdef ARDUINO_ARCH_ESP32
  loopPeakUs = std::max(loopPeakUs, uint32_t(finished - started));
  if (previousLoopStart) loopGapPeakUs = std::max(loopGapPeakUs, uint32_t(started - previousLoopStart));
  previousLoopStart = started;
#endif
}
bool diagnosticsCommand(const char *command, char *reply, size_t capacity) {
  if (!strcmp(command, "get diagnostics")) {
    snprintf(reply, capacity, "Diagnostics completed=%u dropped=%u USB-dropped=%u; since boot",
             completedDiagnostics.load(), droppedDiagnostics.load(), usbDropped.load());
  } else if (!strcmp(command, "stats system")) {
#ifdef ARDUINO_ARCH_ESP32
    snprintf(reply, capacity, "reset=%u loop_peak_us=%u loop_gap_peak_us=%u diag_stack_free_bytes=%u queue_psram=%u",
             unsigned(esp_reset_reason()), loopPeakUs, loopGapPeakUs,
             diagnosticTask ? unsigned(uxTaskGetStackHighWaterMark(diagnosticTask)) : 0,
             diagnosticQueueStorage != nullptr);
#else
    snprintf(reply, capacity, "Error: device task measurements unavailable");
#endif
  } else return false;
  return true;
}

static void diagnosticWorker(void *argument) {
  const auto queue = static_cast<QueueHandle_t>(argument);
  Diagnostic entry;
  for (;;) {
    if (xQueueReceive(queue, &entry, portMAX_DELAY) != pdTRUE)
      continue;
#if MESHCORE_NODE_BACKUP
    nodeBackup().work();
#endif
    char line[176];
    const int size = snprintf(line, sizeof(line), "%s%s%s",
                              entry.companion ? "Companion: " : "", entry.message,
                              entry.message[0] && entry.message[strlen(entry.message) - 1] == '\n' ? "" : "\n");
#ifdef ARDUINO_ARCH_ESP32
    if (size > 0 && size_t(size) < sizeof(line) && Serial && Serial.availableForWrite() >= size)
      Serial.write(reinterpret_cast<const uint8_t *>(line), size);
    else ++usbDropped;
#else
    if (size > 0 && size_t(size) < sizeof(line))
      Serial.write(reinterpret_cast<const uint8_t *>(line), size);
#endif
    if (entry.remote) sendSyslog(entry.message);
    ++completedDiagnostics;
  }
}

bool beginDiagnostics() {
  if (diagnosticQueue) return true;
  QueueHandle_t queue = nullptr;
#ifdef ARDUINO_ARCH_ESP32
  diagnosticQueueStorage = static_cast<uint8_t *>(heap_caps_malloc(
      8 * sizeof(Diagnostic), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (diagnosticQueueStorage)
    queue = xQueueCreateStatic(8, sizeof(Diagnostic), diagnosticQueueStorage, &diagnosticQueueControl);
  else Serial.println("Diagnostics PSRAM unavailable; using internal queue storage");
  if (!queue && diagnosticQueueStorage) {
    heap_caps_free(diagnosticQueueStorage);
    diagnosticQueueStorage = nullptr;
    Serial.println("Diagnostics PSRAM queue creation failed; using internal queue storage");
  }
#endif
  if (!queue) queue = xQueueCreate(8, sizeof(Diagnostic));
  if (!queue)
    return false;
  beginSyslog();
  if (xTaskCreate(diagnosticWorker, "mesh-diagnostics",
#if MESHCORE_NODE_BACKUP
                  8192,
#else
                  4096,
#endif
                  queue, 1,
#ifdef ARDUINO_ARCH_ESP32
                  &diagnosticTask
#else
                  nullptr
#endif
                  ) != pdPASS) {
    vQueueDelete(queue);
#ifdef ARDUINO_ARCH_ESP32
    heap_caps_free(diagnosticQueueStorage);
    diagnosticQueueStorage = nullptr;
#endif
    return false;
  }
  diagnosticQueue.store(queue);
#if MESHCORE_NODE_BACKUP
  beginEspNodeBackup();
#endif
  return true;
}
#if MESHCORE_NODE_BACKUP
bool backupWorkerReady() { return diagnosticQueue.load() != nullptr; }
void wakeBackupWorker() { diagnosticEvent("Node backup preparation requested"); }
#endif
CompanionSessions &companionSessions() {
#ifdef COMPANION_SESSIONS_HOST
  static CompanionSessions sessions(0, 10000, companionDiagnostic);
#else
  static CompanionSessions sessions(ONCHIP_COMPANION_PORT, 10000,
                                    companionDiagnostic);
#endif
  return sessions;
}
static_assert(KISS_LOCAL_SOURCES >= 4,
              "Combined firmware needs three roles and a management source");
bool begin(WifiKissMultiplexer &mux, const mesh::Identity &bot_identity) {
  if (!publicProvisioningReady()) {
    Serial.println("Public setup unavailable; refusing on-chip role startup");
    return false;
  }
#if defined(MESHCORE_PUBLIC_PROVISIONING) && MESHCORE_PUBLIC_PROVISIONING
  if (!initializePublicRuntimePreferences()) return false;
#endif
  reloadAutomaticAdverts();
  if (!loadServiceName(NamedService::Kiss, botStatus.name)) return false;
  if (!loadRoleProfile(bootProfile)) {
    Serial.println("On-chip role profile unavailable; refusing role startup");
    return false;
  }
  if (!bootProfile.bootableWith(adminPassword(), roomPassword())) {
    Serial.println(
        "On-chip roles require an administrator password of 1-15 bytes");
    return false;
  }
  if (!management.begin(mux, operatorPublicKey()))
    return false;
  bool needDiagnostics = bootProfile.has(Role::Companion);
#if defined(ARDUINO_ARCH_ESP32) && defined(MESHCORE_MAST_ADMIN) && MESHCORE_MAST_ADMIN
  needDiagnostics = true;
#endif
#if defined(MESHCORE_ONCHIP_BOT) && MESHCORE_ONCHIP_BOT
  bool commandEnabled = false;
  if (loadBotEnabled(commandEnabled)) needDiagnostics |= commandEnabled;
#endif
  if (needDiagnostics) {
    if (!beginDiagnostics()) {
      Serial.println("On-chip diagnostic worker startup failed");
      if (bootProfile.has(Role::Companion)) return false;
    }
  }
  if (bootProfile.has(Role::Companion)) {
    if (!companionSessions().begin()) {
      Serial.println("On-chip companion listener startup failed");
      return false;
    }
    companionSessions().disable();
  }
  beginClocks(bootProfile);
#if defined(MESHCORE_MAST_ADMIN) && MESHCORE_MAST_ADMIN
  beginNetworkClock(true);
#else
  if (bootProfile.enabled &
      (RoleProfile::Repeater | RoleProfile::Room | RoleProfile::Companion))
    beginNetworkClock();
#endif
  const RoleCallbacks callbacks[] = {
      {repeaterBegin, repeaterFlush, repeaterStop, repeaterEraseStep,
       repeaterLoop, nullptr},
      {roomBegin, roomFlush, roomStop, roomEraseStep, roomLoop, nullptr},
      {companionBegin, companionFlush, companionStop, companionEraseStep,
       companionLoop, companionRequestFailed}};
  beginLifecycles(mux, callbacks, bootProfile);
  strcpy(botStatus.role, "bot");
  memcpy(botStatus.public_key, bot_identity.pub_key,
         sizeof(botStatus.public_key));
  botStatus.has_identity = true;
#if defined(MESHCORE_ONCHIP_BOT) && MESHCORE_ONCHIP_BOT
  static_assert(KISS_LOCAL_SOURCES >= 5, "Command bot needs a fifth local radio source");
  if (diagnosticQueue)
    commandBot.setDiagnosticSink([](const char *text) {
      const bool metrics = !strncmp(text, "Command VM: ", 12) ||
          !strncmp(text, "Command phases: ", 16) || !strncmp(text, "Command task: ", 14) ||
          !strncmp(text, "Wasm pool=", 10) || !strncmp(text, "Command LittleFS source read: ", 29) ||
          !strncmp(text, "Command SPIFFS source read: ", 27);
      return queueDiagnostic(text, false, metrics);
    });
  commandBot.setContactLookup(companionContactAdvert);
  if (!commandBot.begin(mux)) {
    Serial.println("On-chip command bot failed; existing services remain available");
  } else if (commandBot.publicKey() &&
             !(bootProfile.enabled &
               (RoleProfile::Repeater | RoleProfile::Room | RoleProfile::Companion))) {
    beginNetworkClock(true);
  }
#if ONCHIP_BOT_HTTPS
  if (!commandBot.ensureNativeHttps())
    Serial.println("Native HTTPS worker unavailable; telemetry remains configurable");
#endif
#endif
  observerActive = bootProfile.observer() && observer.begin(mux);
  if (bootProfile.observer() && !observerActive) {
    Serial.println("On-chip observer unavailable; native roles remain active");
  }
  roleMux = &mux;
#if defined(ARDUINO_ARCH_ESP32) && MESHCORE_ONCHIP_BOT
  if (!beginPacketPrograms(mux))
    Serial.println("Packet program controller unavailable; inspect boot diagnostics");
#endif
#if defined(MESHCORE_CLOUD_ROOM) && MESHCORE_CLOUD_ROOM
  static_assert(KISS_LOCAL_SOURCES >= 6, "Cloud room needs one source beside the five native services");
  if (cloudRoomConfiguration() && !beginCloudRoom(mux,commandBot))
    Serial.println("Cloud-room driver or HTTPS worker unavailable; service disabled");
#endif
  if (!refreshRolePresence()) {
    Serial.println("On-chip role presence reporting unavailable");
    return false;
  }
  Serial.printf("On-chip boot role mask: 0x%02x (changes require reboot)\n",
                bootProfile.enabled);
  Serial.printf("On-chip network capacity upper bound: %u of %u SDK sockets; "
                "KISS %u, companion %u, HTTP %u\n",
                NETWORK_SOCKET_BUDGET, CONFIG_LWIP_MAX_SOCKETS,
                KISS_MAX_TCP_CLIENTS, CompanionSessions::MaxClients,
                unsigned(RadioDashboard::HTTP_CLIENTS));
  Serial.printf(
      "On-chip role lifecycle service started; companion TCP %u, clients %u; "
      "free heap %u\n",
      ONCHIP_COMPANION_PORT, CompanionSessions::MaxClients, ESP.getFreeHeap());
#ifdef ARDUINO_ARCH_ESP32
  char boot[96];
  snprintf(boot, sizeof(boot), "Boot roles=%u reset=%u heap_free=%u heap_min=%u",
           bootProfile.enabled, unsigned(esp_reset_reason()), ESP.getFreeHeap(), ESP.getMinFreeHeap());
  diagnosticEvent(boot);
#endif
  return true;
}
bool localTransmitSource(uint8_t slot, uint32_t generation,
                         RadioDashboard::RoleStatus &status) {
  RadioDashboard::RoleStatus candidate;
  const auto matches = [&]() {
    if (!candidate.has_identity || candidate.source_slot != slot ||
        candidate.source_generation != generation)
      return false;
    status = candidate;
    return true;
  };
  for (unsigned i = 0; i < 3; ++i) {
    roleStatus(static_cast<Role>(i), candidate);
    if (matches()) return true;
  }
  management.dashboardStatus(candidate);
  if (matches()) return true;
#if defined(MESHCORE_ONCHIP_BOT) && MESHCORE_ONCHIP_BOT
  commandBotService().dashboardStatus(candidate);
  if (matches()) return true;
#endif
  return false;
}
void dashboardStatus(RadioDashboard::RadioStatus &status, bool kiss_listening) {
  strcpy(status.device_name, botStatus.name);
  status.role_count = RadioDashboard::ROLE_CAPACITY;
  for (unsigned i = 0; i < 3; ++i)
    roleStatus(static_cast<Role>(i), status.roles[i]);
  if (bootProfile.has(Role::Companion) &&
      companionSessions().stats().nativeFault) {
    auto &companion = status.roles[2];
    companion.ready = false;
    strcpy(companion.state, "fault");
    strcpy(companion.fault, "Companion session bridge fault");
  }
  if (bootProfile.observer())
    observer.dashboardStatus(status.roles[3]);
  else {
    auto &entry = status.roles[3];
    strcpy(entry.role, "observer");
    strcpy(entry.name, ONCHIP_OBSERVER_NAME);
    strcpy(entry.state, "disabled");
  }
  status.roles[4] = botStatus;
  auto &bot = status.roles[4];
  bot.ready = kiss_listening && status.wifi_connected && !status.fault;
  strcpy(bot.state, status.fault            ? "fault"
                    : !kiss_listening       ? "fault"
                    : status.wifi_connected ? "running"
                                            : "waiting-network");
  if (status.fault)
    strcpy(bot.fault, "Shared radio unavailable");
  else if (!kiss_listening)
    strcpy(bot.fault, "KISS listener unavailable");
  management.dashboardStatus(status.roles[5]);
  companionDashboardContacts(status);
#if defined(MESHCORE_ONCHIP_BOT) && MESHCORE_ONCHIP_BOT
  static_assert(RadioDashboard::ROLE_CAPACITY >= 7, "Command bot needs a dashboard identity slot");
  commandBot.dashboardStatus(status.roles[6]);
#endif
  if (bootProfile.observer())
    observer.observeRoles(status);
}
void loop() {
  loopClocks();
#if defined(ARDUINO_ARCH_ESP32) && MESHCORE_ONCHIP_BOT
  servicePacketPrograms();
#endif
#ifdef ARDUINO_ARCH_ESP32
  static uint32_t lastClockCheck = 0;
  static bool clockKnown = false, clockTrusted = false;
  if (uint32_t(millis() - lastClockCheck) >= 1000) {
    lastClockCheck = millis();
    uint32_t lower = 0, upper = 0;
    const bool trusted = trustedNetworkTime(lower, upper);
    if (!clockKnown || trusted != clockTrusted) {
      diagnosticEvent(trusted ? "UTC synchronized" : "UTC unavailable; inspect get sntp.current");
      clockKnown = true;
      clockTrusted = trusted;
    }
  }
#endif
  management.loop();
  loopLifecycles();
#if defined(MESHCORE_CLOUD_ROOM) && MESHCORE_CLOUD_ROOM
  loopCloudRoom();
#endif
#if defined(MESHCORE_ONCHIP_BOT) && MESHCORE_ONCHIP_BOT
  commandBot.loop();
#endif
  if (!refreshRolePresence())
    Serial.println("On-chip role presence reporting unavailable");
}
uint8_t managementPathWidth() { return management.pathWidth(); }
bool setNativeRolePathWidth(uint8_t width) {
  if (width < 1 || width > 3) return false;
  bool active = management.pathWidth() != 0;
  for (uint8_t i = 0; i < 3; ++i) {
    const auto role = static_cast<Role>(i);
    if (!bootProfile.has(role)) continue;
    if (rolePhase(role) != RolePhase::Running) return false;
    active = true;
  }
  if (!active) return false;
  const bool manager = management.setPathWidth(width);
  const bool repeater = !bootProfile.has(Role::Repeater) || repeaterSetPathWidth(width);
  const bool room = !bootProfile.has(Role::Room) || roomSetPathWidth(width);
  const bool companion = !bootProfile.has(Role::Companion) || companionSetPathWidth(width);
  if (!manager || !repeater || !room || !companion)
    Serial.println("Native role path save failed; partial persistent changes possible");
  return manager && repeater && room && companion;
}
#ifdef COMPANION_SESSIONS_HOST
void stopManagementForTest() {
#if defined(ARDUINO_ARCH_ESP32) && MESHCORE_ONCHIP_BOT
  stopPacketPrograms();
#endif
#if defined(MESHCORE_ONCHIP_BOT) && MESHCORE_ONCHIP_BOT
  commandBot.stop();
#endif
  management.stop();
  botStatus = {};
  roleMux = nullptr;
}
#endif
} // namespace onchip
