// SPDX-License-Identifier: Apache-2.0
#include "../Config.h"
#include "../Lifecycle.h"
#include "BotSettings.h"
#include <SPIFFS.h>
#include <Utils.h>
#include <KissModem.h>
#include <nvs.h>
#include <cassert>
#include <cstdio>

// The lifecycle only passes this opaque handle to its role startup callback.
class WifiKissMultiplexer {};
unsigned long millis() { return 0; }
void delay(unsigned long) {}
namespace onchip {
bool resetPending(Role, bool &pending) { pending = false; return true; }
bool prepareIdentityReset(Role) { return true; }
bool finishIdentityReset(Role) { return true; }
}
namespace {
unsigned starts = 0;
bool start(WifiKissMultiplexer &) { ++starts; return true; }
bool flush() { return true; }
void stop() {}
void loop() {}
bool erase(bool &done) { done = true; return true; }
void digest(onchip::PublicProvisioningRecord &record) {
  mesh::Utils::sha256(record.digest, sizeof(record.digest),
      reinterpret_cast<const uint8_t *>(&record),
      offsetof(onchip::PublicProvisioningRecord, digest));
}
void select(const onchip::PublicProvisioningRecord &record) {
  const auto *bytes = reinterpret_cast<const uint8_t *>(&record);
  filesystem_test::files["/public-setup.bin"] = {bytes, bytes + sizeof(record)};
}
void offline() {
  assert(!onchip::publicProvisioningReady());
  RadioConfig radio{};
  assert(!onchip::loadPublicInitialRadio(radio));
  assert(!onchip::adminPassword()[0] && !onchip::mastPassword()[0] &&
         !onchip::operatorPublicKey()[0]);
  onchip::RoleProfile profile;
  assert(!onchip::loadRoleProfile(profile) && profile.enabled == 0);
  WifiKissMultiplexer mux;
  const onchip::RoleCallbacks callbacks[3] = {
      {start, flush, stop, erase, loop, nullptr},
      {start, flush, stop, erase, loop, nullptr},
      {start, flush, stop, erase, loop, nullptr}};
  starts = 0;
  onchip::beginLifecycles(mux, callbacks, onchip::RoleProfile{7});
  assert(starts == 0);
  for (unsigned i = 0; i < 3; ++i)
    assert(onchip::rolePhase(static_cast<onchip::Role>(i)) == onchip::RolePhase::Failed);
}
}
int main(int argc, char **argv) {
  assert(argc == 2);
  assert(!onchip::beginPublicProvisioning());
  offline();
  const char layout[] = "meshcore-onchip-fs-v1\n";
  filesystem_test::files["/onchip-layout"] = {layout, layout + sizeof(layout) - 1};
  assert(!onchip::beginPublicProvisioning());
  offline();
  onchip::PublicProvisioningRecord record;
  FILE *input = fopen(argv[1], "rb");
  assert(input && fread(&record, 1, sizeof(record), input) == sizeof(record));
  assert(fgetc(input) == EOF);
  fclose(input);
  assert(onchip::validatePublicProvisioning(record));
  select(record);
  assert(onchip::beginPublicProvisioning());
  RadioConfig radio{};
  assert(onchip::loadPublicInitialRadio(radio) && radio.freq_hz == 912525000 &&
         radio.bw_hz == 250000 && radio.sf == 7 && radio.cr == 5 && radio.tx_power == 2);
  assert(!strcmp(onchip::adminPassword(), "test-admin"));
  assert(!strcmp(onchip::mastPassword(), "test-mast"));
  assert(!strcmp(onchip::operatorPublicKey(), record.operatorPublicKey));
  onchip::RoleProfile profile;
  uint8_t width;
  assert(onchip::loadRoleProfile(profile) && profile.enabled == 7);
  assert(onchip::loadOriginPathWidth(width) && width == 3);
  assert(onchip::initializePublicRuntimePreferences());
  onchip::BotRadioPolicy policy;
  assert(onchip::loadBotRadioPolicy(policy) && policy.pathWidth == 3);
  policy.pathWidth = 1;
  assert(onchip::saveBotRadioPolicy(policy));
  assert(onchip::initializePublicRuntimePreferences());
  assert(onchip::loadBotRadioPolicy(policy) && policy.pathWidth == 1);
  identity_test::durable[{"mc-onchip", "bot-radio"}] = {0, 1};
  assert(!onchip::initializePublicRuntimePreferences());
  offline();
  identity_test::durable.erase({"mc-onchip", "bot-radio"});
  select(record);
  assert(onchip::beginPublicProvisioning() && onchip::initializePublicRuntimePreferences());
  assert(onchip::saveRoleProfile({4}) && onchip::saveOriginPathWidth(2));
  const auto nvsBefore = identity_test::durable;
  const auto filesBefore = filesystem_test::files;
  assert(onchip::beginPublicProvisioning());
  assert(identity_test::durable == nvsBefore && filesystem_test::files == filesBefore);
  assert(onchip::loadRoleProfile(profile) && profile.enabled == 4);
  assert(onchip::loadOriginPathWidth(width) && width == 2);
  WifiKissMultiplexer mux;
  const onchip::RoleCallbacks callbacks[3] = {
      {start, flush, stop, erase, loop, nullptr},
      {start, flush, stop, erase, loop, nullptr},
      {start, flush, stop, erase, loop, nullptr}};
  starts = 0;
  onchip::beginLifecycles(mux, callbacks, profile);
  assert(starts == 1);
  // A changed persisted public authority, not a compiled fleet key, is selected.
  memset(record.operatorPublicKey, '1', 64);
  digest(record);
  select(record);
  assert(onchip::beginPublicProvisioning());
  assert(!strcmp(onchip::operatorPublicKey(), record.operatorPublicKey));
  for (unsigned offset : {0u, 6u, 19u, 303u, 304u}) {
    auto malformed = record;
    reinterpret_cast<uint8_t *>(&malformed)[offset] ^= 1;
    if (offset != 304) digest(malformed);
    select(malformed);
    assert(!onchip::beginPublicProvisioning());
    offline();
  }
  for (const auto &field : {std::pair<unsigned, uint8_t>{4, 16}, {5, 0},
                           {16, 13}, {17, 4}, {18, 23}, {68, 'g'}, {296, 2}}) {
    auto malformed = record;
    reinterpret_cast<uint8_t *>(&malformed)[field.first] = field.second;
    digest(malformed);
    select(malformed);
    assert(!onchip::beginPublicProvisioning());
    offline();
  }
  auto malformed = record;
  memset(malformed.adminPassword, 'x', sizeof(malformed.adminPassword));
  digest(malformed); select(malformed);
  assert(!onchip::beginPublicProvisioning());
  offline();
  select(record);
  filesystem_test::files["/public-setup.bin"].pop_back();
  assert(!onchip::beginPublicProvisioning());
  offline();
  select(record);
  filesystem_test::files["/public-setup.bin"].push_back(0);
  assert(!onchip::beginPublicProvisioning());
  offline();
  select(record);
  filesystem_test::readLimit = sizeof(layout) - 1;
  assert(!onchip::beginPublicProvisioning());
  offline();
  puts("Public provisioning validation, startup gates and retained preferences passed");
}
