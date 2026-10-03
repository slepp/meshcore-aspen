#include "PineFieldUpdate.h"
#include "PineDfuService.h"
#include <Arduino.h>

namespace nrfmast {
FieldUpdateWindow fieldWindow;
bool fieldBleAuthenticated(uint16_t handle) {
  ble_gap_conn_sec_t security{};
  auto* connection = Bluefruit.Connection(handle);
  return connection && sd_ble_gap_conn_sec_get(handle, &security) == NRF_SUCCESS &&
      authenticatedBleBond(connection->connected(), connection->secured(), connection->bonded(),
                           security.sec_mode.sm, security.sec_mode.lv);
}
bool authorizeFieldDfu(uint16_t handle, const ble_gatts_evt_write_t* request) {
  const bool authenticated = fieldBleAuthenticated(handle);
  bond_keys_t keys{};
  if (!authenticated || !Bluefruit.Connection(handle)->loadBondKey(&keys)) return false;
  return request && request->op == BLE_GATTS_OP_WRITE_REQ &&
      fieldWindow.authorize(millis(), authenticated, request->data, request->len);
}
}
