// SPDX-License-Identifier: Apache-2.0
#include <helpers/sensors/MicroNMEALocationProvider.h>
#include "../../nrf52840/PineTrustedTime.h"
#include <cassert>
#include <deque>
#include <string>
#include <cstdio>

static unsigned long now;
static unsigned updates;
static uint32_t lastUtc;
static nrfmast::TrustedTimeSample trusted;
unsigned long millis() { return now; }
void delay(unsigned long value) { now += value; }
void pinMode(int, int) {}
void digitalWrite(int, int) {}
int digitalRead(int) { return HIGH; }
void LocationProvider::sendSentence(const char*) {}
struct SerialInput : Stream {
  std::deque<char> input;
  int available() override { return input.size(); }
  int read() override { const char c = input.front(); input.pop_front(); return c; }
  size_t write(const uint8_t* bytes, size_t count) override {
    input.insert(input.end(), bytes, bytes + count); return count;
  }
  void sentence(const char* text) { MicroNMEA::sendSentence(*this, text); }
};
struct RTC : mesh::RTCClock {
  uint32_t utc = 0;
  unsigned writes = 0;
  void setCurrentTime(uint32_t value) override { utc = value; ++writes; }
};
int main() {
  RTC clock;
  SerialInput serial;
  MicroNMEALocationProvider gps(serial, &clock, -1, -1);
  meshcore::gpsTimeHandler() = [](uint32_t utc) {
    ++updates; lastUtc = utc; assert(trusted.refresh(utc, uint32_t(now)));
  };
  gps.begin();
  for (int i = 0; i < 5; ++i) {
    now += 1001;
    serial.sentence("$GPRMC,120000.00,V,5300.0000,N,11300.0000,W,0.0,0.0,031026,,,N");
    gps.loop();
  }
  assert(updates == 0 && clock.utc == 0);
  for (int i = 0; i < 5; ++i) {
    now += 1001;
    const char* corrupt = "$GPRMC,120000.00,A,5300.0000,N,11300.0000,W,0.0,0.0,031026,,,A*00\r\n";
    serial.write(reinterpret_cast<const uint8_t*>(corrupt), strlen(corrupt));
    gps.loop();
  }
  assert(updates == 0 && clock.utc == 0);
  for (int i = 0; i < 5; ++i) {
    now += 1001;
    serial.sentence("$GPRMC,120000.00,A,5300.0000,N,11300.0000,W,0.0,0.0,010100,,,A");
    gps.loop();
  }
  assert(updates == 0 && clock.utc == 0);
  for (int i = 0; i < 5; ++i) {
    now += 1001;
    serial.sentence("$GPRMC,120000.00,A,5300.0000,N,11300.0000,W,0.0,0.0,031026,,,A");
    gps.loop();
  }
  assert(updates == 1 && clock.writes == 1 && clock.utc == lastUtc && lastUtc == 1791028800u);
  uint32_t lower, upper;
  assert(nrfmast::trustedTimeBounds(trusted.epoch(), trusted.ageMs(uint32_t(now)), lower, upper));
  now += 1800001;
  gps.loop();
  assert(updates == 1); // Cached fix is not a new time authority.
  now += 1800000;
  trusted.poll(uint32_t(now));
  assert(!trusted.fresh(uint32_t(now)));
  for (int i = 0; i < 5; ++i) {
    now += 1001;
    serial.sentence("$GPRMC,130100.00,A,5300.0000,N,11300.0000,W,0.0,0.0,031026,,,A");
    gps.loop();
  }
  assert(updates == 2 && clock.writes == 2 && trusted.fresh(uint32_t(now)) && clock.utc == 1791032460u);
  assert(!meshcore::publishGpsTime(0) && !meshcore::publishGpsTime(4102444801u));
  puts("PASS real NMEA fixes update board RTC and Pine trusted UTC; invalid/missing fixes, expiry and reacquisition");
}
