// SPDX-License-Identifier: Apache-2.0
#include "ObserverConfig.h"
#include "Config.h"
#include "RoleStorage.h"
#include <SPIFFS.h>
#include <Utils.h>
#include <nvs.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#ifdef ARDUINO_ARCH_ESP32
#include <mbedtls/x509_crt.h>
#endif

namespace onchip {
namespace {
constexpr const char *Slots[] = {"/observer-config-a.bin", "/observer-config-b.bin"};
struct Reference {
  uint8_t magic[4]{'M', 'O', 'C', 1}, slot = 0, reserved[3]{}, digest[32]{};
  bool valid() const {
    return !memcmp(magic, "MOC\1", 4) && slot < 2 &&
           !reserved[0] && !reserved[1] && !reserved[2];
  }
};
struct Store {
  ObserverConfig active, staged, check;
  bool saved = false, dirty = false, fault = false;
};
Store *store = nullptr;
template <size_t N> bool text(const char (&value)[N], bool spaces = false, bool certificate = false) {
  const auto *end = static_cast<const char *>(memchr(value, 0, N));
  if (!end) return false;
  for (const char *p = value; p < end; ++p) {
    const auto c = uint8_t(*p);
    if ((c < (spaces ? 32 : 33) || c > 126) &&
        !(certificate && (c == '\r' || c == '\n'))) return false;
  }
  for (const char *p = end; p < value + N; ++p) if (*p) return false;
  return true;
}
bool copy(char *destination, size_t capacity, const char *source) {
  if (strlen(source) >= capacity) return false;
  memset(destination, 0, capacity);
  strcpy(destination, source);
  return true;
}
int digit(char c) {
  return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
         c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}
bool unescape(char *value) {
  char *output = value;
  for (const char *p = value; *p; ++p) {
    if (*p == '%') {
      if (!p[1] || !p[2] || digit(p[1]) < 0 || digit(p[2]) < 0) return false;
      const int c = digit(p[1]) * 16 + digit(p[2]);
      if (c < 32 || c > 126) return false;
      *output++ = char(c);
      p += 2;
    } else *output++ = *p;
  }
  memset(output, 0, strlen(output) + 1);
  return true;
}
bool defaults(ObserverConfig &config) {
  config = {};
  config.format = ONCHIP_MQTT_FORMAT;
  config.filter = ONCHIP_MQTT_PACKET_FILTER;
  if (!copy(config.uri, sizeof(config.uri), ONCHIP_MQTT_URI) ||
      !copy(config.audience, sizeof(config.audience), ONCHIP_MQTT_AUDIENCE) ||
      !copy(config.prefix, sizeof(config.prefix), ONCHIP_MQTT_TOPIC_PREFIX) ||
      !copy(config.name, sizeof(config.name), ONCHIP_OBSERVER_NAME) ||
      !copy(config.iata, sizeof(config.iata), ONCHIP_MQTT_IATA) ||
      !copy(config.ca, sizeof(config.ca), ONCHIP_MQTT_CA_PEM)) return false;
  char *scheme = strstr(config.uri, "://");
  char *userinfo = scheme ? scheme + 3 : nullptr;
  char *at = userinfo ? strchr(userinfo, '@') : nullptr;
  if (at) {
    char *colon = strchr(userinfo, ':');
    if (!colon || colon > at || size_t(colon - userinfo) >= sizeof(config.username) ||
        size_t(at - colon - 1) >= sizeof(config.password)) return false;
    memcpy(config.username, userinfo, colon - userinfo);
    memcpy(config.password, colon + 1, at - colon - 1);
    if (!unescape(config.username) || !unescape(config.password)) return false;
    const size_t remaining = strlen(at + 1);
    memmove(userinfo, at + 1, remaining + 1);
    memset(userinfo + remaining + 1, 0, sizeof(config.uri) - size_t(userinfo - config.uri) - remaining - 1);
  }
  return config.valid();
}
bool readReference(Reference &reference, bool &present) {
  present = false;
  nvs_handle_t handle;
  auto result = nvs_open("mc-onchip", NVS_READONLY, &handle);
  if (result == ESP_ERR_NVS_NOT_FOUND) return true;
  if (result != ESP_OK) return false;
  size_t size = sizeof(reference);
  result = nvs_get_blob(handle, "observer-config", &reference, &size);
  nvs_close(handle);
  if (result == ESP_ERR_NVS_NOT_FOUND) return true;
  if (result != ESP_OK || size != sizeof(reference) || !reference.valid()) return false;
  present = true;
  return true;
}
void digest(const ObserverConfig &config, uint8_t output[32]) {
  mesh::Utils::sha256(output, 32, reinterpret_cast<const uint8_t *>(&config), sizeof(config));
}
bool readFile(const Reference &reference, ObserverConfig &config) {
  auto file = SPIFFS.open(Slots[reference.slot], "r");
  if (!file) return false;
  const bool complete = file.size() == sizeof(config) &&
      file.read(reinterpret_cast<uint8_t *>(&config), sizeof(config)) == sizeof(config) &&
      file.size() == sizeof(config);
  file.close();
  if (!complete || !config.valid()) return false;
  uint8_t hash[32];
  digest(config, hash);
  return !memcmp(hash, reference.digest, sizeof(hash));
}
bool save(const ObserverConfig &config) {
  Reference previous, next;
  bool present;
  if (!readReference(previous, present)) return false;
  next.slot = present ? 1 - previous.slot : 0;
  digest(config, next.digest);
  auto file = SPIFFS.open(Slots[next.slot], "w");
  if (!file) return false;
  const bool written = file.write(reinterpret_cast<const uint8_t *>(&config), sizeof(config)) == sizeof(config);
  file.flush();
  file.close();
  if (!written || !readFile(next, store->check) || memcmp(&config, &store->check, sizeof(config))) return false;
  nvs_handle_t handle;
  if (nvs_open("mc-onchip", NVS_READWRITE, &handle) != ESP_OK) return false;
  auto result = nvs_set_blob(handle, "observer-config", &next, sizeof(next));
  if (result == ESP_OK) result = nvs_commit(handle);
  nvs_close(handle);
  Reference observed;
  return result == ESP_OK && readReference(observed, present) && present &&
      !memcmp(&next, &observed, sizeof(next)) && readFile(observed, store->check) &&
      !memcmp(&config, &store->check, sizeof(config));
}
bool initialize() {
  if (store) return !store->fault;
  store = allocateRoleStorage<Store>("observer configuration");
  if (!store) return false;
  Reference reference;
  bool present;
  const bool loaded = readReference(reference, present) &&
      (present ? readFile(reference, store->active) : defaults(store->active));
  store->saved = loaded && present;
  store->fault = !loaded;
  if (!loaded) {
    store->active = {};
    Serial.println("Observer configuration read/validation failed; MQTT disabled; repair saved configuration");
  }
  store->staged = store->active;
  return loaded;
}
bool appendHex(char *output, size_t capacity, const char *hex, bool certificate) {
  const size_t length = strlen(hex), offset = strlen(output);
  if (!length || length % 2 || offset + length / 2 >= capacity) return false;
  for (size_t i = 0; i < length; i += 2) {
    const int a = digit(hex[i]), b = digit(hex[i + 1]);
    if (a < 0 || b < 0) return false;
    const int c = a * 16 + b;
    if ((c < 32 || c > 126) && !(certificate && (c == '\r' || c == '\n'))) return false;
  }
  for (size_t i = 0; i < length; i += 2) output[offset + i / 2] = char(digit(hex[i]) * 16 + digit(hex[i + 1]));
  output[offset + length / 2] = 0;
  return true;
}
} // namespace
bool ObserverConfig::valid() const {
  if (version != 1 || format > 2 || reserved || !text(uri) || !text(audience) ||
      !text(prefix) || !text(name, true) || !text(iata) || !text(username, true) ||
      !text(password, true) || !text(ca, true, true) || !name[0] || !prefix[0] ||
      prefix[0] == '/' || prefix[strlen(prefix) - 1] == '/' || strpbrk(prefix, "+#") ||
      (audience[0] && (!format || username[0] || password[0])) ||
      (password[0] && !username[0])) return false;
  if (format) {
    if (strlen(iata) != 3) return false;
    for (char c : iata) if (c && (c < 'A' || c > 'Z')) return false;
  }
  if (!uri[0]) return true;
  const bool secure = !strncmp(uri, "mqtts://", 8) || !strncmp(uri, "wss://", 6);
  if (!secure && strncmp(uri, "mqtt://", 7) && strncmp(uri, "ws://", 5)) return false;
  if (strpbrk(uri, "@#\\") || !strstr(uri, "://")[3] ||
      strstr(uri, "://")[3] == '/' || (secure && !ca[0])) return false;
#ifdef ARDUINO_ARCH_ESP32
  if (ca[0]) {
    mbedtls_x509_crt certificate;
    mbedtls_x509_crt_init(&certificate);
    const bool parsed = mbedtls_x509_crt_parse(&certificate,
        reinterpret_cast<const unsigned char *>(ca), strlen(ca) + 1) == 0;
    mbedtls_x509_crt_free(&certificate);
    if (!parsed) return false;
  }
#endif
  return true;
}
bool loadObserverConfig(ObserverConfig &config) {
  if (!initialize()) return false;
  config = store->active;
  return true;
}
#if defined(ONCHIP_OBSERVER_CONFIG_TEST)
void resetObserverConfigForTest() { releaseRoleStorage(store); }
#endif
void observerConfigCommand(const char *command, char *reply, size_t capacity) {
  const auto error = [&](const char *message) { snprintf(reply, capacity, "Error: %s", message); };
  const bool loaded = initialize();
  if (!store) { error("observer configuration allocation failed"); return; }
  if (!command[0] || !strcmp(command, "status")) {
    snprintf(reply, capacity, "MQTT saved=%u staged=%u storage=%s format=%u iata=%s auth=%s ca-bytes=%u; saved changes apply after reboot",
             store->saved, store->dirty, store->fault ? "error" : "ok",
             store->active.format, store->active.iata,
             store->active.audience[0] ? "jwt" : store->active.username[0] ? "basic" : "none",
             unsigned(strlen(store->active.ca)));
    return;
  }
  if (!strcmp(command, "help")) {
    snprintf(reply, capacity, "mqtt status|uri|name|iata|prefix|audience|format|filter; FIELD VALUE; username|password|ca clear|HEX; commit|discard; reboot to apply");
    return;
  }
  if (!loaded && !strcmp(command, "commit")) {
    error("observer configuration storage invalid; repair saved record before committing"); return;
  }
  if (!strcmp(command, "discard")) {
    store->staged = store->active;
    store->dirty = false;
    snprintf(reply, capacity, "Discarded staged MQTT settings"); return;
  }
  if (!strcmp(command, "commit")) {
    if (!store->staged.valid()) {
      error("invalid MQTT settings: check URI, IATA, format, credentials, audience and TLS CA"); return;
    }
    if (!save(store->staged)) {
      store->fault = true;
      store->saved = false;
      store->check = {};
      Serial.println("Observer configuration commit/readback failed; saved outcome unknown; reboot and inspect mqtt status");
      error("MQTT commit/readback failed; saved outcome unknown; reboot and inspect mqtt status"); return;
    }
    store->active = store->staged;
    store->dirty = false;
    store->saved = true;
    store->check = {};
    snprintf(reply, capacity, "Saved MQTT settings; reboot to apply; observer identity unchanged"); return;
  }
  struct Field { const char *name; char *value; const char *active; size_t capacity; };
  Field fields[] = {{"uri", store->staged.uri, store->active.uri, sizeof(store->staged.uri)},
                   {"name", store->staged.name, store->active.name, sizeof(store->staged.name)},
                   {"iata", store->staged.iata, store->active.iata, sizeof(store->staged.iata)},
                   {"prefix", store->staged.prefix, store->active.prefix, sizeof(store->staged.prefix)},
                   {"audience", store->staged.audience, store->active.audience, sizeof(store->staged.audience)}};
  for (const auto &field : fields) {
    const size_t length = strlen(field.name);
    if (!strcmp(command, field.name)) {
      if (strlen(field.active) >= capacity) {
        error("saved MQTT field exceeds reply capacity; use mqtt status"); return;
      }
      snprintf(reply, capacity, "%s", field.active[0] ? field.active : "(empty)");
      return;
    }
    if (!strncmp(command, field.name, length) && command[length] == ' ') {
      const char *value = command + length + 1;
      if (!strcmp(value, "-")) value = "";
      if (!copy(field.value, field.capacity, value)) { error("MQTT field exceeds saved setting capacity"); return; }
      store->dirty = true;
      snprintf(reply, capacity, "Staged MQTT field; mqtt commit then reboot to apply"); return;
    }
  }
  if (!strcmp(command, "format") || !strcmp(command, "filter")) {
    snprintf(reply, capacity, "%u", !strcmp(command, "format") ? store->active.format : store->active.filter); return;
  }
  if (!strncmp(command, "format ", 7) || !strncmp(command, "filter ", 7)) {
    const char *value = command + 7;
    const unsigned maximum = command[1] == 'o' ? 2 : 65535;
    if (!*value || strspn(value, "0123456789") != strlen(value)) {
      error("MQTT format requires 0..2; filter requires decimal 0..65535"); return;
    }
    char *end = nullptr;
    const unsigned long number = strtoul(value, &end, 10);
    if (*end || number > maximum) { error("MQTT format requires 0..2; filter requires decimal 0..65535"); return; }
    if (maximum == 2) store->staged.format = uint8_t(number);
    else store->staged.filter = uint16_t(number);
  } else {
    char *target = nullptr;
    size_t size = 0, skip = 0;
    bool certificate = false;
    if (!strncmp(command, "ca ", 3)) { target = store->staged.ca; size = sizeof(store->staged.ca); skip = 3; certificate = true; }
    if (!strncmp(command, "username ", 9)) { target = store->staged.username; size = sizeof(store->staged.username); skip = 9; }
    if (!strncmp(command, "password ", 9)) { target = store->staged.password; size = sizeof(store->staged.password); skip = 9; }
    if (!target) { error("unknown MQTT command; use mqtt help"); return; }
    if (!strcmp(command + skip, "clear")) memset(target, 0, size);
    else if (!appendHex(target, size, command + skip, certificate)) {
      error("MQTT credential/CA needs bounded printable HEX; use clear before replacing"); return;
    }
  }
  store->dirty = true;
  snprintf(reply, capacity, "Staged MQTT field; mqtt commit then reboot to apply");
}
} // namespace onchip
