// SPDX-License-Identifier: Apache-2.0
#pragma once

#if defined(MESHCORE_ONCHIP) || defined(NRF52_PLATFORM) || defined(NRFMAST_RF_ENABLED)
#include "FirmwareIdentity.h"
#endif

namespace radio_firmware {
#if defined(MESHCORE_ONCHIP) || defined(NRF52_PLATFORM) || defined(NRFMAST_RF_ENABLED)
constexpr char version[] = ONCHIP_FIRMWARE_VERSION;
#else
constexpr char version[] = "1.17.1-slp-birch";
#endif
}
