// SPDX-License-Identifier: Apache-2.0
#include "../Config.h"
#include "../RoleProfile.h"
#include <nvs.h>
#include <cassert>
#include <cstdio>
unsigned long millis() { return 0; }
void delay(unsigned long) {}
int main() {
  assert(onchip::publicProvisioningReady());
  assert(!strcmp(onchip::adminPassword(), "private-admin"));
  assert(!strcmp(onchip::roomPassword(), "private-room"));
  assert(!strcmp(onchip::mastPassword(), "private-mast"));
  assert(!strcmp(onchip::operatorPublicKey(), "private-authority"));
  onchip::RoleProfile profile;
  uint8_t width;
  assert(onchip::loadRoleProfile(profile) && profile.enabled == onchip::RoleProfile::All);
  assert(onchip::loadOriginPathWidth(width) && width == 1);
  assert(onchip::saveRoleProfile({2}) && onchip::saveOriginPathWidth(3));
  assert(onchip::loadRoleProfile(profile) && profile.enabled == 2);
  assert(onchip::loadOriginPathWidth(width) && width == 3);
  puts("Sealed compiled defaults and saved preferences unchanged");
}
