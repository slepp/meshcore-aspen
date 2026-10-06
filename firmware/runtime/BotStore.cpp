// SPDX-License-Identifier: Apache-2.0
#if defined(MESHCORE_ONCHIP_BOT) && MESHCORE_ONCHIP_BOT
#include "BotStore.h"
#include "BotJournal.h"
#include "RoleStorage.h"
#include <Utils.h>
#include <nvs.h>
#include <SPIFFS.h>

namespace onchip {
struct BotStore::Record {
  uint8_t magic[4]{}, bot[32]{};
  struct Entry {
    uint8_t used = 0, scope = 0, principal[32]{};
    char key[BotKeyLimit + 1]{}, value[BotValueLimit + 1]{};
  } entry;
  uint8_t digest[32]{};
};
struct BotStore::Journal {
  uint8_t magic[4]{}, count = 0, slots[BotKeysPerScope]{}, reserved[3]{};
  Record records[BotKeysPerScope]{};
  uint8_t digest[32]{};
};
namespace {
#if ONCHIP_BOT_COMPACT_PROFILE
constexpr unsigned Shards = 3;
#else
constexpr unsigned Shards = 5;
#endif
constexpr unsigned ShardSlots = 8, Slots = Shards * ShardSlots, PrivateSlots = Slots - BotKeysPerScope;
struct Authority {
  uint8_t magic[4]{'B', 'K', 'S', 1}, banks = 0, reclaim = 0, reserved[2]{};
  uint8_t hashes[Shards][32]{}, digest[32]{};
};
static_assert(sizeof(Authority) == 40 + 32 * Shards, "Recheck KV authority NVS budget");
static_assert(BotKvAuthorityEntries == (sizeof(Authority) + 31) / 32 + 2 &&
              BotKvBootstrapEntries == (48 + 31) / 32 + 2, "KV metadata entry budget changed");
void fileName(unsigned shard, unsigned bank, char path[40]) {
  snprintf(path, 40, "/command-bot/kv-%u-%c.bin", shard, bank ? 'b' : 'a');
}
}
struct BotStore::Files {
  Authority authority;
  Record records[Slots], check[ShardSlots];
  bool present = false, initialized = false, namespacePresent = false, verified = false;
};
BotStore::~BotStore() {
  releaseRoleStorage(record_); releaseRoleStorage(journal_); releaseRoleStorage(files_);
}
namespace {
void slotName(unsigned slot, char key[16]) {
  snprintf(key, 16, "%c%02u", slot >= PrivateSlots ? 'r' : 'v', slot >= PrivateSlots ? slot - PrivateSlots : slot);
}
bool storageError(char *error, size_t capacity, const char *message) {
  snprintf(error, capacity, "%s", message);
  Serial.printf("On-chip bot KV: %s\n", message);
  return false;
}
bool checkedRecord(const void *record, size_t size) {
  uint8_t digest[32];
  const auto *bytes = static_cast<const uint8_t *>(record);
  mesh::Utils::sha256(digest, sizeof(digest), bytes, size - sizeof(digest));
  return !memcmp(digest, bytes + size - sizeof(digest), sizeof(digest));
}
void seal(void *record, size_t size) {
  auto *bytes = static_cast<uint8_t *>(record);
  mesh::Utils::sha256(bytes + size - 32, 32, bytes, size - 32);
}
bool validAuthority(const Authority &a) {
  return !memcmp(a.magic, "BKS\1", 4) && a.banks < (1 << Shards) &&
      a.reclaim <= 1 && !a.reserved[0] && !a.reserved[1] && checkedRecord(&a, sizeof(a));
}
}
bool BotStore::admitJournal(bool publicScope, char *error, size_t capacity) {
  static_assert(sizeof(Record) == 136 + BotValueLimit &&
                sizeof(Journal) == 48 + BotKeysPerScope * sizeof(Record), "Recheck NVS entry budget");
  const size_t recovery = BotKvRecoveryEntries + (files_->initialized ? 0 : BotKvBootstrapEntries) +
      !files_->namespacePresent;
  const size_t required = publicScope && BotKvPublicEntries > recovery ? BotKvPublicEntries : recovery;
  nvs_stats_t stats{};
  if (nvs_get_stats(nullptr, &stats) != ESP_OK)
    return storageError(error, capacity, "KV journal NVS headroom unavailable; not published");
  if (stats.free_entries < required) {
    snprintf(error, capacity, "KV journal NVS headroom: %u free, %u required; not published",
             unsigned(stats.free_entries), unsigned(required));
    Serial.printf("On-chip bot KV: %s\n", error);
    return false;
  }
  return true;
}
bool BotStore::validateRecords(char *error, size_t capacity, bool logical) {
  const Record *empty = nullptr;
  for (unsigned i = 0; i < Slots; ++i) {
    const auto &r = files_->records[i];
    const auto &e = r.entry;
    const bool identical = empty && !memcmp(&r, empty, sizeof(r));
    if (memcmp(r.magic, "BKV\1", 4) || (!identical && !checkedRecord(&r, sizeof(r))) ||
        e.used > 1 || e.scope > BotIoRequest::Channel ||
        !memchr(e.key, 0, sizeof(e.key)) || !memchr(e.value, 0, sizeof(e.value)) ||
        (e.used && (!e.key[0] || (i >= PrivateSlots && e.scope != BotIoRequest::Bot))))
      return storageError(error, capacity, "KV record corrupt/unavailable; storage blocked");
    if (!e.used) { empty = &r; continue; }
    if (!logical) continue;
    unsigned owned = 1;
    for (unsigned j = 0; j < i; ++j) {
      const auto &other = files_->records[j];
      if (!other.entry.used || memcmp(r.bot, other.bot, 32) ||
          e.scope != other.entry.scope || memcmp(e.principal, other.entry.principal, 32)) continue;
      if (!strcmp(e.key, other.entry.key))
        return storageError(error, capacity, "KV duplicate durable key; storage blocked");
      if (++owned > BotKeysPerScope)
        return storageError(error, capacity, "KV scope exceeds bounded capacity; storage blocked");
    }
  }
  return true;
}
bool BotStore::readFiles(uint32_t started, char *error, size_t capacity) {
  for (unsigned shard = 0; shard < Shards; ++shard) {
    if (uint32_t(millis() - started) >= BotKvRecoveryBudgetMs)
      return storageError(error, capacity, "KV recovery file deadline; retry recovery");
    char path[40]; fileName(shard, (files_->authority.banks >> shard) & 1, path);
    auto file = SPIFFS.open(path, "r");
    auto *bytes = reinterpret_cast<uint8_t *>(&files_->records[shard * ShardSlots]);
    const size_t size = sizeof(files_->check);
    const bool complete = file && file.size() == size && file.read(bytes, size) == size &&
        file.size() == size;
    file.close();
    uint8_t hash[32]; mesh::Utils::sha256(hash, sizeof(hash), bytes, size);
    if (!complete || memcmp(hash, files_->authority.hashes[shard], sizeof(hash)))
      return storageError(error, capacity, "KV file corrupt/unavailable; storage blocked");
  }
  if (!validateRecords(error, capacity)) return false;
  return uint32_t(millis() - started) < BotKvRecoveryBudgetMs ||
      storageError(error, capacity, "KV recovery file deadline; retry recovery");
}
bool BotStore::publish(uint8_t changed, BotIoResult::Outcome &outcome, char *error, size_t capacity,
                       uint32_t started, uint32_t budgetMs,
                       uint32_t epoch, const std::atomic<uint32_t> *generation, const BotIoRequest *request,
                       const std::atomic<bool> *sharedState, const std::atomic<uint32_t> *sharedGrant,
                       const std::atomic<uint32_t> *eventEpoch) {
  files_->verified = false;
  const auto current = [&] {
    if (generation && epoch != generation->load()) return false;
    return !request || (botEventCurrent(*request, eventEpoch) &&
        (!request->sharedScope() || (sharedState->load() && request->grant == sharedGrant->load())));
  };
  const auto fail = [&](const char *message) {
    return storageError(error, capacity, message);
  };
  if (!validateRecords(error, capacity)) return false;
  if (!files_->present && budgetMs == 2000) {
    started = millis();
    budgetMs = BotKvRecoveryBudgetMs;
  }
  if (!files_->initialized) {
    // Establish a durable "no file bank published yet" state before creating
    // any files. Missing authority alongside files can then fail closed, rather
    // than confusing lost metadata with an interrupted first write/migration.
    uint8_t bootstrap[48]{}; memcpy(bootstrap, "BKI\1", 4);
    nvs_handle_t handle;
    if (nvs_open("mc-bot-kv", NVS_READWRITE, &handle) != ESP_OK)
      return fail("KV initialization open failed; not published");
    files_->namespacePresent = true;
    outcome = BotIoResult::Unknown;
    if (!commitBotJournal(handle, "mc-bot-kv", "txn", bootstrap, sizeof(bootstrap), started,
                          current, error, capacity, budgetMs)) return false;
    files_->initialized = true;
    outcome = BotIoResult::Rejected;
  }
  // No file is overwritten until committed authority has been read by recover().
  // Every operation (also a retry after uncertainty) starts with that read.
  auto next = files_->authority;
  if (!files_->present) changed = (1 << Shards) - 1;
  for (unsigned shard = 0; shard < Shards; ++shard) if (changed & (1 << shard)) {
    if (!current() || uint32_t(millis() - started) >= budgetMs)
      return fail("KV cancelled/deadline before file publication");
    const unsigned bank = files_->present ? 1 - ((next.banks >> shard) & 1) : 0;
    next.banks = (next.banks & ~(1 << shard)) | (bank << shard);
    char path[40]; fileName(shard, bank, path);
    const auto *bytes = reinterpret_cast<const uint8_t *>(&files_->records[shard * ShardSlots]);
    const size_t size = sizeof(files_->check);
    mesh::Utils::sha256(next.hashes[shard], 32, bytes, size);
    auto file = SPIFFS.open(path, "w");
    if (!file) return fail("KV inactive file open failed; not published");
    const bool written = file.write(bytes, size) == size;
    file.flush(); file.close();
    auto check = SPIFFS.open(path, "r");
    const bool verified = check && check.size() == size &&
        check.read(reinterpret_cast<uint8_t *>(files_->check), size) == size &&
        check.size() == size && !memcmp(bytes, files_->check, size);
    check.close();
    if (!written || !verified) return fail("KV inactive file write/readback failed; not published");
  }
  if (!current() || uint32_t(millis() - started) >= budgetMs)
    return fail("KV cancelled/deadline before authority publication");
  nvs_handle_t handle;
  if (nvs_open("mc-bot-kv", NVS_READWRITE, &handle) != ESP_OK)
    return fail("KV authority open failed; not published");
  outcome = BotIoResult::Unknown;
  if (!commitBotJournal(handle, "mc-bot-kv", "txn", &next, sizeof(next), started,
                        current, error, capacity, budgetMs)) return false;
  files_->authority = next; files_->present = true;
  outcome = BotIoResult::Committed;
  return true;
}
bool BotStore::reclaim(uint32_t started, char *error, size_t capacity) {
  // The verified file authority is already durable. A cut at any erase resumes
  // here, never importing the now-partial legacy bank again.
  for (unsigned slot = 0; slot < Slots; ++slot) {
    if (uint32_t(millis() - started) >= BotKvRecoveryBudgetMs)
      return storageError(error, capacity, "KV migration reclaim deadline; retry recovery");
    nvs_handle_t handle;
    if (nvs_open("mc-bot-kv", NVS_READWRITE, &handle) != ESP_OK)
      return storageError(error, capacity, "KV legacy reclaim open failed; storage blocked");
    char key[16]; slotName(slot, key);
    auto status = nvs_erase_key(handle, key);
    if (status == ESP_ERR_NVS_NOT_FOUND) { nvs_close(handle); continue; }
    if (status == ESP_OK) status = nvs_commit(handle);
    nvs_close(handle);
    if (status != ESP_OK || nvs_open("mc-bot-kv", NVS_READONLY, &handle) != ESP_OK)
      return storageError(error, capacity, "KV legacy reclaim failed; outcome unknown");
    size_t size = 0; status = nvs_get_blob(handle, key, nullptr, &size); nvs_close(handle);
    if (status != ESP_ERR_NVS_NOT_FOUND)
      return storageError(error, capacity, "KV legacy reclaim readback failed; outcome unknown");
  }
  auto next = files_->authority; next.reclaim = 0;
  nvs_handle_t handle;
  if (nvs_open("mc-bot-kv", NVS_READWRITE, &handle) != ESP_OK)
    return storageError(error, capacity, "KV migration completion unavailable");
  if (!commitBotJournal(handle, "mc-bot-kv", "txn", &next, sizeof(next), started,
                        [] { return true; }, error, capacity, BotKvRecoveryBudgetMs)) return false;
  files_->authority = next;
  return true;
}
bool BotStore::recover(char *error, size_t capacity) {
  if (!journal_) journal_ = allocateRoleStorage<Journal>("bot KV transaction");
  if (!files_) files_ = allocateRoleStorage<Files>("bot KV files");
  if (!journal_ || !files_) return storageError(error, capacity, "KV transaction storage RAM unavailable");
  const uint32_t started = millis();
  auto &journal = *journal_;
  const auto previous = files_->authority;
  const bool verified = files_->verified;
  // Only a successful, exact authority read can reuse a verified arena.
  files_->verified = false;
  files_->present = files_->initialized = false; files_->authority = {};
  nvs_handle_t handle;
  auto status = nvs_open("mc-bot-kv", NVS_READONLY, &handle);
  const bool opened = status == ESP_OK;
  files_->namespacePresent = opened;
  if (!opened && status != ESP_ERR_NVS_NOT_FOUND)
    return storageError(error, capacity, "KV recovery open failed; storage blocked");
  journal = {};
  size_t size = sizeof(journal);
  if (opened) status = nvs_get_blob(handle, "txn", &journal, &size);
  if (status == ESP_OK && size == sizeof(Authority)) {
    memcpy(static_cast<void *>(&files_->authority), &journal, sizeof(Authority)); nvs_close(handle);
    if (!validAuthority(files_->authority))
      return storageError(error, capacity, "KV recovery authority corrupt; storage blocked");
    if (uint32_t(millis() - started) >= BotKvRecoveryBudgetMs)
      return storageError(error, capacity, "KV recovery authority deadline; retry recovery");
    files_->present = files_->initialized = true;
    if ((!verified || memcmp(&previous, &files_->authority, sizeof(previous))) &&
        !readFiles(started, error, capacity)) return false;
    if (files_->authority.reclaim && !reclaim(started, error, capacity)) return false;
    files_->verified = true;
    return true;
  }
  const bool bootstrap = status == ESP_OK && size == 48 && !memcmp(journal.magic, "BKI\1", 4);
  if (bootstrap) {
    const auto *bytes = reinterpret_cast<const uint8_t *>(&journal);
    bool valid = checkedRecord(&journal, size);
    for (unsigned i = 4; i < 16; ++i) valid = valid && !bytes[i];
    if (!valid) {
      nvs_close(handle);
      return storageError(error, capacity, "KV initialization marker corrupt; storage blocked");
    }
    files_->initialized = true;
    journal = {};
  }
  const bool legacyJournal = !bootstrap && status != ESP_ERR_NVS_NOT_FOUND;
  if (legacyJournal && (status != ESP_OK || (size != sizeof(journal) && (size != 48 || journal.count)) ||
      memcmp(journal.magic, "BTX\1", 4) ||
      !checkedRecord(&journal, size) || journal.count > BotKeysPerScope ||
      journal.reserved[0] || journal.reserved[1] || journal.reserved[2])) {
    if (opened) nvs_close(handle);
    return storageError(error, capacity, "KV recovery journal corrupt/unavailable; storage blocked");
  }
  if (legacyJournal) files_->initialized = true;
  if (!files_->initialized) {
    for (unsigned shard = 0; shard < Shards; ++shard) for (unsigned bank = 0; bank < 2; ++bank) {
      if (uint32_t(millis() - started) >= BotKvRecoveryBudgetMs) {
        if (opened) nvs_close(handle);
        return storageError(error, capacity, "KV empty-media scan deadline; retry recovery");
      }
      char path[40]; fileName(shard, bank, path);
      if (SPIFFS.exists(path)) {
        if (opened) nvs_close(handle);
        return storageError(error, capacity, "KV authority missing for existing files; storage blocked");
      }
    }
  }
  bool legacy = legacyJournal;
  auto &empty = files_->check[0];
  empty = {};
  memcpy(empty.magic, "BKV\1", 4);
  seal(&empty, sizeof(empty));
  for (unsigned slot = 0; slot < Slots; ++slot) {
    if (uint32_t(millis() - started) >= BotKvRecoveryBudgetMs) {
      if (opened) nvs_close(handle);
      return storageError(error, capacity, "KV legacy migration read deadline; retry recovery");
    }
    auto &record = files_->records[slot]; record = {};
    char key[16]; slotName(slot, key);
    size_t length = sizeof(record);
    status = opened ? nvs_get_blob(handle, key, &record, &length) : ESP_ERR_NVS_NOT_FOUND;
    if (status == ESP_ERR_NVS_NOT_FOUND) {
      record = empty;
    } else if (status != ESP_OK || length != sizeof(record)) {
      nvs_close(handle);
      return storageError(error, capacity, "KV legacy record unavailable; storage blocked");
    } else legacy = true;
  }
  if (opened) nvs_close(handle);
  // Validate all identities/scopes, even records that redo is about to replace.
  if (!validateRecords(error, capacity, false)) return false;
  for (unsigned i = 0; i < journal.count; ++i) {
    const auto &record = journal.records[i];
    if (journal.slots[i] >= Slots || memcmp(record.magic, "BKV\1", 4) ||
        !checkedRecord(&record, sizeof(record)) || record.entry.used > 1 ||
        record.entry.scope > BotIoRequest::Channel ||
        !memchr(record.entry.key, 0, sizeof(record.entry.key)) ||
        !memchr(record.entry.value, 0, sizeof(record.entry.value)) ||
        (record.entry.used && (!record.entry.key[0] ||
         (record.entry.scope != BotIoRequest::Bot && journal.slots[i] >= PrivateSlots))))
      return storageError(error, capacity, "KV recovery record invalid; storage blocked");
    for (unsigned j = 0; j < i; ++j)
      if (journal.slots[i] == journal.slots[j])
        return storageError(error, capacity, "KV recovery duplicate slot; storage blocked");
  }
  for (unsigned i = 0; i < journal.count; ++i)
    files_->records[journal.slots[i]] = journal.records[i];
  if (!validateRecords(error, capacity)) return false;
  if (uint32_t(millis() - started) >= BotKvRecoveryBudgetMs)
    return storageError(error, capacity, "KV legacy migration read deadline; retry recovery");
  // BKI owns no published file contents; resume interrupted empty-bank initialization here.
  if (!legacy && !bootstrap) return true;
  if (!admitJournal(false, error, capacity)) return false;
  files_->authority.reclaim = legacy;
  BotIoResult::Outcome outcome = BotIoResult::Rejected;
  if (!publish((1 << Shards) - 1, outcome, error, capacity, started, BotKvRecoveryBudgetMs))
    return false;
  if (legacy && !reclaim(started, error, capacity)) return false;
  // Recovery published and read back every group, then verified final authority.
  files_->verified = true;
  return true;
}
bool BotStore::transact(const uint8_t bot[32], const BotIoRequest &request, BotIoResult &result,
                        const std::atomic<uint32_t> &generation, const std::atomic<bool> &sharedState,
                        const std::atomic<uint32_t> &sharedGrant, const std::atomic<uint32_t> *eventEpoch,
                        uint32_t started) {
  const auto fail = [&](const char *message) { return storageError(result.error, sizeof(result.error), message); };
  if (!request.mutations || request.mutations > BotTransactionLimit ||
      (request.kind == BotIoRequest::Cas && (request.mutations != 1 || !request.mutation[0].compare)))
    return fail(BotTransactionLimit == 2 ? "KV transaction requires 1..2 distinct operations" :
                                         "KV transaction requires 1..4 distinct operations");
  for (unsigned i = 0; i < request.mutations; ++i) {
    const auto &op = request.mutation[i];
    if (!op.key[0] || !memchr(op.key, 0, sizeof(op.key)) ||
        !memchr(op.value, 0, sizeof(op.value)) || !memchr(op.expected, 0, sizeof(op.expected)))
      return fail("KV transaction text exceeds bounds");
    for (unsigned j = 0; j < i; ++j)
      if (!strcmp(op.key, request.mutation[j].key)) return fail("KV transaction duplicate key");
  }
  auto &journal = *journal_;
  journal = {}; memcpy(journal.magic, "BTX\1", 4);
  int matched[BotTransactionLimit]; unsigned free[Slots], available = 0, owned = 0;
  char ownedKeys[BotKeysPerScope][BotKeyLimit + 1]{};
  for (auto &slot : matched) slot = -1;
  const auto current = [&] {
    return botEventCurrent(request, eventEpoch) && request.token.generation == generation.load() &&
        (!request.sharedScope() || (sharedState.load() && request.grant == sharedGrant.load()));
  };
  for (unsigned slot = 0; slot < Slots; ++slot) {
    const auto &record = files_->records[slot];
    if (!current() || uint32_t(millis() - started) >= 2000) {
      return fail("KV transaction cancelled/deadline before write");
    }
    const auto &entry = record.entry;
    if (!entry.used) {
      if ((slot >= PrivateSlots) == (request.scope == BotIoRequest::Bot)) free[available++] = slot;
    } else if (!memcmp(record.bot, bot, 32) && entry.scope == request.scope &&
               !memcmp(entry.principal, request.principal, 32)) {
      if (owned == BotKeysPerScope) return fail("KV transaction scope exceeds bounded capacity");
      for (unsigned i = 0; i < owned; ++i)
        if (!strcmp(ownedKeys[i], entry.key)) return fail("KV transaction duplicate durable key");
      strcpy(ownedKeys[owned++], entry.key);
      for (unsigned i = 0; i < request.mutations; ++i) if (!strcmp(entry.key, request.mutation[i].key)) {
        if (matched[i] >= 0) return fail("KV transaction duplicate durable key");
        matched[i] = slot; journal.records[i] = record;
      }
    }
  }
  int nextOwned = int(owned);
  for (unsigned i = 0; i < request.mutations; ++i) {
    const auto &op = request.mutation[i];
    const bool found = matched[i] >= 0;
    if (op.compare && (op.present != found || (found && strcmp(op.expected, journal.records[i].entry.value)))) {
      result.ok = true; result.outcome = BotIoResult::Conflict; return true;
    }
    if (found && op.remove) { --nextOwned; free[available++] = unsigned(matched[i]); }
    if (!found && !op.remove) ++nextOwned;
  }
  if (nextOwned > int(BotKeysPerScope)) return fail("KV transaction scope capacity exhausted");
  // Assign additions before emitting records; deleted slots can be reused in
  // this same atomic set, without requiring spare public capacity.
  for (unsigned i = 0; i < request.mutations; ++i) if (matched[i] < 0 && !request.mutation[i].remove) {
    if (!available) return fail("KV transaction capacity exhausted");
    matched[i] = int(free[--available]);
  }
  journal.count = 0;
  for (unsigned i = 0; i < request.mutations; ++i) {
    const auto &op = request.mutation[i];
    if (matched[i] < 0) continue;
    bool reused = false;
    if (op.remove) for (unsigned j = 0; j < request.mutations; ++j)
      reused = reused || (i != j && matched[j] == matched[i] && !request.mutation[j].remove);
    if (reused) continue;
    const auto at = journal.count++;
    journal.slots[at] = uint8_t(matched[i]);
    auto &record = journal.records[at]; record = {};
    memcpy(record.magic, "BKV\1", 4); memcpy(record.bot, bot, 32);
    if (!op.remove) {
      record.entry.used = 1; record.entry.scope = request.scope;
      memcpy(record.entry.principal, request.principal, 32);
      strcpy(record.entry.key, op.key); strcpy(record.entry.value, op.value);
    }
    mesh::Utils::sha256(record.digest, 32, reinterpret_cast<const uint8_t *>(&record), offsetof(Record, digest));
  }
  if (!journal.count) {
    if (!current() || uint32_t(millis() - started) >= 2000) return fail("KV transaction cancelled before no-op commit");
    result.ok = true; result.outcome = BotIoResult::Committed; return true;
  }
  if (!admitJournal(request.scope != BotIoRequest::Bot, result.error, sizeof(result.error))) {
    return false;
  }
  if (!current()) return fail("KV transaction cancelled before publication");
  uint8_t changed = 0;
  files_->verified = false;
  for (unsigned i = 0; i < journal.count; ++i) {
    files_->records[journal.slots[i]] = journal.records[i];
    changed |= 1 << (journal.slots[i] / ShardSlots);
  }
  if (!publish(changed, result.outcome, result.error, sizeof(result.error),
                started, 2000, request.token.generation, &generation, &request,
               &sharedState, &sharedGrant, eventEpoch))
    return false;
  if (!current()) return fail("KV transaction committed after cancellation; do not retry blindly");
  result.outcome = BotIoResult::Committed; result.ok = true; return true;
}
bool BotStore::validSnapshotEnvelope(const Snapshot &data, const uint8_t bot[32], const char magic[4],
                                     size_t itemSize, unsigned limit, char *error, size_t capacity) {
  static_assert(sizeof(Snapshot) == 2422, "KV backup wire format changed");
  const auto fail = [&](const char *message) { return storageError(error, capacity, message); };
  if (memcmp(data.magic, magic, 4) || memcmp(data.bot, bot, 32) ||
      data.scope > BotIoRequest::Channel || data.count > limit ||
      data.count * itemSize > sizeof(data.entries) ||
      !checkedRecord(&data, sizeof(data))) return fail("Bot-data version, identity, bounds or SHA256 invalid");
  uint8_t bits = 0;
  for (auto byte : data.principal) bits |= byte;
  if ((data.scope == BotIoRequest::Bot) != !bits) return fail("Bot-data scope/principal invalid");
  const auto *bytes = reinterpret_cast<const uint8_t *>(data.entries);
  for (size_t i = data.count * itemSize; i < sizeof(data.entries); ++i)
    if (bytes[i]) return fail("Bot-data noncanonical unused records");
  return true;
}
bool BotStore::validSnapshot(const Snapshot &data, const uint8_t bot[32], char *error, size_t capacity) {
  const auto fail = [&](const char *message) { return storageError(error, capacity, message); };
  if (!validSnapshotEnvelope(data, bot, "BKD\1", sizeof(Snapshot::Entry), BotKeysPerScope, error, capacity))
    return false;
  for (unsigned i = 0; i < BotKeysPerScope; ++i) {
    const auto &entry = data.entries[i];
    if (!memchr(entry.key, 0, sizeof(entry.key)) || !memchr(entry.value, 0, sizeof(entry.value)) ||
        (i < data.count && (!entry.key[0] || (i && strcmp(data.entries[i - 1].key, entry.key) >= 0))))
      return fail("Bot-data keys/values invalid or not strictly sorted");
    const size_t keySize = i < data.count ? strlen(entry.key) : 0;
    const size_t valueSize = i < data.count ? strlen(entry.value) : 0;
    if (valueSize > BotValueLimit) return fail("Bot-data value exceeds this platform's KV byte limit");
    for (size_t n = keySize; n < sizeof(entry.key); ++n)
      if (entry.key[n]) return fail("Bot-data noncanonical key padding");
    for (size_t n = valueSize; n < sizeof(entry.value); ++n)
      if (entry.value[n]) return fail("Bot-data noncanonical value padding");
  }
  return true;
}
bool BotStore::snapshot(const uint8_t bot[32], Snapshot &data, char *error, size_t capacity) {
  if (!record_) record_ = allocateRoleStorage<Record>("bot durable KV");
  if (!record_) return storageError(error, capacity, "Bot-data export storage RAM unavailable");
  if (!recover(error, capacity)) return false;
  const uint32_t started = millis();
  const auto scope = data.scope;
  uint8_t principal[32]; memcpy(principal, data.principal, 32);
  data = {}; data.scope = scope; memcpy(data.principal, principal, 32);
  memcpy(data.bot, bot, 32); memcpy(data.magic, "BKD\1", 4);
  const auto fail = [&](const char *message) { return storageError(error, capacity, message); };
  for (unsigned slot = 0; slot < Slots; ++slot) {
    const auto &record = files_->records[slot];
    const auto &entry = record.entry;
    if (uint32_t(millis() - started) >= 2000) return fail("Bot-data export deadline");
    if (!entry.used || memcmp(record.bot, bot, 32) || entry.scope != scope ||
        memcmp(entry.principal, principal, 32)) continue;
    if (data.count == BotKeysPerScope) return fail("Bot-data scope capacity exceeded");
    unsigned at = 0;
    while (at < data.count && strcmp(data.entries[at].key, entry.key) < 0) ++at;
    if (at < data.count && !strcmp(data.entries[at].key, entry.key)) {
      return fail("Bot-data contains duplicate keys");
    }
    for (unsigned i = data.count; i > at; --i) data.entries[i] = data.entries[i - 1];
    data.entries[at] = {};
    strcpy(data.entries[at].key, entry.key); strcpy(data.entries[at].value, entry.value); ++data.count;
  }
  mesh::Utils::sha256(data.digest, 32, reinterpret_cast<const uint8_t *>(&data), offsetof(Snapshot, digest));
  return validSnapshot(data, bot, error, capacity);
}
void BotStore::restore(const uint8_t bot[32], const Snapshot &data, BotIoResult &result,
                       uint32_t epoch, const std::atomic<uint32_t> &generation) {
  resetBotIoResult(result);
  const auto fail = [&](const char *message) { storageError(result.error, sizeof(result.error), message); };
  if (!validSnapshot(data, bot, result.error, sizeof(result.error))) return;
  if (!record_) record_ = allocateRoleStorage<Record>("bot durable KV");
  if (!record_) { fail("Bot-data restore storage RAM unavailable"); return; }
  if (files_) files_->verified = false;
  if (!recover(result.error, sizeof(result.error))) return;
  if (epoch != generation.load()) { fail("Bot-data restore cancelled before admission"); return; }
  const uint32_t started = millis();
  auto &journal = *journal_;
  journal = {}; memcpy(journal.magic, "BTX\1", 4);
  uint8_t free[Slots]{}; unsigned available = 0;
  for (unsigned slot = 0; slot < Slots; ++slot) {
    const auto &record = files_->records[slot];
    const auto &entry = record.entry;
    if (!entry.used) {
      if ((slot >= PrivateSlots) == (data.scope == BotIoRequest::Bot)) free[available++] = uint8_t(slot);
    } else if (!memcmp(record.bot, bot, 32) && entry.scope == data.scope &&
               !memcmp(entry.principal, data.principal, 32)) {
      if (journal.count == BotKeysPerScope) { fail("Bot-data scope capacity exceeded"); return; }
      for (unsigned i = 0; i < journal.count; ++i)
        if (!strcmp(journal.records[i].entry.key, entry.key)) {
          fail("Bot-data restore blocked by duplicate durable keys"); return;
        }
      journal.records[journal.count] = record;
      journal.slots[journal.count++] = uint8_t(slot);
    }
  }
  while (journal.count < data.count) {
    if (!available) { fail("Bot-data restore capacity exhausted"); return; }
    journal.slots[journal.count++] = free[--available];
  }
  for (unsigned i = 0; i < journal.count; ++i) {
    auto &record = journal.records[i];
    record = {};
    memcpy(record.magic, "BKV\1", 4); memcpy(record.bot, bot, 32);
    if (i < data.count) {
      record.entry.used = 1; record.entry.scope = data.scope;
      memcpy(record.entry.principal, data.principal, 32);
      strcpy(record.entry.key, data.entries[i].key); strcpy(record.entry.value, data.entries[i].value);
    }
    mesh::Utils::sha256(record.digest, 32, reinterpret_cast<const uint8_t *>(&record), offsetof(Record, digest));
  }
  if (!journal.count) {
    if (epoch != generation.load() || uint32_t(millis() - started) >= 2000) {
      fail("Bot-data restore cancelled before no-op commit"); return;
    }
    result.ok = true; result.outcome = BotIoResult::Committed; return;
  }
  if (!admitJournal(data.scope != BotIoRequest::Bot, result.error, sizeof(result.error))) {
    return;
  }
  if (epoch != generation.load() || uint32_t(millis() - started) >= 2000) {
    fail("Bot-data restore cancelled/deadline before publication"); return;
  }
  uint8_t changed = 0;
  files_->verified = false;
  for (unsigned i = 0; i < journal.count; ++i) {
    files_->records[journal.slots[i]] = journal.records[i];
    changed |= 1 << (journal.slots[i] / ShardSlots);
  }
  if (!publish(changed, result.outcome, result.error, sizeof(result.error),
                started, 2000, epoch, &generation)) return;
  if (epoch != generation.load()) { fail("Bot-data committed after cancellation; do not retry blindly"); return; }
  result.ok = true; result.outcome = BotIoResult::Committed;
}
void BotStore::perform(const uint8_t bot[32], const BotIoRequest &request, BotIoResult &result,
                       const std::atomic<uint32_t> &generation, const std::atomic<bool> &sharedState,
                       const std::atomic<uint32_t> &sharedGrant, const std::atomic<uint32_t> *eventEpoch) {
  static_assert(sizeof(Record) == 136 + BotValueLimit, "Durable KV record format changed");
  uint32_t started = millis();
  resetBotIoResult(result); result.token = request.token;
  const auto fail = [&](const char *message) {
    snprintf(result.error, sizeof(result.error), "%s", message);
    Serial.printf("On-chip bot KV: %s\n", message);
  };
  if (!bot || request.token.generation != generation.load()) {
    fail("KV cancelled before admission"); return;
  }
  const auto allowed = [&]() {
    return botEventCurrent(request, eventEpoch) &&
        (!request.sharedScope() || (sharedState.load() && request.grant == sharedGrant.load()));
  };
  if (!allowed()) { fail("Shared KV grant revoked/unavailable"); return; }
  if ((request.kind != BotIoRequest::Get && request.kind != BotIoRequest::Put &&
       request.kind != BotIoRequest::Delete && request.kind != BotIoRequest::List &&
       request.kind != BotIoRequest::Cas && request.kind != BotIoRequest::Transaction) ||
      request.scope > BotIoRequest::Channel ||
      (request.kind != BotIoRequest::List && request.kind != BotIoRequest::Cas &&
       request.kind != BotIoRequest::Transaction && !request.key[0]) ||
      !memchr(request.key, 0, sizeof(request.key)) ||
      !memchr(request.value, 0, sizeof(request.value))) {
    fail("Invalid bounded KV operation"); return;
  }
  if (strlen(request.value) > BotValueLimit) {
    fail("KV value exceeds this platform's byte limit"); return;
  }
  if (!record_) record_ = allocateRoleStorage<Record>("bot durable KV");
  if (!record_) { fail("KV storage RAM unavailable"); return; }
  if (files_ && request.kind != BotIoRequest::Get && request.kind != BotIoRequest::List)
    files_->verified = false;
  if (!recover(result.error, sizeof(result.error))) return;
  // Empty-media checks have their own recovery budget before the first operation.
  if (!files_->initialized) started = millis();
  if (request.kind == BotIoRequest::Cas || request.kind == BotIoRequest::Transaction) {
    transact(bot, request, result, generation, sharedState, sharedGrant, eventEpoch, started); return;
  }
  auto &record = *record_;
  const uint8_t magic[] = {'B', 'K', 'V', 1};
  int matched = -1, free = -1;
  unsigned owned = 0;
  const bool listing = request.kind == BotIoRequest::List;
  for (unsigned i = 0; i < Slots; ++i) {
    if (!allowed() || request.token.generation != generation.load() || uint32_t(millis() - started) >= 2000) {
      fail("KV read cancelled/deadline"); return;
    }
    const bool reserved = i >= PrivateSlots;
    const bool allocatable = reserved == (request.scope == BotIoRequest::Bot);
    record = files_->records[i];
    const auto &entry = record.entry;
    if (!entry.used) { if (allocatable && free < 0) free = i; continue; }
    if (!memcmp(record.bot, bot, 32) && entry.scope == request.scope &&
        !memcmp(entry.principal, request.principal, 32)) {
      ++owned;
      if (listing) {
        if (owned > BotKeysPerScope) {
          fail("KV scope exceeds bounded key capacity"); return;
        }
        auto &keys = result.keys;
        unsigned at = 0;
        while (at < keys.count && strcmp(keys.keys[at], entry.key) < 0) ++at;
        if (at < keys.count && !strcmp(keys.keys[at], entry.key)) {
          fail("KV contains duplicate durable keys"); return;
        }
        for (unsigned i = keys.count; i > at; --i) strcpy(keys.keys[i], keys.keys[i - 1]);
        strcpy(keys.keys[at], entry.key); ++keys.count;
        continue;
      }
      if (!strcmp(entry.key, request.key)) {
        if (matched >= 0) { fail("KV contains duplicate durable keys"); return; }
        matched = i;
        result.found = true; strcpy(result.value, entry.value);
      }
    }
  }
  if (!allowed() || request.token.generation != generation.load() || uint32_t(millis() - started) >= 2000) {
    fail("KV read cancelled/deadline"); return;
  }
  if (listing) {
    auto &keys = result.keys;
    unsigned count = 0;
    for (unsigned i = 0; i < keys.count; ++i)
      if (!strncmp(keys.keys[i], request.key, strlen(request.key))) {
        if (count != i) strcpy(keys.keys[count], keys.keys[i]);
        ++count;
      }
    for (unsigned i = count; i < keys.count; ++i) memset(keys.keys[i], 0, sizeof(keys.keys[i]));
    keys.count = count;
    result.found = count != 0;
  }
  if (request.kind == BotIoRequest::Get || listing) {
    result.ok = true; return;
  }
  record = {}; memcpy(record.magic, magic, sizeof(magic)); memcpy(record.bot, bot, 32);
  auto &entry = record.entry;
  if (request.kind == BotIoRequest::Put) {
    if (matched < 0 && (free < 0 || owned >= BotKeysPerScope)) {
      snprintf(result.error, sizeof(result.error), "KV capacity exhausted (%u keys/scope, %u public + %u bot-reserved)",
               BotKeysPerScope, PrivateSlots, BotKeysPerScope);
      result.ok = false; return;
    }
    if (matched < 0) matched = free;
    entry.used = 1; entry.scope = request.scope;
    memcpy(entry.principal, request.principal, 32);
    strcpy(entry.key, request.key); strcpy(entry.value, request.value);
  } else if (request.kind == BotIoRequest::Delete) {
    if (matched < 0) { result.ok = true; return; }
  } else { fail("Unknown KV operation"); return; }
  if (!admitJournal(request.kind != BotIoRequest::Delete && request.scope != BotIoRequest::Bot,
                    result.error, sizeof(result.error))) return;
  seal(&record, sizeof(record));
  files_->verified = false;
  files_->records[matched] = record;
  result.ok = publish(1 << (matched / ShardSlots), result.outcome, result.error, sizeof(result.error),
                       started, 2000, request.token.generation,
                      &generation, &request, &sharedState, &sharedGrant, eventEpoch);
  if (!result.ok) Serial.printf("On-chip bot KV: %s\n", result.error);
}
}
#endif
