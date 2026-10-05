// SPDX-License-Identifier: Apache-2.0
#include "Clock.h"
#include "BuildClock.h"
#include "Config.h"
#include <Arduino.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include "RadioTimeProtocol.h"
#include <new>
#if defined(MESHCORE_HOST_BOT_SOURCE) && MESHCORE_HOST_BOT_SOURCE
#include <time.h>
#endif

#ifdef ARDUINO_ARCH_ESP32
#include "EspSntpClock.h"
#include <time.h>
#endif
#ifdef ONCHIP_CLOCK_TEST_BUSY
extern bool onchipClockTestBusy();
#endif

namespace onchip {
namespace {
RoleClock clocks[3];
RoleProfile clockProfile;
bool additionalNetworkRole = false;
std::atomic<uint32_t> sampleVersion{0}, pendingNetworkTime{0}, sampleMillis{0};
std::atomic<uint32_t> pendingNetworkGeneration{0};
uint32_t consumedVersion = 0;
std::atomic_flag publishing = ATOMIC_FLAG_INIT;
ClockSnapshot published;
#if defined(MESHCORE_HOST_BOT_SOURCE) && MESHCORE_HOST_BOT_SOURCE
struct HostClockPublication {
  uint64_t earliestUtcMs = 0, latestUtcMs = 0;
  uint32_t sampledAt = 0;
  uint64_t epoch = 0;
} hostClock;
std::atomic<bool> hostClockTrusted{false};
std::atomic<uint64_t> hostEpoch{1};
#endif
uint32_t networkEpoch = 0, rejectedSamples = 0;
uint32_t networkPreviousMillis = 0;
uint64_t networkElapsedMillis = 0;
uint32_t acceptedNetworkGeneration = 0;
uint32_t gpsEpoch = 0, gpsPreviousMillis = 0;
uint64_t gpsElapsedMillis = 0;
uint32_t networkInterval() {
#ifdef ARDUINO_ARCH_ESP32
  return radio_time::settings.interval;
#else
  return ONCHIP_SNTP_INTERVAL_SECONDS;
#endif
}
bool networkEnabled() {
#ifdef ARDUINO_ARCH_ESP32
  return radio_time::settings.server[0];
#else
  return ONCHIP_SNTP_SERVER[0];
#endif
}
uint32_t networkGeneration() {
#ifdef ARDUINO_ARCH_ESP32
  return radio_time::generation.load();
#else
  return 0;
#endif
}
void receiveNetworkTimeAtGeneration(uint32_t epoch, uint32_t generation) {
  sampleVersion.fetch_add(1);
  pendingNetworkTime.store(epoch ? epoch : 1);
  sampleMillis.store(millis());
  pendingNetworkGeneration.store(generation);
  sampleVersion.fetch_add(1);
}
static_assert(ONCHIP_CLOCK_BUILD_EPOCH >= 1715770351u &&
                  ONCHIP_CLOCK_BUILD_EPOCH <= 4102444800u,
              "Build clock baseline must be between May 2024 and January 2100");

const char *sourceName(ClockSource source) {
  switch (source) {
  case ClockSource::Build:
    return "build";
  case ClockSource::Native:
    return "native";
  case ClockSource::Network:
    return "sntp";
  case ClockSource::Gps:
    return "gps";
  }
  return "unknown";
}
} // namespace

RoleClock::RoleClock(uint32_t baseline)
    : seconds_(baseline), previousMillis_(millis()) {}
void RoleClock::tick() {
  const uint32_t now = millis();
  const uint64_t elapsed =
      uint32_t(now - previousMillis_) + uint64_t(remainder_);
  previousMillis_ = now;
  seconds_ += elapsed / 1000;
  remainder_ = elapsed % 1000;
}
uint32_t RoleClock::getCurrentTime() {
  tick();
  return seconds_;
}
void RoleClock::setCurrentTime(uint32_t seconds) {
  tick();
  if (seconds < seconds_) {
    Serial.println("On-chip clock refused a backward native update");
    return;
  }
  seconds_ = seconds;
  remainder_ = 0;
  source_ = ClockSource::Native;
}
void RoleClock::synchronize(uint32_t seconds, ClockSource source) {
  tick();
  if (seconds >= seconds_) {
    seconds_ = seconds;
    remainder_ = 0;
    source_ = source;
  }
}
RoleClock &repeaterClock() { return clocks[0]; }
RoleClock &roomClock() { return clocks[1]; }
RoleClock &companionClock() { return clocks[2]; }

void beginClocks(RoleProfile profile) {
  clockProfile = profile;
  additionalNetworkRole = false;
  // Boot is the repair boundary: reconstruct RTC unique counters before native
  // ACL loading. Scoped role reboots deliberately do not call this function.
  for (unsigned i = 0; i < 3; ++i) {
    if (!clockProfile.has(static_cast<Role>(i)))
      continue;
    auto &clock = clocks[i];
    clock.~RoleClock();
    new (&clock) RoleClock(ONCHIP_CLOCK_BUILD_EPOCH);
  }
  networkEpoch = rejectedSamples = 0;
  networkElapsedMillis = 0;
  networkPreviousMillis = millis();
  gpsEpoch = 0;
  gpsElapsedMillis = 0;
  gpsPreviousMillis = networkPreviousMillis;
  sampleVersion.store(0);
  consumedVersion = 0;
  pendingNetworkTime.store(0);
  loopClocks();
  Serial.printf(
      "On-chip clocks initialized from build baseline %u; "
      "ESP system RTC and contact timestamps are not time authorities\n",
      unsigned(ONCHIP_CLOCK_BUILD_EPOCH));
}
void receiveNetworkTime(uint32_t epoch) {
  // One SNTP producer; sequence fencing pairs epoch with its receipt time.
  // A reader crossing publication simply defers to the next dispatch pass.
  receiveNetworkTimeAtGeneration(epoch, networkGeneration());
}
void beginNetworkClock(bool additionalRole) {
  additionalNetworkRole = additionalNetworkRole || additionalRole;
#ifdef ARDUINO_ARCH_ESP32
  radio_time::start(receiveNetworkTimeAtGeneration);
#endif
}
bool receiveGpsTime(uint32_t epoch) {
  if (epoch < ONCHIP_CLOCK_BUILD_EPOCH || epoch > 4102444800u) {
    ++rejectedSamples;
    return false;
  }
  gpsEpoch = epoch;
  gpsElapsedMillis = 0;
  gpsPreviousMillis = millis();
  for (unsigned i = 0; i < 3; ++i)
    if (clockProfile.has(static_cast<Role>(i)))
      clocks[i].synchronize(epoch, ClockSource::Gps);
  return true;
}
void loopClocks() {
  const uint32_t now = millis();
  networkElapsedMillis += uint32_t(now - networkPreviousMillis);
  networkPreviousMillis = now;
  gpsElapsedMillis += uint32_t(now - gpsPreviousMillis);
  gpsPreviousMillis = now;
  if (gpsElapsedMillis >= 3600000u) gpsEpoch = 0;
  const uint32_t version = sampleVersion.load();
  const uint32_t sample = pendingNetworkTime.load();
  const uint32_t receivedAt = sampleMillis.load();
  const uint32_t receivedGeneration = pendingNetworkGeneration.load();
  if (acceptedNetworkGeneration != networkGeneration()) networkEpoch = 0;
  if (!(version & 1) && version != consumedVersion &&
      version == sampleVersion.load()) {
    consumedVersion = version;
    if (receivedGeneration != networkGeneration()) {
      // A server change cannot renew trust from an in-flight old callback.
    } else if (sample < ONCHIP_CLOCK_BUILD_EPOCH || sample > 4102444800u) {
      ++rejectedSamples;
      Serial.println("On-chip SNTP sample outside build/2100 clock bounds");
    } else {
      networkEpoch = sample;
      acceptedNetworkGeneration = receivedGeneration;
      networkPreviousMillis = millis();
      networkElapsedMillis = uint32_t(networkPreviousMillis - receivedAt);
      for (unsigned i = 0; i < 3; ++i)
        if (clockProfile.has(static_cast<Role>(i)) && !gpsEpoch)
          clocks[i].synchronize(sample + networkElapsedMillis / 1000);
    }
  }
  ClockSnapshot snapshot;
  snapshot.sampled_at_ms = now;
  snapshot.build_epoch = ONCHIP_CLOCK_BUILD_EPOCH;
  snapshot.network_enabled =
      networkEnabled() &&
      (additionalNetworkRole ||
       (clockProfile.enabled &
        (RoleProfile::Repeater | RoleProfile::Room | RoleProfile::Companion)));
  snapshot.network_epoch = networkEpoch;
  snapshot.network_interval_seconds = networkInterval();
  snapshot.network_age_seconds = networkElapsedMillis / 1000;
  snapshot.network_age_ms = networkElapsedMillis;
  snapshot.gps_epoch = gpsEpoch;
  snapshot.gps_age_ms = gpsElapsedMillis;
  snapshot.rejected_samples = rejectedSamples;
  const uint64_t networkNow = networkEpoch + networkElapsedMillis / 1000;
  for (unsigned i = 0; i < 3; ++i) {
    auto &status = snapshot.roles[i];
    status.enabled = clockProfile.has(static_cast<Role>(i));
    if (!status.enabled)
      continue;
    status.epoch = clocks[i].getCurrentTime();
    status.source = clocks[i].source();
    const bool gps = status.source == ClockSource::Gps;
    const uint64_t sampleNow = gps ? gpsEpoch + gpsElapsedMillis / 1000 : networkNow;
    const int64_t difference = int64_t(status.epoch) - int64_t(sampleNow);
    status.synchronized =
        (gps ? gpsEpoch != 0 : networkEpoch != 0) && difference >= -2 && difference <= 2 &&
        (gps ? gpsElapsedMillis < 3600000u :
               snapshot.network_age_seconds <= snapshot.network_interval_seconds * 2u);
  }
  if (!publishing.test_and_set(std::memory_order_acquire)) {
    published = snapshot;
    publishing.clear(std::memory_order_release);
  }
}
bool clockSnapshot(ClockSnapshot &snapshot) {
#ifdef ONCHIP_CLOCK_TEST_BUSY
  if (onchipClockTestBusy()) return false;
#endif
  if (publishing.test_and_set(std::memory_order_acquire))
    return false;
  snapshot = published;
  publishing.clear(std::memory_order_release);
  return true;
}
#if defined(MESHCORE_HOST_BOT_SOURCE) && MESHCORE_HOST_BOT_SOURCE
uint64_t hostClockEpoch() { return hostEpoch.load(); }
void revokeHostClock() {
  hostClockTrusted.store(false);
  hostEpoch.fetch_add(1);
}
void publishHostClock(uint64_t earliestUtcMs, uint64_t latestUtcMs, uint64_t sampledMonotonicNs, uint64_t epoch) {
  const bool validBounds = earliestUtcMs >= uint64_t(ONCHIP_CLOCK_BUILD_EPOCH) * 1000 &&
      latestUtcMs >= earliestUtcMs && latestUtcMs <= 4102444800000ULL &&
      latestUtcMs - earliestUtcMs <= 5000;
  timespec before{}, after{};
  const bool capturedBefore = clock_gettime(CLOCK_MONOTONIC, &before) == 0;
  const uint32_t sampledAt = millis();
  const bool capturedAfter = clock_gettime(CLOCK_MONOTONIC, &after) == 0;
  const uint64_t beforeNs = uint64_t(before.tv_sec) * 1000000000 + uint64_t(before.tv_nsec);
  const uint64_t afterNs = uint64_t(after.tv_sec) * 1000000000 + uint64_t(after.tv_nsec);
  if (!validBounds || epoch != hostEpoch.load() ||
      !capturedBefore || !capturedAfter || !sampledMonotonicNs ||
      beforeNs < sampledMonotonicNs || afterNs < beforeNs ||
      afterNs - sampledMonotonicNs > 3000000000ULL) {
    earliestUtcMs = latestUtcMs = 0;
  } else {
    earliestUtcMs += (beforeNs - sampledMonotonicNs) / 1000000;
    latestUtcMs += (afterNs - sampledMonotonicNs + 999999) / 1000000;
  }
  if (earliestUtcMs < uint64_t(ONCHIP_CLOCK_BUILD_EPOCH) * 1000 ||
      latestUtcMs < earliestUtcMs || latestUtcMs > 4102444800000ULL ||
      latestUtcMs - earliestUtcMs > 5000)
    earliestUtcMs = latestUtcMs = 0;
  if (!earliestUtcMs) revokeHostClock();
  // Revoked clock trust stays denied even if a reader delays the snapshot write.
  if (publishing.test_and_set(std::memory_order_acquire)) return;
  hostClock = {earliestUtcMs, latestUtcMs, sampledAt, epoch};
  published = {};
  published.sampled_at_ms = hostClock.sampledAt;
  published.build_epoch = ONCHIP_CLOCK_BUILD_EPOCH;
  published.network_enabled = earliestUtcMs != 0;
  published.network_epoch = uint32_t(earliestUtcMs / 1000);
  hostClockTrusted.store(earliestUtcMs != 0 && epoch == hostEpoch.load());
  publishing.clear(std::memory_order_release);
}
#endif
bool trustedNetworkTime(uint32_t &earliest, uint32_t &latest, const char **reason) {
  const auto denied = [&](const char *message) {
    if (reason) *reason = message;
    return false;
  };
#if defined(MESHCORE_HOST_BOT_SOURCE) && MESHCORE_HOST_BOT_SOURCE
  HostClockPublication sample;
  bool captured = false;
  for (unsigned attempt = 0; attempt < 4; ++attempt) {
    if (!publishing.test_and_set(std::memory_order_acquire)) {
      sample = hostClock;
      publishing.clear(std::memory_order_release);
      captured = true;
      break;
    }
    if (attempt < 3) delay(1);
  }
  if (!captured) return denied("Host scheduler clock publication busy");
  if (!hostClockTrusted.load() || !sample.earliestUtcMs || sample.epoch != hostEpoch.load())
    return denied("Host scheduler clock unsynchronized, discontinuous or outside UTC error bounds; inspect clock status");
  const uint32_t lag = uint32_t(millis() - sample.sampledAt);
  if (lag > 3000) return denied("Host scheduler clock publication stale");
  const uint64_t lower = (sample.earliestUtcMs + lag) / 1000;
  const uint64_t upper = (sample.latestUtcMs + lag + 999) / 1000;
  if (lower < ONCHIP_CLOCK_BUILD_EPOCH || upper > 4102444800u)
    return denied("Host scheduler UTC bounds unavailable");
  earliest = uint32_t(lower);
  latest = uint32_t(upper);
  if (reason) *reason = nullptr;
  return true;
#else
  ClockSnapshot snapshot;
  bool captured = false;
  for (unsigned attempt = 0; attempt < 4; ++attempt) {
    if (clockSnapshot(snapshot)) { captured = true; break; }
    if (attempt < 3) delay(1);
  }
  if (!captured) return denied("UTC publication busy");
  const uint32_t lag = uint32_t(millis() - snapshot.sampled_at_ms);
  const uint64_t gpsAge = snapshot.gps_age_ms + lag;
  const bool gps = snapshot.gps_epoch && gpsAge < 3600000u;
  const uint64_t age = gps ? gpsAge : snapshot.network_age_ms + lag;
  if (!gps && (!snapshot.network_enabled || !snapshot.network_epoch))
    return denied("UTC unsynchronized; obtain GPS fix or SNTP; build/native/system RTC is not trusted");
  if (lag > 3000) return denied("UTC publication stale");
  if (!gps && age > uint64_t(snapshot.network_interval_seconds) * 2000)
    return denied("SNTP sample expired");
  const uint64_t current = uint64_t(gps ? snapshot.gps_epoch : snapshot.network_epoch) + age / 1000;
  const uint32_t uncertainty = gps ? 2 + age / 600000 : 0;
  if (current < snapshot.build_epoch || current + (gps ? uncertainty : 1) > 4102444800u)
    return denied("UTC epoch out of bounds");
  earliest = uint32_t(current) - uncertainty;
  latest = uint32_t(current) + (gps ? uncertainty : 1);
  if (reason) *reason = nullptr;
  return true;
#endif
}
bool networkClockCommand(const char *command, char *reply, size_t capacity, bool writeAllowed) {
  if (!strcmp(command, "get sntp.current")) {
#if defined(MESHCORE_HOST_BOT_SOURCE) && MESHCORE_HOST_BOT_SOURCE
    snprintf(reply, capacity, "Error: radio SNTP/GPS authority unavailable on this host");
#else
    uint32_t lower = 0, upper = 0;
    const char *reason = nullptr;
    if (!trustedNetworkTime(lower, upper, &reason))
      snprintf(reply, capacity, "Error: %s", reason);
    else {
      ClockSnapshot snapshot;
      if (!clockSnapshot(snapshot)) snprintf(reply, capacity, "Error: UTC publication busy");
      else snprintf(reply, capacity, "utc=%u..%u source=%s age=%llu",
          unsigned(lower), unsigned(upper), snapshot.gps_epoch && snapshot.gps_age_ms < 3600000u ? "gps" : "sntp",
          (unsigned long long)((snapshot.gps_epoch && snapshot.gps_age_ms < 3600000u ?
              snapshot.gps_age_ms : snapshot.network_age_ms) / 1000));
    }
#endif
    return true;
  }
#ifdef ARDUINO_ARCH_ESP32
  if (radio_time::configCommand(command, reply, capacity, writeAllowed)) {
    if (!strncmp(command, "set sntp.", 9) && !strncmp(reply, "OK ", 3)) {
      networkEpoch = 0;
      consumedVersion = sampleVersion.load();
      loopClocks();
    }
    return true;
  }
#endif
  return false;
}
bool networkTimeReply(const uint8_t *request, size_t size, uint8_t *reply) {
  if (!radio_time::request(request, size)) return false;
#if defined(MESHCORE_HOST_BOT_SOURCE) && MESHCORE_HOST_BOT_SOURCE
  radio_time::response(request, reply, 0, 0, 0, 0, 0);
  return true;
#else
  uint32_t lower = 0, upper = 0;
  ClockSnapshot snapshot;
  if (!trustedNetworkTime(lower, upper) || !clockSnapshot(snapshot)) {
    radio_time::response(request, reply, 0, 0, 0, 0, 0);
    return true;
  }
  const uint32_t lag = uint32_t(millis() - snapshot.sampled_at_ms);
  const bool gps = snapshot.gps_epoch && snapshot.gps_age_ms + lag < 3600000u;
  const uint64_t age = (gps ? snapshot.gps_age_ms : snapshot.network_age_ms) + lag;
  const uint64_t lifetime = gps ? 3600000u : uint64_t(snapshot.network_interval_seconds) * 2000;
  const uint32_t drift = gps ? 0 : 1 + (age + 599999u) / 600000;
  if (lag > 3000 || age >= lifetime || lower < 1715770351u + drift ||
      uint64_t(upper) + drift > 4102444800u)
    radio_time::response(request, reply, 0, 0, 0, 0, 0);
  else radio_time::response(request, reply, gps ? 2 : 1, lower - drift, upper + drift,
                             age / 1000, lifetime - age);
  return true;
#endif
}
bool publicTimeCommand(const uint8_t *data, size_t size) {
  static constexpr char Command[] = "get sntp.current";
  if (size < 5 + sizeof(Command) - 1 || (data[4] >> 2) != 1 ||
      memcmp(data + 5, Command, sizeof(Command) - 1)) return false;
  for (size_t i = 5 + sizeof(Command) - 1; i < size; ++i)
    if (data[i]) return false;
  return true;
}
size_t formatClockJSON(const ClockSnapshot &s, char *output, size_t capacity) {
  const int length = snprintf(
      output, capacity,
      "{\"sampled_at_ms\":%u,\"build_epoch\":%u,\"sntp_enabled\":%s,\"last_"
      "sntp_epoch\":%u,"
      "\"sntp_age_seconds\":%u,\"gps_epoch\":%u,\"gps_age_seconds\":%llu,\"rejected_samples\":%u,\"roles\":["
      "{\"role\":\"repeater\",\"epoch\":%u,\"source\":\"%s\",\"synchronized\":%"
      "s,\"enabled\":%s},"
      "{\"role\":\"room\",\"epoch\":%u,\"source\":\"%s\",\"synchronized\":%s,\"enabled\":%s},"
      "{\"role\":\"companion\",\"epoch\":%u,\"source\":\"%s\",\"synchronized\":"
      "%s,\"enabled\":%s}]}",
      s.sampled_at_ms, s.build_epoch, s.network_enabled ? "true" : "false",
      s.network_epoch, s.network_age_seconds, s.gps_epoch,
      (unsigned long long)(s.gps_age_ms / 1000), s.rejected_samples,
      s.roles[0].epoch, sourceName(s.roles[0].source),
      s.roles[0].synchronized ? "true" : "false",
      s.roles[0].enabled ? "true" : "false", s.roles[1].epoch,
      sourceName(s.roles[1].source), s.roles[1].synchronized ? "true" : "false",
      s.roles[1].enabled ? "true" : "false",
      s.roles[2].epoch, sourceName(s.roles[2].source),
      s.roles[2].synchronized ? "true" : "false",
      s.roles[2].enabled ? "true" : "false");
  return length > 0 && size_t(length) < capacity ? size_t(length) : 0;
}
#ifdef ARDUINO_ARCH_ESP32
esp_err_t registerClockHTTP(httpd_handle_t server) {
  httpd_uri_t route{};
  route.uri = "/api/clock";
  route.method = HTTP_GET;
  route.handler = [](httpd_req_t *request) {
    ClockSnapshot snapshot;
    char json[768];
    if (!clockSnapshot(snapshot) ||
        uint32_t(millis() - snapshot.sampled_at_ms) > 3000) {
      httpd_resp_set_status(request, "503 Service Unavailable");
      httpd_resp_set_hdr(request, "Retry-After", "1");
      return httpd_resp_sendstr(request, "Clock snapshot unavailable or stale");
    }
    const size_t length = formatClockJSON(snapshot, json, sizeof(json));
    if (!length) {
      httpd_resp_set_status(request, "500 Internal Server Error");
      return httpd_resp_sendstr(request, "Clock snapshot overflow");
    }
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, json, length);
  };
  return httpd_register_uri_handler(server, &route);
}
#endif
} // namespace onchip
