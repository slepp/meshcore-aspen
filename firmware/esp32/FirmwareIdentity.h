// SPDX-License-Identifier: Apache-2.0
#pragma once

#define MESHCORE_SLP_ASPEN_VERSION "1.17.1-slp-aspen"
#define MESHCORE_SLP_PINE_VERSION "1.17.1-slp-pine"

#ifndef ONCHIP_FIRMWARE_VERSION
#if defined(NRF52_PLATFORM) || defined(NRFMAST_RF_ENABLED)
#define ONCHIP_FIRMWARE_VERSION MESHCORE_SLP_PINE_VERSION
#else
#define ONCHIP_FIRMWARE_VERSION MESHCORE_SLP_ASPEN_VERSION
#endif
#endif

static_assert(sizeof(ONCHIP_FIRMWARE_VERSION) <= 20,
              "Firmware profile must fit the companion device-info version field");
