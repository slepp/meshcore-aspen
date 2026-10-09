// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <atomic>
#include <stdint.h>
#include <string.h>

#ifndef ONCHIP_NATIVE_SERVICE_SOCKETS
#if defined(MESHCORE_CLOUD_ROOM) && MESHCORE_CLOUD_ROOM
#ifndef ONCHIP_CLOUD_ROOM_CONNECTIONS
#define ONCHIP_CLOUD_ROOM_CONNECTIONS 1
#endif
#define ONCHIP_NATIVE_SERVICE_SOCKETS ONCHIP_CLOUD_ROOM_CONNECTIONS
#else
#define ONCHIP_NATIVE_SERVICE_SOCKETS 0
#endif
#endif

namespace onchip {
struct NativeServiceBudget {
  const char *name = nullptr;
  unsigned sockets = 0;
  unsigned work = 0;
};
enum class NativeServiceRegistration : uint8_t {
  Attached, Unavailable, Stopping, InvalidBudget, Duplicate, Full, SocketBudget
};
inline const char *nativeServiceRegistrationText(NativeServiceRegistration result) {
  switch (result) {
    case NativeServiceRegistration::Attached: return "registered";
    case NativeServiceRegistration::Unavailable: return "network worker is not running";
    case NativeServiceRegistration::Stopping: return "network worker is stopping";
    case NativeServiceRegistration::InvalidBudget: return "invalid service name or work budget";
    case NativeServiceRegistration::Duplicate: return "service object or name is already registered";
    case NativeServiceRegistration::Full: return "network service registry is full";
    case NativeServiceRegistration::SocketBudget: return "reserved service socket budget exceeded";
  }
  return "unknown network service registration result";
}
// Registered objects outlive the host. Only its network task calls poll/close;
// radio admission and result consumption stay on the dispatch task.
class NativeNetworkService {
public:
  virtual ~NativeNetworkService() = default;
  virtual void poll(unsigned work) = 0;
  virtual void close() = 0;
};

class NativeNetworkHost {
public:
  virtual ~NativeNetworkHost() = default;
  virtual bool ensureNativeHttps() = 0;
  // Dispatch-thread registration; objects and budget names outlive the host.
  // A rejected service stays caller-owned and must release its startup resources.
  virtual NativeServiceRegistration attachNetworkService(
      NativeNetworkService &service, const NativeServiceBudget &budget) = 0;
};

class NativeServiceRegistry {
  static constexpr unsigned Sealed = 1u << 31;
  struct Entry {
    NativeNetworkService *service = nullptr;
    NativeServiceBudget budget;
  };
  Entry entries_[2];
  const unsigned socketLimit_;
  std::atomic<unsigned> published_{0};
  unsigned cursor_ = 0;
  bool closed_ = false;
public:
  static constexpr unsigned Capacity = 2;
  explicit NativeServiceRegistry(unsigned sockets) : socketLimit_(sockets) {}
  NativeServiceRegistration attach(NativeNetworkService &service,
                                   const NativeServiceBudget &budget) {
    auto state = published_.load(std::memory_order_acquire);
    if (state & Sealed) return NativeServiceRegistration::Stopping;
    if (!budget.name || !budget.name[0] || strnlen(budget.name, 32) == 32 ||
        !budget.work || budget.work > UINT16_MAX)
      return NativeServiceRegistration::InvalidBudget;
    for (const char *c = budget.name; *c; ++c)
      if (*c < 33 || *c > 126) return NativeServiceRegistration::InvalidBudget;
    unsigned sockets = 0;
    for (unsigned i = 0; i < state; ++i) {
      if (entries_[i].service == &service || !strcmp(entries_[i].budget.name, budget.name))
        return NativeServiceRegistration::Duplicate;
      sockets += entries_[i].budget.sockets;
    }
    if (state == Capacity) return NativeServiceRegistration::Full;
    if (sockets > socketLimit_ || budget.sockets > socketLimit_ - sockets)
      return NativeServiceRegistration::SocketBudget;
    entries_[state] = {&service, budget};
    // Publish initialized metadata atomically, or lose to shutdown without
    // exposing the rejected entry to the network task.
    if (!published_.compare_exchange_strong(state, state + 1,
                                            std::memory_order_acq_rel,
                                            std::memory_order_acquire))
      return NativeServiceRegistration::Stopping;
    return NativeServiceRegistration::Attached;
  }
  unsigned size() const { return published_.load(std::memory_order_acquire) & ~Sealed; }
  void seal() { published_.fetch_or(Sealed, std::memory_order_acq_rel); }
  void poll() {
    const auto state = published_.load(std::memory_order_acquire);
    if ((state & Sealed) || !state) return;
    for (unsigned n = 0; n < state; ++n) {
      if (published_.load(std::memory_order_acquire) & Sealed) return;
      auto &entry = entries_[(cursor_ + n) % state];
      entry.service->poll(entry.budget.work);
    }
    cursor_ = (cursor_ + 1) % state;
  }
  void close() {
    seal();
    if (closed_) return;
    closed_ = true;
    for (unsigned i = size(); i > 0; --i) entries_[i - 1].service->close();
  }
};

using BotNetworkService = NativeNetworkService;
} // namespace onchip
