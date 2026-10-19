// SPDX-License-Identifier: Apache-2.0
// Public stand-in for a private provider header: one fixture alias and nothing else.
#pragma once
#include "CloudRoomService.h"
namespace onchip {
const CloudRoomConfiguration *cloudRoomConfiguration() {
  static CloudRoomConfiguration configuration;
  configuration.count = 1;
  configuration.peers[0] = {"room.example", "192.0.2.1", "ca", "fixture-token", "room-1"};
  return &configuration;
}
}
