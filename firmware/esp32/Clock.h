// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "RoleProfile.h"
#include <MeshCore.h>
#include <stddef.h>

#ifdef ARDUINO_ARCH_ESP32
#include <esp_http_server.h>
#endif

namespace onchip {
enum class ClockSource : uint8_t { Build, Native, Network, Gps };

class RoleClock : public mesh::RTCClock {
  uint32_t seconds_ = 0, previousMillis_ = 0;
  uint16_t remainder_ = 0;
  ClockSource source_ = ClockSource::Build;

public:
  explicit RoleClock(uint32_t baseline = 0);
  uint32_t getCurrentTime() override;
  void setCurrentTime(uint32_t seconds) override;
  void tick() override;
  void synchronize(uint32_t seconds, ClockSource source = ClockSource::Network);
  ClockSource source() const { return source_; }
};

struct ClockStatus {
  uint32_t epoch = 0;
  ClockSource source = ClockSource::Build;
  bool synchronized = false;
  bool enabled = false;
};
struct ClockSnapshot {
  uint32_t sampled_at_ms = 0;
  uint32_t build_epoch = 0, network_epoch = 0, network_age_seconds = 0;
  uint64_t network_age_ms = 0;
  uint32_t gps_epoch = 0;
  uint64_t gps_age_ms = 0;
  uint32_t rejected_samples = 0;
  uint32_t network_interval_seconds = 3600;
  bool network_enabled = false;
  ClockStatus roles[3]{};
};

// Dispatch task only, except the SNTP mailbox and published snapshot reader.
void beginClocks(RoleProfile profile = {});
void beginNetworkClock(bool additionalRole = false);
void loopClocks();
void receiveNetworkTime(uint32_t epoch);
bool receiveGpsTime(uint32_t epoch);
RoleClock &repeaterClock();
RoleClock &roomClock();
RoleClock &companionClock();
bool clockSnapshot(ClockSnapshot &snapshot);
// Conservative UTC bounds from a fresh SNTP or GPS sample, never a role RTC.
bool trustedNetworkTime(uint32_t &earliest, uint32_t &latest, const char **reason = nullptr);
bool networkClockCommand(const char *command, char *reply, size_t capacity, bool writeAllowed = false);
bool networkTimeReply(const uint8_t *request, size_t size, uint8_t *reply);
bool publicTimeCommand(const uint8_t *data, size_t size);
#if defined(MESHCORE_HOST_BOT_SOURCE) && MESHCORE_HOST_BOT_SOURCE
// Dispatch-thread publication from the kernel-verified native clock provider.
// Zero bounds revoke trust without changing stored deadlines.
uint64_t hostClockEpoch();
void revokeHostClock();
void publishHostClock(uint64_t earliestUtcMs, uint64_t latestUtcMs, uint64_t sampledMonotonicNs, uint64_t epoch);
#endif
size_t formatClockJSON(const ClockSnapshot &, char *output, size_t capacity);
#ifdef ARDUINO_ARCH_ESP32
esp_err_t registerClockHTTP(httpd_handle_t server);
#endif
} // namespace onchip
