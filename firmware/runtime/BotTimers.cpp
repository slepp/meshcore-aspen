// SPDX-License-Identifier: Apache-2.0
#if defined(MESHCORE_ONCHIP_BOT) && MESHCORE_ONCHIP_BOT
#include "BotTimers.h"
#include "BotJournal.h"
#include "BotScheduleFiles.h"
#include "Clock.h"
#include "RoleStorage.h"
#include <algorithm>

namespace onchip {
bool botTimerClaimCurrent(uint32_t due, char *error, size_t capacity) {
  uint32_t earliest, latest;
  if (!trustedNetworkTime(earliest, latest) || earliest < due || earliest - due > BotTimerOverdueSeconds) {
    snprintf(error, capacity, "Timer claimed but clock no longer permits delivery; claim is not replayed");
    return false;
  }
  return true;
}
struct BotTimers::Record {
  uint8_t magic[4]{}, bot[32]{}, principal[32]{};
  char name[BotKeyLimit + 1]{};
  uint8_t scope = 0, state = 0, reserved = 0;
  uint32_t due = 0, revision = 0;
  uint8_t digest[32]{};
};
struct BotTimers::Restore {
  Record records[BotTimerSlots + BotTimerReservedSlots];
  bool occupied[BotTimerSlots + BotTimerReservedSlots]{};
};
struct BotTimers::Storage : BotScheduleFiles<Record, BotTimerSlots + BotTimerReservedSlots> {
  Storage() : BotScheduleFiles("mc-bot-timer", "timers", "BTF\1", "BTI\1") {}
};
BotTimers::~BotTimers() { releaseRoleStorage(storage_); releaseRoleStorage(restore_); }
namespace {
void timerKey(unsigned i, char key[16]) {
  snprintf(key, 16, "%c%02u", i >= BotTimerSlots ? 'r' : 't',
           i >= BotTimerSlots ? i - BotTimerSlots : i);
}
}
bool BotTimers::read(char *error, size_t capacity) {
  static_assert(sizeof(Record) == 144, "Durable timer record format changed");
#ifdef ARDUINO_ARCH_ESP32
  static_assert(sizeof(Storage) == 2972, "Recheck timer storage RAM budget");
#endif
  if (!storage_) storage_ = allocateRoleStorage<Storage>("bot timer files");
  if (!storage_) { snprintf(error, capacity, "Timer storage RAM unavailable"); return false; }
  const auto valid = [&](const Record *records) {
    const Record empty{};
    for (unsigned i = 0; i < BotTimerSlots + BotTimerReservedSlots; ++i) {
      const auto &r = records[i];
      if (!memcmp(&r, &empty, sizeof(r))) continue;
      if (!validRecord(r) || (i >= BotTimerSlots && r.scope != BotIoRequest::Bot)) return false;
      for (unsigned j = 0; j < i; ++j) {
        const auto &other = records[j];
        if (other.revision && r.scope == other.scope && !memcmp(r.bot, other.bot, 32) &&
            !memcmp(r.principal, other.principal, 32) && !strcmp(r.name, other.name)) return false;
      }
    }
    return true;
  };
  return storage_->recover(valid, timerKey, error, capacity);
}
bool BotTimers::validRecord(const Record &record) {
  uint8_t digest[32];
  mesh::Utils::sha256(digest, sizeof(digest), reinterpret_cast<const uint8_t *>(&record), offsetof(Record, digest));
  return !memcmp(record.magic, "BTM\1", 4) && !memcmp(record.digest, digest, 32) &&
      !record.reserved && record.revision && record.scope <= BotIoRequest::Channel &&
      record.name[0] && memchr(record.name, 0, sizeof(record.name)) &&
      record.state >= uint8_t(BotTimerState::Pending) && record.state <= uint8_t(BotTimerState::Overdue) &&
      record.due >= 1715770351u && record.due <= 4102444800u;
}
bool BotTimers::validSnapshot(const BotStore::Snapshot &data, const uint8_t bot[32], char *error, size_t capacity) {
  if (!BotStore::validSnapshotEnvelope(data, bot, "BTD\1", sizeof(Record),
                                       BotTimerSlots + BotTimerReservedSlots, error, capacity)) return false;
  const auto *bytes = reinterpret_cast<const uint8_t *>(data.entries);
  Record record{}, previous{};
  for (unsigned i = 0; i < data.count; ++i) {
    memcpy(&record, bytes + i * sizeof(record), sizeof(record));
    if (!validRecord(record) || memcmp(record.bot, bot, 32) || record.scope != data.scope ||
        memcmp(record.principal, data.principal, 32) || (i && strcmp(previous.name, record.name) >= 0)) {
      snprintf(error, capacity, "Timer snapshot record, identity, scope or ordering invalid"); return false;
    }
    previous = record;
  }
  return true;
}
bool BotTimers::snapshot(const uint8_t bot[32], BotStore::Snapshot &data, char *error, size_t capacity) {
  const auto scope = data.scope;
  uint8_t principal[32]; memcpy(principal, data.principal, 32);
  data = {}; data.scope = scope; memcpy(data.principal, principal, 32);
  memcpy(data.bot, bot, 32); memcpy(data.magic, "BTD\1", 4);
  auto *bytes = reinterpret_cast<uint8_t *>(data.entries);
  if (!read(error, capacity)) return false;
  {
    for (unsigned i = 0; i < BotTimerSlots + BotTimerReservedSlots; ++i) {
      const auto &record = storage_->records[i];
      Record other{};
      if (!record.revision) continue;
      if (memcmp(record.bot, bot, 32) || record.scope != scope || memcmp(record.principal, principal, 32)) continue;
      unsigned at = 0;
      for (; at < data.count; ++at) {
        memcpy(&other, bytes + at * sizeof(other), sizeof(other));
        if (strcmp(other.name, record.name) >= 0) break;
      }
      if (at < data.count && !strcmp(other.name, record.name)) {
        snprintf(error, capacity, "Timer export duplicate name"); return false;
      }
      memmove(bytes + (at + 1) * sizeof(record), bytes + at * sizeof(record), (data.count - at) * sizeof(record));
      memcpy(bytes + at * sizeof(record), &record, sizeof(record)); ++data.count;
    }
  }
  mesh::Utils::sha256(data.digest, 32, reinterpret_cast<const uint8_t *>(&data), offsetof(BotStore::Snapshot, digest));
  return validSnapshot(data, bot, error, capacity);
}
void BotTimers::restore(const uint8_t bot[32], const BotStore::Snapshot &data, BotIoResult &result,
                       uint32_t epoch, const std::atomic<uint32_t> &generation) {
  resetBotIoResult(result);
  const auto fail = [&](const char *message) {
    if (message != result.error) snprintf(result.error, sizeof(result.error), "%s", message);
    Serial.printf("On-chip bot timer restore: %s\n", message);
  };
  if (!validSnapshot(data, bot, result.error, sizeof(result.error))) { fail(result.error); return; }
  if (epoch != generation.load()) { fail("Timer restore cancelled before admission"); return; }
  if (!data.count) { result.ok = true; result.outcome = BotIoResult::Committed; return; }
  if (!restore_) restore_ = allocateRoleStorage<Restore>("timer restore plan");
  if (!restore_) { fail("Timer restore storage RAM unavailable"); return; }
  auto &plan = *restore_; plan = {};
  if (!read(result.error, sizeof(result.error))) { fail(result.error); return; }
  uint32_t revision = 0;
  const uint32_t started = millis();
  for (unsigned i = 0; i < BotTimerSlots + BotTimerReservedSlots; ++i) {
    if (epoch != generation.load() || uint32_t(millis() - started) >= 2000) {
      fail("Timer restore read cancelled/deadline"); return;
    }
    auto &record = plan.records[i]; record = storage_->records[i];
    if (!record.revision) continue;
    plan.occupied[i] = true; revision = std::max(revision, record.revision);
  }
  const auto *bytes = reinterpret_cast<const uint8_t *>(data.entries);
  for (unsigned i = 0; i < data.count; ++i) {
    Record record; memcpy(&record, bytes + i * sizeof(record), sizeof(record));
    revision = std::max(revision, record.revision);
  }
  if (revision > UINT32_MAX - data.count) { fail("Timer restore revision space exhausted"); return; }
  for (unsigned i = 0; i < data.count; ++i) {
    Record record; memcpy(&record, bytes + i * sizeof(record), sizeof(record));
    int matched = -1, free = -1;
    for (unsigned j = 0; j < BotTimerSlots + BotTimerReservedSlots; ++j) {
      const auto &live = plan.records[j];
      if (!plan.occupied[j]) {
        if ((j >= BotTimerSlots) == (data.scope == BotIoRequest::Bot) && free < 0) free = int(j);
      } else if (live.scope == data.scope && !memcmp(live.bot, bot, 32) &&
                 !memcmp(live.principal, data.principal, 32) && !strcmp(live.name, record.name)) matched = int(j);
    }
    if (matched < 0 && free < 0) { fail("Timer restore capacity exhausted; live records are not evicted"); return; }
    const unsigned slot = unsigned(matched < 0 ? free : matched);
    if (matched >= 0) record = plan.records[slot];
    if (record.state == uint8_t(BotTimerState::Pending)) record.state = uint8_t(BotTimerState::Cancelled);
    record.revision = ++revision;
    mesh::Utils::sha256(record.digest, 32, reinterpret_cast<const uint8_t *>(&record), offsetof(Record, digest));
    plan.records[slot] = record; plan.occupied[slot] = true;
  }
  if (!admitScheduleRestore(BotScheduleAuthorityEntries, data.scope != BotIoRequest::Bot, result.error, sizeof(result.error))) {
    fail(result.error); return;
  }
  const auto current = [&] { return epoch == generation.load(); };
  if (!current()) { fail("Timer restore cancelled before mutation"); return; }
  storage_->invalidate();
  memcpy(storage_->records, plan.records, sizeof(plan.records));
  if (!storage_->publish(result, started, current)) { fail(result.error); return; }
  result.ok = true; result.outcome = BotIoResult::Committed;
}
void BotTimers::perform(const uint8_t bot[32], const BotIoRequest &request, BotIoResult &result,
                        const std::atomic<uint32_t> &generation, const std::atomic<bool> &sharedState,
                        const std::atomic<uint32_t> &sharedGrant, const std::atomic<uint32_t> *eventEpoch) {
  static_assert(sizeof(Record) == 144, "Durable timer record format changed");
  resetBotIoResult(result); result.token = request.token;
  const auto fail = [&](const char *message) {
    snprintf(result.error, sizeof(result.error), "%s", message);
    Serial.printf("On-chip bot timer: %s\n", message);
  };
  const auto current = [&]() {
    return botEventCurrent(request, eventEpoch) && request.token.generation == generation.load() &&
        (!request.sharedScope() || (sharedState.load() && request.grant == sharedGrant.load()));
  };
  if (!bot || !current()) { fail("Timer cancelled or shared grant revoked"); return; }
  if (request.kind < BotIoRequest::TimerSet || request.kind > BotIoRequest::TimerWait ||
      request.scope > BotIoRequest::Channel || !request.key[0] ||
      !memchr(request.key, 0, sizeof(request.key)) ||
      (request.kind == BotIoRequest::TimerSet &&
       (!request.delaySeconds || request.delaySeconds > BotTimerMaximumSeconds))) {
    fail("Invalid bounded durable timer operation"); return;
  }
  if (!read(result.error, sizeof(result.error))) return;
  const uint32_t started = millis();
  Record record{};
  const uint8_t magic[] = {'B', 'T', 'M', 1};
  int matched = -1, free = -1, reusable = -1;
  unsigned owned = 0;
  uint32_t latestRevision = 0;
  Record saved{};
  for (unsigned i = 0; i < BotTimerSlots + BotTimerReservedSlots; ++i) {
    if (!current() || uint32_t(millis() - started) >= 2000) {
      fail("Timer cancelled/deadline during read"); return;
    }
    const bool reserved = i >= BotTimerSlots;
    const bool allocatable = reserved == (request.scope == BotIoRequest::Bot);
    record = storage_->records[i];
    if (!record.revision) {
      if (allocatable && free < 0) free = i;
      continue;
    }
    latestRevision = std::max(latestRevision, record.revision);
    const bool pending = record.state == uint8_t(BotTimerState::Pending);
    if (allocatable && !pending && reusable < 0) reusable = i;
    if (record.scope == request.scope && !memcmp(record.bot, bot, 32) &&
        !memcmp(record.principal, request.principal, 32)) {
      if (pending) ++owned;
      if (!strcmp(record.name, request.key)) {
        if (matched >= 0) { fail("Timer contains duplicate durable names"); return; }
        matched = i; saved = record;
      }
    }
  }
  uint32_t earliest = 0, latest = 0;
  const char *reason = nullptr;
  result.timeTrusted = trustedNetworkTime(earliest, latest, &reason);
  if (!current() || uint32_t(millis() - started) >= 2000) {
    fail("Timer read cancelled/deadline"); return;
  }
  result.found = matched >= 0;
  result.timerState = result.found ? BotTimerState(saved.state) : BotTimerState::Missing;
  result.deadlineUtc = saved.due; result.revision = saved.revision;
  if (request.kind == BotIoRequest::TimerGet) {
    result.ok = true; return;
  }
  if (request.kind == BotIoRequest::TimerWait) {
    if (!result.found) { fail("Timer name not found"); return; }
    if (request.revision && request.revision != saved.revision) {
      fail("Timer replaced/cancelled/claimed while waiting"); return;
    }
    if (result.timerState != BotTimerState::Pending) {
      fail(result.timerState == BotTimerState::Claimed ? "Timer already claimed; delivery not guaranteed" :
           result.timerState == BotTimerState::Cancelled ? "Timer cancelled" : "Timer overdue");
      return;
    }
    if (!result.timeTrusted || earliest < saved.due) {
      result.pending = result.ok = true; return;
    }
  }
  if (request.kind == BotIoRequest::TimerCancel &&
      (!result.found || result.timerState != BotTimerState::Pending)) {
    result.ok = true; return;
  }
  if (latestRevision == UINT32_MAX) {
    fail("Timer revision space exhausted"); return;
  }
  record = saved;
  if (request.kind == BotIoRequest::TimerSet) {
    if (request.scope != BotIoRequest::Bot && !admitPublicBotStorage(result.error, sizeof(result.error))) {
      Serial.printf("On-chip bot timer: %s\n", result.error); return;
    }
    if (!result.timeTrusted) { fail(reason); return; }
    if (latest > 4102444800u - request.delaySeconds) {
      fail("Timer deadline outside trusted epoch range"); return;
    }
    if ((!result.found || result.timerState != BotTimerState::Pending) && owned >= BotTimersPerScope) {
      fail("Timer scope quota exhausted (2 pending)"); return;
    }
    if (matched < 0) matched = free >= 0 ? free : reusable;
    if (matched < 0) { fail("Timer capacity exhausted (8 public + 2 bot-reserved)"); return; }
    record = {};
    memcpy(record.magic, magic, sizeof(magic)); memcpy(record.bot, bot, 32);
    memcpy(record.principal, request.principal, 32);
    strcpy(record.name, request.key); record.scope = request.scope;
    record.state = uint8_t(BotTimerState::Pending);
    record.due = latest + request.delaySeconds;
    result.replaced = result.found;
  } else if (request.kind == BotIoRequest::TimerCancel) {
    record.state = uint8_t(BotTimerState::Cancelled);
  } else {
    record.state = uint8_t(earliest - saved.due > BotTimerOverdueSeconds ?
                          BotTimerState::Overdue : BotTimerState::Claimed);
  }
  record.revision = latestRevision + 1;
  storage_->invalidate(); storage_->records[matched] = record;
  if (!storage_->publish(result, started, current)) {
    Serial.printf("On-chip bot timer: %s\n", result.error); return;
  }
  result.found = true; result.timerState = BotTimerState(record.state);
  result.deadlineUtc = record.due; result.revision = record.revision;
  if (request.kind == BotIoRequest::TimerWait) {
    if (result.timerState == BotTimerState::Overdue) { fail("Timer overdue; no delivery claim"); return; }
    if (!botTimerClaimCurrent(record.due, result.error, sizeof(result.error))) {
      Serial.printf("On-chip bot timer: %s\n", result.error); return;
    }
    if (uint32_t(millis() - started) >= 2000) {
      fail("Timer committed after deadline; do not retry blindly"); return;
    }
  }
  result.ok = true;
}
}
#endif
