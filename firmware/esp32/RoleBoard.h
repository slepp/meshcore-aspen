// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "Lifecycle.h"
#include <Arduino.h>

namespace onchip {
class RoleBoard final : public mesh::MainBoard {
  mesh::MainBoard &physical;
  Role role;

public:
  RoleBoard(mesh::MainBoard &board, Role value)
      : physical(board), role(value) {}
  uint16_t getBattMilliVolts() override { return physical.getBattMilliVolts(); }
  float getMCUTemperature() override { return physical.getMCUTemperature(); }
  const char *getManufacturerName() const override {
    return physical.getManufacturerName();
  }
  uint8_t getStartupReason() const override {
    return physical.getStartupReason();
  }
  float getAdcMultiplier() const override {
    return physical.getAdcMultiplier();
  }
  uint32_t getIRQGpio() override { return physical.getIRQGpio(); }
  uint32_t getGpio() override { return physical.getGpio(); }
  bool getBootloaderVersion(char *version, size_t size) override {
    return physical.getBootloaderVersion(version, size);
  }
  bool isExternalPowered() override { return physical.isExternalPowered(); }
  uint16_t getBootVoltage() override { return physical.getBootVoltage(); }
  uint32_t getResetReason() const override { return physical.getResetReason(); }
  const char *getResetReasonString(uint32_t reason) override {
    return physical.getResetReasonString(reason);
  }
  uint8_t getShutdownReason() const override {
    return physical.getShutdownReason();
  }
  const char *getShutdownReasonString(uint8_t reason) override {
    return physical.getShutdownReasonString(reason);
  }
  void reboot() override {
    if (!requestLifecycle(role, LifecycleAction::Reboot))
      Serial.printf("On-chip %s reboot request rejected: busy\n",
                    roleName(role));
  }
  void powerOff() override { Serial.println("Role power-off is unsupported"); }
};
} // namespace onchip
