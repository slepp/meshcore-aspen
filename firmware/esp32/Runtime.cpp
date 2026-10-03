// SPDX-License-Identifier: Apache-2.0
#include "Runtime.h"
#include "Capacity.h"
#include "CompanionSessions.h"
#include "Config.h"
#include "Observer.h"
#include "Management.h"
#include "ServiceName.h"
#if defined(MESHCORE_ONCHIP_BOT) && MESHCORE_ONCHIP_BOT
#include "CommandBot.h"
#endif
#include <atomic>
#include <freertos/queue.h>
#include <freertos/task.h>

namespace onchip {
static_assert(CompanionSessions::MaxClients == ONCHIP_COMPANION_MAX_CLIENTS,
              "Companion server and combined capacity profile disagree");
struct Diagnostic {
  char message[160];
  bool companion;
};
#if defined(MESHCORE_ONCHIP_BOT) && MESHCORE_ONCHIP_BOT
static_assert(sizeof(Diagnostic::message) >= CommandBot::DiagnosticCapacity,
              "Command diagnostic must fit without truncation");
#endif
static QueueHandle_t diagnosticQueue;
static std::atomic<uint32_t> droppedDiagnostics{0};
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
void observerStatistics(char *reply, size_t capacity) {
  if (observerActive) observer.statistics(reply, capacity);
  else snprintf(reply, capacity, "Error: observer is not running");
}
bool repeaterName(char name[32]);
bool roomName(char name[32]);
bool companionName(char name[32]);
bool repeaterSetName(const char *name);
bool roomSetName(const char *name);
bool companionSetName(const char *name);
RolePasswordUpdate repeaterSetPassword(const char *password);
RolePasswordUpdate roomSetPassword(const char *password);
bool repeaterAdvertiseZeroHop();
bool roomAdvertiseZeroHop();
bool companionAdvertiseZeroHop();

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
bool nativeRoleAdvertiseZeroHop(Role role) {
  if (lifecycleBusy(role)) return false;
  switch (role) {
  case Role::Repeater: return repeaterAdvertiseZeroHop();
  case Role::Room: return roomAdvertiseZeroHop();
  case Role::Companion: return companionAdvertiseZeroHop();
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
  static constexpr uint8_t Capacity = 3 + queued_tx::ROLE_COUNT;
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
  if (!roleMux->setNativeRolePresence(bootProfile.enabled, &keys.keys[0][0],
                                      keys.count))
    return false;
  roleMux->setActiveRolePresence(active);
#if defined(MESHCORE_ONCHIP_BOT) && MESHCORE_ONCHIP_BOT
  commandBot.setNodeRoles(bootProfile.enabled, ready);
#endif
  return true;
}

static bool queueDiagnostic(const char *message, bool companion) {
  Diagnostic entry{};
  snprintf(entry.message, sizeof(entry.message), "%s", message);
  entry.companion = companion;
  if (!diagnosticQueue || xQueueSend(diagnosticQueue, &entry, 0) != pdTRUE) {
    droppedDiagnostics.fetch_add(1);
    return false;
  }
  return true;
}
static void companionDiagnostic(const char *message) {
  queueDiagnostic(message, true);
}

static void diagnosticWorker(void *) {
  Diagnostic entry;
  for (;;) {
    if (xQueueReceive(diagnosticQueue, &entry, portMAX_DELAY) != pdTRUE)
      continue;
    if (entry.companion) Serial.printf("Companion: %s\n", entry.message);
    else Serial.write(reinterpret_cast<const uint8_t *>(entry.message), strlen(entry.message));
    const uint32_t dropped = droppedDiagnostics.exchange(0);
    if (dropped)
      Serial.printf("On-chip diagnostics dropped under backpressure: %u\n",
                    dropped);
  }
}

static bool beginDiagnostics() {
  if (diagnosticQueue) return true;
  diagnosticQueue = xQueueCreate(8, sizeof(Diagnostic));
  if (!diagnosticQueue)
    return false;
  if (xTaskCreate(diagnosticWorker, "mesh-diagnostics", 4096, nullptr, 1,
                  nullptr) != pdPASS) {
    vQueueDelete(diagnosticQueue);
    diagnosticQueue = nullptr;
    return false;
  }
  return true;
}
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
    commandBot.setDiagnosticSink([](const char *text) { return queueDiagnostic(text, false); });
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
#if defined(MESHCORE_ONCHIP_BOT) && MESHCORE_ONCHIP_BOT
  static_assert(RadioDashboard::ROLE_CAPACITY >= 7, "Command bot needs a dashboard identity slot");
  commandBot.dashboardStatus(status.roles[6]);
#endif
  if (bootProfile.observer())
    observer.observeRoles(status);
}
void loop() {
  loopClocks();
  management.loop();
  loopLifecycles();
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
#if defined(MESHCORE_ONCHIP_BOT) && MESHCORE_ONCHIP_BOT
  commandBot.stop();
#endif
  management.stop();
  botStatus = {};
  roleMux = nullptr;
}
#endif
} // namespace onchip
