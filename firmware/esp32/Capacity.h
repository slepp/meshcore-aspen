// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "RadioDashboard.h"

#ifndef ONCHIP_COMPANION_MAX_CLIENTS
#define ONCHIP_COMPANION_MAX_CLIENTS 2
#endif
#ifndef ONCHIP_BOT_HTTPS
#define ONCHIP_BOT_HTTPS 0
#endif

namespace onchip {
constexpr unsigned BOT_NET_SOCKETS = ONCHIP_BOT_HTTPS ? 1 : 0;
static_assert(KISS_MAX_TCP_CLIENTS == (ONCHIP_BOT_HTTPS ? 3 : 4),
              "HTTPS requires three physical KISS clients; baseline requires four");
static_assert(ONCHIP_COMPANION_MAX_CLIENTS == 2,
              "The stock-SDK combined profile has two companion clients");
static_assert(RadioDashboard::LIVE_CLIENTS == 2 &&
                  RadioDashboard::HTTP_CLIENTS == 3,
              "Combined HTTP capacity is two live and one ordinary client");
constexpr unsigned NETWORK_SOCKET_BUDGET =
    KISS_MAX_TCP_CLIENTS + 1 + RadioDashboard::HTTP_INTERNAL_SOCKETS +
    RadioDashboard::HTTP_CLIENTS + ONCHIP_COMPANION_MAX_CLIENTS + 1 + 1 + 1 + BOT_NET_SOCKETS;
static_assert(NETWORK_SOCKET_BUDGET == 16, "Combined socket budget changed");
static_assert(NETWORK_SOCKET_BUDGET <= CONFIG_LWIP_MAX_SOCKETS,
              "Combined profile exceeds the SDK socket table");
} // namespace onchip
