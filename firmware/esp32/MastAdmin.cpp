// SPDX-License-Identifier: Apache-2.0
#if defined(MESHCORE_MAST_ADMIN) && MESHCORE_MAST_ADMIN
#include "MastAdmin.h"
#include "Config.h"
#include "BotHttps.h"
#include "BotSettings.h"
#include "CommandBot.h"
#include "CompanionSessions.h"
#include "Runtime.h"
#include "Management.h"
#include "ServiceName.h"
#include "FirmwareIdentity.h"
#include <Utils.h>
#include "TelemetryService.h"
#include <nvs.h>
#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include <target.h>
#include <cmath>
#include <inttypes.h>
#ifdef ARDUINO_ARCH_ESP32
#include <WiFi.h>
#include "RadioNetwork.h"
#include "EspFieldUpdate.h"
#endif

namespace onchip {
bool mastRecord(const char *key, void *data, size_t size, bool write, bool &present) {
  nvs_handle_t handle;
  auto result = nvs_open("mc-mast-admin", write ? NVS_READWRITE : NVS_READONLY, &handle);
  present = false;
  if (!write && result == ESP_ERR_NVS_NOT_FOUND) return true;
  if (result != ESP_OK) {
    Serial.println("Mast admin storage open failed");
    return false;
  }
  size_t length = size;
  if (write) {
    result = nvs_set_blob(handle, key, data, size);
    if (result == ESP_OK) result = nvs_commit(handle);
  } else {
    result = nvs_get_blob(handle, key, data, &length);
  }
  nvs_close(handle);
  if (!write && result == ESP_ERR_NVS_NOT_FOUND) return true;
  if (result != ESP_OK || length != size) {
    Serial.println("Mast admin storage read/commit failed");
    return false;
  }
  present = true;
  return true;
}
namespace {
MastAdmin *activeAdmin = nullptr;
void clearSecret(void *data, size_t size) {
  auto *bytes = static_cast<volatile uint8_t *>(data);
  while (size--) *bytes++ = 0;
}
struct PasswordRecord {
  uint8_t magic[4] = {'M', 'P', 'W', 1};
  char password[16]{};
  uint8_t digest[32]{};
};
static_assert(sizeof(PasswordRecord) == 52, "Management password record layout changed");
bool loadPassword(PasswordRecord &record, bool &present) {
  if (!mastRecord("password", &record, sizeof(record), false, present)) return false;
  if (!present) return true;
  uint8_t digest[32];
  mesh::Utils::sha256(digest, sizeof(digest), reinterpret_cast<const uint8_t *>(&record),
                      offsetof(PasswordRecord, digest));
  bool valid = !memcmp(record.magic, "MPW\1", 4) &&
               record.password[0] && memchr(record.password, 0, sizeof(record.password)) &&
               !memcmp(digest, record.digest, sizeof(digest));
  if (valid) for (const unsigned char *p = reinterpret_cast<const unsigned char *>(record.password); *p; ++p)
    valid = valid && *p >= 32 && *p <= 126;
  clearSecret(digest, sizeof(digest));
  if (!valid) Serial.println("Mast password record invalid; password login disabled");
  return valid;
}
bool hex(const char *text, uint8_t *out, size_t size) {
  if (strlen(text) != size * 2) return false;
  for (size_t i = 0; i < size; ++i) {
    unsigned value = 0;
    for (unsigned j = 0; j < 2; ++j) {
      const char c = text[2 * i + j];
      const int n = c >= '0' && c <= '9' ? c - '0' :
                    c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                    c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
      if (n < 0) return false;
      value = value * 16 + n;
    }
    out[i] = value;
  }
  return true;
}
bool number(const char *text, uint32_t &value) {
  if (!*text) return false;
  uint64_t n = 0;
  for (; *text; ++text) {
    if (*text < '0' || *text > '9') return false;
    n = n * 10 + (*text - '0');
    if (n > UINT32_MAX) return false;
  }
  value = n;
  return true;
}
bool wifiPassword(const char *text) {
  const size_t size = strlen(text);
  if (size == 0 || (size >= 8 && size <= 63)) return true;
  uint8_t key[32];
  return size == 64 && hex(text, key, sizeof(key));
}
const char *commandUsage(const char *topic) {
  struct Help { const char *topic, *syntax; };
  static constexpr Help topics[] = {
    {"wifi", "wifi status|ssid HEX|password HEX|apply|forget; get wifi.FIELD; set wifi.ssid|pwd TEXT; set wifi.enabled 0|1; secrets: encrypted RF"},
    {"radio", "radio FREQ_HZ BW_HZ SF CR TX_DBM; read: get radio|get freq|get tx; shared PHY changes after reply"},
    {"tempradio", "tempradio SECONDS FREQ_HZ BW_HZ SF CR TX_DBM; duration 1..3600; restores saved PHY"},
    {"role", "role help; role config ROLE; role name ROLE [TEXT]; role advert ROLE zerohop; role key|channel|password ROLE ..."},
    {"roles", "roles MASK; MASK=0..15 (repeater=1,room=2,companion=4,observer=8); apply reboots; Management stays available"},
    {"key", "key ROLE [pending|cancel|HEX128]; use key help; private imports: encrypted RF only"},
    {"password", "password [help|HEX]; 1..15 printable bytes encoded as hex; changes: encrypted RF only"},
    {"source", "source help; source status|hash|metadata|helptext|api; source begin|chunk|commit|rollback|remove ..."},
    {"bot", "bot help; bot status|stats|radio|policy|mesh|name|discovery|adaptive|shared|reminders|events|forward|https ..."},
    {"auth", "auth status [KEY64]; auth peer 1..4; auth forget KEY64 (not the compiled owner)"},
    {"trust", "trust KEY64|none; one runtime trusted companion; native role ACLs unchanged"},
    {"data", "data help; data status|export|read|begin|chunk|stage|restore|cancel ..."},
    {"telemetry", "telemetry help; telemetry status|counts|times|tls|endpoint ...; network collector settings"},
    {"room", "room access; native Room settings belong to the Room contact"},
    {"companion", "companion stats|errors|help; native contacts/channels use the companion connection"},
    {"stats", "stats [radio|signal|tx|airtime|admission|sensors|memory|psram|bot|vm|observer|companion]; schema=1 key=value; counters since boot"},
    {"get", "get name|owner.info|radio|freq|tx|wifi.FIELD; WiFi fields: enabled,ssid,pwd,ip,status"},
    {"set", "set name TEXT; set owner.info TEXT (| separates lines); set wifi.ssid|pwd TEXT; set wifi.enabled 0|1"}
  };
  for (const auto &entry : topics)
    if (!strcmp(topic, entry.topic)) return entry.syntax;
  return nullptr;
}
struct OwnerInfoRecord {
  uint32_t version = 1;
  char text[MastAdmin::OwnerInfoLimit + 1]{};
};
char *word(char *&cursor) {
  while (*cursor == ' ') ++cursor;
  char *value = cursor;
  while (*cursor && *cursor != ' ') ++cursor;
  if (*cursor) *cursor++ = 0;
  while (*cursor == ' ') ++cursor;
  return value;
}
bool role(const char *name, Role &value) {
  if (!strcmp(name, "repeater")) value = Role::Repeater;
  else if (!strcmp(name, "room")) value = Role::Room;
  else if (!strcmp(name, "companion")) value = Role::Companion;
  else return false;
  return true;
}
void fingerprint(const uint8_t key[16], char out[17]) {
  uint8_t digest[8];
  mesh::Utils::sha256(digest, sizeof(digest), key, 16);
  for (unsigned i = 0; i < 8; ++i) snprintf(out + 2 * i, 3, "%02x", digest[i]);
}
}
void MastAdmin::roleCommand(char *command, Reply &reply, Transport transport,
                            const uint8_t *nativeSender, uint32_t invokingBotJob) {
  char *cursor = command;
  const char *operation = word(cursor), *name = word(cursor);
  if (!strcmp(operation, "help") && !*name) {
    strcpy(reply.text, "role config|name|advert|key|channel ROLE ...; role password repeater|room HEX (authenticated encrypted RF only); role config ROLE reports support");
    return;
  }
  if (!*name) {
    snprintf(reply.text, sizeof(reply.text), "Error: usage: role %s ROLE ...; use help role", operation);
    return;
  }
  if (!strcmp(operation, "password")) {
    if (transport != Transport::NativeEncrypted || invokingBotJob ||
        !management_->authenticatedNativeSender(nativeSender)) {
      strcpy(reply.text, "Error: role password requires authenticated encrypted Management RF; web/Lua denied");
      return;
    }
    Role target = Role::Repeater;
    if (!role(name, target) || (target != Role::Repeater && target != Role::Room)) {
      strcpy(reply.text, "Error: administrator password ROLE is repeater or room"); return;
    }
    char password[16]{};
    const size_t size = strlen(cursor);
    bool valid = size >= 2 && size <= 30 && !(size & 1) &&
                 hex(cursor, reinterpret_cast<uint8_t *>(password), size / 2);
    for (size_t i = 0; valid && i < size / 2; ++i)
      valid = uint8_t(password[i]) >= 32 && uint8_t(password[i]) <= 126;
    if (!valid) {
      clearSecret(password, sizeof(password));
      strcpy(reply.text, "Error: role password needs hex for 1..15 printable ASCII bytes"); return;
    }
    const auto outcome = setNativeRolePassword(target, password);
    clearSecret(password, sizeof(password));
    if (outcome == RolePasswordUpdate::SavedApplied)
      snprintf(reply.text, sizeof(reply.text),
               "Saved and applied %s administrator password; ACL/sessions unchanged", name);
    else
      strcpy(reply.text, outcome == RolePasswordUpdate::Unavailable ?
             "Error: role inactive/busy; password unchanged" :
             "Error: role password persistence unknown; live unchanged; saved may differ; verify before retry");
    return;
  }
  const bool bot = !strcmp(name, "bot") || !strcmp(name, "command-bot");
  const bool management = !strcmp(name, "management"), kiss = !strcmp(name, "kiss");
  Role nativeRole = Role::Repeater;
  if (!bot && !management && !kiss && !role(name, nativeRole)) {
    strcpy(reply.text, "Error: ROLE is bot, repeater, room, companion, management or kiss"); return;
  }
  if (!strcmp(operation, "config") && !*cursor) {
    if (management || kiss) {
      snprintf(reply.text, sizeof(reply.text),
               "role=%s name=persistent/live key=%s channel-slots=0 advert=%s RF=%s",
               name, management ? "import-stage/reboot" : "unchanged",
               management ? "zerohop" : "none", management ? "shared" : "service-only");
      return;
    }
    const unsigned channels = bot ? 1 : nativeRole == Role::Companion ? companionChannelCount() : 0;
    snprintf(reply.text, sizeof(reply.text),
             "role=%s name=persistent/%s key=generate-stage/reboot channel-slots=%u channel-key-bits=%u RF=shared advert=zerohop",
             name, bot ? "live" : "active-role", channels, channels ? 128 : 0);
    return;
  }
  if (!strcmp(operation, "name")) {
    char actual[33]{};
    if (management || kiss) {
      if (!*cursor) {
        if (management) strcpy(actual, management_->name());
        else if (!kissName(actual)) { strcpy(reply.text, "Error: KISS name unavailable"); return; }
        snprintf(reply.text, sizeof(reply.text), "Name: %s", actual);
        return;
      }
      if (!validRuntimeName(cursor)) {
        strcpy(reply.text, "Error: name requires 1..31 printable bytes without colon"); return;
      }
      const bool saved = management ? management_->setName(cursor) : setKissName(cursor);
      strcpy(reply.text, saved ? "Saved and applied service name; identity unchanged" :
             "Error: name persistence failed; live name unchanged, saved state may be uncertain");
    } else if (bot) {
      BotMeshPolicy policy;
      if (!loadBotMeshPolicy(policy)) { strcpy(reply.text, "Error: saved bot name unavailable"); return; }
      if (!*cursor) { snprintf(reply.text, sizeof(reply.text), "Name: %s", policy.name); return; }
      if (strlen(cursor) > 31) { strcpy(reply.text, "Error: name requires 1..31 printable bytes without colon"); return; }
      strcpy(policy.name, cursor);
      if (!policy.valid()) { strcpy(reply.text, "Error: name requires 1..31 printable bytes without colon"); return; }
      strcpy(reply.text, commandBotService().setMeshPolicy(policy) ?
             "Saved and applied bot name; identity unchanged" : "Error: name persistence unknown; inspect bot mesh");
    } else if (lifecycleBusy(nativeRole)) {
      strcpy(reply.text, "Error: role inactive/busy; enable/apply before editing native preferences");
    } else if (!*cursor) {
      strcpy(reply.text, nativeRoleName(nativeRole, actual) ? "Name: " : "Error: native name unavailable");
      if (actual[0]) strncat(reply.text, actual, sizeof(reply.text) - strlen(reply.text) - 1);
    } else {
      strcpy(reply.text, setNativeRoleName(nativeRole, cursor) ?
             "Saved and applied native role name; identity unchanged" :
             "Error: invalid name or persistence failure; saved state may be uncertain");
    }
    return;
  }
  if (!strcmp(operation, "advert")) {
    if (strcmp(cursor, "zerohop")) {
      strcpy(reply.text, "Error: role advert ROLE zerohop"); return;
    }
    if (kiss) {
      strcpy(reply.text, "Error: KISS is a service label, not an advertised RF role"); return;
    }
    const bool queued = management ? management_->advertiseZeroHop() :
                        bot ? commandBotService().advertiseOwnerZeroHop() :
                              nativeRoleAdvertiseZeroHop(nativeRole);
    strcpy(reply.text, queued ? "Queued zero-hop advert; delivery/peer learning unconfirmed" :
           "Error: advert unavailable; role inactive, radio/queue busy or rate/airtime limit");
    return;
  }
  if (management || kiss) {
    strcpy(reply.text, "Error: service supports role config/name only; management also supports advert zerohop");
    return;
  }
  if (!strcmp(operation, "key")) {
    uint8_t key[32]{};
    const char *storedName = bot ? "command-bot" : roleName(nativeRole);
    const bool rotating = !strcmp(cursor, "rotate"), pending = !strcmp(cursor, "pending");
    if (*cursor && !rotating && !pending) {
      strcpy(reply.text, "Error: role key ROLE [pending|rotate]; no private key import/export"); return;
    }
    const bool ok = rotating ? rotateIdentity(storedName, key) : identityPublicKey(storedName, key, pending);
    if (!ok) {
      strcpy(reply.text, "Error: identity absent, pending change or persistence unknown; inspect role key ROLE pending"); return;
    }
    strcpy(reply.text, rotating ? "Pending " : pending ? "Pending " : "KEY ");
    const size_t offset = strlen(reply.text);
    for (unsigned i = 0; i < 32; ++i) snprintf(reply.text + offset + 2 * i, 3, "%02x", key[i]);
    if (rotating || pending) strcat(reply.text, "; reboot required; peers must learn new key");
    return;
  }
  if (!strcmp(operation, "channel")) {
    if (!bot && nativeRole != Role::Companion) {
      strcpy(reply.text, "Error: this native role has no application group-channel keys; identity keys are separate"); return;
    }
    uint32_t slot = 0;
    if (!number(word(cursor), slot) || slot >= (bot ? 1 : companionChannelCount())) {
      strcpy(reply.text, "Error: channel SLOT is 0 for bot; use role config companion for its slot count"); return;
    }
    BotRadioPolicy policy;
    if (bot && !loadBotRadioPolicy(policy)) { strcpy(reply.text, "Error: saved bot channel unavailable"); return; }
    if (!*cursor) {
      char channelName[33]{}, keyId[17] = "none";
      if (bot) {
        strcpy(channelName, policy.channel);
        if (policy.channelKeySet) fingerprint(policy.channelKey, keyId);
        else if (channelName[0]) strcpy(keyId, "hashtag");
      } else {
        uint8_t digest[8];
        if (!companionChannelInfo(slot, channelName, digest)) {
          strcpy(reply.text, "Error: companion inactive/busy or channel unavailable"); return;
        }
        if (channelName[0]) for (unsigned i = 0; i < 8; ++i) snprintf(keyId + 2 * i, 3, "%02x", digest[i]);
      }
      snprintf(reply.text, sizeof(reply.text), "Channel %u name=%s key-id=%s; %s", unsigned(slot),
               channelName[0] ? channelName : "off", keyId, bot ? "saved; reboot applies" : "applied/native-prefs");
      return;
    }
    char channelName[32]{};
    uint8_t key[16]{};
    if (strcmp(cursor, "off")) {
      const char *encodedName = word(cursor);
      const size_t size = strlen(encodedName) / 2;
      if (!size || size > 31 || !hex(encodedName, reinterpret_cast<uint8_t *>(channelName), size) ||
          !hex(cursor, key, sizeof(key))) {
        strcpy(reply.text, "Error: role channel ROLE SLOT NAMEHEX KEY32; name 1..31 bytes, key exactly 16 bytes"); return;
      }
      bool nonzero = false;
      for (auto byte : key) nonzero = nonzero || byte;
      for (size_t i = 0; i < size; ++i)
        if (channelName[i] < 32 || channelName[i] > 126) {
          strcpy(reply.text, "Error: channel name must be printable ASCII without NUL"); return;
        }
      if (!nonzero) { strcpy(reply.text, "Error: channel key must be nonzero; use off to disable"); return; }
    }
    bool ok;
    if (bot) {
      strcpy(policy.channel, channelName);
      policy.channelKeySet = channelName[0] != 0;
      memcpy(policy.channelKey, key, sizeof(key));
      ok = saveBotRadioPolicy(policy);
    } else ok = companionSetChannel(slot, channelName, key);
    memset(key, 0, sizeof(key));
    strcpy(reply.text, ok ?
           (bot ? "Saved bot channel/key; reboot required; channel storage follows the key" :
                  "Saved and applied companion channel/key; no identity or PHY change") :
           "Error: role inactive/busy or channel persistence failed; saved state may be uncertain");
    return;
  }
  strcpy(reply.text, "Error: use role help");
}
bool MastAdmin::loadWifi(WifiCredentials &credentials, bool &present) {
  if (!publicProvisioningReady()) {
    Serial.println("Public setup unavailable; WiFi credentials disabled");
    present = false;
    credentials = {};
    return false;
  }
  Settings settings;
  if (!mastRecord("settings", &settings, sizeof(settings), false, present) ||
      (present && (settings.version != 1 || settings.wifiSet > 1 ||
                   !memchr(settings.wifi.ssid, 0, sizeof(settings.wifi.ssid)) ||
                   !memchr(settings.wifi.password, 0, sizeof(settings.wifi.password)) ||
                   !wifiPassword(settings.wifi.password))))
    return false;
  present = present && settings.wifiSet;
  credentials = settings.wifi;
#if defined(MESHCORE_PUBLIC_PROVISIONING) && MESHCORE_PUBLIC_PROVISIONING
  if (!present) {
    strcpy(credentials.ssid, publicProvisioning().wifiSsid);
    strcpy(credentials.password, publicProvisioning().wifiPassword);
    present = true;
  }
#endif
  return true;
}
bool MastAdmin::loadWifiEnabled(bool &enabled) {
  if (!publicProvisioningReady()) {
    enabled = false;
    Serial.println("Public setup unavailable; WiFi join disabled");
    return false;
  }
  uint8_t value = 1;
  bool present;
  if (!mastRecord("wifi-enabled", &value, sizeof(value), false, present) || value > 1) {
    Serial.println("Saved WiFi enable setting invalid; use set wifi.enabled 0|1");
    return false;
  }
#if defined(MESHCORE_PUBLIC_PROVISIONING) && MESHCORE_PUBLIC_PROVISIONING
  enabled = present ? value != 0 : publicProvisioning().wifiEnabled != 0;
#else
  enabled = value != 0;
#endif
  return true;
}
bool MastAdmin::ownerInfo(char *text, size_t size) const {
  OwnerInfoRecord record;
  bool present;
  if (!mastRecord("owner-info", &record, sizeof(record), false, present) ||
      record.version != 1 || !memchr(record.text, 0, sizeof(record.text))) {
    Serial.println("Management owner information unavailable");
    return false;
  }
  for (const unsigned char *p = reinterpret_cast<const unsigned char *>(record.text); *p; ++p)
    if ((*p < 32 && *p != '\n') || *p > 126) {
      Serial.println("Management owner information invalid");
      return false;
    }
  if (strlen(record.text) >= size) return false;
  strcpy(text, record.text);
  return true;
}
void MastAdmin::preferenceCommand(char *command, Reply &reply, Transport transport,
                                  uint32_t invokingBotJob) {
  if (!strncmp(command, "get wifi.", 9) || !strncmp(command, "set wifi.", 9)) {
    wifiCommand(command, reply, transport, invokingBotJob); return;
  }
  if (!strcmp(command, "get name")) {
    snprintf(reply.text, sizeof(reply.text), "> %s", management_->name());
  } else if (!strncmp(command, "set name ", 9)) {
    strcpy(reply.text, management_->setName(command + 9) ? "OK - saved Management name; identity unchanged" :
           "Error: invalid name or persistence failure; use role name management to inspect");
  } else if (!strcmp(command, "get owner.info")) {
    char text[OwnerInfoLimit + 1];
    if (!ownerInfo(text, sizeof(text))) {
      strcpy(reply.text, "Error: Management owner information unavailable"); return;
    }
    for (char *p = text; *p; ++p) if (*p == '\n') *p = '|';
    snprintf(reply.text, sizeof(reply.text), "> %s", text);
  } else if (!strncmp(command, "set owner.info ", 15)) {
    OwnerInfoRecord next, actual;
    if (strlen(command + 15) > OwnerInfoLimit) {
      strcpy(reply.text, "Error: owner.info exceeds 119 bytes; use | between lines"); return;
    }
    strcpy(next.text, command + 15);
    for (char *p = next.text; *p; ++p) if (*p == '|') *p = '\n';
    bool present;
    const bool saved = mastRecord("owner-info", &next, sizeof(next), true, present) &&
                       mastRecord("owner-info", &actual, sizeof(actual), false, present) &&
                       present && !memcmp(&next, &actual, sizeof(next));
    strcpy(reply.text, saved ? "OK - saved Management owner information" :
           "Error: owner.info commit/readback unknown; inspect get owner.info before retry");
  } else if (!strcmp(command, "get radio") || !strcmp(command, "get freq") ||
             !strcmp(command, "get tx")) {
    const auto p = mux_->currentConfiguration();
    if (!strcmp(command, "get tx"))
      snprintf(reply.text, sizeof(reply.text), "> %u", unsigned(p.tx_power));
    else if (!strcmp(command, "get freq"))
      snprintf(reply.text, sizeof(reply.text), "> %.7f", double(p.freq_hz) / 1e6);
    else
      snprintf(reply.text, sizeof(reply.text), "> %.7f,%.3f,%u,%u",
               double(p.freq_hz) / 1e6, double(p.bw_hz) / 1000, unsigned(p.sf), unsigned(p.cr));
  } else if (!strncmp(command, "set radio", 9) || !strncmp(command, "set freq", 8) ||
             !strncmp(command, "set tx", 6)) {
    strcpy(reply.text, "Error: use radio FREQ_HZ BW_HZ SF CR TX_DBM; changes affect every role");
  } else {
    const char *verb = !strncmp(command, "get", 3) ? "get" : "set";
    snprintf(reply.text, sizeof(reply.text), "Error: Management setting unavailable; use help %s", verb);
  }
}
void MastAdmin::wifiCommand(char *command, Reply &reply, Transport transport,
                            uint32_t invokingBotJob) {
  const bool get = !strncmp(command, "get wifi.", 9);
  const char *field = command + 9;
  const char *space = strchr(field, ' ');
  const size_t fieldSize = space ? size_t(space - field) : strlen(field);
  const auto matches = [&](const char *name) {
    return strlen(name) == fieldSize && !strncmp(field, name, fieldSize);
  };
  const bool password = matches("pwd");
  if ((password || !get) && (transport != Transport::NativeEncrypted || invokingBotJob)) {
    strcpy(reply.text, "Error: WiFi secrets and setters require encrypted Management RF; web/Lua denied"); return;
  }
  if (get) {
    if (space) { strcpy(reply.text, "Error: usage: get wifi.enabled|ssid|pwd|ip|status"); return; }
    if (matches("enabled")) {
      bool enabled;
      if (!loadWifiEnabled(enabled)) { strcpy(reply.text, "Error: saved WiFi enable setting unavailable"); return; }
      snprintf(reply.text, sizeof(reply.text), "> %u", unsigned(enabled)); return;
    }
    if (matches("ssid") || password) {
      WifiCredentials credentials;
      bool saved;
      if (!loadWifi(credentials, saved)) {
        strcpy(reply.text, "Error: saved WiFi credentials unavailable"); return;
      }
      if (saved)
        snprintf(reply.text, sizeof(reply.text), "> %s", password ? credentials.password : credentials.ssid);
#ifdef ARDUINO_ARCH_ESP32
      else if (WIFI_SSID[0])
        snprintf(reply.text, sizeof(reply.text), "> %s", password ? WIFI_PWD : WIFI_SSID);
      else
        snprintf(reply.text, sizeof(reply.text), "> %s", password ? WiFi.psk().c_str() : WiFi.SSID().c_str());
#else
      else strcpy(reply.text, "Error: WiFi station credentials unavailable");
#endif
      clearSecret(&credentials, sizeof(credentials)); return;
    }
    if (matches("ip") || matches("status")) {
#ifdef ARDUINO_ARCH_ESP32
      if (matches("ip"))
        snprintf(reply.text, sizeof(reply.text), "> %s", WiFi.localIP().toString().c_str());
      else
        snprintf(reply.text, sizeof(reply.text), "> %u", unsigned(WiFi.status()));
#else
      strcpy(reply.text, "Error: WiFi connection readback unavailable on this target");
#endif
      return;
    }
  } else if (space) {
    const char *value = space + 1;
    if (matches("enabled")) {
      const bool enable = !strcmp(value, "1") || !strcmp(value, "on");
      if (!enable && strcmp(value, "0") && strcmp(value, "off")) {
        strcpy(reply.text, "Error: usage: set wifi.enabled 0|1 (or off|on); disabling WiFi leaves encrypted RF administration available"); return;
      }
      if (!reserve(Effect::Wifi, reply)) return;
      uint8_t enabled = enable, actual = 2;
      bool present;
      if (!mastRecord("wifi-enabled", &enabled, sizeof(enabled), true, present) ||
          !mastRecord("wifi-enabled", &actual, sizeof(actual), false, present) ||
          !present || actual != enabled) {
        effect_ = Effect::None; reply.ticket = 0;
        strcpy(reply.text, "Error: WiFi enable commit/readback unknown; inspect get wifi.enabled before retry"); return;
      }
      strcpy(reply.text, enabled ? "OK - saved WiFi enabled; reconnect after reply" :
             "OK - saved WiFi disabled; disconnect after reply; encrypted RF administration remains");
      return;
    }
    if (matches("ssid") || password) {
      Settings next = settings_, actual;
      const size_t size = strlen(value);
      if ((password && !wifiPassword(value)) || (!password && (!size || size > 32))) {
        strcpy(reply.text, password ? "Error: wifi.pwd needs 0 or 8..63 bytes, or 64 ASCII hex digits" :
               "Error: wifi.ssid needs 1..32 bytes"); return;
      }
      strcpy(password ? next.wifi.password : next.wifi.ssid, value);
      next.wifiSet = next.wifi.ssid[0] != 0;
      bool present;
      const bool saved = mastRecord("settings", &next, sizeof(next), true, present) &&
                         mastRecord("settings", &actual, sizeof(actual), false, present) &&
                         present && !memcmp(&next, &actual, sizeof(next));
      if (saved) settings_ = next;
      clearSecret(&next, sizeof(next)); clearSecret(&actual, sizeof(actual));
      strcpy(reply.text, saved ? "OK - saved WiFi field; use wifi apply" :
             "Error: WiFi field commit/readback unknown; inspect saved state before retry");
      return;
    }
  }
  strcpy(reply.text, "Error: usage: get wifi.enabled|ssid|pwd|ip|status; set wifi.ssid|pwd TEXT; set wifi.enabled 0|1");
}
bool MastAdmin::begin(WifiKissMultiplexer &mux, uint8_t mask, ProfileJournal &journal,
                      Management &management) {
  activeAdmin = this;
  mux_ = &mux;
  management_ = &management;
  journal_ = &journal;
  appliedMask_ = mask;
  bool present;
  WifiCredentials wifi;
  if (!mastRecord("settings", &settings_, sizeof(settings_), false, present) ||
      settings_.version != 1 || settings_.trustedSet > 1 || !loadWifi(wifi, present)) {
    settings_ = {};
    strcpy(outcome_, "Error: settings invalid; native password/compiled-key recovery required");
    Serial.println(outcome_);
  }
  ready_ = mastRecord("replay", &replay_, sizeof(replay_), false, present) &&
           replay_.version == 1 &&
           mastRecord("owner-replay", &ownerReplay_, sizeof(ownerReplay_), false, present) &&
           ownerReplay_.version == 1;
  if (!ready_) {
    strcpy(outcome_, "Error: mast replay storage invalid; native administration disabled");
    Serial.println(outcome_);
  }
  if (!source_.begin())
    Serial.println("Mast source recovery required; native administration remains available");
  return ready_;
}
bool MastAdmin::passwordMatches(const char *password) {
  if (!publicProvisioningReady() || !password) return false;
  PasswordRecord record;
  bool present;
  if (!loadPassword(record, present)) {
    clearSecret(&record, sizeof(record)); return false;
  }
  const char *expected = present ? record.password : mastPassword();
  const size_t n = strlen(expected), size = strlen(password);
  unsigned difference = unsigned(n ^ size) | unsigned(!n);
  for (size_t i = 0; i < n; ++i)
    difference |= uint8_t(expected[i]) ^ (i < size ? uint8_t(password[i]) : 0);
  clearSecret(&record, sizeof(record));
  return difference == 0;
}
bool MastAdmin::trusted(const uint8_t key[32]) const {
  return (settings_.trustedSet && !memcmp(key, settings_.trustedKey, 32)) ||
         compiledTrusted(key);
}
bool MastAdmin::compiledTrusted(const uint8_t key[32]) const {
  uint8_t compiled[32];
  return publicProvisioningReady() && hex(trustedCompanionPublicKey(), compiled, 32) &&
         !memcmp(key, compiled, 32);
}
uint32_t MastAdmin::lastTimestamp(const uint8_t key[32]) const {
  uint32_t timestamp = !memcmp(ownerReplay_.peer.key, key, 32) ? ownerReplay_.peer.timestamp : 0;
  for (const auto &peer : replay_.peers)
    if (!memcmp(peer.key, key, 32) && peer.timestamp > timestamp) timestamp = peer.timestamp;
  return timestamp;
}
bool MastAdmin::rememberTimestamp(const uint8_t key[32], uint32_t timestamp) {
  if (!ready_ || !timestamp || timestamp <= lastTimestamp(key)) return false;
  if (compiledTrusted(key)) {
    OwnerReplay next;
    memcpy(next.peer.key, key, 32);
    next.peer.timestamp = timestamp;
    bool present;
    if (!mastRecord("owner-replay", &next, sizeof(next), true, present)) return false;
    ownerReplay_ = next;
    return true;
  }
  Replay next = replay_;
  Replay::Peer *slot = nullptr;
  for (auto &peer : next.peers)
    if (!memcmp(peer.key, key, 32)) { slot = &peer; break; }
    else if (!peer.timestamp && !slot) slot = &peer;
  if (!slot) {
    Serial.println("Mast authenticated principal capacity exhausted");
    return false;
  }
  memcpy(slot->key, key, 32);
  slot->timestamp = timestamp;
  bool present;
  if (!mastRecord("replay", &next, sizeof(next), true, present)) return false;
  replay_ = next;
  return true;
}
bool MastAdmin::reserve(Effect effect, Reply &reply) {
#ifdef ARDUINO_ARCH_ESP32
  if (espUpdateBusy()) {
    strcpy(reply.text, "Error: application update active; inspect /admin/update before changes");
    return false;
  }
#endif
  if (effect_ != Effect::None) {
    strcpy(reply.text, "Error: administration change pending");
    return false;
  }
  effect_ = effect;
  if (++nextTicket_ == 0) ++nextTicket_;
  ticket_ = reply.ticket = nextTicket_;
  admittedAt_ = millis();
  armed_ = false;
  strcpy(outcome_, "waiting for acceptance reply transmission");
  return true;
}
MastAdmin *MastAdmin::service() { return activeAdmin; }
void MastAdmin::stop() {
  if (activeAdmin == this) activeAdmin = nullptr;
  source_.stop();
}
void MastAdmin::passwordCommand(const char *argument, Reply &reply, Transport transport) {
  while (*argument == ' ') ++argument;
  if (!strcmp(argument, "help")) {
    strcpy(reply.text, "password HEX; 1..15 printable bytes; encrypted Management RF only; new logins apply now; reboot ends current sessions");
    return;
  }
  if (!*argument) {
    PasswordRecord record;
    bool present;
    const bool valid = loadPassword(record, present);
    clearSecret(&record, sizeof(record));
    if (!valid) strcpy(reply.text, "Error: password record unavailable; use trusted key to set a new password");
    else {
#if defined(MESHCORE_PUBLIC_PROVISIONING) && MESHCORE_PUBLIC_PROVISIONING
      const char *initialSource = "provisioned";
#else
      const char *initialSource = "compiled";
#endif
      snprintf(reply.text, sizeof(reply.text), "Password source=%s; changes require encrypted Management RF",
               present ? "runtime" : mastPassword()[0] ? initialSource : "disabled");
    }
    return;
  }
  if (transport != Transport::NativeEncrypted) {
    strcpy(reply.text, "Error: password changes require encrypted Management RF; web/Lua changes denied"); return;
  }
  PasswordRecord next, actual;
  const size_t length = strlen(argument);
  bool valid = length >= 2 && length <= 30 && !(length & 1) &&
               hex(argument, reinterpret_cast<uint8_t *>(next.password), length / 2);
  if (valid) for (size_t i = 0; i < length / 2; ++i)
    valid = valid && next.password[i] >= 32 && next.password[i] <= 126;
  if (!valid) {
    clearSecret(&next, sizeof(next)); clearSecret(&actual, sizeof(actual));
    strcpy(reply.text, "Error: password needs 2..30 hex digits encoding 1..15 printable bytes"); return;
  }
  mesh::Utils::sha256(next.digest, sizeof(next.digest), reinterpret_cast<const uint8_t *>(&next),
                      offsetof(PasswordRecord, digest));
  bool present;
  const bool saved = mastRecord("password", &next, sizeof(next), true, present) &&
                     loadPassword(actual, present) && present && !memcmp(&next, &actual, sizeof(next));
  clearSecret(&next, sizeof(next)); clearSecret(&actual, sizeof(actual));
  if (!saved) {
    Serial.println("Mast password commit/readback unknown");
    strcpy(reply.text, "Error: password commit/readback unknown; keep this session; verify login or use trusted key");
  } else strcpy(reply.text, "Saved password; applies to new logins; reboot ends current sessions; trusted keys unchanged");
}
void MastAdmin::keyCommand(const char *command, Reply &reply, Transport transport) {
  if (!strcmp(command, "help")) {
    strcpy(reply.text, "key ROLE [pending|cancel|HEX128]; ROLE=bot,repeater,room,companion,management; private import: encrypted Management RF only");
    return;
  }
  const size_t length = strcspn(command, " ");
  char name[12]{};
  if (!length || length >= sizeof(name)) {
    strcpy(reply.text, "Error: key ROLE [pending|cancel|HEX128]; use key help"); return;
  }
  memcpy(name, command, length);
  const char *argument = command + length;
  while (*argument == ' ') ++argument;
  Role nativeRole = Role::Repeater;
  const char *storedName = !strcmp(name, "bot") || !strcmp(name, "command-bot") ? "command-bot" :
                           !strcmp(name, "management") ? "management" :
                           role(name, nativeRole) ? roleName(nativeRole) : nullptr;
  if (!storedName) {
    strcpy(reply.text, "Error: key ROLE is bot, repeater, room, companion or management"); return;
  }
  uint8_t publicKey[32]{};
  const bool pending = !strcmp(argument, "pending"), cancel = !strcmp(argument, "cancel");
  IdentityChange result;
  if (!*argument || pending) {
    if (!identityPublicKey(storedName, publicKey, pending)) {
      strcpy(reply.text, "Error: identity or pending key unavailable"); return;
    }
    result = pending ? IdentityChange::Staged : IdentityChange::Active;
  } else if (cancel) {
    result = cancelIdentityChange(storedName, publicKey);
  } else {
    if (transport != Transport::NativeEncrypted) {
      strcpy(reply.text, "Error: private identity import requires encrypted Management RF; web/Lua import denied"); return;
    }
    uint8_t privateKey[PRV_KEY_SIZE]{};
    const bool valid = hex(argument, privateKey, sizeof(privateKey));
    result = valid ? importIdentity(storedName, privateKey, publicKey) : IdentityChange::Rejected;
    clearSecret(privateKey, sizeof(privateKey));
    if (!valid) {
      strcpy(reply.text, "Error: import needs exactly 128 hex digits of a native 64-byte private identity"); return;
    }
  }
  if (result == IdentityChange::Rejected) {
    strcpy(reply.text, "Error: invalid private identity or role identity unavailable"); return;
  }
  if (result == IdentityChange::Conflict) {
    strcpy(reply.text, "Error: different pending identity or role reset; inspect pending key before cancelling"); return;
  }
  if (result == IdentityChange::Duplicate) {
    strcpy(reply.text, "Error: public identity already active or pending for another role; no change"); return;
  }
  if (result == IdentityChange::Unknown) {
    strcpy(reply.text, "Error: identity commit/readback unknown; inspect pending key before reboot"); return;
  }
  strcpy(reply.text, result == IdentityChange::Staged ? "Pending " : "KEY ");
  const size_t offset = strlen(reply.text);
  for (unsigned i = 0; i < sizeof(publicKey); ++i)
    snprintf(reply.text + offset + 2 * i, 3, "%02x", publicKey[i]);
  if (result == IdentityChange::Staged) strcat(reply.text, "; reboot required; peers must learn new key");
  else if (cancel) strcat(reply.text, "; no pending change; running key unchanged");
  else if (*argument) strcat(reply.text, "; already active; no reboot required");
}
void MastAdmin::statsCommand(const char *topic, Reply &reply) {
  auto format = [&](const char *pattern, auto... values) {
    const int length = snprintf(reply.text, sizeof(reply.text), pattern, values...);
    if (length < 0 || length > 130)
      strcpy(reply.text, "Error: stats response exceeds encrypted CLI capacity");
  };
  if (!strcmp(topic, "help")) {
    snprintf(reply.text, sizeof(reply.text), "%s", commandUsage("stats"));
  } else if (!strcmp(topic, "sensors")) {
    char battery[12] = "unavailable", temperature[24] = "unavailable";
    const auto mv = board.getBattMilliVolts();
    const auto celsius = board.getMCUTemperature();
    if (mv) snprintf(battery, sizeof(battery), "%u", unsigned(mv));
    if (std::isfinite(celsius)) {
      const int size = snprintf(temperature, sizeof(temperature), "%.2f", double(celsius));
      if (size < 0 || size_t(size) >= sizeof(temperature)) {
        strcpy(reply.text, "Error: MCU temperature exceeds stats representation"); return;
      }
    }
    format("schema=1 scope=device battery_mv=%s mcu_temp_c=%s", battery, temperature);
  } else if (!strcmp(topic, "radio")) {
    RadioDashboard::Totals totals;
    RadioDashboard::RadioStatus status;
    if (!mux_->dashboardTotals(totals, &status)) {
      strcpy(reply.text, "Error: shared modem counter snapshot unavailable"); return;
    }
    format("schema=1 scope=modem rx_packets=%" PRIu64 " rx_errors=%u queued=%u transmitting=%u",
           totals.rx_packets, status.rx_errors,
           unsigned(status.queued), unsigned(status.transmitting));
  } else if (!strcmp(topic, "signal")) {
    const auto &radio = mux_->physicalRadio();
    RadioDashboard::Totals totals;
    if (!mux_->dashboardTotals(totals) || !totals.rx_packets ||
        !std::isfinite(radio.getLastRSSI()) || !std::isfinite(radio.getLastSNR())) {
      strcpy(reply.text, "Error: RF packet signal unavailable before first reception"); return;
    }
    char noise[16] = "unavailable";
    if (radio.getNoiseFloor()) snprintf(noise, sizeof(noise), "%d", radio.getNoiseFloor());
    format("schema=1 scope=modem last_rssi_dbm=%.2f last_snr_db=%.2f noise_floor_dbm=%s",
           double(radio.getLastRSSI()), double(radio.getLastSNR()), noise);
  } else if (!strcmp(topic, "tx") || !strcmp(topic, "admission") || !strcmp(topic, "airtime")) {
    RadioDashboard::Totals totals;
    if (!mux_->dashboardTotals(totals)) {
      strcpy(reply.text, "Error: shared modem counter snapshot unavailable"); return;
    }
    if (!strcmp(topic, "tx"))
      format("schema=1 scope=modem tx_confirmed=%" PRIu64 " tx_failed=%" PRIu64
             " tx_unknown=%" PRIu64,
             totals.tx_succeeded, totals.tx_failed, totals.tx_unknown);
    else if (!strcmp(topic, "airtime"))
      format("schema=1 scope=modem tx_rf_ms=%" PRIu64 " rx_estimated_ms=%" PRIu64,
             totals.tx_rf_ms, totals.rx_estimated_ms);
    else
      format("schema=1 scope=modem uptime_ms=%" PRIu64 " tx_accepted=%" PRIu64 " tx_rejected=%" PRIu64,
             totals.uptime_ms, totals.tx_accepted, totals.tx_rejected);
  } else if (!strcmp(topic, "memory") || !strcmp(topic, "psram")) {
#ifdef ARDUINO_ARCH_ESP32
    if (!strcmp(topic, "memory"))
      format("schema=1 scope=device heap_free_bytes=%u heap_min_bytes=%u heap_largest_bytes=%u",
             ESP.getFreeHeap(), ESP.getMinFreeHeap(), ESP.getMaxAllocHeap());
    else
      format("schema=1 scope=device psram_total_bytes=%u psram_free_bytes=%u psram_min_bytes=%u",
             ESP.getPsramSize(), ESP.getFreePsram(), ESP.getMinFreePsram());
#else
    strcpy(reply.text, "Error: device allocator measurements unavailable");
#endif
  } else if (!strcmp(topic, "observer")) {
    observerStatistics(reply.text, sizeof(reply.text));
  } else if (!strcmp(topic, "companion")) {
    const auto s = companionSessions().stats();
    format("schema=1 scope=companion clients=%u accepted=%u rejected=%u dropped=%u",
           s.connectedClients, s.acceptedClients, s.rejectedClients, s.droppedClients);
  } else if (!strcmp(topic, "bot") || !strcmp(topic, "vm")) {
    auto &bot = commandBotService();
    if (!bot.publicKey()) { strcpy(reply.text, "Error: command bot is not running"); return; }
    const auto &s = bot.counters();
    if (!strcmp(topic, "bot"))
      format("schema=1 scope=bot jobs=%u replies=%u rejected=%u vm_failures=%u",
             bot.jobsInUse(), s.replies, s.rejected, s.vmFailures);
    else if (s.lastVm.peakBytes)
      format("schema=1 scope=last_vm peak_bytes=%zu instructions=%u elapsed_us=%" PRIu64 " stack_free_bytes=%u",
             s.lastVm.peakBytes, s.lastVm.instructions, s.lastVm.elapsedUs, s.lastVm.stackHighWaterBytes);
    else strcpy(reply.text, "Error: Lua execution measurements unavailable before first execution");
  } else {
    strcpy(reply.text, "Error: unknown stats topic; use stats help");
  }
}
void MastAdmin::execute(const char *input, Reply &reply, uint32_t invokingBotJob,
                        Transport transport, const uint8_t *nativeSender) {
  reply = {};
  if (!ready_) {
    strcpy(reply.text, "Error: mast replay storage invalid; administration disabled"); return;
  }
  if (!input || strlen(input) > TextLimit) {
    strcpy(reply.text, "Error: CLI text exceeds 162 bytes"); return;
  }
  for (const unsigned char *p = reinterpret_cast<const unsigned char *>(input); *p; ++p)
    if (*p < 32 || *p > 126) {
      strcpy(reply.text, "Error: printable CLI text required"); return;
    }
  if (!strcmp(input, "help") || !strncmp(input, "help ", 5)) {
    if (!input[4]) {
      static constexpr char Help[] = "status; stats; ver; board; help TOPIC; role; key; password; bot help; bot https; source; wifi; radio; get; set; auth; data; telemetry";
      static_assert(sizeof(Help) <= TextLimit - 17 + 1, "Help must fit a nonce-tagged native reply");
      strcpy(reply.text, Help);
    } else {
      const char *topic = input + 5;
      while (*topic == ' ') ++topic;
      const char *usage = commandUsage(topic);
      if (usage) snprintf(reply.text, sizeof(reply.text), "%s", usage);
      else snprintf(reply.text, sizeof(reply.text), "Error: unknown help topic; use help");
    }
    return;
  }
  if (const char *usage = commandUsage(input)) {
    if (strcmp(input, "password") && strcmp(input, "telemetry") && strcmp(input, "stats")) {
      snprintf(reply.text, sizeof(reply.text), "Error: usage: %s", usage); return;
    }
  }
  if (!strcmp(input, "wifi ssid") || !strcmp(input, "wifi password") ||
      !strcmp(input, "set wifi.ssid") || !strcmp(input, "set wifi.pwd") ||
      !strcmp(input, "set wifi.enabled")) {
    snprintf(reply.text, sizeof(reply.text), "Error: usage: %s", commandUsage("wifi")); return;
  }
  if (!strcmp(input, "set name") || !strcmp(input, "set owner.info")) {
    snprintf(reply.text, sizeof(reply.text), "Error: usage: %s TEXT; use help set", input); return;
  }
  if (!strncmp(input, "key ", 4)) {
    keyCommand(input + 4, reply, transport); return;
  }
  if (!strcmp(input, "password") || !strncmp(input, "password ", 9)) {
    passwordCommand(input + 8, reply, transport); return;
  }
  if (!strncmp(input, "bot https ", 10) && transport != Transport::NativeEncrypted) {
    const char *operation = input + 10;
    while (*operation == ' ') ++operation;
    if ((!strncmp(operation, "ca", 2) && (!operation[2] || operation[2] == ' ')) ||
        (!strncmp(operation, "token", 5) && (!operation[5] || operation[5] == ' '))) {
      strcpy(reply.text, "Error: HTTPS CA/token staging requires encrypted Management RF; web/Lua staging denied");
      return;
    }
  }
  char command[TextLimit + 1];
  strcpy(command, input);
  struct ClearCommand {
    char *command;
    ~ClearCommand() { clearSecret(command, TextLimit + 1); }
  } clear{command};
  if (!strcmp(command, "ver")) {
    snprintf(reply.text, sizeof(reply.text), "v%s (Build: %s)", ONCHIP_FIRMWARE_VERSION, __DATE__);
  } else if (!strcmp(command, "board")) {
    strcpy(reply.text, "ESP32; shared modem and on-device roles");
  } else if (!strcmp(command, "stats") || !strcmp(command, "get stats") ||
             !strncmp(command, "stats ", 6)) {
    statsCommand(!strncmp(command, "stats ", 6) ? command + 6 : "radio", reply);
  } else if (!strncmp(command, "get ", 4) || !strncmp(command, "set ", 4)) {
    preferenceCommand(command, reply, transport, invokingBotJob);
  } else if (!strcmp(command, "wifi help")) {
    snprintf(reply.text, sizeof(reply.text), "%s", commandUsage("wifi"));
  } else if (!strncmp(command, "source ", 7)) {
    source_.execute(command + 7, reply.text, sizeof(reply.text));
  } else if (!strncmp(command, "role ", 5)) {
    roleCommand(command + 5, reply, transport, nativeSender, invokingBotJob);
  } else if (!strcmp(command, "telemetry") || !strncmp(command, "telemetry ", 10)) {
    telemetryCommand(command[9] ? command + 10 : "", reply.text, sizeof(reply.text));
  } else if (!strncmp(command, "data ", 5)) {
    commandBotService().dataCommand(command + 5, reply.text, sizeof(reply.text));
  } else if (!strcmp(command, "bot https")) {
    strcpy(reply.text, "bot https status|discard|commit|endpoint|ops|ca|token|rpc|drop|unmap; ca/token NAME HEX <=128; grant: bot home on|off");
  } else if (!strncmp(command, "bot https ", 10)) {
    if (!botHttpsAdmin(command + 10, reply.text, sizeof(reply.text))) {
      char reason[sizeof(reply.text)];
      snprintf(reason, sizeof(reason), "%s",
               reply.text[0] ? reply.text : "Invalid configured HTTPS command");
      snprintf(reply.text, sizeof(reply.text), "Error: %.*s",
               int(sizeof(reply.text) - 8), reason);
    }
  } else if (!strcmp(command, "status")) {
    ProfileJournal saved;
    bool bot = false;
    if (!loadProfileJournal(saved) || !loadBotEnabled(bot)) {
      strcpy(reply.text, "Error: saved role state unavailable"); return;
    }
    *journal_ = saved;
    const auto p = mux_->currentConfiguration();
    snprintf(reply.text, sizeof(reply.text),
             "roles applied=%u saved=%u generation=%llu bot-saved=%u PHY=%u,%u,%u,%u,%u effective-gen=%u temp=%u",
             appliedMask_, saved.profile.enabled,
             static_cast<unsigned long long>(saved.generation), bot,
             p.freq_hz, p.bw_hz, p.sf, p.cr, p.tx_power,
             mux_->configurationGeneration(), temporary_);
  } else if (!strcmp(command, "job")) {
    snprintf(reply.text, sizeof(reply.text), "%s", outcome_);
  } else if (!strcmp(command, "companion stats")) {
    const auto s = companionSessions().stats();
    snprintf(reply.text, sizeof(reply.text),
             "TCP clients=%u accepted=%u rejected=%u dropped=%u messages=%u replayed=%u resets=%u",
             s.connectedClients, s.acceptedClients, s.rejectedClients, s.droppedClients,
             s.collectedMessages, s.replayedMessages, s.nativeResets);
  } else if (!strcmp(command, "companion errors")) {
    const auto s = companionSessions().stats();
    snprintf(reply.text, sizeof(reply.text),
             "TCP timeout=%u output-full=%u journal-full=%u malformed=%u native=%u fault=%u",
             s.timedOutClients, s.outputOverflows, s.journalOverruns, s.malformedFrames,
             s.nativeErrors, s.nativeFault);
  } else if (!strcmp(command, "companion help")) {
    strcpy(reply.text, "companion stats: TCP clients/messages since boot; companion errors: disconnect/native counters; neither clears state");
  } else if (!strcmp(command, "bot key")) {
    const auto *key = commandBotService().publicKey();
    if (!key) { strcpy(reply.text, "Error: command bot is disabled"); return; }
    strcpy(reply.text, "KEY ");
    for (unsigned i = 0; i < 32; ++i) snprintf(reply.text + 4 + 2 * i, 3, "%02x", key[i]);
  } else if (!strcmp(command, "bot admission")) {
    commandBotService().admissionStatus(reply.text, sizeof(reply.text));
  } else if (!strcmp(command, "bot diagnostics")) {
    commandBotService().diagnosticStatus(reply.text, sizeof(reply.text));
  } else if (!strcmp(command, "bot discovery") || !strncmp(command, "bot discovery ", 14)) {
    commandBotService().discoveryCommand(command[13] ? command + 14 : "", reply.text, sizeof(reply.text));
  } else if (!strcmp(command, "bot help")) {
    strcpy(reply.text, "bot status|stats|log|diagnostics|admission|policy|mesh|discovery|name|destination|channel-wait|shared|home|reminders|events|cancel; role help; source status; reboot");
  } else if (!strcmp(command, "bot cancel")) {
    commandBotService().cancelJobs(invokingBotJob);
    strcpy(reply.text, "Cancellation requested for other running commands/events; admitted effects may have committed; reminders unchanged");
  } else if (!strcmp(command, "bot log")) {
    RadioDashboard::RoleStatus status;
    commandBotService().dashboardStatus(status);
    snprintf(reply.text, sizeof(reply.text), "Current state=%s fault=%s; counters: bot stats",
             status.state, status.fault[0] ? status.fault : "none");
  } else if (!strcmp(command, "bot mesh")) {
    commandBotService().meshPolicyStatus(reply.text, sizeof(reply.text));
  } else if (!strcmp(command, "bot limits")) {
    const BotVmLimits limits;
    snprintf(reply.text, sizeof(reply.text),
             "Cooldown 0s; no command/min cap; 4 jobs (3 custom,1 event); 8 IO/job; VM load/init/active %llu/%llu/%llums; bundle %llums; TX airtime; reads 250..1250ms",
             static_cast<unsigned long long>(limits.loadWallUs / 1000),
             static_cast<unsigned long long>(limits.initWallUs / 1000),
             static_cast<unsigned long long>(limits.wallUs / 1000),
             static_cast<unsigned long long>(BotBundledLoadWallUs / 1000));
  } else if (!strcmp(command, "bot contention")) {
    const auto &stats = commandBotService().counters();
    snprintf(reply.text, sizeof(reply.text), "Read queries jittered=%u suppressed=%u; channel suppression best-effort, unauthenticated; writes require DM/target",
             stats.readJittered, stats.readSuppressed);
  } else if (!strcmp(command, "bot name") || !strncmp(command, "bot name ", 9) ||
             !strcmp(command, "bot destination") || !strncmp(command, "bot destination ", 16) ||
             !strcmp(command, "bot channel-wait") || !strncmp(command, "bot channel-wait ", 17)) {
    BotMeshPolicy policy;
    if (!loadBotMeshPolicy(policy)) { strcpy(reply.text, "Error: saved mesh policy unavailable"); return; }
    if (!strcmp(command, "bot name")) { snprintf(reply.text, sizeof(reply.text), "Bot name: %s", policy.name); return; }
    if (!strcmp(command, "bot channel-wait")) {
      snprintf(reply.text, sizeof(reply.text), "Channel follow-up waits saved=%u; selected channel only, no authenticated sender", policy.channelWait); return;
    }
    if (!strcmp(command, "bot destination")) {
      strcpy(reply.text, "bot destination SLOT [KEY64|off], slots 1..4; fresh authenticated direct route required"); return;
    }
    if (!strncmp(command, "bot name ", 9)) {
      if (!command[9] || strlen(command + 9) > 31) {
        strcpy(reply.text, "Error: bot name requires 1..31 printable bytes, no colon"); return;
      }
      strcpy(policy.name, command + 9);
    } else if (!strncmp(command, "bot channel-wait ", 17)) {
      if (strcmp(command + 17, "on") && strcmp(command + 17, "off")) {
        strcpy(reply.text, "Error: bot channel-wait on|off"); return;
      }
      policy.channelWait = !strcmp(command + 17, "on");
    } else {
      const char *arg = command + 16;
      if (*arg < '1' || *arg > '4' || (arg[1] && arg[1] != ' ')) {
        strcpy(reply.text, "Error: bot destination SLOT [KEY64|off], slots 1..4"); return;
      }
      auto *key = policy.destinations[*arg - '1'];
      if (!arg[1]) {
        strcpy(reply.text, "KEY ");
        for (unsigned i = 0; i < 32; ++i) snprintf(reply.text + 4 + 2 * i, 3, "%02x", key[i]);
        return;
      }
      if (!strcmp(arg + 2, "off")) memset(key, 0, 32);
      else {
        const bool valid = hex(arg + 2, key, 32);
        bool nonzero = false;
        for (unsigned i = 0; i < 32; ++i) nonzero = nonzero || key[i];
        if (!valid || !nonzero) {
          strcpy(reply.text, "Error: destination requires a nonzero full 64-hex public key or off"); return;
        }
      }
    }
    if (!policy.valid()) {
      strcpy(reply.text, "Error: name requires 1..31 printable bytes without colon; destination keys must be distinct"); return;
    }
    strcpy(reply.text, commandBotService().setMeshPolicy(policy) ?
           "Saved and applied bot mesh policy; running mesh grants fenced" :
           "Error: invalid mesh policy or commit/readback unknown; inspect bot mesh");
  } else if (!strcmp(command, "bot stats")) {
    const auto &stats = commandBotService().counters();
    snprintf(reply.text, sizeof(reply.text),
             "Replies=%u rejected=%u vm-fail=%u malformed=%u duplicate=%u notices=%u suppressed=%u",
             stats.replies, stats.rejected, stats.vmFailures, stats.malformed,
             stats.duplicates, stats.notices, stats.noticesSuppressed);
  } else if (!strcmp(command, "bot status")) {
    bool saved;
    if (!loadBotEnabled(saved)) { strcpy(reply.text, "Error: saved bot selection unavailable"); return; }
    RadioDashboard::RoleStatus status;
    commandBotService().dashboardStatus(status);
    snprintf(reply.text, sizeof(reply.text), "bot applied=%u saved=%u ready=%u state=%s",
             status.has_identity, saved, status.ready, status.state);
  } else if (!strcmp(command, "bot shared on") || !strcmp(command, "bot shared off")) {
    strcpy(reply.text, commandBotService().setSharedState(!strcmp(command, "bot shared on")) ?
           "Saved and applied shared KV policy" : "Error: shared KV policy commit failed");
  } else if (!strcmp(command, "bot shared")) {
    bool enabled;
    if (!loadBotSharedState(enabled)) strcpy(reply.text, "Error: shared KV policy unavailable");
    else snprintf(reply.text, sizeof(reply.text), "Shared KV %s", enabled ? "on" : "off");
  } else if (!strcmp(command, "bot home on") || !strcmp(command, "bot home off")) {
    strcpy(reply.text, commandBotService().setHomeAccess(!strcmp(command, "bot home on")) ?
           "Saved and applied home RPC grant" : "Error: home HTTPS configuration, SNTP trust or grant commit failed");
  } else if (!strcmp(command, "bot home")) {
    bool enabled;
    if (!loadBotHomeAccess(enabled)) strcpy(reply.text, "Error: home RPC policy unavailable");
    else snprintf(reply.text, sizeof(reply.text), "Home HTTPS configured=%u saved=%u applied=%u clock=%u",
                  botHttpsConfigured(), enabled, commandBotService().homeAccess(), botHttpsClockTrusted());
  } else if (!strcmp(command, "bot events")) {
    uint8_t mask = 0;
    if (!loadBotEventAccess(mask)) { strcpy(reply.text, "Error: event grant unavailable"); return; }
    const auto &stats = commandBotService().counters();
    snprintf(reply.text, sizeof(reply.text), "Events saved=%u subscribed=%u queued=%u dropped=%u completed=%u failed=%u",
             mask, commandBotService().eventMask(), stats.eventsQueued, stats.eventsDropped,
             stats.eventsCompleted, stats.eventsFailed);
  } else if (!strncmp(command, "bot events ", 11)) {
    uint32_t mask = 0;
    if (!number(command + 11, mask) || mask > 15) {
      strcpy(reply.text, "Error: bot events MASK 0..15; 1 startup, 2 connectivity, 4 message, 8 node_status"); return;
    }
    strcpy(reply.text, commandBotService().setEventAccess(uint8_t(mask)) ?
           "Saved event grants; inspect bot events for applied subscriptions" :
           "Error: event grant commit/readback failed; live event access disabled");
  } else if (!strcmp(command, "bot reminders") || !strncmp(command, "bot reminders ", 14)) {
    bool saved;
    if (!strcmp(command, "bot reminders")) {
      if (!loadBotReminderAccess(saved)) strcpy(reply.text, "Error: reminder grant unavailable");
      else snprintf(reply.text, sizeof(reply.text), "Personal reminders saved=%u applied=%u; private DM, direct route, no retries",
                    saved, commandBotService().reminderAccess());
    } else if (!strcmp(command, "bot reminders on") || !strcmp(command, "bot reminders off")) {
      saved = !strcmp(command + 14, "on");
      strcpy(reply.text, commandBotService().setReminderAccess(saved) ?
             "Saved reminder grant; off suspends pending jobs; inspect applied status" :
             "Error: reminder grant commit/readback failed; live access disabled");
    } else strcpy(reply.text, "Error: bot reminders [on|off]");
  } else if (!strcmp(command, "bot forward") || !strncmp(command, "bot forward ", 12)) {
    BotForwardPolicy policy;
    if (!strcmp(command, "bot forward off")) {
      strcpy(reply.text, commandBotService().setForwardPolicy(policy) ?
             "Saved and applied DM forwarding off" : "Error: forward revoke persistence failed; live access disabled");
    } else if (!strcmp(command, "bot forward") || !strcmp(command, "bot forward from") ||
               !strcmp(command, "bot forward to")) {
      if (!loadBotForwardPolicy(policy)) strcpy(reply.text, "Error: forward policy unavailable");
      else if (!strcmp(command, "bot forward"))
        snprintf(reply.text, sizeof(reply.text), "Forward DM saved=%u applied=%u; one authorized pair, direct route only",
                 policy.enabled(), commandBotService().forwardAccess());
      else {
        const auto *key = !strcmp(command, "bot forward from") ? policy.from : policy.to;
        strcpy(reply.text, "KEY ");
        for (unsigned i = 0; i < 32; ++i) snprintf(reply.text + 4 + 2 * i, 3, "%02x", key[i]);
      }
    } else {
      const char *keys = command + 12;
      if (strlen(keys) != 129 || keys[64] != ':') {
        strcpy(reply.text, "Error: bot forward FROM64:TO64 or off"); return;
      }
      char from[65]; memcpy(from, keys, 64); from[64] = 0;
      if (!hex(from, policy.from, 32) || !hex(keys + 65, policy.to, 32) ||
          !policy.enabled() || !policy.valid()) {
        strcpy(reply.text, "Error: distinct nonzero full public keys required"); return;
      }
      strcpy(reply.text, commandBotService().setForwardPolicy(policy) ?
             (commandBotService().forwardAccess() ? "Saved and applied authorized DM forwarding pair" :
              "Saved authorized DM forwarding pair; bot disabled") :
             "Error: forward policy not applied; inspect native fault");
    }
  } else if (!strcmp(command, "bot adaptive") || !strncmp(command, "bot adaptive ", 13)) {
    commandBotService().adaptiveCommand(command, reply.text, sizeof(reply.text));
  } else if (!strcmp(command, "bot policy") || !strncmp(command, "bot channel ", 12) ||
             !strncmp(command, "bot path ", 9) || !strncmp(command, "bot airtime ", 12)) {
    BotRadioPolicy policy;
    if (!loadBotRadioPolicy(policy)) {
      strcpy(reply.text, "Error: saved bot radio policy unavailable"); return;
    }
    if (!strcmp(command, "bot policy")) {
      snprintf(reply.text, sizeof(reply.text), "Saved channel=%s path-bytes=%u airtime-ms/min=%u; applied on boot",
               policy.channel[0] ? policy.channel : "off", policy.pathWidth, policy.airtimeMs);
      return;
    }
    if (!strncmp(command, "bot channel ", 12)) {
      const char *name = command + 12;
      if (!strcmp(name, "off")) name = "";
      if (strlen(name) >= sizeof(policy.channel)) {
        strcpy(reply.text, "Error: hashtag channel exceeds 32 bytes"); return;
      }
      strcpy(policy.channel, name);
      policy.channelKeySet = false; memset(policy.channelKey, 0, sizeof(policy.channelKey));
    } else {
      uint32_t value;
      const bool path = !strncmp(command, "bot path ", 9);
      if (!number(command + (path ? 9 : 12), value) ||
          (path ? value < 1 || value > 3 : value < 360 || value > 3600)) {
        strcpy(reply.text, "Error: bot path requires 1..3; airtime requires 360..3600 ms/min"); return;
      }
      if (path) policy.pathWidth = value;
      else policy.airtimeMs = value;
    }
    strcpy(reply.text, saveBotRadioPolicy(policy) ?
           "Saved bot radio policy; apply reboots" : "Error: invalid hashtag or bot radio policy commit failed");
  } else if (!strcmp(command, "role-path")) {
    snprintf(reply.text, sizeof(reply.text),
             "Role path bytes repeater=%u room=%u companion=%u management=%u (0=inactive)",
             repeaterPathWidth(), roomPathWidth(), companionPathWidth(), managementPathWidth());
  } else if (!strcmp(command, "room access")) {
    const int access = roomGuestAccess();
    strcpy(reply.text, access < 0 ? "Room inactive" :
           access ? "Room guest access open" : "Room guest access password-protected");
  } else if (!strncmp(command, "role-path ", 10)) {
    uint32_t width;
    if (!number(command + 10, width) || width < 1 || width > 3) {
      strcpy(reply.text, "Error: role-path requires 1..3 bytes"); return;
    }
    strcpy(reply.text, setNativeRolePathWidth(width) ?
           "Saved and applied active native role path widths" :
           "Error: native roles inactive/busy or path save failed; partial changes possible");
  } else if (!strncmp(command, "roles ", 6)) {
    uint32_t mask;
    ProfileJournal saved;
    if (!number(command + 6, mask) || mask > 15 ||
        !RoleProfile(mask).bootableWith(adminPassword(), roomPassword())) {
      strcpy(reply.text, "Error: roles requires bootable mask 0..15"); return;
    }
    if (!loadProfileJournal(saved) || saved.generation == UINT64_MAX) {
      strcpy(reply.text, "Error: role journal unavailable/exhausted"); return;
    }
    const ProfileJournal next{RoleProfile(uint8_t(mask)), saved.generation + 1,
                              (uint64_t(millis()) << 1) | 1};
    if (!commitProfileJournal(next)) {
      strcpy(reply.text, "Error: role journal commit failed"); return;
    }
    *journal_ = next;
    strcpy(reply.text, "Saved roles; apply reboots into the saved profile");
  } else if (!strcmp(command, "bot on") || !strcmp(command, "bot off")) {
    strcpy(reply.text, saveBotEnabled(!strcmp(command, "bot on")) ?
           "Saved bot selection; apply reboots" : "Error: bot selection commit failed");
  } else if (!strcmp(command, "apply") || !strcmp(command, "reboot")) {
    if (reserve(Effect::Reboot, reply))
      strcpy(reply.text, "Accepted reboot after this reply; saved roles apply on boot");
  } else if (!strcmp(command, "auth status") || !strncmp(command, "auth status ", 12)) {
    unsigned used = 0;
    for (const auto &peer : replay_.peers) if (peer.timestamp) ++used;
    if (!command[11]) {
      snprintf(reply.text, sizeof(reply.text),
               "RF peers=%u/4 compiled-owner=%u; auth peer 1..4; auth status KEY; auth forget KEY",
               used, ownerReplay_.peer.timestamp != 0);
    } else {
      uint8_t key[32];
      if (!hex(command + 12, key, 32)) {
        strcpy(reply.text, "Error: auth status requires a full 32-byte public key"); return;
      }
      snprintf(reply.text, sizeof(reply.text),
               "key=%s trusted=%u compiled=%u timestamp=%lu peers=%u/4",
               command + 12, trusted(key), compiledTrusted(key),
               static_cast<unsigned long>(lastTimestamp(key)), used);
    }
  } else if (!strncmp(command, "auth peer ", 10)) {
    uint32_t slot;
    if (!number(command + 10, slot) || slot < 1 || slot > 4) {
      strcpy(reply.text, "Error: auth peer requires slot 1..4"); return;
    }
    const auto &peer = replay_.peers[slot - 1];
    if (!peer.timestamp) {
      snprintf(reply.text, sizeof(reply.text), "RF peer %u empty", unsigned(slot));
    } else {
      char key[65];
      for (unsigned i = 0; i < sizeof(peer.key); ++i)
        snprintf(key + 2 * i, 3, "%02x", peer.key[i]);
      snprintf(reply.text, sizeof(reply.text), "RF peer %u key=%s timestamp=%lu trusted=%u",
               unsigned(slot), key, static_cast<unsigned long>(peer.timestamp), trusted(peer.key));
    }
  } else if (!strncmp(command, "auth forget ", 12)) {
    uint8_t key[32];
    if (!hex(command + 12, key, 32) || compiledTrusted(key)) {
      strcpy(reply.text, "Error: specify a non-compiled principal full key"); return;
    }
    Replay next = replay_;
    bool found = false;
    for (auto &peer : next.peers)
      if (peer.timestamp && !memcmp(peer.key, key, 32)) { peer = {}; found = true; }
    bool present;
    if (!found || !mastRecord("replay", &next, sizeof(next), true, present)) {
      strcpy(reply.text, "Error: principal absent or replay commit failed"); return;
    }
    replay_ = next;
    strcpy(reply.text, "Forgot principal replay state and session; fresh login required");
  } else if (!strncmp(command, "trust ", 6)) {
    Settings next = settings_;
    if (!strcmp(command + 6, "none")) {
      next.trustedSet = 0; memset(next.trustedKey, 0, 32);
    } else if (hex(command + 6, next.trustedKey, 32)) next.trustedSet = 1;
    else { strcpy(reply.text, "Error: trust requires a full 32-byte public key"); return; }
    bool present;
    if (!mastRecord("settings", &next, sizeof(next), true, present)) {
      strcpy(reply.text, "Error: trust commit failed"); return;
    }
    settings_ = next;
    strcpy(reply.text, "Saved mast companion trust; role-local ACLs unchanged");
  } else if (!strcmp(command, "wifi status")) {
    WifiCredentials credentials;
    bool saved;
    if (!loadWifi(credentials, saved)) {
      strcpy(reply.text, "Error: saved WiFi state unavailable"); return;
    }
#ifdef ARDUINO_ARCH_ESP32
    snprintf(reply.text, sizeof(reply.text), "wifi saved=%u ssid-bytes=%u connected=%u",
             saved, saved ? unsigned(strlen(credentials.ssid)) : 0, WiFi.status() == WL_CONNECTED);
#else
    snprintf(reply.text, sizeof(reply.text), "wifi saved=%u ssid-bytes=%u connectivity=unavailable",
             saved, saved ? unsigned(strlen(credentials.ssid)) : 0);
#endif
  } else if (!strcmp(command, "wifi forget")) {
    if (transport != Transport::NativeEncrypted || invokingBotJob) {
      strcpy(reply.text, "Error: WiFi setters require encrypted Management RF; web/Lua denied"); return;
    }
    auto next = settings_;
    next.wifiSet = 0; next.wifi = {};
    bool present;
    if (!mastRecord("settings", &next, sizeof(next), true, present)) {
      strcpy(reply.text, "Error: WiFi removal commit failed"); return;
    }
    settings_ = next;
    strcpy(reply.text, "Removed WiFi credentials override; enable setting unchanged; next enabled join uses compiled/SDK credentials");
  } else if (!strcmp(command, "wifi apply")) {
    bool enabled;
    if (!loadWifiEnabled(enabled)) {
      strcpy(reply.text, "Error: saved WiFi enable setting unavailable; use set wifi.enabled 0|1"); return;
    }
    if (!enabled) {
      strcpy(reply.text, "Error: WiFi disabled; use set wifi.enabled 1 to reconnect"); return;
    }
    if (reserve(Effect::Wifi, reply))
      strcpy(reply.text, "Accepted WiFi reconnect after this reply");
  } else if (!strncmp(command, "wifi ssid ", 10) || !strncmp(command, "wifi password ", 14)) {
    if (transport != Transport::NativeEncrypted || invokingBotJob) {
      strcpy(reply.text, "Error: WiFi secrets and setters require encrypted Management RF; web/Lua denied"); return;
    }
    const bool ssid = command[5] == 's';
    const char *value = command + (ssid ? 10 : 14);
    Settings next = settings_;
    char *destination = ssid ? next.wifi.ssid : next.wifi.password;
    const size_t maximum = ssid ? 32 : 64, n = strlen(value) / 2;
    memset(destination, 0, maximum + 1);
    if ((!ssid && !strcmp(value, "-"))) {
      // Explicit open network; an empty password is never inferred on errors.
    } else if (!n || n > maximum || !hex(value, reinterpret_cast<uint8_t *>(destination), n) ||
               strlen(destination) != n) {
      strcpy(reply.text, "Error: invalid bounded WiFi field"); return;
    }
    if (!wifiPassword(next.wifi.password)) {
      strcpy(reply.text, "Error: WPA password must be 8..63 bytes or 64 hex digits"); return;
    }
    next.wifiSet = next.wifi.ssid[0] != 0;
    bool present;
    if (!mastRecord("settings", &next, sizeof(next), true, present)) {
      strcpy(reply.text, "Error: WiFi field commit failed"); return;
    }
    settings_ = next;
    strcpy(reply.text, "Saved WiFi field; use wifi apply or reboot");
  } else if (!strncmp(command, "wifi ", 5)) {
    if (transport != Transport::NativeEncrypted || invokingBotJob) {
      strcpy(reply.text, "Error: WiFi secrets and setters require encrypted Management RF; web/Lua denied"); return;
    }
    char *ssid = command + 5, *password = strchr(ssid, ' ');
    Settings next = settings_;
    if (!password) { snprintf(reply.text, sizeof(reply.text), "Error: usage: %s", commandUsage("wifi")); return; }
    *password++ = 0;
    const size_t ssidSize = strlen(ssid) / 2, passSize = strlen(password) / 2;
    next.wifi = {};
    if (!ssidSize || ssidSize > 32 || passSize > 64 ||
        !hex(ssid, reinterpret_cast<uint8_t *>(next.wifi.ssid), ssidSize) ||
        (strcmp(password, "-") && (!passSize ||
          !hex(password, reinterpret_cast<uint8_t *>(next.wifi.password), passSize))) ||
        strlen(next.wifi.ssid) != ssidSize ||
        (strcmp(password, "-") && strlen(next.wifi.password) != passSize) ||
        !wifiPassword(next.wifi.password)) {
      strcpy(reply.text, "Error: invalid bounded WiFi credentials"); return;
    }
    if (!reserve(Effect::Wifi, reply)) return;
    next.wifiSet = 1;
    bool present;
    if (!mastRecord("settings", &next, sizeof(next), true, present)) {
      effect_ = Effect::None; reply.ticket = 0;
      strcpy(reply.text, "Error: WiFi commit failed"); return;
    }
    settings_ = next;
    strcpy(reply.text, "Saved WiFi credentials; apply saved enable setting after this reply");
  } else if (!strncmp(command, "radio ", 6) || !strncmp(command, "tempradio ", 10)) {
    const bool temporary = command[0] == 't';
    uint32_t values[6]{};
    unsigned count = 0;
    char *rest = command + (temporary ? 10 : 6);
    char *context;
    for (char *part = strtok_r(rest, " ", &context); part; part = strtok_r(nullptr, " ", &context)) {
      if (count == 6 || !number(part, values[count++])) {
        snprintf(reply.text, sizeof(reply.text), "Error: usage: %s", commandUsage(temporary ? "tempradio" : "radio")); return;
      }
    }
    if (count != (temporary ? 6u : 5u)) {
      snprintf(reply.text, sizeof(reply.text), "Error: usage: %s", commandUsage(temporary ? "tempradio" : "radio")); return;
    }
    const auto *p = values + (temporary ? 1 : 0);
    if (p[0] < 150000000 || p[0] > 960000000 ||
        (p[1] != 7810 && p[1] != 10420 && p[1] != 15630 && p[1] != 20830 &&
         p[1] != 31250 && p[1] != 41700 && p[1] != 62500 && p[1] != 125000 &&
         p[1] != 250000 && p[1] != 500000) ||
        p[2] < 5 || p[2] > 12 || p[3] < 5 || p[3] > 8 || p[4] > 22 ||
        (temporary && (!values[0] || values[0] > 3600)) || temporary_) {
      strcpy(reply.text, "Error: radio range/bandwidth invalid or temporary radio active"); return;
    }
    if (!reserve(temporary ? Effect::Temporary : Effect::Radio, reply)) return;
    if (temporary && !mux_->hasPersistedConfiguration()) {
      effect_ = Effect::None; reply.ticket = 0;
      strcpy(reply.text, "Error: tempradio requires a durable restore profile"); return;
    }
    nextRadio_ = {p[0], p[1], uint8_t(p[2]), uint8_t(p[3]), uint8_t(p[4])};
    duration_ = temporary ? values[0] * 1000 : 0;
    strcpy(reply.text, "Accepted radio change after old-PHY reply; queued old-PHY jobs will fail STALE");
  } else {
    strcpy(reply.text, "Error: unknown mast command; use help");
  }
}
void MastAdmin::acknowledged(uint32_t ticket, bool transmitted) {
  if (!ticket || ticket != ticket_ || effect_ == Effect::None || armed_) return;
  if (!transmitted) {
    cancelledTicket_ = ticket_;
    effect_ = Effect::None;
    strcpy(outcome_, "Error: acceptance reply failed; deferred effect cancelled");
    return;
  }
  armed_ = true;
  readyAt_ = millis() + 500;
}
void MastAdmin::loop() {
  source_.loop();
  const uint32_t now = millis();
  if (temporary_ && int32_t(now - restoreAt_) >= 0) {
    if (mux_->restoreMastConfiguration()) {
      temporary_ = false;
      strcpy(outcome_, "Temporary radio restored; effective generation advanced");
    }
  }
#ifdef ARDUINO_ARCH_ESP32
  if (espUpdateBusy()) return;
#endif
  if (effect_ == Effect::None) return;
  if (uint32_t(now - admittedAt_) >= 30000) {
    cancelledTicket_ = ticket_;
    effect_ = Effect::None;
    strcpy(outcome_, "Error: effect timed out; saved state may differ from applied state");
    return;
  }
  if (!armed_ || int32_t(now - readyAt_) < 0) return;
  if (effect_ == Effect::Radio || effect_ == Effect::Temporary) {
    if (mux_->isActuallyTransmitting() || mux_->physicalRadio().isReceiving()) return;
    const bool temporary = effect_ == Effect::Temporary;
    if (!mux_->applyMastConfiguration(nextRadio_, !temporary)) {
      strcpy(outcome_, "Error: radio apply/storage failed; old PHY retained");
    } else {
      if (temporary) {
        restoreAt_ = now + duration_;
        temporary_ = true;
      }
      strcpy(outcome_, temporary ? "Temporary radio applied; restore timer active" :
                                   "Radio applied and saved; effective generation advanced");
    }
  } else if (effect_ == Effect::Wifi) {
    bool enabled;
    if (!loadWifiEnabled(enabled)) {
      strcpy(outcome_, "Error: saved WiFi enable setting unavailable; connection unchanged");
      effect_ = Effect::None; return;
    }
#ifdef ARDUINO_ARCH_ESP32
    radio_network::wifiRecovery().begin(millis(), enabled);
    WiFi.disconnect(false, false);
    if (!enabled) {
      if (!WiFi.mode(WIFI_OFF)) {
        strcpy(outcome_, "Error: WiFi disable failed; saved disabled; inspect connection");
        effect_ = Effect::None; return;
      }
    } else {
      if (!radio_network::startStation()) {
        radio_network::wifiRecovery().begin(millis(), false);
        strcpy(outcome_, "Error: WiFi station restart failed; saved enabled; use wifi apply to retry");
        effect_ = Effect::None; return;
      }
      if (settings_.wifiSet) WiFi.begin(settings_.wifi.ssid, settings_.wifi.password);
      else if (WIFI_SSID[0]) WiFi.begin(WIFI_SSID, WIFI_PWD);
      else WiFi.begin();
    }
#endif
    strcpy(outcome_, enabled ? "WiFi reconnect requested; association not yet confirmed" : "WiFi disabled; RF roles remain active");
  } else if (effect_ == Effect::Reboot) {
    strcpy(outcome_, "Reboot requested");
#ifdef ARDUINO_ARCH_ESP32
    ESP.restart();
#endif
  }
  effect_ = Effect::None;
}
} // namespace onchip
#endif
