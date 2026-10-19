// SPDX-License-Identifier: Apache-2.0
#include "CloudRoomLifecycle.h"

namespace onchip {
namespace {
CloudRoomRoomMode roomMode = CloudRoomRoomMode::Off;
}
bool __attribute__((weak)) cloudRoomScheduleRestart(uint32_t) { return false; }
void cloudRoomReportRoom(CloudRoomRoomMode mode) { roomMode = mode; }
CloudRoomRoomMode cloudRoomRoomMode() { return roomMode; }
} // namespace onchip
