// SPDX-License-Identifier: Apache-2.0
#include "../sdk/packet.h"
enum { PEERS = 24, WINDOW = 256, STALE_MS = 3600000 };
typedef struct {
  uint8_t key[3], width;
  uint16_t count, next;
  uint32_t heard, last_ms, last_utc;
  int32_t snr_sum, rssi_sum;
  int8_t snr[WINDOW];
  int16_t rssi[WINDOW];
  double mean, p95, rssi_mean;
} peer;
static peer peers[PEERS];
static mp_info info;
static mp_system clock_info;
static mp_report report;
static uint16_t histogram[256];
static uint8_t path[64], header[6];
static void text(char *out, const char *in) { while ((*out++ = *in++)) {} }
static int same(const peer *p, const uint8_t *key, unsigned width) {
  if (!p->count || p->width != width) return 0;
  for (unsigned i = 0; i < width; ++i) if (p->key[i] != key[i]) return 0;
  return 1;
}
static void reset(peer *p, const uint8_t *key, unsigned width) {
  p->width = width; p->count = p->next = 0; p->heard = 0;
  p->snr_sum = p->rssi_sum = 0;
  for (unsigned i = 0; i < width; ++i) p->key[i] = key[i];
}
static peer *find(const uint8_t *key, unsigned width, uint32_t now) {
  peer *empty = 0, *least = &peers[0];
  for (unsigned i = 0; i < PEERS; ++i) {
    peer *p = &peers[i];
    if (p->count && (uint32_t)(now - p->last_ms) >= STALE_MS) p->count = 0;
    if (same(p, key, width)) return p;
    if (!p->count && !empty) empty = p;
    if (p->heard < least->heard || (p->heard == least->heard &&
        (uint32_t)(now - p->last_ms) > (uint32_t)(now - least->last_ms))) least = p;
  }
  peer *p = empty ? empty : least;
  reset(p, key, width); return p;
}
static void sample(peer *p) {
  if (p->count == WINDOW) {
    p->snr_sum -= p->snr[p->next]; p->rssi_sum -= p->rssi[p->next];
  } else ++p->count;
  p->snr[p->next] = (int8_t)info.snr_quarter_db;
  p->rssi[p->next] = (int16_t)info.rssi;
  p->snr_sum += info.snr_quarter_db; p->rssi_sum += info.rssi;
  p->next = (p->next + 1) % WINDOW;
  if (p->heard != UINT32_MAX) ++p->heard;
  p->last_ms = clock_info.uptime_ms;
  p->last_utc = clock_info.flags & MP_TRUSTED_TIME ? clock_info.unix_time : 0;
  p->mean = (double)p->snr_sum / (4 * p->count);
  p->rssi_mean = (double)p->rssi_sum / p->count;
  for (unsigned i = 0; i < 256; ++i) histogram[i] = 0;
  for (unsigned i = 0; i < p->count; ++i) ++histogram[(int)p->snr[i] + 128];
  const unsigned rank = (p->count * 95 + 99) / 100;
  unsigned count = 0;
  for (unsigned i = 0; i < 256; ++i) {
    count += histogram[i];
    if (count >= rank) { p->p95 = ((int)i - 128) / 4.0; break; }
  }
}
static int publish(void) {
  report.row_count = 0;
  uint32_t selected = 0;
  for (unsigned row = 0; row < MP_REPORT_ROWS; ++row) {
    unsigned best = PEERS;
    for (unsigned i = 0; i < PEERS; ++i) {
      if (!peers[i].count || (uint32_t)(clock_info.uptime_ms - peers[i].last_ms) >= STALE_MS) continue;
      if (selected & (1u << i)) continue;
      if (best == PEERS || peers[i].heard > peers[best].heard ||
          (peers[i].heard == peers[best].heard &&
           (uint32_t)(clock_info.uptime_ms - peers[i].last_ms) <
           (uint32_t)(clock_info.uptime_ms - peers[best].last_ms))) best = i;
    }
    if (best == PEERS) break;
    selected |= 1u << best;
    const peer *p = &peers[best]; mp_report_row *out = &report.rows[row];
    static const char hex[] = "0123456789ABCDEF";
    for (unsigned i = 0; i < p->width; ++i) {
      out->label[2 * i] = hex[p->key[i] >> 4]; out->label[2 * i + 1] = hex[p->key[i] & 15];
    }
    out->label[2 * p->width] = 0;
    out->values[0] = p->count; out->values[1] = p->heard;
    out->values[2] = p->mean; out->values[3] = p->p95;
    out->values[4] = p->rssi_mean; out->values[5] = p->last_ms;
    out->values[6] = WINDOW; out->values[7] = p->last_utc;
    ++report.row_count;
  }
  return mp_publish_report(&report, sizeof(report)) == sizeof(report) ? MP_CONTINUE : -1;
}
MP_EXPORT("mp_init") int mp_init(void) {
  report.version = MP_REPORT_VERSION; report.field_count = 8;
  text(report.name, "repeater_snr");
  static const char *fields[] = {"samples", "heard_total", "mean_db", "p95_db", "rssi_mean_dbm",
                                 "last_seen_ms", "window", "last_seen_utc"};
  for (unsigned i = 0; i < 8; ++i) text(report.fields[i], fields[i]);
  return MP_ABI_VERSION;
}
MP_EXPORT("mp_process") int mp_process(void) {
  if (mp_get_info(&info, sizeof(info)) != sizeof(info)) return -1;
  if (info.stage != MP_RECEIVE || (info.flags & (MP_LOCAL | MP_REFLECTION_ORIGIN | MP_ENGINE_ORIGIN)) ||
      info.snr_quarter_db < -128 || info.snr_quarter_db > 127 || info.rssi < -160 || info.rssi > 0 ||
      info.length < 3) return MP_CONTINUE;
  if (mp_read(0, header, 1) != 1) return -1;
  const unsigned route = header[0] & 3;
  if (route > 1 || (header[0] >> 6)) return MP_CONTINUE;
  const unsigned offset = route == 0 ? 5 : 1;
  if (info.length <= offset + 1 || mp_read(offset, header, 1) != 1) return MP_CONTINUE;
  const unsigned width = (header[0] >> 6) + 1, count = header[0] & 63;
  const unsigned bytes = width * count;
  if (!count || width > 3 || bytes > sizeof(path) || offset + 1 + bytes >= info.length) return MP_CONTINUE;
  if (mp_read(offset + 1, path, bytes) != bytes ||
      mp_get_system(&clock_info, sizeof(clock_info)) != sizeof(clock_info)) return -1;
  peer *p = find(path + bytes - width, width, clock_info.uptime_ms);
  sample(p); return publish();
}
