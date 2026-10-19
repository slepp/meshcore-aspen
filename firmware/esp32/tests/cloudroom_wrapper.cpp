// SPDX-License-Identifier: Apache-2.0
// CloudRoomConfig.cpp keeps a private provider header usable beside saved settings.
#include "CloudRoomService.h"
#include <cassert>
#include <string.h>
#ifdef EXPECT_RENAMED
namespace onchip {
// Saved settings own this name; a second definition from the header would not link.
const CloudRoomConfiguration *cloudRoomConfiguration() { return nullptr; }
}
int main() {
  const auto *provider = onchip::cloudRoomCompiledConfiguration();
  assert(provider && provider->count == 1 && !strcmp(provider->peers[0].token, "fixture-token"));
  assert(!onchip::cloudRoomConfiguration());
}
#else
int main() {
  const auto *provider = onchip::cloudRoomConfiguration();
  assert(provider && provider->count == 1 && !strcmp(provider->peers[0].alias, "room-1"));
}
#endif
