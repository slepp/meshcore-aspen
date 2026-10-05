// SPDX-License-Identifier: Apache-2.0
#include "Telemetry.h"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <inttypes.h>

namespace onchip {
TelemetrySample::TelemetrySample() : temperatureC(NAN) {}
const char *telemetryErrorName(TelemetryError error) {
  static const char *const names[] = {"none", "disabled", "storage", "wifi", "clock",
      "endpoint", "busy", "snapshot", "encoding", "transport", "http", "timeout",
      "cancelled", "suspended", "heap"};
  const auto index = unsigned(error);
  return index < sizeof(names) / sizeof(*names) ? names[index] : "unknown";
}
namespace {
class Line {
  char *out_;
  size_t capacity_, length_ = 0;
  bool ok_ = true;
public:
  Line(char *out, size_t capacity) : out_(out), capacity_(capacity) {}
  void append(const char *format, ...) {
    if (!ok_ || !capacity_) { ok_ = false; return; }
    va_list args;
    va_start(args, format);
    const int count = vsnprintf(out_ + length_, capacity_ - length_, format, args);
    va_end(args);
    if (count < 0 || size_t(count) >= capacity_ - length_) ok_ = false;
    else length_ += count;
  }
  void tag(const char *text, size_t limit) {
    if (!text || !*text || strnlen(text, limit + 1) > limit) { ok_ = false; return; }
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(text); *p; ++p) {
      if (*p < 32 || *p == 127) { ok_ = false; return; }
      if (*p == ' ' || *p == ',' || *p == '=' || *p == '\\') append("\\");
      append("%c", *p);
    }
  }
  void measurement(const char *measurement, const char *device) {
    append("%s,device=", measurement); tag(device, 32);
  }
  size_t finish() {
    if (!ok_ && capacity_) out_[0] = 0;
    return ok_ ? length_ : 0;
  }
};
}
size_t encodeTelemetry(const TelemetrySample &s, const char *device, const char *name,
                       const TelemetryStatus &status, char *body, size_t capacity) {
  Line out(body, capacity);
  const auto &t = s.totals;
  const auto &r = s.radio;
  out.measurement("meshcore_info", device);
  out.append(",name="); out.tag(name, 31);
  out.append(" schema=1i\n");
  out.measurement("meshcore_device", device);
  out.append(" uptime_seconds=%.3f,heap_free_bytes=%ui,heap_minimum_bytes=%ui,"
             "heap_largest_bytes=%ui,dma_free_bytes=%ui,dma_minimum_bytes=%ui,"
             "dma_largest_bytes=%ui,wifi_connected=%ui",
             double(t.uptime_ms) / 1000, r.free_heap, r.minimum_heap, s.largestHeap,
             r.dma_free_heap, r.dma_minimum_heap, r.dma_largest_heap, unsigned(r.wifi_connected));
  if (r.wifi_connected)
    out.append(",wifi_rssi_dbm=%di,wifi_uptime_seconds=%.3f", r.wifi_rssi, double(t.wifi_uptime_ms) / 1000);
  if (s.psramTotal)
    out.append(",psram_total_bytes=%ui,psram_free_bytes=%ui,psram_minimum_bytes=%ui,psram_largest_bytes=%ui",
               s.psramTotal, s.psramFree, s.psramMinimum, s.psramLargest);
  if (s.batteryMv) out.append(",battery_volts=%.3f", double(s.batteryMv) / 1000);
  if (std::isfinite(s.temperatureC)) out.append(",mcu_temperature_celsius=%.3f", double(s.temperatureC));
  out.append("\n");
  out.measurement("meshcore_radio", device);
  out.append(" rx_packets_total=%" PRIu64 "i,rx_errors_total=%ui,rx_estimated_seconds_total=%.3f,"
             "tx_accepted_total=%" PRIu64 "i,tx_rejected_total=%" PRIu64 "i,"
             "tx_succeeded_total=%" PRIu64 "i,tx_failed_total=%" PRIu64 "i,"
             "tx_unknown_total=%" PRIu64 "i,tx_rf_seconds_total=%.3f,"
             "queued=%ui,transmitting=%ui,carrier_wait=%ui,fault=%ui,credit_seconds=%.3f",
             t.rx_packets, r.rx_errors, double(t.rx_estimated_ms) / 1000,
             t.tx_accepted, t.tx_rejected, t.tx_succeeded, t.tx_failed, t.tx_unknown,
             double(t.tx_rf_ms) / 1000, unsigned(r.queued), unsigned(r.transmitting),
             unsigned(r.carrier_wait), unsigned(r.fault), double(r.credit_ms) / 1000);
  if (r.generation)
    out.append(",frequency_hz=%ui,bandwidth_hz=%ui,spreading_factor=%ui,coding_rate=%ui,tx_power_dbm=%di",
               queued_tx::get32(r.profile), queued_tx::get32(r.profile + 4),
               unsigned(r.profile[8]), unsigned(r.profile[9]), int(int8_t(r.profile[10])));
  out.append("\n");
#ifdef MESHCORE_ONCHIP
  // Role labels are fixed firmware slots, never user strings or identities.
  static const char *const roles[] = {"repeater", "room", "companion", "observer",
                                     "bot", "management", "command-bot"};
  for (size_t i = 0; i < std::min<size_t>(r.role_count, RadioDashboard::ROLE_CAPACITY); ++i) {
    out.measurement("meshcore_role", device);
    out.append(",role=%s ready=%ui,identity_present=%ui", roles[i],
               unsigned(r.roles[i].ready), unsigned(r.roles[i].has_identity));
    const auto &role = s.role[i];
    if (role.available)
      out.append(",source_generation=%ui,queued=%ui,transmitting=%ui,credit_seconds=%.3f,"
                 "tx_rf_seconds_total=%.3f,tx_succeeded_total=%ui,tx_failed_total=%ui",
                 role.generation, unsigned(role.queued), unsigned(role.transmitting),
                 double(role.creditMs) / 1000, double(role.rfMs) / 1000, role.succeeded, role.failed);
    out.append("\n");
  }
#endif
  const auto &lua = s.lua;
  if (lua.available) {
    out.measurement("meshcore_lua", device);
    out.append(" jobs_in_use=%ui,rejected_total=%ui,vm_failures_total=%ui,replies_total=%ui,busy_total=%ui,"
               "sender_limited_total=%ui,channel_limited_total=%ui,global_limited_total=%ui,"
               "airtime_limited_total=%ui,events_queued_total=%ui,events_dropped_total=%ui,"
               "events_failed_total=%ui,events_completed_total=%ui",
               lua.jobsInUse, lua.rejected, lua.vmFailures, lua.replies, lua.busy, lua.senderLimited,
               lua.channelLimited, lua.globalLimited, lua.airtimeLimited,
               lua.eventsQueued, lua.eventsDropped, lua.eventsFailed, lua.eventsCompleted);
    if (lua.vmAvailable)
      out.append(",last_vm_peak_bytes=%ui,last_vm_instructions=%ui,last_vm_seconds=%.6f,"
                 "last_vm_load_seconds=%.6f,last_vm_init_seconds=%.6f,"
                 "last_vm_invoke_seconds=%.6f,last_vm_cleanup_seconds=%.6f,"
                 "last_vm_free_internal_bytes=%ui,last_vm_free_psram_bytes=%ui,last_vm_stack_free_bytes=%ui",
                 lua.peakBytes, lua.instructions, double(lua.elapsedUs) / 1000000,
                 double(lua.loadUs) / 1000000, double(lua.initUs) / 1000000,
                 double(lua.invokeUs) / 1000000, double(lua.cleanupUs) / 1000000,
                 lua.freeInternalBytes, lua.freePsramBytes, lua.stackHighWaterBytes);
    out.append("\n");
  }
  out.measurement("meshcore_publisher", device);
  out.append(" attempts_total=%" PRIu64 "i,successes_total=%" PRIu64 "i,failures_total=%" PRIu64
             "i,dropped_total=%" PRIu64 "i\n", status.attempts, status.successes, status.failures, status.dropped);
  for (const auto &peer : s.repeaters) if (peer.configured) {
    out.measurement("meshcore_repeater", device);
    out.append(",peer=");
    out.tag(peer.alias, 16);
    out.append(" available=%ui,fresh=%ui,error_code=%ui,attempts_total=%ui,failures_total=%ui",
               unsigned(peer.available), unsigned(peer.fresh), unsigned(peer.error), peer.attempts, peer.failures);
    if (peer.available) out.append(",sample_age_seconds=%ui", peer.ageSeconds);
    if (peer.available && peer.fresh) {
      const auto &v = peer.stats;
      if (v.batteryMv) out.append(",battery_volts=%.3f", double(v.batteryMv) / 1000);
      out.append(",queued_packets=%ui,uptime_seconds=%ui,rx_packets_total=%ui,tx_packets_total=%ui,"
                 "tx_airtime_seconds_total=%ui,rx_airtime_seconds_total=%ui,"
                 "tx_flood_total=%ui,tx_direct_total=%ui,rx_flood_total=%ui,rx_direct_total=%ui,"
                 "error_flags=%ui,direct_duplicates_total=%ui,flood_duplicates_total=%ui,rx_errors_total=%ui,"
                 "noise_dbm=%di,last_rssi_dbm=%di,last_snr_db=%.2f",
                 unsigned(v.queued), v.uptimeSeconds, v.received, v.sent, v.txSeconds, v.rxSeconds,
                 v.sentFlood, v.sentDirect, v.receivedFlood, v.receivedDirect, unsigned(v.errors),
                 unsigned(v.directDuplicates), unsigned(v.floodDuplicates), v.receiveErrors,
                 int(v.noise), int(v.rssi), double(v.snrQuarterDb) / 4);
    }
    out.append("\n");
  }
  return out.finish();
}
void TelemetryPublisher::configure(const TelemetryConfig &config, uint64_t now) {
  if (status_.pending) { sink_.cancel(); cancelling_ = true; }
  timedOut_ = false;
  config_ = config;
  if (!config_.valid()) config_.enabled = false;
  status_.suspended = false;
  consecutiveFailures_ = 0;
  status_.backoffSeconds = 0;
  status_.nextMs = now + uint64_t(config_.intervalSeconds) * 1000;
  status_.error = config_.enabled ? TelemetryError::None : TelemetryError::Disabled;
}
bool TelemetryPublisher::due(uint64_t now) const {
  return config_.enabled && !status_.pending && !status_.suspended && now >= status_.nextMs;
}
void TelemetryPublisher::drop(uint64_t now, TelemetryError reason) {
  ++status_.dropped;
  status_.lastDropMs = now;
  status_.error = reason;
  status_.nextMs = now + uint64_t(std::max(config_.intervalSeconds, status_.backoffSeconds)) * 1000;
}
void TelemetryPublisher::failed(uint64_t now, TelemetryError error, uint16_t http) {
  ++status_.failures;
  status_.lastFailureMs = now;
  status_.error = error;
  status_.httpStatus = http;
  consecutiveFailures_ = std::min<unsigned>(consecutiveFailures_ + 1, 7);
  status_.backoffSeconds = std::max(config_.intervalSeconds,
      std::min<uint32_t>(3600, 30u << consecutiveFailures_));
  status_.nextMs = now + uint64_t(status_.backoffSeconds) * 1000;
  // These are configuration/payload failures, not a reason to hammer a server.
  status_.suspended = http >= 300 && http < 500 && http != 408 && http != 429;
}
void TelemetryPublisher::poll(uint64_t now) {
  if (!status_.pending) return;
  TelemetryCompletion result;
  if (sink_.poll(result)) {
    status_.pending = false;
    status_.httpStatus = result.httpStatus;
    status_.tls = result.tls;
    status_.tlsAttempt = status_.attempts;
    if (cancelling_) {
      cancelling_ = false;
      if (result.ok && result.httpStatus >= 200 && result.httpStatus < 300) {
        ++status_.successes;
        status_.lastSuccessMs = now;
      } else {
        ++status_.dropped;
        status_.lastDropMs = now;
      }
      status_.error = !config_.enabled ? TelemetryError::Disabled :
                      timedOut_ ? TelemetryError::Timeout : TelemetryError::Cancelled;
      timedOut_ = false;
    } else if (result.ok && result.httpStatus >= 200 && result.httpStatus < 300) {
      ++status_.successes;
      status_.lastSuccessMs = now;
      status_.error = TelemetryError::None;
      status_.backoffSeconds = consecutiveFailures_ = 0;
      status_.nextMs = now + uint64_t(config_.intervalSeconds) * 1000;
    } else {
      failed(now, result.httpStatus ? TelemetryError::Http :
                  result.error == TelemetryError::None ? TelemetryError::Transport : result.error,
             result.httpStatus);
    }
  } else if (!cancelling_ && now - status_.lastAttemptMs >= 35000) {
    // Keep ownership until worker acknowledges cancellation; never reuse its slot.
    sink_.cancel();
    cancelling_ = true;
    timedOut_ = true;
    failed(now, TelemetryError::Timeout, 0);
  }
}
void TelemetryPublisher::publish(uint64_t now, const char *body, size_t size) {
  if (!due(now)) return;
  if (!size || size > TelemetryBodyLimit) { drop(now, TelemetryError::Encoding); return; }
  if (!sink_.submit(body, size)) { drop(now, TelemetryError::Busy); return; }
  ++status_.attempts;
  status_.lastAttemptMs = now;
  status_.pending = true;
  status_.httpStatus = 0;
  status_.error = TelemetryError::None;
}
} // namespace onchip
