// SPDX-License-Identifier: Apache-2.0
#if defined(MESHCORE_MAST_ADMIN) && MESHCORE_MAST_ADMIN
#include "MastAdmin.h"
#include "EspPacketPrograms.h"
#include "ObserverConfig.h"
#if MESHCORE_NODE_BACKUP
#include "NodeBackup.h"
#endif
#include "Config.h"
#include "Clock.h"
#include "BotHttps.h"
#include "BotSettings.h"
#include "CommandBot.h"
#include "CompanionSessions.h"
#include "Runtime.h"
#include "Syslog.h"
#include "Management.h"
#include "ServiceName.h"
#include "FirmwareIdentity.h"
#include "CloudRoomService.h"
#include <Utils.h>
#include <helpers/ClientACL.h>
#include "TelemetryService.h"
#include <nvs.h>
#include <SPIFFS.h>
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
static bool legacyMastRecord(const char *key, void *data, size_t size, bool write, bool &present) {
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
bool mastRecord(const char *key, void *data, size_t size, bool write, bool &present) {
  if (!strcmp(key, "settings") || !strcmp(key, "acl")) {
    const bool ok = MastAdmin::configurationRecord(key, data, size, write, present);
    if (!ok) Serial.println("Management settings/ACL file storage read/commit failed; inspect storage and restart");
    return ok;
  }
  return legacyMastRecord(key, data, size, write, present);
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
constexpr const char *ReplayFiles[] = {"/mast-replay-a.bin", "/mast-replay-b.bin"};
struct ReplayReference {
  uint8_t magic[4]{'M', 'R', 'P', 2}, slot = 0, reserved[3]{}, digest[32]{};
  bool valid() const {
    return !memcmp(magic, "MRP\2", 4) && slot < 2 &&
           !reserved[0] && !reserved[1] && !reserved[2];
  }
};
bool replayReference(ReplayReference &reference, bool &present, bool &legacy) {
  present = legacy = false;
  nvs_handle_t handle;
  auto result = nvs_open("mc-mast-admin", NVS_READONLY, &handle);
  if (result == ESP_ERR_NVS_NOT_FOUND) return true;
  if (result != ESP_OK) return false;
  size_t size = 0;
  result = nvs_get_blob(handle, "replay", nullptr, &size);
  if (result == ESP_ERR_NVS_NOT_FOUND) { nvs_close(handle); return true; }
  legacy = size == 148;
  if (result != ESP_OK || (!legacy && size != sizeof(reference))) {
    nvs_close(handle); return false;
  }
  if (!legacy) {
    result = nvs_get_blob(handle, "replay", &reference, &size);
    if (size != sizeof(reference) || !reference.valid()) result = ESP_ERR_INVALID_STATE;
  }
  nvs_close(handle);
  present = result == ESP_OK;
  return present;
}
bool reclaimLegacyRecord(const char *key) {
  nvs_handle_t handle;
  auto result = nvs_open("mc-mast-admin", NVS_READONLY, &handle);
  if (result == ESP_ERR_NVS_NOT_FOUND) return true;
  if (result != ESP_OK) return false;
  size_t size = 0;
  result = nvs_get_blob(handle, key, nullptr, &size);
  nvs_close(handle);
  if (result == ESP_ERR_NVS_NOT_FOUND) return true;
  if (result != ESP_OK || nvs_open("mc-mast-admin", NVS_READWRITE, &handle) != ESP_OK) return false;
  result = nvs_erase_key(handle, key);
  if (result == ESP_OK) result = nvs_commit(handle);
  nvs_close(handle);
  if (result != ESP_OK || nvs_open("mc-mast-admin", NVS_READONLY, &handle) != ESP_OK) return false;
  size = 0;
  result = nvs_get_blob(handle, key, nullptr, &size);
  nvs_close(handle);
  return result == ESP_ERR_NVS_NOT_FOUND;
}
bool reclaimExtraReplay() { return reclaimLegacyRecord("replay-extra"); }
constexpr const char *ConfigurationFiles[] = {"/mast-config-a.bin", "/mast-config-b.bin"};
struct ConfigurationReference {
  uint8_t magic[4]{'M', 'C', 'F', 2}, slot = 0, reserved[3]{}, digest[32]{};
  bool valid() const {
    return !memcmp(magic, "MCF\2", 4) && slot < 2 &&
           !reserved[0] && !reserved[1] && !reserved[2];
  }
};
bool configurationReference(ConfigurationReference &reference, bool &present, bool &legacy) {
  present = legacy = false;
  nvs_handle_t handle;
  auto result = nvs_open("mc-mast-admin", NVS_READONLY, &handle);
  if (result == ESP_ERR_NVS_NOT_FOUND) return true;
  if (result != ESP_OK) return false;
  size_t size = 0;
  result = nvs_get_blob(handle, "settings", nullptr, &size);
  if (result == ESP_ERR_NVS_NOT_FOUND) { nvs_close(handle); return true; }
  legacy = size == 140;
  if (result != ESP_OK || (!legacy && size != sizeof(reference))) {
    nvs_close(handle); return false;
  }
  if (!legacy) {
    result = nvs_get_blob(handle, "settings", &reference, &size);
    if (size != sizeof(reference) || !reference.valid()) result = ESP_ERR_INVALID_STATE;
  }
  nvs_close(handle);
  present = result == ESP_OK;
  return present;
}
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
struct Help { const char *topic, *syntax; unsigned page = 1; };
constexpr Help topics[] = {
    {"wifi", "wifi status|ssid TEXT|password TEXT|-|apply|forget; explicit bytes: ssid|password hex HEX; set wifi.enabled 0|1; encrypted RF only"},
    {"wifi", "wifi 2/5: literal ASCII, including spaces and hex-looking text; SSID 1..32 bytes; password 8..63 bytes or 64 hex digits; '-' opens network", 2},
    {"wifi", "wifi 3/5: wifi ssid hex HEX; wifi password hex HEX; UTF-8/control SSID: hex; combined legacy wifi HEX HEX: use separate forms", 3},
    {"wifi", "wifi 4/5: tagged content max 145 bytes; 64-byte PSK: wifi password TEXT or set wifi.pwd TEXT; set wifi.ssid|pwd preserves literal hex prefix", 4},
    {"wifi", "wifi 5/5: status shows usable IPv4 and retry timer; retries back off 30/60/120s; station restarts after 3 attempts; no settings erased", 5},
    {"radio", "radio FREQ_HZ BW_HZ SF CR TX_DBM; read: get radio|get freq|get tx; shared PHY changes after reply"},
    {"tempradio", "tempradio SECONDS FREQ_HZ BW_HZ SF CR TX_DBM; duration 1..3600; restores saved PHY"},
    {"cad", "get cad; set cad on|off; hardware channel activity detection before shared-radio TX; saved after reply"},
    {"radio-controls", "get/set cad on|off; int.thresh 0..255; agc.reset.interval 0..1020 seconds (4s units); rxboost on|off; af 0..9; saved after reply"},
    {"autoadvert", "get autoadvert; set autoadvert on|off; saved/live device-wide startup/periodic adverts; manual app/admin adverts unchanged"},
    {"sntp", "get sntp.current|server|interval; set sntp.server HOST|off; set sntp.interval 60..86400; seconds; saved/live; current: fresh SNTP/GPS only"},
    {"syslog", "get syslog|syslog.stats; set syslog IP[:PORT]|off; syslog test; UDP default port 514; saved/live; get diagnostics; stats system"},
    {"mqtt", "mqtt status|uri|name|iata|prefix|audience|format|filter; FIELD VALUE; username|password|ca clear|HEX; commit|discard; reboot applies"},
    {"cloudroom", "cloudroom status|error|advertise ALIAS|config|enable on|off; direct authenticated administration; config changes apply after restart"},
    {"setup", "setup status|migrate; save private initial settings for a generic application update; identities and existing SPIFFS/NVS retained"},
    {"role", "role help; role config ROLE; role name ROLE [TEXT]; role advert ROLE zerohop|flood; role key|channel|password ROLE ..."},
    {"roles", "roles; roles list [1|2]: named applied/saved selection; roles MASK=0..15 (repeater=1,room=2,companion=4,observer=8); apply reboots"},
    {"key", "key ROLE [pending|cancel|HEX128]; use key help; private imports: encrypted RF only"},
    {"password", "password [help|HEX]; 1..15 printable bytes encoded as hex; changes: encrypted RF only"},
    {"source", "source help; source status|hash|metadata|helptext|api; source begin|chunk|commit|rollback|remove ..."},
    {"source", "source 2/4: source api; api fetch; metadata; fetch package SHA256; begin ID16 SIZE SHA256; chunk ID16 INDEX HEX; help source 3", 2},
    {"source", "source 3/4: source commit|status|read INDEX|rollback|remove|retry|cancel; source help TEXT saves help; source helptext reads it; help source 4", 3},
    {"source", "source 4/4: admin.py source list|install NAME FILE|export NAME FILE|remove NAME; files share one Lua VM; rollback restores source, not data", 4},
    {"packet", "packet api|phy; packet SLOT status|hash|stats|memory|timing|enable on|off|rollback|remove|retry; SLOT=0..1; Management RF/web"},
    {"packet", "packet SLOT budget STAGES FUEL US CAPS; stages=1..255 fuel=1..100000 us=1..20000; caps system=1 PHY=2 compose=4; help packet 3", 2},
    {"packet", "packet 3/3: packet SLOT begin ID16 lua|wasm SIZE SHA256; chunk ID16 INDEX HEX; commit|cancel ID16; read INDEX; admin.py packet install SLOT FILE", 3},
    {"bot", "bot help; bot status|stats|contacts|radio|policy|mesh|name|aliases|discovery|adaptive|shared|reminders|events|forward|https ..."},
    {"bot", "bot 2/4: bot log|diagnostics|admission|destination|channel-wait|home|cancel; role help; source status; help bot 3", 2},
    {"bot", "bot 3/4: bot membership [SLOT ...]; bot access [CONTEXT ...]; Public commands default denied; use help channels; help bot 4", 3},
    {"bot", "bot 4/4: bot thread ...; bot repeaters ...; bot data ...; source api storage; help threads; help repeaters; data help", 4},
    {"channels", "bot membership SLOT off|public|#NAME|private NAME_HEX KEY32; bot access dm|SLOT default|COMMAND MASK|inherit; slots 0..7"},
    {"channels", "channels 2/2: access bits 1/2 bare execute/reply,4/8 addressed execute/reply,16/32 thread read/write; Public defaults denied", 2},
    {"threads", "bot thread dm|SLOT|native NAME MASK|inherit; list INDEX reads grants; read/write bits 16/32; native caller grants still required"},
    {"repeaters", "bot repeaters help; bot repeaters discovery 60..86400; bot repeaters interval 60..86400; seconds; retained samples keep original RF time"},
    {"auth", "auth status [KEY64]; auth peer 1..10; auth forget KEY64 (not the compiled owner)"},
    {"trust", "trust KEY64|none; legacy singleton authority; compiled owner and Management ACL unchanged"},
    {"setperm", "setperm KEY64 PERMISSIONS (0..255); native roles: 0=remove,1=read-only,2=read-write,3=admin; Management/bot admin only; get acl"},
    {"data", "data help; data status|export|read|begin|chunk|stage|restore|cancel ..."},
    {"telemetry", "telemetry help; telemetry status|counts|times|tls|endpoint ...; network collector settings"},
    {"room", "room access; native Room settings belong to the Room contact"},
    {"companion", "companion stats|errors|help; native contacts/channels use the companion connection"},
    {"stats", "stats [radio|signal|tx|airtime|admission|sensors|memory|psram|bot|vm|observer|companion|system]; counters since boot; help syslog"},
    {"get", "get name|owner.info|radio|freq|tx|cad|autoadvert|int.thresh|agc.reset.interval|rxboost|af|wifi.FIELD; help get 2"},
    {"get", "get acl [KEY64|1..5]; checked Management ACL, independent of repeater/room; only native admin role grants passwordless Management/bot", 2},
    {"set", "set name|owner.info TEXT; set wifi.ssid|pwd TEXT; set wifi.enabled 0|1; set cad|autoadvert on|off"}
};
constexpr size_t textLength(const char *text) {
  size_t length = 0;
  while (text[length]) ++length;
  return length;
}
constexpr bool boundedHelp() {
  for (const auto &entry : topics) {
    size_t length = textLength(entry.syntax);
    for (const auto &next : topics)
      if (entry.page == 1 && next.page == 2) {
        size_t i = 0;
        while (entry.topic[i] && entry.topic[i] == next.topic[i]) ++i;
        if (!entry.topic[i] && !next.topic[i]) length += 9 + i;
      }
    if (length > MastAdmin::TextLimit - 17) return false;
  }
  return true;
}
static_assert(boundedHelp(), "Help content must fit a 16-hex-tagged reply");
const char *commandUsage(const char *topic, unsigned page = 1) {
  for (const auto &entry : topics)
    if (!strcmp(topic, entry.topic) && page == entry.page) return entry.syntax;
  return nullptr;
}
void ssidReply(const char *ssid, MastAdmin::Reply &reply) {
  if (strlen(ssid) > 32) {
    strcpy(reply.text, "Error: WiFi SSID readback exceeds 32 bytes"); return;
  }
  bool printable = true;
  for (const auto *p = reinterpret_cast<const unsigned char *>(ssid); *p; ++p)
    if (*p < 32 || *p > 126) printable = false;
  if (printable) {
    snprintf(reply.text, sizeof(reply.text), "> %s", ssid);
    return;
  }
  strcpy(reply.text, "> hex ");
  for (size_t i = 0; ssid[i]; ++i)
    snprintf(reply.text + 6 + 2 * i, 3, "%02x", static_cast<unsigned char>(ssid[i]));
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
void MastAdmin::helpCommand(const char *argument, Reply &reply) {
  static constexpr const char *index[] = {
    "help 1/3: status; roles; stats; ver; board; help bot|channels|companion|repeaters|role|roles|room|stats|threads; next: help 2",
    "help 2/3: help autoadvert|cad|cloudroom|get|mqtt|radio|radio-controls|set|setup|sntp|syslog|tempradio|wifi; next: help 3",
    "help 3/3: help auth|backup|data|key|packet|password|setperm|source|telemetry|trust; apply|reboot; one page per request"
  };
  static_assert(textLength(index[0]) <= TextLimit - 17 &&
                textLength(index[1]) <= TextLimit - 17 &&
                textLength(index[2]) <= TextLimit - 17, "Help index must fit a tagged reply");
  char input[TextLimit + 1];
  strcpy(input, argument);
  char *cursor = input;
  const char *topic = word(cursor), *pageText = word(cursor);
  uint32_t page = 1;
  if (*cursor) {
    strcpy(reply.text, "Error: usage: help [TOPIC] [PAGE]; use help"); return;
  }
  const bool indexPage = !*topic || (*topic >= '0' && *topic <= '9');
  if (indexPage) {
    if (*pageText || (*topic && !number(topic, page)) || page < 1 || page > 3) {
      strcpy(reply.text, "Error: help index page requires 1..3; use help"); return;
    }
    strcpy(reply.text, index[page - 1]);
  } else {
    if (!commandUsage(topic)) {
      strcpy(reply.text, "Error: unknown help topic; use help, help 2 or help 3"); return;
    }
    if ((*pageText && !number(pageText, page)) || !commandUsage(topic, page)) {
      snprintf(reply.text, sizeof(reply.text), "Error: unknown help page; use help %s", topic); return;
    }
    snprintf(reply.text, sizeof(reply.text), "%s", commandUsage(topic, page));
    if (page == 1 && commandUsage(topic, 2)) {
      const size_t used = strlen(reply.text);
      snprintf(reply.text + used, sizeof(reply.text) - used, "; help %s 2", topic);
    }
  }
}
void MastAdmin::rolesCommand(const char *argument, Reply &reply) {
  uint32_t page = 1;
  if (*argument && !number(argument, page)) {
    strcpy(reply.text, "Error: usage: roles list [1|2]; roles MASK saves next boot"); return;
  }
  if (page < 1 || page > 2) {
    strcpy(reply.text, "Error: roles list page requires 1..2"); return;
  }
  ProfileJournal saved;
  bool bot;
  if (!loadProfileJournal(saved) || !loadBotEnabled(bot)) {
    strcpy(reply.text, "Error: saved role state unavailable"); return;
  }
  if (page == 1) {
    snprintf(reply.text, sizeof(reply.text),
             "roles 1/2: applied/saved repeater=%u/%u room=%u/%u companion=%u/%u observer=%u/%u; next: roles list 2",
             !!(appliedMask_ & RoleProfile::Repeater), !!(saved.profile.enabled & RoleProfile::Repeater),
             !!(appliedMask_ & RoleProfile::Room), !!(saved.profile.enabled & RoleProfile::Room),
             !!(appliedMask_ & RoleProfile::Companion), !!(saved.profile.enabled & RoleProfile::Companion),
             !!(appliedMask_ & RoleProfile::Observer), !!(saved.profile.enabled & RoleProfile::Observer));
  } else {
    RadioDashboard::RoleStatus status;
    commandBotService().dashboardStatus(status);
    snprintf(reply.text, sizeof(reply.text),
             "roles 2/2: Management=available bot applied=%u saved=%u; KISS=shared modem service (no mask bit); apply reboots",
             status.has_identity, bot);
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
               management ? "zerohop,flood" : "none", management ? "shared" : "service-only");
      return;
    }
    const unsigned channels = bot ? 1 : nativeRole == Role::Companion ? companionChannelCount() : 0;
    snprintf(reply.text, sizeof(reply.text),
             "role=%s name=persistent/%s key=generate-stage/reboot channel-slots=%u channel-key-bits=%u RF=shared advert=zerohop,flood",
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
    const bool zeroHop = !strcmp(cursor, "zerohop");
    if (!zeroHop && strcmp(cursor, "flood")) {
      strcpy(reply.text, "Error: role advert ROLE zerohop|flood"); return;
    }
    if (kiss) {
      strcpy(reply.text, "Error: KISS is a service label, not an advertised RF role"); return;
    }
    const bool queued = management ? management_->advertise(zeroHop) :
                        bot ? (zeroHop ? commandBotService().advertiseOwnerZeroHop() :
                                         commandBotService().advertise(false)) :
                              nativeRoleAdvertise(nativeRole, zeroHop);
    strcpy(reply.text, queued ? (zeroHop ? "Queued zero-hop advert" : "Queued flood advert") :
           "Error: advert unavailable; role inactive, radio/queue busy or rate/airtime limit");
    return;
  }
  if (management || kiss) {
    strcpy(reply.text, "Error: service supports role config/name only; management also supports advert zerohop|flood");
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
    if (!number(word(cursor), slot) || slot >= (bot ? BotRadioPolicy::ChannelLimit : companionChannelCount())) {
      strcpy(reply.text, "Error: bot channel SLOT is 0..7; use role config companion for its slot count"); return;
    }
    BotRadioPolicy policy;
    if (bot && !loadBotRadioPolicy(policy)) { strcpy(reply.text, "Error: saved bot channel unavailable"); return; }
    if (!*cursor) {
      char channelName[33]{}, keyId[17] = "none";
      if (bot) {
        const auto membership = policy.membership(slot);
        strcpy(channelName, membership.name);
        if (membership.keySet) fingerprint(membership.key, keyId);
        else if (channelName[0]) strcpy(keyId, !strcmp(channelName, "Public") ? "public" : "hashtag");
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
      BotRadioPolicy::Membership membership;
      strcpy(membership.name, channelName);
      membership.keySet = channelName[0] != 0;
      memcpy(membership.key, key, sizeof(key));
      policy.setMembership(slot, membership);
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
bool MastAdmin::configurationRecord(const char *key, void *data, size_t size, bool write, bool &present) {
  const bool settings = !strcmp(key, "settings");
  present = false;
  if (size != (settings ? sizeof(Settings) : sizeof(ACL))) return false;
  struct Snapshot {
    uint8_t magic[4]{'M', 'C', 'F', 1}, flags = 0, reserved[3]{};
    Settings settings;
    ACL acl;
  } snapshot{}, check{};
  static_assert(sizeof(Snapshot) == 352 && sizeof(ConfigurationReference) == 40,
                "Recheck Management configuration layout and NVS budget");
  const auto valid = [](const Snapshot &record) {
    const auto &s = record.settings;
    return !memcmp(record.magic, "MCF\1", 4) && !(record.flags & ~3) &&
           !record.reserved[0] && !record.reserved[1] && !record.reserved[2] &&
           (!(record.flags & 1) || (s.version == 1 && s.wifiSet <= 1 && s.trustedSet <= 1 &&
             memchr(s.wifi.ssid, 0, sizeof(s.wifi.ssid)) &&
             memchr(s.wifi.password, 0, sizeof(s.wifi.password)) && wifiPassword(s.wifi.password))) &&
           (!(record.flags & 2) || validACL(record.acl));
  };
  ConfigurationReference previous;
  bool referencePresent, legacy;
  if (!configurationReference(previous, referencePresent, legacy)) return false;
  const auto verifyFile = [&](const ConfigurationReference &reference) {
    auto file = SPIFFS.open(ConfigurationFiles[reference.slot], "r");
    if (!file || file.size() != sizeof(check) ||
        file.read(reinterpret_cast<uint8_t *>(&check), sizeof(check)) != sizeof(check)) return false;
    file.close();
    uint8_t digest[32];
    mesh::Utils::sha256(digest, sizeof(digest), reinterpret_cast<const uint8_t *>(&check), sizeof(check));
    return !memcmp(digest, reference.digest, sizeof(digest)) && valid(check);
  };
  if (referencePresent && !legacy) {
    if (!verifyFile(previous) || !reclaimLegacyRecord("acl")) return false;
    snapshot = check;
  } else {
    bool settingsPresent, aclPresent;
    if (!legacyMastRecord("settings", &snapshot.settings, sizeof(snapshot.settings), false, settingsPresent) ||
        !legacyMastRecord("acl", &snapshot.acl, sizeof(snapshot.acl), false, aclPresent)) return false;
    snapshot.flags = (settingsPresent ? 1 : 0) | (aclPresent ? 2 : 0);
    if (!valid(snapshot)) return false;
  }
  if (write) {
    if (settings) memcpy(&snapshot.settings, data, size);
    else memcpy(&snapshot.acl, data, size);
    snapshot.flags |= settings ? 1 : 2;
    if (!valid(snapshot)) return false;
  }
  if (write || ((legacy || !referencePresent) && snapshot.flags)) {
    ConfigurationReference next;
    next.slot = referencePresent && !legacy ? previous.slot ^ 1 : 0;
    mesh::Utils::sha256(next.digest, sizeof(next.digest),
                        reinterpret_cast<const uint8_t *>(&snapshot), sizeof(snapshot));
    auto file = SPIFFS.open(ConfigurationFiles[next.slot], "w");
    if (!file || file.write(reinterpret_cast<const uint8_t *>(&snapshot), sizeof(snapshot)) != sizeof(snapshot))
      return false;
    file.flush(); file.close();
    if (!verifyFile(next) || memcmp(&snapshot, &check, sizeof(snapshot))) return false;
    bool saved;
    if (!legacyMastRecord("settings", &next, sizeof(next), true, saved)) return false;
    ConfigurationReference actual;
    if (!configurationReference(actual, saved, legacy) || !saved || legacy ||
        memcmp(&next, &actual, sizeof(next)) || !verifyFile(actual) ||
        memcmp(&snapshot, &check, sizeof(snapshot)) || !reclaimLegacyRecord("acl")) return false;
  }
  present = snapshot.flags & (settings ? 1 : 2);
  if (!write && present) {
    if (settings) memcpy(data, &snapshot.settings, size);
    else memcpy(data, &snapshot.acl, size);
  }
  return true;
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
  } else if (!strcmp(command, "get cad")) {
    snprintf(reply.text, sizeof(reply.text), "> %s", mux_->cadEnabled() ? "on" : "off");
  } else if (!strcmp(command, "get autoadvert")) {
    bool saved;
    if (!loadAutomaticAdverts(saved)) {
      strcpy(reply.text, "Error: saved autoadvert unavailable; automatic adverts disabled on next boot; set autoadvert off to repair");
      return;
    }
    snprintf(reply.text, sizeof(reply.text), "> saved=%s live=%s; manual adverts enabled",
             saved ? "on" : "off", automaticAdvertsEnabled() ? "on" : "off");
  } else if (!strncmp(command, "set autoadvert", 14)) {
    if (strcmp(command, "set autoadvert on") && strcmp(command, "set autoadvert off")) {
      strcpy(reply.text, "Error: use set autoadvert on|off"); return;
    }
    strcpy(reply.text, saveAutomaticAdverts(!strcmp(command, "set autoadvert on")) ?
           "Saved and applied autoadvert; already queued/transmitted adverts unchanged; manual adverts enabled" :
           "Error: autoadvert persistence unknown; live unchanged; inspect get autoadvert before retuning or restarting");
  } else if (!strcmp(command, "get int.thresh")) {
    snprintf(reply.text, sizeof(reply.text), "> %d", mux_->interferenceThreshold());
  } else if (!strcmp(command, "get af")) {
    snprintf(reply.text, sizeof(reply.text), "> %.9g", double(mux_->airtimeFactor()));
  } else if (!strcmp(command, "get agc.reset.interval")) {
    snprintf(reply.text, sizeof(reply.text), "> %u", unsigned(mux_->agcResetIntervalSeconds()));
  } else if (!strcmp(command, "get rxboost")) {
    if (!mux_->rxBoostAvailable()) {
      strcpy(reply.text, "Error: shared-radio RX boost unavailable"); return;
    }
    snprintf(reply.text, sizeof(reply.text), "> %s", mux_->rxBoostEnabled() ? "on" : "off");
  } else if (!strncmp(command, "set int.thresh ", 15) ||
             !strncmp(command, "set af ", 7) ||
             !strncmp(command, "set agc.reset.interval ", 23) ||
             !strncmp(command, "set rxboost ", 12)) {
    if (temporary_) {
      strcpy(reply.text, "Error: temporary radio active; wait for saved PHY restoration"); return;
    }
    Effect effect;
    uint8_t nextInterference = 0;
    float nextAirtime = 1;
    uint16_t nextAGCSeconds = mux_->agcResetIntervalSeconds();
    bool nextRxConfigured = mux_->rxBoostConfigured(), nextRxBoost = mux_->rxBoostEnabled();
    if (!strncmp(command, "set int.thresh ", 15)) {
      uint32_t value;
      if (!number(command + 15, value) || value > 255) {
        strcpy(reply.text, "Error: int.thresh requires 0..255; 0 disables interference detection"); return;
      }
      nextInterference = uint8_t(value);
      effect = Effect::Interference;
    } else if (!strncmp(command, "set af ", 7)) {
      char *end;
      const double value = strtod(command + 7, &end);
      if (!command[7] || *end || !isfinite(value) || value < 0 || value > 9) {
        strcpy(reply.text, "Error: shared-radio af requires a finite value in 0..9"); return;
      }
      nextAirtime = float(value);
      effect = Effect::Airtime;
    } else {
      if (!strncmp(command, "set agc.reset.interval ", 23)) {
        uint32_t value;
        if (!number(command + 23, value) || value > 1020) {
          strcpy(reply.text, "Error: agc.reset.interval requires 0..1020 seconds; 0 disables resets"); return;
        }
        nextAGCSeconds = uint16_t(value - value % 4);
      } else {
        if (!mux_->rxBoostAvailable()) {
          strcpy(reply.text, "Error: shared-radio RX boost unavailable"); return;
        }
        if (strcmp(command + 12, "on") && strcmp(command + 12, "off")) {
          strcpy(reply.text, "Error: use set rxboost on|off"); return;
        }
        nextRxConfigured = true;
        nextRxBoost = !strcmp(command + 12, "on");
      }
      effect = Effect::Controls;
    }
    if (!reserve(effect, reply)) return;
    nextInterference_ = nextInterference;
    nextAirtime_ = nextAirtime;
    nextAGCSeconds_ = nextAGCSeconds;
    nextRxConfigured_ = nextRxConfigured;
    nextRxBoost_ = nextRxBoost;
    if (!strncmp(command, "set agc.reset.interval ", 23))
      snprintf(reply.text, sizeof(reply.text),
               "Accepted shared-radio setting after reply; AGC interval=%us (4-second units); inspect job",
               unsigned(nextAGCSeconds_));
    else
      strcpy(reply.text, "Accepted shared-radio setting after reply; inspect get setting and job for saved/applied result");
  } else if (!strncmp(command, "set cad", 7)) {
    if (strcmp(command, "set cad on") && strcmp(command, "set cad off")) {
      strcpy(reply.text, "Error: use set cad on|off"); return;
    }
    if (temporary_) {
      strcpy(reply.text, "Error: temporary radio active; wait for saved PHY restoration"); return;
    }
    if (!reserve(Effect::CAD, reply)) return;
    nextCAD_ = !strcmp(command, "set cad on");
    strcpy(reply.text, "Accepted shared-radio CAD change after reply; queued jobs will fail STALE");
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
      if (saved) {
        if (password) snprintf(reply.text, sizeof(reply.text), "> %s", credentials.password);
        else ssidReply(credentials.ssid, reply);
      }
#ifdef ARDUINO_ARCH_ESP32
      else if (WIFI_SSID[0]) {
        if (password) snprintf(reply.text, sizeof(reply.text), "> %s", WIFI_PWD);
        else ssidReply(WIFI_SSID, reply);
      } else {
        if (password) snprintf(reply.text, sizeof(reply.text), "> %s", WiFi.psk().c_str());
        else ssidReply(WiFi.SSID().c_str(), reply);
      }
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
      bool present;
      if (!loadWifi(next.wifi, present)) {
        strcpy(reply.text, "Error: saved WiFi credentials unavailable; field unchanged"); return;
      }
      strcpy(password ? next.wifi.password : next.wifi.ssid, value);
      next.wifiSet = next.wifi.ssid[0] != 0;
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
bool MastAdmin::loadReplay() {
  ReplayReference reference;
  bool present, legacy;
  if (!replayReference(reference, present, legacy)) return false;
  if (present && !legacy) {
    struct Snapshot { Replay replay; ExtraReplay extra; } snapshot{};
    auto file = SPIFFS.open(ReplayFiles[reference.slot], "r");
    const bool complete = file && file.size() == sizeof(snapshot) &&
        file.read(reinterpret_cast<uint8_t *>(&snapshot), sizeof(snapshot)) == sizeof(snapshot) &&
        file.size() == sizeof(snapshot);
    file.close();
    uint8_t digest[32];
    mesh::Utils::sha256(digest, sizeof(digest), reinterpret_cast<const uint8_t *>(&snapshot), sizeof(snapshot));
    if (!complete || memcmp(digest, reference.digest, sizeof(digest)) ||
        snapshot.replay.version != 1 || !validExtraReplay(snapshot.extra)) return false;
    if (!reclaimExtraReplay()) return false;
    replay_ = snapshot.replay;
    extraReplay_ = snapshot.extra;
    return true;
  }
  bool replayPresent, extraPresent;
  if (!mastRecord("replay", &replay_, sizeof(replay_), false, replayPresent) || replay_.version != 1 ||
      !mastRecord("replay-extra", &extraReplay_, sizeof(extraReplay_), false, extraPresent) ||
      (extraPresent && !validExtraReplay(extraReplay_))) return false;
  if (!replayPresent && !extraPresent) return true;
  mesh::Utils::sha256(extraReplay_.digest, sizeof(extraReplay_.digest),
                      reinterpret_cast<const uint8_t *>(&extraReplay_), offsetof(ExtraReplay, digest));
  return saveReplay(replay_, extraReplay_);
}
bool MastAdmin::saveReplay(const Replay &replay, const ExtraReplay &extra) {
  struct Snapshot { Replay replay; ExtraReplay extra; } snapshot{replay, extra}, check{};
  static_assert(sizeof(snapshot) == 400 && sizeof(ReplayReference) == 40,
                "Recheck replay migration and NVS budget");
  mesh::Utils::sha256(snapshot.extra.digest, sizeof(snapshot.extra.digest),
                      reinterpret_cast<const uint8_t *>(&snapshot.extra), offsetof(ExtraReplay, digest));
  if (snapshot.replay.version != 1 || !validExtraReplay(snapshot.extra)) return false;
  ReplayReference previous, next;
  bool present, legacy;
  if (!replayReference(previous, present, legacy)) return false;
  next.slot = present && !legacy ? 1 - previous.slot : 0;
  mesh::Utils::sha256(next.digest, sizeof(next.digest),
                      reinterpret_cast<const uint8_t *>(&snapshot), sizeof(snapshot));
  const auto verifyFile = [&](const ReplayReference &reference) {
    auto file = SPIFFS.open(ReplayFiles[reference.slot], "r");
    const bool complete = file && file.size() == sizeof(check) &&
        file.read(reinterpret_cast<uint8_t *>(&check), sizeof(check)) == sizeof(check) &&
        file.size() == sizeof(check);
    file.close();
    uint8_t digest[32];
    mesh::Utils::sha256(digest, sizeof(digest), reinterpret_cast<const uint8_t *>(&check), sizeof(check));
    return complete && !memcmp(digest, reference.digest, sizeof(digest));
  };
  if (present && !legacy && !verifyFile(previous)) return false;
  auto file = SPIFFS.open(ReplayFiles[next.slot], "w");
  if (!file) return false;
  const bool complete = file.write(reinterpret_cast<const uint8_t *>(&snapshot), sizeof(snapshot)) == sizeof(snapshot);
  file.flush();
  file.close();
  if (!complete || !verifyFile(next) || memcmp(&snapshot, &check, sizeof(snapshot))) return false;
  if (!mastRecord("replay", &next, sizeof(next), true, present)) return false;
  ReplayReference actual;
  if (!replayReference(actual, present, legacy) || !present || legacy ||
      memcmp(&actual, &next, sizeof(next)) || !verifyFile(actual) ||
      memcmp(&snapshot, &check, sizeof(snapshot))) return false;
  return reclaimExtraReplay();
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
  ready_ = loadReplay() &&
           mastRecord("owner-replay", &ownerReplay_, sizeof(ownerReplay_), false, present) &&
           ownerReplay_.version == 1;
  if (!ready_) {
    strcpy(outcome_, "Error: mast replay storage invalid; native administration disabled");
    Serial.println(outcome_);
  }
  aclReady_ = mastRecord("acl", &acl_, sizeof(acl_), false, present) &&
              (!present || validACL(acl_));
  if (!aclReady_) {
    strcpy(outcome_, "Error: Management ACL storage invalid; ACL grants disabled; inspect NVS acl and restart");
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
  if ((settings_.trustedSet && !memcmp(key, settings_.trustedKey, 32)) || compiledTrusted(key))
    return true;
  if (aclReady_) for (const auto &entry : acl_.entries) {
    ClientInfo client{};
    client.permissions = entry.permissions;
    if (client.isAdmin() && !memcmp(key, entry.key, 32)) return true;
  }
  return false;
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
  for (const auto &peer : extraReplay_.peers)
    if (!memcmp(peer.key, key, 32) && peer.timestamp > timestamp) timestamp = peer.timestamp;
  return timestamp;
}
bool MastAdmin::rememberTimestamp(const uint8_t key[32], uint32_t timestamp) {
  if (!ready_ || !timestamp || timestamp <= lastTimestamp(key)) return false;
  if (compiledTrusted(key)) {
    OwnerReplay next, actual;
    memcpy(next.peer.key, key, 32);
    next.peer.timestamp = timestamp;
    bool present;
    if (!mastRecord("owner-replay", &next, sizeof(next), true, present) ||
        !mastRecord("owner-replay", &actual, sizeof(actual), false, present) ||
        !present || memcmp(&next, &actual, sizeof(next))) {
      ready_ = false;
      strcpy(outcome_, "Error: owner replay commit/readback unknown; restart before native administration");
      Serial.println(outcome_); return false;
    }
    ownerReplay_ = next;
    return true;
  }
  Replay next = replay_;
  ExtraReplay extra = extraReplay_;
  Replay::Peer *slot = nullptr;
  for (auto &peer : next.peers)
    if (!memcmp(peer.key, key, 32)) { slot = &peer; break; }
    else if (!peer.timestamp && !slot) slot = &peer;
  for (auto &peer : extra.peers)
    if (!memcmp(peer.key, key, 32)) { slot = &peer; break; }
    else if (!peer.timestamp && !slot) { slot = &peer; }
  if (!slot) {
    Serial.println("Mast replay capacity exhausted (10); inspect auth peer and auth forget obsolete KEY");
    return false;
  }
  memcpy(slot->key, key, 32);
  slot->timestamp = timestamp;
  mesh::Utils::sha256(extra.digest, sizeof(extra.digest), reinterpret_cast<const uint8_t *>(&extra),
                      offsetof(ExtraReplay, digest));
  if (!saveReplay(next, extra)) {
    ready_ = false;
    strcpy(outcome_, "Error: replay commit/readback unknown; restart before native administration");
    Serial.println(outcome_); return false;
  }
  replay_ = next;
  extraReplay_ = extra;
  return true;
}
bool MastAdmin::validExtraReplay(const ExtraReplay &replay) const {
  uint8_t digest[32];
  mesh::Utils::sha256(digest, sizeof(digest), reinterpret_cast<const uint8_t *>(&replay),
                      offsetof(ExtraReplay, digest));
  return replay.version == 1 && !memcmp(digest, replay.digest, sizeof(digest));
}
bool MastAdmin::validACL(const ACL &acl) {
  uint8_t digest[32];
  mesh::Utils::sha256(digest, sizeof(digest), reinterpret_cast<const uint8_t *>(&acl),
                      offsetof(ACL, digest));
  if (acl.version != 1 || memcmp(digest, acl.digest, sizeof(digest))) return false;
  for (unsigned i = 0; i < ACLSlots; ++i) {
    const auto &entry = acl.entries[i];
    if (entry.permissions && (entry.permissions & PERM_ACL_ROLE_MASK) == PERM_ACL_GUEST)
      return false;
    for (unsigned j = 0; entry.permissions && j < i; ++j)
      if (acl.entries[j].permissions && !memcmp(entry.key, acl.entries[j].key, 32))
        return false;
  }
  return true;
}
void MastAdmin::aclCommand(char *command, Reply &reply) {
  if (!aclReady_) {
    strcpy(reply.text, "Error: Management ACL unavailable; ACL grants disabled; inspect NVS acl and restart");
    return;
  }
  if (!strncmp(command, "setperm ", 8)) {
    char *argument = command + 8;
    char *space = strchr(argument, ' ');
    uint8_t key[32];
    uint32_t permissions;
    if (!space) {
      strcpy(reply.text, "Error: setperm requires KEY64 PERMISSIONS (0..255)"); return;
    }
    *space++ = 0;
    if (!hex(argument, key, 32) || !number(space, permissions) || permissions > 255) {
      strcpy(reply.text, "Error: setperm requires KEY64 PERMISSIONS (0..255)"); return;
    }
    if ((permissions & PERM_ACL_ROLE_MASK) != PERM_ACL_ADMIN) {
      if (compiledTrusted(key)) {
        strcpy(reply.text, "Error: compiled owner recovery authority cannot be revoked"); return;
      }
      if (settings_.trustedSet && !memcmp(key, settings_.trustedKey, 32)) {
        strcpy(reply.text, "Error: key retains legacy trust; use trust none before setperm downgrade/remove"); return;
      }
    }
    ACL next = acl_, actual;
    ACL::Entry *slot = nullptr;
    for (auto &entry : next.entries)
      if (entry.permissions && !memcmp(entry.key, key, 32)) { slot = &entry; break; }
    const bool remove = (permissions & PERM_ACL_ROLE_MASK) == PERM_ACL_GUEST;
    if (!slot && !remove)
      for (auto &entry : next.entries) if (!entry.permissions) { slot = &entry; break; }
    if (!slot) {
      strcpy(reply.text, remove ? "Error: Management ACL key absent" :
             "Error: Management ACL full (5); inspect get acl 1..5 and remove an obsolete key"); return;
    }
    if (remove) *slot = {};
    else { memcpy(slot->key, key, 32); slot->permissions = permissions; }
    mesh::Utils::sha256(next.digest, sizeof(next.digest), reinterpret_cast<const uint8_t *>(&next),
                        offsetof(ACL, digest));
    bool present;
    if (!mastRecord("acl", &next, sizeof(next), true, present) ||
        !mastRecord("acl", &actual, sizeof(actual), false, present) ||
        !present || !validACL(actual) || memcmp(&next, &actual, sizeof(next))) {
      aclReady_ = false;
      strcpy(reply.text, "Error: Management ACL commit/readback unknown; ACL grants disabled; inspect NVS acl and restart");
      return;
    }
    acl_ = next;
    strcpy(reply.text, "OK - saved Management/bot permission; new logins/jobs apply now; existing sessions expire normally");
    return;
  }
  unsigned used = 0;
  for (const auto &entry : acl_.entries) if (entry.permissions) ++used;
  if (!command[7]) {
    snprintf(reply.text, sizeof(reply.text),
             "Management ACL=%u/5; get acl KEY64|1..5; setperm KEY64 3=admin,0=remove; legacy/compiled trust retained", used);
    return;
  }
  const char *argument = command + 8;
  uint32_t slot;
  uint8_t key[32];
  unsigned permissions = 0;
  if (number(argument, slot) && slot >= 1 && slot <= ACLSlots) {
    const auto &entry = acl_.entries[slot - 1];
    if (!entry.permissions) {
      snprintf(reply.text, sizeof(reply.text), "Management ACL slot %u empty", unsigned(slot)); return;
    }
    memcpy(key, entry.key, 32);
    permissions = entry.permissions;
  } else if (hex(argument, key, 32)) {
    for (const auto &entry : acl_.entries)
      if (entry.permissions && !memcmp(entry.key, key, 32)) permissions = entry.permissions;
  } else {
    strcpy(reply.text, "Error: get acl requires KEY64 or slot 1..5"); return;
  }
  char encoded[65];
  for (unsigned i = 0; i < 32; ++i) snprintf(encoded + 2 * i, 3, "%02x", key[i]);
  snprintf(reply.text, sizeof(reply.text), "key=%s permissions=%u admin=%u legacy=%u compiled=%u",
           encoded, permissions, trusted(key),
           settings_.trustedSet && !memcmp(settings_.trustedKey, key, 32), compiledTrusted(key));
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
void MastAdmin::setupCommand(const char *command, Reply &reply) {
  if (!command[0] || !strcmp(command, "status")) {
    strcpy(reply.text, publicProvisioningStored() ?
        "Public setup saved and valid; generic application updates retain identities, settings and files" :
        "Error: public setup missing or invalid; use setup migrate on the configured private application before a generic update");
    return;
  }
  if (strcmp(command, "migrate")) {
    strcpy(reply.text, "Error: setup status|migrate; migrate saves initial private defaults without replacing SPIFFS"); return;
  }
#if defined(MESHCORE_PUBLIC_PROVISIONING) && MESHCORE_PUBLIC_PROVISIONING
  strcpy(reply.text, "Error: this application already uses public setup; use setup status"); return;
#else
  if (!mux_ || !journal_ || effect_ != Effect::None || temporary_) {
    strcpy(reply.text, "Error: setup migration requires committed radio and no pending administration change"); return;
  }
  struct Migration {
    PublicProvisioningRecord record{};
    WifiCredentials wifi{};
    PasswordRecord password{};
    ~Migration() { clearSecret(this, sizeof(*this)); }
  } migration;
  auto &record = migration.record;
  bool present, enabled;
  uint8_t width;
  if (!loadWifi(migration.wifi, present) || !loadWifiEnabled(enabled) ||
      !loadPassword(migration.password, present) || !loadOriginPathWidth(width)) {
    strcpy(reply.text, "Error: setup migration could not read saved WiFi, authority or path settings"); return;
  }
  memcpy(record.magic, "MCP\1", 4);
  record.roles = journal_->profile.enabled;
  record.pathWidth = width;
  const auto radio = mux_->currentConfiguration();
  queued_tx::put32(record.frequencyHz, radio.freq_hz);
  queued_tx::put32(record.bandwidthHz, radio.bw_hz);
  record.sf = radio.sf; record.cr = radio.cr; record.txDbm = radio.tx_power;
  const auto copy = [](char *out, size_t capacity, const char *text) {
    if (strlen(text) >= capacity) return false;
    strcpy(out, text); return true;
  };
  if (!copy(record.adminPassword, sizeof(record.adminPassword), adminPassword()) ||
      !copy(record.roomPassword, sizeof(record.roomPassword), roomPassword()) ||
      !copy(record.mastPassword, sizeof(record.mastPassword), present ? migration.password.password : mastPassword()) ||
      !copy(record.operatorPublicKey, sizeof(record.operatorPublicKey), operatorPublicKey()) ||
      !copy(record.trustedCompanionPublicKey, sizeof(record.trustedCompanionPublicKey), trustedCompanionPublicKey())) {
    strcpy(reply.text, "Error: setup migration authority fields exceed record limits"); return;
  }
  if (settings_.trustedSet)
    for (unsigned i = 0; i < 32; ++i)
      snprintf(record.trustedCompanionPublicKey + 2 * i, 3, "%02x", settings_.trustedKey[i]);
  if (!migration.wifi.ssid[0]) {
#ifdef WIFI_SSID
    if (!copy(migration.wifi.ssid, sizeof(migration.wifi.ssid), WIFI_SSID)) {
      strcpy(reply.text, "Error: initial WiFi SSID exceeds setup limit"); return;
    }
#endif
#ifdef WIFI_PWD
    if (!copy(migration.wifi.password, sizeof(migration.wifi.password), WIFI_PWD)) {
      strcpy(reply.text, "Error: initial WiFi password exceeds setup limit"); return;
    }
#endif
  }
  memcpy(record.wifiSsid, migration.wifi.ssid, sizeof(record.wifiSsid));
  memcpy(record.wifiPassword, migration.wifi.password, sizeof(record.wifiPassword));
  record.wifiEnabled = enabled;
  mesh::Utils::sha256(record.digest, sizeof(record.digest), reinterpret_cast<const uint8_t *>(&record),
                      offsetof(PublicProvisioningRecord, digest));
  char error[120]{};
  if (!retainPublicProvisioning(record, error, sizeof(error))) {
    snprintf(reply.text, sizeof(reply.text), "Error: %s", error); return;
  }
  strcpy(reply.text, "Saved public setup; identities, settings and files retained. Save mqtt and HTTPS settings before generic update");
#endif
}
void MastAdmin::execute(const char *input, Reply &reply, uint32_t invokingBotJob,
                        Transport transport, const uint8_t *nativeSender, size_t replyCapacity) {
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
#if MESHCORE_NODE_BACKUP
  if (!strcmp(input, "backup") || !strncmp(input, "backup ", 7) || !strcmp(input, "help backup")) {
    const bool native = transport == Transport::NativeEncrypted &&
                        management_->authenticatedNativeSender(nativeSender);
    if (invokingBotJob || (!native && transport != Transport::AuthenticatedWeb)) {
      strcpy(reply.text, "Error: node backups require authenticated Management RF or Web administration"); return;
    }
    nodeBackup().command(!strcmp(input, "help backup") ? "help" : input[6] ? input + 7 : "",
                         reply.text, std::min(replyCapacity, sizeof(reply.text)), native);
    return;
  }
#endif
  if (!strcmp(input, "setup") || !strncmp(input, "setup ", 6)) {
    if (invokingBotJob || (transport != Transport::AuthenticatedWeb &&
        !(transport == Transport::NativeEncrypted && management_->authenticatedNativeSender(nativeSender)))) {
      strcpy(reply.text, "Error: setup migration requires authenticated Management RF or Web administration"); return;
    }
    setupCommand(input[5] ? input + 6 : "", reply);
    return;
  }
  if (!strcmp(input, "cloudroom") || !strncmp(input, "cloudroom ", 10)) {
    if (invokingBotJob || (transport != Transport::AuthenticatedWeb &&
        !(transport == Transport::NativeEncrypted && management_->authenticatedNativeSender(nativeSender)))) {
      strcpy(reply.text, "Error: cloud room controls require direct authenticated Management RF or Web administration");
      return;
    }
    const char *command = input[9] ? input + 10 : "";
    if (transport != Transport::NativeEncrypted &&
        (!strncmp(command, "config begin ", 13) || !strncmp(command, "config chunk ", 13) ||
         !strncmp(command, "config commit ", 14))) {
      strcpy(reply.text, "Error: cloud room credential uploads require encrypted Management RF; config retain copies existing settings");
      return;
    }
    cloudRoomCommand(command, reply.text, std::min(replyCapacity, sizeof(reply.text)));
    return;
  }
  if (!strcmp(input, "packet") || !strncmp(input, "packet ", 7) || !strcmp(input, "help packet")) {
    if (invokingBotJob || (transport != Transport::AuthenticatedWeb &&
        !(transport == Transport::NativeEncrypted && management_->authenticatedNativeSender(nativeSender)))) {
      strcpy(reply.text, "Error: packet programs require direct authenticated Management RF or Web administration");
      return;
    }
#if defined(ARDUINO_ARCH_ESP32) && MESHCORE_ONCHIP_BOT
    packetProgramCommand(!strcmp(input, "help packet") ? "help" : input[6] ? input + 7 : "",
                         reply.text, std::min(replyCapacity, sizeof(reply.text)));
#else
    strcpy(reply.text, "Error: packet program controller unavailable on this target");
#endif
    return;
  }
  if (!strcmp(input, "mqtt") || !strncmp(input, "mqtt ", 5) || !strcmp(input, "help mqtt")) {
    if (invokingBotJob || (transport != Transport::AuthenticatedWeb &&
        !(transport == Transport::NativeEncrypted && management_->authenticatedNativeSender(nativeSender)))) {
      strcpy(reply.text, "Error: MQTT settings require direct authenticated administration"); return;
    }
    const char *command = !strcmp(input, "help mqtt") ? "help" : input[4] ? input + 5 : "";
    if (transport != Transport::NativeEncrypted &&
        (!strncmp(command, "username ", 9) || !strncmp(command, "password ", 9) ||
         !strncmp(command, "ca ", 3))) {
      strcpy(reply.text, "Error: MQTT credentials and CA staging require encrypted Management RF"); return;
    }
    observerConfigCommand(command, reply.text, std::min(replyCapacity, sizeof(reply.text)));
    return;
  }
  if (!strncmp(input, "bot https retain", 16) &&
      (invokingBotJob || (transport != Transport::AuthenticatedWeb &&
       !(transport == Transport::NativeEncrypted && management_->authenticatedNativeSender(nativeSender))))) {
    strcpy(reply.text, "Error: retaining HTTPS defaults requires authenticated Management RF or Web administration"); return;
  }
  if (networkClockCommand(input, reply.text, sizeof(reply.text), true) ||
      syslogCommand(input, reply.text, sizeof(reply.text)) ||
      diagnosticsCommand(input, reply.text, sizeof(reply.text))) return;
  if (!strcmp(input, "help") || !strncmp(input, "help ", 5)) {
    helpCommand(input[4] ? input + 5 : "", reply);
    return;
  }
  if (!strcmp(input, "wifi help") || !strncmp(input, "wifi help ", 10) ||
      !strcmp(input, "bot help") || !strncmp(input, "bot help ", 9)) {
    char argument[TextLimit + 1];
    const bool wifi = input[0] == 'w';
    const size_t offset = wifi ? 9 : 8;
    snprintf(argument, sizeof(argument), "%s%s", wifi ? "wifi" : "bot", input + offset);
    helpCommand(argument, reply);
    return;
  }
  if (!strcmp(input, "source help")) {
    helpCommand("source", reply);
    return;
  }
  if (!strcmp(input, "roles") || !strcmp(input, "roles list") || !strncmp(input, "roles list ", 11)) {
    rolesCommand(!strncmp(input, "roles list ", 11) ? input + 11 : "", reply);
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
    snprintf(reply.text, sizeof(reply.text), "%s (Build: %s)", ONCHIP_FIRMWARE_VERSION, __DATE__);
  } else if (!strcmp(command, "board")) {
    strcpy(reply.text, "ESP32; shared modem and on-device roles");
  } else if (!strcmp(command, "stats") || !strcmp(command, "get stats") ||
             !strncmp(command, "stats ", 6)) {
    statsCommand(!strncmp(command, "stats ", 6) ? command + 6 : "radio", reply);
  } else if (!strcmp(command, "get acl") || !strncmp(command, "get acl ", 8) ||
             !strncmp(command, "setperm ", 8)) {
    aclCommand(command, reply);
  } else if (!strncmp(command, "get ", 4) || !strncmp(command, "set ", 4)) {
    preferenceCommand(command, reply, transport, invokingBotJob);
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
  } else if (!strcmp(command, "bot contacts")) {
    commandBotService().contactStatus(reply.text, sizeof(reply.text));
  } else if (!strcmp(command, "bot diagnostics")) {
    commandBotService().diagnosticStatus(reply.text, sizeof(reply.text));
  } else if (!strcmp(command, "bot discovery") || !strncmp(command, "bot discovery ", 14)) {
    commandBotService().discoveryCommand(command[13] ? command + 14 : "", reply.text, sizeof(reply.text));
  } else if (!strcmp(command, "bot repeaters") || !strncmp(command, "bot repeaters ", 14)) {
    commandBotService().repeaterCommand(command[13] ? command + 14 : "", reply.text, sizeof(reply.text));
  } else if (!strcmp(command, "bot membership") || !strncmp(command, "bot membership ", 15) ||
             !strcmp(command, "bot access") || !strncmp(command, "bot access ", 11) ||
             !strcmp(command, "bot thread") || !strncmp(command, "bot thread ", 11)) {
    commandBotService().radioPolicyCommand(command + 4, reply.text, sizeof(reply.text));
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
    if (!number(command + 11, mask) || mask > 31) {
      strcpy(reply.text, "Error: bot events MASK 0..31; 1 startup, 2 connectivity, 4 message, 8 node_status, 16 recurring"); return;
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
  } else if (!strcmp(command, "bot aliases") || !strncmp(command, "bot aliases ", 12)) {
    commandBotService().targetAliasesCommand(command[11] ? command + 12 : "", reply.text, sizeof(reply.text));
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
      BotRadioPolicy::Membership membership;
      strcpy(membership.name, name);
      policy.setMembership(0, membership);
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
    for (const auto &peer : extraReplay_.peers) if (peer.timestamp) ++used;
    if (!command[11]) {
      snprintf(reply.text, sizeof(reply.text),
               "RF peers=%u/10 compiled-owner=%u; sessions=6+compiled; auth peer 1..10; auth status KEY; auth forget KEY",
               used, ownerReplay_.peer.timestamp != 0);
    } else {
      uint8_t key[32];
      if (!hex(command + 12, key, 32)) {
        strcpy(reply.text, "Error: auth status requires a full 32-byte public key"); return;
      }
      snprintf(reply.text, sizeof(reply.text),
               "key=%s trusted=%u compiled=%u timestamp=%lu peers=%u/10",
               command + 12, trusted(key), compiledTrusted(key),
               static_cast<unsigned long>(lastTimestamp(key)), used);
    }
  } else if (!strncmp(command, "auth peer ", 10)) {
    uint32_t slot;
    if (!number(command + 10, slot) || slot < 1 || slot > ReplaySlots) {
      strcpy(reply.text, "Error: auth peer requires slot 1..10"); return;
    }
    const auto &peer = slot <= LegacyReplaySlots ? replay_.peers[slot - 1] :
                       extraReplay_.peers[slot - LegacyReplaySlots - 1];
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
    ExtraReplay extra = extraReplay_;
    bool found = false, foundExtra = false;
    for (auto &peer : next.peers)
      if (peer.timestamp && !memcmp(peer.key, key, 32)) { peer = {}; found = true; }
    for (auto &peer : extra.peers)
      if (peer.timestamp && !memcmp(peer.key, key, 32)) { peer = {}; foundExtra = true; }
    if (foundExtra)
      mesh::Utils::sha256(extra.digest, sizeof(extra.digest), reinterpret_cast<const uint8_t *>(&extra),
                          offsetof(ExtraReplay, digest));
    if (!found && !foundExtra) {
      strcpy(reply.text, "Error: principal absent"); return;
    }
    if (!saveReplay(next, extra)) {
      ready_ = false;
      strcpy(reply.text, "Error: replay forget commit/readback unknown; restart before native administration"); return;
    }
    replay_ = next;
    extraReplay_ = extra;
    strcpy(reply.text, "Forgot principal replay state and session; fresh login required");
  } else if (!strncmp(command, "trust ", 6)) {
    Settings next = settings_, actual;
    if (!strcmp(command + 6, "none")) {
      next.trustedSet = 0; memset(next.trustedKey, 0, 32);
    } else if (hex(command + 6, next.trustedKey, 32)) next.trustedSet = 1;
    else { strcpy(reply.text, "Error: trust requires a full 32-byte public key"); return; }
    bool present;
    if (!mastRecord("settings", &next, sizeof(next), true, present) ||
        !mastRecord("settings", &actual, sizeof(actual), false, present) ||
        !present || memcmp(&next, &actual, sizeof(next))) {
      strcpy(reply.text, "Error: trust commit/readback unknown; inspect saved settings and restart"); return;
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
    const auto &recovery = radio_network::wifiRecovery();
    const auto bits = WiFi.getStatusBits();
    const bool connected = recovery.enabled() && (bits & STA_CONNECTED_BIT) &&
                           (bits & STA_HAS_IP_BIT) && uint32_t(WiFi.localIP());
    snprintf(reply.text, sizeof(reply.text),
             "wifi saved=%u ssid-bytes=%u connected=%u enabled=%u attempts=%u retry-ms=%u",
             saved, saved ? unsigned(strlen(credentials.ssid)) : 0, connected,
             recovery.enabled(), recovery.attempts(), unsigned(recovery.retryIn(millis())));
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
    const bool encoded = !strncmp(value, "hex ", 4);
    if (!encoded && (ssid || strcmp(value, "-"))) {
      char literal[TextLimit + 1];
      snprintf(literal, sizeof(literal), "set wifi.%s %s", ssid ? "ssid" : "pwd", value);
      wifiCommand(literal, reply, transport, invokingBotJob);
      clearSecret(literal, sizeof(literal));
      return;
    }
    if (encoded) value += 4;
    Settings next = settings_;
    bool present;
    if (!loadWifi(next.wifi, present)) {
      strcpy(reply.text, "Error: saved WiFi credentials unavailable; field unchanged"); return;
    }
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
        (p[1] != 7800 && p[1] != 7810 && p[1] != 10400 && p[1] != 10420 &&
         p[1] != 15600 && p[1] != 15630 && p[1] != 20800 && p[1] != 20830 &&
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
  if (effect_ == Effect::Radio || effect_ == Effect::Temporary || effect_ == Effect::CAD ||
      effect_ == Effect::Interference || effect_ == Effect::Airtime || effect_ == Effect::Controls) {
    if (mux_->isActuallyTransmitting() || mux_->physicalRadio().isReceiving()) return;
    const bool temporary = effect_ == Effect::Temporary;
    const bool applied = effect_ == Effect::CAD ? mux_->applyMastCAD(nextCAD_) :
                         effect_ == Effect::Interference ? mux_->applyMastInterference(nextInterference_) :
                         effect_ == Effect::Airtime ? mux_->applyMastAirtimeFactor(nextAirtime_) :
                         effect_ == Effect::Controls ?
                             mux_->applyMastControls(nextAGCSeconds_, nextRxConfigured_, nextRxBoost_) :
                                                 mux_->applyMastConfiguration(nextRadio_, !temporary);
    if (!applied) {
      strcpy(outcome_, "Error: shared-radio apply/storage failed; inspect saved/applied status before retry");
    } else {
      if (temporary) {
        restoreAt_ = now + duration_;
        temporary_ = true;
      }
      strcpy(outcome_, effect_ == Effect::CAD ? "CAD applied and saved; effective generation advanced" :
                       temporary ? "Temporary radio applied; restore timer active" :
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
