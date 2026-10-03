// SPDX-License-Identifier: Apache-2.0
#if defined(MESHCORE_ONCHIP_BOT) && MESHCORE_ONCHIP_BOT
#include "BotReminders.h"
#include "BotJournal.h"
#include "BotScheduleFiles.h"
#include "Clock.h"
#include "RoleStorage.h"
#include <algorithm>

namespace onchip {
struct BotReminders::Record {
  uint8_t magic[4]{}, bot[32]{}, principal[32]{};
  uint32_t id = 0, revision = 0, due = 0, sourceGeneration = 0;
  uint8_t state = 0, reserved[3]{};
  char text[BotReminderTextLimit + 4]{};
  uint8_t digest[32]{};
};
struct BotReminders::Storage : BotScheduleFiles<Record, BotReminderSlots> {
  Storage() : BotScheduleFiles("mc-bot-remind", "reminders", "BRF\1", "BRI\1") {}
};
namespace {
void reminderKey(unsigned slot, char key[16]) { snprintf(key, 16, "r%02u", slot); }
bool failure(char *error, size_t capacity, const char *message) {
  snprintf(error, capacity, "%s", message);
  return false;
}
bool printable(const char *text, size_t limit) {
  const size_t size = strnlen(text, limit + 1);
  if (!size || size > limit) return false;
  for (size_t i = 0; i < size; ++i) if (text[i] < 32 || text[i] > 126) return false;
  return true;
}
}
BotReminders::~BotReminders() { releaseRoleStorage(storage_); }
bool BotReminders::validRecord(const Record &record) {
  uint8_t digest[32];
  mesh::Utils::sha256(digest, sizeof(digest), reinterpret_cast<const uint8_t *>(&record), offsetof(Record, digest));
  return !memcmp(record.magic, "BRM\1", 4) && !memcmp(record.digest, digest, 32) &&
      record.id && record.revision >= record.id && record.sourceGeneration &&
      record.due >= 1715770351u && record.due <= 4102444800u &&
      record.state >= uint8_t(BotReminderState::Pending) && record.state <= uint8_t(BotReminderState::Overdue) &&
      !record.reserved[0] && !record.reserved[1] && !record.reserved[2] &&
      printable(record.text, BotReminderTextLimit);
}
bool BotReminders::validSnapshot(const BotStore::Snapshot &data, const uint8_t bot[32], char *error, size_t capacity) {
  if (!BotStore::validSnapshotEnvelope(data, bot, "BRD\1", sizeof(Record), BotReminderSlots, error, capacity)) return false;
  if (data.scope != BotIoRequest::Caller) {
    snprintf(error, capacity, "Reminder snapshots require authenticated caller scope"); return false;
  }
  const auto *bytes = reinterpret_cast<const uint8_t *>(data.entries);
  uint32_t previous = 0;
  for (unsigned i = 0; i < data.count; ++i) {
    Record record; memcpy(&record, bytes + i * sizeof(record), sizeof(record));
    if (!validRecord(record) || memcmp(record.bot, bot, 32) || memcmp(record.principal, data.principal, 32) ||
        record.id <= previous) {
      snprintf(error, capacity, "Reminder snapshot record, identity or ordering invalid"); return false;
    }
    previous = record.id;
  }
  return true;
}
bool BotReminders::snapshot(const uint8_t bot[32], BotStore::Snapshot &data, char *error, size_t capacity) {
  const auto scope = data.scope;
  uint8_t principal[32]; memcpy(principal, data.principal, 32);
  data = {}; data.scope = scope; memcpy(data.principal, principal, 32);
  memcpy(data.bot, bot, 32); memcpy(data.magic, "BRD\1", 4);
  if (!read(error, capacity)) return false;
  auto *bytes = reinterpret_cast<uint8_t *>(data.entries);
  for (unsigned i = 0; i < BotReminderSlots; ++i) {
    const auto &record = records_[i];
    if (!record.id || memcmp(record.bot, bot, 32) || memcmp(record.principal, principal, 32)) continue;
    unsigned at = 0; Record other{};
    for (; at < data.count; ++at) {
      memcpy(&other, bytes + at * sizeof(other), sizeof(other));
      if (other.id > record.id) break;
    }
    memmove(bytes + (at + 1) * sizeof(record), bytes + at * sizeof(record), (data.count - at) * sizeof(record));
    memcpy(bytes + at * sizeof(record), &record, sizeof(record)); ++data.count;
  }
  mesh::Utils::sha256(data.digest, 32, reinterpret_cast<const uint8_t *>(&data), offsetof(BotStore::Snapshot, digest));
  return validSnapshot(data, bot, error, capacity);
}
void BotReminders::restore(const uint8_t bot[32], const BotStore::Snapshot &data, BotIoResult &result,
                          uint32_t epoch, const std::atomic<uint32_t> &generation) {
  resetBotIoResult(result);
  const auto fail = [&](const char *message) {
    if (message != result.error) snprintf(result.error, sizeof(result.error), "%s", message);
    Serial.printf("On-chip bot reminder restore: %s\n", message);
  };
  if (!validSnapshot(data, bot, result.error, sizeof(result.error))) { fail(result.error); return; }
  if (epoch != generation.load()) { fail("Reminder restore cancelled before admission"); return; }
  if (!data.count) { result.ok = true; result.outcome = BotIoResult::Committed; return; }
  if (!read(result.error, sizeof(result.error))) { fail(result.error); return; }
  uint32_t revision = 0;
  for (unsigned i = 0; i < BotReminderSlots; ++i) revision = std::max(revision, records_[i].revision);
  const auto *bytes = reinterpret_cast<const uint8_t *>(data.entries);
  for (unsigned i = 0; i < data.count; ++i) {
    Record record; memcpy(&record, bytes + i * sizeof(record), sizeof(record));
    revision = std::max(revision, record.revision);
  }
  if (revision > UINT32_MAX - data.count) { fail("Reminder restore revision space exhausted"); return; }
  storage_->invalidate();
  for (unsigned i = 0; i < data.count; ++i) {
    Record record; memcpy(&record, bytes + i * sizeof(record), sizeof(record));
    int matched = -1, free = -1;
    for (unsigned j = 0; j < BotReminderSlots; ++j) {
      if (!records_[j].id && free < 0) free = int(j);
      else if (records_[j].id == record.id) matched = int(j);
    }
    if (matched >= 0 && (memcmp(records_[matched].bot, bot, 32) ||
                         memcmp(records_[matched].principal, data.principal, 32))) {
      fail("Reminder restore ID conflicts with another bot/principal"); return;
    }
    if (matched < 0 && free < 0) { fail("Reminder restore capacity exhausted; live records are not evicted"); return; }
    const unsigned slot = unsigned(matched < 0 ? free : matched);
    if (matched >= 0) record = records_[slot];
    if (record.state == uint8_t(BotReminderState::Pending)) record.state = uint8_t(BotReminderState::Cancelled);
    record.revision = ++revision;
    mesh::Utils::sha256(record.digest, 32, reinterpret_cast<const uint8_t *>(&record), offsetof(Record, digest));
    records_[slot] = record;
  }
  if (!admitScheduleRestore(BotScheduleAuthorityEntries, true, result.error, sizeof(result.error))) { fail(result.error); return; }
  const auto current = [&] { return epoch == generation.load(); };
  if (!current()) { fail("Reminder restore cancelled before mutation"); return; }
  if (!storage_->publish(result, millis(), current)) { fail(result.error); return; }
  result.ok = true; result.outcome = BotIoResult::Committed;
}
bool BotReminders::read(char *error, size_t capacity) {
  static_assert(sizeof(Record) == 244, "Reminder journal record changed");
#ifdef ARDUINO_ARCH_ESP32
  static_assert(sizeof(Storage) == 3996, "Recheck reminder storage RAM budget");
#endif
  if (!records_) {
    storage_ = allocateRoleStorage<Storage>("bot durable reminders");
    if (storage_) records_ = storage_->records;
  }
  if (!records_) return failure(error, capacity, "Reminder storage RAM unavailable");
  const auto valid = [&](const Record *records) {
    const Record empty{};
    for (unsigned i = 0; i < BotReminderSlots; ++i) {
      const auto &r = records[i];
      if (!memcmp(&r, &empty, sizeof(r))) continue;
      if (!validRecord(r)) return false;
      for (unsigned j = 0; j < i; ++j) if (records[j].id == r.id) return false;
    }
    return true;
  };
  return storage_->recover(valid, reminderKey, error, capacity);
}
bool BotReminders::transition(unsigned slot, BotReminderState state,
                             const std::atomic<bool> &stopping, char *error, size_t capacity,
                             BotIoResult *outcome) {
  uint32_t revision = 0;
  for (unsigned i = 0; i < BotReminderSlots; ++i) revision = std::max(revision, records_[i].revision);
  if (revision == UINT32_MAX) return failure(error, capacity, "Reminder revision exhausted");
  storage_->invalidate();
  auto &record = records_[slot];
  record.revision = revision + 1; record.state = uint8_t(state);
  const auto current = [&] {
    if (stopping.load()) return false;
    if (state != BotReminderState::Unknown && state != BotReminderState::Overdue) return true;
    uint32_t earliest, latest;
    if (!trustedNetworkTime(earliest, latest) || earliest < record.due) return false;
    return (earliest - record.due > BotTimerOverdueSeconds) == (state == BotReminderState::Overdue);
  };
  BotSchedulePublication result;
  const bool ok = storage_->publish(result, millis(), current);
  if (outcome) outcome->outcome = result.outcome;
  if (!ok) snprintf(error, capacity, "%s", result.error);
  return ok;
}
void BotReminders::perform(const uint8_t bot[32], const BotIoRequest &request, BotIoResult &result,
                          const std::atomic<uint32_t> &generation, const std::atomic<bool> &enabled,
                          const std::atomic<uint32_t> &grant, const std::atomic<bool> &stopping,
                          uint32_t protectedId) {
  resetBotIoResult(result); result.token = request.token;
  const auto fail = [&](const char *message) { failure(result.error, sizeof(result.error), message); };
  const auto current = [&] {
    return !stopping.load() && request.token.generation == generation.load() &&
        (request.kind != BotIoRequest::ReminderSet || (enabled.load() && request.grant == grant.load()));
  };
  uint8_t principalBits = 0;
  for (auto byte : request.principal) principalBits |= byte;
  if (!bot || !current() || !principalBits || request.scope != BotIoRequest::Caller || !request.reminder()) {
    fail("Personal reminder denied/cancelled; authenticated private DM and live grant required"); return;
  }
  if (!read(result.error, sizeof(result.error))) return;
  int free = -1, reusable = -1, matched = -1;
  unsigned owned = 0;
  uint32_t revision = 0;
  size_t listed = 0;
  for (unsigned i = 0; i < BotReminderSlots; ++i) {
    auto &record = records_[i];
    revision = std::max(revision, record.revision);
    if (!record.id) { if (free < 0) free = i; continue; }
    if (record.state != uint8_t(BotReminderState::Pending) && record.id != protectedId && reusable < 0) reusable = i;
    if (memcmp(record.bot, bot, 32) || memcmp(record.principal, request.principal, 32)) continue;
    if (record.state == uint8_t(BotReminderState::Pending)) ++owned;
    if (record.id == request.revision) matched = i;
    if (request.kind == BotIoRequest::ReminderList) {
      char part[64];
      snprintf(part, sizeof(part), "%s%u %s @%u", listed ? "; " : "",
               record.id, botReminderStateName(BotReminderState(record.state)), record.due);
      if (listed + strlen(part) + 11 <= BotReplyLimit) {
        strcpy(result.value + listed, part); listed += strlen(part);
      } else result.truncated = true;
    }
  }
  if (!current()) { fail("Reminder cancelled before mutation"); return; }
  if (request.kind == BotIoRequest::ReminderList) {
    if (result.truncated) strcat(result.value, "; truncated");
    if (!result.value[0]) strcpy(result.value, "No personal reminders");
    result.ok = true; return;
  }
  if (request.kind == BotIoRequest::ReminderCancel) {
    if (matched < 0) { fail("Personal reminder ID not found"); return; }
    const auto state = BotReminderState(records_[matched].state);
    if (state == BotReminderState::Pending &&
        !transition(unsigned(matched), BotReminderState::Cancelled, stopping, result.error, sizeof(result.error), &result)) return;
    result.reminderState = BotReminderState(records_[matched].state);
    result.revision = records_[matched].id;
    result.deadlineUtc = records_[matched].due;
    result.ok = current();
    if (!result.ok) fail("Reminder request ended after source cancellation; inspect list");
    return;
  }
  if (!request.delaySeconds || request.delaySeconds > BotTimerMaximumSeconds ||
      !printable(request.value, BotReminderTextLimit)) { fail("Reminder requires 1..86400 seconds and 1..120 printable bytes"); return; }
  if (owned >= BotRemindersPerCaller) { fail("Personal reminder quota exhausted (2 pending)"); return; }
  const int slot = free >= 0 ? free : reusable;
  if (slot < 0 || revision == UINT32_MAX) { fail("Reminder capacity/revision exhausted (8 total)"); return; }
  if (!admitPublicBotStorage(result.error, sizeof(result.error))) return;
  uint32_t earliest, latest;
  const char *reason;
  if (!trustedNetworkTime(earliest, latest, &reason)) { fail(reason); return; }
  if (latest > 4102444800u - request.delaySeconds) { fail("Reminder deadline outside trusted epoch"); return; }
  storage_->invalidate();
  auto &record = records_[slot];
  record = {};
  memcpy(record.magic, "BRM\1", 4); memcpy(record.bot, bot, 32); memcpy(record.principal, request.principal, 32);
  record.id = record.revision = revision + 1; record.sourceGeneration = request.token.generation;
  record.due = latest + request.delaySeconds; record.state = uint8_t(BotReminderState::Pending);
  strcpy(record.text, request.value);
  if (!storage_->publish(result, millis(), current)) return;
  result.revision = record.id; result.deadlineUtc = record.due;
  result.reminderState = BotReminderState::Pending; result.ok = true;
}
bool BotReminders::next(const uint8_t bot[32], BotReminderDispatch &dispatch,
                        const std::atomic<bool> &stopping, char *error, size_t capacity) {
  error[0] = 0; dispatch = {};
  uint32_t earliest, latest;
  if (!trustedNetworkTime(earliest, latest) || stopping.load()) return false;
  if (!read(error, capacity)) return false;
  for (unsigned n = 0; n < BotReminderSlots; ++n) {
    const unsigned i = (nextSlot_ + n) % BotReminderSlots;
    const auto &record = records_[i];
    if (!record.id || memcmp(record.bot, bot, 32) || record.state != uint8_t(BotReminderState::Pending) ||
        earliest < record.due) continue;
    if (earliest - record.due > BotTimerOverdueSeconds) {
      if (!transition(i, BotReminderState::Overdue, stopping, error, capacity)) return false;
      continue;
    }
    memcpy(dispatch.principal, record.principal, 32); strcpy(dispatch.text, record.text);
    dispatch.id = record.id; dispatch.revision = record.revision;
    dispatch.due = record.due; dispatch.sourceGeneration = record.sourceGeneration;
    nextSlot_ = (i + 1) % BotReminderSlots;
    return true;
  }
  return false;
}
bool BotReminders::claim(const uint8_t bot[32], BotReminderDispatch &dispatch,
                         const std::atomic<bool> &enabled, const std::atomic<uint32_t> &grant,
                         const std::atomic<bool> &stopping, char *error, size_t capacity) {
  if (!enabled.load() || grant.load() != dispatch.grant || stopping.load())
    return failure(error, capacity, "Reminder grant changed before claim");
  if (!read(error, capacity)) return false;
  for (unsigned i = 0; i < BotReminderSlots; ++i) {
    auto &record = records_[i];
    if (record.id != dispatch.id || memcmp(record.bot, bot, 32)) continue;
    if (record.revision != dispatch.revision || record.state != uint8_t(BotReminderState::Pending))
      return failure(error, capacity, "Reminder cancelled/replaced before claim");
    if (!botTimerClaimCurrent(record.due, error, capacity)) return false;
    if (!transition(i, BotReminderState::Unknown, stopping, error, capacity)) return false;
    dispatch.revision = record.revision;
    if (!enabled.load() || grant.load() != dispatch.grant || stopping.load())
      return failure(error, capacity, "Reminder claimed during grant change; unknown, not replayed");
    return botTimerClaimCurrent(record.due, error, capacity);
  }
  return failure(error, capacity, "Reminder disappeared before claim");
}
bool BotReminders::complete(const uint8_t bot[32], const BotReminderDispatch &dispatch,
                            const std::atomic<bool> &stopping, char *error, size_t capacity) {
  if (!read(error, capacity)) return false;
  for (unsigned i = 0; i < BotReminderSlots; ++i) {
    const auto &record = records_[i];
    if (record.id != dispatch.id || memcmp(record.bot, bot, 32)) continue;
    if (record.revision != dispatch.revision || record.state != uint8_t(BotReminderState::Unknown))
      return failure(error, capacity, "Reminder late/stale completion; not replayed");
    if (dispatch.outcome == BotReminderState::Unknown) return true;
    if (dispatch.outcome != BotReminderState::Sent)
      return failure(error, capacity, "Invalid reminder TX outcome");
    return transition(i, BotReminderState::Sent, stopping, error, capacity);
  }
  return failure(error, capacity, "Reminder completion ID unavailable");
}
}
#endif
