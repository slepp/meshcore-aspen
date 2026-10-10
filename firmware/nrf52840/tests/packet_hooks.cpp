// SPDX-License-Identifier: Apache-2.0
#include "PinePacketPrograms.h"
#include "PinePeerDevice.h"
namespace nrfmast {
packet_engine::Pipeline *hookTestPipeline;
void packetHooksTestAttach(packet_engine::Pipeline *pipeline) { hookTestPipeline = pipeline; }
bool packetProgramsSelected() { return false; }
bool peerDeviceSelected() { return false; }
void peerDeviceRelay(const mesh::PacketEngineInfo &, const uint8_t *, uint16_t) {}
bool packetProgramHook(RadioPort &, const packet_engine::Metadata &metadata, uint8_t *bytes,
                       uint16_t &length, uint16_t capacity, packet_engine::Validator validator) {
  return !hookTestPipeline ||
         hookTestPipeline->process(metadata, bytes, length, capacity, validator) != packet_engine::Decision::Drop;
}
}
