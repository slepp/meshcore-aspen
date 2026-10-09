// SPDX-License-Identifier: Apache-2.0
#include "CloudRoomPreferences.h"
#if (defined(MESHCORE_CLOUD_ROOM) && MESHCORE_CLOUD_ROOM) || \
    (defined(ONCHIP_CLOUD_ROOM_RUNTIME_CONFIG) && ONCHIP_CLOUD_ROOM_RUNTIME_CONFIG)
#include "CloudRoomService.h"
#include "CloudRoomSettings.h"
#include "RoleStorage.h"
#include "MastSource.h"
#include <Arduino.h>
#include <SPIFFS.h>
#include <Utils.h>
#include <mbedtls/x509_crt.h>
#include <algorithm>
#include <stdio.h>
#include <stdlib.h>

namespace onchip {
namespace {
constexpr char Stage[] = "/cloudroom.stage";
struct Journal {
  uint8_t magic[4]{'C','R','J',1}, file = 0, reserved[3]{};
  uint8_t digest[32]{}, checksum[32]{};
};
static_assert(sizeof(Journal) == 72, "Cloud room journal layout changed");
enum class Saved { None, Valid, Invalid };
#if ONCHIP_CLOUD_ROOM_RUNTIME_CONFIG
CloudRoomSettings *bootSettings;
CloudRoomConfiguration bootConfig;
bool bootRead;
#endif
uint8_t uploadDigest[32]{};
bool uploading;
void wipe(void *data, size_t size) {
  auto *bytes = static_cast<volatile uint8_t *>(data);
  while (size--) *bytes++ = 0;
}
struct SettingsBuffer {
  CloudRoomSettings *value = allocateRoleStorage<CloudRoomSettings>("cloud-room settings");
  ~SettingsBuffer() {
    if (value) wipe(value, sizeof(*value));
    releaseRoleStorage(value);
  }
};
void digest(const void *data, size_t size, uint8_t hash[32]) {
  mesh::Utils::sha256(hash, 32, static_cast<const uint8_t *>(data), size);
}
void filename(uint8_t index, char (&path)[24]) {
  snprintf(path, sizeof(path), "/cloudroom-%u.bin", index);
}
bool valid(const CloudRoomSettings &settings) {
  uint8_t hash[32];
  digest(&settings, offsetof(CloudRoomSettings, digest), hash);
  return !memcmp(hash, settings.digest, 32) && cloudRoomSettingsFields(settings);
}
bool read(const char *path, CloudRoomSettings &settings) {
  auto file = SPIFFS.open(path, "r");
  return file && file.size() == sizeof(settings) &&
      file.read(reinterpret_cast<uint8_t *>(&settings), sizeof(settings)) == sizeof(settings) &&
      file.size() == sizeof(settings) && valid(settings);
}
Saved saved(CloudRoomSettings &settings, Journal &journal) {
  bool present = false;
  if (!mastRecord("cloud-room", &journal, sizeof(journal), false, present)) return Saved::Invalid;
  if (present) {
    uint8_t hash[32];
    digest(&journal, offsetof(Journal, checksum), hash);
    if (memcmp(journal.magic, "CRJ\1", 4) || journal.file > 1 ||
        journal.reserved[0] || journal.reserved[1] || journal.reserved[2] ||
        memcmp(hash, journal.checksum, 32)) return Saved::Invalid;
  } else {
    journal = {};
    if (!SPIFFS.exists("/cloudroom-0.bin")) return Saved::None;
  }
  char path[24]; filename(journal.file, path);
  if (!read(path, settings)) return Saved::Invalid;
  if (present && memcmp(journal.digest, settings.digest, 32)) return Saved::Invalid;
  memcpy(journal.digest, settings.digest, 32);
  return Saved::Valid;
}
bool certificates(const CloudRoomSettings &settings) {
  for (unsigned i = 0; i < settings.count; ++i) {
    mbedtls_x509_crt chain;
    mbedtls_x509_crt_init(&chain);
    const int result = mbedtls_x509_crt_parse(&chain,
        reinterpret_cast<const uint8_t *>(settings.aliases[i].ca),
        strlen(settings.aliases[i].ca) + 1);
    mbedtls_x509_crt_free(&chain);
    if (result != 0) return false;
  }
  return true;
}
bool publish(CloudRoomSettings &settings, char *reply, size_t capacity) {
  SettingsBuffer previous;
  if (!previous.value) {
    snprintf(reply, capacity, "Error: cloud room settings allocation failed"); return false;
  }
  Journal journal;
  const Saved state = saved(*previous.value, journal);
  if (state == Saved::Invalid) {
    snprintf(reply, capacity, "Error: saved cloud room settings invalid; restore the private node backup"); return false;
  }
  if (state == Saved::Valid && !memcmp(previous.value, &settings, sizeof(settings))) {
    snprintf(reply, capacity, "Cloud room settings saved; restart to apply"); return true;
  }
  const uint8_t next = state == Saved::None ? 1 : journal.file ^ 1;
  char path[24]; filename(next, path);
  auto file = SPIFFS.open(path, "w");
  if (!file) {
    snprintf(reply, capacity, "Error: cloud room settings file unavailable; saved selection unchanged"); return false;
  }
  const bool written = file.write(reinterpret_cast<const uint8_t *>(&settings), sizeof(settings)) == sizeof(settings);
  file.flush(); file.close();
  if (!written || !read(path, *previous.value) || memcmp(previous.value, &settings, sizeof(settings))) {
    snprintf(reply, capacity, "Error: cloud room settings write/readback failed; saved selection unchanged"); return false;
  }
  journal = {}; journal.file = next;
  memcpy(journal.digest, settings.digest, 32);
  digest(&journal, offsetof(Journal, checksum), journal.checksum);
  bool present = false;
  Journal actual;
  if (!mastRecord("cloud-room", &journal, sizeof(journal), true, present) ||
      !mastRecord("cloud-room", &actual, sizeof(actual), false, present) ||
      !present || memcmp(&actual, &journal, sizeof(journal))) {
    snprintf(reply, capacity, "Error: cloud room save outcome unknown; inspect config hash before retry"); return false;
  }
  snprintf(reply, capacity, "Cloud room settings saved; restart to apply");
  return true;
}
int hex(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}
bool unhex(const char *text, uint8_t *bytes, size_t size) {
  if (strlen(text) != size * 2) return false;
  for (size_t i = 0; i < size; ++i) {
    const int a = hex(text[i * 2]), b = hex(text[i * 2 + 1]);
    if (a < 0 || b < 0) return false;
    bytes[i] = uint8_t(a * 16 + b);
  }
  return true;
}
void hashText(const uint8_t hash[32], char (&text)[65]) {
  constexpr char alphabet[] = "0123456789abcdef";
  for (unsigned i = 0; i < 32; ++i) {
    text[i * 2] = alphabet[hash[i] >> 4]; text[i * 2 + 1] = alphabet[hash[i] & 15];
  }
  text[64] = 0;
}
}
#if ONCHIP_CLOUD_ROOM_RUNTIME_CONFIG
const CloudRoomConfiguration *cloudRoomConfiguration() {
  if (bootRead) return bootSettings && bootSettings->enabled ? &bootConfig : nullptr;
  bootRead = true;
  SettingsBuffer buffer;
  if (!buffer.value) { Serial.println("Cloud room settings allocation failed; service disabled"); return nullptr; }
  Journal journal;
  const Saved state = saved(*buffer.value, journal);
  if (state == Saved::Invalid) {
    Serial.println("Cloud room saved settings invalid; restore the private node backup"); return nullptr;
  }
  if (state != Saved::Valid || !buffer.value->enabled) return nullptr;
  if (!certificates(*buffer.value)) {
    Serial.println("Cloud room CA certificate invalid; replace saved configuration"); return nullptr;
  }
  bootSettings = buffer.value; buffer.value = nullptr;
  bootConfig.count = bootSettings->count;
  for (unsigned i = 0; i < bootConfig.count; ++i) {
    const auto &alias = bootSettings->aliases[i];
    bootConfig.peers[i] = {alias.host, alias.address, alias.ca, alias.token, alias.id};
    bootConfig.aliases[i].id = alias.id; bootConfig.aliases[i].name = alias.name;
    memcpy(bootConfig.aliases[i].publicKey, alias.publicKey, 32);
  }
  return &bootConfig;
}
#endif
bool cloudRoomPreferencesCommand(const char *command, char *reply, size_t capacity) {
  const auto error = [&](const char *message) { snprintf(reply, capacity, "Error: %s", message); return true; };
  if (!strcmp(command, "config api")) {
#if ONCHIP_CLOUD_ROOM_RUNTIME_CONFIG
    snprintf(reply, capacity, "Cloud room config ABI=1 bytes=9296 chunk=48 aliases=2 apply=restart"); return true;
#else
    return error("this image uses compiled cloud room settings; config retain before a generic application update");
#endif
  }
  if (!strcmp(command, "config status") || !strcmp(command, "config hash")) {
    SettingsBuffer buffer;
    if (!buffer.value) return error("cloud room settings allocation failed");
    Journal journal;
    const Saved state = saved(*buffer.value, journal);
    if (state == Saved::Invalid) return error("saved cloud room settings invalid; restore the private node backup");
    if (!strcmp(command, "config hash")) {
      if (state == Saved::None) snprintf(reply, capacity, "none");
      else { char hash[65]; hashText(buffer.value->digest, hash); snprintf(reply, capacity, "%s", hash); }
    } else {
      snprintf(reply, capacity, "Cloud room saved=%u enabled=%u aliases=%u live=%u upload=%u apply=%s",
          unsigned(state == Saved::Valid), state == Saved::Valid ? unsigned(buffer.value->enabled) : 0,
          state == Saved::Valid ? unsigned(buffer.value->count) : 0,
          cloudRoomAliases(), unsigned(uploading),
#if ONCHIP_CLOUD_ROOM_RUNTIME_CONFIG
          "restart");
#else
          "generic-update");
#endif
    }
    return true;
  }
  if (!strcmp(command, "config retain")) {
    if (uploading) return error("cloud room upload in progress; commit or abort it first");
    const auto *configuration = cloudRoomConfiguration();
    if (!configuration || !configuration->count || configuration->count > 2)
      return error("running cloud room configuration unavailable");
    SettingsBuffer buffer;
    if (!buffer.value) return error("cloud room settings allocation failed");
    auto &settings = *buffer.value;
    memcpy(settings.magic, "CRC\1", 4);
    settings.enabled = 1; settings.count = configuration->count;
    const auto copy = [](char *out, size_t size, const char *text, bool required = true) {
      if (!text) return !required;
      const size_t length = strnlen(text, size);
      if (length >= size) return false;
      memcpy(out, text, length + 1); return true;
    };
    for (unsigned i = 0; i < settings.count; ++i) {
      auto &alias = settings.aliases[i];
      const auto &peer = configuration->peers[i];
      const auto &metadata = configuration->aliases[i];
      if (!copy(alias.host, sizeof(alias.host), peer.host) ||
          !copy(alias.address, sizeof(alias.address), peer.address, false) ||
          !copy(alias.ca, sizeof(alias.ca), peer.ca) ||
          !copy(alias.token, sizeof(alias.token), peer.token) ||
          !copy(alias.id, sizeof(alias.id), metadata.id) ||
          !copy(alias.name, sizeof(alias.name), metadata.name) ||
          !peer.alias || strcmp(peer.alias, alias.id))
        return error("running cloud room fields exceed saved settings limits");
      memcpy(alias.publicKey, metadata.publicKey, 32);
    }
    digest(&settings, offsetof(CloudRoomSettings, digest), settings.digest);
    if (!valid(settings) || !certificates(settings)) return error("running cloud room fields or CA invalid");
    if (publish(settings, reply, capacity))
      snprintf(reply, capacity, "Cloud room settings retained; generic application will load them after update");
    return true;
  }
#if !ONCHIP_CLOUD_ROOM_RUNTIME_CONFIG
  if (!strncmp(command, "config", 6) || !strncmp(command, "enable", 6))
    return error("this image uses compiled cloud room settings; config retain before a generic application update");
#endif
  if (!strncmp(command, "enable ", 7)) {
    if (strcmp(command + 7, "on") && strcmp(command + 7, "off")) return error("cloudroom enable on|off");
    if (uploading) return error("cloud room upload in progress; commit or abort it first");
    SettingsBuffer buffer;
    if (!buffer.value) return error("cloud room settings allocation failed");
    Journal journal;
    const Saved state = saved(*buffer.value, journal);
    if (state != Saved::Valid) return error("cloud room configuration unavailable; install a private profile");
    buffer.value->enabled = !strcmp(command + 7, "on");
    digest(buffer.value, offsetof(CloudRoomSettings, digest), buffer.value->digest);
    publish(*buffer.value, reply, capacity); return true;
  }
  if (!strcmp(command, "config abort")) {
    if (SPIFFS.exists(Stage) && !SPIFFS.remove(Stage)) return error("cloud room staging file removal failed");
    uploading = false; wipe(uploadDigest, sizeof(uploadDigest));
    snprintf(reply, capacity, "Cloud room upload discarded; saved selection unchanged"); return true;
  }
  if (!strncmp(command, "config begin ", 13)) {
    uint8_t hash[32];
    if (!unhex(command + 13, hash, 32)) return error("cloudroom config begin SHA256 requires 64 lowercase hex digits");
    if (uploading && !memcmp(hash, uploadDigest, 32)) {
      auto file = SPIFFS.open(Stage, "r");
      if (!file || file.size() > sizeof(CloudRoomSettings)) return error("cloud room staging file invalid; abort upload");
      snprintf(reply, capacity, "Cloud room upload ready received=%u", unsigned(file.size())); return true;
    }
    if (uploading) return error("cloud room upload in progress; abort it before replacing");
    auto file = SPIFFS.open(Stage, "w");
    if (!file) return error("cloud room staging file unavailable");
    file.flush();
    if (file.size()) return error("cloud room staging file truncation failed");
    uploading = true; memcpy(uploadDigest, hash, 32);
    snprintf(reply, capacity, "Cloud room upload ready received=0"); return true;
  }
  if (!strncmp(command, "config chunk ", 13)) {
    if (!uploading) return error("cloud room upload not started; use config begin");
    char identifier[65]; hashText(uploadDigest, identifier);
    if (strlen(command + 13) < 18 || strncmp(command + 13, identifier, 16) || command[29] != ' ')
      return error("cloud room upload ID differs; inspect config status");
    const char *number = command + 30;
    if (*number < '0' || *number > '9') return error("cloudroom config chunk ID16 INDEX HEX");
    char *end;
    const unsigned long index = strtoul(number, &end, 10);
    if (*end != ' ' || index > (sizeof(CloudRoomSettings) - 1) / 48) return error("cloud room chunk index invalid");
    const size_t offset = index * 48, count = std::min(size_t(48), sizeof(CloudRoomSettings) - offset);
    struct Chunk {
      uint8_t bytes[48]{}, actual[48]{};
      ~Chunk() { wipe(this, sizeof(*this)); }
    } chunk;
    auto &bytes = chunk.bytes;
    if (!unhex(end + 1, bytes, count)) return error("cloud room chunk size or hex invalid");
    auto file = SPIFFS.open(Stage, "r");
    if (!file || file.size() > sizeof(CloudRoomSettings)) return error("cloud room staging file invalid; abort upload");
    const size_t received = file.size();
    if (offset < received) {
      if (count > received - offset || !file.seek(offset) || file.read(chunk.actual, count) != count ||
          memcmp(chunk.actual, bytes, count)) return error("cloud room repeated chunk differs");
    } else {
      file.close();
      if (offset != received) return error("cloud room chunk out of order");
      file = SPIFFS.open(Stage, "a");
      if (!file || file.write(bytes, count) != count) {
        return error("cloud room chunk write failed; inspect upload before retry");
      }
      file.flush();
    }
    snprintf(reply, capacity, "Cloud room chunk saved received=%u", unsigned(file.size())); return true;
  }
  if (!strncmp(command, "config commit ", 14)) {
    if (!uploading) return error("cloud room upload not started; inspect config hash");
    char identifier[65]; hashText(uploadDigest, identifier);
    if (strlen(command + 14) != 16 || strncmp(command + 14, identifier, 16))
      return error("cloud room upload ID differs; inspect config status");
    SettingsBuffer buffer;
    if (!buffer.value) return error("cloud room settings allocation failed");
    if (!read(Stage, *buffer.value) || memcmp(buffer.value->digest, uploadDigest, 32))
      return error("cloud room upload fields, size or SHA256 invalid; saved selection unchanged");
    if (!certificates(*buffer.value)) return error("cloud room CA certificate invalid; saved selection unchanged");
    if (publish(*buffer.value, reply, capacity)) {
      uploading = false; wipe(uploadDigest, sizeof(uploadDigest));
      if (!SPIFFS.remove(Stage)) return error("cloud room settings saved; staging cleanup failed; restart to apply");
    }
    return true;
  }
  if (!strncmp(command, "config", 6)) return error("cloudroom config api|status|hash|begin SHA256|chunk ID16 INDEX HEX|commit ID16|abort");
  return false;
}
} // namespace onchip
#else
namespace onchip {
bool cloudRoomPreferencesCommand(const char *, char *, size_t) { return false; }
}
#endif
