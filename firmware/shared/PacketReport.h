// SPDX-License-Identifier: Apache-2.0
#ifndef MESHCORE_PACKET_REPORT_H
#define MESHCORE_PACKET_REPORT_H
#include <stdint.h>
enum { MP_REPORT_VERSION = 1, MP_REPORT_ROWS = 16, MP_REPORT_FIELDS = 8, MP_REPORT_NAME = 24 };
typedef struct {
  char label[MP_REPORT_NAME];
  double values[MP_REPORT_FIELDS];
} mp_report_row;
typedef struct {
  uint32_t version, row_count, field_count, reserved;
  char name[MP_REPORT_NAME];
  char fields[MP_REPORT_FIELDS][MP_REPORT_NAME];
  mp_report_row rows[MP_REPORT_ROWS];
} mp_report;
#ifdef __cplusplus
static_assert(sizeof(mp_report) == 1640 && sizeof(mp_report_row) == 88,
              "Packet report ABI has fixed copied records");
#endif
#endif
