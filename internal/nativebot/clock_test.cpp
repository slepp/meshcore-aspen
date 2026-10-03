// SPDX-License-Identifier: Apache-2.0
#include "Clock.h"
#include "NativeClock.h"
#include <cassert>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <sys/timex.h>
#include <thread>
#include <time.h>
#include <unistd.h>

static uint32_t nowMs = 100;
unsigned long millis() { return nowMs; }
void delay(unsigned long ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

int main(int argc, char **argv) {
  assert(argc == 2 && argv[1][0] == '/');
  const std::string path = std::string(argv[1]) + "/clock-fixture-" + std::to_string(getpid());
  const int file = open(path.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
  assert(file >= 0 && !setenv("MESHCORE_NATIVE_CLOCK_FIXTURE", path.c_str(), 1));
  const auto configure = [&](int status, int state, long long offset, long error = 250000) {
    char input[128];
    const int size = snprintf(input, sizeof(input), "%d %d %lld %ld\n", status, state, offset, error);
    assert(!ftruncate(file, 0) && pwrite(file, input, size, 0) == size);
  };
  onchip::NativeClockSample sample;
  uint32_t earliest = 0, latest = 0;
  configure(0, TIME_OK, 0);
  assert(onchip::nativeBotClockSample(sample) && sample.latestUtcMs > sample.earliestUtcMs &&
         sample.latestUtcMs - sample.earliestUtcMs >= 500);
  onchip::publishHostClock(sample.earliestUtcMs, sample.latestUtcMs, sample.sampledMonotonicNs, sample.epoch);
  assert(onchip::trustedNetworkTime(earliest, latest));
  for (long error : {990000L, 1000000L, 1024000L, 5000L, 1900000L}) {
    configure(0, TIME_OK, 0, error);
    assert(onchip::nativeBotClockSample(sample) && sample.httpsTrusted &&
           sample.errorBoundUs >= uint64_t(error) &&
           sample.latestUtcMs - sample.earliestUtcMs >= uint64_t(error) / 500);
    onchip::publishHostClock(sample.earliestUtcMs, sample.latestUtcMs, sample.sampledMonotonicNs, sample.epoch);
    assert(onchip::trustedNetworkTime(earliest, latest));
  }
  configure(0, TIME_OK, 0, 2100000);
  assert(!onchip::nativeBotClockSample(sample) && sample.httpsTrusted &&
         !onchip::trustedNetworkTime(earliest, latest));
  configure(0, TIME_OK, 0, 5000);
  assert(onchip::nativeBotClockSample(sample) && sample.httpsTrusted);
  configure(STA_UNSYNC, TIME_OK, 0);
  assert(!onchip::nativeBotClockSample(sample) && !sample.httpsTrusted);
  assert(!onchip::trustedNetworkTime(earliest, latest));
  configure(0, TIME_ERROR, 0);
  assert(!onchip::nativeBotClockSample(sample) && !sample.httpsTrusted);
  configure(STA_CLOCKERR, TIME_OK, 0);
  assert(!onchip::nativeBotClockSample(sample) && !sample.httpsTrusted);
  configure(0, TIME_OK, 0, 2000001);
  assert(!onchip::nativeBotClockSample(sample));
  configure(0, TIME_OK, 0);
  assert(!onchip::nativeBotClockSample(sample));
  delay(1100);
  assert(onchip::nativeBotClockSample(sample));
  configure(0, TIME_OK, 0, LONG_MAX);
  assert(!onchip::nativeBotClockSample(sample) && sample.httpsTrusted &&
         sample.errorBoundUs >= uint64_t(LONG_MAX));
  configure(0, TIME_OK, 10000000000LL, LONG_MAX);
  assert(!onchip::nativeBotClockSample(sample) && !sample.httpsTrusted);
  configure(0, TIME_OK, 0);
  assert(!onchip::nativeBotClockSample(sample) && !sample.httpsTrusted);
  delay(1100);
  assert(onchip::nativeBotClockSample(sample) && sample.httpsTrusted);
  configure(0, TIME_OK, -5000000000LL);
  assert(!onchip::nativeBotClockSample(sample));
  configure(0, TIME_OK, 10000000000LL);
  assert(!onchip::nativeBotClockSample(sample));
  configure(0, TIME_OK, 0);
  assert(!onchip::nativeBotClockSample(sample));
  delay(1100);
  assert(onchip::nativeBotClockSample(sample));

  constexpr uint64_t epoch = 1767225600000ULL;
  const auto sampled = [] {
    timespec now{};
    assert(!clock_gettime(CLOCK_MONOTONIC, &now));
    return uint64_t(now.tv_sec) * 1000000000 + uint64_t(now.tv_nsec);
  };
  onchip::publishHostClock(epoch + 250, epoch + 450, sampled(), onchip::hostClockEpoch());
  assert(onchip::trustedNetworkTime(earliest, latest) &&
         earliest == epoch / 1000 && latest == epoch / 1000 + 1);
  nowMs += 900;
  assert(onchip::trustedNetworkTime(earliest, latest) &&
         earliest == epoch / 1000 + 1 && latest == epoch / 1000 + 2);
  nowMs += 2101;
  assert(!onchip::trustedNetworkTime(earliest, latest));
  onchip::publishHostClock(epoch, epoch + 6000, sampled(), onchip::hostClockEpoch());
  assert(!onchip::trustedNetworkTime(earliest, latest));
  onchip::publishHostClock(epoch, epoch + 1, sampled(), onchip::hostClockEpoch());
  assert(onchip::trustedNetworkTime(earliest, latest));
  const uint64_t revoked = onchip::hostClockEpoch();
  onchip::revokeHostClock();
  onchip::publishHostClock(epoch, epoch + 1, sampled(), revoked);
  assert(!onchip::trustedNetworkTime(earliest, latest));
  onchip::publishHostClock(epoch, epoch + 1, sampled() - 4000000000ULL, onchip::hostClockEpoch());
  assert(!onchip::trustedNetworkTime(earliest, latest));
  onchip::publishHostClock(epoch, epoch + 1, sampled() + 1000000000ULL, onchip::hostClockEpoch());
  assert(!onchip::trustedNetworkTime(earliest, latest));
  onchip::publishHostClock(0, 0, 0, 0);
  assert(!onchip::trustedNetworkTime(earliest, latest));
  onchip::ClockSnapshot snapshot;
  assert(onchip::clockSnapshot(snapshot) && !snapshot.network_enabled && !snapshot.network_epoch);
  assert(!close(file) && !unlink(path.c_str()));
  puts("PASS native clock: timesyncd uncertainty ramp/reset without trust flap, separate HTTPS/scheduler limits, loss/recovery, discontinuities, uncertainty/age and epoch revocation");
}
