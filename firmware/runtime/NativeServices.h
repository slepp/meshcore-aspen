// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace onchip {
// Registered objects outlive the host. Only its network task calls poll/close;
// radio admission and result consumption stay on the dispatch task.
class NativeNetworkService {
public:
  virtual ~NativeNetworkService() = default;
  virtual void poll() = 0;
  virtual void close() = 0;
};

class NativeNetworkHost {
public:
  virtual ~NativeNetworkHost() = default;
  virtual bool ensureNativeHttps() = 0;
  // Dispatch-thread registration, once before shutdown. Failure leaves the
  // service unattached; the caller releases its unregistered resources.
  virtual bool attachNetworkService(NativeNetworkService &service) = 0;
};

using BotNetworkService = NativeNetworkService;
} // namespace onchip
