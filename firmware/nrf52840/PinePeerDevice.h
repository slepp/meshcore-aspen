// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "SharedRadio.h"
#if NRFMAST_PRODUCTION_LUA
namespace nrfmast {
bool peerDeviceSelected();
bool beginPeerDevice(SharedRadio &, mesh::LocalIdentity &repeater);
void peerDeviceLoop();
bool peerDeviceCommand(const char *, char *, size_t, bool local = false);
void peerDeviceRelay(const mesh::PacketEngineInfo &, const uint8_t *, uint16_t);
}
#endif
