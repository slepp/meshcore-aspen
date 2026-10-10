#pragma once
#include <helpers/ArduinoHelpers.h>
#include <helpers/SensorManager.h>
class RoomTestBoard : public mesh::MainBoard {
public:
  uint16_t getBattMilliVolts() override { return 4000; }
  const char* getManufacturerName() const override { return "Pine native Room test"; }
  uint8_t getStartupReason() const override { return 0; }
  void reboot() override { assert(false); }
};
class RoomTestDriver {
public:
  float getLastRSSI() const { return -90; }
  float getLastSNR() const { return 0; }
  uint32_t getPacketsRecv() const { return 0; }
  uint32_t getPacketsSent() const { return 0; }
  uint32_t getPacketsRecvErrors() const { return 0; }
};
extern RoomTestBoard board;
extern RoomTestDriver radio_driver;
extern SensorManager sensors;
mesh::LocalIdentity radio_new_identity();
