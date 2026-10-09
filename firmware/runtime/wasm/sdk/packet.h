// SPDX-License-Identifier: Apache-2.0
#ifndef MESHCORE_PACKET_V1_H
#define MESHCORE_PACKET_V1_H
#include <stdint.h>
#ifdef __wasm__
#define MP_IMPORT(name) __attribute__((import_module("meshcore_packet_v1"), import_name(name)))
#define MP_EXPORT(name) __attribute__((export_name(name)))
#else
#define MP_IMPORT(name)
#define MP_EXPORT(name)
#endif
enum mp_stage {
  MP_RECEIVE, MP_LOCAL_DELIVERY, MP_ADMISSION, MP_TRANSMIT, MP_REFLECTION,
  MP_RELAY, MP_PLAIN_RECEIVE, MP_PLAIN_COMPOSE
};
enum {
  MP_ABI_VERSION = 1, MP_CONTINUE = 0, MP_DROP = 1,
  MP_LOCAL = 1, MP_AUTHENTICATED = 2, MP_ENGINE_ORIGIN = 4, MP_REFLECTION_ORIGIN = 8
};
typedef struct {
  uint32_t version, stage, length, capacity, source, destination, payload_type, flags;
  uint32_t generation, job;
  int32_t rssi, snr_quarter_db;
  uint8_t identity[32];
} mp_info;
enum {
  MP_READ_SYSTEM = 1, MP_WRITE_PHY = 2, MP_COMPOSE_OWNED = 4,
  MP_RADIO_READY = 1, MP_TRANSMITTING = 2, MP_TRUSTED_TIME = 4, MP_HEAP_MEASURED = 8,
  MP_ADVERT = 0, MP_DATAGRAM = 1, MP_ANONYMOUS = 2, MP_GROUP = 3,
  MP_FLOOD = 1, MP_DIRECT = 2
};
typedef struct {
  uint32_t frequency_hz, bandwidth_hz, spreading_factor, coding_rate, tx_power;
} mp_phy;
typedef struct {
  mp_phy phy;
  uint32_t generation, persist;
} mp_phy_change;
typedef struct {
  uint32_t version, uptime_ms, unix_time, free_internal_bytes, free_psram_bytes,
           enabled_roles, ready_roles;
  mp_phy phy;
  uint32_t generation, flags;
} mp_system;
typedef struct {
  uint32_t kind, payload_type, route, path_width, path_count, channel, timestamp;
  uint8_t identity[32], destination[32], path[64];
} mp_compose;
#ifdef __cplusplus
static_assert(sizeof(mp_info) == 80, "Packet ABI v1 metadata has a fixed size");
static_assert(sizeof(mp_phy_change) == 28 && sizeof(mp_system) == 56 &&
              sizeof(mp_compose) == 156, "Packet service ABI v1 records have fixed sizes");
#endif
// Addresses designate guest linear memory. Imports are valid only inside
// mp_process; initialization cannot access a packet or stage transmissions.
MP_IMPORT("info") int32_t mp_get_info(mp_info *out, uint32_t capacity);
MP_IMPORT("read") int32_t mp_read(uint32_t offset, void *out, uint32_t length);
MP_IMPORT("write") int32_t mp_write(uint32_t offset, const void *bytes, uint32_t length);
MP_IMPORT("replace") int32_t mp_replace(const void *bytes, uint32_t length);
MP_IMPORT("emit") int32_t mp_emit(const void *bytes, uint32_t length, uint32_t priority,
                                 uint32_t delay_ms, uint32_t expiry_ms);
MP_IMPORT("system") int32_t mp_get_system(mp_system *out, uint32_t capacity);
MP_IMPORT("set_phy") int32_t mp_set_phy(const mp_phy_change *request, uint32_t length);
MP_IMPORT("compose") int32_t mp_compose_owned(const mp_compose *request, uint32_t request_length,
                                            const void *data, uint32_t length,
                                            void *out, uint32_t capacity);
// Required exports: mp_init()->i32 returns MP_ABI_VERSION;
// mp_process()->i32 returns MP_CONTINUE or MP_DROP.
// Imports return a copied byte count or -1 with an execution fault.
#endif
