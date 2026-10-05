// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "FirmwareIdentity.h"

namespace radio_firmware {
#if defined(MESHCORE_ONCHIP) || defined(NRF52_PLATFORM) || defined(NRFMAST_RF_ENABLED)
constexpr char version[] = ONCHIP_FIRMWARE_VERSION;
#else
constexpr char version[] = MESHCORE_SLP_BIRCH_VERSION;
#endif
}
