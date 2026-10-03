#include <Arduino.h>
#include "target.h"

#ifndef KISS_MODEM_HOST
#error "KISS_MODEM_HOST must be defined"
#endif
#ifndef WIFI_SSID
#error "WIFI_SSID must be defined"
#endif
#ifndef WIFI_PWD
#error "WIFI_PWD must be defined"
#endif
#ifndef KISS_TCP_PORT
#define KISS_TCP_PORT 8001
#endif

#ifdef DISPLAY_CLASS
DISPLAY_CLASS display;
MomentaryButton user_btn(PIN_USER_BTN, 1000, true);
#endif

XiaoS3Board board;
Esp32TcpKissLink kiss_link(WIFI_SSID, WIFI_PWD, KISS_MODEM_HOST, KISS_TCP_PORT);
Esp32RemoteKissRadio radio_driver(kiss_link);
ESP32RTCClock fallback_clock;
AutoDiscoverRTCClock rtc_clock(fallback_clock);
EnvironmentSensorManager sensors;

bool radio_init() {
  fallback_clock.begin();
  rtc_clock.begin(Wire);
  radio_driver.begin();
  return true;
}

mesh::LocalIdentity radio_new_identity() {
  return RemoteKissRadio::newIdentity();
}
