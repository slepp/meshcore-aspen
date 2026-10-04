// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <Identity.h>
#include <Packet.h>
#include <stddef.h>
#include <stdint.h>

namespace onchip::observerWire {
// agessaman/MeshCore observer-firmware c2c4cb59e34c9d305a23bfc5d30f16c6fc3e30b2.
constexpr uint32_t TOKEN_LIFETIME = 86400, RENEWAL_MARGIN = 300;
bool decode(const uint8_t *, size_t, mesh::Packet &);
bool token(const mesh::LocalIdentity &, const char *audience, uint32_t epoch,
           char *, size_t, uint32_t lifetime = TOKEN_LIFETIME);
size_t packet(const uint8_t *, size_t, const char *origin, const char *key,
              uint32_t epoch, float rssi, float snr, uint16_t filter,
              char *, size_t, bool captureFormat = false);
size_t status(const char *state, const char *origin, const char *key,
              const char *model, const char *version, const char *radio,
              uint32_t epoch, char *, size_t);
} // namespace onchip::observerWire
