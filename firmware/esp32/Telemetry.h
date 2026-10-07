// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "RadioDashboard.h"
#include "BotHttpsDiagnostics.h"
#include "RepeaterMetrics.h"

namespace onchip {
constexpr size_t TelemetryBodyLimit = 6144;
constexpr unsigned TelemetryRepeaterLimit = 3;
class TelemetryRepeaterCursor {
public:
  unsigned select(unsigned peers, uint64_t successes) {
    if (successes != successes_) cursor_ += count_;
    successes_ = successes;
    cursor_ = peers ? cursor_ % peers : 0;
    count_ = peers < TelemetryRepeaterLimit ? peers : TelemetryRepeaterLimit;
    return cursor_;
  }
private:
  unsigned cursor_ = 0, count_ = 0;
  uint64_t successes_ = 0;
};

struct TelemetryConfig {
  bool enabled = false;
  uint32_t intervalSeconds = 60;
  bool valid() const { return intervalSeconds >= 30 && intervalSeconds <= 86400; }
};
bool loadTelemetryConfig(TelemetryConfig &config);
bool saveTelemetryConfig(const TelemetryConfig &config);

struct TelemetrySample {
  RadioDashboard::Totals totals;
  RadioDashboard::RadioStatus radio;
  uint32_t largestHeap = 0, psramTotal = 0, psramFree = 0, psramMinimum = 0,
           psramLargest = 0;
  // Zero is the native board API's unsupported battery sentinel.
  uint16_t batteryMv = 0;
  float temperatureC;
  struct Role {
    bool available = false, transmitting = false;
    uint8_t queued = 0;
    uint32_t generation = 0, creditMs = 0, rfMs = 0, succeeded = 0, failed = 0;
  } role[RadioDashboard::ROLE_CAPACITY];
  struct Lua {
    bool available = false;
    unsigned jobsInUse = 0;
    uint32_t rejected = 0, vmFailures = 0, replies = 0, busy = 0,
             senderLimited = 0, channelLimited = 0, globalLimited = 0,
             airtimeLimited = 0, eventsQueued = 0, eventsDropped = 0,
             eventsFailed = 0, eventsCompleted = 0;
    bool vmAvailable = false;
    uint32_t peakBytes = 0, instructions = 0, freeInternalBytes = 0,
             freePsramBytes = 0, stackHighWaterBytes = 0;
    uint64_t elapsedUs = 0;
    uint64_t loadUs = 0, initUs = 0, invokeUs = 0, cleanupUs = 0;
  } lua;
  BotRepeaterSnapshot repeaters[TelemetryRepeaterLimit]{};
  TelemetrySample();
};

// Only these finite codes enter status. Transport/remote strings are not metrics.
enum class TelemetryError : uint8_t {
  None, Disabled, Storage, Wifi, Clock, Endpoint, Busy, Snapshot, Encoding,
  Transport, Http, Timeout, Cancelled, Suspended, Heap
};
const char *telemetryErrorName(TelemetryError error);
struct TelemetryCompletion {
  bool ok = false;
  uint16_t httpStatus = 0;
  TelemetryError error = TelemetryError::Transport;
  BotHttpsDiagnostics tls{};
};
class TelemetrySink {
public:
  virtual ~TelemetrySink() = default;
  virtual bool submit(const char *body, size_t size) = 0;
  virtual bool poll(TelemetryCompletion &result) = 0;
  virtual void cancel() = 0;
};
struct TelemetryStatus {
  bool pending = false, suspended = false;
  uint64_t attempts = 0, successes = 0, failures = 0, dropped = 0;
  uint64_t lastAttemptMs = 0, lastSuccessMs = 0, lastFailureMs = 0, lastDropMs = 0, nextMs = 0;
  uint32_t backoffSeconds = 0;
  uint16_t httpStatus = 0;
  TelemetryError error = TelemetryError::Disabled;
  // Last acknowledged worker completion, retained across pending/dropped samples.
  BotHttpsDiagnostics tls{};
  uint64_t tlsAttempt = 0;
};
size_t encodeTelemetry(const TelemetrySample &sample, const char *device,
                       const char *name, const TelemetryStatus &status,
                       char *body, size_t capacity);

class TelemetryPublisher {
public:
  explicit TelemetryPublisher(TelemetrySink &sink) : sink_(sink) {}
  bool begin(uint64_t now);
  // The caller is the authenticated MastAdmin backend, never public Lua.
  void command(const char *text, char *reply, size_t capacity, uint64_t now);
  void configure(const TelemetryConfig &config, uint64_t now);
  void poll(uint64_t now);
  bool due(uint64_t now) const;
  void drop(uint64_t now, TelemetryError reason);
  void publish(uint64_t now, const char *body, size_t size);
  const TelemetryStatus &status() const { return status_; }
  const TelemetryConfig &config() const { return config_; }
private:
  TelemetrySink &sink_;
  TelemetryConfig config_;
  TelemetryStatus status_;
  uint8_t consecutiveFailures_ = 0;
  bool cancelling_ = false;
  bool timedOut_ = false;
  void failed(uint64_t now, TelemetryError error, uint16_t http);
};
} // namespace onchip
