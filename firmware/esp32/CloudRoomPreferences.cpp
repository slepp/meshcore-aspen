// SPDX-License-Identifier: Apache-2.0
#include "CloudRoomPreferences.h"
#if (defined(MESHCORE_CLOUD_ROOM) && MESHCORE_CLOUD_ROOM) || \
    (defined(ONCHIP_CLOUD_ROOM_RUNTIME_CONFIG) && ONCHIP_CLOUD_ROOM_RUNTIME_CONFIG)
#include "CloudRoomLifecycle.h"
#include "CloudRoomService.h"
#include "CloudRoomSettings.h"
#include "RoleIdentity.h"
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
using Settings = CloudRoomSettingsV2;
constexpr size_t V1Size = sizeof(CloudRoomSettings), V2Size = sizeof(Settings);
constexpr size_t ChunkBytes = 48, LastChunk = V1Size / ChunkBytes;
static_assert(V1Size % ChunkBytes == 32 && V2Size % ChunkBytes == 40 &&
                  V2Size / ChunkBytes == LastChunk, "Cloud room upload chunking changed");
#if ONCHIP_CLOUD_ROOM_RUNTIME_CONFIG
Settings *bootSettings;
CloudRoomConfiguration bootConfig;
const CloudRoomConfiguration *bootActive;
bool bootRead;
#endif
uint8_t uploadDigest[32]{};
bool uploading;
void wipe(void *data, size_t size) {
  auto *bytes = static_cast<volatile uint8_t *>(data);
  while (size--) *bytes++ = 0;
}
struct SettingsBuffer {
  Settings *value = allocateRoleStorage<Settings>("cloud-room settings");
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
// Reads a version 1 or 2 file. The result is always version 2 in memory; its digest
// field holds the digest of the bytes on disk. `version` reports the file's version.
bool read(const char *path, Settings &settings, uint8_t &version) {
  auto file = SPIFFS.open(path, "r");
  if (!file) return false;
  const size_t size = file.size();
  if ((size != V1Size && size != V2Size) ||
      file.read(reinterpret_cast<uint8_t *>(&settings), size) != size || file.size() != size) return false;
  uint8_t hash[32];
  if (size == V1Size) {
    auto &old = reinterpret_cast<CloudRoomSettings &>(settings);
    digest(&old, offsetof(CloudRoomSettings, digest), hash);
    if (memcmp(hash, old.digest, 32) || !cloudRoomSettingsFields(old)) return false;
    cloudRoomWiden(settings);
    version = 1;
    return true;
  }
  digest(&settings, offsetof(Settings, digest), hash);
  version = 2;
  return !memcmp(hash, settings.digest, 32) && cloudRoomSettingsFieldsV2(settings);
}
// Encodes `settings` as the lowest record version that can hold it into `scratch`.
// The returned digest points into `scratch`.
bool encode(const Settings &settings, Settings &scratch, size_t &size, const uint8_t *&hash) {
  if (cloudRoomFitsV1(settings)) {
    memset(&scratch, 0, V2Size);
    auto &old = reinterpret_cast<CloudRoomSettings &>(scratch);
    cloudRoomNarrow(settings, old);
    digest(&old, offsetof(CloudRoomSettings, digest), old.digest);
    size = V1Size; hash = old.digest;
    return cloudRoomSettingsFields(old);
  }
  memcpy(&scratch, &settings, V2Size);
  memcpy(scratch.magic, "CRC\2", 4);
  digest(&scratch, offsetof(Settings, digest), scratch.digest);
  size = V2Size; hash = scratch.digest;
  return cloudRoomSettingsFieldsV2(scratch);
}
Saved saved(Settings &settings, Journal &journal, uint8_t &version) {
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
  if (!read(path, settings, version)) return Saved::Invalid;
  if (present && memcmp(journal.digest, settings.digest, 32)) return Saved::Invalid;
  memcpy(journal.digest, settings.digest, 32);
  return Saved::Valid;
}
bool certificates(const Settings &settings) {
  for (unsigned i = 0; i < settings.count; ++i) {
    if (settings.aliases[i].caMode == CloudCaSystem) continue;
    mbedtls_x509_crt chain;
    mbedtls_x509_crt_init(&chain);
    const int result = mbedtls_x509_crt_parse(&chain,
        reinterpret_cast<const uint8_t *>(settings.aliases[i].base.ca),
        strlen(settings.aliases[i].base.ca) + 1);
    mbedtls_x509_crt_free(&chain);
    if (result != 0) return false;
  }
  return true;
}
bool publish(const Settings &settings, char *reply, size_t capacity) {
  SettingsBuffer previous, encoded;
  if (!previous.value || !encoded.value) {
    snprintf(reply, capacity, "Error: cloud room settings allocation failed"); return false;
  }
  size_t size = 0;
  const uint8_t *newDigest = nullptr;
  if (!encode(settings, *encoded.value, size, newDigest)) {
    snprintf(reply, capacity, "Error: cloud room settings fields invalid; saved selection unchanged"); return false;
  }
  Journal journal;
  uint8_t version = 0;
  const Saved state = saved(*previous.value, journal, version);
  if (state == Saved::Invalid) {
    snprintf(reply, capacity, "Error: saved cloud room settings invalid; restore the private node backup"); return false;
  }
  if (state == Saved::Valid && !memcmp(journal.digest, newDigest, 32)) {
    snprintf(reply, capacity, "Cloud room settings saved; restart to apply"); return true;
  }
  const uint8_t next = state == Saved::None ? 1 : journal.file ^ 1;
  char path[24]; filename(next, path);
  auto file = SPIFFS.open(path, "w");
  if (!file) {
    snprintf(reply, capacity, "Error: cloud room settings file unavailable; saved selection unchanged"); return false;
  }
  const bool written = file.write(reinterpret_cast<const uint8_t *>(encoded.value), size) == size;
  file.flush(); file.close();
  if (!written || !read(path, *previous.value, version) || memcmp(previous.value->digest, newDigest, 32)) {
    snprintf(reply, capacity, "Error: cloud room settings write/readback failed; saved selection unchanged"); return false;
  }
  journal = {}; journal.file = next;
  memcpy(journal.digest, newDigest, 32);
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
// Copies a running configuration (saved earlier or compiled in) into version 2 settings
// with role frontend and a pinned CA. False when a field does not fit the record.
bool fromRunning(const CloudRoomConfiguration &configuration, Settings &settings) {
  if (!configuration.count || configuration.count > 2) return false;
  memset(&settings, 0, sizeof(settings));
  memcpy(settings.magic, "CRC\2", 4);
  settings.enabled = 1; settings.count = uint8_t(configuration.count);
  const auto copy = [](char *out, size_t size, const char *text, bool required = true) {
    if (!text) return !required;
    const size_t length = strnlen(text, size);
    if (length >= size) return false;
    memcpy(out, text, length + 1); return true;
  };
  for (unsigned i = 0; i < settings.count; ++i) {
    auto &alias = settings.aliases[i];
    const auto &peer = configuration.peers[i];
    const auto &metadata = configuration.aliases[i];
    alias.caMode = peer.systemRoots ? CloudCaSystem : CloudCaPinned;
    if (!copy(alias.base.host, sizeof(alias.base.host), peer.host) ||
        !copy(alias.base.address, sizeof(alias.base.address), peer.address, false) ||
        !copy(alias.base.ca, sizeof(alias.base.ca), peer.ca, !peer.systemRoots) ||
        !copy(alias.base.token, sizeof(alias.base.token), peer.token) ||
        !copy(alias.base.id, sizeof(alias.base.id), metadata.id) ||
        !copy(alias.base.name, sizeof(alias.base.name), metadata.name) ||
        !peer.alias || strcmp(peer.alias, alias.base.id)) return false;
    memcpy(alias.base.publicKey, metadata.publicKey, 32);
  }
  return true;
}
#if ONCHIP_CLOUD_ROOM_RUNTIME_CONFIG
bool sameText(const char *a, const char *b) { return !strcmp(a ? a : "", b ? b : ""); }
// True when every saved alias equals the compiled provider's, field for field.
bool matchesProvider(const Settings &settings, const CloudRoomConfiguration &provider) {
  if (settings.count != provider.count) return false;
  for (unsigned i = 0; i < settings.count; ++i) {
    const auto &alias = settings.aliases[i];
    const auto &peer = provider.peers[i];
    const auto &metadata = provider.aliases[i];
    if (alias.role != CloudRoleFrontend || alias.flags ||
        (alias.caMode == CloudCaSystem) != peer.systemRoots ||
        !sameText(alias.base.host, peer.host) || !sameText(alias.base.address, peer.address) ||
        !sameText(alias.base.ca, peer.ca) || !sameText(alias.base.token, peer.token) ||
        !sameText(alias.base.id, metadata.id) || !sameText(alias.base.name, metadata.name) ||
        memcmp(alias.base.publicKey, metadata.publicKey, 32)) return false;
  }
  return true;
}
enum class Source { Off, Saved, Provider, Adopt };
// The saved record wins unless a version 1 record disagrees with the provider in
// the image (a rebuilt private image). A lifecycle write uses version 1 only when
// the settings fit it, so a version 2 record is always an operator-saved choice.
Source choose(Saved state, uint8_t version, const Settings &settings,
              const CloudRoomConfiguration *provider, const char *&warning) {
  warning = nullptr;
  switch (state) {
  case Saved::Valid:
    if (provider && version == 1 && !matchesProvider(settings, *provider)) {
      warning = "Cloud room saved settings differ from the compiled provider; using the provider for this boot";
      return Source::Provider;
    }
    return Source::Saved;
  case Saved::Invalid:
    if (provider) {
      warning = "Cloud room saved settings invalid; using the compiled provider for this boot";
      return Source::Provider;
    }
    warning = "Cloud room saved settings invalid; restore the private node backup";
    return Source::Off;
  case Saved::None:
    return provider ? Source::Adopt : Source::Off;
  }
  return Source::Off;
}
#endif
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
__attribute__((weak)) const CloudRoomConfiguration *cloudRoomCompiledConfiguration() { return nullptr; }
namespace {
void publishBootConfiguration(const Settings &settings) {
  bootConfig = {};
  bootConfig.count = settings.count;
  for (unsigned i = 0; i < bootConfig.count; ++i) {
    const auto &alias = settings.aliases[i];
    bootConfig.peers[i] = {alias.base.host, alias.base.address, alias.base.ca, alias.base.token,
                           alias.base.id, alias.caMode == CloudCaSystem};
    bootConfig.aliases[i].id = alias.base.id; bootConfig.aliases[i].name = alias.base.name;
    memcpy(bootConfig.aliases[i].publicKey, alias.base.publicKey, 32);
  }
}
const char *sourceName(Source source) {
  switch (source) {
  case Source::Saved: return "saved";
  case Source::Provider: return "compiled provider";
  case Source::Adopt: return "compiled provider (first boot, saving)";
  default: return "none";
  }
}
// One line per boot: which settings serve this boot and the saved record's
// version and digest prefix. A saved version 2 record that differs from the
// compiled provider says so, because it overrides a rebuilt image's token or CA.
void logSource(Source source, Saved state, uint8_t version, const Settings &settings,
               const CloudRoomConfiguration *provider) {
  char hash[65]; hashText(settings.digest, hash); hash[8] = 0;
  if (state == Saved::Valid)
    Serial.printf("Cloud room source=%s saved=v%u hash=%s aliases=%u\n", sourceName(source),
                  unsigned(version), hash, unsigned(settings.count));
  else
    Serial.printf("Cloud room source=%s saved=%s\n", sourceName(source),
                  state == Saved::None ? "none" : "invalid");
  if (source == Source::Saved && provider && !matchesProvider(settings, *provider))
    Serial.println("Cloud room saved settings differ from the compiled provider; the saved settings stay in use");
}
}
const CloudRoomConfiguration *cloudRoomConfiguration() {
  if (bootRead) return bootActive;
  bootRead = true;
  const auto *provider = cloudRoomCompiledConfiguration();
  SettingsBuffer buffer;
  if (!buffer.value) {
    Serial.println("Cloud room settings allocation failed; service disabled");
    return nullptr;
  }
  Journal journal;
  uint8_t version = 0;
  const Saved state = saved(*buffer.value, journal, version);
  const char *warning = nullptr;
  const Source source = choose(state, version, *buffer.value, provider, warning);
  if (warning) Serial.println(warning);
  logSource(source, state, version, *buffer.value, provider);
  switch (source) {
  case Source::Off: return nullptr;
  case Source::Provider: bootActive = provider; return provider;
  case Source::Adopt: {
    // First boot of an image with a compiled provider: save its aliases so the
    // lifecycle verbs can edit them. The provider serves this boot either way.
    char reason[96];
    if (!fromRunning(*provider, *buffer.value) || !certificates(*buffer.value))
      Serial.println("Cloud room compiled provider exceeds the saved settings limits; running it unsaved");
    else if (!publish(*buffer.value, reason, sizeof(reason)))
      Serial.printf("Cloud room adoption not saved (%s); retrying next boot\n", reason);
    bootActive = provider;
    return provider;
  }
  case Source::Saved: break;
  }
  if (!buffer.value->enabled || !buffer.value->count) return nullptr;
  if (!certificates(*buffer.value)) {
    Serial.println("Cloud room CA certificate invalid; replace saved configuration"); return nullptr;
  }
  bootSettings = buffer.value; buffer.value = nullptr;
  publishBootConfiguration(*bootSettings);
  bootActive = &bootConfig;
  return bootActive;
}
#ifdef ONCHIP_CLOUD_ROOM_TEST_HOOKS
// Native tests simulate a restart: the next cloudRoomConfiguration() resolves again.
void cloudRoomBootResetForTest() {
  if (bootSettings) wipe(bootSettings, sizeof(*bootSettings));
  releaseRoleStorage(bootSettings);
  bootSettings = nullptr; bootActive = nullptr; bootRead = false; bootConfig = {};
}
#endif
bool cloudRoomOwnsLocalRoom() {
  SettingsBuffer buffer;
  if (!buffer.value) return false;
  Journal journal;
  uint8_t version = 0;
  const Saved state = saved(*buffer.value, journal, version);
  const char *warning = nullptr;
  if (state != Saved::Valid || !buffer.value->enabled ||
      choose(state, version, *buffer.value, cloudRoomCompiledConfiguration(), warning) != Source::Saved)
    return false;
  for (unsigned i = 0; i < buffer.value->count; ++i) {
    const auto &alias = buffer.value->aliases[i];
    uint8_t room[32];
    if (alias.role == CloudRoleOwner && (alias.flags & CloudFlagConverted) &&
        identityPublicKey("room", room) && !memcmp(room, alias.base.publicKey, 32)) return true;
  }
  return false;
}
bool cloudRoomAliasInfo(unsigned alias, CloudRoomAliasInfo &info) {
  info = {};
  if (!bootActive || alias >= bootActive->count) return false;
  if (bootSettings && bootActive == &bootConfig) {
    const auto &entry = bootSettings->aliases[alias];
    info.role = entry.role; info.flags = entry.flags; info.systemRoots = entry.caMode == CloudCaSystem;
  }
  return true;
}
CloudRoomLoad cloudRoomLoadSettings(CloudRoomSettingsV2 &out) {
  Journal journal;
  uint8_t version = 0;
  const Saved state = saved(out, journal, version);
  if (state == Saved::Invalid) return CloudRoomLoad::Invalid;
  if (state == Saved::Valid) return CloudRoomLoad::Ready;
  const auto *provider = cloudRoomCompiledConfiguration();
  if (provider) return fromRunning(*provider, out) ? CloudRoomLoad::Ready : CloudRoomLoad::Unavailable;
  memset(&out, 0, sizeof(out));
  memcpy(out.magic, "CRC\2", 4);
  out.enabled = 1;
  return CloudRoomLoad::Ready;
}
bool cloudRoomPublishSettings(CloudRoomSettingsV2 &settings, char *reply, size_t capacity) {
  if (!certificates(settings)) {
    snprintf(reply, capacity, "Error: cloud room CA certificate invalid; saved selection unchanged");
    return false;
  }
  return publish(settings, reply, capacity);
}
bool cloudRoomUploadStaged() { return uploading; }
#endif
bool cloudRoomPreferencesCommand(const char *command, char *reply, size_t capacity) {
  const auto error = [&](const char *message) { snprintf(reply, capacity, "Error: %s", message); return true; };
  if (!strcmp(command, "config api") || !strcmp(command, "config api 2")) {
#if ONCHIP_CLOUD_ROOM_RUNTIME_CONFIG
    if (!strcmp(command, "config api 2"))
      snprintf(reply, capacity, "Cloud room config ABI=2 bytes=9304 chunk=48 aliases=2 apply=restart");
    else
      snprintf(reply, capacity, "Cloud room config ABI=1 bytes=9296 chunk=48 aliases=2 apply=restart");
    return true;
#else
    return error("this image uses compiled cloud room settings; config retain before a generic application update");
#endif
  }
  if (!strcmp(command, "config status") || !strcmp(command, "config hash")) {
    SettingsBuffer buffer;
    if (!buffer.value) return error("cloud room settings allocation failed");
    Journal journal;
    uint8_t version = 0;
    const Saved state = saved(*buffer.value, journal, version);
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
    {
      SettingsBuffer existing;
      if (!existing.value) return error("cloud room settings allocation failed");
      Journal journal;
      uint8_t version = 0;
      const Saved state = saved(*existing.value, journal, version);
      if (state == Saved::Invalid) return error("saved cloud room settings invalid; restore the private node backup");
      if (state == Saved::Valid) return error("already saved; nothing to retain");
    }
    const auto *configuration = cloudRoomConfiguration();
    if (!configuration || !configuration->count || configuration->count > 2)
      return error("running cloud room configuration unavailable");
    SettingsBuffer buffer;
    if (!buffer.value) return error("cloud room settings allocation failed");
    if (!fromRunning(*configuration, *buffer.value)) return error("running cloud room fields exceed saved settings limits");
    if (!certificates(*buffer.value)) return error("running cloud room fields or CA invalid");
    if (publish(*buffer.value, reply, capacity))
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
    uint8_t version = 0;
    const Saved state = saved(*buffer.value, journal, version);
    if (state != Saved::Valid) return error("cloud room configuration unavailable; install a private profile");
    buffer.value->enabled = !strcmp(command + 7, "on");
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
      if (!file || file.size() > V2Size) return error("cloud room staging file invalid; abort upload");
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
    if (*end != ' ' || index > LastChunk) return error("cloud room chunk index invalid");
    // Every chunk carries 48 bytes except the last, whose length selects the record version.
    const size_t offset = index * ChunkBytes, count = strlen(end + 1) / 2;
    if (!(count == ChunkBytes && offset + count <= V2Size) &&
        !(index == LastChunk && (count == V1Size - offset || count == V2Size - offset)))
      return error("cloud room chunk size or hex invalid");
    struct Chunk {
      uint8_t bytes[ChunkBytes]{}, actual[ChunkBytes]{};
      ~Chunk() { wipe(this, sizeof(*this)); }
    } chunk;
    auto &bytes = chunk.bytes;
    if (!unhex(end + 1, bytes, count)) return error("cloud room chunk size or hex invalid");
    auto file = SPIFFS.open(Stage, "r");
    if (!file || file.size() > V2Size) return error("cloud room staging file invalid; abort upload");
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
    uint8_t version = 0;
    if (!read(Stage, *buffer.value, version) || memcmp(buffer.value->digest, uploadDigest, 32))
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
