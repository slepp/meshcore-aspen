// SPDX-License-Identifier: Apache-2.0
#include "CloudRoomPreferences.h"
#include "CloudRoomService.h"
#include "CloudRoomSettings.h"
#include <SPIFFS.h>
#include <Utils.h>
#include <mbedtls/x509_crt.h>
#include <esp_heap_caps.h>
#include <cassert>
#include <map>
#include <string>

unsigned long millis() { return 0; }
void delay(unsigned long) {}
namespace {
std::map<std::string, std::vector<uint8_t>> records;
bool failJournalWrite, failJournalReadback, readbackPending;
void hash(onchip::CloudRoomSettings &settings) {
  mesh::Utils::sha256(settings.digest, 32, reinterpret_cast<const uint8_t *>(&settings),
      offsetof(onchip::CloudRoomSettings, digest));
}
onchip::CloudRoomSettings fixture(bool enabled = true) {
  onchip::CloudRoomSettings settings{};
  memcpy(settings.magic, "CRC\1", 4); settings.enabled = enabled; settings.count = 1;
  auto &alias = settings.aliases[0];
  strcpy(alias.host, "room.example");
  strcpy(alias.address, "192.0.2.1");
  strcpy(alias.ca, "-----BEGIN CERTIFICATE-----\nfixture\n-----END CERTIFICATE-----\n");
  strcpy(alias.token, "fixture-token"); strcpy(alias.id, "room-1"); strcpy(alias.name, "Test Room");
  for (unsigned i = 0; i < 32; ++i) alias.publicKey[i] = i;
  hash(settings);
  return settings;
}
std::string hex(const uint8_t *bytes, size_t size) {
  const char alphabet[] = "0123456789abcdef";
  std::string text;
  for (size_t i = 0; i < size; ++i) { text += alphabet[bytes[i] >> 4]; text += alphabet[bytes[i] & 15]; }
  return text;
}
std::string command(const std::string &input) {
  char reply[163]{};
  assert(onchip::cloudRoomPreferencesCommand(input.c_str(), reply, sizeof(reply)));
  assert(!strstr(reply, "fixture-token") && !strstr(reply, "BEGIN CERTIFICATE"));
  assert(strlen(reply) <= 145);
  return reply;
}
void upload(const onchip::CloudRoomSettings &settings) {
  const std::string identifier = hex(settings.digest, 8);
  assert(command("config begin " + hex(settings.digest, 32)) == "Cloud room upload ready received=0");
  assert(command("config begin " + std::string(64, '0')).find("in progress") != std::string::npos);
  assert(command("config chunk " + std::string(16, '0') + " 0 " + std::string(96, '0')).find("ID differs") != std::string::npos);
  const auto *bytes = reinterpret_cast<const uint8_t *>(&settings);
  for (size_t offset = 0; offset < sizeof(settings); offset += 48) {
    const size_t count = std::min(size_t(48), sizeof(settings) - offset);
    const std::string text = "config chunk " + identifier + " " + std::to_string(offset / 48) +
        " " + hex(bytes + offset, count);
    const std::string response = "Cloud room chunk saved received=" + std::to_string(offset + count);
    assert(command(text) == response);
    assert(command(text) == response);
    assert(command("config begin " + hex(settings.digest, 32)) ==
        "Cloud room upload ready received=" + std::to_string(offset + count));
  }
  assert(command("config commit " + std::string(16, '0')).find("ID differs") != std::string::npos);
}
std::string commit(const onchip::CloudRoomSettings &settings) {
  return command("config commit " + hex(settings.digest, 8));
}
}
namespace onchip {
unsigned cloudRoomAliases() { return 0; }
bool mastRecord(const char *key, void *data, size_t size, bool write, bool &present) {
  assert(!strcmp(key, "cloud-room") && size == 72);
  if (write) {
    if (failJournalWrite) return false;
    auto *bytes = static_cast<const uint8_t *>(data);
    records[key] = {bytes, bytes + size};
    readbackPending = failJournalReadback;
  } else if (readbackPending) { readbackPending = false; return false; }
  present = records.count(key);
  if (!write && present) memcpy(data, records.at(key).data(), size);
  return true;
}
#ifdef CLOUDROOM_COMPILED_PROVIDER_TEST
const CloudRoomConfiguration *cloudRoomConfiguration() {
  static auto settings = fixture();
  static CloudRoomConfiguration configuration;
  configuration.count = 1;
  const auto &alias = settings.aliases[0];
  configuration.peers[0] = {alias.host, alias.address, alias.ca, alias.token, alias.id};
  configuration.aliases[0] = {alias.id, alias.name, {}};
  memcpy(configuration.aliases[0].publicKey, alias.publicKey, 32);
  return &configuration;
}
#endif
}
int main(int argc, char **argv) {
  assert(argc == 2);
  auto settings = fixture();
  assert(onchip::cloudRoomSettingsFields(settings));
  for (unsigned kind = 0; kind < 8; ++kind) {
    auto invalid = settings;
    switch (kind) {
    case 0: invalid.enabled = 2; break;
    case 1: invalid.count = 0; break;
    case 2: invalid.count = 3; break;
    case 3: invalid.reserved[0] = 1; break;
    case 4: invalid.aliases[0].token[256] = 1; break;
    case 5: memset(invalid.aliases[0].host, 'x', sizeof(invalid.aliases[0].host)); break;
    case 6: strcpy(invalid.aliases[0].address, "192.00.2.1"); break;
    case 7: memset(invalid.aliases[0].publicKey, 255, 32); break;
    }
    assert(!onchip::cloudRoomSettingsFields(invalid));
  }
  auto two = settings;
  two.count = 2; two.aliases[1] = two.aliases[0];
  assert(!onchip::cloudRoomSettingsFields(two));
  two.aliases[1].publicKey[31] ^= 1;
  strcpy(two.aliases[1].host, "ROOM.EXAMPLE");
  assert(!onchip::cloudRoomSettingsFields(two));
  strcpy(two.aliases[1].id, "room-2");
  assert(onchip::cloudRoomSettingsFields(two));
#ifdef CLOUDROOM_COMPILED_PROVIDER_TEST
  assert(!strcmp(argv[1], "legacy"));
  assert(command("config hash") == "none");
  assert(command("config api").find("compiled cloud room") != std::string::npos);
  assert(command("config retain") == "Cloud room settings retained; generic application will load them after update");
  assert(command("config hash") == hex(settings.digest, 32));
  assert(command("config status").find("apply=generic-update") != std::string::npos);
  assert(command("enable off").find("compiled cloud room") != std::string::npos);
  assert(psram_test::allocations.empty());
  return 0;
#endif
  if (!strcmp(argv[1], "boot")) {
    const auto *bytes = reinterpret_cast<const uint8_t *>(&settings);
    filesystem_test::files["/cloudroom-0.bin"] = {bytes, bytes + sizeof(settings)};
    const auto *config = onchip::cloudRoomConfiguration();
    assert(config && config->count == 1 && !strcmp(config->peers[0].token, "fixture-token"));
    assert(psram_test::allocations.size() == 1);
    assert(command("enable off") == "Cloud room settings saved; restart to apply");
    assert(onchip::cloudRoomConfiguration() == config && config->count == 1);
    assert(command("config hash") != hex(settings.digest, 32));
    assert(psram_test::allocations.size() == 1);
    assert(command("config retain").find("settings retained") != std::string::npos);
    assert(command("config hash") == hex(settings.digest, 32));
    return 0;
  }
  if (!strcmp(argv[1], "disabled")) {
    settings.enabled = false; hash(settings);
    const auto *bytes = reinterpret_cast<const uint8_t *>(&settings);
    filesystem_test::files["/cloudroom-0.bin"] = {bytes, bytes + sizeof(settings)};
    assert(!onchip::cloudRoomConfiguration());
    assert(psram_test::allocations.empty());
    assert(command("enable on") == "Cloud room settings saved; restart to apply");
    assert(!onchip::cloudRoomConfiguration());
    return 0;
  }
  assert(!onchip::cloudRoomConfiguration());
  assert(command("config hash") == "none" && psram_test::allocations.empty());
  assert(command("enable on").find("unavailable") != std::string::npos);
  upload(settings);
  cloudroom_certificate_test::result = -1;
  assert(commit(settings).find("certificate invalid") != std::string::npos);
  assert(command("config hash") == "none");
  cloudroom_certificate_test::result = 0;
  filesystem_test::writeLimit = 100;
  assert(commit(settings).find("write/readback failed") != std::string::npos);
  filesystem_test::writeLimit = SIZE_MAX;
  assert(command("config hash") == "none");
  failJournalWrite = true;
  assert(commit(settings).find("outcome unknown") != std::string::npos);
  failJournalWrite = false;
  assert(command("config hash") == "none");
  assert(commit(settings) == "Cloud room settings saved; restart to apply");
  assert(command("config hash") == hex(settings.digest, 32));
  assert(!onchip::cloudRoomConfiguration() && psram_test::allocations.empty());
  const auto retained = settings;
  strcpy(settings.aliases[0].token, "new-fixture-token"); hash(settings);
  upload(settings);
  failJournalWrite = true;
  assert(commit(settings).find("outcome unknown") != std::string::npos);
  failJournalWrite = false;
  assert(command("config hash") == hex(retained.digest, 32));
  assert(commit(settings) == "Cloud room settings saved; restart to apply");
  assert(command("config hash") == hex(settings.digest, 32));
  strcpy(settings.aliases[0].token, "committed-fixture-token"); hash(settings);
  upload(settings);
  failJournalReadback = true;
  assert(commit(settings).find("outcome unknown") != std::string::npos);
  failJournalReadback = false;
  assert(command("config hash") == hex(settings.digest, 32));
  assert(commit(settings) == "Cloud room settings saved; restart to apply");
  upload(settings);
  filesystem_test::files["/cloudroom.stage"][0] ^= 1;
  assert(commit(settings).find("invalid") != std::string::npos);
  assert(command("config hash") == hex(settings.digest, 32));
  assert(command("config abort").find("discarded") != std::string::npos);
  assert(command("config status").find("upload=0") != std::string::npos);
  assert(psram_test::allocations.empty());
  assert(cloudroom_certificate_test::parsed == cloudroom_certificate_test::freed);
  records.at("cloud-room")[0] ^= 1;
  assert(command("config hash").find("invalid") != std::string::npos);
}
