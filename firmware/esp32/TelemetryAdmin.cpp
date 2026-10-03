// SPDX-License-Identifier: Apache-2.0
#include "Telemetry.h"
#include <cstdio>
#include <cstring>
#include <inttypes.h>

namespace onchip {
bool TelemetryPublisher::begin(uint64_t now) {
  TelemetryConfig config;
  const bool ok = loadTelemetryConfig(config);
  configure(config, now);
  if (!ok) status_.error = TelemetryError::Storage;
  return ok;
}
void TelemetryPublisher::command(const char *text, char *reply, size_t capacity, uint64_t now) {
  if (!strcmp(text, "") || !strcmp(text, "status")) {
    snprintf(reply, capacity, "Telemetry on=%u interval=%u pending=%u suspended=%u error=%s http=%u",
             config_.enabled, config_.intervalSeconds, status_.pending, status_.suspended,
             telemetryErrorName(status_.error), status_.httpStatus);
    return;
  }
  if (!strcmp(text, "counts")) {
    snprintf(reply, capacity, "Telemetry attempts=%" PRIu64 " ok=%" PRIu64 " failed=%" PRIu64
             " dropped=%" PRIu64 " backoff=%us",
             status_.attempts, status_.successes, status_.failures, status_.dropped, status_.backoffSeconds);
    return;
  }
  if (!strcmp(text, "times")) {
    snprintf(reply, capacity, "Telemetry uptime-ms attempt=%" PRIu64 " success=%" PRIu64
             " failure=%" PRIu64 " drop=%" PRIu64 " next=%" PRIu64,
             status_.lastAttemptMs, status_.lastSuccessMs, status_.lastFailureMs, status_.lastDropMs,
             config_.enabled && !status_.suspended ? status_.nextMs : 0);
    return;
  }
  const auto &tls = status_.tls;
  if (!strcmp(text, "tls")) {
    snprintf(reply, capacity, "TLS attempt=%" PRIu64 " detail=%s sdk=%" PRId32
             " cipher=%u peer=%u/%u cached=%u",
             status_.tlsAttempt, botHttpsFailureName(tls.point), tls.sdk,
             unsigned(tls.cipher), unsigned(tls.peerCount), unsigned(tls.peerBytes), tls.caCached);
    return;
  }
  if (!strcmp(text, "tls-heap")) {
    snprintf(reply, capacity, "TLS heap before=%u ca=%u connected=%u failure=%u after=%u",
             unsigned(tls.before), unsigned(tls.ca), unsigned(tls.connected),
             unsigned(tls.failure), unsigned(tls.after));
    return;
  }
  if (!strcmp(text, "tls-blocks")) {
    snprintf(reply, capacity, "TLS largest before=%u failure=%u after=%u; global-min before=%u after=%u",
             unsigned(tls.largestBefore), unsigned(tls.largestFailure), unsigned(tls.largestAfter),
             unsigned(tls.globalMinBefore), unsigned(tls.globalMinAfter));
    return;
  }
  if (!strcmp(text, "help")) {
    snprintf(reply, capacity, "telemetry status|counts|times|tls|tls-heap|tls-blocks|on|off|interval 30..86400; telemetry endpoint status|host|address|path|port");
    return;
  }
  TelemetryConfig next = config_;
  if (!strcmp(text, "on") || !strcmp(text, "off")) next.enabled = !strcmp(text, "on");
  else if (!strncmp(text, "interval ", 9)) {
    uint32_t seconds = 0;
    const char *p = text + 9;
    bool valid = *p;
    for (; valid && *p; ++p) {
      if (*p < '0' || *p > '9' || seconds > 86400) valid = false;
      else seconds = seconds * 10 + unsigned(*p - '0');
    }
    if (!valid || seconds < 30 || seconds > 86400) {
      snprintf(reply, capacity, "Error: telemetry interval must be 30..86400 seconds"); return;
    }
    next.intervalSeconds = seconds;
  } else {
    snprintf(reply, capacity, "Error: telemetry help"); return;
  }
  if (!saveTelemetryConfig(next)) {
    TelemetryConfig disabled = config_;
    disabled.enabled = false;
    configure(disabled, now);
    status_.error = TelemetryError::Storage;
    snprintf(reply, capacity, "Error: telemetry commit/readback failed; live publishing disabled, saved outcome unknown");
    return;
  }
  configure(next, now);
  snprintf(reply, capacity, "Saved telemetry on=%u interval=%u; %s", next.enabled, next.intervalSeconds,
           next.enabled ? "latest samples only; inspect telemetry status" : "pending write cancelled");
}
} // namespace onchip
