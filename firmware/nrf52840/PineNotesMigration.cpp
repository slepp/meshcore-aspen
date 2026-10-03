// SPDX-License-Identifier: Apache-2.0
#if NRFMAST_PRODUCTION_LUA
#include "PineNotesMigration.h"
#include "onchip/BotStore.h"
#include "onchip/MastSource.h"
#include <cstring>
namespace nrfmast {
bool migrateLuaNotes(const NoteStore &notes, char *error, size_t capacity) {
  using namespace onchip;
  bool present;
  uint8_t marker[4]{};
  if (!mastRecord("pine-notes", marker, sizeof(marker), false, present)) {
    snprintf(error, capacity, "Notes migration marker unreadable; original notes retained"); return false;
  }
  if (present) {
    if (!memcmp(marker, "PNM\1", 4)) return true;
    snprintf(error, capacity, "Notes migration marker corrupt; original notes retained"); return false;
  }
  struct Migration {
    BotStore store;
    std::atomic<uint32_t> generation{1}, grant{1};
    std::atomic<bool> shared{false};
    char *error;
    size_t capacity;
  } migration{{}, 1, 1, false, error, capacity};
  if (!migration.store.recover(error, capacity)) return false;
  const bool copied = notes.visitEntries([](void *context, const NoteStore::Entry &entry) {
    auto &m = *static_cast<Migration *>(context);
    BotIoRequest request;
    request.token = {1, 1, 1}; request.scope = BotIoRequest::Caller;
    request.kind = BotIoRequest::Get;
    memcpy(request.principal, entry.principal, 32); strcpy(request.key, entry.key);
    BotIoResult result;
    m.store.perform(entry.bot, request, result, m.generation, m.shared, m.grant);
    if (!result.ok || (result.found && strcmp(result.value, entry.value))) {
      snprintf(m.error, m.capacity, "Notes migration conflict/unavailable; original notes retained"); return false;
    }
    if (result.found) return true;
    request.kind = BotIoRequest::Put; strcpy(request.value, entry.value);
    m.store.perform(entry.bot, request, result, m.generation, m.shared, m.grant);
    if (!result.ok || result.outcome != BotIoResult::Committed) {
      snprintf(m.error, m.capacity, "Notes migration write unconfirmed; original notes retained"); return false;
    }
    return true;
  }, &migration);
  if (!copied) {
    if (!error[0]) snprintf(error, capacity, "Original notes unavailable; restore/provision notes before migration");
    return false;
  }
  memcpy(marker, "PNM\1", 4);
  if (!mastRecord("pine-notes", marker, sizeof(marker), true, present)) return false;
  uint8_t actual[4]{};
  return mastRecord("pine-notes", actual, sizeof(actual), false, present) && present && !memcmp(marker, actual, 4);
}
}
#endif
