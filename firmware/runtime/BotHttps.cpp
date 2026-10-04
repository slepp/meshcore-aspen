// SPDX-License-Identifier: Apache-2.0
#include "BotHttps.h"
#include "BotHttpsMetrics.h"
#include "BotNetworkConfig.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <algorithm>
#include <cmath>
#include <new>
#if ONCHIP_BOT_HTTPS
#include "RoleStorage.h"
#include <SHA256.h>
#include <cJSON.h>
#ifdef ONCHIP_BOT_HOME_CONFIG_HEADER
#include ONCHIP_BOT_HOME_CONFIG_HEADER
#endif
#ifdef ARDUINO_ARCH_ESP32
#include "Clock.h"
#include <WiFiClientSecure.h>
#include <mbedtls/pem.h>
#include <mbedtls/ssl_internal.h>
#if ONCHIP_TLS_PSRAM
#include <mbedtls/platform.h>
#endif
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#ifndef ONCHIP_CLOCK_TEST_BUSY
#include <esp_system.h>
#endif
#endif
#endif

#ifndef ONCHIP_BOT_HOME_ADDRESS
#define ONCHIP_BOT_HOME_ADDRESS ""
#endif
#ifndef ONCHIP_BOT_HOME_HOST
#define ONCHIP_BOT_HOME_HOST ""
#endif
#ifndef ONCHIP_BOT_HOME_CA
#define ONCHIP_BOT_HOME_CA ""
#endif
#ifndef ONCHIP_BOT_HOME_TOKEN
#define ONCHIP_BOT_HOME_TOKEN ""
#endif
#ifndef ONCHIP_BOT_HOME_PORT
#define ONCHIP_BOT_HOME_PORT 443
#endif
#ifndef ONCHIP_BOT_HOME_OPERATIONS
#define ONCHIP_BOT_HOME_OPERATIONS 0
#endif

namespace onchip {
namespace {
void failure(BotIoResult &result, const char *code, const char *message) {
  result.ok = false;
  result.json[0] = 0;
  snprintf(result.rpcCode, sizeof(result.rpcCode), "%s", code);
  snprintf(result.error, sizeof(result.error), "%s", message);
}
bool bounded(const char *text, size_t maximum) {
  return text && strnlen(text, maximum + 1) <= maximum;
}
bool ipv4(const char *address) {
  if (!bounded(address, 15)) return false;
  unsigned octets[4]{};
  const char *p = address;
  for (unsigned i = 0; i < 4; ++i) {
    unsigned digits = 0;
    while (*p >= '0' && *p <= '9') {
      if (++digits > 3) return false;
      octets[i] = octets[i] * 10 + unsigned(*p++ - '0');
    }
    if (!digits || octets[i] > 255 || (i < 3 ? *p++ != '.' : *p != 0)) return false;
  }
  return octets[0] != 0 && octets[0] < 224 && octets[3] != 255;
}
}
bool BotHttpsConfig::validPeer() const {
  if (!ipv4(address) || !port ||
      !bounded(host, 253) || !host[0] || !bounded(token, 256) ||
      !bounded(ca, 4096) || strncmp(ca, "-----BEGIN CERTIFICATE-----", 27) ||
      !strstr(ca, "-----END CERTIFICATE-----")) return false;
  for (const char *p = host; *p; ++p)
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
          (*p >= '0' && *p <= '9') || *p == '.' || *p == '-')) return false;
  for (const unsigned char *p = reinterpret_cast<const unsigned char *>(token); *p; ++p)
    if (*p < 33 || *p > 126) return false;
  return true;
}
bool BotHttpsConfig::valid() const {
  return validPeer() && operations && !(operations & ~7u) && strlen(token) >= 32;
}
const BotHttpsConfig &botHttpsConfig() {
  static const BotHttpsConfig config{ONCHIP_BOT_HOME_ADDRESS, ONCHIP_BOT_HOME_HOST,
      ONCHIP_BOT_HOME_CA, ONCHIP_BOT_HOME_TOKEN, ONCHIP_BOT_HOME_PORT, ONCHIP_BOT_HOME_OPERATIONS};
  return config;
}
bool botHttpsConfigured() {
  return ONCHIP_BOT_HTTPS && (botHttpsConfig().valid() || botNetworkConfigured());
}
bool botHttpsClockTrusted() {
#if ONCHIP_BOT_HTTPS && defined(ARDUINO_ARCH_ESP32)
  uint32_t earliest, latest;
  return trustedNetworkTime(earliest, latest);
#elif ONCHIP_BOT_HTTPS && defined(ONCHIP_BOT_NATIVE_HTTPS)
  extern bool nativeBotHttpsClockTrusted();
  return nativeBotHttpsClockTrusted();
#else
  return false;
#endif
}

#if ONCHIP_BOT_HTTPS
namespace {
bool text(const char *s, size_t maximum, bool empty = false) {
  if (!bounded(s, maximum) || (!empty && !*s)) return false;
  const auto *p = reinterpret_cast<const unsigned char *>(s);
  while (*p) {
    uint32_t code = *p++;
    if (code < 128) { if (code < 32 || code == 127) return false; continue; }
    const unsigned count = code >= 0xc2 && code <= 0xdf ? 1 :
                           code >= 0xe0 && code <= 0xef ? 2 :
                           code >= 0xf0 && code <= 0xf4 ? 3 : 0;
    if (!count) return false;
    code &= (1u << (6 - count)) - 1;
    for (unsigned i = 0; i < count; ++i) {
      if (*p < 0x80 || *p > 0xbf) return false;
      code = (code << 6) | (*p++ & 63);
    }
    if (code < (count == 1 ? 0x80u : count == 2 ? 0x800u : 0x10000u) ||
        code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff) || code <= 0x9f) return false;
  }
  return true;
}
bool utcTimestamp(const char *s) {
  if (strlen(s) != 20 || s[4] != '-' || s[7] != '-' || s[10] != 'T' ||
      s[13] != ':' || s[16] != ':' || s[19] != 'Z') return false;
  const unsigned starts[] = {0, 5, 8, 11, 14, 17}, widths[] = {4, 2, 2, 2, 2, 2};
  unsigned values[6]{};
  for (unsigned i = 0; i < 6; ++i) for (unsigned j = 0; j < widths[i]; ++j) {
    const char c = s[starts[i] + j];
    if (c < '0' || c > '9') return false;
    values[i] = values[i] * 10 + unsigned(c - '0');
  }
  const unsigned days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (values[0] < 1970 || values[1] < 1 || values[1] > 12 || values[2] < 1 ||
      values[3] > 23 || values[4] > 59 || values[5] > 59) return false;
  const bool leap = values[0] % 4 == 0 && (values[0] % 100 != 0 || values[0] % 400 == 0);
  return values[2] <= days[values[1] - 1] + (values[1] == 2 && leap ? 1u : 0u);
}
bool shape(const cJSON *object, const char *const names[], unsigned count) {
  if (!cJSON_IsObject(object)) return false;
  unsigned seen = 0;
  for (const cJSON *item = object->child; item; item = item->next) {
    unsigned field = 0;
    while (field < count && strcmp(item->string, names[field])) ++field;
    if (field == count || (seen & (1u << field))) return false;
    seen |= 1u << field;
  }
  return seen == (1u << count) - 1;
}
const cJSON *field(const cJSON *object, const char *name) {
  return cJSON_GetObjectItemCaseSensitive(object, name);
}
bool stringField(const cJSON *object, const char *name, char *out, size_t capacity) {
  const auto *value = field(object, name);
  if (!cJSON_IsString(value) || !text(value->valuestring, capacity - 1)) return false;
  strcpy(out, value->valuestring);
  return true;
}
bool numberField(const cJSON *object, const char *name, double minimum, double maximum,
                 double &out, bool integer = false) {
  const auto *value = field(object, name);
  if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble) ||
      value->valuedouble < minimum || value->valuedouble > maximum ||
      (integer && std::floor(value->valuedouble) != value->valuedouble)) return false;
  out = value->valuedouble;
  return true;
}
bool jsonBudget(const char *body, size_t size) {
  if (!size || size > BotHttpsBodyLimit || memchr(body, 0, size)) return false;
  bool quoted = false;
  unsigned depth = 0, parts = 0;
  for (size_t i = 0; i < size; ++i) {
    const unsigned char c = body[i];
    if (quoted) {
      if (c < 32) return false;
      if (c == '\\') {
        if (++i == size) return false;
        if (body[i] == 'u' && i + 4 < size && !memcmp(body + i + 1, "0000", 4)) return false;
      } else if (c == '"') quoted = false;
    } else if (c == '-' || (c >= '0' && c <= '9')) {
      size_t end = i;
      if (body[end] == '-' && ++end == size) return false;
      if (body[end] == '0') ++end;
      else {
        if (body[end] < '1' || body[end] > '9') return false;
        while (end < size && body[end] >= '0' && body[end] <= '9') ++end;
      }
      if (end < size && body[end] == '.') {
        const size_t start = ++end;
        while (end < size && body[end] >= '0' && body[end] <= '9') ++end;
        if (end == start) return false;
      }
      if (end < size && (body[end] == 'e' || body[end] == 'E')) {
        ++end;
        if (end < size && (body[end] == '+' || body[end] == '-')) ++end;
        const size_t start = end;
        while (end < size && body[end] >= '0' && body[end] <= '9') ++end;
        if (end == start) return false;
      }
      if (end < size && !strchr(" \t\r\n,}]", body[end])) return false;
      i = end - 1;
    } else if (c < 32 && c != '\t' && c != '\n' && c != '\r') return false;
    else if (c == '"') quoted = true;
    else if (c == '{' || c == '[') {
      if (++depth > 4 || ++parts > 48) return false;
    } else if (c == '}' || c == ']') {
      if (!depth) return false;
      --depth;
    } else if ((c == ',' || c == ':') && ++parts > 48) return false;
  }
  return !quoted && !depth;
}
void requestId(const BotIoRequest &request, uint64_t nonce, char id[65]) {
  if (nonce)
    snprintf(id, 65, "%016llx-%u-%u-%u", static_cast<unsigned long long>(nonce),
             unsigned(request.token.generation), unsigned(request.token.job), unsigned(request.token.operation));
  else
    snprintf(id, 65, "%u-%u-%u", unsigned(request.token.generation),
             unsigned(request.token.job), unsigned(request.token.operation));
}
bool encode(const BotIoRequest &request, char *body, size_t capacity, uint8_t &operation,
            uint64_t nonce) {
  operation = !strcmp(request.key, "health") ? BotHttpsConfig::Health :
              !strcmp(request.key, "echo") ? BotHttpsConfig::Echo :
              !strcmp(request.key, "weather") ? BotHttpsConfig::Weather : 0;
  if (!operation || (operation == BotHttpsConfig::Health ? request.value[0] != 0 :
      !text(request.value, operation == BotHttpsConfig::Weather ? 80 : BotValueLimit))) return false;
  if (operation == BotHttpsConfig::Weather &&
      (request.value[0] == ' ' || request.value[strlen(request.value) - 1] == ' ')) return false;
  char escaped[2 * BotValueLimit + 1];
  size_t n = 0;
  for (const char *p = request.value; *p; ++p) {
    if (*p == '"' || *p == '\\') escaped[n++] = '\\';
    escaped[n++] = *p;
  }
  escaped[n] = 0;
  char id[65];
  requestId(request, nonce, id);
  const int length = snprintf(body, capacity,
      "{\"operation\":\"%s\",\"request_id\":\"%s\",\"args\":{%s%s%s%s%s}}",
      request.key, id,
      operation == BotHttpsConfig::Health ? "" : "\"",
      operation == BotHttpsConfig::Health ? "" : operation == BotHttpsConfig::Echo ? "text" : "place",
      operation == BotHttpsConfig::Health ? "" : "\":\"",
      operation == BotHttpsConfig::Health ? "" : escaped,
      operation == BotHttpsConfig::Health ? "" : "\"");
  return length > 0 && size_t(length) < capacity;
}
bool jsonValue(const char *data, size_t size, bool object = false) {
  if (!jsonBudget(data, size)) return false;
  const char *end = nullptr;
  cJSON *root = cJSON_ParseWithLengthOpts(data, size, &end, false);
  if (!root) return false;
  while (end < data + size && (*end == ' ' || *end == '\t' ||
                                *end == '\r' || *end == '\n')) ++end;
  const bool valid = end == data + size && (!object || cJSON_IsObject(root));
  cJSON_Delete(root);
  return valid;
}
bool containsCredential(const cJSON *item, const char *credential) {
  if (!item) return false;
  if ((item->string && strstr(item->string, credential)) ||
      (cJSON_IsString(item) && item->valuestring && strstr(item->valuestring, credential)))
    return true;
  for (auto *child = item->child; child; child = child->next)
    if (containsCredential(child, credential)) return true;
  return false;
}
bool credentialInResponse(const char *body, size_t size, const char *credential) {
  if (!credential[0]) return false;
  const char *end = nullptr;
  cJSON *root = cJSON_ParseWithLengthOpts(body, size, &end, false);
  if (!root) return false;
  const bool found = containsCredential(root, credential);
  cJSON_Delete(root);
  return found;
}
bool legacy(const BotIoRequest &request) {
  return request.kind == BotIoRequest::Rpc &&
      (!request.endpoint[0] || !strcmp(request.endpoint, "home")) &&
      (!strcmp(request.key, "health") || !strcmp(request.key, "echo") ||
       !strcmp(request.key, "weather"));
}
bool generalBody(const BotIoRequest &request, char *body, size_t capacity, uint64_t nonce) {
  if (request.kind == BotIoRequest::HttpGet) return !request.json[0];
  if (request.kind == BotIoRequest::HttpPost) {
    const size_t size = strnlen(request.json, sizeof(request.json));
    if (!size || size > BotNetworkPayloadLimit || !jsonValue(request.json, size)) return false;
    memcpy(body, request.json, size + 1);
    return true;
  }
  const char *args = request.json[0] ? request.json : "{}";
  if (!jsonValue(args, strlen(args), true)) return false;
  char id[65];
  requestId(request, nonce, id);
  const int length = snprintf(body, capacity,
      "{\"operation\":\"%s\",\"request_id\":\"%s\",\"args\":%s}",
      request.key, id, args);
  return length > 0 && size_t(length) < capacity;
}
}
struct BotHttps::Workspace {
  char body[BotHttpsBodyLimit + 1]{}, request[1024]{}, line[512]{};
};
BotHttps::~BotHttps() { releaseRoleStorage(workspace_); }
bool BotHttps::decode(const BotIoRequest &request, const char *body, size_t size, BotIoResult &result) {
  const auto bad = [&]() {
    failure(result, "invalid_response", "Invalid bounded home RPC response"); return false;
  };
  if (!jsonBudget(body, size)) return bad();
  const char *end = nullptr;
  cJSON *root = cJSON_ParseWithLengthOpts(body, size, &end, false);
  if (!root) return bad();
  while (end < body + size && (*end == ' ' || *end == '\r' || *end == '\n' || *end == '\t')) ++end;
  bool valid = end == body + size;
  if (request.kind == BotIoRequest::HttpGet || request.kind == BotIoRequest::HttpPost) {
    valid = valid && result.httpStatus >= 200 && result.httpStatus < 300;
    if (valid) memcpy(result.json, body, size + 1);
    cJSON_Delete(root);
    if (!valid) {
      failure(result, result.httpStatus >= 400 ? "http_error" : "invalid_response",
              result.httpStatus >= 400 ? "Configured HTTPS endpoint returned an error" :
                                          "Invalid bounded HTTPS JSON response");
    } else result.ok = true;
    return valid;
  }
  if (!legacy(request)) {
    const char *success[] = {"ok", "result"}, *failed[] = {"ok", "error"};
    const auto *value = field(root, "result");
    if (valid && result.httpStatus >= 400 && result.httpStatus <= 599 &&
        cJSON_IsFalse(field(root, "ok")) && shape(root, failed, 2)) {
      const auto *error = field(root, "error");
      const char *names[] = {"code", "message"};
      char ignored[128]{};
      valid = shape(error, names, 2) &&
              stringField(error, "code", result.rpcCode, sizeof(result.rpcCode)) &&
              stringField(error, "message", ignored, sizeof(ignored));
      if (valid) for (const char *p = result.rpcCode; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || *p == '_')) valid = false;
      cJSON_Delete(root);
      if (!valid) return bad();
      result.ok = false;
      strcpy(result.error, "Configured RPC rejected");
      return true;
    }
    valid = valid && result.httpStatus == 200 && shape(root, success, 2) &&
            cJSON_IsTrue(field(root, "ok")) && value;
    if (valid) {
      char *serialized = cJSON_PrintUnformatted(value);
      valid = serialized && strlen(serialized) <= BotNetworkResponseLimit;
      if (valid) strcpy(result.json, serialized);
      cJSON_free(serialized);
    }
    cJSON_Delete(root);
    if (!valid) failure(result, result.httpStatus >= 400 ? "remote_error" : "invalid_response",
                        result.httpStatus >= 400 ? "Configured RPC rejected" :
                                                   "Invalid bounded RPC result");
    else result.ok = true;
    return valid;
  }
  const auto *ok = field(root, "ok");
  const char *success[] = {"ok", "result"}, *failed[] = {"ok", "error"};
  if (valid && cJSON_IsFalse(ok) && result.httpStatus >= 400 && result.httpStatus <= 599 &&
      shape(root, failed, 2)) {
    const auto *error = field(root, "error");
    const char *names[] = {"code", "message"};
    valid = shape(error, names, 2) &&
            stringField(error, "code", result.rpcCode, sizeof(result.rpcCode)) &&
            stringField(error, "message", result.error, sizeof(result.error));
    if (valid) for (const char *p = result.rpcCode; *p; ++p)
      if (!((*p >= 'a' && *p <= 'z') || *p == '_')) valid = false;
    cJSON_Delete(root);
    if (!valid) return bad();
    result.ok = false;
    return true;
  }
  valid = valid && cJSON_IsTrue(ok) && result.httpStatus == 200 && shape(root, success, 2);
  const auto *value = field(root, "result");
  if (valid && !strcmp(request.key, "health")) {
    const char *names[] = {"status"};
    valid = shape(value, names, 1) && stringField(value, "status", result.value, sizeof(result.value)) &&
            !strcmp(result.value, "ok");
  } else if (valid && !strcmp(request.key, "echo")) {
    const char *names[] = {"text"};
    valid = shape(value, names, 1) && stringField(value, "text", result.value, sizeof(result.value)) &&
            !strcmp(result.value, request.value);
  } else if (valid && !strcmp(request.key, "weather")) {
    const char *names[] = {"source", "location", "country", "temperature_c", "weather_code",
                          "observed_at", "source_age_seconds"};
    double code = 0, age = 0;
    valid = shape(value, names, 7) &&
        stringField(value, "source", result.source, sizeof(result.source)) &&
        !strcmp(result.source, "open-meteo") &&
        stringField(value, "location", result.value, 81) &&
        stringField(value, "country", result.country, sizeof(result.country)) &&
        stringField(value, "observed_at", result.observedAt, sizeof(result.observedAt)) &&
        utcTimestamp(result.observedAt) &&
        numberField(value, "temperature_c", -150, 100, result.temperatureC) &&
        numberField(value, "weather_code", 0, 99, code, true) &&
        numberField(value, "source_age_seconds", 0, 7200, age, true);
    result.weatherCode = uint8_t(code); result.sourceAgeSeconds = uint32_t(age);
  } else valid = false;
  cJSON_Delete(root);
  if (!valid) return bad();
  result.ok = true;
  return true;
}
bool BotHttps::rate(const uint8_t principal[32]) {
  const auto now = transport_.now();
  if (uint32_t(now - window_) >= 60000) { window_ = now; count_ = 0; }
  if (count_ >= 4) return false;
  Rate *slot = nullptr;
  for (auto &entry : rates_) {
    if (entry.used && uint32_t(now - entry.at) >= 60000) entry = {};
    if (entry.used && !memcmp(entry.principal, principal, 32)) { slot = &entry; break; }
    if (!entry.used) slot = &entry;
  }
  if (!slot || slot->count >= 2) return false;
  if (!slot->used) { slot->used = true; slot->at = now; memcpy(slot->principal, principal, 32); }
  ++slot->count; ++count_;
  return true;
}
void BotHttps::perform(const BotIoRequest &request, BotIoResult &result,
                      const std::atomic<uint32_t> &generation, const std::atomic<uint32_t> &grant,
                      const std::atomic<bool> &enabled, const std::atomic<bool> &stopping,
                      const BotNetworkRoute *route, const BotHttpsFetchRequest *fetch,
                      BotHttpsFetchResult *fetchResult) {
  result = {}; result.token = request.token;
  transport_.resetDiagnostics();
  if (fetchResult) {
    *fetchResult = {};
    fetchResult->id = fetch ? fetch->id : 0;
    fetchResult->state = BotHttpsFetchResult::Running;
  }
  struct Close { BotHttpsTransport &transport; ~Close() { transport.close(); } } close{transport_};
  bool submitted = false;
  const auto alive = [&]() {
    if (stopping || generation.load() != request.token.generation) {
      failure(result, "cancelled", "RPC source/job cancelled; remote outcome may be unknown"); return false;
    }
    if (!enabled || grant.load() != request.grant) {
      failure(result, submitted ? "unknown" : "permission_denied",
              submitted ? "HTTPS grant revoked after submission; remote outcome unknown" :
                          "HTTPS grant revoked before submission");
      return false;
    }
    if (request.networkEpoch && request.networkEpoch != botNetworkEpoch()) {
      failure(result, submitted ? "unknown" : "permission_denied",
              submitted ? "HTTPS configuration changed after submission; remote outcome unknown" :
                          "HTTPS configuration changed before submission");
      return false;
    }
    if (int32_t(transport_.now() - request.deadline) >= 0) {
      failure(result, "timeout", "HTTPS deadline exceeded; remote outcome may be unknown"); return false;
    }
    if (!transport_.online()) {
      failure(result, submitted ? "unknown" : "offline",
              submitted ? "WiFi disconnected after HTTPS submission; remote outcome unknown" :
                          "WiFi is offline");
      return false;
    }
    char policyError[sizeof(result.error)]{};
    if (!transport_.validate(policyError, sizeof(policyError))) {
      const char *reason = policyError[0] ? policyError : "TLS time trust unavailable";
      failure(result, submitted ? "unknown" : "tls_policy", reason);
      if (submitted)
        snprintf(result.error, sizeof(result.error),
                 "%.*s; remote outcome unknown", 96, reason);
      return false;
    }
    return true;
  };
  if (!alive()) return;
  if (uint32_t(request.deadline - transport_.now()) > BotHttpsDeadlineMs) {
    failure(result, "invalid_request", "RPC deadline exceeds native bound"); return;
  }
  const BotHttpsConfig selected = route ? route->https() : config_;
  if (!selected.valid()) { failure(result, "unconfigured", "Verified HTTPS is not configured"); return; }
  const bool package = request.kind == BotIoRequest::PackageGet;
  if ((request.kind != BotIoRequest::Rpc && request.kind != BotIoRequest::HttpGet &&
       request.kind != BotIoRequest::HttpPost && !package) ||
      (package && (!route || route->post || strcmp(request.key, "package") ||
                   !fetch || !fetchResult || !fetch->id || !fetch->sink ||
                   !fetch->maxBytes || fetch->maxBytes > BotSourceLimit)) ||
      !request.token.job || !request.token.operation ||
      !memchr(request.key, 0, sizeof(request.key)) ||
      !memchr(request.value, 0, sizeof(request.value)) ||
      !memchr(request.json, 0, sizeof(request.json)) ||
      !memchr(request.endpoint, 0, sizeof(request.endpoint)) ||
      (!route && !legacy(request))) {
    failure(result, "invalid_request", "Invalid bounded RPC operation"); return;
  }
  uint8_t principal = 0;
  for (const auto byte : request.principal) principal |= byte;
  if (!principal) { failure(result, "permission_denied", "RPC requires native caller attribution"); return; }
  if (!workspace_) workspace_ = allocateRoleStorage<Workspace>("bot HTTPS buffers");
  if (!workspace_) { failure(result, "memory", "HTTPS PSRAM workspace unavailable"); return; }
  auto &w = *workspace_;
  struct Scrub {
    Workspace &workspace;
    ~Scrub() {
      volatile char *header = workspace.request, *body = workspace.body;
      for (size_t i = 0; i < sizeof(workspace.request); ++i) header[i] = 0;
      for (size_t i = 0; i < sizeof(workspace.body); ++i) body[i] = 0;
    }
  } scrub{w};
  uint8_t operation = 0;
  const bool old = legacy(request);
  if (!package && (old ? (!encode(request, w.body, sizeof(w.body), operation, transport_.requestNonce()) ||
             !(selected.operations & operation)) :
            (!route || !generalBody(request, w.body, sizeof(w.body), transport_.requestNonce())))) {
    failure(result, "permission_denied", "Home RPC operation/arguments not allowed"); return;
  }
  if (!rate(request.principal)) { failure(result, "rate_limited", "Home RPC limit: 2/caller, 4/global per minute"); return; }
  if (!transport_.open(selected, result.error, sizeof(result.error))) {
    snprintf(result.rpcCode, sizeof(result.rpcCode), "%s",
             transport_.failure() == BotHttpsTransport::Failure::Heap ? "heap_unavailable" : "transport_error");
    if (!result.error[0]) strcpy(result.error, "Verified TLS connection failed");
    return;
  }
  const bool post = old || (route && route->post);
  const char *path = old ? "/v1/rpc" : route->path;
  const int bytes = post ? snprintf(w.request, sizeof(w.request),
      "POST %s HTTP/1.1\r\nHost: %s:%u\r\nAuthorization: Bearer %s\r\n"
      "Content-Type: application/json\r\nAccept: application/json\r\n"
      "Accept-Encoding: identity\r\nConnection: close\r\nContent-Length: %u\r\n\r\n",
      path, selected.host, selected.port, selected.token, unsigned(strlen(w.body))) :
      snprintf(w.request, sizeof(w.request),
      "GET %s HTTP/1.1\r\nHost: %s:%u\r\nAuthorization: Bearer %s\r\n"
      "Accept: %s\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n",
      path, selected.host, selected.port, selected.token,
      package ? "application/octet-stream" : "application/json");
  if (bytes <= 0 || size_t(bytes) >= sizeof(w.request)) {
    failure(result, "invalid_request", "Bounded RPC headers exceeded"); return;
  }
  const auto write = [&](const char *data, size_t size) {
    size_t offset = 0;
    while (offset < size && alive()) {
      const int count = transport_.write(reinterpret_cast<const uint8_t *>(data + offset), size - offset);
      if (count < 0 || size_t(count) > size - offset) {
        failure(result, "unknown", "HTTPS write failed; remote outcome unknown"); return false;
      }
      if (!count) { transport_.idle(); continue; }
      submitted = result.networkSubmitted = true;
      offset += size_t(count);
    }
    return offset == size;
  };
  if (!write(w.request, size_t(bytes)) || (post && !write(w.body, strlen(w.body)))) return;
  unsigned headerBytes = 0;
  const auto read = [&](char *data, size_t size) {
    size_t offset = 0;
    while (offset < size && alive()) {
      const int count = transport_.read(reinterpret_cast<uint8_t *>(data + offset), size - offset);
      if (count < 0 || size_t(count) > size - offset) {
        failure(result, "unknown", "HTTPS response incomplete; remote outcome unknown"); return false;
      }
      if (!count) transport_.idle();
      else offset += size_t(count);
    }
    return offset == size;
  };
  const auto line = [&]() {
    size_t n = 0;
    while (n + 1 < sizeof(w.line) && ++headerBytes <= 2048) {
      if (!read(w.line + n, 1)) return false;
      if (w.line[n++] == '\n') {
        if (n < 2 || w.line[n - 2] != '\r') break;
        for (size_t i = 0; i + 2 < n; ++i)
          if (uint8_t(w.line[i]) < 32 && w.line[i] != '\t') {
            failure(result, "invalid_response", "Control byte in HTTP header"); return false;
          }
        w.line[n - 2] = 0; return true;
      }
    }
    failure(result, "invalid_response", "HTTP header syntax/budget exceeded"); return false;
  };
  if (!line()) return;
  if (strlen(w.line) < 12 || strncmp(w.line, "HTTP/1.1 ", 9) ||
      w.line[9] < '1' || w.line[9] > '5' || w.line[10] < '0' || w.line[10] > '9' ||
      w.line[11] < '0' || w.line[11] > '9' || (w.line[12] && w.line[12] != ' ')) {
    failure(result, "invalid_response", "Invalid HTTP status"); return;
  }
  result.httpStatus = uint16_t((w.line[9] - '0') * 100 + (w.line[10] - '0') * 10 + w.line[11] - '0');
  if (result.httpStatus >= 300 && result.httpStatus < 400) {
    failure(result, "redirect_denied", "HTTPS redirects are disabled"); return;
  }
  size_t length = 0;
  bool hasLength = false, hasType = false, hasEncoding = false;
  while (line()) {
    if (!w.line[0]) break;
    char *colon = strchr(w.line, ':');
    if (!colon || w.line[0] == ' ' || w.line[0] == '\t') {
      failure(result, "invalid_response", "Malformed HTTP header"); return;
    }
    *colon++ = 0;
    if (!w.line[0]) { failure(result, "invalid_response", "Empty HTTP field name"); return; }
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(w.line); *p; ++p)
      if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') || strchr("!#$%&'*+-.^_`|~", *p))) {
        failure(result, "invalid_response", "Invalid HTTP field name"); return;
      }
    while (*colon == ' ' || *colon == '\t') ++colon;
    if (!strcasecmp(w.line, "content-length")) {
      if (hasLength || !*colon) { failure(result, "invalid_response", "Duplicate/empty content length"); return; }
      hasLength = true;
      for (const char *p = colon; *p; ++p) {
        if (*p < '0' || *p > '9' ||
            length > (package ? fetch->maxBytes : BotHttpsBodyLimit)) {
          failure(result, "invalid_response", "Invalid bounded content length"); return;
        }
        length = length * 10 + unsigned(*p - '0');
      }
    } else if (!strcasecmp(w.line, "content-type")) {
      if (hasType || (package && result.httpStatus < 400 ?
          (strcasecmp(colon, "application/octet-stream") &&
           strcasecmp(colon, "text/plain") &&
           strcasecmp(colon, "text/plain; charset=utf-8") &&
           strcasecmp(colon, "application/x-lua") &&
           strcasecmp(colon, "application/wasm")) :
          (strcasecmp(colon, "application/json") &&
           strcasecmp(colon, "application/json; charset=utf-8")))) {
        failure(result, "invalid_response", "Unexpected HTTPS content type"); return;
      }
      hasType = true;
    } else if (!strcasecmp(w.line, "transfer-encoding") ||
               (!strcasecmp(w.line, "content-encoding") &&
                (hasEncoding || strcasecmp(colon, "identity")))) {
      failure(result, "invalid_response", "Encoded/chunked HTTP bodies are not supported"); return;
    } else if (!strcasecmp(w.line, "content-encoding")) hasEncoding = true;
  }
  if (result.rpcCode[0] || !alive()) return;
  if (package) {
    if (result.httpStatus != 200 || !hasLength || !hasType || !length ||
        length > fetch->maxBytes) {
      failure(result, result.httpStatus >= 400 ? "http_error" : "invalid_response",
              result.httpStatus >= 400 ? "Package endpoint rejected the fetch" :
                                         "Package requires bounded raw HTTP 200 source");
      return;
    }
    struct SinkGuard {
      BotHttpsBodySink &sink;
      const BotIoResult &result;
      bool started = false, complete = false;
      ~SinkGuard() {
        if (started && !complete)
          sink.abort(result.error[0] ? result.error : "Package fetch interrupted");
      }
    } guard{*fetch->sink, result};
    guard.started = true;
    if (!fetch->sink->begin(uint32_t(length))) {
      failure(result, "storage_error", "Package staging failed to begin"); return;
    }
    SHA256 hash;
    uint8_t chunk[128]{};
    struct ScrubChunk {
      uint8_t *bytes;
      ~ScrubChunk() { volatile uint8_t *p = bytes; for (size_t i = 0; i < 128; ++i) p[i] = 0; }
    } scrubChunk{chunk};
    char credentialWindow[256]{};
    struct ScrubWindow {
      char *bytes;
      ~ScrubWindow() { volatile char *p = bytes; for (size_t i = 0; i < 256; ++i) p[i] = 0; }
    } scrubWindow{credentialWindow};
    const size_t credentialSize = strlen(selected.token);
    size_t held = 0;
    for (size_t offset = 0; offset < length;) {
      const size_t count = std::min(sizeof(chunk), length - offset);
      if (!read(reinterpret_cast<char *>(chunk), count)) return;
      for (size_t i = 0; i < count; ++i) {
        if (held == credentialSize) {
          memmove(credentialWindow, credentialWindow + 1, credentialSize - 1);
          --held;
        }
        credentialWindow[held++] = char(chunk[i]);
        if (held == credentialSize &&
            !memcmp(credentialWindow, selected.token, credentialSize)) {
          failure(result, "invalid_response", "Package source contained a configured credential");
          return;
        }
      }
      hash.update(chunk, count);
      if (!fetch->sink->write(chunk, count)) {
        failure(result, "storage_error", "Package staging write failed"); return;
      }
      offset += count;
    }
    if (!alive()) return;
    hash.finalize(fetchResult->sha256, sizeof(fetchResult->sha256));
    fetchResult->bytes = uint32_t(length);
    if (memcmp(fetchResult->sha256, fetch->expectedSha256, 32)) {
      failure(result, "hash_mismatch", "Package source SHA-256 did not match expected hash");
      return;
    }
    if (!fetch->sink->commit(uint32_t(length), fetchResult->sha256)) {
      failure(result, "storage_error", "Package staging commit failed"); return;
    }
    if (!alive()) return;
    guard.complete = true;
    result.ok = true;
    return;
  }
  if (!hasLength || !hasType || !length || length > BotNetworkResponseLimit) {
    failure(result, "invalid_response", "Missing/oversized bounded JSON body"); return;
  }
  if (!read(w.body, length) || !alive()) return;
  w.body[length] = 0;
  if (jsonBudget(w.body, length) && credentialInResponse(w.body, length, selected.token)) {
    failure(result, "invalid_response", "HTTPS response contained a configured credential");
    return;
  }
  decode(request, w.body, length, result);
  alive();
}

#ifdef ARDUINO_ARCH_ESP32
#ifndef MBEDTLS_SSL_KEEP_PEER_CERTIFICATE
#error Direct HTTPS requires the SDK to retain the peer certificate chain
#endif
namespace {
constexpr size_t RecordBytes[] = {MBEDTLS_SSL_IN_BUFFER_LEN, MBEDTLS_SSL_OUT_BUFFER_LEN};
constexpr size_t RecordCount = sizeof(RecordBytes) / sizeof(*RecordBytes);
constexpr size_t RemainingWorkBytes = BotHttpsWorkBudgetBytes - RecordBytes[0] - RecordBytes[1];
constexpr unsigned TlsWorkCaps =
    (ONCHIP_TLS_PSRAM ? MALLOC_CAP_SPIRAM : MALLOC_CAP_INTERNAL) | MALLOC_CAP_8BIT;
constexpr size_t InternalWorkBytes =
    ONCHIP_TLS_PSRAM ? BotHttpsClientBudgetBytes : BotHttpsWorkBudgetBytes;
static_assert(MBEDTLS_SSL_IN_BUFFER_LEN <= 17 * 1024 &&
              MBEDTLS_SSL_OUT_BUFFER_LEN <= 17 * 1024 &&
              sizeof(sslclient_context) <= 4096,
              "Recheck the native TLS memory budget for this SDK");
BotHttpsFailurePoint tlsMemoryAvailable() {
  constexpr unsigned internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  if (heap_caps_get_free_size(internal) < InternalWorkBytes + BotHttpsRadioReserveBytes ||
      (ONCHIP_TLS_PSRAM && heap_caps_get_free_size(TlsWorkCaps) < BotHttpsWorkBudgetBytes))
    return BotHttpsFailurePoint::AdmissionTotal;
  if (heap_caps_get_largest_free_block(TlsWorkCaps) < std::max(RecordBytes[0], RecordBytes[1]) ||
      (ONCHIP_TLS_PSRAM && heap_caps_get_largest_free_block(internal) < sizeof(sslclient_context)))
    return BotHttpsFailurePoint::AdmissionBlock;
  // Only the SDK's two record buffers need large-block probes. Context and
  // handshake allocations use the remaining aggregate work budget.
  void *blocks[RecordCount]{};
  unsigned count = 0;
  for (; count < RecordCount; ++count) {
    blocks[count] = heap_caps_malloc(RecordBytes[count], TlsWorkCaps);
    if (!blocks[count]) break;
  }
  const auto point = count < RecordCount ?
      (count ? BotHttpsFailurePoint::AdmissionOutput : BotHttpsFailurePoint::AdmissionInput) :
      heap_caps_get_free_size(TlsWorkCaps) <
          RemainingWorkBytes + (ONCHIP_TLS_PSRAM ? 0 : BotHttpsRadioReserveBytes) ?
      BotHttpsFailurePoint::AdmissionReserve : BotHttpsFailurePoint::None;
  while (count) heap_caps_free(blocks[--count]);
  return point;
}
bool tlsAllocationFailed(int error) {
  if (error >= 0 || error < -0x7fff) return false;
  const unsigned code = unsigned(-error);
  // Mbed TLS can combine a high-level error with a low-level allocation error.
  const unsigned low = code & 0x7f;
  if (low == unsigned(-MBEDTLS_ERR_MPI_ALLOC_FAILED) ||
      low == unsigned(-MBEDTLS_ERR_ASN1_ALLOC_FAILED)) return true;
  switch (-int(code & 0x7f80)) {
    case MBEDTLS_ERR_SSL_ALLOC_FAILED:
    case MBEDTLS_ERR_X509_ALLOC_FAILED:
    case MBEDTLS_ERR_PK_ALLOC_FAILED:
    case MBEDTLS_ERR_PEM_ALLOC_FAILED:
    case MBEDTLS_ERR_ECP_ALLOC_FAILED:
    case MBEDTLS_ERR_DHM_ALLOC_FAILED:
    case MBEDTLS_ERR_MD_ALLOC_FAILED:
    case MBEDTLS_ERR_CIPHER_ALLOC_FAILED: return true;
    default: return false;
  }
}
bool certificateTime(const mbedtls_x509_time &time, int64_t &seconds) {
  if (time.year < 1 || time.year > 9999 || time.mon < 1 || time.mon > 12 ||
      time.day < 1 || time.hour < 0 || time.hour > 23 || time.min < 0 || time.min > 59 ||
      time.sec < 0 || time.sec > 59) return false;
  const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const bool leap = time.year % 4 == 0 && (time.year % 100 != 0 || time.year % 400 == 0);
  if (time.day > days[time.mon - 1] + (time.mon == 2 && leap ? 1 : 0)) return false;
  const int64_t year = time.year - 1;
  int64_t date = 365 * year + year / 4 - year / 100 + year / 400 - 719162 + time.day - 1;
  for (int month = 1; month < time.mon; ++month)
    date += days[month - 1] + (month == 2 && leap ? 1 : 0);
  seconds = date * 86400 + time.hour * 3600 + time.min * 60 + time.sec;
  return true;
}
struct CertificateWindow {
  int64_t from = INT64_MIN, until = INT64_MAX;
  bool add(const mbedtls_x509_crt *chain) {
    if (!chain) return false;
    unsigned count = 0;
    size_t bytes = 0;
    for (const auto *certificate = chain; certificate; certificate = certificate->next) {
      if (++count > 8 || !certificate->raw.p || !certificate->raw.len ||
          certificate->raw.len > 16384 - bytes) return false;
      bytes += certificate->raw.len;
      int64_t start, end;
      if (!certificateTime(certificate->valid_from, start) ||
          !certificateTime(certificate->valid_to, end) || start > end) return false;
      from = std::max(from, start);
      until = std::min(until, end);
    }
    return from <= until;
  }
};
class FixedPeerClient final : public WiFiClientSecure {
public:
  uint16_t cipher() const {
    const char *name = mbedtls_ssl_get_ciphersuite(&sslclient->ssl_ctx);
    return name ? uint16_t(mbedtls_ssl_get_ciphersuite_id(name)) : 0;
  }
  void measureBuffers() {
#if ONCHIP_BOT_HTTPS_METRICS
    const auto &tls = sslclient->ssl_ctx;
    Serial.printf("HTTPS buffers: input=%u output=%u context=%u\n",
        unsigned(tls.in_buf ? heap_caps_get_allocated_size(tls.in_buf) : 0),
        unsigned(tls.out_buf ? heap_caps_get_allocated_size(tls.out_buf) : 0),
        unsigned(heap_caps_get_allocated_size(sslclient)));
#endif
  }
  void disableRenegotiation() {
#ifdef MBEDTLS_SSL_RENEGOTIATION
    mbedtls_ssl_conf_renegotiation(&sslclient->ssl_conf, MBEDTLS_SSL_RENEGOTIATION_DISABLED);
#endif
  }
};
class SecureTransport final : public BotHttpsTransport {
public:
  uint64_t requestNonce() const override {
#ifndef ONCHIP_CLOCK_TEST_BUSY
    return nonce_;
#else
    return 0;
#endif
  }
private:
#ifndef ONCHIP_CLOCK_TEST_BUSY
  uint64_t nonce_ = (uint64_t(esp_random()) << 32) | esp_random();
#endif
  FixedPeerClient *client_ = nullptr;
  bool measuring_ = false;
  bool certificates_ = false;
  Failure failure_ = Failure::Transport;
  CertificateWindow validity_;
  struct AnchorCache {
    char pem[4097]{};
    CertificateWindow validity;
  };
  AnchorCache *anchors_ = nullptr;
  BotHttpsDiagnostics diagnostics_;
  bool connected_ = false;
  static constexpr unsigned HeapCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  void failedAt(BotHttpsFailurePoint point, int sdk = 0) {
    diagnostics_.point = point;
    diagnostics_.sdk = sdk;
    diagnostics_.failure = heap_caps_get_free_size(HeapCaps);
    diagnostics_.largestFailure = heap_caps_get_largest_free_block(HeapCaps);
  }
  bool anchorWindow(const char *ca, char *error, size_t capacity) {
    if (anchors_ && !strcmp(anchors_->pem, ca)) {
      validity_ = anchors_->validity;
      diagnostics_.caCached = true;
      return true;
    }
    // Only the date window is cached. The SDK still verifies this request's
    // chain and hostname against the configured CA during every handshake.
    mbedtls_x509_crt parsed;
    mbedtls_x509_crt_init(&parsed);
    const int code = mbedtls_x509_crt_parse(&parsed,
        reinterpret_cast<const unsigned char *>(ca), strlen(ca) + 1);
    phaseBotHttpsMetrics("ca-parsed");
    diagnostics_.ca = heap_caps_get_free_size(HeapCaps);
    validity_ = {};
    const bool valid = code == 0 && validity_.add(&parsed);
    if (!valid) failedAt(BotHttpsFailurePoint::CaParse, code);
    mbedtls_x509_crt_free(&parsed);
    phaseBotHttpsMetrics("ca-freed");
    if (!valid) {
      if (tlsAllocationFailed(code)) {
        failure_ = Failure::Heap;
        snprintf(error, capacity, "TLS configured CA allocation failed (SDK %d)", code);
      } else snprintf(error, capacity, "TLS configured CA dates/bundle invalid");
      return false;
    }
    if (!anchors_) anchors_ = allocateRoleStorage<AnchorCache>("HTTPS CA date cache");
    if (anchors_) {
      strcpy(anchors_->pem, ca);
      anchors_->validity = validity_;
    }
    return true;
  }
public:
  ~SecureTransport() override { close(); releaseRoleStorage(anchors_); }
  bool online() const override { return WiFi.status() == WL_CONNECTED; }
  Failure failure() const override { return failure_; }
  BotHttpsDiagnostics diagnostics() const override { return diagnostics_; }
  void resetDiagnostics() override {
    diagnostics_ = {};
    diagnostics_.before = heap_caps_get_free_size(HeapCaps);
    diagnostics_.largestBefore = heap_caps_get_largest_free_block(HeapCaps);
    diagnostics_.globalMinBefore = heap_caps_get_minimum_free_size(HeapCaps);
    failure_ = Failure::Transport;
  }
  bool validate(char *error, size_t capacity) override {
    if (client_ && heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < BotHttpsRadioReserveBytes) {
      failure_ = Failure::Heap;
      failedAt(connected_ ? BotHttpsFailurePoint::IoReserve : BotHttpsFailurePoint::ConnectedReserve);
      snprintf(error, capacity, "TLS radio heap reserve unavailable"); return false;
    }
    uint32_t earliest, latest;
    const char *reason = nullptr;
    if (!trustedNetworkTime(earliest, latest, &reason)) {
      failedAt(BotHttpsFailurePoint::Clock);
      snprintf(error, capacity, "%s", reason);
      return false;
    }
    if (certificates_ && (validity_.from > earliest || validity_.until < latest)) {
      failedAt(BotHttpsFailurePoint::CertificateTime);
      snprintf(error, capacity, "TLS certificate chain expired or not yet valid");
      return false;
    }
    return true;
  }
  bool open(const BotHttpsConfig &config, char *error, size_t capacity) override {
    failure_ = Failure::Transport;
    if (!beginBotHttpsMemory()) {
      failure_ = Failure::Heap;
      failedAt(BotHttpsFailurePoint::AdmissionTotal);
      snprintf(error, capacity, "TLS PSRAM allocator setup failed");
      return false;
    }
    if (!validate(error, capacity)) return false;
    startBotHttpsMetrics();
    measuring_ = true;
    phaseBotHttpsMetrics("before-client");
    if (WiFi.status() != WL_CONNECTED) {
      snprintf(error, capacity, "WiFi is offline"); return false;
    }
    const uint32_t admissionAt = millis();
    const auto admit = [&]() {
      bool waiting = false;
      for (;;) {
        const auto point = tlsMemoryAvailable();
        if (point == BotHttpsFailurePoint::None) break;
        if (uint32_t(millis() - admissionAt) >= 1000) {
          failure_ = Failure::Heap;
          failedAt(point);
          snprintf(error, capacity, ONCHIP_TLS_PSRAM ?
                   "TLS memory unavailable: 68KiB PSRAM, 8KiB client, 32KiB radio reserve" :
                   "TLS memory unavailable: 68KiB internal work, 32KiB radio reserve");
          return false;
        }
        if (!waiting) { phaseBotHttpsMetrics("admission-wait"); waiting = true; }
        if (!validate(error, capacity)) return false;
        vTaskDelay(1);
      }
      if (waiting) phaseBotHttpsMetrics("admission-recovered");
      return true;
    };
    if (!admit()) return false;
    IPAddress address;
    if (!address.fromString(config.address)) {
      snprintf(error, capacity, "Invalid configured HTTPS address"); return false;
    }
    if (!bounded(config.ca, 4096)) {
      failedAt(BotHttpsFailurePoint::CaParse);
      snprintf(error, capacity, "TLS configured CA exceeds 4096 bytes"); return false;
    }
    if (!anchorWindow(config.ca, error, capacity)) return false;
    certificates_ = true;
    if (!validate(error, capacity)) return false;
    // A cache miss parsed certificates since admission. Recheck after releasing
    // them, before creating the socket/context; this is still not a reservation.
    if (!diagnostics_.caCached && !admit()) return false;
    client_ = new (std::nothrow) FixedPeerClient;
    if (!client_) {
      failure_ = Failure::Heap;
      failedAt(BotHttpsFailurePoint::Client);
      snprintf(error, capacity, "TLS client allocation failed"); return false;
    }
    phaseBotHttpsMetrics("client-created");
    client_->setTimeout(1);
    client_->setHandshakeTimeout(8);
    client_->setCACert(config.ca);
    // Fixed native address avoids the SDK's potentially 31-second blocking DNS.
    // Supplying host explicitly retains SNI and certificate hostname validation.
    if (!client_->connect(address, config.port, config.host, config.ca, nullptr, nullptr)) {
      phaseBotHttpsMetrics("connect-failed");
      char detail[80]{};
      const int code = client_->lastError(detail, sizeof(detail));
      failure_ = tlsAllocationFailed(code) ? Failure::Heap : Failure::Transport;
      failedAt(BotHttpsFailurePoint::Connect, code);
      snprintf(error, capacity, "TLS %s failed (SDK %d): %.79s",
               failure_ == Failure::Heap ? "allocation" : "connection/verification", code, detail);
      return false;
    }
    phaseBotHttpsMetrics("connected");
    diagnostics_.connected = heap_caps_get_free_size(HeapCaps);
    diagnostics_.cipher = client_->cipher();
    for (auto *peer = client_->getPeerCertificate(); peer && diagnostics_.peerCount < 8; peer = peer->next) {
      if (!peer->raw.p || !peer->raw.len || peer->raw.len > 16384 - diagnostics_.peerBytes) break;
      ++diagnostics_.peerCount;
      diagnostics_.peerBytes += peer->raw.len;
    }
    if (!validate(error, capacity)) return false;
    client_->measureBuffers();
    client_->disableRenegotiation();
    if (!validity_.add(client_->getPeerCertificate())) {
      failedAt(BotHttpsFailurePoint::PeerCertificate);
      snprintf(error, capacity, "TLS peer certificate dates/chain missing or invalid"); return false;
    }
    if (!validate(error, capacity)) return false;
    connected_ = true;
    return true;
  }
  int write(const uint8_t *data, size_t size) override { return int(client_->write(data, size)); }
  int read(uint8_t *data, size_t size) override {
    const int available = client_->available();
    if (available <= 0) return client_->connected() ? 0 : -1;
    return client_->read(data, size_t(available) < size ? size_t(available) : size);
  }
  void close() override {
    if (measuring_) sampleBotHttpsMetrics();
    if (client_) {
      phaseBotHttpsMetrics("before-close");
      client_->stop();
      phaseBotHttpsMetrics("stopped");
      delete client_; client_ = nullptr;
      phaseBotHttpsMetrics("client-freed");
    }
    if (measuring_) { finishBotHttpsMetrics(); measuring_ = false; }
    certificates_ = false;
    connected_ = false;
    validity_ = {};
    failure_ = Failure::Transport;
    diagnostics_.after = heap_caps_get_free_size(HeapCaps);
    diagnostics_.largestAfter = heap_caps_get_largest_free_block(HeapCaps);
    diagnostics_.globalMinAfter = heap_caps_get_minimum_free_size(HeapCaps);
  }
  uint32_t now() const override { return millis(); }
  void idle() override { vTaskDelay(1); }
};
}
bool beginBotHttpsMemory() {
#if ONCHIP_TLS_PSRAM
  // Install process-wide SDK hooks before starting workers. The free hook also
  // accepts allocations made by the SDK's previous internal-memory allocator.
  static const bool configured = mbedtls_platform_set_calloc_free(
      [](size_t count, size_t size) -> void * {
        return heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      }, heap_caps_free) == 0;
  return configured;
#else
  return true;
#endif
}
BotHttpsTransport *createBotHttpsTransport() { return new (std::nothrow) SecureTransport; }
#elif defined(ONCHIP_BOT_NATIVE_HTTPS)
bool beginBotHttpsMemory() { return true; }
BotHttpsTransport *createNativeBotHttpsTransport();
BotHttpsTransport *createBotHttpsTransport() { return createNativeBotHttpsTransport(); }
#else
bool beginBotHttpsMemory() { return true; }
BotHttpsTransport *createBotHttpsTransport() { return nullptr; }
#endif
#else
bool beginBotHttpsMemory() { return true; }
BotHttps::~BotHttps() = default;
BotHttpsTransport *createBotHttpsTransport() { return nullptr; }
bool BotHttps::decode(const BotIoRequest &, const char *, size_t, BotIoResult &result) {
  failure(result, "unavailable", "Home HTTPS is not built into this profile"); return false;
}
void BotHttps::perform(const BotIoRequest &request, BotIoResult &result,
                      const std::atomic<uint32_t> &, const std::atomic<uint32_t> &,
                      const std::atomic<bool> &, const std::atomic<bool> &,
                      const BotNetworkRoute *, const BotHttpsFetchRequest *fetch,
                      BotHttpsFetchResult *fetchResult) {
  result = {}; result.token = request.token;
  failure(result, "unavailable", "Home HTTPS is not built into this profile");
  if (fetchResult) {
    *fetchResult = {};
    fetchResult->id = fetch ? fetch->id : 0;
    fetchResult->state = BotHttpsFetchResult::Failed;
    snprintf(fetchResult->rpcCode, sizeof(fetchResult->rpcCode), "%s", result.rpcCode);
    snprintf(fetchResult->error, sizeof(fetchResult->error), "%.*s",
             int(sizeof(fetchResult->error) - 1), result.error);
  }
}
#endif
} // namespace onchip
