// SPDX-License-Identifier: Apache-2.0
#pragma once
#if NRFMAST_PRODUCTION_LUA
#include <Mesh.h>

namespace nrfmast {
class SharedRadio;
bool roomUnsafeCommand(const char*);
bool roomCommand(uint32_t senderTimestamp, const char*, char*);
bool beginRoom(SharedRadio&, mesh::MillisecondClock&, mesh::RNG&, mesh::RTCClock&);
bool roomSelected();
bool roomRunning();
void roomLoop();
}
#endif
