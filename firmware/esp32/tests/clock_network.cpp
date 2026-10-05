// SPDX-License-Identifier: Apache-2.0
#include "Clock.h"
#include "Config.h"
#include <cassert>
#include <cstring>
#include <string>
#include <sys/time.h>
#include "../../shared/EspSntpClock.h"
#include "../../shared/RadioTimeProtocol.h"

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
  onchip::beginClocks();
  onchip::beginNetworkClock();
  assert(onchip::receiveGpsTime(1790265500u));
  onchip::loopClocks();
  assert(onchip::trustedNetworkTime(earliest, latest) &&
         earliest == 1790265498u && latest == 1790265502u);
  onchip::ClockSnapshot gps;
  assert(onchip::clockSnapshot(gps) && gps.gps_epoch == 1790265500u &&
         gps.roles[0].source == onchip::ClockSource::Gps &&
         gps.roles[1].source == onchip::ClockSource::Gps &&
         gps.roles[2].synchronized);
  assert(!onchip::receiveGpsTime(0));
  onchip::loopClocks();
  assert(onchip::trustedNetworkTime(earliest, latest) && earliest == 1790265498u);
  now += 3600000u;
  onchip::loopClocks();
  assert(!onchip::trustedNetworkTime(earliest, latest));
  now = UINT32_MAX - 1000u;
  onchip::receiveGpsTime(1790265600u);
  onchip::loopClocks();
  now = 1000u;
  onchip::loopClocks();
  assert(onchip::trustedNetworkTime(earliest, latest) && earliest == 1790265600u &&
         latest == 1790265604u);
  char gpsJSON[768];
  assert(onchip::clockSnapshot(gps) && onchip::formatClockJSON(gps, gpsJSON, sizeof(gpsJSON)));
  assert(strstr(gpsJSON, "\"gps_epoch\":1790265600") && strstr(gpsJSON, "\"source\":\"gps\""));
  if (ONCHIP_SNTP_SERVER[0]) {
    onchip::beginClocks();
    assert(onchip::receiveGpsTime(1790265600u));
    onchip::loopClocks();
    now += 10;
    onchip::receiveNetworkTime(1790265700u);
    onchip::loopClocks();
    assert(onchip::trustedNetworkTime(earliest, latest) && earliest == 1790265598u &&
           latest == 1790265602u);
    assert(onchip::companionClock().source() == onchip::ClockSource::Gps);
  }
  char reply[160];
  assert(onchip::networkClockCommand("set sntp.server ntp.example.org", reply, sizeof(reply), false));
  assert(!strncmp(reply, "Error:", 6));
  assert(onchip::networkClockCommand("set sntp.server ntp.example.org", reply, sizeof(reply), true));
  assert(!strncmp(reply, "OK ", 3) && configuredServer == "ntp.example.org");
  assert(onchip::networkClockCommand("set sntp.interval 60", reply, sizeof(reply), true));
  assert(!strncmp(reply, "OK ", 3) && configuredInterval == 60000);
  assert(onchip::networkClockCommand("set sntp.interval 59", reply, sizeof(reply), true));
  assert(!strncmp(reply, "Error:", 6) && configuredInterval == 60000);
  assert(onchip::networkClockCommand("set sntp.server bad host", reply, sizeof(reply), true));
  assert(!strncmp(reply, "Error:", 6) && configuredServer == "ntp.example.org");
  identity_test::failCommit = true;
  assert(onchip::networkClockCommand("set sntp.interval 120", reply, sizeof(reply), true));
  assert(!strncmp(reply, "Error:", 6) && configuredInterval == 60000);
  identity_test::failCommit = false;
  radio_time::settings = {};
  radio_time::settingsLoaded = false;
  onchip::beginNetworkClock();
  assert(configuredServer == "ntp.example.org" && configuredInterval == 60000);
  onchip::beginClocks();
  uint8_t timeRequest[16]{}, response[32]{};
  radio_time::put32(timeRequest, 99);
  timeRequest[4] = radio_time::RequestType; timeRequest[5] = radio_time::ProtocolVersion;
  assert(onchip::networkTimeReply(timeRequest, sizeof(timeRequest), response) && response[7] == 1);
  timeval fresh{1790266000, 0};
  notification(&fresh);
  onchip::loopClocks();
  assert(onchip::networkClockCommand("get sntp.current", reply, sizeof(reply)));
  assert(strstr(reply, "source=sntp"));
  assert(onchip::networkTimeReply(timeRequest, sizeof(timeRequest), response) && response[6] == 1 &&
         !response[7] && radio_time::get32(response + 28) == 120000);
  const auto oldHandler = radio_time::sampleHandler.load();
  const auto oldGeneration = radio_time::generation.load();
  assert(onchip::networkClockCommand("set sntp.server ntp.changed.example.org", reply, sizeof(reply), true));
  oldHandler(1790266000, oldGeneration);
  onchip::loopClocks();
  assert(!onchip::trustedNetworkTime(earliest, latest));
  notification(&fresh);
  onchip::loopClocks();
  assert(onchip::trustedNetworkTime(earliest, latest));
  uint8_t cli[32]{};
  cli[4] = 1 << 2;
  strcpy(reinterpret_cast<char *>(cli + 5), "get sntp.current");
  assert(onchip::publicTimeCommand(cli, sizeof(cli)));
  strcpy(reinterpret_cast<char *>(cli + 5), "set sntp.interval 60");
  assert(!onchip::publicTimeCommand(cli, sizeof(cli)));
  now += 120001;
  onchip::loopClocks();
  assert(!onchip::trustedNetworkTime(earliest, latest));
  assert(onchip::networkTimeReply(timeRequest, sizeof(timeRequest), response) && response[7] == 1);
  assert(onchip::networkClockCommand("set sntp.server off", reply, sizeof(reply), true));
  assert(!strncmp(reply, "OK ", 3));
  assert(!onchip::trustedNetworkTime(earliest, latest));
  identity_test::durable.at({"mc-time", "sntp"}).resize(2);
  radio_time::settings = {};
  radio_time::settingsLoaded = false;
  onchip::beginClocks();
  onchip::beginNetworkClock();
  onchip::loopClocks();
  assert(radio_time::settingsFault && !radio_time::settings.server[0]);
  assert(!onchip::trustedNetworkTime(earliest, latest));
  assert(onchip::networkClockCommand("set sntp.server ntp.example.org", reply, sizeof(reply), true));
  assert(!strncmp(reply, "OK ", 3) && !radio_time::settingsFault);
  puts("PASS actual clock SDK callback, offline configuration and HTTP "
       "status/staleness; GPS preference; saved/live SNTP, IO failure, guest read-only UTC and expiry");
}
