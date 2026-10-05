#pragma once

#include "CommandBot.h"
#include "BleConfig.h"
#include <helpers/IdentityStore.h>

namespace nrfmast {

class RuntimeConfig {
  FILESYSTEM& fs;
  mesh::LocalIdentity& repeater;
  CommandBot& bot;
#if !NRFMAST_PRODUCTION_LUA
  bool adaptivePolicyFault = false;
#endif

  bool load(const char* record, mesh::LocalIdentity& identity, char name[32]);
  bool save(const char* record, mesh::LocalIdentity& identity, const char* name);
  void publicKey(bool botRole, char* reply);
  void importKey(bool botRole, uint32_t senderTimestamp, const char* value, char* reply);
  bool loadAdaptive(bool& enabled);
  bool saveAdaptive(bool enabled);
  bool loadTimeProvider(uint8_t key[32]);
  bool saveTimeProvider(const uint8_t key[32]);
  bool timeProviderFault = false;

public:
  BleConfig ble;
  RuntimeConfig(FILESYSTEM& storage, mesh::LocalIdentity& repeaterIdentity, CommandBot& commandBot)
      : fs(storage), repeater(repeaterIdentity), bot(commandBot), ble(storage) {}
  bool begin();
  bool setBotName(const char* name);
  // Called only by USB or the native repeater's authenticated, encrypted admin CLI.
  bool handleCommand(uint32_t senderTimestamp, const char* command, char* reply);
};

}  // namespace nrfmast
