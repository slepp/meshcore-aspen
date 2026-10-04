// SPDX-License-Identifier: Apache-2.0
#include "RadioDashboard.h"
#include "RadioFirmwareIdentity.h"
#include <cassert>
#include <cstdio>

int main() {
  RadioDashboard::Snapshot snapshot{};
  queued_tx::putFloat(snapshot.radio.profile + 11, 1);
  char json[RadioDashboard::JSON_CAPACITY];
  assert(RadioDashboard::formatJSON(snapshot, "operator-selected-name", json,
                                    sizeof(json)));
  std::puts(radio_firmware::version);
  std::puts(json);
}
