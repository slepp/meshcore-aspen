// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stddef.h>
class WifiKissMultiplexer;
namespace onchip {
bool beginPacketPrograms(WifiKissMultiplexer &);
void servicePacketPrograms();
void packetProgramCommand(const char *, char *, size_t);
void stopPacketPrograms();
}
