// SPDX-License-Identifier: Apache-2.0
#include "../../../firmware/runtime/BotSignal.h"
#include <cassert>
#include <future>
#include <poll.h>
#include <vector>

int main() {
  using namespace onchip;
  auto &wake = BotWake::shared();
  BotSignal<unsigned> value{0};
  const auto unchanged = wake.snapshot();
  value = 0;
  value.store(0);
  assert(value.exchange(0) == 0);
  unsigned expected = 0;
  assert(value.compare_exchange_strong(expected, 0));
  assert(wake.snapshot() == unchanged);
  pollfd event{wake.descriptor(), POLLIN, 0};
  assert(poll(&event, 1, 0) == 0);

  value = 1;
  assert(poll(&event, 1, 100) == 1);
  wake.drain();
  assert(poll(&event, 1, 0) == 0);
  const auto before = std::chrono::steady_clock::now();
  wake.wait(unchanged, 1000);
  assert(std::chrono::steady_clock::now() - before < std::chrono::milliseconds(100));
  const auto revision = wake.snapshot();
  wake.wait(revision, 20);
  assert(wake.snapshot() == revision);

  for (unsigned round = 0; round < 100; ++round) {
    const auto revision = wake.snapshot();
    std::vector<std::future<void>> waiting;
    for (unsigned n = 0; n < 6; ++n)
      waiting.push_back(std::async(std::launch::async, [&wake, revision] { wake.wait(revision); }));
    ++value;
    for (auto &task : waiting) {
      assert(task.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
      task.get();
    }
  }
  expected = value.load() + 1;
  const auto failed = wake.snapshot();
  assert(!value.compare_exchange_strong(expected, 0));
  assert(wake.snapshot() == failed);
  wake.drain();
  assert(poll(&event, 1, 0) == 0);
  return 0;
}
