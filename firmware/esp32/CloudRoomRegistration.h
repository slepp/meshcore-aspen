// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "NativeServices.h"

namespace onchip {
// Registers the cloud-room service with the network task. A node with a linked alias
// registers at boot; a node with none registers on its first link job, so an unlinked node
// leaves the registry's service slot and reserved socket budget free. Dispatch task only.
class CloudRoomRegistration {
  NativeNetworkHost *host_ = nullptr;
  bool registered_ = false;
public:
  void bind(NativeNetworkHost &host) { host_ = &host; }
  bool registered() const { return registered_; }
  // Already registered, or registers now; false (with the reason) when the registry refuses.
  bool ensure(NativeNetworkService &service, const NativeServiceBudget &budget,
              NativeServiceRegistration *reason = nullptr) {
    if (registered_) return true;
    const auto result = host_ ? host_->attachNetworkService(service, budget) : NativeServiceRegistration::Unavailable;
    if (reason) *reason = result;
    registered_ = result == NativeServiceRegistration::Attached;
    return registered_;
  }
  // The alias path registered through the host directly at boot.
  void adopted() { registered_ = true; }
};
} // namespace onchip
