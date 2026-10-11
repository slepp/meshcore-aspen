// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stddef.h>
#include "PacketReport.h"
class WifiKissMultiplexer;
namespace onchip {
bool beginPacketPrograms(WifiKissMultiplexer &);
void servicePacketPrograms();
void packetProgramCommand(const char *, char *, size_t);
bool packetProgramReport(unsigned slot, mp_report &);
void stopPacketPrograms();
}
