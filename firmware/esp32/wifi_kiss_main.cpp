#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <esp_wifi.h>
#include <target.h>
#include <helpers/ArduinoHelpers.h>
#include <helpers/IdentityStore.h>
#include "KissModem.h"
#include "WifiKissMultiplexer.h"
#include "RadioNetwork.h"
#include "RadioFirmwareIdentity.h"
#include <atomic>
#ifndef MESHCORE_ONCHIP
#include "EspSntpClock.h"
#endif

#if KISS_STREAM_ENDPOINT
#include <HardwareSerial.h>
#ifndef KISS_UART_RX_PIN
#error "Queued UART requires an explicit KISS_UART_RX_PIN"
#endif
#ifndef KISS_UART_TX_PIN
#error "Queued UART requires an explicit KISS_UART_TX_PIN"
#endif
#ifndef KISS_UART_BAUD
#error "Queued UART requires an explicit KISS_UART_BAUD"
#endif
static_assert(KISS_UART_RX_PIN >= 0 && KISS_UART_TX_PIN >= 0 &&
                  KISS_UART_RX_PIN != KISS_UART_TX_PIN,
              "Queued UART requires distinct physical RX/TX pins");
static_assert(KISS_UART_BAUD >= 9600 && KISS_UART_BAUD <= 1000000,
              "Queued UART baud must be 9600..1000000");
#if ENV_INCLUDE_GPS
static HardwareSerial queued_uart(2);
#else
static HardwareSerial queued_uart(1);
#endif
#endif

#include <SPIFFS.h>
#ifdef MESHCORE_ONCHIP
#include <helpers/sensors/GpsTime.h>
#include "onchip/Config.h"
#include "onchip/Runtime.h"
#include "onchip/Clock.h"
#include "onchip/CompanionSessions.h"
#if defined(MESHCORE_ONCHIP_BOT) && MESHCORE_ONCHIP_BOT
#include "onchip/CommandBot.h"
#endif
#if defined(MESHCORE_MAST_ADMIN) && MESHCORE_MAST_ADMIN
#include "onchip/MastAdmin.h"
#include "onchip/MastWeb.h"
#include "onchip/EspFieldUpdate.h"
#include "onchip/TelemetryService.h"
#endif
#endif
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>

#ifndef WIFI_SSID
#error "WIFI_SSID must be defined"
#endif
#ifndef WIFI_PWD
#error "WIFI_PWD must be defined"
#endif
#ifndef KISS_WIFI_TX_POWER
#define KISS_WIFI_TX_POWER WIFI_POWER_8_5dBm
#endif
#ifndef LORA_CR
#define LORA_CR 5
#endif

#ifdef MESHCORE_ONCHIP
#include "onchip/PhyConfig.h"
// Native crypto and observer callbacks share the radio loop's stack.
SET_LOOP_TASK_STACK_SIZE(16384)
#endif

#define NOISE_FLOOR_CALIB_INTERVAL_MS 2000

StdRNG rng;
mesh::LocalIdentity identity;
WiFiServer kiss_server(KISS_TCP_PORT);
WifiKissMultiplexer kiss_stream;
#ifdef MESHCORE_ONCHIP
static RadioDashboard* dashboard_storage;
RadioDashboard& getDashboard() { return *dashboard_storage; }
#else
RadioDashboard dashboard;
RadioDashboard& getDashboard() { return dashboard; }
#endif
KissModem* modem;

static uint32_t next_noise_floor_calib_ms;
static uint32_t next_agc_reset_ms;
static radio_network::WifiRecovery& wifi_recovery = radio_network::wifiRecovery();
static std::atomic<uint32_t> wifi_loss_generation{0};
static bool wifi_was_connected;
static bool http_ready;
static bool discovery_ready;
static uint32_t next_network_service_ms;
static uint32_t last_dashboard_publish_ms;
static bool wifi_join_enabled = true;
static bool wifi_initialized;
static bool roles_ready;
static bool dispatch_watched;
static bool wifi_configuration_valid = true;

void publishDashboard() {
  RadioDashboard::RadioStatus status;
  kiss_stream.dashboardStatus(status);
  status.wifi_connected = wifi_was_connected;
  if (status.wifi_connected) status.wifi_rssi = WiFi.RSSI();
  status.free_heap = ESP.getFreeHeap();
  status.minimum_heap = ESP.getMinFreeHeap();
  constexpr unsigned dma_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT;
  status.dma_free_heap = heap_caps_get_free_size(dma_caps);
  status.dma_largest_heap = heap_caps_get_largest_free_block(dma_caps);
  status.dma_minimum_heap = heap_caps_get_minimum_free_size(dma_caps);
  status.rx_errors = radio_driver.getPacketsRecvErrors();
#ifdef MESHCORE_ONCHIP
  onchip::dashboardStatus(status, bool(kiss_server));
#endif
  getDashboard().publish(millis(), status);
#if defined(MESHCORE_MAST_ADMIN) && MESHCORE_MAST_ADMIN && defined(ONCHIP_BOT_HTTPS) && ONCHIP_BOT_HTTPS
  onchip::telemetryLoop(getDashboard(), status, kiss_stream, board);
#endif
}

void halt() {
#if defined(MESHCORE_MAST_ADMIN) && MESHCORE_MAST_ADMIN
  onchip::espUpdateRecovery("Startup failed; inspect USB diagnostics and restore retained configuration");
  onchip::serviceMastWebRecovery();
  // Retain storage and identity. A signed application can still be uploaded
  // when startup failed; missing/corrupt configuration needs operator repair.
  while (true) {
    if (dispatch_watched) esp_task_wdt_reset();
    static uint32_t nextRetry;
    if (wifi_initialized && wifi_join_enabled && WiFi.status() != WL_CONNECTED &&
        int32_t(millis() - nextRetry) >= 0) {
      WiFi.reconnect();
      nextRetry = millis() + 30000;
    }
    if (wifi_initialized && WiFi.status() == WL_CONNECTED && dashboard_storage && !http_ready)
      http_ready = getDashboard().beginHTTP();
    onchip::serviceEspUpdate(false, false, wifi_join_enabled);
    delay(100);
  }
#else
  while (true) delay(1000);
#endif
}

void loadOrCreateIdentity() {
#ifdef MESHCORE_ONCHIP
  if (!SPIFFS.begin(false) || !onchip::loadIdentity("modem", identity)) {
    Serial.println("Shared storage unavailable; refusing automatic format");
    halt();
  }
#else
  SPIFFS.begin(true);
  IdentityStore store(SPIFFS, "/identity");
  if (!store.load("_main", identity)) {
    identity = radio_new_identity();
    while (identity.pub_key[0] == 0x00 || identity.pub_key[0] == 0xFF) {
      identity = radio_new_identity();
    }
    store.save("_main", identity);
  }
#endif
}

void onSetRadio(float freq, float bw, uint8_t sf, uint8_t cr) {
  radio_driver.setParams(freq, bw, sf, cr);
}

void onSetTxPower(uint8_t power) {
  radio_driver.setTxPower(power);
}

float onGetCurrentRssi() {
  return radio_driver.getCurrentRSSI();
}

void onGetStats(uint32_t* rx, uint32_t* tx, uint32_t* errors) {
  *rx = radio_driver.getPacketsRecv();
  *tx = radio_driver.getPacketsSent();
  *errors = radio_driver.getPacketsRecvErrors();
}

bool radio_network::startStation() {
  if (!WiFi.mode(WIFI_STA)) {
    Serial.println("WiFi station initialization failed"); return false;
  }
  const esp_err_t error = esp_wifi_set_max_tx_power(KISS_WIFI_TX_POWER);
  if (error != ESP_OK) {
    Serial.printf("WiFi transmit power configuration failed: %s\n", esp_err_to_name(error));
    return false;
  }
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(false);
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
  return true;
}

void connectWifi() {
  WiFi.persistent(false);
  if (!WiFi.setHostname(radio_network::hostname) ||
      strcmp(WiFi.getHostname(), radio_network::hostname) != 0) {
    Serial.println("WiFi hostname configuration failed");
    halt();
  }
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
#ifdef MESHCORE_ONCHIP
    char message[80];
#endif
    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
      wifi_loss_generation.fetch_add(1, std::memory_order_relaxed);
#ifdef MESHCORE_ONCHIP
      snprintf(message, sizeof(message), "WiFi association lost: reason %u", info.wifi_sta_disconnected.reason);
      onchip::diagnosticEvent(message);
#else
      Serial.printf("WiFi association lost: reason %u\n",
                    info.wifi_sta_disconnected.reason);
#endif
    } else if (event == ARDUINO_EVENT_WIFI_STA_LOST_IP) {
      wifi_loss_generation.fetch_add(1, std::memory_order_relaxed);
#ifdef MESHCORE_ONCHIP
      onchip::diagnosticEvent("WiFi lost IP address");
#endif
    } else if (event == ARDUINO_EVENT_WIFI_STA_CONNECTED) {
#ifdef MESHCORE_ONCHIP
      snprintf(message, sizeof(message), "WiFi associated: channel %u, AP auth mode %u",
               info.wifi_sta_connected.channel, info.wifi_sta_connected.authmode);
      onchip::diagnosticEvent(message);
#else
      Serial.printf("WiFi associated: channel %u, AP auth mode %u\n",
                    info.wifi_sta_connected.channel,
                    info.wifi_sta_connected.authmode);
#endif
    } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
#ifdef MESHCORE_ONCHIP
      onchip::diagnosticEvent("WiFi IP ready");
#endif
    }
  });
  bool stationReady = false;
  for (unsigned attempt = 0; attempt < 5 && !stationReady; ++attempt) {
    stationReady = radio_network::startStation();
    if (!stationReady) delay(1000u << attempt);
    if (dispatch_watched) esp_task_wdt_reset();
  }
  if (!stationReady) halt();
  wifi_initialized = true;
  bool join_enabled = true;
#if defined(MESHCORE_MAST_ADMIN) && MESHCORE_MAST_ADMIN
  onchip::MastAdmin::WifiCredentials credentials;
  bool saved = false;
  if (!onchip::MastAdmin::loadWifiEnabled(join_enabled)) {
    Serial.println("Saved WiFi enable setting invalid; WiFi join disabled; use native mast recovery");
    join_enabled = false;
    wifi_configuration_valid = false;
  } else if (!join_enabled) {
    if (!WiFi.mode(WIFI_OFF)) {
      Serial.println("Saved WiFi disable could not be applied"); halt();
    }
    Serial.println("WiFi disabled by saved Management setting; RF roles remain active");
  } else if (!onchip::MastAdmin::loadWifi(credentials, saved)) {
    Serial.println("Saved WiFi credentials invalid; WiFi join disabled; use native mast recovery");
    join_enabled = false;
    wifi_configuration_valid = false;
  } else if (saved || WIFI_SSID[0])
    WiFi.begin(saved ? credentials.ssid : WIFI_SSID,
               saved ? credentials.password : WIFI_PWD);
  else
    WiFi.begin(); // Preserve SDK-saved station credentials, when provisioned.
#else
  WiFi.begin(WIFI_SSID, WIFI_PWD);
#endif
  wifi_recovery.begin(millis(), join_enabled);
  wifi_join_enabled = join_enabled;
}

bool startDiscovery(bool http_ready) {
  if (!MDNS.begin(radio_network::hostname) ||
      !MDNS.addService("kiss", "tcp", KISS_TCP_PORT) ||
      (http_ready && !MDNS.addService("http", "tcp", KISS_HTTP_PORT))) {
    Serial.println("mDNS discovery unavailable; radio services remain reachable by IP");
    MDNS.end();
    return false;
  }
  Serial.printf("mDNS hostname: %s.local; KISS TCP port %u\n",
                radio_network::hostname, KISS_TCP_PORT);
  if (http_ready)
    Serial.printf("Dashboard: http://%s.local:%u/\n",
                  radio_network::hostname, KISS_HTTP_PORT);
  return true;
}

void serviceWifi() {
  const uint32_t now = millis();
  const auto bits = WiFi.getStatusBits();
  const auto actions = wifi_recovery.update(
      now, bits & STA_CONNECTED_BIT, bits & STA_HAS_IP_BIT,
      uint32_t(WiFi.localIP()),
      wifi_loss_generation.load(std::memory_order_relaxed));
  wifi_was_connected = wifi_recovery.ready();
  if (actions.down) {
    Serial.println("WiFi IP connection lost; TCP clients must reconnect");
    kiss_stream.disconnectTcpClients();
    kiss_server.end();
    MDNS.end();
    discovery_ready = false;
#ifdef MESHCORE_ONCHIP
    onchip::companionSessions().setNetworkAvailable(false);
#endif
  }
  if (actions.up) {
    Serial.printf("WiFi connected: %s, RSSI %d\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
    next_network_service_ms = now;
#ifdef MESHCORE_ONCHIP
    onchip::companionSessions().setNetworkAvailable(true);
#endif
  }

  if (actions.retry) {
    Serial.printf("WiFi reconnecting (status %d, free heap %u, minimum %u)\n",
                  WiFi.status(), ESP.getFreeHeap(), ESP.getMinFreeHeap());
    if (!WiFi.reconnect()) Serial.println("WiFi reconnect request failed");
  }

  if (wifi_was_connected && int32_t(now - next_network_service_ms) >= 0) {
    if (!kiss_server) {
      kiss_server.begin();
      kiss_server.setNoDelay(true);
    }
    // IDF HTTP and companion listeners bind INADDR_ANY and survive an IP
    // change. Keep their workers and RF-side state; retry a failed HTTP start.
    if (!http_ready && getDashboard().beginHTTP()) {
      http_ready = true;
      MDNS.end();
      discovery_ready = false;
    }
    if (!discovery_ready) discovery_ready = startDiscovery(http_ready);
    next_network_service_ms = now + radio_network::WifiRecovery::RetryMs;
  }
  if (wifi_was_connected) kiss_stream.poll(kiss_server);
}

void setup() {
  Serial.begin(115200);
  Serial.printf("Firmware: %s\n", radio_firmware::version);
#if defined(MESHCORE_MAST_ADMIN) && MESHCORE_MAST_ADMIN
  // Cover startup as well as dispatch; flash streaming uses the HTTP worker.
  if (esp_task_wdt_init(60, true) != ESP_OK || esp_task_wdt_add(nullptr) != ESP_OK) {
    Serial.println("Dispatch watchdog initialization failed; recovery mode");
    halt();
  }
  dispatch_watched = true;
#endif
#if defined(MESHCORE_PUBLIC_PROVISIONING) && MESHCORE_PUBLIC_PROVISIONING
  if (!onchip::beginPublicProvisioning()) halt();
#endif
  board.begin();
  board.setInhibitSleep(true);
  if (dispatch_watched) esp_task_wdt_reset();

#ifdef MESHCORE_ONCHIP
  void* memory = heap_caps_malloc(sizeof(RadioDashboard), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!memory) {
    Serial.println("Combined firmware requires PSRAM for the bounded dashboard");
    halt();
  }
  dashboard_storage = new (memory) RadioDashboard();
#endif
#ifdef MESHCORE_ONCHIP
#if defined(MESHCORE_MAST_ADMIN) && MESHCORE_MAST_ADMIN
  if (!onchip::beginDiagnostics())
    Serial.println("Diagnostics worker unavailable; WiFi event logs will be dropped");
#endif
  // Start the ESP entropy source before creating durable role identities.
  connectWifi();
#endif
  bool radioReady = false;
  for (unsigned attempt = 0; attempt < 5 && !radioReady; ++attempt) {
    radioReady = radio_init();
    if (!radioReady) {
      Serial.println("Radio initialization failed; retrying");
      delay(1000u << attempt);
    }
    if (dispatch_watched) esp_task_wdt_reset();
  }
  if (!radioReady) {
    Serial.println("Radio initialization failed after five attempts; recovery mode");
    halt();
  }
  radio_driver.begin();
  if (dispatch_watched) esp_task_wdt_reset();
  rng.begin(radio_driver.getRngSeed());
  loadOrCreateIdentity();
  if (dispatch_watched) esp_task_wdt_reset();
  sensors.begin();
#ifndef MESHCORE_ONCHIP
  radio_time::start();
#endif
#if ENV_INCLUDE_GPS && defined(MESHCORE_ONCHIP)
  meshcore::gpsTimeHandler() = [](uint32_t utc) {
    if (!onchip::receiveGpsTime(utc)) return;
#if defined(MESHCORE_ONCHIP_BOT) && MESHCORE_ONCHIP_BOT
    onchip::commandBotService().synchronizeTime(utc, onchip::ClockSource::Gps);
#endif
  };
#endif

  modem = new KissModem(kiss_stream, identity, rng, radio_driver, board, sensors);
  modem->setRadioCallback(onSetRadio);
  modem->setTxPowerCallback(onSetTxPower);
  modem->setGetCurrentRssiCallback(onGetCurrentRssi);
  modem->setGetStatsCallback(onGetStats);
  modem->begin();
  if (dispatch_watched) esp_task_wdt_reset();
#if defined(USE_SX1262) || defined(USE_SX1268) || defined(USE_LR2021)
  kiss_stream.attachRadio(radio_driver, rng, onSetRadio, onSetTxPower,
                          [](bool enabled) { return radio_driver.setRxBoostedGainMode(enabled); },
                          []() { return radio_driver.getRxBoostedGainMode(); });
#else
  kiss_stream.attachRadio(radio_driver, rng, onSetRadio, onSetTxPower);
#endif
#ifdef MESHCORE_ONCHIP
  RadioConfig startup_radio = {
      static_cast<uint32_t>(ONCHIP_RADIO_FREQ_MHZ * 1000000.0 + 0.5),
      static_cast<uint32_t>(ONCHIP_RADIO_BW_KHZ * 1000.0 + 0.5), ONCHIP_RADIO_SF,
      ONCHIP_RADIO_CR, ONCHIP_RADIO_TX_POWER};
#if defined(MESHCORE_PUBLIC_PROVISIONING) && MESHCORE_PUBLIC_PROVISIONING
  if (!onchip::loadPublicInitialRadio(startup_radio)) halt();
#endif
  const bool profile_ready = kiss_stream.setInitialConfiguration(startup_radio, true);
#else
  const RadioConfig startup_radio = {
      static_cast<uint32_t>(LORA_FREQ * 1000000.0),
      static_cast<uint32_t>(LORA_BW * 1000.0), LORA_SF, LORA_CR, LORA_TX_POWER};
  const bool profile_ready = kiss_stream.setInitialConfiguration(startup_radio);
#endif
  if (!profile_ready) {
    Serial.println("PHY profile restore failed; radio service disabled");
    halt();
  }
#if KISS_STREAM_ENDPOINT
  if (queued_uart.setRxBufferSize(1024) != 1024 ||
      queued_uart.setTxBufferSize(512) != 512) {
    Serial.println("Queued UART buffer configuration failed");
    halt();
  }
  queued_uart.begin(KISS_UART_BAUD, SERIAL_8N1, KISS_UART_RX_PIN, KISS_UART_TX_PIN);
  if (!queued_uart || !kiss_stream.attachStream(queued_uart)) {
    Serial.println("Queued UART startup failed");
    halt();
  }
  Serial.printf("Queued UART1: RX %d, TX %d, baud %u; diagnostic USB is separate\n",
                KISS_UART_RX_PIN, KISS_UART_TX_PIN, unsigned(KISS_UART_BAUD));
#endif
  kiss_stream.observeWith(getDashboard());

#ifndef MESHCORE_ONCHIP
  connectWifi();
#endif
#ifdef MESHCORE_ONCHIP
  if (!onchip::begin(kiss_stream, identity)) {
    Serial.println("On-chip startup failed");
    halt();
  }
  roles_ready = true;
  if (dispatch_watched) esp_task_wdt_reset();
#endif
  // Network listeners/discovery start after DHCP, never block the RF loop.
#ifdef MESHCORE_ONCHIP
  onchip::companionSessions().setNetworkAvailable(false);
#endif
#if ENV_INCLUDE_GPS
  sensors.setSettingValue("gps", "1");
#endif
  publishDashboard();
  board.onBootComplete();
#if defined(MESHCORE_MAST_ADMIN) && MESHCORE_MAST_ADMIN
  esp_task_wdt_reset();
#endif
}

void loop() {
#ifdef MESHCORE_ONCHIP
  const uint32_t loopStarted = micros();
#endif
  kiss_stream.serviceTransmit();
#if KISS_STREAM_ENDPOINT
  kiss_stream.pollStream();
#endif
  serviceWifi();
#ifndef MESHCORE_ONCHIP
  radio_time::poll();
#endif
  static char timeCommand[96]{};
  static unsigned timeCommandLength = 0;
  static bool timeCommandOverflow = false;
  unsigned timeCommandBudget = 64;
  while (timeCommandBudget-- && Serial.available()) {
    const char c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      char reply[160];
      timeCommand[timeCommandLength] = 0;
      if (timeCommandOverflow) Serial.println("Error: USB time command exceeds 95 bytes");
#ifdef MESHCORE_ONCHIP
      else if (onchip::networkClockCommand(timeCommand, reply, sizeof(reply), true)) Serial.println(reply);
#else
      else if (!strcmp(timeCommand, "get sntp.current")) {
        uint32_t lower, upper;
        if (radio_time::bounds(lower, upper))
          snprintf(reply, sizeof(reply), "utc=%u..%u source=sntp", unsigned(lower), unsigned(upper));
        else strcpy(reply, "Error: SNTP unsynchronized or expired");
        Serial.println(reply);
      } else if (radio_time::configCommand(timeCommand, reply, sizeof(reply), true)) Serial.println(reply);
#endif
      else if (timeCommandLength) Serial.println("Error: use get sntp.current|server|interval; set sntp.server HOST; set sntp.interval SECONDS");
      timeCommandLength = 0; timeCommandOverflow = false;
    } else if (timeCommandLength + 1 < sizeof(timeCommand))
      timeCommand[timeCommandLength++] = c;
    else timeCommandOverflow = true;
  }
  sensors.loop();
  rtc_clock.tick();
#ifdef MESHCORE_ONCHIP
  onchip::loop();
#endif
  modem->loop();
  kiss_stream.afterModemLoop();

  if (!kiss_stream.isActuallyTransmitting()
#ifndef MESHCORE_ONCHIP
      && !modem->isHostOutputBackedUp()
#endif
      ) {
    const uint32_t agcInterval = uint32_t(kiss_stream.agcResetIntervalSeconds()) * 1000;
    if (agcInterval && !kiss_stream.hasPendingTransmit() &&
        (uint32_t)(millis() - next_agc_reset_ms) >= agcInterval) {
      radio_driver.resetAGC();
      next_agc_reset_ms = millis();
    }

    uint8_t rx_buf[256];
    int rx_len = radio_driver.recvRaw(rx_buf, sizeof(rx_buf));
    if (rx_len > 0) {
      const float snr = radio_driver.getLastSNR();
      const float rssi = radio_driver.getLastRSSI();
      getDashboard().received(millis(), rx_buf, rx_len, rssi, snr,
                         radio_driver.getEstAirtimeFor(rx_len));
      kiss_stream.received(rx_buf, rx_len, rssi, snr);
      if (KISS_STREAM_ENDPOINT || kiss_stream.clientCount() > 0)
        modem->onPacketReceived((int8_t)(snr * 4), (int8_t)rssi, rx_buf, rx_len);
    }
  }

  if ((uint32_t)(millis() - next_noise_floor_calib_ms) >=
      NOISE_FLOOR_CALIB_INTERVAL_MS) {
    radio_driver.triggerNoiseFloorCalibrate(kiss_stream.interferenceThreshold());
    next_noise_floor_calib_ms = millis();
  }
  radio_driver.loop();
#if defined(MESHCORE_MAST_ADMIN) && MESHCORE_MAST_ADMIN
  bool nativeRolesReady = roles_ready;
  for (unsigned role = 0; role < 3; ++role) {
    const auto phase = onchip::rolePhase(static_cast<onchip::Role>(role));
    nativeRolesReady = nativeRolesReady &&
      (phase == onchip::RolePhase::Running || phase == onchip::RolePhase::Disabled);
  }
  const auto *admin = onchip::MastAdmin::service();
  nativeRolesReady = nativeRolesReady && admin && admin->ready() && wifi_configuration_valid;
  nativeRolesReady = nativeRolesReady && (!wifi_join_enabled || WiFi.getMode() != WIFI_OFF) &&
    (!wifi_was_connected || http_ready);
  nativeRolesReady = nativeRolesReady && !onchip::companionSessions().stats().nativeFault;
#if defined(MESHCORE_ONCHIP_BOT) && MESHCORE_ONCHIP_BOT
  RadioDashboard::RoleStatus botStatus;
  onchip::commandBotService().dashboardStatus(botStatus);
  nativeRolesReady = nativeRolesReady &&
    onchip::commandBotService().sourceDeploymentReady() &&
    (!strcmp(botStatus.state, "disabled") || (botStatus.ready && !botStatus.fault[0]));
#endif
  onchip::serviceEspUpdate(nativeRolesReady, wifi_was_connected && http_ready,
                          wifi_join_enabled);
  esp_task_wdt_reset();
#endif
  if ((uint32_t)(millis() - last_dashboard_publish_ms) >= 500) {
    publishDashboard();
    last_dashboard_publish_ms = millis();
  }

  // Yield to the WiFi/lwIP tasks. Without this the busy loop starves the
  // network stack and the node stops answering ARP within ~30 s.
#ifdef MESHCORE_ONCHIP
  onchip::diagnosticLoopSample(loopStarted, micros());
#endif
  delay(1);
}
