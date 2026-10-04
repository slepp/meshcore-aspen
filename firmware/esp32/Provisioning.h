// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stddef.h>
#include <stdint.h>

struct RadioConfig;
namespace onchip {
// Byte arrays keep the offline record independent of compiler alignment.
struct PublicProvisioningRecord {
  uint8_t magic[4];
  uint8_t roles, pathWidth, reserved[2];
  uint8_t frequencyHz[4], bandwidthHz[4], sf, cr, txDbm, radioReserved;
  char adminPassword[16], roomPassword[16], mastPassword[16];
  char operatorPublicKey[65], trustedCompanionPublicKey[65];
  char wifiSsid[33], wifiPassword[65];
  uint8_t wifiEnabled, tailReserved[7], digest[32];
};
static_assert(sizeof(PublicProvisioningRecord) == 336, "Public setup record layout changed");

#if defined(MESHCORE_PUBLIC_PROVISIONING) && MESHCORE_PUBLIC_PROVISIONING
bool beginPublicProvisioning();
bool publicProvisioningReady();
bool loadPublicInitialRadio(RadioConfig &radio);
bool initializePublicRuntimePreferences();
bool validatePublicProvisioning(const PublicProvisioningRecord &record);
const PublicProvisioningRecord &publicProvisioning();
uint32_t provisioningUint32(const uint8_t bytes[4]);
#endif
} // namespace onchip
