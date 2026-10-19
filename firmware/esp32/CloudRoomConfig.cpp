// SPDX-License-Identifier: Apache-2.0
#include "CloudRoomService.h"
#if defined(MESHCORE_CLOUD_ROOM) && MESHCORE_CLOUD_ROOM && defined(ONCHIP_CLOUD_ROOM_CONFIG_HEADER)
#if defined(ONCHIP_CLOUD_ROOM_RUNTIME_CONFIG) && ONCHIP_CLOUD_ROOM_RUNTIME_CONFIG
// Saved settings own cloudRoomConfiguration(). The private header's provider is renamed
// so the first boot can adopt it into a saved record without editing the header.
#define cloudRoomConfiguration cloudRoomCompiledConfiguration
#include ONCHIP_CLOUD_ROOM_CONFIG_HEADER
#undef cloudRoomConfiguration
#else
#include ONCHIP_CLOUD_ROOM_CONFIG_HEADER
#endif
#endif
