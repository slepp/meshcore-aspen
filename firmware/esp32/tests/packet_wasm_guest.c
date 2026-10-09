// SPDX-License-Identifier: Apache-2.0
#include "../../runtime/wasm/sdk/packet.h"
#ifndef INIT_CASE
#define INIT_CASE 0
#endif
static mp_info info;
static mp_system system_info;
static mp_phy_change phy;
static mp_compose compose;
static unsigned char composed[255];
static unsigned char bytes[255];
static unsigned counter;
static volatile unsigned spinning;
MP_EXPORT("mp_init") int mp_init(void) {
#if INIT_CASE == 1
  while (1) ++spinning;
#elif INIT_CASE == 2
  return 2;
#elif INIT_CASE == 3
  mp_get_info(&info, sizeof(info));
#endif
  return MP_ABI_VERSION;
}
MP_EXPORT("mp_process") int mp_process(void) {
#if INIT_CASE == 4
  while (1) ++spinning;
#endif
  if (mp_get_info(&info, sizeof(info)) != sizeof(info)) return 99;
  if (info.version != 1 || info.source != 3 || info.destination != 4 ||
      info.payload_type != 5 || info.generation != 6 || info.job != 7 ||
      info.rssi != -101 || info.snr_quarter_db != -9 || info.identity[31] != 0xa5)
    return 98;
  if (info.length != 2 || info.capacity != 255 || mp_read(0, bytes, 2) != 2) return 97;
  const unsigned mode = bytes[0];
  bytes[1] = 0x42;
  mp_write(1, bytes + 1, 1);
  bytes[0] = 0x70;
  bytes[2] = 0x43;
  mp_replace(bytes, 3);
  if (mode == 0) return MP_CONTINUE;
  if (mode == 1) {
    mp_emit(bytes, 3, 1, 12, 23);
    mp_emit(bytes, 3, 2, 13, 24);
    return MP_DROP;
  }
  if (mode == 2) {
    mp_emit(bytes, 3, 1, 12, 23);
    __builtin_trap();
  }
  if (mode == 3 || mode == 13) {
    if (mode == 13) mp_emit(bytes, 3, 1, 12, 23);
    while (1) ++spinning;
  }
  if (mode == 4) mp_read(0, (void *)65535, 2);
  if (mode == 5) mp_write(3, bytes, 1);
  if (mode == 6) return 256;
  if (mode == 7) for (unsigned i = 0; i < 65; ++i) mp_get_info(&info, sizeof(info));
  if (mode == 8 && __builtin_wasm_memory_grow(0, 1) != -1) return 96;
  if (mode == 9) mp_replace(bytes, 0);
  if (mode == 10) for (unsigned i = 0; i < 3; ++i) mp_emit(bytes, 3, 1, 0, 0);
  if (mode == 11) mp_emit(bytes, 3, 256, 0, 0);
  if (mode == 12) mp_write(0x10000, bytes, 1);
  if (mode == 14) mp_replace((void *)0xffffffff, 2);
  if (mode == 15) {
    mp_read(3, bytes, 0);
    mp_write(3, bytes, 0);
  }
  if (mode == 16) {
    bytes[1] = ++counter;
    mp_write(1, bytes + 1, 1);
  }
  if (mode >= 17) {
    if (mp_get_system(&system_info, mode == 20 ? 1 : sizeof(system_info)) != sizeof(system_info)) return 95;
    if (system_info.version != 1 || system_info.uptime_ms != 123 ||
        system_info.unix_time != 1791500000 || system_info.free_internal_bytes != 456 ||
        system_info.free_psram_bytes != 789 || system_info.enabled_roles != 15 ||
        system_info.ready_roles != 7 || system_info.generation != 21 || system_info.flags != 13 ||
        system_info.phy.frequency_hz != 910525000 || system_info.phy.bandwidth_hz != 62500 ||
        system_info.phy.spreading_factor != 7 || system_info.phy.coding_rate != 5 ||
        system_info.phy.tx_power != 22) return 94;
    phy.phy = system_info.phy; phy.phy.tx_power = 2;
    phy.generation = system_info.generation; phy.persist = 0;
    if (mp_set_phy(&phy, mode == 21 ? 1 : sizeof(phy)) != sizeof(phy)) return 93;
    compose.kind = MP_ADVERT; compose.payload_type = 4; compose.route = MP_DIRECT;
    compose.path_width = 2; compose.path_count = 1; compose.path[0] = 'a'; compose.path[1] = 'b';
    compose.timestamp = system_info.unix_time;
    for (unsigned i = 0; i < 32; ++i) compose.identity[i] = info.identity[i];
    bytes[0] = 'x'; bytes[1] = 'y';
    if (mp_compose_owned(&compose, sizeof(compose), bytes, 2,
                         mode == 22 ? (void *)65535 : composed, sizeof(composed)) != 6) return 92;
    if (composed[0] != 0x12 || composed[1] != 0x41 ||
        composed[2] != 'a' || composed[3] != 'b' || composed[4] != 'x' || composed[5] != 'y') return 91;
    mp_emit(composed, 6, 4, 0, 0);
    if (mode == 18) __builtin_trap();
    if (mode == 19) mp_set_phy(&phy, sizeof(phy));
  }
  return MP_CONTINUE;
}
