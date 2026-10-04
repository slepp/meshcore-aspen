#include "RuntimeConfig.h"
#include "FirmwareIdentity.h"
#if NRFMAST_PRODUCTION_LUA
#include "PineAdmin.h"
#include "onchip/CommandBot.h"
#include "onchip/BotSettings.h"
#endif
#include <cstdio>
#include <cstdlib>

namespace nrfmast {

static void clearBytes(uint8_t* bytes, size_t count) {
  volatile uint8_t* p = bytes;
  while (count--) *p++ = 0;
}

bool RuntimeConfig::load(const char* record, mesh::LocalIdentity& identity, char name[32]) {
  char path[40];
  snprintf(path, sizeof(path), "/%s.id", record);
  auto file = fs.open(path);
  if (!file) return false;
  const auto size = file.size();
  file.close();
  if (size != PUB_KEY_SIZE + PRV_KEY_SIZE && size != PUB_KEY_SIZE + PRV_KEY_SIZE + 32)
    return false;
  memset(name, 0, 32);
  IdentityStore store(fs, "");
  if (!store.load(record, identity, name, 32)) return false;
  return size == PUB_KEY_SIZE + PRV_KEY_SIZE || CommandBot::validName(name);
}

bool RuntimeConfig::save(const char* record, mesh::LocalIdentity& identity, const char* name) {
  IdentityStore store(fs, "");
  // Native IdentityStore truncates its destination and does not propagate short writes.
  // Verify a separate record before LittleFS atomically replaces the live one.
  if (!(name ? store.save("_nrfstage", identity, name) : store.save("_nrfstage", identity)))
    return false;
  mesh::LocalIdentity readback;
  char savedName[32];
  if (!load("_nrfstage", readback, savedName) || (name && strcmp(savedName, name))) return false;
  uint8_t expected[PUB_KEY_SIZE + PRV_KEY_SIZE], actual[sizeof(expected)];
  identity.writeTo(expected, sizeof(expected));
  readback.writeTo(actual, sizeof(actual));
  const bool equal = memcmp(expected, actual, sizeof(expected)) == 0;
  clearBytes(expected, sizeof(expected));
  clearBytes(actual, sizeof(actual));
  if (!equal) return false;
  char path[40];
  snprintf(path, sizeof(path), "/%s.id", record);
  return fs.rename("/_nrfstage.id", path);
}

bool RuntimeConfig::begin() {
  mesh::LocalIdentity saved;
  char name[32];
  if (!load("_nrfbot", saved, name) || !saved.matches(bot.self_id)) return false;
#if !NRFMAST_PRODUCTION_LUA
  bool adaptive = false;
  adaptivePolicyFault = !loadAdaptive(adaptive);
  if (adaptivePolicyFault) {
    adaptive = false;
    Serial.println("Adaptive policy unreadable; static admission active; use bot adaptive off then reboot");
  }
  bot.setAdaptiveAdmission(adaptive);
#endif
  if (!ble.begin()) Serial.println("BLE: saved configuration unreadable; BLE disabled; provision a PIN through USB");
  return !name[0] || bot.setName(name);
}

bool RuntimeConfig::loadAdaptive(bool& enabled) {
#if NRFMAST_PRODUCTION_LUA
  return onchip::loadBotAdaptiveAdmission(enabled);
#else
  enabled = false;
  if (!fs.exists("/pine-adapt")) return true;
  auto file = fs.open("/pine-adapt");
  uint8_t record[5]{};
  const bool valid = file && file.size() == sizeof(record) &&
      file.read(record, sizeof(record)) == sizeof(record) &&
      !memcmp(record, "BAA\1", 4) && record[4] <= 1;
  file.close();
  if (valid) enabled = record[4];
  return valid;
#endif
}
bool RuntimeConfig::saveAdaptive(bool enabled) {
#if NRFMAST_PRODUCTION_LUA
  return onchip::saveBotAdaptiveAdmission(enabled);
#else
  uint8_t record[5] = {'B', 'A', 'A', 1, uint8_t(enabled)}, readback[5]{};
  fs.remove("/pine-adapt.new");
#if defined(NRF52_PLATFORM)
  auto file = fs.open("/pine-adapt.new", FILE_O_WRITE);
#else
  auto file = fs.open("/pine-adapt.new", "w");
#endif
  if (!file) return false;
  const bool written = file.write(record, sizeof(record)) == sizeof(record);
  file.close();
  file = fs.open("/pine-adapt.new");
  const bool verified = written && file && file.size() == sizeof(record) &&
      file.read(readback, sizeof(readback)) == sizeof(readback) && !memcmp(record, readback, sizeof(record));
  file.close();
  if (!verified || !fs.rename("/pine-adapt.new", "/pine-adapt")) {
    fs.remove("/pine-adapt.new"); return false;
  }
  bool actual = false;
  return loadAdaptive(actual) && actual == enabled;
#endif
}

void RuntimeConfig::publicKey(bool botRole, char* reply) {
  mesh::LocalIdentity saved;
  char name[32], activeHex[PUB_KEY_SIZE * 2 + 1], savedHex[sizeof(activeHex)];
  auto& active = botRole ? bot.self_id : repeater;
  if (!load(botRole ? "_nrfbot" : "_main", saved, name)) {
    strcpy(reply, "Error: saved identity unavailable");
    return;
  }
  mesh::Utils::toHex(activeHex, active.pub_key, PUB_KEY_SIZE);
  mesh::Utils::toHex(savedHex, saved.pub_key, PUB_KEY_SIZE);
  snprintf(reply, 157, "active=%s saved=%s reboot=%u", activeHex, savedHex, !saved.matches(active));
}

void RuntimeConfig::importKey(bool botRole, uint32_t senderTimestamp, const char* value, char* reply) {
  if (senderTimestamp != 0) {
    strcpy(reply, "Error: private-key import requires local USB; no key was changed");
    return;
  }
  uint8_t key[PRV_KEY_SIZE] = {};
  const bool valid = strlen(value) == PRV_KEY_SIZE * 2 &&
                     mesh::Utils::fromHex(key, sizeof(key), value) &&
                     mesh::LocalIdentity::validatePrivateKey(key);
  mesh::LocalIdentity candidate;
  if (valid) candidate.readFrom(key, sizeof(key));
  clearBytes(key, sizeof(key));
  if (!valid) {
    strcpy(reply, "Error: invalid native private key");
    return;
  }
  mesh::LocalIdentity saved, other;
  char name[32], otherName[32];
  const char* record = botRole ? "_nrfbot" : "_main";
  if (!load(record, saved, name) || !load(botRole ? "_main" : "_nrfbot", other, otherName)) {
    strcpy(reply, "Error: saved identity unavailable");
    return;
  }
  if (candidate.matches(other) || candidate.matches(botRole ? repeater : bot.self_id)) {
    strcpy(reply, "Error: roles must retain distinct identities");
    return;
  }
  if (!save(record, candidate, botRole ? (name[0] ? name : bot.getName()) : nullptr)) {
    strcpy(reply, "Error: identity commit failed; inspect saved public key before retrying");
    return;
  }
  char publicHex[PUB_KEY_SIZE * 2 + 1];
  mesh::Utils::toHex(publicHex, candidate.pub_key, PUB_KEY_SIZE);
  snprintf(reply, 157, "OK saved; reboot required; public-key=%s", publicHex);
}

bool RuntimeConfig::setBotName(const char* name) {
  if (!CommandBot::validName(name)) return false;
  mesh::LocalIdentity saved;
  char oldName[32];
  return load("_nrfbot", saved, oldName) && save("_nrfbot", saved, name) && bot.setName(name);
}
bool RuntimeConfig::handleCommand(uint32_t senderTimestamp, const char* command, char* reply) {
  if (!strcmp(command, "ver")) {
    snprintf(reply, 157, "v%s (Build: %s)", MESHCORE_SLP_PINE_VERSION, __DATE__);
    return true;
  }
  if (!strcmp(command, "help")) {
    strcpy(reply, "help bot|source|wifi; bot help; source help; ver; board; get name|owner.info|radio|freq|tx; native settings on the repeater");
    return true;
  }
  if (!strcmp(command, "wifi") || !strcmp(command, "wifi help") || !strcmp(command, "help wifi") ||
      !strncmp(command, "get wifi.", 9) || !strncmp(command, "set wifi.", 9)) {
    strcpy(reply, "Error: slp-pine has no WiFi; use native repeater settings and encrypted RF source administration");
    return true;
  }
  if (!strcmp(command, "bot adaptive") || !strncmp(command, "bot adaptive ", 13)) {
#if NRFMAST_PRODUCTION_LUA
    onchip::commandBotService().adaptiveCommand(command, reply, 157);
#else
    if (!strcmp(command, "bot adaptive")) {
      bool saved = false;
      const bool readable = loadAdaptive(saved);
      if (!readable) adaptivePolicyFault = true;
      if (adaptivePolicyFault)
        snprintf(reply, 157, "Error: adaptive policy %s; live=%u policy-fault=1; use bot adaptive off then reboot",
                 readable ? "fault retained" : "unreadable", bot.adaptiveEnabled());
      else bot.adaptiveStatus(saved, reply, 157);
    } else if (strcmp(command + 13, "on") && strcmp(command + 13, "off")) {
      strcpy(reply, "Error: use bot adaptive [on|off]");
    } else {
      const bool saved = saveAdaptive(!strcmp(command + 13, "on"));
      if (saved) adaptivePolicyFault = false;
      strcpy(reply, saved ? "Saved adaptive admission; reboot required" :
                           "Error: adaptive admission save/readback failed; inspect saved selection");
    }
#endif
    return true;
  }
  if (!strcmp(command, "bot advert.zerohop")) {
    strcpy(reply, bot.advertise(true) ? "Queued zero-hop advert; delivery/peer learning unconfirmed" :
                                     "Error: bot advert unavailable");
  } else if (!strcmp(command, "get capabilities")) {
    strcpy(reply, "roles=repeater,bot;bot-name=saved;key-import=USB-only;private-export=disabled;"
                  "channel-keys=unsupported;radio=shared;key/radio=reboot;notes=DM;BLE=v13");
  } else if (!strcmp(command, "get bot.ble")) {
    snprintf(reply, 157, "BLE saved=%s pin=%s; enable/PIN apply after reboot; radio=read-only; bonds cleared on boot",
             ble.isEnabled() ? "on" : "off", ble.getPin() ? "configured" : "unset");
  } else if (!strncmp(command, "set bot.ble ", 12) || !strncmp(command, "set bot.ble.pin ", 16)) {
    if (senderTimestamp) {
      strcpy(reply, "Error: BLE provisioning requires local USB or paired companion PIN command");
    } else {
      bool saved = false;
      if (!strcmp(command, "set bot.ble on")) saved = ble.setEnabled(true);
      else if (!strcmp(command, "set bot.ble off")) saved = ble.setEnabled(false);
      else if (!strncmp(command, "set bot.ble.pin ", 16)) {
        const char* value = command + 16;
        bool valid = strlen(value) == 6;
        for (const char* p = value; *p; ++p) valid = valid && *p >= '0' && *p <= '9';
        if (valid) saved = ble.setPin(strtoul(value, nullptr, 10));
      }
      strcpy(reply, saved ? "OK saved; reboot required; BLE PIN must be provisioned before enabling" :
                           "Error: BLE needs a six-digit PIN (100000-999999); commit failed or invalid setting");
    }
  } else if (!strcmp(command, "get bot.name")) {
    snprintf(reply, 157, "> %s", bot.getName());
  } else if (!strncmp(command, "set bot.name ", 13)) {
    const char* name = command + 13;
    if (!CommandBot::validName(name)) {
      strcpy(reply, "Error: name requires 1-31 printable ASCII bytes, without edge spaces");
    } else {
      mesh::LocalIdentity saved;
      char oldName[32];
      if (!load("_nrfbot", saved, oldName) || !save("_nrfbot", saved, name)) {
        strcpy(reply, "Error: bot name commit failed");
      } else if (!bot.setName(name)) {
        strcpy(reply, "Error: saved bot name could not be applied");
      } else {
        strcpy(reply, "OK saved; name applies to next advert; no reboot required");
      }
    }
  } else if (!strcmp(command, "get bot.pub.key") || !strcmp(command, "get pub.key")) {
    publicKey(!strcmp(command, "get bot.pub.key"), reply);
  } else if (!strncmp(command, "set bot.prv.key ", 16)) {
    importKey(true, senderTimestamp, command + 16, reply);
  } else if (!strncmp(command, "set prv.key ", 12)) {
    importKey(false, senderTimestamp, command + 12, reply);
  } else if (!strncmp(command, "get bot.prv.key", 15) || !strncmp(command, "get prv.key", 11)) {
    strcpy(reply, "Error: private-key export disabled");
  } else if (!strncmp(command, "get bot.", 8) || !strncmp(command, "set bot.", 8) ||
             !strcmp(command, "set prv.key")) {
    strcpy(reply, "Error: unsupported bot setting; get capabilities; use native get/set radio for the shared PHY");
  } else {
    return false;
  }
  return true;
}

}  // namespace nrfmast
