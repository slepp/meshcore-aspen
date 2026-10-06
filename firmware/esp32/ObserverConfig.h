// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace onchip {
struct ObserverConfig {
  uint32_t version = 1;
  char uri[257]{}, audience[254]{}, prefix[121]{}, name[32]{}, iata[4]{};
  char username[129]{}, password[257]{}, ca[4097]{};
  uint16_t filter = 0xffff;
  uint8_t format = 0, reserved = 0;
  bool valid() const;
};
bool loadObserverConfig(ObserverConfig &config);
void observerConfigCommand(const char *command, char *reply, size_t capacity);
#if defined(ONCHIP_OBSERVER_CONFIG_TEST)
void resetObserverConfigForTest();
#endif
} // namespace onchip
