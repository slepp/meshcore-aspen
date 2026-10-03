// SPDX-License-Identifier: Apache-2.0
#include "Clock.h"
#include "BuildClock.h"
#include "Config.h"
#include <Arduino.h>
#include <atomic>
#include <cstdio>
#include <new>
#if defined(MESHCORE_HOST_BOT_SOURCE) && MESHCORE_HOST_BOT_SOURCE
#include <time.h>
#endif

#ifdef ARDUINO_ARCH_ESP32
#include <esp_sntp.h>
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
void RoleClock::synchronize(uint32_t seconds) {
  tick();
  if (seconds >= seconds_) {
    seconds_ = seconds;
    remainder_ = 0;
    source_ = ClockSource::Network;
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
  sampleVersion.fetch_add(1);
  pendingNetworkTime.store(epoch ? epoch : 1);
  sampleMillis.store(millis());
  sampleVersion.fetch_add(1);
}
void beginNetworkClock(bool additionalRole) {
  additionalNetworkRole = additionalNetworkRole || additionalRole;
#ifdef ARDUINO_ARCH_ESP32
  if (!ONCHIP_SNTP_SERVER[0])
    return;
  sntp_set_time_sync_notification_cb([](struct timeval *time) {
    receiveNetworkTime(time->tv_sec > 0 && uint64_t(time->tv_sec) <= UINT32_MAX
                           ? uint32_t(time->tv_sec)
                           : 0);
  });
  sntp_set_sync_interval(ONCHIP_SNTP_INTERVAL_SECONDS * 1000u);
  configTime(0, 0, ONCHIP_SNTP_SERVER);
#endif
}
void loopClocks() {
  const uint32_t now = millis();
  networkElapsedMillis += uint32_t(now - networkPreviousMillis);
  networkPreviousMillis = now;
  const uint32_t version = sampleVersion.load();
  const uint32_t sample = pendingNetworkTime.load();
  const uint32_t receivedAt = sampleMillis.load();
  if (!(version & 1) && version != consumedVersion &&
      version == sampleVersion.load()) {
    consumedVersion = version;
    if (sample < ONCHIP_CLOCK_BUILD_EPOCH || sample > 4102444800u) {
      ++rejectedSamples;
      Serial.println("On-chip SNTP sample outside build/2100 clock bounds");
    } else {
      networkEpoch = sample;
      networkPreviousMillis = millis();
      networkElapsedMillis = uint32_t(networkPreviousMillis - receivedAt);
      for (unsigned i = 0; i < 3; ++i)
        if (clockProfile.has(static_cast<Role>(i)))
          clocks[i].synchronize(sample + networkElapsedMillis / 1000);
    }
  }
  ClockSnapshot snapshot;
  snapshot.sampled_at_ms = now;
  snapshot.build_epoch = ONCHIP_CLOCK_BUILD_EPOCH;
  snapshot.network_enabled =
      ONCHIP_SNTP_SERVER[0] &&
      (additionalNetworkRole ||
       (clockProfile.enabled &
        (RoleProfile::Repeater | RoleProfile::Room | RoleProfile::Companion)));
  snapshot.network_epoch = networkEpoch;
  snapshot.network_age_seconds = networkElapsedMillis / 1000;
  snapshot.network_age_ms = networkElapsedMillis;
  snapshot.rejected_samples = rejectedSamples;
  const uint64_t networkNow = networkEpoch + networkElapsedMillis / 1000;
  for (unsigned i = 0; i < 3; ++i) {
    auto &status = snapshot.roles[i];
    status.enabled = clockProfile.has(static_cast<Role>(i));
    if (!status.enabled)
      continue;
    status.epoch = clocks[i].getCurrentTime();
    status.source = clocks[i].source();
    const int64_t difference = int64_t(status.epoch) - int64_t(networkNow);
    status.synchronized =
        networkEpoch && difference >= -2 && difference <= 2 &&
        snapshot.network_age_seconds <= ONCHIP_SNTP_INTERVAL_SECONDS * 2u;
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
  if (!captured) return denied("SNTP publication busy");
  const uint32_t lag = uint32_t(millis() - snapshot.sampled_at_ms);
  const uint64_t age = snapshot.network_age_ms + lag;
  if (!snapshot.network_enabled || !snapshot.network_epoch)
    return denied("SNTP unsynchronized; build/native/system RTC is not trusted");
  if (lag > 3000) return denied("SNTP publication stale");
  if (age > uint64_t(ONCHIP_SNTP_INTERVAL_SECONDS) * 2000)
    return denied("SNTP sample expired");
  const uint64_t current = uint64_t(snapshot.network_epoch) + age / 1000;
  if (current < snapshot.build_epoch || current + 1 > 4102444800u)
    return denied("SNTP epoch out of bounds");
  earliest = uint32_t(current);
  // The SNTP mailbox retains whole seconds, not the sample's microseconds.
  latest = earliest + 1;
  if (reason) *reason = nullptr;
  return true;
#endif
}
size_t formatClockJSON(const ClockSnapshot &s, char *output, size_t capacity) {
  const int length = snprintf(
      output, capacity,
      "{\"sampled_at_ms\":%u,\"build_epoch\":%u,\"sntp_enabled\":%s,\"last_"
      "sntp_epoch\":%u,"
      "\"sntp_age_seconds\":%u,\"rejected_samples\":%u,\"roles\":["
      "{\"role\":\"repeater\",\"epoch\":%u,\"source\":\"%s\",\"synchronized\":%"
      "s,\"enabled\":%s},"
      "{\"role\":\"room\",\"epoch\":%u,\"source\":\"%s\",\"synchronized\":%s,\"enabled\":%s},"
      "{\"role\":\"companion\",\"epoch\":%u,\"source\":\"%s\",\"synchronized\":"
      "%s,\"enabled\":%s}]}",
      s.sampled_at_ms, s.build_epoch, s.network_enabled ? "true" : "false",
      s.network_epoch, s.network_age_seconds, s.rejected_samples,
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
