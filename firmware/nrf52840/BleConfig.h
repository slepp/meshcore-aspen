#pragma once

#include <Arduino.h>
#include <helpers/IdentityStore.h>

namespace nrfmast {

class BleConfig {
  struct Record {
    uint8_t magic[8];
    uint32_t pin;
    uint8_t enabled, reserved[3];
  };
  FILESYSTEM& fs;
  uint32_t pin = 0;
  bool enabled = false;
  bool save(uint32_t newPin, bool newEnabled);

public:
  explicit BleConfig(FILESYSTEM& storage) : fs(storage) {}
  bool begin();
  uint32_t getPin() const { return pin; }
  bool isEnabled() const { return enabled; }
  bool setPin(uint32_t value) { return value >= 100000 && value <= 999999 && save(value, enabled); }
  bool setEnabled(bool value) { return (!value || pin >= 100000) && save(pin, value); }
};

}  // namespace nrfmast
