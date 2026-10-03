// SPDX-License-Identifier: Apache-2.0
#include "TelemetryEndpoint.h"
#include "BotStore.h"
#include "BotJournal.h"
#include <SPIFFS.h>
#include <cassert>
#include <cstring>
#include <string>

using namespace onchip;
unsigned long millis() { return 0; }
void delay(unsigned long) {}
static const identity_test::Key EndpointKey{"mc-onchip", "telemetry-peer"};
static const char *const Slots[] = {"/telemetry-a.bin", "/telemetry-b.bin"};
static void clearFaults() {
  identity_test::failRead = identity_test::failWrite = identity_test::failCommit = false;
  identity_test::afterWrite = identity_test::afterCommit = nullptr;
  identity_test::readHook = nullptr;
  filesystem_test::failOpen = false;
  filesystem_test::writeLimit = filesystem_test::readLimit = SIZE_MAX;
  filesystem_test::appendOnRead = false;
  filesystem_test::afterWrite = filesystem_test::afterFlush = nullptr;
}
static void reboot() {
  clearFaults();
  identity_test::handles.clear();
  resetTelemetryEndpointForTest();
}
static void reset() {
  reboot();
  identity_test::durable.clear();
  identity_test::namespaces.clear();
  identity_test::entryCapacity = identity_test::peakEntries = 0;
  identity_test::eagerWrites = false;
  filesystem_test::files.clear();
}
static TelemetryEndpoint fixture(bool maximum = false) {
  TelemetryEndpoint endpoint;
  strcpy(endpoint.address, "192.0.2.1");
  strcpy(endpoint.host, "vm.example");
  strcpy(endpoint.path, "/write");
  const std::string begin = "-----BEGIN CERTIFICATE-----\n", end = "\n-----END CERTIFICATE-----\n";
  const std::string ca = begin + std::string(maximum ? 4096 - begin.size() - end.size() : 20, 'A') + end;
  strcpy(endpoint.ca, ca.c_str());
  strcpy(endpoint.token, (maximum ? std::string(256, 't') : "secret-token").c_str());
  if (maximum) {
    strcpy(endpoint.host, (std::string(60, 'a') + "." + std::string(60, 'b')).c_str());
    strcpy(endpoint.path, ("/" + std::string(120, 'p')).c_str());
    strcpy(endpoint.address, "192.168.100.100");
    endpoint.port = 65535;
  }
  assert(endpoint.valid());
  return endpoint;
}
static void seed(const char *space, const char *key, const void *bytes, size_t size) {
  nvs_handle_t handle;
  assert(nvs_open(space, NVS_READWRITE, &handle) == ESP_OK);
  assert(nvs_set_blob(handle, key, bytes, size) == ESP_OK);
  assert(nvs_commit(handle) == ESP_OK);
  nvs_close(handle);
}
static void seedLegacy(const TelemetryEndpoint &endpoint) {
  seed("mc-onchip", "telemetry-peer", &endpoint, sizeof(endpoint));
}
static void command(const std::string &text, bool good = true) {
  char reply[163];
  telemetryEndpointCommand(text.c_str(), reply, sizeof(reply));
  if ((strncmp(reply, "Error:", 6) != 0) != good) fprintf(stderr, "Command %s: %s\n", text.c_str(), reply);
  assert((strncmp(reply, "Error:", 6) != 0) == good);
}
static void stage(const TelemetryEndpoint &endpoint) {
  command(std::string("address ") + endpoint.address);
  command(std::string("host ") + endpoint.host);
  command(std::string("path ") + endpoint.path);
  command("port " + std::to_string(endpoint.port));
  command("ca clear"); command("token clear");
  for (bool ca : {true, false}) {
    const char *text = ca ? endpoint.ca : endpoint.token;
    for (size_t offset = 0; offset < strlen(text); offset += 56) {
      std::string hex;
      for (size_t i = offset; i < strlen(text) && i < offset + 56; ++i) {
        char byte[3];
        snprintf(byte, sizeof(byte), "%02x", unsigned(uint8_t(text[i])));
        hex += byte;
      }
      command(std::string(ca ? "ca " : "token ") + hex);
    }
  }
}
static void expect(const TelemetryEndpoint &expected) {
  TelemetryEndpoint endpoint;
  assert(telemetryEndpoint(endpoint));
  assert(!memcmp(&endpoint, &expected, sizeof(endpoint)));
  char status[163];
  telemetryEndpointCommand("status", status, sizeof(status));
  assert(strstr(status, "configured=1") && strstr(status, "storage=ok"));
  assert(!strstr(status, expected.token));
  assert(identity_test::handles.empty());
}
static void unavailable() {
  assert(!telemetryEndpointConfigured());
  TelemetryEndpoint endpoint;
  assert(!telemetryEndpoint(endpoint));
  char status[163];
  telemetryEndpointCommand("status", status, sizeof(status));
  assert(strstr(status, "configured=0") && strstr(status, "storage=error"));
  assert(identity_test::handles.empty());
}
static void retention() {
  reset();
  assert(!telemetryEndpointConfigured());
  auto endpoint = fixture(true);
  stage(endpoint); command("commit");
  assert(identity_test::durable.at(EndpointKey).size() == 40);
  assert(filesystem_test::files.size() == 1);
  reboot(); expect(endpoint);
  memset(endpoint.path, 0, sizeof(endpoint.path));
  strcpy(endpoint.path, "/replacement");
  command("path /replacement"); command("commit");
  assert(filesystem_test::files.size() == 2);
  assert(SPIFFS.usedBytes() == 2 * sizeof(TelemetryEndpoint));
  reboot(); expect(endpoint);
  puts("PASS maximum endpoint/CA/token restart retention, two bounded SPIFFS slots, native-only credentials");
}
static void migration() {
  for (bool maximum : {false, true}) {
    reset();
    const auto endpoint = fixture(maximum);
    seedLegacy(endpoint);
    // These are unrelated identity-bound settings/data and must not be rewritten.
    seed("mc-onchip", "command-bot", "identity", 8);
    seed("mc-bot-kv", "old-key-data", "retained", 8);
    const auto before = identity_test::durable;
    expect(endpoint);
    assert(identity_test::durable.at(EndpointKey).size() == 40);
    for (const auto &entry : before)
      if (entry.first != EndpointKey) assert(identity_test::durable.at(entry.first) == entry.second);
    const auto committed = identity_test::durable;
    const auto files = filesystem_test::files;
    const auto commits = identity_test::commits;
    reboot(); expect(endpoint);
    assert(identity_test::durable == committed && filesystem_test::files == files);
    assert(identity_test::commits == commits);
  }
  puts("PASS automatic legacy migration, verified replacement before reclaim, no restart rewrites or unrelated-data changes");
}
static void migrationFaults() {
  for (unsigned fault = 0; fault < 8; ++fault) {
    reset();
    const auto endpoint = fixture();
    seedLegacy(endpoint);
    const auto before = identity_test::durable;
    switch (fault) {
      case 0: filesystem_test::failOpen = true; break;
      case 1: filesystem_test::writeLimit = sizeof(endpoint) - 1; break;
      case 2: filesystem_test::afterFlush = [] { filesystem_test::files.at(Slots[0])[20] ^= 1; }; break;
      case 3: filesystem_test::readLimit = sizeof(endpoint) - 1; break;
      case 4: filesystem_test::appendOnRead = true; break;
      case 5: identity_test::failWrite = true; break;
      case 6: identity_test::failCommit = true; break;
      case 7: identity_test::failRead = true; break;
    }
    unavailable();
    assert(identity_test::durable == before);
    reboot(); expect(endpoint);
  }
  for (bool eager : {false, true}) {
    reset();
    auto endpoint = fixture();
    seedLegacy(endpoint);
    identity_test::eagerWrites = eager;
    identity_test::failCommit = true;
    unavailable();
    assert(identity_test::durable.at(EndpointKey).size() == (eager ? 40 : sizeof(endpoint)));
    reboot(); expect(endpoint);
  }
  reset();
  auto endpoint = fixture();
  seedLegacy(endpoint);
  identity_test::afterCommit = [] { identity_test::failRead = true; };
  unavailable();
  assert(identity_test::durable.at(EndpointKey).size() == 40);
  reboot(); expect(endpoint);
  puts("PASS migration mount/open, short/full write, flush corruption, short/growing read, NVS read/write/commit/readback faults");
}
struct PowerLoss {};
static void powerCuts() {
  for (bool legacy : {false, true}) for (unsigned cut = 0; cut < 4; ++cut) {
    reset();
    const auto old = fixture();
    seedLegacy(old);
    auto next = old;
    if (!legacy) {
      expect(old);
      strcpy(next.path, "/next");
      command("path /next");
    }
    identity_test::eagerWrites = true;
    switch (cut) {
      case 0:
        filesystem_test::writeLimit = 100;
        filesystem_test::afterWrite = [] { throw PowerLoss{}; };
        break;
      case 1: filesystem_test::afterFlush = [] { throw PowerLoss{}; }; break;
      case 2: identity_test::afterWrite = [] { throw PowerLoss{}; }; break;
      case 3: identity_test::afterCommit = [] { throw PowerLoss{}; }; break;
    }
    bool interrupted = false;
    try {
      if (legacy) telemetryEndpointConfigured();
      else command("commit");
    } catch (const PowerLoss &) { interrupted = true; }
    assert(interrupted);
    reboot();
    expect(legacy || cut < 2 ? old : next);
  }
  puts("PASS restart during partial file write, flush, reference replacement and commit: only complete old/new endpoint");
}
static void corruptionAndRetry() {
  for (unsigned corruption = 0; corruption < 8; ++corruption) {
    reset();
    auto endpoint = fixture();
    seedLegacy(endpoint); expect(endpoint);
    auto &reference = identity_test::durable.at(EndpointKey);
    auto &file = filesystem_test::files.at(Slots[reference[4]]);
    switch (corruption) {
      case 0: file[20] ^= 1; break;
      case 1: file.pop_back(); break;
      case 2: file.push_back(0); break;
      case 3: filesystem_test::files.clear(); break;
      case 4: reference[4] = 2; break;
      case 5: reference[5] = 1; break;
      case 6: reference.back() ^= 1; break;
      case 7: reference.pop_back(); break;
    }
    const auto before = identity_test::durable;
    reboot(); unavailable();
    assert(identity_test::durable == before);
  }
  reset();
  auto endpoint = fixture();
  endpoint.version = 2;
  seedLegacy(endpoint);
  const auto before = identity_test::durable;
  unavailable();
  assert(identity_test::durable == before && filesystem_test::files.empty());

  reset(); endpoint = fixture();
  seedLegacy(endpoint); expect(endpoint);
  command("path /second");
  identity_test::afterCommit = [] { identity_test::failRead = true; };
  command("commit", false); unavailable();
  clearFaults();
  command("path /third");
  filesystem_test::writeLimit = 100;
  command("commit", false); unavailable();
  reboot();
  strcpy(endpoint.path, "/second");
  expect(endpoint);
  puts("PASS corrupt/missing file and reference fail closed; retry re-reads authority after uncertain commit");
}
static void physicalHeadroom() {
  reset();
  identity_test::entryCapacity = 630;
  BotStore store;
  uint8_t bot[32]{1};
  std::atomic<uint32_t> generation{1}, grant{1};
  std::atomic<bool> shared{true};
  BotIoRequest request;
  request.token = {1, 1, 1}; request.kind = BotIoRequest::Put; request.principal[0] = 2;
  strcpy(request.key, "existing-note"); strcpy(request.value, "retain old identity data");
  BotIoResult result;
  const auto run = [&] { store.perform(bot, request, result, generation, shared, grant); };
  run(); assert(result.ok);
  const auto endpoint = fixture(true);
  seedLegacy(endpoint);
  assert(identity_test::blobEntries(sizeof(endpoint)) == 148);
  const size_t paddingEntries = 630 - identity_test::usedEntries() - 166;
  std::vector<uint8_t> other((paddingEntries - 3) * 32, 0x5a);
  seed("mc-onchip", "other-settings", other.data(), other.size());
  nvs_stats_t stats{};
  assert(nvs_get_stats(nullptr, &stats) == ESP_OK && stats.free_entries == 166);
  const auto before = identity_test::durable;
  bot[0] = 3; strcpy(request.key, "new-identity-note"); strcpy(request.value, "remember works");
  run();
  assert(!result.ok && strstr(result.error, "166 free, 197 required"));
  assert(identity_test::durable == before);

  expect(endpoint);
  assert(nvs_get_stats(nullptr, &stats) == ESP_OK);
  assert(stats.total_entries == 630 && stats.free_entries == 310);
  assert(identity_test::blobEntries(identity_test::durable.at(EndpointKey).size()) == 4);
  for (const auto &entry : before)
    if (entry.first != EndpointKey) assert(identity_test::durable.at(entry.first) == entry.second);
  run(); assert(result.ok && !result.error[0]);
  request.kind = BotIoRequest::Get;
  run(); assert(result.ok && result.found && !strcmp(result.value, "remember works"));
  bot[0] = 1; strcpy(request.key, "existing-note");
  run(); assert(result.ok && result.found && !strcmp(result.value, "retain old identity data"));
  const auto after = identity_test::durable;
  const auto kvFileBytes = SPIFFS.usedBytes() - sizeof(endpoint);
  assert(nvs_get_stats(nullptr, &stats) == ESP_OK);
  const auto free = stats.free_entries;
  for (unsigned i = 0; i < 8; ++i) {
    command("commit"); reboot(); expect(endpoint);
    assert(nvs_get_stats(nullptr, &stats) == ESP_OK && stats.free_entries == free);
  }
  assert(identity_test::durable == after && SPIFFS.usedBytes() == kvFileBytes + 9240);
  assert(identity_test::peakEntries <= 630 - 126);
  puts("PASS physical 630-entry NVS: 166 -> 310 free; KV metadata admission permits new-key remember/recall; old-key data retained; 126-entry GC reserve; endpoint and KV files remain bounded");
}
int main() {
  retention(); migration(); migrationFaults(); powerCuts(); corruptionAndRetry(); physicalHeadroom();
  reset();
}
