#include "RadioNetwork.h"
#include <atomic>
#include <cassert>
#include <cstdio>
#include <string>

constexpr unsigned STA_CONNECTED_BIT = 1, STA_HAS_IP_BIT = 2;
using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0, ESP_FAIL = -1;
static unsigned stops, starts;
static esp_err_t stopResult = ESP_OK, startResult = ESP_OK, disconnectResult = ESP_OK;
const char* esp_err_to_name(esp_err_t result) { return result == ESP_OK ? "ESP_OK" : "ESP_FAIL"; }
static uint32_t clock_ms;
unsigned long millis() { return clock_ms; }
struct {
  template<class... Args> void printf(const char*, Args...) {}
  void println(const char*) {}
} Serial;
struct {
  unsigned getFreeHeap() { return 100000; }
  unsigned getMinFreeHeap() { return 90000; }
} ESP;
struct IP {
  uint32_t value;
  operator uint32_t() const { return value; }
  std::string toString() const { return "192.0.2.1"; }
};
struct {
  unsigned bits = 0, retries = 0;
  uint32_t address = 0;
  bool retryResult = true;
  unsigned getStatusBits() { return bits; }
  IP localIP() { return {address}; }
  int RSSI() { return -50; }
  int status() { return 3; }
} WiFi;
esp_err_t esp_wifi_disconnect() { return disconnectResult; }
esp_err_t esp_wifi_connect() { ++WiFi.retries; return WiFi.retryResult ? ESP_OK : ESP_FAIL; }
esp_err_t esp_wifi_stop() { ++stops; return stopResult; }
esp_err_t esp_wifi_start() { ++starts; return startResult; }
struct {
  unsigned starts = 0, stops = 0;
  bool listening = false, failStart = false;
  operator bool() const { return listening; }
  void begin() { ++starts; listening = !failStart; }
  void end() { ++stops; listening = false; }
  void setNoDelay(bool) {}
} kiss_server;
struct {
  unsigned polls = 0, retired = 0;
  template<class Server> void poll(Server&) { ++polls; }
  void disconnectTcpClients() { ++retired; }
} kiss_stream;
struct {
  unsigned stops = 0;
  void end() { ++stops; }
} MDNS;
struct Dashboard {
  unsigned starts = 0;
  bool failStart = false;
  bool beginHTTP() { ++starts; return !failStart; }
} dashboard;
Dashboard& getDashboard() { return dashboard; }
namespace onchip {
enum class DiagnosticSubsystem { Wifi };
void diagnosticEvent(const char*, DiagnosticSubsystem) {}
struct Sessions {
  bool available = false;
  unsigned retired = 0;
  void setNetworkAvailable(bool value) {
    available = value;
    if (!value) ++retired;
  }
};
Sessions& companionSessions() { static Sessions sessions; return sessions; }
}
static radio_network::WifiRecovery& wifi_recovery = radio_network::wifiRecovery();
static std::atomic<uint32_t> wifi_loss_generation{0};
static bool wifi_was_connected, http_ready, discovery_ready;
static uint32_t next_network_service_ms;
static unsigned discoveries;
static bool failDiscovery;
bool startDiscovery(bool http) {
  assert(http == http_ready);
  ++discoveries;
  return !failDiscovery;
}

// Extracted from firmware/esp32/wifi_kiss_main.cpp by the Make target.
#include "wifi-service.inc"

int main() {
  wifi_recovery.begin(0, true);
  clock_ms = 30000;
  WiFi.retryResult = false;
  serviceWifi();
  assert(WiFi.retries == 1 && !kiss_server && !http_ready);
  clock_ms = 30001;
  serviceWifi();
  assert(WiFi.retries == 1);
  WiFi.bits = STA_CONNECTED_BIT;
  clock_ms = 31000;
  serviceWifi();
  clock_ms = 150999;
  serviceWifi();
  assert(WiFi.retries == 1 && !wifi_was_connected);
  clock_ms = 151000;
  serviceWifi();
  assert(WiFi.retries == 2);
  WiFi.bits |= STA_HAS_IP_BIT;
  WiFi.address = 1;
  dashboard.failStart = failDiscovery = true;
  clock_ms++;
  serviceWifi();
  assert(wifi_was_connected && kiss_server && !http_ready && !discovery_ready);
  assert(kiss_stream.polls == 1 && onchip::companionSessions().available);
  dashboard.failStart = failDiscovery = false;
  clock_ms += radio_network::WifiRecovery::RetryMs;
  serviceWifi();
  assert(http_ready && discovery_ready && dashboard.starts == 2);
  WiFi.bits &= ~STA_HAS_IP_BIT;
  ++wifi_loss_generation;
  clock_ms++;
  serviceWifi();
  assert(!wifi_was_connected && !kiss_server && !discovery_ready);
  assert(kiss_stream.retired == 1 && !onchip::companionSessions().available);
  assert(http_ready && dashboard.starts == 2);
  clock_ms += 120000;
  serviceWifi();
  assert(WiFi.retries == 3);
  WiFi.bits |= STA_HAS_IP_BIT;
  WiFi.address = 2;
  kiss_server.failStart = true;
  failDiscovery = true;
  serviceWifi();
  assert(wifi_was_connected && !kiss_server && !discovery_ready);
  kiss_server.failStart = failDiscovery = false;
  clock_ms += 29999;
  serviceWifi();
  assert(!kiss_server && !discovery_ready);
  clock_ms++;
  serviceWifi();
  assert(kiss_server && discovery_ready && dashboard.starts == 2);
  ++wifi_loss_generation; // Same IP, disconnect/rejoin happened between passes.
  serviceWifi();
  assert(kiss_stream.retired == 2 && kiss_server.stops == 2);
  assert(onchip::companionSessions().retired == 2 && onchip::companionSessions().available);
  assert(kiss_server && dashboard.starts == 2 && discovery_ready);
  WiFi.address = 3;
  serviceWifi();
  assert(kiss_stream.retired == 3 && kiss_server && discovery_ready);
  // AP outage and failed SDK calls: independent connect, bounded restart,
  // preserved HTTP worker, then normal listener recovery.
  WiFi.bits = 0;
  WiFi.address = 0;
  ++wifi_loss_generation;
  serviceWifi();
  const unsigned before = WiFi.retries;
  disconnectResult = ESP_FAIL;
  WiFi.retryResult = false;
  clock_ms += 30000;
  serviceWifi();
  assert(WiFi.retries == before + 1 && stops == 0);
  clock_ms += 30000;
  serviceWifi();
  assert(WiFi.retries == before + 2 && stops == 0);
  clock_ms += 59999;
  serviceWifi();
  assert(WiFi.retries == before + 2);
  clock_ms++;
  stopResult = startResult = ESP_FAIL;
  serviceWifi();
  assert(stops == 1 && starts == 1 && WiFi.retries == before + 2);
  clock_ms += 120000;
  startResult = ESP_OK;
  serviceWifi();
  assert(stops == 2 && starts == 2 && WiFi.retries == before + 3);
  clock_ms += 120000;
  WiFi.retryResult = true;
  stopResult = ESP_OK;
  serviceWifi();
  assert(stops == 3 && starts == 3 && WiFi.retries == before + 4);
  assert(!wifi_was_connected && !onchip::companionSessions().available);
  WiFi.bits = STA_CONNECTED_BIT | STA_HAS_IP_BIT;
  WiFi.address = 4;
  serviceWifi();
  assert(wifi_was_connected && kiss_server && discovery_ready && dashboard.starts == 2);
  assert(wifi_recovery.attempts() == 0);
  wifi_recovery.begin(clock_ms, false);
  serviceWifi(); // Disable must retire sockets even before SDK bits clear.
  assert(!wifi_was_connected && !kiss_server);
  clock_ms += 1000000;
  serviceWifi();
  assert(WiFi.retries == before + 4 && stops == 3);
  std::puts("Production WiFi SDK seam: DHCP/lost-IP, failed retries/listeners/discovery and same-IP rejoin passed");
}
