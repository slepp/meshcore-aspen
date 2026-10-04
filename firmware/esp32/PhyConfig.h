// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>

#if !defined(ONCHIP_RADIO_FREQ_MHZ) || !defined(ONCHIP_RADIO_BW_KHZ) ||        \
    !defined(ONCHIP_RADIO_SF) || !defined(ONCHIP_RADIO_CR) ||                  \
    !defined(ONCHIP_RADIO_TX_POWER)
#error                                                                         \
    "Configure all five ONCHIP_RADIO_* values explicitly in the private operator profile"
#else
static_assert(ONCHIP_RADIO_FREQ_MHZ >= 150 && ONCHIP_RADIO_FREQ_MHZ <= 960 &&
                  ONCHIP_RADIO_BW_KHZ > 0 && ONCHIP_RADIO_BW_KHZ <= 500 &&
                  ONCHIP_RADIO_SF >= 5 && ONCHIP_RADIO_SF <= 12 &&
                  ONCHIP_RADIO_CR >= 5 && ONCHIP_RADIO_CR <= 8 &&
                  ONCHIP_RADIO_TX_POWER >= 0 && ONCHIP_RADIO_TX_POWER <= 22,
              "Operator PHY values exceed the Xiao SX1262 limits");
static_assert(
    LORA_FREQ == ONCHIP_RADIO_FREQ_MHZ && LORA_BW == ONCHIP_RADIO_BW_KHZ &&
        LORA_SF == ONCHIP_RADIO_SF && LORA_CR == ONCHIP_RADIO_CR &&
        LORA_TX_POWER == ONCHIP_RADIO_TX_POWER,
    "Native LORA_* defaults must match the explicit shared operator PHY");
#endif
