// SPDX-License-Identifier: Apache-2.0
#include "Clock.h"
#include "Config.h"
#include <cassert>
#include <cstring>
#include <string>
#include <sys/time.h>

static unsigned long now;
static bool publishDuringMillis;
unsigned long millis() {
  const auto value = now;
  if (publishDuringMillis) {
    publishDuringMillis = false;
    ++now;
    onchip::receiveNetworkTime(1790265440);
  }
  return value;
}
void delay(unsigned long ms) { now += ms; }
static void (*notification)(struct timeval *);
static uint32_t configuredInterval;
static std::string configuredServer, responseBody, responseStatus, contentType;
static bool noCache;
static httpd_uri_t registered;
static esp_err_t registrationResult = ESP_OK;

void sntp_set_time_sync_notification_cb(void (*callback)(struct timeval *)) {
  notification = callback;
}
void sntp_set_sync_interval(uint32_t interval) {
  configuredInterval = interval;
}
void configTime(long offset, int daylight, const char *server) {
  assert(offset == 0 && daylight == 0);
  configuredServer = server;
}
esp_err_t httpd_register_uri_handler(httpd_handle_t, const httpd_uri_t *route) {
  registered = *route;
  return registrationResult;
}
esp_err_t httpd_resp_set_type(httpd_req_t *, const char *type) {
  contentType = type;
  return ESP_OK;
}
esp_err_t httpd_resp_set_hdr(httpd_req_t *, const char *name,
                             const char *value) {
  if (!strcmp(name, "Cache-Control"))
    noCache = !strcmp(value, "no-store");
  return ESP_OK;
}
esp_err_t httpd_resp_set_status(httpd_req_t *, const char *status) {
  responseStatus = status;
  return ESP_OK;
}
esp_err_t httpd_resp_send(httpd_req_t *, const char *data, size_t length) {
  responseBody.assign(data, length);
  return ESP_OK;
}
esp_err_t httpd_resp_sendstr(httpd_req_t *request, const char *data) {
  return httpd_resp_send(request, data, strlen(data));
}

int main() {
  onchip::beginClocks();
  uint32_t earliest = 0, latest = 0;
  assert(!onchip::trustedNetworkTime(earliest, latest));
  onchip::beginNetworkClock();
  if (ONCHIP_SNTP_SERVER[0]) {
    assert(configuredServer == ONCHIP_SNTP_SERVER && notification);
    assert(configuredInterval == ONCHIP_SNTP_INTERVAL_SECONDS * 1000u);
    struct timeval time{};
    time.tv_sec = 1790265409;
    notification(&time);
    assert(onchip::companionClock().getCurrentTime() == 1767225600u);
    now = 30000; // Dispatch delay must not turn a stale sample into fresh UTC.
    onchip::loopClocks();
    assert(onchip::companionClock().getCurrentTime() == 1790265439u);
    onchip::ClockSnapshot snapshot;
    assert(onchip::clockSnapshot(snapshot) &&
           snapshot.network_age_seconds == 30 &&
           snapshot.roles[2].synchronized);
    assert(onchip::trustedNetworkTime(earliest, latest) &&
           earliest == 1790265439u && latest == earliest + 1);
    time.tv_sec = -1;
    notification(&time);
    onchip::loopClocks();
    publishDuringMillis = true;
    onchip::loopClocks();
    assert(onchip::companionClock().getCurrentTime() == 1790265440u);
  } else {
    assert(configuredServer.empty() && !notification && !configuredInterval);
  }
  assert(onchip::registerClockHTTP(nullptr) == ESP_OK);
  assert(std::string(registered.uri) == "/api/clock");
  assert(registered.method == HTTP_GET && !registered.is_websocket);
  httpd_req_t request{};
  assert(registered.handler(&request) == ESP_OK);
  assert(contentType == "application/json" && noCache);
  assert(responseBody.find("\"role\":\"companion\"") != std::string::npos);
  if (ONCHIP_SNTP_SERVER[0]) {
    assert(responseBody.find("\"source\":\"sntp\",\"synchronized\":true") !=
           std::string::npos);
    assert(responseBody.find("\"rejected_samples\":1") != std::string::npos);
  } else {
    assert(responseBody.find("\"sntp_enabled\":false") != std::string::npos);
    assert(responseBody.find("\"source\":\"build\",\"synchronized\":false") !=
           std::string::npos);
  }
  now += 3001;
  assert(!onchip::trustedNetworkTime(earliest, latest));
  assert(registered.handler(&request) == ESP_OK);
  assert(responseStatus == "503 Service Unavailable");
  registrationResult = ESP_FAIL;
  assert(onchip::registerClockHTTP(nullptr) == ESP_FAIL);
  puts("PASS actual clock SDK callback, offline configuration and HTTP "
       "status/staleness");
}
