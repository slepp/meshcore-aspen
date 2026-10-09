// SPDX-License-Identifier: Apache-2.0
#if defined(MESHCORE_MAST_ADMIN) && MESHCORE_MAST_ADMIN
#include "MastWeb.h"
#include "MastAdminPage.h"
#include "Runtime.h"
#include "RoleStorage.h"
#include "ObserverWire.h"
#if MESHCORE_NODE_BACKUP
#include "NodeBackup.h"
#include "Runtime.h"
#endif
#include "Config.h"
#include "RadioNetwork.h"
#include <atomic>
#include <mutex>
#ifdef ARDUINO_ARCH_ESP32
#include <WiFi.h>
#include "EspFieldUpdate.h"
#endif
#if defined(ARDUINO_ARCH_ESP32) || defined(MESHCORE_MAST_WEB_TEST)
#include <esp_system.h>
#ifdef MESHCORE_MAST_WEB_TEST
#include <thread>
#include <chrono>
#else
#include <freertos/task.h>
#endif
#endif

namespace onchip {
#if defined(ARDUINO_ARCH_ESP32) || defined(MESHCORE_MAST_WEB_TEST)
namespace {
void pauseHTTP() {
#ifdef MESHCORE_MAST_WEB_TEST
  std::this_thread::sleep_for(std::chrono::milliseconds(1));
#else
  vTaskDelay(1);
#endif
}
enum { Idle, Pending, Running, Done, Abandoned };
std::atomic<int> state{Idle};
std::atomic<uint32_t> acknowledgedTicket{0};
std::atomic<bool> acknowledgedOK{false};
char command[MastAdmin::TextLimit + 1];
MastAdmin::Reply reply;
struct Session { char token[33]{}; uint32_t issued = 0; bool used = false; } sessions[2];
std::mutex sessionsMutex;
uint32_t lastLogin = 0;
std::atomic<bool> adminReady{false};
std::atomic<bool> recoveryOnly{false};
void expireSessions() {
  for (auto &session : sessions)
    if (session.used && uint32_t(millis() - session.issued) >= 600000) session = {};
}
void headers(httpd_req_t *request) {
  httpd_resp_set_type(request, "text/plain");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  httpd_resp_set_hdr(request, "Connection", "close");
  httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
  httpd_resp_set_hdr(request, "X-Frame-Options", "DENY");
  httpd_resp_set_hdr(request, "Referrer-Policy", "no-referrer");
}
esp_err_t error(httpd_req_t *request, const char *status, const char *text) {
  headers(request); httpd_resp_set_status(request, status);
  return httpd_resp_sendstr(request, text);
}
bool origin(httpd_req_t *request) {
  char host[100];
  if (httpd_req_get_hdr_value_str(request, "Host", host, sizeof(host)) != ESP_OK) return false;
  const auto allowed = [&](const char *name) {
    if (!name || !*name) return false;
    char withPort[100];
    snprintf(withPort, sizeof(withPort), "%s:%u", name, KISS_HTTP_PORT);
    return (KISS_HTTP_PORT == 80 && !strcasecmp(host, name)) || !strcasecmp(host, withPort);
  };
  char localName[64];
  snprintf(localName, sizeof(localName), "%s.local", radio_network::hostname);
  bool known = allowed(radio_network::hostname) || allowed(localName) || allowed(ONCHIP_MAST_WEB_HOST);
#ifdef ARDUINO_ARCH_ESP32
  if (WiFi.status() == WL_CONNECTED) known = known || allowed(WiFi.localIP().toString().c_str());
#endif
  if (!known) return false;
  const size_t size = httpd_req_get_hdr_value_len(request, "Origin");
  if (!size) return true;
  char value[128], expected[128];
  if (httpd_req_get_hdr_value_str(request, "Origin", value, sizeof(value)) != ESP_OK ||
      httpd_req_get_hdr_value_str(request, "Host", host, sizeof(host)) != ESP_OK) return false;
  snprintf(expected, sizeof(expected), "http://%s", host);
  return !strcmp(value, expected);
}
bool body(httpd_req_t *request, char *text, size_t capacity) {
  if (!request->content_len || request->content_len >= capacity) return false;
  const uint32_t started = millis();
  size_t n = 0;
  while (n < request->content_len) {
    if (uint32_t(millis() - started) >= 2000) return false;
    const int got = httpd_req_recv(request, text + n, request->content_len - n);
    if (got <= 0) return false;
    n += got;
  }
  if (memchr(text, 0, n) || uint32_t(millis() - started) >= 2000) return false;
  text[n] = 0;
  return true;
}
bool authenticated(httpd_req_t *request) {
  std::lock_guard<std::mutex> lock(sessionsMutex);
  expireSessions();
  if (!adminReady.load()) return false;
  char token[33];
  if (httpd_req_get_hdr_value_str(request, "X-Mast-Session", token, sizeof(token)) != ESP_OK ||
      strlen(token) != 32) return false;
  for (const auto &session : sessions) {
    unsigned difference = 0;
    for (unsigned i = 0; i < 32; ++i) difference |= uint8_t(token[i] ^ session.token[i]);
    if (session.used && !difference) return true;
  }
  return false;
}
esp_err_t login(httpd_req_t *request) {
  if (!origin(request)) return error(request, "403 Forbidden", "Cross-origin request rejected");
  if (!adminReady.load()) return error(request, "503 Service Unavailable", "Mast administration unavailable");
  const uint32_t now = millis();
  char password[16];
  const bool valid = body(request, password, sizeof(password)) && MastAdmin::passwordMatches(password);
  memset(password, 0, sizeof(password));
  if (!valid)
    return error(request, "403 Forbidden", "Login denied");
  if (lastLogin && uint32_t(now - lastLogin) < 1000)
    return error(request, "429 Too Many Requests", "Wait before login");
  Session *slot = nullptr;
  std::unique_lock<std::mutex> lock(sessionsMutex);
  expireSessions();
  for (auto &session : sessions)
    if (!session.used) { slot = &session; break; }
  if (!slot) {
    lock.unlock();
    return error(request, "429 Too Many Requests", "Two admin sessions already active");
  }
  uint8_t random[16]; esp_fill_random(random, sizeof(random));
  for (unsigned i = 0; i < 16; ++i) snprintf(slot->token + 2 * i, 3, "%02x", random[i]);
  slot->issued = now; slot->used = true;
  char token[33];
  memcpy(token, slot->token, sizeof(token));
  lock.unlock();
  lastLogin = now ? now : 1;
  headers(request);
  return httpd_resp_sendstr(request, token);
}
esp_err_t execute(httpd_req_t *request) {
  if (!origin(request) || !authenticated(request))
    return error(request, "403 Forbidden", "Authenticated same-origin session required");
  if (recoveryOnly)
    return error(request, "503 Service Unavailable", "Recovery mode: only signed application updates are available");
#ifdef ARDUINO_ARCH_ESP32
  if (espUpdateBusy())
    return error(request, "409 Conflict", "Application update active; inspect /admin/update before commands");
#endif
  char input[sizeof(command)];
  if (!body(request, input, sizeof(input)))
    return error(request, "400 Bad Request", "Expected 1..162 bytes of CLI text");
  const auto verb = [&](const char *prefix) {
    const size_t length = strlen(prefix);
    return !strncmp(input, prefix, length) && (!input[length] || input[length] == ' ');
  };
  if (verb("set wifi.ssid") || verb("set wifi.pwd") || verb("set wifi.enabled") ||
      verb("get wifi.pwd") || verb("wifi ssid") || verb("wifi password") ||
      verb("wifi forget") || (verb("wifi") && !verb("wifi status") &&
        !verb("wifi help") && !verb("wifi apply") && strcmp(input, "wifi"))) {
    memset(input, 0, sizeof(input));
    return error(request, "403 Forbidden", "WiFi secrets and setters require encrypted Management RF");
  }
  if (verb("cloudroom config begin") || verb("cloudroom config chunk") || verb("cloudroom config commit")) {
    memset(input, 0, sizeof(input));
    return error(request, "403 Forbidden", "Cloud room credential uploads require encrypted Management RF");
  }
  if (verb("role password")) {
    memset(input, 0, sizeof(input));
    return error(request, "403 Forbidden",
                 "Role password changes require authenticated encrypted Management RF");
  }
  if (verb("telemetry endpoint ca") || verb("telemetry endpoint token")) {
    memset(input, 0, sizeof(input));
    return error(request, "403 Forbidden", "Telemetry CA/token staging requires encrypted authenticated RF");
  }
  if (verb("mqtt ca") || verb("mqtt username") || verb("mqtt password")) {
    memset(input, 0, sizeof(input));
    return error(request, "403 Forbidden", "MQTT credentials and CA staging require encrypted Management RF");
  }
  if (!strncmp(input, "bot https ", 10)) {
    const char *operation = input + 10;
    while (*operation == ' ') ++operation;
    if ((!strncmp(operation, "ca", 2) &&
          (operation[2] == ' ' || !operation[2])) ||
        (!strncmp(operation, "token", 5) &&
          (operation[5] == ' ' || !operation[5]))) {
      memset(input, 0, sizeof(input));
      return error(request, "403 Forbidden",
                   "HTTPS CA and token staging require encrypted RF administration");
    }
  }
  int expected = Idle;
  if (!state.compare_exchange_strong(expected, Running))
    return error(request, "429 Too Many Requests", "Mast command mailbox busy");
  strcpy(command, input);
  reply = {};
  state = Pending;
  const uint32_t started = millis();
  while (state.load() != Done && uint32_t(millis() - started) < 2000) pauseHTTP();
  expected = Done;
  if (state.load() != Done) {
    state = Abandoned;
    return error(request, "504 Gateway Timeout", "Command outcome unknown; inspect status before retry");
  }
  const auto result = reply;
  headers(request);
  const auto sent = httpd_resp_sendstr(request, result.text);
  if (result.ticket) {
    acknowledgedOK = sent == ESP_OK;
    acknowledgedTicket = result.ticket;
  }
  state = Idle;
  return sent;
}
esp_err_t logout(httpd_req_t *request) {
  if (!origin(request) || !authenticated(request))
    return error(request, "403 Forbidden", "Authenticated same-origin session required");
  char token[33];
  if (httpd_req_get_hdr_value_str(request, "X-Mast-Session", token, sizeof(token)) != ESP_OK)
    return error(request, "403 Forbidden", "Session required");
  {
    std::lock_guard<std::mutex> lock(sessionsMutex);
    for (auto &session : sessions)
      if (!strcmp(token, session.token)) session = {};
  }
  headers(request);
  return httpd_resp_sendstr(request, "Logged out");
}
esp_err_t page(httpd_req_t *request) {
  if (!origin(request)) return error(request, "403 Forbidden", "Unrecognized mast host");
  headers(request);
  httpd_resp_set_type(request, "text/html");
  httpd_resp_set_hdr(request, "Content-Security-Policy",
                     "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; frame-ancestors 'none'");
  return httpd_resp_sendstr(request, MastAdminPage);
}
#ifdef ARDUINO_ARCH_ESP32
esp_err_t updateUpload(httpd_req_t *request) {
  if (!origin(request) || !authenticated(request))
    return error(request, "403 Forbidden", "Authenticated same-origin session required");
#if MESHCORE_NODE_BACKUP
  if (nodeBackup().active())
    return error(request, "409 Conflict", "Backup preparation active; wait or cancel it before an application update");
#endif
  return uploadEspUpdate(request);
}
esp_err_t updateStatus(httpd_req_t *request) {
  if (!origin(request) || !authenticated(request))
    return error(request, "403 Forbidden", "Authenticated same-origin session required");
  return statusEspUpdate(request);
}
esp_err_t updateReboot(httpd_req_t *request) {
  if (!origin(request) || !authenticated(request))
    return error(request, "403 Forbidden", "Authenticated same-origin session required");
  if (request->content_len)
    return error(request, "400 Bad Request", "Reboot request must have an empty body");
  return rebootEspUpdate(request);
}
#endif
esp_err_t observerTokenRequest(httpd_req_t *request) {
  if (!origin(request) || !authenticated(request))
    return error(request, "403 Forbidden", "Authenticated same-origin session required");
  if (recoveryOnly)
    return error(request, "503 Service Unavailable", "Recovery mode: observer identity signing unavailable");
#ifdef ARDUINO_ARCH_ESP32
  if (espUpdateBusy())
    return error(request, "409 Conflict", "Application update active; inspect /admin/update before signing");
#endif
  struct Signing {
    char audience[254]{}, token[1024]{}, error[120]{};
  };
  Signing *signing = allocateRoleStorage<Signing>("observer token request");
  if (!signing) return error(request, "503 Service Unavailable", "Observer token storage allocation failed");
  struct Release { Signing *&signing; ~Release() { releaseRoleStorage(signing); } } release{signing};
  if (!body(request, signing->audience, sizeof(signing->audience)))
    return error(request, "400 Bad Request", "Expected a broker DNS audience as 1..253 ASCII bytes");
  if (!observerWire::dnsAudience(signing->audience))
    return error(request, "400 Bad Request", "Observer token audience must be a broker DNS name");
  static uint32_t lastSigned = 0;
  if (lastSigned && uint32_t(millis() - lastSigned) < 2000)
    return error(request, "429 Too Many Requests", "Observer token rate limit; retry after two seconds");
  if (!observerToken(signing->audience, signing->token, sizeof(signing->token),
                      signing->error, sizeof(signing->error)))
    return error(request, "503 Service Unavailable", signing->error);
  lastSigned = millis() ? millis() : 1;
  headers(request);
  return httpd_resp_sendstr(request, signing->token);
}
#if MESHCORE_NODE_BACKUP
esp_err_t backupDownload(httpd_req_t *request) {
  if (!origin(request) || !authenticated(request))
    return error(request, "403 Forbidden", "Authenticated same-origin session required");
  if (recoveryOnly)
    return error(request, "503 Service Unavailable", "Recovery mode: node backups are unavailable");
  char query[24]{}, id[17]{};
  if (httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK ||
      strlen(query) != 19 || strncmp(query, "id=", 3) || strspn(query + 3, "0123456789abcdef") != 16)
    return error(request, "400 Bad Request", "Expected the saved backup id=ID16");
  memcpy(id, query + 3, 16);
  auto &backup = nodeBackup();
  if (!backup.beginRead(id))
    return error(request, "409 Conflict", "Backup changed or is busy; inspect backup status");
  struct Release { NodeBackup &backup; ~Release() { backup.endRead(); } } release{backup};
  headers(request);
  httpd_resp_set_type(request, "application/octet-stream");
  char disposition[100];
  snprintf(disposition, sizeof(disposition), "attachment; filename=\"node-backup-%s.mcb\"", id);
  httpd_resp_set_hdr(request, "Content-Disposition", disposition);
  uint8_t bytes[512];
  for (uint32_t offset = 0; offset < backup.bytes();) {
    const size_t count = std::min(size_t(backup.bytes() - offset), sizeof(bytes));
    if (backup.read(offset, bytes, count) != count) {
      diagnosticEvent("Node backup download read failed", DiagnosticSubsystem::Backup); return ESP_FAIL;
    }
    if (httpd_resp_send_chunk(request, reinterpret_cast<const char *>(bytes), count) != ESP_OK)
      return ESP_FAIL;
    offset += count;
  }
  return httpd_resp_send_chunk(request, nullptr, 0);
}
#endif
} // namespace
void serviceMastWebRecovery() {
  recoveryOnly = true;
  adminReady = publicProvisioningReady();
}
void serviceMastWeb(MastAdmin &admin) {
  adminReady = publicProvisioningReady() && admin.ready();
  {
    std::lock_guard<std::mutex> lock(sessionsMutex);
    expireSessions();
    if (!admin.ready()) for (auto &session : sessions) session = {};
  }
  const uint32_t ticket = acknowledgedTicket.exchange(0);
  if (ticket) admin.acknowledged(ticket, acknowledgedOK.load());
  int expected = Pending;
  if (state.compare_exchange_strong(expected, Running)) {
    admin.execute(command, reply, 0, MastAdmin::Transport::AuthenticatedWeb);
    expected = Running;
    if (!state.compare_exchange_strong(expected, Done)) {
      admin.acknowledged(reply.ticket, false); state = Idle;
    }
  } else if (state.load() == Abandoned) {
    admin.acknowledged(reply.ticket, false);
    state = Idle;
  }
}
esp_err_t registerMastWeb(httpd_handle_t server) {
  const char *paths[] = {"/admin", "/admin/login", "/admin/command", "/admin/logout"};
  esp_err_t (*handlers[])(httpd_req_t *) = {page, login, execute, logout};
  for (unsigned i = 0; i < 4; ++i) {
    httpd_uri_t route{};
    route.uri = paths[i]; route.method = i ? HTTP_POST : HTTP_GET; route.handler = handlers[i];
    const auto result = httpd_register_uri_handler(server, &route);
    if (result != ESP_OK) return result;
  }
  httpd_uri_t tokenRoute{};
  tokenRoute.uri = "/admin/observer-token"; tokenRoute.method = HTTP_POST; tokenRoute.handler = observerTokenRequest;
  const auto tokenResult = httpd_register_uri_handler(server, &tokenRoute);
  if (tokenResult != ESP_OK) return tokenResult;
#ifdef ARDUINO_ARCH_ESP32
  const char *updatePaths[] = {"/admin/update", "/admin/update", "/admin/update/reboot"};
  esp_err_t (*updateHandlers[])(httpd_req_t *) = {updateStatus, updateUpload, updateReboot};
  for (unsigned i = 0; i < 3; ++i) {
    httpd_uri_t route{};
    route.uri = updatePaths[i]; route.method = i ? HTTP_POST : HTTP_GET;
    route.handler = updateHandlers[i];
    const auto result = httpd_register_uri_handler(server, &route);
    if (result != ESP_OK) return result;
  }
#endif
#if MESHCORE_NODE_BACKUP
  httpd_uri_t backupRoute{};
  backupRoute.uri = "/admin/backup"; backupRoute.method = HTTP_GET; backupRoute.handler = backupDownload;
  const auto backupResult = httpd_register_uri_handler(server, &backupRoute);
  if (backupResult != ESP_OK) return backupResult;
#endif
  return ESP_OK;
}
#else
void serviceMastWeb(MastAdmin &) {}
void serviceMastWebRecovery() {}
#endif
} // namespace onchip
#endif
