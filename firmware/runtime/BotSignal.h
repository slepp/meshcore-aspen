// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <atomic>
#if defined(MESHCORE_HOST_BOT_SOURCE) && MESHCORE_HOST_BOT_SOURCE
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <system_error>
#include <sys/eventfd.h>
#include <unistd.h>

namespace onchip {
class BotWake {
  std::mutex mutex_;
  std::condition_variable changed_;
  uint64_t revision_ = 0;
  int descriptor_;

public:
  BotWake() : descriptor_(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) {
    if (descriptor_ < 0) throw std::system_error(errno, std::generic_category(), "native bot eventfd");
  }
  ~BotWake() { close(descriptor_); }
  BotWake(const BotWake &) = delete;
  BotWake &operator=(const BotWake &) = delete;
  static BotWake &shared() {
    static BotWake value;
    return value;
  }
  int descriptor() const { return descriptor_; }
  uint64_t snapshot() {
    std::lock_guard<std::mutex> lock(mutex_);
    return revision_;
  }
  void notify() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++revision_;
    }
    changed_.notify_all();
    uint64_t value = 1;
    ssize_t result;
    do { result = write(descriptor_, &value, sizeof(value)); } while (result < 0 && errno == EINTR);
    if (result < 0 && errno != EAGAIN)
      throw std::system_error(errno, std::generic_category(), "native bot wake write");
  }
  void drain() {
    uint64_t value;
    ssize_t result;
    do { result = read(descriptor_, &value, sizeof(value)); } while (result < 0 && errno == EINTR);
    if (result < 0 && errno != EAGAIN)
      throw std::system_error(errno, std::generic_category(), "native bot wake read");
  }
  void wait(uint64_t revision, uint32_t milliseconds = UINT32_MAX) {
    std::unique_lock<std::mutex> lock(mutex_);
    const auto ready = [&] { return revision_ != revision; };
    if (milliseconds == UINT32_MAX) changed_.wait(lock, ready);
    else changed_.wait_for(lock, std::chrono::milliseconds(milliseconds), ready);
  }
};

template <typename T> class BotSignal : public std::atomic<T> {
  using Atomic = std::atomic<T>;
public:
  BotSignal() : Atomic(T{}) {}
  constexpr BotSignal(T value) : Atomic(value) {}
  void store(T value, std::memory_order order = std::memory_order_seq_cst) {
    if (Atomic::exchange(value, order) != value) BotWake::shared().notify();
  }
  T operator=(T value) { store(value); return value; }
  T exchange(T value, std::memory_order order = std::memory_order_seq_cst) {
    const T previous = Atomic::exchange(value, order);
    if (previous != value) BotWake::shared().notify();
    return previous;
  }
  bool compare_exchange_strong(T &expected, T desired,
                              std::memory_order order = std::memory_order_seq_cst) {
    const T previous = expected;
    const bool exchanged = Atomic::compare_exchange_strong(expected, desired, order);
    if (exchanged && previous != desired) BotWake::shared().notify();
    return exchanged;
  }
  T operator++() {
    const T previous = Atomic::fetch_add(1);
    BotWake::shared().notify();
    return previous + 1;
  }
};
inline void botEarlier(uint32_t &wait, uint32_t now, uint32_t deadline) {
  const int32_t remaining = int32_t(deadline - now);
  const uint32_t delay = remaining > 0 ? uint32_t(remaining) : 0;
  if (delay < wait) wait = delay;
}
} // namespace onchip
#else
namespace onchip {
template <typename T> using BotSignal = std::atomic<T>;
}
#endif
