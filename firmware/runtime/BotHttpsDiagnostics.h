// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>

namespace onchip {
enum class BotHttpsFailurePoint : uint8_t {
  None, AdmissionTotal, AdmissionBlock, AdmissionInput, AdmissionOutput,
  AdmissionReserve, Client, Connect, CaParse, PeerCertificate, Clock,
  CertificateTime, ConnectedReserve, IoReserve
};
inline const char *botHttpsFailureName(BotHttpsFailurePoint point) {
  static const char *const names[] = {
      "none", "admission-total", "admission-block", "admission-input", "admission-output",
      "admission-reserve", "client", "connect", "ca-parse", "peer-certificate", "clock",
      "certificate-time", "connected-reserve", "io-reserve"};
  const auto index = unsigned(point);
  return index < sizeof(names) / sizeof(*names) ? names[index] : "unknown";
}
// Numeric snapshots only: no endpoint, certificate, token or SDK error strings.
struct BotHttpsDiagnostics {
  BotHttpsFailurePoint point = BotHttpsFailurePoint::None;
  int32_t sdk = 0;
  uint32_t before = 0, ca = 0, connected = 0, failure = 0, after = 0;
  uint32_t largestBefore = 0, largestFailure = 0, largestAfter = 0;
  uint32_t globalMinBefore = 0, globalMinAfter = 0;
  uint32_t peerBytes = 0;
  uint16_t cipher = 0;
  uint8_t peerCount = 0;
  bool caCached = false;
};
} // namespace onchip
