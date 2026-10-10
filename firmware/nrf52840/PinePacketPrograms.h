// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "SharedRadio.h"
#if NRFMAST_PRODUCTION_LUA
#include "PacketPipeline.h"
namespace nrfmast {
bool packetProgramsSelected();
bool beginPacketPrograms(SharedRadio &);
void servicePacketPrograms(bool drainOnly = false);
bool packetProgramCommand(const char *, char *, size_t);
bool packetProgramHook(RadioPort &, const packet_engine::Metadata &, uint8_t *, uint16_t &, uint16_t,
                       packet_engine::Validator = nullptr);
}
#endif
