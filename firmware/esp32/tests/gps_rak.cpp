// SPDX-License-Identifier: Apache-2.0
#include <helpers/sensors/MicroNMEALocationProvider.h>
#include "../../nrf52840/PineTrustedTime.h"
#include <cassert>
#include <cstdio>

static uint32_t now;
static unsigned updates;
static nrfmast::TrustedTimeSample trusted;
unsigned long millis() { return now; }
void delay(unsigned long value) { now += value; }
void pinMode(int, int) {}
void digitalWrite(int, int) {}
int digitalRead(int) { return HIGH; }
void LocationProvider::sendSentence(const char*) {}

struct Gnss {
  bool fresh = true, fix = true, dateValid = true, timeValid = true, resolved = true;
  unsigned year = 2026, month = 10;
  uint32_t utc = 1791028800u;
  unsigned epochReads = 0;
  bool getPVT(unsigned wait) { assert(wait == 8); return fresh; }
  bool getGnssFixOk(unsigned) { return fix; }
  long getLatitude(unsigned) { return 530000000; }
  long getLongitude(unsigned) { return -1130000000; }
  long getAltitude(unsigned) { return 650000; }
  int getSIV(unsigned) { return 10; }
  bool getDateValid(unsigned) { return dateValid; }
  bool getTimeValid(unsigned) { return timeValid; }
  bool getTimeFullyResolved(unsigned) { return resolved; }
  unsigned getYear(unsigned) { return year; }
  unsigned getMonth(unsigned) { return month; }
  uint32_t getUnixEpoch(unsigned) { ++epochReads; return utc; }
} ublox_GNSS;

#include <helpers/sensors/RakGpsProvider.h>

struct SerialInput : Stream {
  int available() override { return 0; }
  int read() override { return -1; }
  size_t write(const uint8_t*, size_t count) override { return count; }
};
struct RTC : mesh::RTCClock {
  uint32_t utc = 0;
  void setCurrentTime(uint32_t value) override { utc = value; }
};

int main() {
  RTC clock;
  SerialInput serial;
  MicroNMEALocationProvider nmea(serial, &clock, -1, -1);
  RAK12500LocationProvider gps;
  meshcore::gpsTimeHandler() = [](uint32_t utc) {
    ++updates; assert(trusted.refresh(utc, now));
  };
  gps.loop();
  assert(updates == 1 && clock.utc == ublox_GNSS.utc && trusted.fresh(now));
  assert(gps.isValid() && gps.getLatitude() == 53000000 &&
         gps.getLongitude() == -113000000 && gps.getAltitude() == 650000 &&
         gps.satellitesCount() == 10 && gps.getTimestamp() == ublox_GNSS.utc);

  ublox_GNSS.fresh = false;
  now += 3600001;
  gps.loop();
  trusted.poll(now);
  assert(updates == 1 && !gps.isValid() && !trusted.fresh(now));
  assert(gps.getTimestamp() == 0 && ublox_GNSS.epochReads == 1);
  ublox_GNSS.fresh = true;
  for (bool* flag : {&ublox_GNSS.dateValid, &ublox_GNSS.timeValid, &ublox_GNSS.resolved}) {
    *flag = false; gps.loop();
    assert(updates == 1 && gps.isValid() && ublox_GNSS.epochReads == 1);
    *flag = true;
  }
  for (unsigned year : {2019u, 2100u}) {
    ublox_GNSS.year = year; gps.loop();
    assert(updates == 1 && ublox_GNSS.epochReads == 1);
  }
  ublox_GNSS.year = 2026;
  for (unsigned month : {0u, 13u}) {
    ublox_GNSS.month = month; gps.loop();
    assert(updates == 1 && ublox_GNSS.epochReads == 1);
  }
  ublox_GNSS.month = 10;
  ublox_GNSS.fix = false; gps.loop();
  assert(updates == 1 && !gps.isValid());
  ublox_GNSS.fix = true;
  ublox_GNSS.utc = 0; gps.loop();
  assert(updates == 1 && clock.utc == 1791028800u);
  ublox_GNSS.utc = 1791032460u; gps.loop();
  assert(updates == 2 && trusted.fresh(now) && clock.utc == ublox_GNSS.utc);
  meshcore::gpsTimeHandler() = nullptr;
  ++ublox_GNSS.utc; gps.loop();
  assert(clock.utc == ublox_GNSS.utc);
  puts("PASS pinned RAK12500 provider: fresh valid UTC, board RTC, location, expiry, invalid UTC and reacquisition");
}
