// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "PacketReport.h"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <new>
#ifdef ARDUINO_ARCH_ESP32
#include <esp_heap_caps.h>
#endif

namespace packet_engine {
// Dispatch-task only. Commands receive copied results through their existing
// worker mailbox; HTTP reads the dashboard's published copy.
class Reports {
  struct Storage {
    mp_report staged[2], published[2];
    uint8_t stagedMask = 0, publishedMask = 0;
  };
  Storage *storage_ = nullptr;
  static bool name(const char *s, bool identifier) {
    const size_t n = strnlen(s, MP_REPORT_NAME);
    if (!n || n == MP_REPORT_NAME) return false;
    for (size_t i = 0; i < n; ++i) {
      const char c = s[i];
      if (identifier ? !((c >= 'a' && c <= 'z') || (i && (c == '_' || (c >= '0' && c <= '9')))) :
                       (c < 32 || c > 126)) return false;
    }
    return true;
  }
public:
  Reports() = default;
  Reports(const Reports &) = delete;
  Reports &operator=(const Reports &) = delete;
  ~Reports() { free(storage_); }
  bool begin() {
    if (storage_) return true;
#ifdef ARDUINO_ARCH_ESP32
    storage_ = static_cast<Storage *>(heap_caps_calloc(1, sizeof(Storage), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
#else
    storage_ = static_cast<Storage *>(calloc(1, sizeof(Storage)));
#endif
    if (storage_) new (storage_) Storage{};
    return storage_ != nullptr;
  }
  static bool valid(const mp_report &r) {
    if (r.version != MP_REPORT_VERSION || r.reserved || r.row_count > MP_REPORT_ROWS ||
        !r.field_count || r.field_count > MP_REPORT_FIELDS || !name(r.name, true)) return false;
    for (unsigned f = 0; f < r.field_count; ++f) {
      if (!name(r.fields[f], true)) return false;
      for (unsigned j = 0; j < f; ++j)
        if (!strcmp(r.fields[f], r.fields[j])) return false;
    }
    for (unsigned i = 0; i < r.row_count; ++i) {
      if (!name(r.rows[i].label, false)) return false;
      for (unsigned j = 0; j < i; ++j)
        if (!strcmp(r.rows[i].label, r.rows[j].label)) return false;
      for (unsigned f = 0; f < r.field_count; ++f)
        if (!std::isfinite(r.rows[i].values[f])) return false;
    }
    return true;
  }
  void start() { if (storage_) storage_->stagedMask = 0; }
  bool stage(uint8_t slot, const void *bytes, size_t size) {
    if (!storage_ || slot >= 2 || !bytes || size != sizeof(mp_report) ||
        (storage_->stagedMask & (1u << slot))) return false;
    auto &report = storage_->staged[slot];
    memcpy(&report, bytes, size);
    if (!valid(report)) return false;
    storage_->stagedMask |= 1u << slot;
    return true;
  }
  void commit() {
    if (!storage_) return;
    for (unsigned i = 0; i < 2; ++i)
      if (storage_->stagedMask & (1u << i)) storage_->published[i] = storage_->staged[i];
    storage_->publishedMask |= storage_->stagedMask;
    storage_->stagedMask = 0;
  }
  void clear(uint8_t slot) {
    if (!storage_ || slot >= 2) return;
    storage_->stagedMask &= ~(1u << slot);
    storage_->publishedMask &= ~(1u << slot);
  }
  bool read(uint8_t slot, mp_report &out) const {
    if (!storage_ || slot >= 2 || !(storage_->publishedMask & (1u << slot))) return false;
    out = storage_->published[slot];
    return true;
  }
};
}
