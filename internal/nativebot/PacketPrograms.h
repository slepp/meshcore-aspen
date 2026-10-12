// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "PacketReport.h"
#include "LocalRadio.h"
#include <stddef.h>
#include <stdint.h>
namespace onchip {
bool beginHostPacketPrograms(const RadioConfig &, const uint8_t identity[32]);
void stopHostPacketPrograms();
void hostPacketReceive(const uint8_t *, size_t, float rssi, float snr, uint8_t origin);
void serviceHostPacketPrograms();
uint32_t hostPacketWaitMs();
void hostPacketCommand(const char *, char *, size_t);
bool hostPacketReport(unsigned, mp_report &);
size_t hostPacketSnapshot(uint8_t *, size_t);
}
