// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>
#include <Mesh.h>

namespace meshcore {
// WisBlock I2C detection replaces the UART provider, not its board RTC.
inline mesh::RTCClock*& gpsRtcClock() {
  static mesh::RTCClock* clock = nullptr;
  return clock;
}
using GpsTimeHandler = void (*)(uint32_t);
inline GpsTimeHandler& gpsTimeHandler() {
  static GpsTimeHandler handler = nullptr;
  return handler;
}
inline bool publishGpsTime(uint32_t utc) {
  if (utc < 1715770351u || utc > 4102444800u) return false;
  if (gpsRtcClock()) gpsRtcClock()->setCurrentTime(utc);
  if (gpsTimeHandler()) gpsTimeHandler()(utc);
  return true;
}
}
