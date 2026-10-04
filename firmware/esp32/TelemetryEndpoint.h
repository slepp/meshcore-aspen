// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "BotHttps.h"
#include "Telemetry.h"

namespace onchip {
struct TelemetryEndpoint {
  uint32_t version = 1;
  char address[16]{}, host[122]{}, path[122]{}, ca[4097]{}, token[257]{};
  uint16_t port = 443;
  BotHttpsConfig https() const { return {address, host, ca, token, port}; }
  bool valid() const;
};
// Dispatch-thread only. Active settings and credentials never cross into Lua.
bool telemetryEndpoint(TelemetryEndpoint &destination);
bool telemetryEndpointConfigured();
void telemetryEndpointCommand(const char *command, char *reply, size_t capacity);
#if defined(ONCHIP_TELEMETRY_TEST)
void resetTelemetryEndpointForTest();
#endif
void performTelemetryPost(BotHttpsTransport &transport, const TelemetryEndpoint &endpoint,
                          const char *body, size_t size, uint32_t deadline,
                          const std::atomic<bool> &cancelled, const std::atomic<bool> &stopping,
                          TelemetryCompletion &result);
} // namespace onchip
