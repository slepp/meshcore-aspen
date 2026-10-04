// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "BotSettings.h"
#include "BotWorker.h"
#include "Clock.h"
#include "LocalRadio.h"
#include "RoleIdentity.h"
#include <helpers/ArduinoHelpers.h>

namespace onchip {
class CommandBot {
  struct Core;
  Core *core_ = nullptr;
  LocalRadio radio_;
  ArduinoMillis millis_;
  RoleClock rtc_;
  HardwareRNG rng_;
  BotWorker worker_;
  RadioDashboard::RoleStatus status_{};
  bool enabled_ = false, adaptivePolicyFault_ = false;
  bool selectedSourcesReady_ = true, sourceStartupBlocked_ = false;
  bool sourceLifecycleFault_ = false;
  char sourceStartupFault_[192]{};
  BotNodeSnapshot node_;
  uint32_t previousMillis_ = 0;
  bool (*diagnosticSink_)(const char *) = nullptr;
  uint32_t diagnosticsQueued_ = 0, diagnosticsDropped_ = 0;
  void diagnostic(const char *format, ...) __attribute__((format(printf, 2, 3)));
  void fault(const char *message);
  friend class MastAdmin;
#if defined(MESHCORE_HOST_BOT_SOURCE) && MESHCORE_HOST_BOT_SOURCE
  friend class HostBotOwner;
#endif
  bool advertiseOwnerZeroHop();

public:
  static const size_t StorageBytes;
#if ONCHIP_BOT_SINGLE_SESSION
  BotWorker &sourceWorker() { return worker_; }
#endif
#ifdef NRF52_PLATFORM
  void configureRadio(const RadioConfig &profile) { radio_.configure(profile); }
  const char *botName() const;
#endif
  void synchronizeTime(uint32_t utc, ClockSource source = ClockSource::Network) {
    rtc_.synchronize(utc, source);
  }
  static constexpr size_t DiagnosticCapacity = 160;
  // Set before begin; the dispatch-thread sink must copy without waiting.
  void setDiagnosticSink(bool (*sink)(const char *)) { diagnosticSink_ = sink; }
  void diagnosticStatus(char *text, size_t capacity) const;
  struct Counters {
    uint32_t malformed = 0, duplicates = 0, rejected = 0, vmFailures = 0,
             replies = 0, traces = 0, observationsDropped = 0;
    uint32_t eventsQueued = 0, eventsDropped = 0, eventsFailed = 0, eventsCompleted = 0;
    uint32_t senderLimited = 0, channelLimited = 0, globalLimited = 0,
             airtimeLimited = 0, busy = 0, notices = 0, noticesSuppressed = 0;
    uint32_t readJittered = 0, readSuppressed = 0;
    BotVmStats lastVm{};
  };
  CommandBot() = default;
  CommandBot(const CommandBot &) = delete;
  CommandBot &operator=(const CommandBot &) = delete;
  ~CommandBot() { stop(); }
#if defined(MESHCORE_HOST_BOT_SOURCE) && MESHCORE_HOST_BOT_SOURCE
  bool beginHost(const RadioConfig &profile, const uint32_t airtime[256], int noiseFloor);
  uint32_t hostWaitMs() const;
  bool hostSourceResultReady() const;
  using OwnerSendSink = void (*)(const uint8_t *, uint8_t, uint8_t, uint8_t);
  using OwnerInboxSink = void (*)(const uint8_t *, uint32_t, const uint8_t *, size_t);
  void setOwnerSinks(OwnerSendSink send, OwnerInboxSink inbox);
  void ownerSend(const uint8_t request[16], const uint8_t recipient[32], const uint8_t *text, size_t size);
  bool hostReceive(const uint8_t *packet, size_t size, float rssi, float snr, bool local) {
    return radio_.received(packet, size, rssi, snr, local);
  }
  bool hostTransmitResult(uint32_t token, uint8_t state, uint8_t reason,
                          uint32_t queueMs, uint32_t rfMs, uint32_t estimateMs, bool hasRfMs) {
    return radio_.completed(token, state, reason, queueMs, rfMs, estimateMs, hasRfMs);
  }
  void hostStatistics(const mesh::QueuedRadioStats &stats) { radio_.updateStats(stats); }
#else
#ifdef NRF52_PLATFORM
  bool begin(nrfmast::RadioPort &port, const RadioConfig &profile);
#else
  bool begin(WifiKissMultiplexer &mux);
#endif
#endif
  void loop();
  void stop();
  void dashboardStatus(RadioDashboard::RoleStatus &status) const;
  // Dispatch-thread snapshot only; no strings from private configuration/faults.
  void nodeSnapshot(BotNodeSnapshot &snapshot) const;
  void setNodeRoles(uint8_t selected, uint8_t ready);
  const uint8_t *publicKey() const;
  const Counters &counters() const;
  unsigned jobsInUse() const { return worker_.jobsInUse(); }
#ifdef MESH_QUEUED_RADIO_API
  bool radioStatistics(mesh::QueuedRadioStats &stats) const {
    return radio_.getQueuedRadioStats(stats);
  }
#endif
  void admissionStatus(char *text, size_t capacity) const;
  void adaptiveCommand(const char *command, char *reply, size_t capacity);
  // Dispatch-thread hooks for the future SAME authenticated RF/web backend.
  // No authentication, persistence, network route or code installer is implied.
  // Poll the result before activating; activation affects subsequent requests.
  bool stageSource(const char *source, size_t size);
  // Reads only BotStagedSourcePath on the worker. The authenticated backend
  // supplies the exact uncompressed size/hash; no codec or transport is involved.
  bool stageSourceFile(size_t expectedSize, const uint8_t expectedSha256[32], uint32_t publication = 0);
  bool activateStaged(uint32_t publication = 0);
  bool removeWasm(uint32_t publication = 0);
  uint32_t reserveSourcePublication(const BotWorker *validator, bool wasm,
                                   char *error, size_t capacity, uint32_t publication = 0) {
    return worker_.reserveSourcePublication(validator, wasm, error, capacity, publication);
  }
  bool sourcePublicationCurrent(uint32_t publication) const { return worker_.sourcePublicationCurrent(publication); }
  void releaseSourcePublication(uint32_t publication) { worker_.releaseSourcePublication(publication); }
  bool pollSourceResult(BotWorker::Result &result);
  bool advertise(bool zeroHop = false);
  bool sourceReady() const;
  bool sourceDeploymentReady() const { return selectedSourcesReady_; }
  void setSourceDeploymentState(bool ready, bool startupBlocked, const char *fault);
  bool retrySourceInitialization();
  void setCommandAdmission(bool enabled);
  bool setSharedState(bool enabled);
  bool sharedState() const;
  bool setHomeAccess(bool enabled);
  bool homeAccess() const;
  bool setDiscovery(bool enabled);
  void discoveryCommand(const char *command, char *reply, size_t capacity);
#if ONCHIP_BOT_HTTPS
  bool ensureNativeHttps() { return worker_.ensureNativeHttps(); }
  bool submitTelemetry(const char *body, size_t size) { return worker_.submitTelemetry(body, size); }
  bool pollTelemetry(TelemetryCompletion &result) { return worker_.pollTelemetry(result); }
  void cancelTelemetry() { worker_.cancelTelemetry(); }
#endif
  // Only the authenticated native owner administration path may call these.
  bool requestOwnerFetch(const char *endpoint) {
    return homeAccess() && worker_.requestOwnerFetch(endpoint);
  }
  bool pollOwnerFetch(BotIoResult &result) { return worker_.pollOwnerFetch(result); }
  void cancelOwnerFetch() { worker_.cancelOwnerFetch(); }
  bool fetchPackage(const BotHttpsFetchRequest &request) { return worker_.fetchPackage(request); }
  bool pollPackageFetch(BotHttpsFetchResult &result) { return worker_.pollPackageFetch(result); }
  void cancelPackageFetch() { worker_.cancelPackageFetch(); }

  bool setForwardPolicy(const BotForwardPolicy &policy);
  bool forwardAccess() const;
  bool setMeshPolicy(const BotMeshPolicy &policy);
  void meshPolicyStatus(char *text, size_t capacity) const;
  void cancelJobs(uint32_t except = 0);
  bool setReminderAccess(bool enabled);
  bool reminderAccess() const;
  bool setEventAccess(uint8_t mask);
  uint8_t eventAccess() const { return worker_.eventAccess(); }
  uint8_t eventMask() const { return worker_.eventMask(); }
  void dataCommand(const char *command, char *reply, size_t capacity) { worker_.dataCommand(command, reply, capacity); }
};
}
