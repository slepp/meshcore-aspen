// SPDX-License-Identifier: Apache-2.0
#if defined(MESHCORE_ONCHIP_BOT) && MESHCORE_ONCHIP_BOT
#include "BotWorker.h"
#include "BotSignal.h"
#include "BotHttpsMetrics.h"
#include "BotHttpsProbe.h"
#include "BotNetworkConfig.h"
#include "BotVm.h"
#include "BotUtilities.h"
#include <array>
#include "BotStore.h"
#include "BotJournal.h"
#include "BotTimers.h"
#include "RoleStorage.h"
#include <SPIFFS.h>
#include <Utils.h>
#include <algorithm>
#include <memory>
#include <string.h>
#if !defined(ARDUINO_ARCH_ESP32) && !defined(NRF52_PLATFORM)
#include <chrono>
#endif

namespace onchip {
struct BotWorker::Control {
  BotSignal<State> state{Idle};
  BotSignal<bool> stopping{false}, stopped{true}, ioStopped{true}, netStopped{true},
                    sharedState{false}, homeAccess{false};
  BotSignal<bool> reminderAccess{false}, reminderReady{false};
  BotSignal<uint32_t> reminderGrant{1};
  BotSignal<ReminderPhase> reminderPhase{NoReminder};
  BotSignal<uint32_t> generation{1}, homeGrant{1}, sharedGrant{1};
  BotSignal<uint32_t> publication{0}, publicationGeneration{0};
  BotSignal<State> jobs[BotJobLimit], io[BotJobLimit], radio[BotJobLimit];
  BotSignal<State> data{Idle};
  BotSignal<uint8_t> eventAccess{0}, subscriptions{0};
  BotSignal<uint32_t> scheduleSeconds{0};
  BotSignal<uint32_t> eventEpoch{1};
  BotSignal<uint32_t> cancelExcept{0};
  BotSignal<bool> cancelRequested{false};
#if ONCHIP_BOT_SINGLE_SESSION
  BotSignal<uint32_t> sourceGeneration{1};
  BotSignal<bool> sourceSuspended{false};
  BotSignal<bool> ioInitialized{false};
#endif
#if ONCHIP_BOT_HTTPS
  BotSignal<State> network[BotJobLimit];
  BotSignal<State> telemetry{Idle};
  BotSignal<bool> telemetryCancelled{false};
  BotSignal<State> ownerFetch{Idle};
  BotSignal<bool> ownerFetchCancelled{false}, ownerFetchDelivered{false};
#endif
  Control() {
    for (unsigned i = 0; i < BotJobLimit; ++i) jobs[i] = io[i] = radio[i] = Idle;
#if ONCHIP_BOT_HTTPS
    for (auto &state : network) state = Idle;
#endif
  }
};
struct BotWorker::Storage {
  char staged[BotSourceLimit + 1]{};
#if ONCHIP_BOT_SINGLE_SESSION
  char retained[BotSourceLimit + 1]{};
  size_t retainedSize = 0;
#endif
  size_t stagedSize = 0;
  size_t fileSize = 0;
  uint8_t fileSha256[32]{};
  char copyFrom[32]{}, copyTo[32]{};
  BotSession active, candidate;
#if ONCHIP_BOT_SEPARATE_DIAGNOSTICS
  BotSession diagnostics;
#endif
#if ONCHIP_BOT_WASM
  BotSession wasmActive;
#endif
  auto sessions() {
    return std::array<BotSession *, 1 + ONCHIP_BOT_SEPARATE_DIAGNOSTICS + ONCHIP_BOT_WASM>{&active,
#if ONCHIP_BOT_WASM
      &wasmActive,
#endif
#if ONCHIP_BOT_SEPARATE_DIAGNOSTICS
      &diagnostics
#endif
    };
  }
  uint8_t subscriptions() {
    uint8_t mask = active.subscriptions();
#if ONCHIP_BOT_WASM
    mask |= wasmActive.subscriptions();
#endif
    return mask;
  }
  bool eventWasmFirst = false;
#if ONCHIP_BOT_SEPARATE_DIAGNOSTICS
  BotManifest combinedHelp{};
#endif
  uint32_t nextGeneration = 1, candidateGeneration = 0;
  uint32_t lastEventEpoch = 1;
  uint8_t botKey[32]{};
  bool hasIdentity = false, diagnosticsReady = false;
  struct Job {
    BotSignal<State> *state = nullptr;
    BotEvent event{};
    Result result{};
    uint8_t runningRuntimes = 0;
  } jobs[BotJobLimit];
  struct Io {
    BotSignal<State> *state = nullptr;
    BotIoRequest request{};
    BotIoResult result{};
    uint32_t pollAt = 0;
    uint32_t startedAt = 0, untrustedAt = 0;
    bool suspended = false;
  } io[BotJobLimit], radio[BotJobLimit];
#if ONCHIP_BOT_HTTPS
  Io network[BotJobLimit];
  Io ownerFetch;
  uint32_t ownerFetchSequence = 0;
  BotHttpsFetchRequest packageRequest{};
  BotHttpsFetchResult packageResult{};
  BotHttpsTransport *transport = nullptr;
  BotHttpsConfig httpsConfig{};
  struct {
    TelemetryEndpoint endpoint;
    char body[TelemetryBodyLimit + 1]{};
    size_t size = 0;
    uint32_t deadline = 0;
    TelemetryCompletion result;
  } telemetry;
#endif
  struct Timer {
    bool used = false;
    BotIoToken token{};
    uint32_t due = 0, eventEpoch = 0;
  } timers[BotJobLimit];
  Result result{};
  BotReminderDispatch reminder{};
  struct Data {
    enum Phase { Empty, Export, Upload, Stage, Staged, Restore, Complete } phase = Empty;
    BotStore::Snapshot snapshot{};
    char id[17]{}, status[128]{};
    uint8_t hash[32]{};
    size_t received = 0;
    uint32_t generation = 0;
  } data;
};
const size_t BotWorker::StorageBytes = sizeof(Storage) + sizeof(Control);
namespace {
std::atomic<uint32_t> publicationSequence{0};
void pauseWorker() {
#if defined(ARDUINO_ARCH_ESP32) || defined(NRF52_PLATFORM)
  vTaskDelay(1);
#else
  std::this_thread::sleep_for(std::chrono::milliseconds(1));
#endif
}
}
bool BotWorker::begin(const uint8_t botKey[32], BotHttpsTransport *transport,
                      const BotHttpsConfig *httpsConfig, bool networkOnly) {
  if (storage_) return false;
#if ONCHIP_BOT_HTTPS
  if (!beginBotHttpsMemory()) {
    Serial.println("On-chip HTTPS memory allocator setup failed");
    return false;
  }
#endif
  storage_ = allocateRoleStorage<Storage>("command VM");
  if (!storage_) return false;
#ifdef ARDUINO_ARCH_ESP32
  void *memory = heap_caps_malloc(sizeof(Control), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  control_ = memory ? new (memory) Control : nullptr;
#else
  control_ = new (std::nothrow) Control;
#endif
  if (!control_) {
    releaseRoleStorage(storage_);
    Serial.println("On-chip command worker internal synchronization storage unavailable");
    return false;
  }
  for (unsigned i = 0; i < BotJobLimit; ++i) {
    storage_->jobs[i].state = &control_->jobs[i];
    storage_->io[i].state = &control_->io[i];
    storage_->radio[i].state = &control_->radio[i];
#if ONCHIP_BOT_HTTPS
    storage_->network[i].state = &control_->network[i];
#endif
  }
#if ONCHIP_BOT_HTTPS
  storage_->ownerFetch.state = &control_->ownerFetch;
  storage_->transport = transport;
  storage_->httpsConfig = httpsConfig ? *httpsConfig : botHttpsConfig();
#else
  (void)transport; (void)httpsConfig;
#endif
  if (botKey) { memcpy(storage_->botKey, botKey, 32); storage_->hasIdentity = true; }
  control_->state = Idle;
  control_->stopping = false;
  control_->generation = 1;
  if (!networkOnly) {
    control_->stopped = false;
#ifdef ARDUINO_ARCH_ESP32
    if (xTaskCreate(entry, "mesh-command", 16384, this, 1, &task_) != pdPASS) {
#elif defined(NRF52_PLATFORM)
    if (xTaskCreate(entry, "mesh-command", 4096, this, 1, &task_) != pdPASS) {
#endif
#if defined(ARDUINO_ARCH_ESP32) || defined(NRF52_PLATFORM)
      control_->stopped = true;
      stop();
      Serial.println("On-chip command VM task allocation failed");
      return false;
    }
#else
    task_ = std::thread(entry, this);
#endif
  }
  if (botKey) {
    control_->ioStopped = false;
#ifdef ARDUINO_ARCH_ESP32
    if (xTaskCreate(ioEntry, "mesh-bot-store", 6144, this, 1, &ioTask_) != pdPASS) {
#elif defined(NRF52_PLATFORM)
    if (xTaskCreate(ioEntry, "mesh-bot-store", 1536, this, 1, &ioTask_) != pdPASS) {
#endif
#if defined(ARDUINO_ARCH_ESP32) || defined(NRF52_PLATFORM)
      control_->ioStopped = true;
      stop();
      Serial.println("On-chip bot storage task allocation failed");
      return false;
    }
#else
    ioTask_ = std::thread(ioEntry, this);
#endif
  }
#if ONCHIP_BOT_HTTPS
  if (botKey || networkOnly) {
    control_->netStopped = false;
#ifdef ARDUINO_ARCH_ESP32
    if (xTaskCreate(netEntry, "mesh-bot-https", 16384, this, 1, &netTask_) != pdPASS) {
      control_->netStopped = true;
      stop();
      Serial.println("On-chip HTTPS task allocation failed");
      return false;
    }
#else
    netTask_ = std::thread(netEntry, this);
#endif
  }
#endif
  return true;
}
void BotWorker::stop() {
  if (!storage_) return;
  control_->stopping = true;
  ++control_->generation;
#if !defined(ARDUINO_ARCH_ESP32) && !defined(NRF52_PLATFORM)
  if (task_.joinable()) task_.join();
  if (ioTask_.joinable()) ioTask_.join();
#if ONCHIP_BOT_HTTPS
  if (netTask_.joinable()) netTask_.join();
#endif
#else
  while (!control_->stopped.load() || !control_->ioStopped.load() || !control_->netStopped.load()) pauseWorker();
#endif
  releaseRoleStorage(storage_);
#ifdef ARDUINO_ARCH_ESP32
  control_->~Control();
  heap_caps_free(control_);
#else
  delete control_;
#endif
  control_ = nullptr;
}
uint32_t BotWorker::generation() const { return control_ ? control_->generation.load() : 0; }
uint32_t BotWorker::sourceGeneration() const {
#if ONCHIP_BOT_SINGLE_SESSION
  return control_ ? control_->sourceGeneration.load() : 0;
#else
  return generation();
#endif
}
bool BotWorker::setSharedState(bool enabled) {
  if (!control_) return false;
  control_->sharedState = false;
  if (control_->sharedGrant.load() == UINT32_MAX) {
    Serial.println("On-chip shared storage grant generation exhausted; disabled until restart");
    return false;
  }
  ++control_->sharedGrant;
  control_->sharedState = enabled;
  return true;
}
void BotWorker::setHomeAccess(bool enabled) {
  if (!control_) return;
  if (control_->homeGrant.load() == UINT32_MAX) {
    control_->homeAccess = false;
    Serial.println("On-chip HTTPS grant generation exhausted; disabled until restart");
    return;
  }
  control_->homeAccess = false;
  ++control_->homeGrant;
  control_->homeAccess = enabled;
}
bool BotWorker::setReminderAccess(bool enabled) {
  if (!control_) return false;
  control_->reminderAccess = false;
  if (control_->reminderGrant == UINT32_MAX) {
    Serial.println("Reminder grant epoch exhausted; disabled until reboot"); return false;
  }
  ++control_->reminderGrant;
  control_->reminderAccess = enabled;
  return true;
}
bool BotWorker::reminderAccess() const { return control_ && control_->reminderAccess.load(); }
bool BotWorker::setEventAccess(uint8_t mask) {
  if (!control_ || mask > 31) return false;
  control_->eventAccess = 0;
  if (!control_->eventEpoch.load() || control_->eventEpoch == UINT32_MAX) {
    Serial.println("Bot event epoch exhausted; disabled until reboot"); return false;
  }
  ++control_->eventEpoch; control_->eventAccess = mask; return true;
}
uint8_t BotWorker::eventAccess() const {
  return control_ && control_->eventEpoch.load() ? control_->eventAccess.load() : 0;
}
uint32_t BotWorker::scheduleSeconds() const {
  return control_ && (eventMask() & 16) ? control_->scheduleSeconds.load() : 0;
}
uint32_t BotWorker::eventEpoch() const {
  return control_ ? control_->eventEpoch.load() : 0;
}
uint8_t BotWorker::eventMask() const {
  return control_ && control_->eventEpoch.load() && control_->state.load() == Idle ?
      control_->eventAccess.load() & control_->subscriptions.load() : 0;
}
void BotWorker::setReminderReady(bool ready) {
  if (control_) control_->reminderReady = ready && control_->state.load() == Idle;
}
bool BotWorker::inspectReminder(BotReminderDispatch &dispatch) {
  if (!control_ || control_->reminderPhase.load() != Offered) return false;
  dispatch = storage_->reminder; control_->reminderPhase = Inspecting; return true;
}
void BotWorker::approveReminder(bool approved) {
  if (control_ && control_->reminderPhase.load() == Inspecting) {
    storage_->reminder.approved = approved; control_->reminderPhase = Checked;
  }
}
bool BotWorker::takeReminder(BotReminderDispatch &dispatch) {
  if (!control_ || control_->reminderPhase.load() != Claimed) return false;
  dispatch = storage_->reminder; control_->reminderPhase = Transmitting; return true;
}
bool BotWorker::reminderCurrent(const BotReminderDispatch &dispatch) const {
  return control_ && !control_->stopping.load() && control_->reminderAccess.load() &&
      control_->reminderReady.load() && control_->state.load() == Idle &&
      control_->generation.load() == dispatch.generation &&
      control_->reminderGrant.load() == dispatch.grant;
}
bool BotWorker::completeReminder(uint32_t id, BotReminderState outcome) {
  if (!control_ || control_->reminderPhase.load() != Transmitting || storage_->reminder.id != id) return false;
  storage_->reminder.outcome = outcome; control_->reminderPhase = Completed; return true;
}
bool BotWorker::busy() const {
  if (!control_) return false;
  if (control_->state.load() != Idle) return true;
  if (storage_) for (const auto &job : storage_->jobs)
    if (job.state->load() != Idle) return true;
  return false;
}
#if defined(MESHCORE_HOST_BOT_SOURCE) && MESHCORE_HOST_BOT_SOURCE
bool BotWorker::resultReady() const { return control_ && control_->state.load() == Done; }
bool BotWorker::ownerWorkPending() const {
  if (!control_) return false;
  if (resultReady()) return true;
  for (unsigned i = 0; i < BotJobLimit; ++i)
    if (control_->jobs[i].load() == Done || control_->radio[i].load() == Pending) return true;
  const auto reminder = control_->reminderPhase.load();
  return reminder == Offered || reminder == Claimed;
}
#endif
unsigned BotWorker::jobsInUse() const {
  unsigned count = 0;
  if (control_) for (const auto &state : control_->jobs) count += state.load() != Idle;
  return count;
}
bool BotWorker::canInvoke() const {
  if (!storage_ || control_->state.load() != Idle) return false;
#if ONCHIP_BOT_SINGLE_SESSION
  if (control_->sourceSuspended) return false;
#endif
  for (const auto &job : storage_->jobs) if (job.state->load() == Idle) return true;
  return false;
}
void BotWorker::dataCommand(const char *command, char *reply, size_t capacity) {
  const auto error = [&](const char *message) { snprintf(reply, capacity, "Error: %s", message); };
  if (!storage_ || !storage_->hasIdentity || control_->stopping) {
    error("bot-data requires the running persistent bot identity"); return;
  }
#if ONCHIP_BOT_SINGLE_SESSION
  if (!control_->ioInitialized) { error("Persistent bot storage is initializing; retry bot data"); return; }
#endif
  auto &data = storage_->data;
  const auto state = control_->data.load();
  if (state == Pending || state == Running) {
    snprintf(reply, capacity, "BUSY bot-data"); return;
  }
  if (state == Done) control_->data = Idle;
  const auto decode = [](const char *hex, uint8_t *out, size_t size) {
    if (strlen(hex) != size * 2) return false;
    for (size_t i = 0; i < size; ++i) {
      const auto digit = [](char c) { return c >= '0' && c <= '9' ? c - '0' :
          c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
      const int a = digit(hex[2 * i]), b = digit(hex[2 * i + 1]);
      if (a < 0 || b < 0) return false;
      out[i] = uint8_t(a * 16 + b);
    }
    return true;
  };
  const auto pending = [&] {
    data.generation = control_->generation;
    strcpy(data.status, "PENDING"); control_->data = Pending;
    snprintf(reply, capacity, "PENDING %s", data.id);
  };
  char id[17]{}, hex[97]{}, position[4]{}, extra = 0;
  unsigned index = 0;
  const auto chunkIndex = [&] {
    if (!position[0]) return false;
    for (const char *p = position; *p; ++p) {
      if (*p < '0' || *p > '9') return false;
      index = index * 10 + unsigned(*p - '0');
    }
    return index <= 51;
  };
  if (!strcmp(command, "help")) {
    snprintf(reply, capacity, "data export [kv|timers|reminders] SCOPE KEY64; status; read ID N; begin ID SHA256; chunk ID N HEX; stage ID; restore ID [no-rearm]; clear");
  } else if (!strcmp(command, "status")) {
    snprintf(reply, capacity, "%s %s", data.status[0] ? data.status : "EMPTY", data.id);
  } else if (!strcmp(command, "clear")) {
    data = {}; snprintf(reply, capacity, "CLEARED");
  } else if (!strncmp(command, "export ", 7)) {
    char scope[13]{};
    const char *arguments = command + 7, *magic = "BKD\1";
    if (!strncmp(arguments, "kv ", 3)) arguments += 3;
    else if (!strncmp(arguments, "timers ", 7)) { arguments += 7; magic = "BTD\1"; }
    else if (!strncmp(arguments, "reminders ", 10)) { arguments += 10; magic = "BRD\1"; }
    if (sscanf(arguments, "%12s %64s %c", scope, hex, &extra) != 2) {
      error("data export [kv|timers|reminders] SCOPE PRINCIPAL64"); return;
    }
    uint8_t principal[32];
    const int selected = !strcmp(scope, "caller") ? BotIoRequest::Caller :
        !strcmp(scope, "conversation") ? BotIoRequest::Conversation :
        !strcmp(scope, "bot") ? BotIoRequest::Bot :
        !strcmp(scope, "channel") ? BotIoRequest::Channel : -1;
    if (selected < 0 || !decode(hex, principal, 32)) { error("invalid data scope/principal"); return; }
    uint8_t bits = 0; for (auto byte : principal) bits |= byte;
    if ((selected == BotIoRequest::Bot) != !bits) { error("bot scope requires zero principal; other scopes require full nonzero identity"); return; }
    if (!memcmp(magic, "BRD", 3) && selected != BotIoRequest::Caller) {
      error("reminders require caller scope"); return;
    }
    data = {}; data.snapshot.scope = uint8_t(selected);
    memcpy(data.snapshot.magic, magic, 4);
    memcpy(data.snapshot.principal, principal, 32); data.phase = Storage::Data::Export; pending();
  } else if (!strncmp(command, "begin ", 6)) {
    uint8_t hash[32], idBytes[8];
    if (sscanf(command + 6, "%16s %64s %c", id, hex, &extra) != 2 ||
        !decode(id, idBytes, 8) || !decode(hex, hash, 32) || strncmp(id, hex, 16)) {
      error("data begin requires content SHA256 and its first 16 hex digits as ID"); return;
    }
    data = {}; strcpy(data.id, id); memcpy(data.hash, hash, 32);
    data.phase = Storage::Data::Upload; strcpy(data.status, "UPLOADING");
    snprintf(reply, capacity, "UPLOADING %s bytes=%u", id, unsigned(sizeof(data.snapshot)));
  } else if (!strncmp(command, "chunk ", 6)) {
    uint8_t bytes[48];
    if (sscanf(command + 6, "%16s %3s %96s %c", id, position, hex, &extra) != 3 ||
        !chunkIndex() || data.phase != Storage::Data::Upload || strcmp(id, data.id) || index >= 51 ||
        !hex[0] || strlen(hex) % 2 || !decode(hex, bytes, strlen(hex) / 2)) {
      error("invalid data chunk, ID or upload phase"); return;
    }
    const size_t offset = size_t(index) * 48, count = strlen(hex) / 2;
    const size_t remaining = offset < sizeof(data.snapshot) ? sizeof(data.snapshot) - offset : 0;
    const size_t expected = std::min(size_t(48), remaining);
    auto *raw = reinterpret_cast<uint8_t *>(&data.snapshot);
    if (count != expected || offset > data.received ||
        (offset < data.received && (offset + count > data.received || memcmp(raw + offset, bytes, count)))) {
      error("data chunks must be complete, ordered and identical on retry"); return;
    }
    memcpy(raw + offset, bytes, count);
    if (offset == data.received) data.received += count;
    snprintf(reply, capacity, "RECEIVED %u", unsigned(data.received));
  } else if (!strncmp(command, "read ", 5)) {
    if (sscanf(command + 5, "%16s %3s %c", id, position, &extra) != 2 || !chunkIndex() ||
        data.phase != Storage::Data::Complete || strcmp(id, data.id) ||
        strncmp(data.status, "EXPORTED ", 9) || index > 51) { error("data export unavailable or ID/index invalid"); return; }
    const size_t offset = size_t(index) * 48;
    if (offset >= sizeof(data.snapshot)) { snprintf(reply, capacity, "EOF"); return; }
    const size_t count = std::min(size_t(48), sizeof(data.snapshot) - offset);
    if (capacity < 6 + count * 2) { error("data reply capacity"); return; }
    strcpy(reply, "DATA ");
    const auto *raw = reinterpret_cast<const uint8_t *>(&data.snapshot);
    for (size_t i = 0; i < count; ++i) snprintf(reply + 5 + 2 * i, capacity - 5 - 2 * i, "%02x", raw[offset + i]);
  } else if (!strncmp(command, "stage ", 6) || !strncmp(command, "restore ", 8)) {
    const bool restore = command[0] == 'r';
    const bool scheduler = !memcmp(data.snapshot.magic, "BTD\1", 4) || !memcmp(data.snapshot.magic, "BRD\1", 4);
    char policy[9]{};
    const int fields = sscanf(command + (restore ? 8 : 6), "%16s %8s %c", id, policy, &extra);
    if (strcmp(id, data.id) || (restore && scheduler ? fields != 2 || strcmp(policy, "no-rearm") : fields != 1)) {
      if (restore && scheduler) { error("scheduler restore requires ID no-rearm; matching pending work is cancelled"); return; }
      error("data stage/restore ID mismatch"); return;
    }
    if (restore && data.phase == Storage::Data::Complete) {
      snprintf(reply, capacity, "%s %s", data.status, id); return;
    }
    if (restore ? data.phase != Storage::Data::Staged :
        data.phase != Storage::Data::Upload || data.received != sizeof(data.snapshot)) {
      error("data restore requires validated stage; stage requires all 2422 bytes"); return;
    }
    data.phase = restore ? Storage::Data::Restore : Storage::Data::Stage; pending();
  } else error("unknown data command; use data help");
}
bool BotWorker::canInvoke(const BotEvent &event, unsigned collecting, unsigned customCollecting) const {
  if (!canInvoke() || collecting >= BotJobLimit || customCollecting > collecting) return false;
  const bool subscription = event.kind != BotEvent::Command;
  unsigned used = collecting, custom = customCollecting;
  for (const auto &job : storage_->jobs) if (job.state->load() != Idle) {
    ++used;
    if (job.result.operation == Operation::Event) {
      if (subscription) return false;
      ++custom;
    } else if (!botReservedCommand(job.event.name)) ++custom;
  }
  if (used >= BotJobLimit) return false;
  if ((subscription || !botReservedCommand(event.name)) && custom >= BotJobLimit - 1) return false;
  if (subscription && (event.kind > BotEvent::Scheduled ||
      !(eventMask() & (1u << (unsigned(event.kind) - 1))))) return false;
  return true;
}
bool BotWorker::invoke(const BotEvent &event, uint32_t id) {
  if (!canInvoke(event)) return false;
  const bool subscription = event.kind != BotEvent::Command;
  for (auto &job : storage_->jobs) if (job.state->load() == Idle) {
    job.event = event;
    job.event.eventEpoch = subscription ? control_->eventEpoch.load() : 0;
    job.event.homeGrant = control_->homeGrant.load();
    job.event.networkEpoch = botNetworkEpoch();
    job.event.homeAccess = event.homeAccess && control_->homeAccess.load();
    job.event.sharedGrant = control_->sharedGrant.load();
    job.event.sharedState = event.sharedState && control_->sharedState.load();
    job.event.reminderAccess = control_->reminderAccess.load();
    job.event.reminderGrant = control_->reminderGrant.load();
    job.result = {};
    job.result.operation = subscription ? Operation::Event : Operation::Invoke;
    job.result.job = id; job.result.generation = control_->generation;
    *job.state = Pending;
    return true;
  }
  return false;
}
bool BotWorker::sourceMutationAllowed(uint32_t publication) const {
  return control_ && (control_->publication.load() ?
      sourcePublicationCurrent(publication) : publication == 0);
}
bool BotWorker::claimSourceMutation(uint32_t publication) {
  if (!control_ || control_->stopping) return false;
  State idle = Idle;
  if (!control_->state.compare_exchange_strong(idle, Expiring)) return false;
  if (sourceMutationAllowed(publication)) return true;
  control_->state = Idle;
  return false;
}
bool BotWorker::sourcePublicationCurrent(uint32_t publication) const {
  return control_ && publication && control_->publication.load() == publication &&
         control_->publicationGeneration.load() == control_->generation.load();
}
void BotWorker::releaseSourcePublication(uint32_t publication) {
  if (control_ && publication) control_->publication.compare_exchange_strong(publication, 0);
}
uint32_t BotWorker::reserveSourcePublication(const BotWorker *validator, bool wasm,
                                            char *error, size_t capacity, uint32_t publication) {
  if (capacity) error[0] = 0;
  const auto fail = [&](const char *message) {
    snprintf(error, capacity, "%s", message); return uint32_t(0);
  };
#if !ONCHIP_BOT_WASM
  if (wasm) return fail("Wasm runtime unavailable in this build");
#endif
  if (!storage_ || control_->stopping) return fail("Command VM unavailable; enable bot before source activation");
  if (validator && (!validator->storage_ || !validator->control_ ||
      validator->control_->state.load() != Idle || !validator->storage_->result.ok ||
      !validator->storage_->stagedSize || validator->storage_->candidate.isWasm() != wasm))
    return fail("Validated source manifest unavailable or runtime changed");
  if (!validator && !wasm) return fail("Lua publication requires a validated command manifest");
  State idle = Idle;
  if (!control_->state.compare_exchange_strong(idle, Expiring)) return 0;
  if (!sourceMutationAllowed(publication)) {
    control_->state = Idle;
    return publication ? fail("Source publication generation changed; prior source retained") : 0;
  }
  // Command names are immutable after initialization. Idle ownership prevents
  // source swap/clear while jobs may keep executing the retained program.
  uint32_t token = publication;
#if ONCHIP_BOT_WASM
  const auto &other = wasm ? storage_->active.manifest() : storage_->wasmActive.manifest();
  if (validator) for (unsigned i = 0; i < validator->storage_->candidate.manifest().count; ++i) {
    const char *name = validator->storage_->candidate.manifest().commands[i].name;
    if (other.find(name)) {
      snprintf(error, capacity, "Command !%s is already registered by %s runtime; source unchanged",
               name, wasm ? "Lua" : "Wasm");
      control_->state = Idle; return 0;
    }
  }
#endif
  if (!token) {
    uint32_t sequence = publicationSequence.load();
    do {
      if (sequence == UINT32_MAX) {
        control_->state = Idle; return fail("Source publication capacity exhausted; reboot required");
      }
    } while (!publicationSequence.compare_exchange_weak(sequence, sequence + 1));
    token = sequence + 1;
    control_->publicationGeneration = control_->generation.load();
    control_->publication = token;
  }
  control_->state = Idle;
  return token;
}
bool BotWorker::stage(const char *source, size_t size, uint32_t publication) {
  if (!storage_ || !source || !size || size > BotSourceLimit ||
      !claimSourceMutation(publication)) return false;
  memcpy(storage_->staged, source, size);
  storage_->staged[size] = 0;
  storage_->stagedSize = size;
  storage_->result = {};
  storage_->result.operation = Operation::Stage;
  control_->state = Pending;
  return true;
}
bool BotWorker::stageFile(size_t expectedSize, const uint8_t expectedSha256[32], uint32_t publication) {
  if (!storage_ || !expectedSha256 || !expectedSize ||
      expectedSize > BotSourceLimit || !claimSourceMutation(publication))
    return false;
  storage_->fileSize = expectedSize;
  memcpy(storage_->fileSha256, expectedSha256, sizeof(storage_->fileSha256));
  storage_->result = {};
  storage_->result.operation = Operation::StageFile;
  control_->state = Pending;
  return true;
}
bool BotWorker::activate(uint32_t publication) {
  if (!storage_ || !claimSourceMutation(publication)) return false;
  storage_->result = {};
  storage_->result.operation = Operation::Activate;
  control_->state = Pending;
  return true;
}
bool BotWorker::removeWasm(uint32_t publication) {
#if !ONCHIP_BOT_WASM
  return false;
#else
  if (!storage_ || !claimSourceMutation(publication)) return false;
  storage_->result = {};
  storage_->result.operation = Operation::RemoveWasm;
  control_->state = Pending;
  return true;
#endif
}
bool BotWorker::copyFile(const char *from, const char *to, size_t expectedSize, uint32_t publication) {
  const auto allowed = [](const char *path) {
    const char *paths[] = {BotStagedSourcePath, "/command-bot/upload.lua",
                          "/command-bot/a.lua", "/command-bot/b.lua", "/command-bot/c.lua",
                          "/command-bot/wupload.lua", "/command-bot/wa.lua",
                          "/command-bot/wb.lua", "/command-bot/wc.lua"};
    for (const auto *candidate : paths) if (!strcmp(path, candidate)) return true;
    return false;
  };
  if (!storage_ || busy() || !to || !allowed(to) || (from && !allowed(from)) ||
      !expectedSize || expectedSize > BotSourceLimit ||
      (!from && expectedSize != strlen(BotDefaultSource)) || !claimSourceMutation(publication)) return false;
  strcpy(storage_->copyFrom, from ? from : "");
  strcpy(storage_->copyTo, to);
  storage_->fileSize = expectedSize;
  storage_->result = {};
  storage_->result.operation = Operation::CopyFile;
  control_->state = Pending;
  return true;
}
#if ONCHIP_BOT_SINGLE_SESSION
bool BotWorker::sourceSuspended() const {
  return !control_ || control_->sourceSuspended.load();
}
bool BotWorker::recoverSource() {
  if (!storage_ || !claimSourceMutation(control_->publication.load())) return false;
  storage_->result = {};
  storage_->result.operation = Operation::Recover;
  control_->state = Pending;
  return true;
}
#endif
bool BotWorker::poll(Result &result) {
  if (!storage_) return false;
  if (control_->state.load() == Done) {
    result = storage_->result;
    control_->state = Idle;
    return true;
  }
  for (auto &job : storage_->jobs) if (job.state->load() == Done) {
    result = job.result;
    *job.state = Idle;
    return true;
  }
  return false;
}
bool BotWorker::pollRadio(BotIoRequest &request) {
  if (storage_) for (auto &io : storage_->radio) if (io.state->load() == Pending) {
    request = io.request; *io.state = Running; return true;
  }
  return false;
}
bool BotWorker::completeRadio(const BotIoResult &result) {
  if (storage_) for (auto &io : storage_->radio)
    if (io.state->load() == Running && io.request.token == result.token) {
      io.result = result; *io.state = Done; return true;
    }
  return false;
}
bool BotWorker::rejectRadio(const BotIoToken &token, const char *reason) {
  if (storage_) for (auto &io : storage_->radio)
    if (io.state->load() == Running && io.request.token == token) {
      resetBotIoResult(io.result);
      io.result.token = token;
      snprintf(io.result.error, sizeof(io.result.error), "%s",
               reason ? reason : "Radio request rejected");
      *io.state = Done;
      return true;
    }
  return false;
}
#if ONCHIP_BOT_HTTPS
bool BotWorker::ensureNativeHttps() {
  return storage_ ? control_ && !control_->netStopped.load() :
                    begin(nullptr, nullptr, nullptr, true);
}
bool BotWorker::submitTelemetry(const char *body, size_t size) {
  if (!storage_ || !control_ || control_->netStopped || control_->stopping ||
      control_->telemetry.load() != Idle || !body || !size || size > TelemetryBodyLimit) return false;
  auto &job = storage_->telemetry;
  if (!telemetryEndpoint(job.endpoint)) return false;
  memcpy(job.body, body, size);
  job.body[size] = 0;
  job.size = size;
  job.deadline = millis() + 2 * BotHttpsDeadlineMs;
  job.result = {};
  control_->telemetryCancelled = false;
  control_->telemetry = Pending;
  return true;
}
bool BotWorker::pollTelemetry(TelemetryCompletion &result) {
  if (!control_ || control_->telemetry.load() != Done) return false;
  result = storage_->telemetry.result;
  auto *secret = reinterpret_cast<volatile uint8_t *>(&storage_->telemetry.endpoint);
  for (size_t i = 0; i < sizeof(storage_->telemetry.endpoint); ++i) secret[i] = 0;
  memset(storage_->telemetry.body, 0, sizeof(storage_->telemetry.body));
  control_->telemetry = Idle;
  return true;
}
void BotWorker::cancelTelemetry() {
  if (control_) control_->telemetryCancelled = true;
}
#endif
bool BotWorker::requestOwnerFetch(const char *endpoint) {
#if ONCHIP_BOT_HTTPS
  if (control_ && control_->ownerFetchDelivered.load() &&
      control_->ownerFetch.load() == Done) control_->ownerFetch = Idle;
  if (!storage_ || !storage_->hasIdentity || !control_->homeAccess.load() ||
      control_->stopping.load() || control_->ownerFetch.load() != Idle ||
      storage_->ownerFetchSequence == UINT32_MAX || !endpoint ||
      !*endpoint || strnlen(endpoint, BotNameLimit + 1) > BotNameLimit ||
      *endpoint < 'a' || *endpoint > 'z') return false;
  for (const char *p = endpoint; *p; ++p)
    if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_')) return false;
  auto &io = storage_->ownerFetch;
  io.request = {};
  io.result = {};
  io.request.kind = BotIoRequest::HttpGet;
  io.request.token = {control_->generation.load(), 1, ++storage_->ownerFetchSequence};
  io.request.grant = control_->homeGrant.load();
  io.request.networkEpoch = botNetworkEpoch();
  io.request.deadline = millis() + BotHttpsDeadlineMs;
  memcpy(io.request.principal, storage_->botKey, 32);
  strcpy(io.request.key, endpoint);
  control_->ownerFetchCancelled = false;
  control_->ownerFetchDelivered = false;
  *io.state = Pending;
  return true;
#else
  (void)endpoint;
  return false;
#endif
}
bool BotWorker::pollOwnerFetch(BotIoResult &result) {
#if ONCHIP_BOT_HTTPS
  if (!storage_) return false;
  auto &io = storage_->ownerFetch;
  if (io.request.kind != BotIoRequest::HttpGet) return false;
  if (control_->ownerFetchCancelled.load() && io.state->load() != Idle) {
    if (control_->ownerFetchDelivered.exchange(true)) return false;
    result = {}; result.token = io.request.token;
    strcpy(result.rpcCode, "cancelled");
    strcpy(result.error, "Owner HTTPS fetch cancelled; remote outcome unknown");
    State done = Done;
    io.state->compare_exchange_strong(done, Idle);
    return true;
  }
  if (control_->ownerFetchDelivered.load()) return false;
  State pending = Pending;
  if (int32_t(millis() - io.request.deadline) >= 0 &&
      io.state->compare_exchange_strong(pending, Expiring)) {
    io.result = {}; io.result.token = io.request.token;
    strcpy(io.result.rpcCode, "timeout");
    strcpy(io.result.error, "Owner HTTPS fetch expired while waiting for shared socket");
    *io.state = Done;
  }
  if (io.state->load() != Done) return false;
  result = io.result;
  if (control_->stopping.load() ||
      io.request.token.generation != control_->generation.load()) {
    result.ok = false; result.json[0] = 0;
    strcpy(result.rpcCode, "cancelled");
    strcpy(result.error, "Owner HTTPS fetch cancelled; remote outcome unknown");
  } else if (!control_->homeAccess.load() ||
             io.request.grant != control_->homeGrant.load() ||
             io.request.networkEpoch != botNetworkEpoch()) {
    const bool uncertain = result.networkSubmitted;
    result.ok = false; result.json[0] = 0;
    strcpy(result.rpcCode, uncertain ? "unknown" : "permission_denied");
    strcpy(result.error, uncertain ?
        "Owner HTTPS fetch grant/configuration revoked; remote outcome unknown" :
        "Owner HTTPS fetch grant/configuration revoked before submission");
  }
  *io.state = Idle;
  control_->ownerFetchDelivered = true;
  return true;
#else
  (void)result;
  return false;
#endif
}
void BotWorker::cancelOwnerFetch() {
#if ONCHIP_BOT_HTTPS
  if (control_ && control_->ownerFetch.load() != Idle &&
      storage_->ownerFetch.request.kind == BotIoRequest::HttpGet)
    control_->ownerFetchCancelled = true;
#endif
}
bool BotWorker::fetchPackage(const BotHttpsFetchRequest &request) {
#if ONCHIP_BOT_HTTPS
  if (control_ && control_->ownerFetchDelivered.load() &&
      control_->ownerFetch.load() == Done) control_->ownerFetch = Idle;
  if (!storage_ || !storage_->hasIdentity || !control_->homeAccess.load() ||
      control_->stopping.load() || control_->ownerFetch.load() != Idle ||
      storage_->ownerFetchSequence == UINT32_MAX || !request.id || !request.sink ||
      !request.maxBytes || request.maxBytes > BotSourceLimit) return false;
  auto &io = storage_->ownerFetch;
  io.request = {}; io.result = {};
  io.request.kind = BotIoRequest::PackageGet;
  io.request.token = {control_->generation.load(), 1, ++storage_->ownerFetchSequence};
  io.request.grant = control_->homeGrant.load();
  io.request.networkEpoch = botNetworkEpoch();
  io.request.deadline = millis() + BotHttpsDeadlineMs;
  memcpy(io.request.principal, storage_->botKey, 32);
  strcpy(io.request.key, "package");
  storage_->packageRequest = request;
  storage_->packageResult = {};
  storage_->packageResult.id = request.id;
  control_->ownerFetchCancelled = false;
  control_->ownerFetchDelivered = false;
  *io.state = Pending;
  return true;
#else
  (void)request;
  return false;
#endif
}
bool BotWorker::pollPackageFetch(BotHttpsFetchResult &result) {
#if ONCHIP_BOT_HTTPS
  if (!storage_ || storage_->ownerFetch.request.kind != BotIoRequest::PackageGet ||
      control_->ownerFetchDelivered.load()) return false;
  auto &io = storage_->ownerFetch;
  State pending = Pending;
  if (int32_t(millis() - io.request.deadline) >= 0 &&
      io.state->compare_exchange_strong(pending, Expiring)) {
    io.result = {}; io.result.token = io.request.token;
    strcpy(io.result.rpcCode, "timeout");
    strcpy(io.result.error, "Package fetch expired while waiting for shared socket");
    storage_->packageResult = {};
    storage_->packageResult.id = storage_->packageRequest.id;
    storage_->packageResult.state = BotHttpsFetchResult::Failed;
    *io.state = Done;
  }
  const State state = io.state->load();
  if (state == Idle || state == Expiring) return false;
  if (state == Pending || state == Running) {
    result = {};
    result.id = storage_->packageRequest.id;
    result.state = state == Pending ? BotHttpsFetchResult::Queued : BotHttpsFetchResult::Running;
    return true;
  }
  result = storage_->packageResult;
  result.httpStatus = io.result.httpStatus;
  result.networkSubmitted = io.result.networkSubmitted;
  if (control_->ownerFetchCancelled.load() || control_->stopping.load() ||
      io.request.token.generation != control_->generation.load()) {
    result.state = result.networkSubmitted ? BotHttpsFetchResult::Unknown :
                                            BotHttpsFetchResult::Failed;
    strcpy(result.rpcCode, "cancelled");
    strcpy(result.error, "Package fetch cancelled; source staging discarded");
  } else if (!control_->homeAccess.load() ||
             io.request.grant != control_->homeGrant.load() ||
             io.request.networkEpoch != botNetworkEpoch()) {
    result.state = result.networkSubmitted ? BotHttpsFetchResult::Unknown :
                                            BotHttpsFetchResult::Failed;
    strcpy(result.rpcCode, result.networkSubmitted ? "unknown" : "permission_denied");
    strcpy(result.error, "Package fetch grant/configuration changed; source staging discarded");
  } else {
    result.state = io.result.ok ? BotHttpsFetchResult::Complete :
                   !strcmp(io.result.rpcCode, "unknown") ? BotHttpsFetchResult::Unknown :
                                                          BotHttpsFetchResult::Failed;
    snprintf(result.rpcCode, sizeof(result.rpcCode), "%s", io.result.rpcCode);
    snprintf(result.error, sizeof(result.error), "%.*s",
             int(sizeof(result.error) - 1), io.result.error);
  }
  if (result.state != BotHttpsFetchResult::Complete)
    storage_->packageRequest.sink->abort(result.error);
  storage_->packageRequest = {};
  storage_->packageResult = result;
  control_->ownerFetchDelivered = true;
  *io.state = Idle;
  return true;
#else
  (void)result;
  return false;
#endif
}
void BotWorker::cancelPackageFetch() {
#if ONCHIP_BOT_HTTPS
  if (control_ && storage_->ownerFetch.request.kind == BotIoRequest::PackageGet &&
      control_->ownerFetch.load() != Idle) {
    control_->ownerFetchCancelled = true;
    auto &io = storage_->ownerFetch;
    State pending = Pending;
    if (io.state->compare_exchange_strong(pending, Expiring)) {
      io.result = {}; io.result.token = io.request.token;
      strcpy(io.result.rpcCode, "cancelled");
      strcpy(io.result.error, "Package fetch cancelled before submission");
      *io.state = Done;
    }
  }
#endif
}
void BotWorker::entry(void *context) {
  static_cast<BotWorker *>(context)->run();
#if defined(ARDUINO_ARCH_ESP32) || defined(NRF52_PLATFORM)
  vTaskDelete(nullptr);
#endif
}
void BotWorker::ioEntry(void *context) {
  static_cast<BotWorker *>(context)->runIo();
#if defined(ARDUINO_ARCH_ESP32) || defined(NRF52_PLATFORM)
  vTaskDelete(nullptr);
#endif
}
void BotWorker::netEntry(void *context) {
  static_cast<BotWorker *>(context)->runNet();
#ifdef ARDUINO_ARCH_ESP32
  vTaskDelete(nullptr);
#endif
}
void BotWorker::runNet() {
#if ONCHIP_BOT_HTTPS
  {
    std::unique_ptr<BotHttpsTransport> owned;
    auto *transport = storage_->transport;
    if (!transport) {
      owned.reset(createBotHttpsTransport());
      transport = owned.get();
    }
    std::unique_ptr<BotHttps> provider(transport ?
        new (std::nothrow) BotHttps(storage_->httpsConfig, *transport) : nullptr);
#if ONCHIP_BOT_HTTPS_SELF_TEST && defined(ARDUINO_ARCH_ESP32)
    if (transport) runBotHttpsProbe(*transport, storage_->httpsConfig, storage_->botKey, control_->stopping);
#endif
    unsigned cursor = 0;
    while (!control_->stopping.load()) {
#if defined(MESHCORE_HOST_BOT_SOURCE) && MESHCORE_HOST_BOT_SOURCE
      const auto revision = BotWake::shared().snapshot();
#endif
      for (unsigned i = 0; i < BotJobLimit; ++i) {
        auto &io = storage_->network[cursor];
        cursor = (cursor + 1) % BotJobLimit;
        State pending = Pending;
        if (!io.state->compare_exchange_strong(pending, Running)) continue;
        const uint32_t started = millis();
        if (provider) {
          const bool bundled = io.request.kind == BotIoRequest::Rpc &&
              (!io.request.endpoint[0] || !strcmp(io.request.endpoint, "home")) &&
              (!strcmp(io.request.key, "health") || !strcmp(io.request.key, "echo") ||
               !strcmp(io.request.key, "weather"));
          BotNetworkRoute route;
          const bool configuredHome = bundled && botNetworkHomeConfigured();
          if ((!bundled || configuredHome) && !botNetworkResolve(io.request, route)) {
            io.result = {}; io.result.token = io.request.token;
            strcpy(io.result.rpcCode, "permission_denied");
            strcpy(io.result.error, "Configured HTTPS endpoint or RPC mapping not granted");
          } else {
            provider->perform(io.request, io.result, control_->generation, control_->homeGrant,
                              control_->homeAccess, control_->stopping,
                              bundled && !configuredHome ? nullptr : &route);
          }
        } else {
          io.result = {}; io.result.token = io.request.token;
          strcpy(io.result.rpcCode, "unavailable");
          strcpy(io.result.error, "Native HTTPS provider unavailable");
        }
        if (!io.result.ok)
          Serial.printf("On-chip HTTPS: code=%s status=%u\n",
              io.result.rpcCode[0] ? io.result.rpcCode : "native_error",
              unsigned(io.result.httpStatus));
#ifdef ARDUINO_ARCH_ESP32
        Serial.printf("HTTPS worker: status=%u elapsed=%u ms internal=%u global-min=%u stack=%u bytes\n",
            io.result.httpStatus, unsigned(millis() - started),
            unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
            unsigned(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
            unsigned(uxTaskGetStackHighWaterMark(nullptr)));
#else
        (void)started;
#endif
        *io.state = Done;
        break;
      }
      // One lower-priority native sample follows at most one interactive RPC.
      // Both use this task's one transport; no Lua grant or identity is borrowed.
      if (control_->telemetry.load() == Pending) {
        control_->telemetry = Running;
        auto &job = storage_->telemetry;
        const int32_t remaining = int32_t(job.deadline - millis());
        if (control_->telemetryCancelled || control_->stopping) {
          job.result = {}; job.result.error = TelemetryError::Cancelled;
        } else if (remaining <= 0) {
          job.result = {}; job.result.error = TelemetryError::Timeout;
        } else if (transport) {
          const uint32_t deadline = millis() + std::min<uint32_t>(uint32_t(remaining), BotHttpsDeadlineMs);
          performTelemetryPost(*transport, job.endpoint, job.body, job.size, deadline,
                               control_->telemetryCancelled, control_->stopping, job.result);
        } else {
          job.result = {}; job.result.error = TelemetryError::Transport;
        }
        control_->telemetry = Done;
      }
      auto &fetch = storage_->ownerFetch;
      State pending = Pending;
      if (fetch.state->compare_exchange_strong(pending, Running)) {
        BotNetworkRoute route;
        if (!botNetworkResolve(fetch.request, route)) {
          fetch.result = {}; fetch.result.token = fetch.request.token;
          strcpy(fetch.result.rpcCode, "permission_denied");
          strcpy(fetch.result.error, "Owner HTTPS endpoint is not an approved GET");
        } else if (provider) {
          provider->perform(fetch.request, fetch.result, control_->generation,
                            control_->homeGrant, control_->homeAccess,
                            control_->ownerFetchCancelled, &route,
                            fetch.request.kind == BotIoRequest::PackageGet ?
                                &storage_->packageRequest : nullptr,
                            fetch.request.kind == BotIoRequest::PackageGet ?
                                &storage_->packageResult : nullptr);
        } else {
          fetch.result = {}; fetch.result.token = fetch.request.token;
          strcpy(fetch.result.rpcCode, "unavailable");
          strcpy(fetch.result.error, "Native HTTPS provider unavailable");
        }
        *fetch.state = Done;
        if (control_->ownerFetchCancelled.load() && control_->ownerFetchDelivered.load()) {
          State done = Done;
          fetch.state->compare_exchange_strong(done, Idle);
        }
      }
#if defined(MESHCORE_HOST_BOT_SOURCE) && MESHCORE_HOST_BOT_SOURCE
      BotWake::shared().wait(revision);
#else
      pauseWorker();
#endif
    }
  }
#endif
  control_->netStopped = true;
}
void BotWorker::runIo() {
  {
    BotStore store;
    BotTimers timers;
    BotReminders reminders;
#if ONCHIP_BOT_SINGLE_SESSION
    char error[128]{};
    auto &snapshot = storage_->data.snapshot;
    snapshot.principal[0] = 0xff;
    if (!store.snapshot(storage_->botKey, snapshot, error, sizeof(error)))
      Serial.printf("Bot KV startup: %s\n", error);
    snapshot = {};
    snapshot.principal[0] = 0xff;
    if (!timers.snapshot(storage_->botKey, snapshot, error, sizeof(error)))
      Serial.printf("Bot timers startup: %s\n", error);
    snapshot = {};
    snapshot.principal[0] = 0xff;
    if (!reminders.snapshot(storage_->botKey, snapshot, error, sizeof(error)))
      Serial.printf("Bot reminders startup: %s\n", error);
    snapshot = {};
    control_->ioInitialized = true;
#endif
    BotIoResult *restoreResult = nullptr;
    uint32_t nextReminderPoll = 0;
    while (!control_->stopping.load()) {
#if defined(MESHCORE_HOST_BOT_SOURCE) && MESHCORE_HOST_BOT_SOURCE
      const auto revision = BotWake::shared().snapshot();
#endif
      if (control_->data.load() == Pending) {
        control_->data = Running;
        auto &data = storage_->data;
        char error[128]{};
        const bool current = data.generation == control_->generation.load();
        if (!current) {
          strcpy(data.status, "REJECTED source generation changed"); data.phase = Storage::Data::Complete;
        } else if (data.phase == Storage::Data::Export) {
          const bool ok = !memcmp(data.snapshot.magic, "BTD\1", 4) ?
              timers.snapshot(storage_->botKey, data.snapshot, error, sizeof(error)) :
              !memcmp(data.snapshot.magic, "BRD\1", 4) ?
              reminders.snapshot(storage_->botKey, data.snapshot, error, sizeof(error)) :
              store.snapshot(storage_->botKey, data.snapshot, error, sizeof(error));
          if (ok) {
            mesh::Utils::sha256(data.hash, 32, reinterpret_cast<const uint8_t *>(&data.snapshot), sizeof(data.snapshot));
            char hash[65];
            for (unsigned i = 0; i < 32; ++i) snprintf(hash + i * 2, 3, "%02x", data.hash[i]);
            memcpy(data.id, hash, 16); data.id[16] = 0;
            snprintf(data.status, sizeof(data.status), "EXPORTED %s", hash);
          } else snprintf(data.status, sizeof(data.status), "REJECTED %.110s", error);
          data.phase = Storage::Data::Complete;
        } else if (data.phase == Storage::Data::Stage) {
          uint8_t hash[32];
          mesh::Utils::sha256(hash, 32, reinterpret_cast<const uint8_t *>(&data.snapshot), sizeof(data.snapshot));
          const bool valid = !memcmp(data.snapshot.magic, "BTD\1", 4) ?
              BotTimers::validSnapshot(data.snapshot, storage_->botKey, error, sizeof(error)) :
              !memcmp(data.snapshot.magic, "BRD\1", 4) ?
              BotReminders::validSnapshot(data.snapshot, storage_->botKey, error, sizeof(error)) :
              BotStore::validSnapshot(data.snapshot, storage_->botKey, error, sizeof(error));
          if (memcmp(hash, data.hash, 32) || !valid) {
            snprintf(data.status, sizeof(data.status), "REJECTED %.100s", error[0] ? error : "transport SHA256 mismatch");
            data.phase = Storage::Data::Complete;
          } else { strcpy(data.status, "STAGED"); data.phase = Storage::Data::Staged; }
        } else if (data.phase == Storage::Data::Restore) {
          if (!restoreResult) restoreResult = allocateRoleStorage<BotIoResult>("bot data restore result");
          if (!restoreResult) {
            strcpy(data.status, "REJECTED data restore result storage unavailable");
            data.phase = Storage::Data::Complete;
            control_->data = Done;
            continue;
          }
          auto &result = *restoreResult;
          resetBotIoResult(result);
          if (!store.recover(result.error, sizeof(result.error))) {
            Serial.printf("On-chip bot data restore blocked by KV recovery: %s\n", result.error);
          } else if (!memcmp(data.snapshot.magic, "BTD\1", 4))
            timers.restore(storage_->botKey, data.snapshot, result, data.generation, control_->generation);
          else if (!memcmp(data.snapshot.magic, "BRD\1", 4)) {
            if (control_->reminderPhase.load() != NoReminder)
              strcpy(result.error, "Reminder delivery in progress; no records imported");
            else reminders.restore(storage_->botKey, data.snapshot, result, data.generation, control_->generation);
          } else store.restore(storage_->botKey, data.snapshot, result, data.generation, control_->generation);
          snprintf(data.status, sizeof(data.status), "%s %.110s",
                   result.outcome == BotIoResult::Committed ? "COMMITTED" :
                   result.outcome == BotIoResult::Unknown ? "UNKNOWN" : "REJECTED", result.error);
          data.phase = Storage::Data::Complete;
        }
        control_->data = Done;
      }
      for (auto &io : storage_->io) if (io.state->load() == Pending) {
        if (io.pollAt && int32_t(millis() - io.pollAt) < 0 &&
            io.request.token.generation == control_->generation &&
            botEventCurrent(io.request, &control_->eventEpoch) &&
            (!io.request.sharedScope() ||
             (control_->sharedState && io.request.grant == control_->sharedGrant.load()))) continue;
        *io.state = Running;
        if (io.request.kind == BotIoRequest::TimerWait &&
            (uint32_t(millis() - io.startedAt) >= BotTimerWaitLifetimeMs ||
             (io.suspended && uint32_t(millis() - io.untrustedAt) >= BotTimerClockSuspendMs))) {
          resetBotIoResult(io.result); io.result.token = io.request.token;
          strcpy(io.result.error, "Timer wait suspension/lifetime exceeded; journal unchanged by this wait");
          Serial.printf("On-chip bot timer: %s\n", io.result.error);
          *io.state = Done; continue;
        }
        if (!botEventCurrent(io.request, &control_->eventEpoch)) {
          resetBotIoResult(io.result); io.result.token = io.request.token;
          strcpy(io.result.error, "Event epoch revoked before storage admission");
        } else if (storage_->hasIdentity) {
          resetBotIoResult(io.result); io.result.token = io.request.token;
          if (io.request.kind == BotIoRequest::Utility) {
            BotUtilityResult utility;
            if (io.request.token.generation != control_->generation.load())
              strcpy(utility.error, "Utility source generation revoked");
            else io.result.ok = io.request.delaySeconds == 0 ? botCalculate(io.request.value, utility) :
                io.request.delaySeconds == 1 ? botConvert(io.request.value, io.request.key, io.request.endpoint, utility) :
                io.request.delaySeconds == 2 ? botRoll(io.request.value[0] ? io.request.value : nullptr, utility) :
                                              botChoose(io.request.value, utility);
            strcpy(io.result.value, utility.text); strcpy(io.result.error, utility.error);
          } else if ((io.request.reminder() || io.request.durableTimer()) &&
              !store.recover(io.result.error, sizeof(io.result.error))) {
            Serial.printf("On-chip scheduler blocked by KV recovery: %s\n", io.result.error);
          } else if (io.request.reminder())
            reminders.perform(storage_->botKey, io.request, io.result, control_->generation,
                              control_->reminderAccess, control_->reminderGrant, control_->stopping,
                              control_->reminderPhase.load() == NoReminder ? 0 : storage_->reminder.id);
          else if (io.request.durableTimer())
            timers.perform(storage_->botKey, io.request, io.result, control_->generation,
                           control_->sharedState, control_->sharedGrant, &control_->eventEpoch);
          else
            store.perform(storage_->botKey, io.request, io.result, control_->generation,
                          control_->sharedState, control_->sharedGrant, &control_->eventEpoch);
        } else {
          resetBotIoResult(io.result); io.result.token = io.request.token;
          strcpy(io.result.error, "Storage requires the bot's persistent identity");
        }
        if (io.result.ok && io.result.pending) {
          if (io.request.kind == BotIoRequest::TimerWait) {
            if (io.result.timeTrusted) io.suspended = false;
            else if (!io.suspended) { io.untrustedAt = millis(); io.suspended = true; }
          }
          io.request.revision = io.result.revision;
          io.pollAt = millis() + 250;
          *io.state = Pending;
          continue;
        }
        *io.state = Done;
      }
      char error[128]{};
      const auto phase = control_->reminderPhase.load();
      if (phase == Checked) {
        auto &dispatch = storage_->reminder;
        if (dispatch.approved && reminderCurrent(dispatch) &&
            store.recover(error, sizeof(error)) &&
            reminders.claim(storage_->botKey, dispatch, control_->reminderAccess,
                            control_->reminderGrant, control_->stopping, error, sizeof(error)))
          control_->reminderPhase = Claimed;
        else control_->reminderPhase = NoReminder;
        nextReminderPoll = millis() + 250;
      } else if (phase == Completed) {
        if (store.recover(error, sizeof(error)))
          reminders.complete(storage_->botKey, storage_->reminder, control_->stopping, error, sizeof(error));
        control_->reminderPhase = NoReminder;
        nextReminderPoll = millis() + 250;
      } else if (phase == NoReminder && control_->reminderAccess && control_->reminderReady &&
                 int32_t(millis() - nextReminderPoll) >= 0) {
        if (store.recover(error, sizeof(error)) &&
            reminders.next(storage_->botKey, storage_->reminder, control_->stopping, error, sizeof(error))) {
          storage_->reminder.generation = control_->generation;
          storage_->reminder.grant = control_->reminderGrant;
          control_->reminderPhase = Offered;
        }
        nextReminderPoll = millis() + 1000;
      }
      if (error[0]) Serial.printf("On-chip reminder: %s\n", error);
#if defined(MESHCORE_HOST_BOT_SOURCE) && MESHCORE_HOST_BOT_SOURCE
      uint32_t wait = UINT32_MAX;
      const uint32_t now = millis();
      for (const auto &io : storage_->io) if (io.state->load() == Pending)
        botEarlier(wait, now, io.pollAt);
      if (control_->reminderPhase.load() == NoReminder &&
          control_->reminderAccess && control_->reminderReady)
        botEarlier(wait, now, nextReminderPoll);
      BotWake::shared().wait(revision, wait);
#else
      pauseWorker();
#endif
    }
    releaseRoleStorage(restoreResult);
  }
  control_->ioStopped = true;
}
void BotWorker::cancelJobs(uint32_t except) {
  if (!control_) return;
  control_->cancelExcept = except;
  control_->cancelRequested = true;
}
void BotWorker::pumpJobs() {
  auto &s = *storage_;
  if (control_->cancelRequested.exchange(false)) {
    const auto except = control_->cancelExcept.load();
    for (auto *vm : s.sessions()) vm->cancel(except);
    for (auto &timer : s.timers) if (timer.used && timer.token.job != except) timer = {};
    for (auto &job : s.jobs) if (job.state->load() == Pending && job.result.job != except) {
      strcpy(job.result.error, "Owner cancelled invocation before startup");
      *job.state = Done;
    }
  }
  if (s.lastEventEpoch != control_->eventEpoch.load()) {
    for (auto *vm : s.sessions()) vm->cancelEvents();
    s.lastEventEpoch = control_->eventEpoch.load();
    for (auto &timer : s.timers)
      if (timer.used && timer.eventEpoch && timer.eventEpoch != s.lastEventEpoch) timer = {};
  }
  control_->subscriptions = s.subscriptions();
  control_->scheduleSeconds = s.active.manifest().scheduleSeconds;
  for (auto &job : s.jobs) if (job.state->load() == Pending) {
    *job.state = Running;
    const auto *installed = s.active.manifest().find(job.event.name);
    const bool diagnostic = job.event.kind == BotEvent::Command &&
        botReservedCommand(job.event.name) &&
        (!installed || botReservedCommand(installed->function));
    bool wasm = false;
#if ONCHIP_BOT_WASM
    wasm = job.event.kind == BotEvent::Command ?
        s.wasmActive.manifest().find(job.event.name) != nullptr :
        !(s.active.subscriptions() & (1u << (unsigned(job.event.kind) - 1)));
    auto &vm = diagnostic ? s.diagnostics : wasm ? s.wasmActive : s.active;
#else
#if ONCHIP_BOT_SEPARATE_DIAGNOSTICS
    auto &vm = diagnostic ? s.diagnostics : s.active;
#else
    auto &vm = s.active;
#endif
#endif
#if ONCHIP_BOT_SEPARATE_DIAGNOSTICS
    if (diagnostic) {
      s.combinedHelp = s.active.manifest();
#if ONCHIP_BOT_WASM
      if (s.wasmActive.manifest().count && s.combinedHelp.isBundled()) s.combinedHelp.clear();
      for (unsigned i = 0; i < s.wasmActive.manifest().count && s.combinedHelp.count < BotBuiltinCommandLimit; ++i)
        s.combinedHelp.writable()[s.combinedHelp.count++] = s.wasmActive.manifest().commands[i];
#endif
      vm.setHelp(s.combinedHelp);
    }
#endif
    if (job.event.kind != BotEvent::Command) {
      job.runningRuntimes = 0;
      job.result.ok = true;
      auto startEvent = [&](BotSession &runtime, uint8_t bit) {
        if (!(runtime.subscriptions() & (1u << (unsigned(job.event.kind) - 1)))) return;
        if (job.result.generation != control_->generation ||
            job.event.eventEpoch != control_->eventEpoch.load() ||
            !runtime.start(job.result.job, job.event, job.result.error, sizeof(job.result.error))) {
          job.result.ok = false;
          if (!job.result.error[0]) strcpy(job.result.error, "Event generation/grant revoked before admission");
        } else job.runningRuntimes |= bit;
      };
#if ONCHIP_BOT_WASM
      s.eventWasmFirst = !s.eventWasmFirst;
      if (s.eventWasmFirst) startEvent(s.wasmActive, 2);
#endif
      startEvent(s.active, 1);
#if ONCHIP_BOT_WASM
      if (!s.eventWasmFirst) startEvent(s.wasmActive, 2);
#endif
      if (!job.runningRuntimes) *job.state = Done;
      continue;
    }
    job.runningRuntimes = diagnostic && ONCHIP_BOT_SEPARATE_DIAGNOSTICS ? 4 : wasm ? 2 : 1;
    if (job.result.generation != control_->generation ||
        (job.event.eventEpoch && job.event.eventEpoch != control_->eventEpoch.load()) ||
        !vm.start(job.result.job, job.event, job.result.error, sizeof(job.result.error))) {
      if (!job.result.error[0]) strcpy(job.result.error, "Command generation changed before admission");
      *job.state = Done;
    }
  }
  for (auto &timer : s.timers) if (timer.used && int32_t(millis() - timer.due) >= 0) {
    BotIoResult result{};
    result.token = timer.token;
    result.ok = uint32_t(millis() - timer.due) < 1000;
    if (!result.ok) strcpy(result.error, "Timer completion too late");
    for (auto *vm : s.sessions()) vm->complete(result);
    timer = {};
  }
  for (auto &io : s.io) if (io.state->load() == Done) {
    if (!botEventCurrent(io.request, &control_->eventEpoch)) {
      io.result.ok = false; strcpy(io.result.error, "Event epoch revoked; admitted effects may have committed");
    }
    if (io.request.sharedScope() &&
        (!control_->sharedState || io.request.grant != control_->sharedGrant.load())) {
      io.result.ok = false;
      io.result.value[0] = 0; io.result.keys = {}; io.result.found = false;
      strcpy(io.result.error, "Shared storage grant revoked before resumption; mutation may have committed");
    }
    if (io.request.kind == BotIoRequest::ReminderSet &&
        (!control_->reminderAccess || io.request.grant != control_->reminderGrant.load())) {
      io.result.ok = false;
      strcpy(io.result.error, "Reminder grant revoked before resumption; reminder may have committed");
    }
    if (io.request.kind == BotIoRequest::TimerWait && io.result.ok &&
        !botTimerClaimCurrent(io.result.deadlineUtc, io.result.error, sizeof(io.result.error))) {
      io.result.ok = false;
      Serial.printf("On-chip bot timer: %s\n", io.result.error);
    }
    for (auto *vm : s.sessions()) vm->complete(io.result);
    *io.state = Idle;
  }
  for (auto &io : s.radio) if (io.state->load() == Done) {
    for (auto *vm : s.sessions()) vm->complete(io.result);
    *io.state = Idle;
  }
#if ONCHIP_BOT_HTTPS
  for (auto &io : s.network) {
    State pending = Pending;
    if (int32_t(millis() - io.request.deadline) >= 0 &&
        io.state->compare_exchange_strong(pending, Expiring)) {
      io.result = {}; io.result.token = io.request.token;
      strcpy(io.result.rpcCode, "timeout");
      strcpy(io.result.error, "HTTPS request expired while waiting for shared socket");
      *io.state = Done;
    }
  }
  for (auto &io : s.network) if (io.state->load() == Done) {
    if (!control_->homeAccess || io.request.grant != control_->homeGrant.load() ||
        io.request.networkEpoch != botNetworkEpoch()) {
      const bool uncertain = io.result.networkSubmitted;
      io.result.ok = false;
      io.result.json[0] = 0;
      io.result.value[0] = io.result.country[0] = io.result.source[0] = io.result.observedAt[0] = 0;
      io.result.temperatureC = 0; io.result.weatherCode = 0; io.result.sourceAgeSeconds = 0;
      strcpy(io.result.rpcCode, uncertain ? "unknown" : "permission_denied");
      strcpy(io.result.error, uncertain ?
          "HTTPS grant/configuration revoked after submission; remote outcome unknown" :
          "HTTPS grant/configuration revoked before submission");
    }
    for (auto *vm : s.sessions()) vm->complete(io.result);
    *io.state = Idle;
  }
#endif
  BotIoRequest request;
  auto providers = s.sessions();
#if ONCHIP_BOT_WASM
  if (s.eventWasmFirst) std::swap(providers[0], providers[1]);
#endif
  for (unsigned round = 0; round < BotJobLimit; ++round)
  for (BotSession *vm : providers) if (vm->nextIo(request)) {
    bool admitted = false;
    if (!botEventCurrent(request, &control_->eventEpoch)) {
      BotIoResult failed{}; failed.token = request.token;
      strcpy(failed.error, "Event epoch revoked before I/O admission");
      vm->complete(failed); continue;
    }
    if (request.kind == BotIoRequest::Sleep) {
      for (auto &timer : s.timers) if (!timer.used) {
        timer = {true, request.token, uint32_t(millis() + request.delayMs), request.eventEpoch};
        admitted = true; break;
      }
    } else if (request.kind == BotIoRequest::Send || request.kind == BotIoRequest::Forward ||
               request.kind == BotIoRequest::Wait ||
               request.kind == BotIoRequest::Trace || request.kind == BotIoRequest::Advert ||
               request.kind == BotIoRequest::Inspect || request.kind == BotIoRequest::Admin ||
               request.kind == BotIoRequest::RepeaterNext || request.kind == BotIoRequest::RepeaterStatus ||
               request.kind == BotIoRequest::RepeaterLogin) {
      for (auto &io : s.radio) if (io.state->load() == Idle) {
        io.request = request; io.result = {};
        *io.state = Pending; admitted = true; break;
      }
    } else if (request.kind == BotIoRequest::Rpc ||
               request.kind == BotIoRequest::HttpGet ||
               request.kind == BotIoRequest::HttpPost) {
#if ONCHIP_BOT_HTTPS
      request.networkEpoch = 0;
      for (const auto &job : s.jobs)
        if (job.state->load() == Running && job.result.job == request.token.job &&
            job.result.generation == request.token.generation) {
          request.networkEpoch = job.event.networkEpoch;
          break;
        }
      if (s.hasIdentity && control_->homeAccess && request.grant == control_->homeGrant.load() &&
          request.networkEpoch && request.networkEpoch == botNetworkEpoch()) {
        if (request.kind != BotIoRequest::Rpc && !request.key[0] &&
            request.endpoint[0]) {
          strcpy(request.key, request.endpoint);
          request.endpoint[0] = 0;
        }
        for (auto &io : s.network) if (io.state->load() == Idle) {
          request.deadline = millis() + BotHttpsDeadlineMs;
          io.request = request; io.result = {};
          *io.state = Pending; admitted = true; break;
        }
      }
#endif
    } else if (s.hasIdentity) {
      for (auto &io : s.io) if (io.state->load() == Idle) {
        io.request = request; io.result = {}; io.pollAt = 0;
        io.startedAt = millis(); io.suspended = false;
        *io.state = Pending;
        admitted = true; break;
      }
    }
    if (!admitted) {
      BotIoResult failed{}; failed.token = request.token;
      strcpy(failed.error, request.kind == BotIoRequest::Rpc ||
             request.kind == BotIoRequest::HttpGet || request.kind == BotIoRequest::HttpPost ?
             request.networkEpoch && request.networkEpoch != botNetworkEpoch() ?
             "HTTPS configuration changed before admission" :
             "Home HTTPS unavailable, denied or busy" : "Native I/O queue exhausted");
      if (request.kind == BotIoRequest::Rpc ||
          request.kind == BotIoRequest::HttpGet ||
          request.kind == BotIoRequest::HttpPost)
        strcpy(failed.rpcCode, request.networkEpoch &&
               request.networkEpoch != botNetworkEpoch() ? "permission_denied" : "unavailable");
      vm->complete(failed);
    }
  }
  BotSession::Result result;
  for (BotSession *vm : s.sessions()) while (vm->poll(result)) {
    for (auto &job : s.jobs)
      if (job.state->load() == Running && job.result.job == result.job) {
        const uint8_t bit = vm == &s.active ? 1 :
#if ONCHIP_BOT_SEPARATE_DIAGNOSTICS
            vm == &s.diagnostics ? 4 :
#endif
            2;
        if (!(job.runningRuntimes & bit)) continue;
        job.runningRuntimes &= ~bit;
        job.result.ok = job.result.operation == Operation::Event ? job.result.ok && result.ok : result.ok;
        job.result.action = result.action;
        job.result.stats = result.stats;
        if (result.error[0]) strcpy(job.result.error, result.error);
#ifdef ARDUINO_ARCH_ESP32
        job.result.stats.freeInternalBytes = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        job.result.stats.freePsramBytes = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        job.result.stats.stackHighWaterBytes = uxTaskGetStackHighWaterMark(nullptr);
#elif defined(NRF52_PLATFORM)
        job.result.stats.freeInternalBytes = dbgHeapFree();
        job.result.stats.stackHighWaterBytes = uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t);
#endif
        if (!job.runningRuntimes) *job.state = Done;
        break;
      }
  }
}
bool BotWorker::loadStagedFile() {
  auto &s = *storage_;
  s.stagedSize = 0;
  const uint32_t started = millis();
  auto file = SPIFFS.open(BotStagedSourcePath, "r");
  const auto fail = [&](const char *message) {
    file.close();
    s.result.sourceReadMs = uint32_t(millis() - started);
    snprintf(s.result.error, sizeof(s.result.error), "%s", message);
    return false;
  };
  if (!file) return fail("Command source SPIFFS file unavailable");
  if (file.isDirectory() || file.size() != s.fileSize)
    return fail("Command source file length does not match bounded manifest");
  for (size_t offset = 0; offset < s.fileSize;) {
    if (uint32_t(millis() - started) >= BotSourceReadBudgetMs)
      return fail("Command source read deadline exceeded");
    const size_t remaining = s.fileSize - offset;
    const size_t chunk = remaining < 256 ? remaining : 256;
    if (file.read(reinterpret_cast<uint8_t *>(s.staged + offset), chunk) != chunk)
      return fail("Command source SPIFFS read incomplete");
    offset += chunk;
  }
  if (uint32_t(millis() - started) >= BotSourceReadBudgetMs)
    return fail("Command source read deadline exceeded");
  if (file.read() != -1)
    return fail("Command source file grew during read");
  file.close();
  uint8_t digest[32];
  mesh::Utils::sha256(digest, sizeof(digest),
                      reinterpret_cast<const uint8_t *>(s.staged), s.fileSize);
  if (memcmp(digest, s.fileSha256, sizeof(digest)))
    return fail("Command source SHA-256 does not match manifest");
  s.result.sourceReadMs = uint32_t(millis() - started);
  if (s.result.sourceReadMs >= BotSourceReadBudgetMs)
    return fail("Command source read deadline exceeded");
  s.staged[s.fileSize] = 0;
  s.stagedSize = s.fileSize;
  return true;
}
bool BotWorker::copySourceFile() {
  auto &s = *storage_;
  const uint32_t started = millis();
  const auto fail = [&](const char *message) {
    s.result.sourceReadMs = uint32_t(millis() - started);
    snprintf(s.result.error, sizeof(s.result.error), "%s", message);
    return false;
  };
  if (s.copyFrom[0]) {
    auto input = SPIFFS.open(s.copyFrom, "r");
    if (!input || input.isDirectory() || input.size() != s.fileSize)
      return fail("Source copy input length/unavailable");
    for (size_t offset = 0; offset < s.fileSize;) {
      const size_t n = std::min(size_t(256), s.fileSize - offset);
      if (control_->stopping || uint32_t(millis() - started) >= BotSourceCopyBudgetMs)
        return fail("Source copy deadline/cancellation");
      if (input.read(reinterpret_cast<uint8_t *>(s.staged + offset), n) != n)
        return fail("Source copy input incomplete");
      offset += n;
    }
  } else memcpy(s.staged, BotDefaultSource, s.fileSize);
  auto output = SPIFFS.open(s.copyTo, "w");
  if (!output) return fail("Source copy destination unavailable");
  for (size_t offset = 0; offset < s.fileSize;) {
    const size_t n = std::min(size_t(256), s.fileSize - offset);
    if (control_->stopping || uint32_t(millis() - started) >= BotSourceCopyBudgetMs)
      return fail("Source copy deadline/cancellation");
    if (output.write(reinterpret_cast<const uint8_t *>(s.staged + offset), n) != n)
      return fail("Source copy write incomplete");
    offset += n;
    pauseWorker();
  }
  output.flush(); output.close();
  auto check = SPIFFS.open(s.copyTo, "r");
  if (!check || check.size() != s.fileSize) return fail("Source copy readback length");
  uint8_t bytes[256];
  for (size_t offset = 0; offset < s.fileSize;) {
    const size_t n = std::min(sizeof(bytes), s.fileSize - offset);
    if (control_->stopping || uint32_t(millis() - started) >= BotSourceCopyBudgetMs)
      return fail("Source copy deadline/cancellation");
    if (check.read(bytes, n) != n || memcmp(bytes, s.staged + offset, n))
      return fail("Source copy readback mismatch");
    offset += n;
  }
  check.close();
  s.result.sourceReadMs = uint32_t(millis() - started);
  if (s.result.sourceReadMs >= BotSourceCopyBudgetMs)
    return fail("Source copy deadline exceeded");
  s.stagedSize = 0;
  return true;
}
void BotWorker::run() {
  while (!control_->stopping.load()) {
#if defined(MESHCORE_HOST_BOT_SOURCE) && MESHCORE_HOST_BOT_SOURCE
    const auto revision = BotWake::shared().snapshot();
#endif
    sampleBotHttpsMetrics();
    pumpJobs();
#if ONCHIP_BOT_SINGLE_SESSION
    if (storage_->hasIdentity && !control_->ioInitialized) {
#if defined(MESHCORE_HOST_BOT_SOURCE) && MESHCORE_HOST_BOT_SOURCE
      BotWake::shared().wait(revision);
#else
      pauseWorker();
#endif
      continue;
    }
#endif
    if (control_->state.load() != Pending) {
#if defined(MESHCORE_HOST_BOT_SOURCE) && MESHCORE_HOST_BOT_SOURCE
      uint32_t wait = UINT32_MAX;
      const uint32_t now = millis();
      for (const auto &timer : storage_->timers)
        if (timer.used) botEarlier(wait, now, timer.due);
#if ONCHIP_BOT_HTTPS
      for (const auto &io : storage_->network)
        if (io.state->load() == Pending) botEarlier(wait, now, io.request.deadline);
#endif
      for (auto *vm : storage_->sessions()) if (vm->hasPendingIo()) wait = 0;
      BotWake::shared().wait(revision, wait);
#else
      pauseWorker();
#endif
      continue;
    }
    control_->state = Running;
    auto &s = *storage_;
    auto &r = s.result;
    if (r.operation == Operation::Stage || r.operation == Operation::StageFile) {
      s.candidate.clear();
      r.ok = r.operation != Operation::StageFile || loadStagedFile();
#if ONCHIP_BOT_SINGLE_SESSION
      if (r.ok) {
        if (s.active.manifest().commands && s.nextGeneration >= UINT32_MAX - 1) {
          r.ok = false; strcpy(r.error, "Lua runtime epoch exhausted; reboot before replacing source");
        } else {
          control_->sourceSuspended = true;
          s.active.cancel();
          pumpJobs();
          if (s.active.manifest().commands) {
            s.active.clear();
            control_->generation = ++s.nextGeneration;
            for (auto &timer : s.timers) timer = {};
          }
          control_->subscriptions = 0;
        }
      }
#endif
#if ONCHIP_BOT_SEPARATE_DIAGNOSTICS
      if (r.ok && s.hasIdentity && !s.diagnosticsReady) {
        s.diagnosticsReady = s.diagnostics.load(BotDefaultSource, strlen(BotDefaultSource), 1,
                                               r.stats, r.error, sizeof(r.error));
        r.ok = s.diagnosticsReady;
      }
#endif
      if (r.ok) {
        if (s.nextGeneration == UINT32_MAX) {
          r.ok = false; strcpy(r.error, "Source generation capacity exhausted");
        } else {
          s.candidateGeneration = ++s.nextGeneration;
          r.ok = s.candidate.load(s.staged, s.stagedSize, s.candidateGeneration,
                                  r.stats, r.error, sizeof(r.error));
#if ONCHIP_BOT_WASM
          const auto &other = s.candidate.isWasm() ? s.active.manifest() : s.wasmActive.manifest();
          if (r.ok) for (unsigned i = 0; i < s.candidate.manifest().count; ++i)
            if (other.find(s.candidate.manifest().commands[i].name)) {
              r.ok = false;
              snprintf(r.error, sizeof(r.error), "Command !%s is already registered by %s runtime",
                       s.candidate.manifest().commands[i].name, s.candidate.isWasm() ? "Lua" : "Wasm");
              break;
            }
#endif
        }
      }
      // A failed candidate must not leave an older candidate activatable.
      if (!r.ok) {
        s.stagedSize = 0;
#if ONCHIP_BOT_SINGLE_SESSION
        s.candidate.clear();
        if (control_->sourceSuspended && s.retainedSize) {
          BotVmStats recovered;
          char error[96]{};
          if (s.active.load(s.retained, s.retainedSize, control_->generation,
                            recovered, error, sizeof(error))) {
            s.active.setEventEpoch(&control_->eventEpoch);
            control_->subscriptions = s.subscriptions();
            control_->sourceSuspended = false;
          } else {
            snprintf(r.error, sizeof(r.error), "Rejected source; prior Lua reload failed: %.79s", error);
          }
        }
#endif
      }
    } else if (r.operation == Operation::CopyFile) {
      r.ok = copySourceFile();
      s.stagedSize = 0;
      s.candidate.clear();
#if ONCHIP_BOT_SINGLE_SESSION
    } else if (r.operation == Operation::Recover) {
      s.candidate.clear();
      s.stagedSize = 0;
      r.ok = !control_->sourceSuspended;
      if (!r.ok && s.retainedSize) {
        s.active.clear();
        if (s.nextGeneration == UINT32_MAX) {
          strcpy(r.error, "Lua runtime epoch exhausted; reboot before recovering source");
        } else {
          control_->generation = ++s.nextGeneration;
          for (auto &timer : s.timers) timer = {};
          r.ok = s.active.load(s.retained, s.retainedSize, control_->generation,
                               r.stats, r.error, sizeof(r.error));
        }
        if (r.ok) {
          s.active.setEventEpoch(&control_->eventEpoch);
          control_->subscriptions = s.subscriptions();
          control_->sourceSuspended = false;
        }
      }
      if (!r.ok && !r.error[0]) strcpy(r.error, "Prior Lua source unavailable; source retry or reboot required");
#endif
    } else {
      const bool removingWasm = r.operation == Operation::RemoveWasm;
      r.ok = (removingWasm || s.stagedSize != 0) && s.nextGeneration != UINT32_MAX;
      if (r.ok) {
        control_->generation = removingWasm ? ++s.nextGeneration : s.candidateGeneration;
#if ONCHIP_BOT_SINGLE_SESSION
        control_->sourceGeneration = control_->generation.load();
#endif
        for (auto *vm : s.sessions()) vm->cancel();
        pumpJobs();
        for (auto &timer : s.timers) timer = {};
#if ONCHIP_BOT_WASM
        if (removingWasm) s.wasmActive.clear();
        else if (s.candidate.isWasm()) s.wasmActive.swap(s.candidate);
        else s.active.swap(s.candidate);
#else
        s.active.swap(s.candidate);
#endif
#if ONCHIP_BOT_SINGLE_SESSION
        memcpy(s.retained, s.staged, s.stagedSize + 1);
        s.retainedSize = s.stagedSize;
        control_->sourceSuspended = false;
#endif
        s.active.setEventEpoch(&control_->eventEpoch);
#if ONCHIP_BOT_WASM
        s.wasmActive.setEventEpoch(&control_->eventEpoch);
#endif
        control_->subscriptions = s.subscriptions();
        s.candidate.clear();
        s.active.setGeneration(control_->generation);
#if ONCHIP_BOT_WASM
        s.wasmActive.setGeneration(control_->generation);
#endif
#if ONCHIP_BOT_SEPARATE_DIAGNOSTICS
        s.diagnostics.setGeneration(control_->generation);
#endif
        s.stagedSize = 0;
      } else {
        strcpy(r.error, "No validated command source is staged");
      }
    }
#ifdef ARDUINO_ARCH_ESP32
    r.stats.freeInternalBytes = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    r.stats.freePsramBytes = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    r.stats.stackHighWaterBytes = uxTaskGetStackHighWaterMark(nullptr);
#elif defined(NRF52_PLATFORM)
    r.stats.freeInternalBytes = dbgHeapFree();
    r.stats.stackHighWaterBytes = uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t);
#endif
    control_->state = Done;
  }
  for (auto *vm : storage_->sessions()) vm->clear();
  storage_->candidate.clear();
  control_->stopped = true;
}
} // namespace onchip
#endif
