#pragma once
#include "../../../../firmware/esp32/tests/seams/target.h"
#include <RemoteKissRadio.h>
#include <vector>
class ObservedRemoteRadio : public RemoteKissRadio {
public:
  using RemoteKissRadio::RemoteKissRadio;
  std::vector<mesh::QueuedTransmitResult> results;
  bool pollQueuedResult(mesh::QueuedTransmitResult &result) override {
    if (!RemoteKissRadio::pollQueuedResult(result))
      return false;
    results.push_back(result);
    return true;
  }
};
extern ObservedRemoteRadio radio_driver;
mesh::LocalIdentity radio_new_identity();
