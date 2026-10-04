#pragma once
#include <bluefruit.h>

// The implementation is staged from the installed Adafruit BLEDfu, with an
// authorization gate before its unchanged Nordic buttonless handover.
class PineDfuService : public BLEService {
protected:
  BLECharacteristic _chr_control;
public:
  PineDfuService();
  err_t begin() override;
};
namespace nrfmast {
bool fieldBleAuthenticated(uint16_t connection);
bool authorizeFieldDfu(uint16_t connection, const ble_gatts_evt_write_t* request);
}
