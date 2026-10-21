// SPDX-License-Identifier: Apache-2.0
// Saved cloud-room record versions, boot resolution and the ownership rule.
#include "CloudRoomLifecycle.h"
#include "CloudRoomPreferences.h"
#include "CloudRoomService.h"
#include "CloudRoomSettings.h"
#include "RoleIdentity.h"
#include <SPIFFS.h>
#include <Utils.h>
#include <mbedtls/x509_crt.h>
#include <esp_heap_caps.h>
#include <cassert>
#include <map>
#include <string>

unsigned long millis() { return 0; }
void delay(unsigned long) {}
using namespace onchip;
namespace onchip { void cloudRoomBootResetForTest(); }
namespace {
// Digest of the 0.1.19 fixture record, computed outside this code base's helpers.
constexpr char LegacyDigest[] = "bec4e39d2198ccb3c07fe1f1741be0aa408f55a62e3c1fcb603df270c8fa2d2e";
std::map<std::string, std::vector<uint8_t>> records;
bool failJournalWrite;
unsigned events, powerLossAt;
struct PowerLoss {};
void tick(const char *) {
  if (powerLossAt && ++events == powerLossAt) throw PowerLoss{};
}
bool roomKeyPresent;
uint8_t roomKey[32];
std::string hex(const uint8_t *bytes, size_t size) {
  const char alphabet[] = "0123456789abcdef";
  std::string text;
  for (size_t i = 0; i < size; ++i) { text += alphabet[bytes[i] >> 4]; text += alphabet[bytes[i] & 15]; }
  return text;
}
std::string command(const std::string &input) {
  char reply[163]{};
  assert(cloudRoomPreferencesCommand(input.c_str(), reply, sizeof(reply)));
  assert(!strstr(reply, "fixture-token") && !strstr(reply, "BEGIN CERTIFICATE"));
  assert(strlen(reply) <= 145);
  return reply;
}
void sha(const void *data, size_t size, uint8_t out[32]) {
  mesh::Utils::sha256(out, 32, static_cast<const uint8_t *>(data), size);
}
// A 0.1.19 record built from the original layout, without the version 2 helpers.
CloudRoomSettings legacy(bool enabled = true) {
  CloudRoomSettings settings{};
  memcpy(settings.magic, "CRC\1", 4); settings.enabled = enabled; settings.count = 1;
  auto &alias = settings.aliases[0];
  strcpy(alias.host, "room.example"); strcpy(alias.address, "192.0.2.1");
  strcpy(alias.ca, "-----BEGIN CERTIFICATE-----\nfixture\n-----END CERTIFICATE-----\n");
  strcpy(alias.token, "fixture-token"); strcpy(alias.id, "room-1"); strcpy(alias.name, "Test Room");
  for (unsigned i = 0; i < 32; ++i) alias.publicKey[i] = uint8_t(0x40 + i);
  sha(&settings, offsetof(CloudRoomSettings, digest), settings.digest);
  return settings;
}
CloudRoomSettingsV2 owner() {
  CloudRoomSettingsV2 settings{};
  memcpy(settings.magic, "CRC\2", 4); settings.enabled = 1; settings.count = 1;
  auto &alias = settings.aliases[0];
  strcpy(alias.base.host, "rooms.example"); strcpy(alias.base.token, "fixture-token");
  strcpy(alias.base.id, "r2q7x3bndkamw4qzfxe7abcdmn2xyz"); strcpy(alias.base.name, "Fleet");
  for (unsigned i = 0; i < 32; ++i) alias.base.publicKey[i] = uint8_t(0x12 + i);
  alias.role = CloudRoleOwner; alias.caMode = CloudCaSystem; alias.flags = CloudFlagConverted;
  return settings;
}
template <size_t N> void setText(char (&field)[N], const char *text) {
  memset(field, 0, N);
  strcpy(field, text);
}
void put(const char *path, const void *data, size_t size) {
  const auto *bytes = static_cast<const uint8_t *>(data);
  filesystem_test::files[path] = {bytes, bytes + size};
}
void putLegacy(const CloudRoomSettings &settings) { put("/cloudroom-0.bin", &settings, sizeof(settings)); }
void reset() {
  records.clear(); filesystem_test::files.clear(); failJournalWrite = false;
  cloudRoomBootResetForTest();
  assert(psram_test::allocations.empty());
}
std::vector<uint8_t> &savedFile() {
  const auto journal = records.find("cloud-room");
  return filesystem_test::files.at(journal == records.end() ? "/cloudroom-0.bin" :
      journal->second[4] ? "/cloudroom-1.bin" : "/cloudroom-0.bin");
}
// The 0.1.19 loader: only a valid version 1 file loads.
bool legacyLoads(const std::vector<uint8_t> &file) {
  if (file.size() != sizeof(CloudRoomSettings)) return false;
  CloudRoomSettings settings;
  memcpy(&settings, file.data(), sizeof(settings));
  uint8_t digest[32];
  sha(&settings, offsetof(CloudRoomSettings, digest), digest);
  return !memcmp(digest, settings.digest, 32) && cloudRoomSettingsFields(settings);
}
bool publish(CloudRoomSettingsV2 &settings, std::string *why = nullptr) {
  char reply[163]{};
  const bool ok = cloudRoomPublishSettings(settings, reply, sizeof(reply));
  if (why) *why = reply;
  return ok;
}
CloudRoomSettingsV2 load() {
  CloudRoomSettingsV2 settings{};
  assert(cloudRoomLoadSettings(settings) == CloudRoomLoad::Ready);
  return settings;
}
std::string fileDigest() {
  const auto &file = savedFile();
  return hex(file.data() + file.size() - 32, 32);
}
}
namespace onchip {
unsigned cloudRoomAliases() { return 0; }
bool identityPublicKey(const char *role, uint8_t publicKey[32], bool) {
  if (strcmp(role, "room") || !roomKeyPresent) return false;
  memcpy(publicKey, roomKey, 32); return true;
}
bool mastRecord(const char *key, void *data, size_t size, bool write, bool &present) {
  assert(!strcmp(key, "cloud-room") && size == 72);
  if (write) {
    tick("journal.before");
    if (failJournalWrite) return false;
    auto *bytes = static_cast<const uint8_t *>(data);
    records[key] = {bytes, bytes + size};
    tick("journal.after");
  }
  present = records.count(key);
  if (!write && present) memcpy(data, records.at(key).data(), size);
  return true;
}
#ifdef CLOUDROOM_RUNTIME_PROVIDER_TEST
CloudRoomSettings providerRecord = legacy();
bool providerSystemRoots, providerPresent = true;
const CloudRoomConfiguration *cloudRoomCompiledConfiguration() {
  static CloudRoomConfiguration configuration;
  if (!providerPresent) return nullptr;
  configuration.count = 1;
  const auto &alias = providerRecord.aliases[0];
  configuration.peers[0] = {alias.host, alias.address, providerSystemRoots ? "" : alias.ca, alias.token,
                            alias.id, providerSystemRoots};
  configuration.aliases[0] = {alias.id, alias.name, {}};
  memcpy(configuration.aliases[0].publicKey, alias.publicKey, 32);
  return &configuration;
}
#endif
}
namespace {
void version1AndLifecycleEdits() {
  const auto v1 = legacy();
  assert(hex(v1.digest, 32) == LegacyDigest);
  putLegacy(v1);
  const auto *config = cloudRoomConfiguration();
  assert(config && config->count == 1 && !config->peers[0].systemRoots &&
         !strcmp(config->peers[0].token, "fixture-token"));
  CloudRoomAliasInfo info;
  assert(cloudRoomAliasInfo(0, info) && !info.role && !info.flags && !info.systemRoots);
  assert(!cloudRoomAliasInfo(1, info) && psram_test::allocations.size() == 1);
  assert(command("config hash") == LegacyDigest);
  // A node that never links keeps a version 1 record, byte for byte.
  assert(command("enable off") == "Cloud room settings saved; restart to apply");
  assert(savedFile().size() == sizeof(CloudRoomSettings) && command("config hash") != LegacyDigest);
  assert(command("enable on") == "Cloud room settings saved; restart to apply");
  assert(savedFile().size() == sizeof(CloudRoomSettings) && command("config hash") == LegacyDigest);
  assert(command("config retain") == "Error: already saved; nothing to retain");

  auto settings = load();
  assert(settings.count == 1 && !memcmp(settings.magic, "CRC\2", 4) &&
         settings.aliases[0].caMode == CloudCaPinned && !settings.aliases[0].role);
  settings.aliases[0].role = CloudRoleOwner; settings.aliases[0].flags = CloudFlagAdvertOnce | CloudFlagConverted;
  assert(publish(settings) && savedFile().size() == sizeof(CloudRoomSettingsV2));
  const std::string v2Digest = command("config hash");
  assert(v2Digest == fileDigest());
  // enable off / on keeps the version 2 fields and returns to the same bytes.
  assert(command("enable off") == "Cloud room settings saved; restart to apply");
  assert(command("config hash") != v2Digest && !load().enabled);
  assert(command("enable on") == "Cloud room settings saved; restart to apply");
  assert(command("config hash") == v2Digest);
  settings = load();
  assert(settings.aliases[0].role == CloudRoleOwner && settings.aliases[0].caMode == CloudCaPinned &&
         settings.aliases[0].flags == (CloudFlagAdvertOnce | CloudFlagConverted));
  cloudRoomBootResetForTest();
  assert(cloudRoomConfiguration() && cloudRoomAliasInfo(0, info) && info.role == CloudRoleOwner &&
         info.flags == 3 && !info.systemRoots);

  // System roots: no PEM is stored or parsed.
  const unsigned parsed = cloudroom_certificate_test::parsed;
  memset(settings.aliases[0].base.ca, 0, sizeof(settings.aliases[0].base.ca));
  settings.aliases[0].caMode = CloudCaSystem;
  assert(publish(settings) && cloudroom_certificate_test::parsed == parsed);
  cloudRoomBootResetForTest();
  config = cloudRoomConfiguration();
  assert(config && config->peers[0].systemRoots && !config->peers[0].ca[0]);

  // Unlinking everything is a version 2 record that the 0.1.19 loader refuses.
  settings = load();
  settings.count = 0; memset(settings.aliases, 0, sizeof(settings.aliases));
  assert(publish(settings) && savedFile().size() == sizeof(CloudRoomSettingsV2) && !legacyLoads(savedFile()));
  cloudRoomBootResetForTest();
  assert(!cloudRoomConfiguration() && psram_test::allocations.empty());
  assert(command("config status").find("saved=1 enabled=1 aliases=0") != std::string::npos);
  assert(command("enable off") == "Cloud room settings saved; restart to apply");
  // A settings change that fits version 1 goes back to version 1.
  put("/cloudroom-0.bin", &v1, sizeof(v1)); records.clear();
  assert(legacyLoads(filesystem_test::files["/cloudroom-0.bin"]));
  settings = load();
  assert(publish(settings) && command("config hash") == LegacyDigest);
  assert(psram_test::allocations.empty());
}
void validatorRules() {
  const auto good = owner();
  assert(cloudRoomSettingsFieldsV2(good));
  auto frontend = good;
  frontend.aliases[0].role = CloudRoleFrontend; frontend.aliases[0].flags = CloudFlagAdvertOnce;
  assert(cloudRoomSettingsFieldsV2(frontend));
  strcpy(frontend.aliases[0].base.ca, "-----BEGIN CERTIFICATE-----\nx\n-----END CERTIFICATE-----");
  frontend.aliases[0].caMode = CloudCaPinned; frontend.aliases[0].flags = 0;
  assert(cloudRoomSettingsFieldsV2(frontend));
  auto empty = good;
  empty.count = 0; memset(empty.aliases, 0, sizeof(empty.aliases));
  assert(cloudRoomSettingsFieldsV2(empty));
  struct Case { const char *name; void (*change)(CloudRoomSettingsV2 &); };
  const Case cases[] = {
    {"role", [](CloudRoomSettingsV2 &s) { s.aliases[0].role = 2; }},
    {"caMode", [](CloudRoomSettingsV2 &s) { s.aliases[0].caMode = 2; }},
    {"reserved byte", [](CloudRoomSettingsV2 &s) { s.aliases[0].reserved = 1; }},
    {"flag bit 2", [](CloudRoomSettingsV2 &s) { s.aliases[0].flags |= 4; }},
    {"converted frontend", [](CloudRoomSettingsV2 &s) { s.aliases[0].role = CloudRoleFrontend; }},
    {"first key byte 0", [](CloudRoomSettingsV2 &s) { s.aliases[0].base.publicKey[0] = 0; }},
    {"first key byte ff", [](CloudRoomSettingsV2 &s) { s.aliases[0].base.publicKey[0] = 255; }},
    {"system roots with PEM", [](CloudRoomSettingsV2 &s) { setText(s.aliases[0].base.ca, "x"); }},
    {"pinned without PEM", [](CloudRoomSettingsV2 &s) { s.aliases[0].caMode = CloudCaPinned; }},
    {"count 3", [](CloudRoomSettingsV2 &s) { s.count = 3; }},
    {"magic", [](CloudRoomSettingsV2 &s) { s.magic[3] = 1; }},
    {"enabled 2", [](CloudRoomSettingsV2 &s) { s.enabled = 2; }},
    {"header reserved", [](CloudRoomSettingsV2 &s) { s.reserved[1] = 1; }},
    {"unused slot data", [](CloudRoomSettingsV2 &s) { s.aliases[1].role = 1; }},
    {"unused slot after count 0", [](CloudRoomSettingsV2 &s) { s.count = 0; }},
    {"empty token", [](CloudRoomSettingsV2 &s) { s.aliases[0].base.token[0] = 0; }},
    {"duplicate key", [](CloudRoomSettingsV2 &s) {
       s.count = 2; s.aliases[1] = s.aliases[0]; setText(s.aliases[1].base.id, "other"); }},
    {"duplicate host and id", [](CloudRoomSettingsV2 &s) {
       s.count = 2; s.aliases[1] = s.aliases[0]; s.aliases[1].base.publicKey[5] ^= 1; }},
  };
  for (const auto &entry : cases) {
    auto bad = good;
    entry.change(bad);
    if (cloudRoomSettingsFieldsV2(bad)) { fprintf(stderr, "accepted: %s\n", entry.name); abort(); }
  }
  auto two = good;
  two.count = 2; two.aliases[1] = good.aliases[0];
  setText(two.aliases[1].base.id, "other"); two.aliases[1].base.publicKey[5] ^= 1;
  assert(cloudRoomSettingsFieldsV2(two));
  assert(cloudRoomFitsV1(two) == false);
  auto plain = frontend;
  plain.aliases[0].flags = 0;
  assert(cloudRoomFitsV1(plain) && !cloudRoomFitsV1(empty));
}
void corruptRecords() {
  const auto write = [](CloudRoomSettingsV2 &s, size_t size, bool refresh = true) {
    std::vector<uint8_t> bytes(sizeof(s));
    memcpy(bytes.data(), &s, sizeof(s));
    if (refresh) sha(bytes.data(), sizeof(s) - 32, bytes.data() + sizeof(s) - 32);
    bytes.resize(size);
    filesystem_test::files["/cloudroom-0.bin"] = bytes;
    records.clear();
  };
  const auto invalid = [&](const char *why) {
    const auto reply = command("config hash");
    if (reply.find("invalid") == std::string::npos) { fprintf(stderr, "accepted: %s (%s)\n", why, reply.c_str()); abort(); }
    assert(command("config status").find("invalid") != std::string::npos);
    cloudRoomBootResetForTest();
    assert(!cloudRoomConfiguration() && psram_test::allocations.empty());
  };
  auto s = owner();
  write(s, sizeof(s)); assert(command("config hash") != "none" && command("config hash").find("invalid") == std::string::npos);
  write(s, sizeof(s), false); filesystem_test::files["/cloudroom-0.bin"].back() ^= 1; invalid("digest bit");
  write(s, sizeof(s)); filesystem_test::files["/cloudroom-0.bin"].resize(sizeof(s) - 1); invalid("truncated v2");
  write(s, sizeof(s)); filesystem_test::files["/cloudroom-0.bin"].push_back(0); invalid("extended v2");
  auto bad = s; bad.aliases[0].role = 2; write(bad, sizeof(bad)); invalid("fields with valid digest");
  bad = s; memcpy(bad.magic, "CRC\1", 4); write(bad, sizeof(bad)); invalid("v1 magic in a v2 file");
  const auto v1 = legacy();
  auto wrongMagic = v1;
  memcpy(wrongMagic.magic, "CRC\2", 4); sha(&wrongMagic, offsetof(CloudRoomSettings, digest), wrongMagic.digest);
  put("/cloudroom-0.bin", &wrongMagic, sizeof(wrongMagic)); records.clear(); invalid("v2 magic in a v1 file");
  auto wrongCount = v1;
  wrongCount.count = 0; sha(&wrongCount, offsetof(CloudRoomSettings, digest), wrongCount.digest);
  put("/cloudroom-0.bin", &wrongCount, sizeof(wrongCount)); records.clear(); invalid("count 0 in a v1 file");
  // Journal digest that names a different valid record.
  s = owner();
  write(s, sizeof(s));
  assert(command("enable off").find("saved") != std::string::npos);
  records["cloud-room"][8] ^= 1;
  uint8_t checksum[32];
  sha(records["cloud-room"].data(), 40, checksum);
  memcpy(records["cloud-room"].data() + 40, checksum, 32);
  invalid("journal digest mismatch");
  assert(command("enable on").find("Error") == 0);
}
std::string chunkCommand(const std::vector<uint8_t> &record, size_t index, size_t count) {
  return "config chunk " + hex(record.data() + record.size() - 32, 8) + " " + std::to_string(index) + " " +
         hex(record.data() + index * 48, count);
}
std::vector<uint8_t> v2File(const CloudRoomSettingsV2 &settings) {
  std::vector<uint8_t> record(sizeof(settings));
  memcpy(record.data(), &settings, sizeof(settings));
  sha(record.data(), record.size() - 32, record.data() + record.size() - 32);
  return record;
}
void uploads() {
  assert(command("config api") == "Cloud room config ABI=1 bytes=9296 chunk=48 aliases=2 apply=restart");
  assert(command("config api 2") == "Cloud room config ABI=2 bytes=9304 chunk=48 aliases=2 apply=restart");
  const auto record = v2File(owner());
  const std::string begin = "config begin " + hex(record.data() + record.size() - 32, 32);
  assert(command(begin) == "Cloud room upload ready received=0");
  assert(command(chunkCommand(record, 194, 48)).find("index invalid") != std::string::npos);
  assert(command(chunkCommand(record, 0, 40)).find("size or hex invalid") != std::string::npos);
  assert(command(chunkCommand(record, 193, 34)).find("size or hex invalid") != std::string::npos);
  assert(command(chunkCommand(record, 193, 48)).find("size or hex invalid") != std::string::npos);
  assert(command(chunkCommand(record, 1, 48)).find("out of order") != std::string::npos);
  for (size_t index = 0; index < 193; ++index)
    assert(command(chunkCommand(record, index, 48)) == "Cloud room chunk saved received=" + std::to_string(index * 48 + 48));
  assert(command(chunkCommand(record, 193, 40)) == "Cloud room chunk saved received=9304");
  assert(command(chunkCommand(record, 193, 40)) == "Cloud room chunk saved received=9304");
  assert(command("config commit " + hex(record.data() + record.size() - 32, 8)) == "Cloud room settings saved; restart to apply");
  assert(savedFile().size() == 9304 && command("config hash") == fileDigest());
  assert(command("config status") == "Cloud room saved=1 enabled=1 aliases=1 live=0 upload=0 apply=restart");
  // A version 1 upload still ends with a 32-byte chunk.
  const auto v1 = legacy();
  std::vector<uint8_t> old(sizeof(v1));
  memcpy(old.data(), &v1, sizeof(v1));
  assert(command("config begin " + std::string(LegacyDigest)) == "Cloud room upload ready received=0");
  for (size_t index = 0; index < 193; ++index) command(chunkCommand(old, index, 48));
  assert(command(chunkCommand(old, 193, 32)) == "Cloud room chunk saved received=9296");
  assert(command("config commit " + std::string(LegacyDigest, 16)) == "Cloud room settings saved; restart to apply");
  assert(savedFile().size() == 9296 && command("config hash") == LegacyDigest);
  // count 0 uploads as version 2.
  auto none = owner();
  none.count = 0; memset(none.aliases, 0, sizeof(none.aliases));
  const auto emptyRecord = v2File(none);
  assert(command("config begin " + hex(emptyRecord.data() + emptyRecord.size() - 32, 32)) == "Cloud room upload ready received=0");
  for (size_t index = 0; index < 193; ++index) command(chunkCommand(emptyRecord, index, 48));
  command(chunkCommand(emptyRecord, 193, 40));
  assert(command("config commit " + hex(emptyRecord.data() + emptyRecord.size() - 32, 8)) == "Cloud room settings saved; restart to apply");
  assert(savedFile().size() == 9304 && command("config status").find("aliases=0") != std::string::npos);
}
void powerLoss() {
  auto next = owner();
  const auto before = legacy();
  unsigned completed = 0;
  for (unsigned at = 1; at < 200; ++at) {
    reset();
    putLegacy(before);
    filesystem_test::step = tick;
    events = 0; powerLossAt = at;
    bool lost = false;
    try { assert(publish(next)); } catch (const PowerLoss &) { lost = true; }
    powerLossAt = 0; filesystem_test::step = nullptr;
    // Whatever moment power failed, the next boot loads either the old or the new record.
    const std::string hash = command("config hash");
    assert(hash == LegacyDigest || hash == hex(v2File(next).data() + sizeof(next) - 32, 32));
    cloudRoomBootResetForTest();
    assert(cloudRoomConfiguration() && command("config status").find("invalid") == std::string::npos);
    if (!lost) { assert(hash != LegacyDigest); ++completed; break; }
    // A retry after the failure succeeds.
    assert(publish(next) && command("config hash") != LegacyDigest);
  }
  assert(completed == 1);
  // A failed journal write leaves the previous record selected.
  reset();
  putLegacy(before);
  failJournalWrite = true;
  std::string why;
  assert(!publish(next, &why) && why.find("outcome unknown") != std::string::npos);
  assert(command("config hash") == LegacyDigest);
  failJournalWrite = false;
  assert(publish(next) && command("config hash") != LegacyDigest);
  // An invalid settings structure is refused before anything is written.
  auto bad = next;
  bad.aliases[0].role = 7;
  const auto written = filesystem_test::files;
  assert(!publish(bad, &why) && why.find("fields invalid") != std::string::npos && filesystem_test::files == written);
}
void ownership() {
  roomKeyPresent = true;
  for (unsigned i = 0; i < 32; ++i) roomKey[i] = uint8_t(0x12 + i);
  assert(!cloudRoomOwnsLocalRoom());
  auto settings = owner();
  assert(publish(settings) && cloudRoomOwnsLocalRoom() && psram_test::allocations.empty());
  struct Case { const char *name; void (*change)(CloudRoomSettingsV2 &); };
  const Case cases[] = {
    {"not converted", [](CloudRoomSettingsV2 &s) { s.aliases[0].flags = CloudFlagAdvertOnce; }},
    {"frontend role", [](CloudRoomSettingsV2 &s) { s.aliases[0].role = 0; s.aliases[0].flags = 0; }},
    {"other key", [](CloudRoomSettingsV2 &s) { s.aliases[0].base.publicKey[9] ^= 1; }},
    {"disabled", [](CloudRoomSettingsV2 &s) { s.enabled = 0; }},
    {"nothing linked", [](CloudRoomSettingsV2 &s) { s.count = 0; memset(s.aliases, 0, sizeof(s.aliases)); }},
  };
  for (const auto &entry : cases) {
    auto changed = owner();
    entry.change(changed);
    if (!publish(changed) || cloudRoomOwnsLocalRoom()) { fprintf(stderr, "owned: %s\n", entry.name); abort(); }
  }
  // A role 0 alias holding the Room key (an adopted provider) never takes the Room role.
  auto adopted = legacy();
  for (unsigned i = 0; i < 32; ++i) adopted.aliases[0].publicKey[i] = uint8_t(0x12 + i);
  sha(&adopted, offsetof(CloudRoomSettings, digest), adopted.digest);
  reset();
  putLegacy(adopted);
  assert(!cloudRoomOwnsLocalRoom());
  // The owner stays registered but cannot stop a Room identity that is absent.
  reset();
  assert(publish(settings = owner()) && cloudRoomOwnsLocalRoom());
  roomKeyPresent = false;
  assert(!cloudRoomOwnsLocalRoom());
}
#ifdef CLOUDROOM_RUNTIME_PROVIDER_TEST
std::string stored() { return command("config hash"); }
void adopt() {
  const auto *provider = cloudRoomCompiledConfiguration();
  assert(provider && !records.count("cloud-room"));
  assert(cloudRoomConfiguration() == provider);
  // The record equals the provider, field for field, as a version 1 file.
  assert(savedFile().size() == sizeof(CloudRoomSettings) && stored() == LegacyDigest);
  assert(psram_test::allocations.empty());
  assert(cloudRoomConfiguration() == provider);
  cloudRoomBootResetForTest();
  const auto *saved = cloudRoomConfiguration();
  assert(saved && saved != provider && saved->count == 1 && !strcmp(saved->peers[0].token, "fixture-token") &&
         psram_test::allocations.size() == 1);
  assert(stored() == LegacyDigest);
  // enable off applies to the adopted record even though the provider is linked.
  assert(command("enable off") == "Cloud room settings saved; restart to apply");
  cloudRoomBootResetForTest();
  assert(!cloudRoomConfiguration());
  assert(command("enable on") == "Cloud room settings saved; restart to apply");
}
void adoptThenUnlink() {
  assert(cloudRoomConfiguration() == cloudRoomCompiledConfiguration());
  auto settings = load();
  assert(settings.count == 1 && !settings.aliases[0].role && settings.aliases[0].caMode == CloudCaPinned);
  settings.count = 0; memset(settings.aliases, 0, sizeof(settings.aliases));
  assert(publish(settings));
  cloudRoomBootResetForTest();
  // The unlinked record stays authoritative: the provider is not adopted again.
  assert(!cloudRoomConfiguration() && command("config status").find("aliases=0") != std::string::npos);
  assert(savedFile().size() == sizeof(CloudRoomSettingsV2));
}
void adoptFails() {
  failJournalWrite = true;
  const auto *provider = cloudRoomCompiledConfiguration();
  assert(cloudRoomConfiguration() == provider);
  assert(command("config hash") == "none" && psram_test::allocations.empty());
  failJournalWrite = false;
  cloudRoomBootResetForTest();
  assert(cloudRoomConfiguration() == provider && command("config hash") == LegacyDigest);
}
void savedWins() {
  auto settings = owner();
  assert(publish(settings));
  const auto hash = stored();
  std::string log;
  serial_test::capture = &log;
  const auto *config = cloudRoomConfiguration();
  serial_test::capture = nullptr;
  assert(config && config != cloudRoomCompiledConfiguration() && !strcmp(config->peers[0].host, "rooms.example") &&
         config->peers[0].systemRoots);
  assert(log.find("source=saved saved=v2 hash=") != std::string::npos &&
         log.find("differ from the compiled provider; the saved settings stay in use") != std::string::npos);
  assert(stored() == hash && psram_test::allocations.size() == 1);
  // A version 2 record also beats the provider after a lifecycle change on a frontend.
  reset();
  settings = owner(); settings.aliases[0].flags = 0; settings.aliases[0].role = 0;
  settings.aliases[0].caMode = CloudCaSystem;
  assert(publish(settings) && cloudRoomConfiguration() != cloudRoomCompiledConfiguration());
}
void staleVersion1() {
  auto stale = legacy();
  strcpy(stale.aliases[0].token, "old-fixture-token"); sha(&stale, offsetof(CloudRoomSettings, digest), stale.digest);
  putLegacy(stale);
  const auto before = filesystem_test::files;
  const auto *provider = cloudRoomCompiledConfiguration();
  std::string log;
  serial_test::capture = &log;
  assert(cloudRoomConfiguration() == provider && filesystem_test::files == before && records.empty());
  serial_test::capture = nullptr;
  assert(log.find("source=compiled provider saved=v1") != std::string::npos);
  assert(!cloudRoomOwnsLocalRoom());
  // A record with the same fields as the provider, or any version 2 record, is not stale.
  reset();
  auto same = legacy(false);
  putLegacy(same);
  assert(!cloudRoomConfiguration() && filesystem_test::files.at("/cloudroom-0.bin").size() == sizeof(same));
}
void corruptWithProvider() {
  auto bad = legacy();
  bad.digest[0] ^= 1;
  putLegacy(bad);
  const auto before = filesystem_test::files;
  const auto *provider = cloudRoomCompiledConfiguration();
  assert(cloudRoomConfiguration() == provider);
  assert(filesystem_test::files == before && records.empty());
  assert(command("config hash").find("invalid") != std::string::npos);
}
void retainSystemRoots() {
  providerSystemRoots = true;
  strcpy(providerRecord.aliases[0].ca, "");
  // The provider is adopted automatically; a node whose adoption failed can still retain.
  failJournalWrite = true;
  const auto *provider = cloudRoomCompiledConfiguration();
  assert(cloudRoomConfiguration() == provider && !records.count("cloud-room"));
  failJournalWrite = false;
  assert(command("config retain") == "Cloud room settings retained; generic application will load them after update");
  assert(savedFile().size() == sizeof(CloudRoomSettingsV2));
  const auto settings = load();
  assert(settings.aliases[0].caMode == CloudCaSystem && !settings.aliases[0].base.ca[0] && !settings.aliases[0].role);
  assert(command("config retain") == "Error: already saved; nothing to retain");
}
#endif
}
int main(int argc, char **argv) {
  assert(argc == 2);
  const std::string mode = argv[1];
  filesystem_test::step = nullptr;
  if (mode == "records") version1AndLifecycleEdits();
  else if (mode == "validator") validatorRules();
  else if (mode == "corrupt") corruptRecords();
  else if (mode == "upload") uploads();
  else if (mode == "powerloss") powerLoss();
  else if (mode == "owner") ownership();
#ifdef CLOUDROOM_RUNTIME_PROVIDER_TEST
  else if (mode == "adopt") adopt();
  else if (mode == "adopt-unlink") adoptThenUnlink();
  else if (mode == "adopt-fail") adoptFails();
  else if (mode == "saved-wins") savedWins();
  else if (mode == "stale") staleVersion1();
  else if (mode == "corrupt-provider") corruptWithProvider();
  else if (mode == "retain-system") retainSystemRoots();
#endif
  else { fprintf(stderr, "unknown mode %s\n", argv[1]); return 2; }
  return 0;
}
