// SPDX-License-Identifier: Apache-2.0
#include "CloudRoomService.h"
#if defined(ONCHIP_CLOUD_ROOM_RUNTIME_CONFIG) && ONCHIP_CLOUD_ROOM_RUNTIME_CONFIG && \
    defined(ONCHIP_CLOUD_ROOM_CONFIG_HEADER)
#error "Select saved cloud room settings or a custom provider, not both"
#endif
#if defined(MESHCORE_CLOUD_ROOM) && MESHCORE_CLOUD_ROOM && defined(ONCHIP_CLOUD_ROOM_CONFIG_HEADER)
#include ONCHIP_CLOUD_ROOM_CONFIG_HEADER
#endif
