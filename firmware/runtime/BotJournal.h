// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <Arduino.h>
#include <Utils.h>
#include <nvs.h>
#include "BotTypes.h"
#include <atomic>

namespace onchip {
inline bool botEventCurrent(const BotIoRequest &request, const std::atomic<uint32_t> *epoch) {
  return !request.eventEpoch || (epoch && request.eventEpoch == epoch->load());
}
// Keep the existing core and mutation reserves after moving payloads to files.
constexpr size_t BotCoreNvsReserveEntries = 288, BotNvsMutationEntries = 20;
// KV replaces eight 15-entry reserved payloads with one 200-byte (9-entry)
// authority. Keep the scheduler/core remainder, mutation margin and GC page.
constexpr size_t BotKvAuthorityEntries =
#if ONCHIP_BOT_COMPACT_PROFILE
    7;
#else
    9;
#endif
constexpr size_t BotKvBootstrapEntries = 4;
constexpr size_t BotKvPublicEntries =
    BotCoreNvsReserveEntries - 8 * 15 + BotNvsMutationEntries + BotKvAuthorityEntries;
constexpr size_t BotKvRecoveryEntries = 126 + BotNvsMutationEntries + BotKvAuthorityEntries;
// Cold verification/migration covers five file groups and legacy reclamation.
constexpr uint32_t BotKvRecoveryBudgetMs = 10000;
inline bool admitPublicBotStorage(char *error, size_t capacity) {
  nvs_stats_t stats{};
  if (nvs_get_stats(nullptr, &stats) != ESP_OK) {
    snprintf(error, capacity, "Public storage NVS headroom unavailable");
    return false;
  }
  if (stats.free_entries < BotCoreNvsReserveEntries + BotNvsMutationEntries) {
    snprintf(error, capacity, "Public storage NVS headroom: %u free, %u required",
             unsigned(stats.free_entries), unsigned(BotCoreNvsReserveEntries + BotNvsMutationEntries));
    return false;
  }
  return true;
}
inline bool admitScheduleRestore(size_t memberEntries, bool publicScope, char *error, size_t capacity) {
  // File-backed scheduler imports publish a single authority, retaining the
  // existing core, replacement and GC reserves.
  size_t required = memberEntries + 126 + BotNvsMutationEntries;
  if (publicScope && required < BotCoreNvsReserveEntries + BotNvsMutationEntries)
    required = BotCoreNvsReserveEntries + BotNvsMutationEntries;
  nvs_stats_t stats{};
  if (nvs_get_stats(nullptr, &stats) != ESP_OK) {
    snprintf(error, capacity, "Schedule restore NVS headroom unavailable; no records imported");
    return false;
  }
  if (stats.free_entries < required) {
    snprintf(error, capacity, "Schedule restore NVS headroom: %u free, %u required; no records imported",
             unsigned(stats.free_entries), unsigned(required));
    return false;
  }
  return true;
}
// Records end in a SHA-256 digest. This consumes the write handle and verifies
// the commit through an independent read-only handle; ambiguous writes stay errors.
template<class Current>
bool commitBotJournal(nvs_handle_t handle, const char *space, const char *key,
                      void *record, size_t size, uint32_t started, Current current,
                      char *error, size_t capacity, uint32_t budgetMs = 2000) {
  auto *bytes = static_cast<uint8_t *>(record);
  uint8_t expected[32];
  mesh::Utils::sha256(expected, sizeof(expected), bytes, size - sizeof(expected));
  memcpy(bytes + size - sizeof(expected), expected, sizeof(expected));
  const auto fail = [&](const char *message) {
    snprintf(error, capacity, "%s", message);
    return false;
  };
  if (!current() || uint32_t(millis() - started) >= budgetMs) {
    nvs_close(handle); return fail("Storage cancelled/deadline before write");
  }
  auto status = nvs_set_blob(handle, key, record, size);
  if (status != ESP_OK) {
    nvs_close(handle); return fail("Storage write failed; outcome unknown");
  }
  if (!current()) {
    nvs_close(handle); return fail("Storage cancelled during write; outcome unknown");
  }
  status = nvs_commit(handle);
  nvs_close(handle);
  if (status != ESP_OK) return fail("Storage commit failed; outcome unknown");
  if (nvs_open(space, NVS_READONLY, &handle) != ESP_OK)
    return fail("Storage commit readback unavailable; outcome unknown");
  size_t actualSize = size;
  memset(record, 0, size);
  status = nvs_get_blob(handle, key, record, &actualSize);
  nvs_close(handle);
  uint8_t actual[32];
  mesh::Utils::sha256(actual, sizeof(actual), bytes, size - sizeof(actual));
  if (status != ESP_OK || actualSize != size || memcmp(actual, expected, sizeof(actual)) ||
      memcmp(bytes + size - sizeof(expected), expected, sizeof(expected)))
    return fail("Storage commit readback failed; outcome unknown");
  if (!current()) return fail("Storage committed after cancellation; do not retry blindly");
  if (uint32_t(millis() - started) >= budgetMs)
    return fail("Storage committed after deadline; do not retry blindly");
  return true;
}
} // namespace onchip
