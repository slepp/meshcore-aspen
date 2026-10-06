// SPDX-License-Identifier: Apache-2.0
#include "BotNetworkConfig.h"
#include <stdio.h>
#include <string.h>
#if ONCHIP_BOT_HTTPS
#include "RoleStorage.h"
#include <SPIFFS.h>
#include <mutex>
#ifdef ARDUINO_ARCH_ESP32
#ifdef ONCHIP_CLOCK_TEST_BUSY
#include <WiFiClientSecure.h>
#else
#include <mbedtls/x509_crt.h>
#endif
#endif
#endif

namespace onchip {
#if ONCHIP_BOT_HTTPS
namespace {
constexpr unsigned Endpoints = 4, Mappings = 8;
constexpr uint32_t Magic = 0x424e4331, Version = 2;
const char *const slots[] = {"/command-bot/network-0", "/command-bot/network-1"};
const char *const scratch = "/command-bot/network-writing";
bool hasRecord() { return SPIFFS.exists(slots[0]) || SPIFFS.exists(slots[1]); }
struct Endpoint {
  char name[BotNameLimit + 1]{}, address[16]{}, host[254]{};
  char path[BotNetworkPathLimit + 1]{}, ca[4097]{}, token[257]{};
  uint16_t port = 0;
  uint8_t methods = 0, operations = 7;
};
struct Mapping {
  char service[BotNameLimit + 1]{}, operation[BotKeyLimit + 1]{};
  char path[BotNetworkPathLimit + 1]{};
};
struct Persistent {
  uint32_t magic = Magic, version = Version, sequence = 0, checksum = 0;
  Endpoint endpoints[Endpoints]{};
  Mapping mappings[Mappings]{};
};
struct State {
  Persistent active{}, staged{}, candidate{};
  bool dirty = false, uncertain = false;
  unsigned slot = 1;
};
std::mutex mutex;
State *state = nullptr;
std::atomic<uint32_t> epoch{1};

uint32_t checksum(const Persistent &data) {
  uint32_t hash = 2166136261u;
  const auto *bytes = reinterpret_cast<const uint8_t *>(&data);
  for (size_t i = offsetof(Persistent, endpoints); i < sizeof(data); ++i)
    hash = (hash ^ bytes[i]) * 16777619u;
  hash = (hash ^ data.sequence) * 16777619u;
  return hash;
}
bool name(const char *value, size_t maximum) {
  if (!value || *value < 'a' || *value > 'z' ||
      strnlen(value, maximum + 1) > maximum) return false;
  for (const char *p = value; *p; ++p)
    if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_')) return false;
  return true;
}
bool path(const char *value) {
  if (!value || value[0] != '/' || value[1] == '/' ||
      strnlen(value, BotNetworkPathLimit + 1) > BotNetworkPathLimit ||
      strstr(value, "..") || strstr(value, "//")) return false;
  for (const char *p = value; *p; ++p)
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
          (*p >= '0' && *p <= '9') || *p == '/' || *p == '-' ||
          *p == '_' || *p == '.')) return false;
  return true;
}
bool valid(const Persistent &data) {
  if (data.magic != Magic || data.version != Version) return false;
  for (unsigned i = 0; i < Endpoints; ++i) {
    const auto &e = data.endpoints[i];
    if (!e.name[0]) continue;
    if (!name(e.name, BotNameLimit) || !path(e.path) ||
        !e.methods || (e.methods & ~3u) ||
        (e.name[0] && !strcmp(e.name, "home") &&
         (strcmp(e.path, "/v1/rpc") || e.methods != 2 ||
          !e.operations || (e.operations & ~7u)))) return false;
    const BotHttpsConfig config{e.address, e.host, e.ca, e.token, e.port, e.operations};
    if (!config.valid()) return false;
#ifdef ARDUINO_ARCH_ESP32
    mbedtls_x509_crt cert;
    mbedtls_x509_crt_init(&cert);
    const bool parsed = mbedtls_x509_crt_parse(&cert,
        reinterpret_cast<const unsigned char *>(e.ca), strlen(e.ca) + 1) == 0;
    mbedtls_x509_crt_free(&cert);
    if (!parsed) return false;
#endif
    for (unsigned j = i + 1; j < Endpoints; ++j)
      if (data.endpoints[j].name[0] &&
          (!name(data.endpoints[j].name, BotNameLimit) ||
           !strcmp(e.name, data.endpoints[j].name))) return false;
  }
  for (unsigned i = 0; i < Mappings; ++i) {
    const auto &m = data.mappings[i];
    if (!m.service[0]) continue;
    if (!name(m.service, BotNameLimit) || !name(m.operation, BotKeyLimit) ||
        !path(m.path)) return false;
    bool service = !strcmp(m.service, "home") && botHttpsConfig().valid();
    for (const auto &e : data.endpoints) service |= !strcmp(e.name, m.service) && e.name[0];
    if (!service) return false;
    for (unsigned j = i + 1; j < Mappings; ++j)
      if (data.mappings[j].service[0] &&
          (!name(data.mappings[j].service, BotNameLimit) ||
           !name(data.mappings[j].operation, BotKeyLimit) ||
           (!strcmp(m.service, data.mappings[j].service) &&
            !strcmp(m.operation, data.mappings[j].operation)))) return false;
  }
  return true;
}
bool load(Persistent &data, unsigned slot, bool &corrupt) {
  if (!SPIFFS.exists(slots[slot])) return false;
  auto file = SPIFFS.open(slots[slot], "r");
  if (file && file.size() == sizeof(data) &&
      file.read(reinterpret_cast<uint8_t *>(&data), sizeof(data)) == sizeof(data) &&
      data.checksum == checksum(data) && valid(data)) return true;
  corrupt = true;
  return false;
}
bool initialize() {
  if (state) return true;
  state = allocateRoleStorage<State>("bot network config");
  if (!state) return false;
  bool corrupt = false;
  const bool first = load(state->active, 0, corrupt);
  const bool second = load(state->candidate, 1, corrupt);
  if (corrupt && !first && !second) {
    releaseRoleStorage(state);
    return false;
  }
  if (second && (!first || int32_t(state->candidate.sequence - state->active.sequence) > 0)) {
    state->active = state->candidate;
    state->slot = 1;
  } else if (first) state->slot = 0;
  else state->active = {};
  state->staged = state->active;
  state->candidate = {};
  return true;
}
Endpoint *endpoint(Persistent &data, const char *alias, bool create) {
  Endpoint *available = nullptr;
  for (auto &entry : data.endpoints) {
    if (!strcmp(entry.name, alias) && entry.name[0]) return &entry;
    if (!entry.name[0] && !available) available = &entry;
  }
  return create ? available : nullptr;
}
enum class StoreOutcome { Rejected, Committed, Unknown };
StoreOutcome store(Persistent &data) {
  const unsigned target = state->slot ^ 1u;
  data.sequence = state->active.sequence + 1;
  data.checksum = checksum(data);
  auto file = SPIFFS.open(scratch, "w");
  if (!file) return StoreOutcome::Rejected;
  const bool written = file.write(reinterpret_cast<const uint8_t *>(&data), sizeof(data)) == sizeof(data);
  file.flush();
  file.close();
  Persistent &check = state->candidate;
  auto verification = SPIFFS.open(scratch, "r");
  const bool verified = written && verification && verification.size() == sizeof(check) &&
      verification.read(reinterpret_cast<uint8_t *>(&check), sizeof(check)) == sizeof(check) &&
      check.checksum == checksum(check) && valid(check);
  verification.close();
  if (!verified) { SPIFFS.remove(scratch); return StoreOutcome::Rejected; }
  // The other slot remains valid even if power fails between remove and rename.
  if (SPIFFS.exists(slots[target]) && !SPIFFS.remove(slots[target])) {
    SPIFFS.remove(scratch); return StoreOutcome::Rejected;
  }
  if (!SPIFFS.rename(scratch, slots[target])) {
    SPIFFS.remove(scratch); return StoreOutcome::Rejected;
  }
  bool corrupt = false;
  if (!load(state->candidate, target, corrupt) || state->candidate.sequence != data.sequence)
    return StoreOutcome::Unknown;
  state->slot = target;
  return StoreOutcome::Committed;
}
bool hexAppend(char *destination, size_t capacity, const char *hex, bool certificate) {
  const size_t length = hex ? strnlen(hex, 129) : 0;
  if (!length || length > 128 || length % 2) return false;
  size_t offset = strnlen(destination, capacity);
  if (offset == capacity || length / 2 > capacity - offset - 1) return false;
  for (size_t i = 0; i < length; i += 2) {
    unsigned byte = 0;
    for (unsigned j = 0; j < 2; ++j) {
      const char c = hex[i + j];
      if (c >= '0' && c <= '9') byte = byte * 16 + unsigned(c - '0');
      else if (c >= 'a' && c <= 'f') byte = byte * 16 + unsigned(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') byte = byte * 16 + unsigned(c - 'A' + 10);
      else return false;
    }
    if (!byte || (!certificate && (byte < 33 || byte > 126)) ||
        (certificate && (byte < 32 && byte != '\n' && byte != '\r'))) return false;
  }
  for (size_t i = 0; i < length; i += 2) {
    unsigned byte = 0;
    for (unsigned j = 0; j < 2; ++j) {
      const char c = hex[i + j];
      byte = byte * 16 + unsigned(c <= '9' ? c - '0' :
                                  (c & ~32) - 'A' + 10);
    }
    destination[offset++] = char(byte);
  }
  destination[offset] = 0;
  return true;
}
void say(char *reply, size_t capacity, const char *text) {
  if (reply && capacity) snprintf(reply, capacity, "%s", text);
}
} // namespace

uint32_t botNetworkEpoch() { return epoch.load(); }
bool botNetworkReload() {
  std::lock_guard<std::mutex> guard(mutex);
  if (epoch == UINT32_MAX) return false;
  ++epoch;
  if (state) {
    volatile uint8_t *bytes = reinterpret_cast<volatile uint8_t *>(state);
    for (size_t i = 0; i < sizeof(*state); ++i) bytes[i] = 0;
  }
  releaseRoleStorage(state);
  return initialize();
}
bool botNetworkConfigured() {
  std::lock_guard<std::mutex> guard(mutex);
  if (!state && !hasRecord()) return false;
  if (!initialize() || state->uncertain) return false;
  for (const auto &e : state->active.endpoints) if (e.name[0]) return true;
  return false;
}
bool botNetworkHomeConfigured() {
  std::lock_guard<std::mutex> guard(mutex);
  if (!state && !hasRecord()) return false;
  if (!initialize()) return true;
  return state->uncertain || endpoint(state->active, "home", false);
}
bool botNetworkResolve(const BotIoRequest &request, BotNetworkRoute &route) {
  std::lock_guard<std::mutex> guard(mutex);
  if (!state && !hasRecord()) return false;
  if (!initialize() || state->uncertain) return false;
  const bool http = request.kind == BotIoRequest::HttpGet ||
                    request.kind == BotIoRequest::HttpPost ||
                    request.kind == BotIoRequest::PackageGet;
  if (request.kind == BotIoRequest::Rpc &&
      (!name(request.key, BotKeyLimit) ||
       (request.endpoint[0] && !name(request.endpoint, BotNameLimit)))) return false;
  const bool builtIn = request.kind == BotIoRequest::Rpc &&
      (!request.endpoint[0] || !strcmp(request.endpoint, "home")) &&
      (!strcmp(request.key, "health") || !strcmp(request.key, "echo") ||
       !strcmp(request.key, "weather"));
  const char *alias = http ? request.key : builtIn ? "home" : request.endpoint;
  if (!name(alias, BotNameLimit)) return false;
  const Endpoint *found = endpoint(state->active, alias, false);
  if (http) {
    if (!found || !(found->methods & (
        request.kind == BotIoRequest::HttpGet || request.kind == BotIoRequest::PackageGet ? 1 : 2)))
      return false;
    snprintf(route.path, sizeof(route.path), "%s", found->path);
    route.post = request.kind != BotIoRequest::HttpGet &&
                 request.kind != BotIoRequest::PackageGet;
  } else if (builtIn && found) {
    snprintf(route.path, sizeof(route.path), "%s", found->path);
    route.post = true;
  } else {
    const Mapping *mapping = nullptr;
    for (const auto &m : state->active.mappings)
      if (m.service[0] && !strcmp(m.service, alias) && !strcmp(m.operation, request.key))
        mapping = &m;
    if (!mapping) return false;
    snprintf(route.path, sizeof(route.path), "%s", mapping->path);
    route.post = true;
  }
  if (!found && strcmp(alias, "home")) return false;
  const BotHttpsConfig config = found ?
      BotHttpsConfig{found->address, found->host, found->ca, found->token, found->port, found->operations} :
      botHttpsConfig();
  if (!config.valid()) return false;
  snprintf(route.address, sizeof(route.address), "%s", config.address);
  snprintf(route.host, sizeof(route.host), "%s", config.host);
  snprintf(route.ca, sizeof(route.ca), "%s", config.ca);
  snprintf(route.token, sizeof(route.token), "%s", config.token);
  route.port = config.port;
  route.operations = config.operations;
  return true;
}
bool botHttpsAdmin(const char *command, char *reply, size_t capacity) {
  say(reply, capacity, "HTTPS configuration rejected");
  if (!reply || !capacity || !command || strnlen(command, 512) >= 512) return false;
  struct Command {
    char bytes[512]{};
    ~Command() {
      volatile char *p = bytes;
      for (size_t i = 0; i < sizeof(bytes); ++i) p[i] = 0;
    }
  } input;
  strcpy(input.bytes, command);
  std::lock_guard<std::mutex> guard(mutex);
  if (!initialize()) { say(reply, capacity, "HTTPS configuration storage unavailable"); return false; }
  char *save = nullptr;
  char *verb = strtok_r(input.bytes, " ", &save);
  char *args[7]{};
  unsigned count = 0;
  while (count < 7 && (args[count] = strtok_r(nullptr, " ", &save))) ++count;
  if (!verb || count == 7) return false;
  auto &stage = state->staged;
  if (!strcmp(verb, "status") && !count) {
    unsigned endpoints = 0, mappings = 0;
    for (const auto &e : state->active.endpoints) endpoints += e.name[0] != 0;
    for (const auto &m : state->active.mappings) mappings += m.service[0] != 0;
    snprintf(reply, capacity, "HTTPS endpoints=%u rpc=%u staged=%u uncertain=%u epoch=%u",
             endpoints, mappings, state->dirty, state->uncertain, epoch.load());
    return true;
  }
  if (state->uncertain) {
    say(reply, capacity, "HTTPS commit outcome unknown; live endpoints blocked until reboot/readback");
    return false;
  }
  if (!strcmp(verb, "discard") && !count) {
    stage = state->active; state->dirty = false;
    say(reply, capacity, "HTTPS staging discarded"); return true;
  }
  if (!strcmp(verb, "commit") && !count) {
    if (!state->dirty || !valid(stage) || epoch == UINT32_MAX ||
        state->active.sequence == UINT32_MAX) {
      say(reply, capacity, "HTTPS configuration validation or commit failed"); return false;
    }
    const auto outcome = store(stage);
    if (outcome == StoreOutcome::Rejected) {
      say(reply, capacity, "HTTPS configuration validation or commit failed"); return false;
    }
    state->active = stage;
    state->dirty = false;
    ++epoch;
    if (outcome == StoreOutcome::Unknown) {
      state->uncertain = true;
      say(reply, capacity, "HTTPS commit outcome unknown; live endpoints blocked until reboot/readback");
      return false;
    }
    say(reply, capacity, "HTTPS configuration verified and committed; previous network requests revoked");
    return true;
  }
  if (!strcmp(verb, "retain") && count == 1 && !strcmp(args[0], "home")) {
    const auto &initial = botHttpsConfig();
    if (!initial.valid() || endpoint(stage, "home", false)) {
      say(reply, capacity, "HTTPS home defaults unavailable or endpoint already staged/saved; inspect bot https status");
      return false;
    }
    Endpoint *e = endpoint(stage, "home", true);
    if (!e) return false;
    *e = {};
    strcpy(e->name, "home");
    strcpy(e->address, initial.address);
    strcpy(e->host, initial.host);
    strcpy(e->ca, initial.ca);
    strcpy(e->token, initial.token);
    strcpy(e->path, "/v1/rpc");
    e->port = initial.port;
    e->methods = 2;
    e->operations = initial.operations;
  } else if (!strcmp(verb, "endpoint") && count == 6) {
    if (!name(args[0], BotNameLimit) ||
        !path(args[4])) return false;
    unsigned port = 0; char extra;
    if (sscanf(args[3], "%u%c", &port, &extra) != 1 || !port || port > 65535) return false;
    const uint8_t methods = !strcmp(args[5], "get") ? 1 :
                            !strcmp(args[5], "post") ? 2 :
                            !strcmp(args[5], "both") ? 3 : 0;
    if (!methods || strnlen(args[1], 16) >= 16 || strnlen(args[2], 254) >= 254) return false;
    if (!strcmp(args[0], "home") && (strcmp(args[4], "/v1/rpc") || methods != 2)) return false;
    Endpoint *e = endpoint(stage, args[0], true);
    if (!e) return false;
    *e = {};
    strcpy(e->name, args[0]); strcpy(e->address, args[1]);
    strcpy(e->host, args[2]); strcpy(e->path, args[4]);
    e->port = uint16_t(port); e->methods = methods;
    e->operations = !strcmp(args[0], "home") ? 0 : 7;
  } else if (!strcmp(verb, "ops") && count == 2) {
    unsigned mask = 0; char extra;
    if (strcmp(args[0], "home") || !endpoint(stage, "home", false) ||
        sscanf(args[1], "%u%c", &mask, &extra) != 1 || !mask || mask > 7) return false;
    endpoint(stage, "home", false)->operations = uint8_t(mask);
  } else if ((!strcmp(verb, "ca") || !strcmp(verb, "token")) && count == 2) {
    if (!name(args[0], BotNameLimit)) return false;
    Endpoint *e = endpoint(stage, args[0], false);
    if (!e || !hexAppend(!strcmp(verb, "ca") ? e->ca : e->token,
                         !strcmp(verb, "ca") ? sizeof(e->ca) : sizeof(e->token),
                         args[1], !strcmp(verb, "ca"))) return false;
  } else if (!strcmp(verb, "rpc") && count == 3) {
    if (!name(args[0], BotNameLimit) || !name(args[1], BotKeyLimit) ||
        !path(args[2]) || (strcmp(args[0], "home") && !endpoint(stage, args[0], false)))
      return false;
    Mapping *available = nullptr;
    for (auto &entry : stage.mappings) {
      if (!strcmp(entry.service, args[0]) && !strcmp(entry.operation, args[1])) {
        available = &entry; break;
      }
      if (!entry.service[0] && !available) available = &entry;
    }
    if (!available) return false;
    *available = {};
    strcpy(available->service, args[0]); strcpy(available->operation, args[1]);
    strcpy(available->path, args[2]);
  } else if (!strcmp(verb, "drop") && count == 1) {
    Endpoint *e = name(args[0], BotNameLimit) ? endpoint(stage, args[0], false) : nullptr;
    if (!e) return false;
    *e = {};
    for (auto &m : stage.mappings) if (!strcmp(m.service, args[0])) m = {};
  } else if (!strcmp(verb, "unmap") && count == 2) {
    bool found = false;
    for (auto &m : stage.mappings)
      if (!strcmp(m.service, args[0]) && !strcmp(m.operation, args[1])) {
        m = {}; found = true; break;
      }
    if (!found) return false;
  } else return false;
  state->dirty = true;
  say(reply, capacity, "HTTPS change staged; commit to verify and activate");
  return true;
}
#else
uint32_t botNetworkEpoch() { return 1; }
bool botNetworkConfigured() { return false; }
bool botNetworkHomeConfigured() { return false; }
bool botNetworkReload() { return false; }
bool botNetworkResolve(const BotIoRequest &, BotNetworkRoute &) { return false; }
bool botHttpsAdmin(const char *, char *reply, size_t capacity) {
  if (reply && capacity) snprintf(reply, capacity, "HTTPS not built into this profile");
  return false;
}
#endif
} // namespace onchip
