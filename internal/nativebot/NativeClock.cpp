// SPDX-License-Identifier: Apache-2.0
#include "NativeClock.h"
#if defined(MESHCORE_HOST_BOT_SOURCE) && MESHCORE_HOST_BOT_SOURCE
#include "Clock.h"
#endif
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <mutex>
#include <sys/stat.h>
#include <sys/timex.h>
#include <time.h>
#include <unistd.h>

namespace onchip {
namespace {
constexpr uint64_t Second = 1000000000;
constexpr uint64_t FirstEpoch = 1767225600;
constexpr uint64_t LastEpoch = 4102444800;
constexpr uint64_t SchedulerErrorLimit = NativeSchedulerErrorLimitUs * 1000;
struct Reading {
  uint64_t utc = 0, monotonic = 0, error = 0;
  uint64_t errorBoundUs = 0;
  bool schedulerBounded = false;
};
bool nanoseconds(clockid_t clock, uint64_t &value) {
  timespec result{};
  if (clock_gettime(clock, &result) || result.tv_sec < 0 ||
      result.tv_nsec < 0 || result.tv_nsec >= long(Second) ||
      uint64_t(result.tv_sec) > (UINT64_MAX - uint64_t(result.tv_nsec)) / Second) return false;
  value = uint64_t(result.tv_sec) * Second + uint64_t(result.tv_nsec);
  return true;
}
bool reading(Reading &value) {
  uint64_t before = 0, after = 0;
  if (!nanoseconds(CLOCK_MONOTONIC, before)) return false;
  timex clock{};
  int state = 0;
  int64_t offset = 0;
  bool fixture = false;
#if defined(MESHCORE_HOST_CLOCK_TEST) && MESHCORE_HOST_CLOCK_TEST
  if (const char *path = std::getenv("MESHCORE_NATIVE_CLOCK_FIXTURE")) {
    const int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    struct stat info{};
    char input[128]{}, extra = 0;
    long long adjustment = 0;
    bool valid = fd >= 0 && !fstat(fd, &info) && S_ISREG(info.st_mode) &&
        info.st_uid == geteuid() && (info.st_mode & 07777) == 0600 &&
        info.st_size > 0 && info.st_size < off_t(sizeof(input)) &&
        read(fd, input, size_t(info.st_size)) == info.st_size &&
        std::sscanf(input, "%d %d %lld %ld %c", &clock.status, &state,
                    &adjustment, &clock.maxerror, &extra) == 4 &&
        adjustment >= -86400LL * int64_t(Second) && adjustment <= 86400LL * int64_t(Second);
    if (fd >= 0) close(fd);
    if (!valid) return false;
    clock.esterror = clock.maxerror;
    offset = adjustment;
    fixture = true;
  } else
#endif
  {
    state = adjtimex(&clock);
  }
  const auto trusted = [](const timex &clock, int state) {
    return state >= 0 && state != TIME_ERROR && !(clock.status & (STA_UNSYNC | STA_CLOCKERR)) &&
        clock.maxerror >= 0 && clock.esterror >= 0;
  };
  if (!trusted(clock, state) || !nanoseconds(CLOCK_REALTIME, value.utc) ||
      value.utc < FirstEpoch * Second || value.utc > LastEpoch * Second) return false;
  if (!fixture) {
    timex verified{};
    const int verifiedState = adjtimex(&verified);
    if (!trusted(verified, verifiedState)) return false;
    clock.maxerror = std::max(clock.maxerror, verified.maxerror);
    clock.esterror = std::max(clock.esterror, verified.esterror);
  }
  if (!nanoseconds(CLOCK_MONOTONIC, after) || after < before || after - before > Second)
    return false;
  value.utc = uint64_t(int64_t(value.utc) + offset);
  value.monotonic = after;
  const uint64_t kernelErrorUs = uint64_t(std::max(clock.maxerror, clock.esterror));
  value.errorBoundUs = kernelErrorUs + (after - before + 999) / 1000;
  // Cap continuity allowance without overflowing or widening scheduler bounds.
  value.error = kernelErrorUs <= NativeSchedulerErrorLimitUs ?
      kernelErrorUs * 1000 + after - before : SchedulerErrorLimit + 1;
  value.schedulerBounded = value.error <= SchedulerErrorLimit &&
      value.utc >= FirstEpoch * Second + value.error &&
      value.utc <= LastEpoch * Second - value.error;
  return value.utc >= FirstEpoch * Second && value.utc <= LastEpoch * Second;
}
}

bool nativeBotClockSample(NativeClockSample &sample) {
  static std::mutex mutex;
  static Reading previous{};
  static bool anchored = false, recovering = false;
  static uint64_t stableSince = 0;
  std::lock_guard<std::mutex> lock(mutex);
  sample = {};
  const auto denied = [] {
#if defined(MESHCORE_HOST_BOT_SOURCE) && MESHCORE_HOST_BOT_SOURCE
    revokeHostClock();
#endif
    return false;
  };
  Reading current;
  if (!reading(current)) {
    anchored = false;
    recovering = true;
    return denied();
  }
  sample.errorBoundUs = current.errorBoundUs;
  if (anchored) {
    const uint64_t elapsed = current.monotonic >= previous.monotonic ?
        current.monotonic - previous.monotonic : 0;
    const int64_t difference = int64_t(current.utc) - int64_t(previous.utc) - int64_t(elapsed);
    const uint64_t allowance = std::min(previous.error, SchedulerErrorLimit) +
        std::min(current.error, SchedulerErrorLimit) + elapsed / 1000 + 1000000;
    if (current.monotonic < previous.monotonic || current.utc < previous.utc ||
        difference > int64_t(allowance) || difference < -int64_t(allowance)) {
      recovering = true;
      stableSince = current.monotonic;
    }
  } else {
    stableSince = current.monotonic;
    anchored = true;
  }
  previous = current;
  if (recovering) {
    if (current.monotonic - stableSince < Second) {
      sample.reason = "recovering-discontinuity-or-sync";
      return denied();
    }
    recovering = false;
  }
  sample.httpsTrusted = true;
  if (!current.schedulerBounded) {
    sample.reason = current.error > SchedulerErrorLimit ?
        "scheduler-error-bound-exceeded" : "scheduler-utc-bounds-exceeded";
    return denied();
  }
  sample.reason = "usable";
  sample.earliestUtcMs = (current.utc - current.error) / 1000000;
  sample.latestUtcMs = (current.utc + current.error + 999999) / 1000000;
  sample.sampledMonotonicNs = current.monotonic;
#if defined(MESHCORE_HOST_BOT_SOURCE) && MESHCORE_HOST_BOT_SOURCE
  sample.epoch = hostClockEpoch();
#endif
  return true;
}
}
