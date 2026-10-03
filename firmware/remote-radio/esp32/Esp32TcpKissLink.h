#pragma once

#if defined(ARDUINO_ARCH_ESP32)

#include "RemoteKissRadio.h"
#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <stdlib.h>

#if !defined(KISS_HOSTNAME) && !defined(WIFI_HOSTNAME) && !defined(HOSTNAME)
#define KISS_HOSTNAME "meshcore-phyless"
#endif
#include "RadioNetwork.h"

#ifndef KISS_WIFI_TX_POWER
#define KISS_WIFI_TX_POWER WIFI_POWER_8_5dBm
#endif

/** Reconnecting Stream used by RemoteKissRadio on a PHY-less ESP32. */
class Esp32TcpKissLink : public Stream {
public:
  Esp32TcpKissLink(const char *ssid, const char *password,
                   const char *modem_host, uint16_t modem_port = 8001)
      : _ssid(ssid), _password(password), _host(modem_host), _port(modem_port),
        _next_attempt(0), _connected_event(false) {}

  void begin() {
    if (_started)
      return;
    WiFi.persistent(false);
    if (!WiFi.setHostname(radio_network::hostname) ||
        !WiFi.getHostname() ||
        strcmp(WiFi.getHostname(), radio_network::hostname) != 0) {
      RemoteKissDiagnostics::record(RemoteKissDiagnostics::WiFi,
                                    "PHY-less WiFi hostname configuration failed");
      abort();
    }
    if (!WiFi.mode(WIFI_STA)) {
      RemoteKissDiagnostics::record(RemoteKissDiagnostics::WiFi,
                                    "PHY-less WiFi station initialization failed");
      abort();
    }
    // Match the physical XIAO's association-safe, operator-overridable power.
    const esp_err_t error = esp_wifi_set_max_tx_power(KISS_WIFI_TX_POWER);
    if (error != ESP_OK) {
      RemoteKissDiagnostics::record(RemoteKissDiagnostics::WiFi,
                                    "PHY-less WiFi transmit power configuration failed", error);
      abort();
    }
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(true);
    WiFi.begin(_ssid, _password);
    _started = true;
  }

  void loop() {
    if (_was_connected && !_client.connected()) {
      _was_connected = false;
      _disconnected_event = true;
    }
    if (_client.connected() || WiFi.status() != WL_CONNECTED)
      return;
    if (static_cast<int32_t>(millis() - _next_attempt) < 0)
      return;
    _client.stop();
    if (_client.connect(_host, _port, 1000)) {
      _client.setNoDelay(true);
      _client.setTimeout(1000);
      _connected_event = true;
      _was_connected = true;
      _backoff = 2000;
    } else {
      _backoff = _backoff < 15000 ? _backoff * 2 : 30000;
    }
    _next_attempt = millis() + _backoff;
  }
  void retire() {
    _client.stop();
    _was_connected = false;
    _disconnected_event = true;
    _next_attempt = millis() + 30000;
  }
  bool takeDisconnectedEvent() {
    const bool event = _disconnected_event;
    _disconnected_event = false;
    return event;
  }

  bool takeConnectedEvent() {
    const bool connected = _connected_event;
    _connected_event = false;
    return connected;
  }

  bool connected() { return _client.connected(); }
  int available() override { return _client.available(); }
  int read() override { return _client.read(); }
  int peek() override { return _client.peek(); }
  void flush() override { _client.flush(); }
  int availableForWrite() override { return _client.connected() ? 1460 : 0; }
  size_t write(uint8_t byte) override { return _client.write(byte); }
  size_t write(const uint8_t *data, size_t length) override {
    return _client.write(data, length);
  }

private:
  const char *_ssid;
  const char *_password;
  const char *_host;
  uint16_t _port;
  WiFiClient _client;
  uint32_t _next_attempt;
  bool _connected_event;
  bool _started = false, _was_connected = false, _disconnected_event = false;
  uint32_t _backoff = 2000;
};

class Esp32RemoteKissRadio : public RemoteKissRadio {
public:
  explicit Esp32RemoteKissRadio(Esp32TcpKissLink &link)
      : RemoteKissRadio(link), _link(link) {}

  void begin() override { _link.begin(); }

  void loop() override {
    _link.loop();
    if (_link.takeDisconnectedEvent())
      onLinkDisconnected();
    if (_link.takeConnectedEvent())
      onLinkConnected();
    RemoteKissRadio::loop();
    if (_link.connected() && linkFault())
      _link.retire();
  }

private:
  Esp32TcpKissLink &_link;
};

#endif
