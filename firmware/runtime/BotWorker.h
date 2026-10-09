// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "BotTypes.h"
#include "BotHttps.h"
#include "BotReminders.h"
#if ONCHIP_BOT_HTTPS
#include "TelemetryEndpoint.h"
#include "NativeServices.h"
#endif
#include <atomic>
#ifdef ARDUINO_ARCH_ESP32
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#elif defined(NRF52_PLATFORM)
#include <Arduino.h>
#else
#include <thread>
#endif

namespace onchip {
constexpr char BotStagedSourcePath[] = "/command-bot/staged.lua";
constexpr uint32_t BotSourceReadBudgetMs = 100;
constexpr uint32_t BotSourceCopyBudgetMs = 2000;

class BotWorker {
public:
  static const size_t StorageBytes;
  enum Runtime : uint8_t { Lua, Wasm, Diagnostics, RuntimeCount };
  enum class Operation : uint8_t { Invoke, Stage, Activate, StageFile, CopyFile, Event, RemoveWasm, Recover };
  struct Result {
    Operation operation = Operation::Invoke;
    bool ok = false;
    BotAction action{};
    BotVmStats stats{};
    uint32_t sourceReadMs = 0;
    uint32_t job = 0, generation = 0;
    uint8_t runtimes = 0;
    uint32_t incarnations[RuntimeCount]{};
    char error[128]{};
  };
  bool begin(const uint8_t botKey[32] = nullptr, BotHttpsTransport *transport = nullptr,
             const BotHttpsConfig *httpsConfig = nullptr, bool networkOnly = false);
  void stop();
  bool busy() const;
#if defined(MESHCORE_HOST_BOT_SOURCE) && MESHCORE_HOST_BOT_SOURCE
  bool ownerWorkPending() const;
  bool resultReady() const;
#endif
  unsigned jobsInUse() const;
  bool canInvoke() const;
  bool canInvoke(const BotEvent &event, unsigned collecting = 0, unsigned customCollecting = 0) const;
  uint32_t generation() const;
  uint32_t sourceGeneration() const;
  uint32_t runtimeGeneration(Runtime runtime) const;
  bool ioCurrent(const BotIoToken &token) const;
  bool resultCurrent(const Result &result) const;
  void setRuntimeAdmission(bool wasm, bool enabled);
  bool runtimeAdmissionEnabled(bool wasm) const;
  void setRuntimeDeploymentBlocked(bool wasm, bool blocked);
  const char *admissionError(const BotEvent &event) const;
  bool setSharedState(bool enabled);
  void setHomeAccess(bool enabled);
  bool setReminderAccess(bool enabled);
  bool reminderAccess() const;
  bool setEventAccess(uint8_t mask);
  // Owner-applied mask, independent of subscriptions and busy state.
  uint8_t eventAccess() const;
  uint8_t eventMask() const;
  uint8_t eventMask(Runtime runtime) const;
  uint32_t scheduleSeconds() const;
  uint32_t eventEpoch() const;
  void setReminderReady(bool ready);
  bool inspectReminder(BotReminderDispatch &dispatch);
  void approveReminder(bool approved);
  bool takeReminder(BotReminderDispatch &dispatch);
  bool reminderCurrent(const BotReminderDispatch &) const;
  bool completeReminder(uint32_t id, BotReminderState outcome);
  bool invoke(const BotEvent &event, uint32_t job = 0);
  bool stage(const char *source, size_t size, uint32_t publication = 0);
  bool stageFile(size_t expectedSize, const uint8_t expectedSha256[32], uint32_t publication = 0);
  bool copyFile(const char *from, const char *to, size_t expectedSize, uint32_t publication = 0);
#if ONCHIP_BOT_SINGLE_SESSION
  bool recoverSource();
  bool sourceSuspended() const;
#endif
  bool activate(uint32_t publication = 0);
  bool removeWasm(uint32_t publication = 0);
  // Dispatch-thread namespace reservation; validation never activates a program.
  uint32_t reserveSourcePublication(const BotWorker *validator, bool wasm,
                                   char *error, size_t capacity, uint32_t publication = 0);
  bool sourcePublicationCurrent(uint32_t publication) const;
  void releaseSourcePublication(uint32_t publication);
  bool poll(Result &result);
  bool pollRadio(BotIoRequest &request);
  bool completeRadio(const BotIoResult &result);
  bool rejectRadio(const BotIoToken &token, const char *reason);
  void cancelJobs(uint32_t except = 0);
#if ONCHIP_BOT_HTTPS
  bool ensureNativeHttps();
  NativeServiceRegistration attachNetworkService(
      NativeNetworkService &service, const NativeServiceBudget &budget);
  bool submitTelemetry(const char *body, size_t size);
  bool pollTelemetry(TelemetryCompletion &result);
  void cancelTelemetry();
#endif
  // Native owner administration must authenticate the caller before enqueueing.
  // Poll or cancel the single bounded configured GET on the existing HTTPS worker.
  bool requestOwnerFetch(const char *endpoint);
  bool pollOwnerFetch(BotIoResult &result);
  void cancelOwnerFetch();
  bool fetchPackage(const BotHttpsFetchRequest &request);
  bool pollPackageFetch(BotHttpsFetchResult &result);
  void cancelPackageFetch();
  // Dispatch-thread entry, called only by the authenticated mast backend.
  void dataCommand(const char *command, char *reply, size_t capacity);
  ~BotWorker() { stop(); }

private:
  enum State { Idle, Pending, Running, Done, Expiring };
  enum ReminderPhase { NoReminder, Offered, Inspecting, Checked, Claimed, Transmitting, Completed };
  struct Storage;
  struct Control;
  Storage *storage_ = nullptr;
  Control *control_ = nullptr;
#if defined(ARDUINO_ARCH_ESP32) || defined(NRF52_PLATFORM)
  TaskHandle_t task_ = nullptr;
  TaskHandle_t ioTask_ = nullptr;
#if ONCHIP_BOT_HTTPS
  TaskHandle_t netTask_ = nullptr;
#endif
#else
  std::thread task_;
  std::thread ioTask_;
#if ONCHIP_BOT_HTTPS
  std::thread netTask_;
#endif
#endif
  static void entry(void *context);
  static void ioEntry(void *context);
  static void netEntry(void *context);
  bool loadStagedFile();
  bool copySourceFile();
  bool sourceMutationAllowed(uint32_t publication) const;
  bool claimSourceMutation(uint32_t publication);
  uint8_t invocationRuntimes(const BotEvent &event) const;
  bool invocationCapacity(const BotEvent &event, unsigned collecting, unsigned customCollecting) const;
  void run();
  void runIo();
  void runNet();
  void pumpJobs();
};
} // namespace onchip
