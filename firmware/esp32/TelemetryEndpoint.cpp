// SPDX-License-Identifier: Apache-2.0
#include "TelemetryEndpoint.h"
#if ONCHIP_BOT_HTTPS
#include "RoleStorage.h"
#include <SPIFFS.h>
#include <Utils.h>
#include <nvs.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace onchip {
bool TelemetryEndpoint::valid() const {
  if (version != 1 || !memchr(address, 0, sizeof(address)) ||
      !memchr(host, 0, sizeof(host)) || !memchr(path, 0, sizeof(path)) ||
      !memchr(ca, 0, sizeof(ca)) || !memchr(token, 0, sizeof(token)) ||
      !https().validPeer() || path[0] != '/' || path[1] == '/' || strchr(path, '#')) return false;
  for (const unsigned char *p = reinterpret_cast<const unsigned char *>(path); *p; ++p)
    if (*p < 33 || *p > 126 || *p == '\\') return false;
  return true;
}
namespace {
constexpr const char *Slots[] = {"/telemetry-a.bin", "/telemetry-b.bin"};
struct Reference {
  uint8_t magic[4]{'T', 'E', 'P', 2}, slot = 0, reserved[3]{}, digest[32]{};
  bool valid() const {
    return !memcmp(magic, "TEP\2", 4) && slot < 2 &&
           !reserved[0] && !reserved[1] && !reserved[2];
  }
};
static_assert(sizeof(TelemetryEndpoint) == 4620 && sizeof(Reference) == 40,
              "Recheck endpoint migration and NVS entry budget");
struct Store {
  TelemetryEndpoint active, staged, check;
  bool usable = false, dirty = false, fault = false;
};
Store *store = nullptr;
bool readReference(Reference &reference, TelemetryEndpoint &endpoint, bool &present, bool &legacy) {
  present = legacy = false;
  nvs_handle_t handle;
  auto result = nvs_open("mc-onchip", NVS_READONLY, &handle);
  if (result == ESP_ERR_NVS_NOT_FOUND) return true;
  if (result != ESP_OK) return false;
  size_t size = 0;
  result = nvs_get_blob(handle, "telemetry-peer", nullptr, &size);
  if (result == ESP_ERR_NVS_NOT_FOUND) { nvs_close(handle); return true; }
  if (result != ESP_OK || (size != sizeof(endpoint) && size != sizeof(reference))) {
    nvs_close(handle); return false;
  }
  legacy = size == sizeof(endpoint);
  const size_t expected = size;
  result = nvs_get_blob(handle, "telemetry-peer",
                        legacy ? static_cast<void *>(&endpoint) : &reference, &size);
  nvs_close(handle);
  if (result != ESP_OK || size != expected || (!legacy && !reference.valid())) return false;
  present = true;
  return true;
}
void digest(const TelemetryEndpoint &endpoint, uint8_t output[32]) {
  mesh::Utils::sha256(output, 32, reinterpret_cast<const uint8_t *>(&endpoint), sizeof(endpoint));
}
bool readFile(const Reference &reference, TelemetryEndpoint &endpoint) {
  auto file = SPIFFS.open(Slots[reference.slot], "r");
  if (!file) return false;
  const bool complete = file.size() == sizeof(endpoint) &&
      file.read(reinterpret_cast<uint8_t *>(&endpoint), sizeof(endpoint)) == sizeof(endpoint) &&
      file.size() == sizeof(endpoint);
  file.close();
  if (!complete || !endpoint.valid()) return false;
  uint8_t actual[32];
  digest(endpoint, actual);
  return !memcmp(actual, reference.digest, sizeof(actual));
}
bool save(const TelemetryEndpoint &endpoint, TelemetryEndpoint &check) {
  Reference previous, next;
  bool present, legacy;
  // Re-read authority after an uncertain commit before choosing the inactive slot.
  if (!readReference(previous, check, present, legacy)) return false;
  next.slot = present && !legacy ? 1 - previous.slot : 0;
  digest(endpoint, next.digest);
  auto file = SPIFFS.open(Slots[next.slot], "w");
  if (!file) return false;
  const bool complete = file.write(reinterpret_cast<const uint8_t *>(&endpoint), sizeof(endpoint)) ==
                        sizeof(endpoint);
  file.flush();
  file.close();
  if (!complete || !readFile(next, check) || memcmp(&endpoint, &check, sizeof(endpoint))) return false;

  // Like source deployment, publish only a small NVS reference to a verified file.
  // Replacing this same key reclaims the legacy blob without a second large write.
  nvs_handle_t handle;
  if (nvs_open("mc-onchip", NVS_READWRITE, &handle) != ESP_OK) return false;
  auto result = nvs_set_blob(handle, "telemetry-peer", &next, sizeof(next));
  if (result == ESP_OK) result = nvs_commit(handle);
  nvs_close(handle);
  Reference observed;
  return result == ESP_OK && readReference(observed, check, present, legacy) && present && !legacy &&
      !memcmp(&next, &observed, sizeof(next)) && readFile(observed, check) &&
      !memcmp(&endpoint, &check, sizeof(endpoint));
}
bool initialize() {
  if (store) return true;
  store = allocateRoleStorage<Store>("telemetry endpoint");
  if (!store) return false;
  Reference reference;
  bool present, legacy;
  bool loaded = readReference(reference, store->active, present, legacy);
  if (loaded && present) {
    loaded = legacy ? store->active.valid() && save(store->active, store->check) :
                      readFile(reference, store->active);
    if (loaded && legacy) Serial.println("Telemetry endpoint migrated to SPIFFS; legacy NVS blob reclaimed");
  }
  store->usable = loaded && present;
  store->fault = !loaded;
  store->check = {};
  if (!loaded) {
    Serial.println("Telemetry endpoint storage/migration failed; publishing unavailable");
    store->active = {};
    store->usable = false;
  }
  store->staged = store->active;
  return true;
}
bool appendHex(char *out, size_t capacity, const char *hex, bool certificate) {
  size_t n = strlen(hex), offset = strlen(out);
  if (!n || n > 128 || n % 2 || offset + n / 2 >= capacity) return false;
  const auto digit = [](char c) { return c >= '0' && c <= '9' ? c - '0' :
      c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
  for (size_t i = 0; i < n; i += 2) {
    const int a = digit(hex[i]), b = digit(hex[i + 1]);
    if (a < 0 || b < 0) return false;
    const int c = a * 16 + b;
    if (c < 33 || c > 126) {
      if (!certificate || (c != '\n' && c != '\r' && c != ' ')) return false;
    }
  }
  for (size_t i = 0; i < n; i += 2) out[offset++] = char(digit(hex[i]) * 16 + digit(hex[i + 1]));
  out[offset] = 0;
  return true;
}
}
bool telemetryEndpoint(TelemetryEndpoint &destination) {
  if (!initialize() || !store->usable) return false;
  destination = store->active;
  return true;
}
bool telemetryEndpointConfigured() { return initialize() && store->usable; }
#if defined(ONCHIP_TELEMETRY_TEST)
void resetTelemetryEndpointForTest() { releaseRoleStorage(store); }
#endif
void telemetryEndpointCommand(const char *command, char *reply, size_t capacity) {
  const auto error = [&](const char *text) { snprintf(reply, capacity, "Error: %s", text); };
  if (!initialize()) { error("telemetry endpoint storage unavailable"); return; }
  if (!strcmp(command, "") || !strcmp(command, "status")) {
    snprintf(reply, capacity, "Telemetry endpoint configured=%u staged=%u ca-bytes=%u auth=%u storage=%s; host/path read separately",
             store->usable, store->dirty, unsigned(strlen(store->active.ca)), bool(store->active.token[0]),
             store->fault ? "error" : "ok");
    return;
  }
  if (!strcmp(command, "host") || !strcmp(command, "path") || !strcmp(command, "address")) {
    const auto &a = store->active;
    snprintf(reply, capacity, "%s", !strcmp(command, "host") ? a.host :
             !strcmp(command, "path") ? a.path : a.address);
    return;
  }
  if (!strcmp(command, "port")) {
    snprintf(reply, capacity, "%u", store->active.port);
    return;
  }
  if (!strcmp(command, "discard")) {
    store->staged = store->active;
    store->dirty = false;
    snprintf(reply, capacity, "Discarded staged telemetry endpoint"); return;
  }
  if (!strcmp(command, "commit")) {
    if (!store->staged.valid()) { error("complete HTTPS address/host/path/port/CA required; token optional"); return; }
    const bool verified = save(store->staged, store->check);
    store->check = {};
    if (!verified) {
      store->usable = false;
      store->fault = true;
      Serial.println("Telemetry endpoint commit/readback failed; saved outcome unknown");
      error("endpoint commit/readback failed; live endpoint disabled, saved outcome unknown"); return;
    }
    store->active = store->staged;
    store->usable = true;
    store->fault = false;
    store->dirty = false;
    snprintf(reply, capacity, "Saved telemetry endpoint; credentials are native-only"); return;
  }
  if (!strcmp(command, "ca clear") || !strcmp(command, "token clear")) {
    if (command[0] == 'c') memset(store->staged.ca, 0, sizeof(store->staged.ca));
    else memset(store->staged.token, 0, sizeof(store->staged.token));
  } else if (!strncmp(command, "ca ", 3) || !strncmp(command, "token ", 6)) {
    const bool ca = command[0] == 'c';
    if (!appendHex(ca ? store->staged.ca : store->staged.token,
                   ca ? sizeof(store->staged.ca) : sizeof(store->staged.token),
                   command + (ca ? 3 : 6), ca)) {
      error("bounded printable HEX required; CA max4096, token max256 bytes"); return;
    }
  } else if (!strncmp(command, "port ", 5)) {
    const char *p = command + 5;
    uint32_t port = 0;
    if (!*p) { error("HTTPS port must be 1..65535"); return; }
    for (; *p; ++p) {
      if (*p < '0' || *p > '9' || port > 65535) { error("HTTPS port must be 1..65535"); return; }
      port = port * 10 + unsigned(*p - '0');
    }
    if (!port || port > 65535) { error("HTTPS port must be 1..65535"); return; }
    store->staged.port = uint16_t(port);
  } else {
    char *target = nullptr;
    size_t size = 0, prefix = 0;
    if (!strncmp(command, "host ", 5)) { target = store->staged.host; size = sizeof(store->staged.host); prefix = 5; }
    if (!strncmp(command, "path ", 5)) { target = store->staged.path; size = sizeof(store->staged.path); prefix = 5; }
    if (!strncmp(command, "address ", 8)) { target = store->staged.address; size = sizeof(store->staged.address); prefix = 8; }
    if (!target || !command[prefix] || strlen(command + prefix) >= size) {
      error("endpoint status|host|path|address|port VALUE; ca|token clear|HEX; commit|discard"); return;
    }
    memset(target, 0, size);
    strcpy(target, command + prefix);
  }
  store->dirty = true;
  snprintf(reply, capacity, "Staged telemetry endpoint; commit to apply");
}
} // namespace onchip
#endif
