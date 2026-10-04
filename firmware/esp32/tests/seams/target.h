#pragma once
#include <cassert>
#include <helpers/ArduinoHelpers.h>
#include <helpers/SensorManager.h>
class LifecycleTestBoard : public mesh::MainBoard {
public:
  float mcuTemperature = NAN;
  uint16_t batteryMv = 4000;
  uint16_t getBattMilliVolts() override { return batteryMv; }
  float getMCUTemperature() override { return mcuTemperature; }
  const char *getManufacturerName() const override {
    return "native-lifecycle-test";
  }
  uint8_t getStartupReason() const override { return 0; }
  void reboot() override {
    assert(false && "Physical MCU reboot is forbidden");
  }
};
extern LifecycleTestBoard board;
extern SensorManager sensors;
extern VolatileRTCClock rtc_clock;
