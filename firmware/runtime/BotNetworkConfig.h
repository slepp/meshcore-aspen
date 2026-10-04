// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "BotHttps.h"

namespace onchip {
constexpr size_t BotNetworkPathLimit = 128;
struct BotNetworkRoute {
  char address[16]{}, host[254]{}, ca[4097]{}, token[257]{};
  char path[BotNetworkPathLimit + 1]{};
  uint16_t port = 443;
  uint8_t operations = 0;
  bool post = false;
  ~BotNetworkRoute() {
    volatile char *bundle = ca, *credential = token;
    for (size_t i = 0; i < sizeof(ca); ++i) bundle[i] = 0;
    for (size_t i = 0; i < sizeof(token); ++i) credential[i] = 0;
  }
  BotHttpsConfig https() const {
    return {address, host, ca, token, port, operations};
  }
};

bool botNetworkResolve(const BotIoRequest &request, BotNetworkRoute &route);
bool botNetworkHomeConfigured();
uint32_t botNetworkEpoch();
bool botNetworkConfigured();
bool botNetworkReload();
} // namespace onchip
