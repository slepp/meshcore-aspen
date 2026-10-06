// SPDX-License-Identifier: Apache-2.0
#include "BotStore.h"
#include "BotJournal.h"
#include "bot_kv_fixture.h"
#include <esp_heap_caps.h>
#include <cassert>
#include <limits>

using namespace onchip;
using namespace kv_fixture;
static unsigned long now;
unsigned long millis() { return now; }
void delay(unsigned long ms) { now += ms; }
static void faultsOff() {
  identity_test::step = nullptr;
  filesystem_test::step = nullptr;
  identity_test::failRead = identity_test::failWrite = identity_test::failCommit = false;
  identity_test::failErase = identity_test::failStats = false;
  identity_test::afterWrite = identity_test::afterCommit = identity_test::afterErase = nullptr;
  identity_test::readHook = nullptr;
  filesystem_test::afterWrite = filesystem_test::afterFlush = nullptr;
  filesystem_test::failOpen = filesystem_test::appendOnRead = false;
  filesystem_test::readLimit = filesystem_test::writeLimit = std::numeric_limits<size_t>::max();
  filesystem_test::readDelayMs = filesystem_test::existsDelayMs = 0;
  now = 0;
}
static void reset() {
  faultsOff();
  assert(identity_test::handles.empty());
  identity_test::durable.clear(); identity_test::namespaces.clear();
  identity_test::entryCapacity = identity_test::peakEntries = 0;
  identity_test::freeEntries = 4096; identity_test::eagerWrites = false;
  filesystem_test::files.clear();
  filesystem_test::readCalls = filesystem_test::bytesRead = filesystem_test::largestRead = 0;
}
struct Client {
  BotStore store;
  uint8_t bot[32]{1};
  std::atomic<uint32_t> generation{1}, grant{1}, event{1};
  std::atomic<bool> shared{true};
  BotIoRequest request;
  BotIoResult result;
  Client() { request.token = {1, 1, 1}; request.grant = 1; request.principal[0] = 7; }
  void run() { store.perform(bot, request, result, generation, shared, grant, &event); }
  void get(const char *key) { request.kind = BotIoRequest::Get; strcpy(request.key, key); run(); }
  void put(const char *key, const char *value) {
    request.kind = BotIoRequest::Put; strcpy(request.key, key); strcpy(request.value, value); run();
  }
  void transaction() {
    request.kind = BotIoRequest::Transaction; request.mutations = 4;
    for (unsigned i = 0; i < 4; ++i) {
      auto &op = request.mutation[i];
      snprintf(op.key, sizeof(op.key), "k%u", i);
      strcpy(op.value, "new"); strcpy(op.expected, "old");
      op.compare = op.present = true;
    }
    run();
  }
};
static void recover(BotStore &store) {
  char error[128]{};
  assert(store.recover(error, sizeof(error)));
}
struct Cut {};
static unsigned at, target;
static void checkpoint(const char *) {
  if (++at == target) throw Cut{};
}
template<class Operation, class Verify>
static unsigned cuts(Operation operation, Verify verify) {
  const auto durable = identity_test::durable;
  const auto files = filesystem_test::files;
  unsigned count = 0;
  for (unsigned cut = 0;; ++cut) {
    faultsOff();
    identity_test::durable = durable; filesystem_test::files = files;
    at = 0; target = cut;
    identity_test::step = filesystem_test::step = checkpoint;
    bool interrupted = false;
    try { operation(); } catch (const Cut &) { interrupted = true; }
    if (!cut) count = at;
    else assert(interrupted);
    faultsOff();
    // Abrupt reset discards open handles and staged host writes, not durable media.
    identity_test::handles.clear();
    verify();
    assert(identity_test::handles.empty());
    if (cut == count) return count;
  }
}
static void atomicCuts() {
  for (bool eager : {false, true}) {
    reset(); identity_test::eagerWrites = eager;
    for (unsigned i = 0; i < 4; ++i) {
      char key[8]; snprintf(key, sizeof(key), "k%u", i);
      seed(i * 8, record(1, BotIoRequest::Caller, 7, key, "old"));
    }
    seed(31, record(2, BotIoRequest::Channel, 9, "other", "retained"));
    { BotStore store; recover(store); }
    const auto other = read(31);
    unsigned count = cuts([] {
      Client c; c.transaction(); assert(c.result.ok);
    }, [&] {
      Client c;
      bool newValues = false;
      for (unsigned i = 0; i < 4; ++i) {
        char key[8]; snprintf(key, sizeof(key), "k%u", i);
        c.get(key); assert(c.result.ok && c.result.found);
        if (!i) newValues = !strcmp(c.result.value, "new");
        assert(!strcmp(c.result.value, newValues ? "new" : "old"));
      }
      const auto retained = read(31);
      assert(!memcmp(&other, &retained, sizeof(other)));
    });
    printf("PASS four-shard atomic transaction: %u file/NVS interruption boundaries, eager=%u\n", count, eager);

    reset(); identity_test::eagerWrites = eager;
    count = cuts([] {
      Client c; c.put("first", "value"); assert(c.result.ok);
    }, [] {
      Client c; c.get("first"); assert(c.result.ok);
      if (c.result.found) assert(!strcmp(c.result.value, "value"));
    });
    printf("PASS initial five-shard publication: %u interruption boundaries, eager=%u\n", count, eager);
  }
}
static void legacyMigration() {
  for (bool eager : {false, true}) for (bool withRedo : {false, true}) {
    reset(); identity_test::eagerWrites = eager;
    Record expected[40];
    for (unsigned i = 0; i < 40; ++i) {
      char key[16]; snprintf(key, sizeof(key), "legacy%u", i);
      expected[i] = record(1 + i % 2, i >= 32 ? unsigned(BotIoRequest::Bot) :
                           i % 3 == 2 ? unsigned(BotIoRequest::Channel) : i % 3,
                           i >= 32 ? 0 : 7 + i / 8, key, "old");
      seed(i, expected[i]);
    }
    // Bot-global entries from before reserved slots remain at their old slots.
    expected[31] = record(3, BotIoRequest::Bot, 0, "early-bot", "retained");
    seed(31, expected[31]);
    Redo redo; redo.count = 8;
    for (unsigned i = 0; i < 8; ++i) {
      redo.slots[i] = i * 5;
      redo.records[i] = expected[i * 5];
      strcpy(redo.records[i].entry.value, "redo"); seal(&redo.records[i], sizeof(Record));
      if (withRedo) expected[i * 5] = redo.records[i];
    }
    if (withRedo) {
      seedRedo(redo);
      seed(0, expected[0]); seed(5, expected[5]); // A prior boot partially replayed the redo.
    }
    identity_test::durable[{"other-role", "keep"}] = {1, 2, 3};
    identity_test::durable[{"mc-bot-kv", "unrelated"}] = {4, 5, 6};
    const auto count = cuts([] { BotStore store; recover(store); }, [&] {
      BotStore store; recover(store);
      assert(authority().size() == 200 && authority()[5] == 0);
      for (unsigned i = 0; i < 40; ++i) {
        const auto actual = read(i);
        assert(!memcmp(&actual, &expected[i], sizeof(actual)));
        char key[16]; snprintf(key, sizeof(key), "%c%02u", i < 32 ? 'v' : 'r', i < 32 ? i : i - 32);
        assert(!identity_test::durable.count({"mc-bot-kv", key}));
      }
      assert((identity_test::durable.at({"other-role", "keep"}) == std::vector<uint8_t>{1, 2, 3}));
      assert((identity_test::durable.at({"mc-bot-kv", "unrelated"}) == std::vector<uint8_t>{4, 5, 6}));
    });
    printf("PASS all legacy slots/scopes/identities: %u migration/reclaim cuts, eager=%u redo=%u\n", count, eager, withRedo);
  }
  reset();
  // Intermediate redo replay may duplicate a key until its later slot changes.
  seed(0, record(1, BotIoRequest::Caller, 7, "a", "old"));
  seed(1, record(1, BotIoRequest::Caller, 7, "a", "new"));
  Redo redo; redo.count = 2; redo.slots[0] = 0; redo.slots[1] = 1;
  redo.records[0] = record(1, BotIoRequest::Caller, 7, "b", "new");
  redo.records[1] = record(1, BotIoRequest::Caller, 7, "a", "new");
  seedRedo(redo);
  Client c; c.get("a"); assert(c.result.ok && !strcmp(c.result.value, "new"));
  c.get("b"); assert(c.result.ok && !strcmp(c.result.value, "new"));
}
static void restoreCuts() {
  for (bool eager : {false, true}) {
    reset(); identity_test::eagerWrites = eager;
    const unsigned slots[] = {0, 8, 16, 24, 32, 33, 34, 35};
    for (unsigned i = 0; i < 8; ++i) {
      char key[8]; snprintf(key, sizeof(key), "k%u", i);
      seed(slots[i], record(1, BotIoRequest::Bot, 0, key, "old"));
    }
    seed(31, record(2, BotIoRequest::Caller, 7, "retained", "other"));
    BotStore::Snapshot backup; backup.scope = BotIoRequest::Bot;
    uint8_t bot[32]{1}; char error[128];
    {
      BotStore store; assert(store.snapshot(bot, backup, error, sizeof(error)));
    }
    assert(backup.count == 8);
    for (auto &entry : backup.entries) strcpy(entry.value, "new");
    seal(&backup, sizeof(backup));
    const auto count = cuts([&] {
      BotStore store; BotIoResult result; std::atomic<uint32_t> generation{1};
      store.restore(bot, backup, result, 1, generation);
      assert(result.ok && result.outcome == BotIoResult::Committed);
    }, [&] {
      BotStore store; BotStore::Snapshot actual; actual.scope = BotIoRequest::Bot;
      assert(store.snapshot(bot, actual, error, sizeof(error)) && actual.count == 8);
      const bool changed = !strcmp(actual.entries[0].value, "new");
      for (const auto &entry : actual.entries) assert(!strcmp(entry.value, changed ? "new" : "old"));
      assert(!strcmp(read(31).entry.value, "other"));
    });
    printf("PASS eight-key owner restore across five groups: %u interruption boundaries, eager=%u\n", count, eager);
  }
}
static void restoreCancellation() {
  static std::atomic<uint32_t> *generation;
  for (bool eager : {false, true}) for (bool published : {false, true}) {
    reset(); identity_test::eagerWrites = eager;
    Client c; generation = &c.generation;
    c.put("a", "old"); assert(c.result.ok);
    c.put("b", "old"); assert(c.result.ok);
    BotStore::Snapshot backup; backup.principal[0] = 7; char error[128];
    assert(c.store.snapshot(c.bot, backup, error, sizeof(error)) && backup.count == 2);
    strcpy(backup.entries[0].value, "new"); strcpy(backup.entries[1].value, "new");
    seal(&backup, sizeof(backup));
    const auto cancel = [] { ++*generation; };
    if (published) identity_test::afterWrite = cancel;
    else filesystem_test::afterFlush = cancel;
    c.store.restore(c.bot, backup, c.result, 1, c.generation);
    assert(!c.result.ok && c.result.outcome == (published ? BotIoResult::Unknown : BotIoResult::Rejected));
    faultsOff();
    BotStore::Snapshot actual; actual.principal[0] = 7;
    assert(c.store.snapshot(c.bot, actual, error, sizeof(error)) && actual.count == 2);
    for (unsigned i = 0; i < actual.count; ++i)
      assert(!strcmp(actual.entries[i].value, eager && published ? "new" : "old"));
  }
  puts("PASS owner restore generation fence before file authority and during uncertain publication");
}
static void physicalMigration() {
  for (bool admitted : {false, true}) {
    reset();
    identity_test::entryCapacity = 630;
    identity_test::namespaces = {"mc-bot-kv", "mc-onchip"};
    for (unsigned i = 0; i < 8; ++i) {
      char key[8]; snprintf(key, sizeof(key), "k%u", i);
      seed(i, record(1, BotIoRequest::Caller, 7, key, "old"));
    }
    Redo redo; redo.count = 8;
    for (unsigned i = 0; i < 8; ++i) {
      char key[8]; snprintf(key, sizeof(key), "k%u", i);
      redo.slots[i] = i; redo.records[i] = record(1, BotIoRequest::Caller, 7, key, "new");
    }
    seedRedo(redo);
    const unsigned available = BotKvRecoveryEntries - !admitted;
    const size_t fill = 630 - identity_test::usedEntries() - available;
    identity_test::durable[{"mc-onchip", "other-data"}] = std::vector<uint8_t>((fill - 3) * 32);
    assert(630 - identity_test::usedEntries() == available);
    const auto before = identity_test::durable;
    Client c; c.get("k0"); assert(c.result.ok == admitted);
    if (admitted) {
      assert(!strcmp(c.result.value, "new") && authority().size() == 200);
      assert(630 - identity_test::usedEntries() == available + 103 + 8 * 15 - 9);
      assert(identity_test::peakEntries == 630 - available + 9 && identity_test::peakEntries <= 504);
    } else {
      assert(strstr(c.result.error, "headroom") && before == identity_test::durable &&
             filesystem_test::files.empty());
    }
  }
  puts("PASS 630-entry legacy redo migration: exact 154/155 admission boundary, no advance reclamation credit, 126-entry GC reserve retained");
}
static void initializationBudget() {
  for (bool present : {false, true}) for (bool admitted : {false, true}) {
    reset(); identity_test::entryCapacity = 630;
    identity_test::namespaces = {"other-role"};
    if (present) identity_test::namespaces.insert("mc-bot-kv");
    const size_t required = BotKvRecoveryEntries + BotKvBootstrapEntries + !present;
    const size_t free = required - !admitted;
    const size_t fill = 630 - identity_test::usedEntries() - free;
    identity_test::durable[{"other-role", "data"}] = std::vector<uint8_t>((fill - 3) * 32);
    const auto before = identity_test::durable;
    Client c; c.request.scope = BotIoRequest::Bot; c.request.principal[0] = 0;
    c.put("initial", "value"); assert(c.result.ok == admitted);
    if (admitted) {
      assert(authority().size() == 200);
      assert(identity_test::peakEntries == 630 - free + BotKvAuthorityEntries + BotKvBootstrapEntries + !present);
      assert(identity_test::peakEntries == 630 - 126 - 20);
    } else {
      assert(before == identity_test::durable && filesystem_test::files.empty());
    }
  }
  puts("PASS first-write 159/160 NVS budgets include verified initialization marker and namespace creation");
}
static void corruptMigration() {
  for (unsigned fault = 0; fault < 12; ++fault) {
    reset();
    seed(0, record(1, BotIoRequest::Caller, 7, "first", "old"));
    seed(39, record(2, BotIoRequest::Bot, 0, "last", "old"));
    auto &last = identity_test::durable.at({"mc-bot-kv", "r07"});
    if (fault == 0) last.back() ^= 1;
    if (fault == 1) last.pop_back();
    if (fault == 2) { last[37] = BotIoRequest::Caller; seal(last.data(), last.size()); }
    if (fault == 3) seed(1, record(1, BotIoRequest::Caller, 7, "first", "duplicate"));
    if (fault == 4) {
      for (unsigned i = 0; i < 9; ++i) {
        char key[8]; snprintf(key, sizeof(key), "k%u", i);
        seed(i, record(1, BotIoRequest::Caller, 7, key, "nine"));
      }
    }
    if (fault >= 5) {
      Redo redo; redo.count = 1; redo.slots[0] = 39;
      redo.records[0] = record(2, BotIoRequest::Bot, 0, "last", "redo");
      if (fault == 5) last.back() ^= 1; // Redo must not conceal a corrupt live member.
      if (fault == 6) redo.slots[0] = 40;
      if (fault == 7) { redo.count = 2; redo.slots[1] = 39; redo.records[1] = redo.records[0]; }
      if (fault == 8) redo.records[0].digest[0] ^= 1;
      seedRedo(redo);
      if (fault == 9) identity_test::durable.at({"mc-bot-kv", "txn"}).pop_back();
      if (fault >= 10) {
        std::vector<uint8_t> marker(48); memcpy(marker.data(), "BKI\1", 4);
        marker[4] = fault == 10;
        seal(marker.data(), marker.size());
        if (fault == 11) marker.back() ^= 1;
        identity_test::durable[{"mc-bot-kv", "txn"}] = marker;
      }
    }
    const auto before = identity_test::durable;
    Client c; c.get("first"); assert(!c.result.ok);
    c.put("first", "new"); assert(!c.result.ok);
    assert(identity_test::durable == before && filesystem_test::files.empty());
  }
  reset();
  std::vector<uint8_t> clear(48); memcpy(clear.data(), "BTX\1", 4); seal(clear.data(), clear.size());
  identity_test::durable[{"mc-bot-kv", "txn"}] = clear;
  Client c; c.get("absent"); assert(c.result.ok && !c.result.found && authority().size() == 200);
  puts("PASS legacy validation: all slots before publication, redo cannot conceal corruption, clear marker migrates");
}
static void ioFailures() {
  for (unsigned fault = 0; fault < 11; ++fault) {
    reset(); Client c; c.put("a", "old"); assert(c.result.ok);
    const auto before = identity_test::durable;
    if (fault <= 2) filesystem_test::writeLimit = fault == 0 ? 0 : fault == 1 ? 100 : 3135;
    if (fault == 3) filesystem_test::afterWrite = [] { filesystem_test::readLimit = 100; };
    if (fault == 4) filesystem_test::afterWrite = [] { filesystem_test::appendOnRead = true; };
    if (fault == 5) filesystem_test::afterFlush = [] {
      for (auto &entry : filesystem_test::files)
        if (entry.first == "/command-bot/kv-0-b.bin") entry.second.back() ^= 1;
    };
    if (fault == 6) filesystem_test::failOpen = true;
    if (fault == 7) filesystem_test::afterFlush = [] { now += 2000; };
    if (fault == 8) identity_test::failStats = true;
    if (fault == 9) identity_test::failWrite = true;
    if (fault == 10) identity_test::failCommit = true;
    c.put("a", "new"); assert(!c.result.ok);
    assert(identity_test::durable == before);
    faultsOff();
    c.get("a"); assert(c.result.ok && !strcmp(c.result.value, "old"));
    c.put("a", "retry"); assert(c.result.ok);
    c.get("a"); assert(c.result.ok && !strcmp(c.result.value, "retry"));
  }
  for (unsigned fault = 0; fault < 8; ++fault) {
    reset(); Client c; c.put("a", "old"); assert(c.result.ok);
    if (fault == 0) file(0).back() ^= 1;
    if (fault == 1) file(0).pop_back();
    if (fault == 2) filesystem_test::files.erase("/command-bot/kv-0-a.bin");
    if (fault == 3) authority().back() ^= 1;
    if (fault == 4) authority().pop_back();
    if (fault == 5) { authority()[4] = 32; seal(authority().data(), authority().size()); }
    if (fault == 6) { authority()[5] = 2; seal(authority().data(), authority().size()); }
    if (fault == 7) identity_test::durable.erase({"mc-bot-kv", "txn"});
    const auto before = identity_test::durable; const auto files = filesystem_test::files;
    c.get("a"); assert(!c.result.ok);
    c.put("a", "new"); assert(!c.result.ok);
    BotStore reboot; char error[128]; assert(!reboot.recover(error, sizeof(error)));
    assert(before == identity_test::durable && files == filesystem_test::files);
  }
  puts("PASS short/truncated/changed file writes, unavailable stats/files, authority failures and corrupt restart fail closed");
}
static void uncertainRetry() {
  for (bool eager : {false, true}) for (unsigned failure = 0; failure < 3; ++failure) {
    reset(); identity_test::eagerWrites = eager;
    Client c; c.put("a", "old"); assert(c.result.ok);
    c.get("a"); assert(c.result.ok);
    if (!failure) identity_test::failCommit = true;
    if (failure == 1) identity_test::afterWrite = [] { identity_test::failRead = true; };
    if (failure == 2) identity_test::afterCommit = [] { identity_test::failRead = true; };
    c.put("a", "uncertain");
    assert(!c.result.ok && c.result.outcome == BotIoResult::Unknown);
    faultsOff();
    auto reads = filesystem_test::readCalls;
    c.get("a"); assert(c.result.ok);
    assert(!strcmp(c.result.value, eager || failure ? "uncertain" : "old"));
    assert(filesystem_test::readCalls == reads + 5);
    c.get("a"); assert(c.result.ok && filesystem_test::readCalls == reads + 5);
    // An unsuccessful retry must never overwrite the bank just committed above.
    filesystem_test::writeLimit = 10;
    c.put("a", "retry"); assert(!c.result.ok);
    faultsOff();
    reads = filesystem_test::readCalls;
    c.get("a");
    assert(c.result.ok && !strcmp(c.result.value, eager || failure ? "uncertain" : "old"));
    assert(filesystem_test::readCalls == reads + 5);
    Client reboot; reboot.get("a");
    assert(reboot.result.ok && !strcmp(reboot.result.value, eager || failure ? "uncertain" : "old"));
    c.put("a", "final"); assert(c.result.ok);
    reboot.get("a"); assert(reboot.result.ok && !strcmp(reboot.result.value, "final"));
  }
  puts("PASS same-instance uncertain commit retries re-read committed authority; failed retry cannot clobber active bank");
}
static void cachedRecovery() {
  reset();
  Client c; c.put("a", "old"); assert(c.result.ok);
  auto reads = filesystem_test::readCalls, bytes = filesystem_test::bytesRead;
  recover(c.store);
  assert(filesystem_test::readCalls == reads + 5 && filesystem_test::bytesRead == bytes + 5 * 3136);
  reads = filesystem_test::readCalls; bytes = filesystem_test::bytesRead;
  static unsigned authorityReads;
  authorityReads = 0;
  identity_test::readHook = [](const char *key) { assert(!strcmp(key, "txn")); ++authorityReads; };
  for (unsigned i = 0; i < 60; ++i) {
    now += 1000;
    recover(c.store);
    c.get("a"); assert(c.result.ok && !strcmp(c.result.value, "old"));
    c.request.kind = BotIoRequest::List; c.request.key[0] = 0; c.run();
    assert(c.result.ok && c.result.keys.count == 1);
    BotStore::Snapshot backup; backup.principal[0] = 7; char error[128];
    assert(c.store.snapshot(c.bot, backup, error, sizeof(error)) && backup.count == 1);
  }
  assert(authorityReads == 240 && filesystem_test::readCalls == reads && filesystem_test::bytesRead == bytes);
  faultsOff();
  c.put("a", "new"); assert(c.result.ok);
  assert(filesystem_test::readCalls == reads + 6); // Five selected groups, one inactive readback.
  reads = filesystem_test::readCalls;
  c.get("a"); assert(c.result.ok && !strcmp(c.result.value, "new"));
  assert(filesystem_test::readCalls == reads + 5);
  recover(c.store); assert(filesystem_test::readCalls == reads + 5);

  replace(0, record(1, BotIoRequest::Caller, 7, "a", "changed-authority"));
  reads = filesystem_test::readCalls;
  c.get("a"); assert(c.result.ok && !strcmp(c.result.value, "changed-authority"));
  assert(filesystem_test::readCalls == reads + 5);
  Client reboot; reads = filesystem_test::readCalls;
  reboot.get("a"); assert(reboot.result.ok && !strcmp(reboot.result.value, "changed-authority"));
  assert(filesystem_test::readCalls == reads + 5);

  for (unsigned fault = 0; fault < 6; ++fault) {
    const auto committed = authority();
    if (fault == 0) identity_test::failRead = true;
    if (fault == 1) authority().back() ^= 1;
    if (fault == 2) authority().pop_back();
    if (fault == 3) { authority()[4] = 32; seal(authority().data(), authority().size()); }
    if (fault == 4) authority().push_back(0);
    if (fault == 5) identity_test::durable.erase({"mc-bot-kv", "txn"});
    reads = filesystem_test::readCalls;
    c.get("a"); assert(!c.result.ok && filesystem_test::readCalls == reads);
    faultsOff(); identity_test::durable[{"mc-bot-kv", "txn"}] = committed;
    c.get("a"); assert(c.result.ok && !strcmp(c.result.value, "changed-authority"));
    assert(filesystem_test::readCalls == reads + 5);
    recover(c.store); assert(filesystem_test::readCalls == reads + 5);
  }
  puts("PASS verified arena: 240 exact-authority reads with zero redundant file reads; metadata change/error and reboot force full verification");
}
static void cachedCorruption() {
  for (unsigned mutation = 0; mutation < 5; ++mutation) {
    reset(); Client c; c.put("a", "old"); assert(c.result.ok);
    c.get("a"); assert(c.result.ok);
    BotStore::Snapshot backup; backup.principal[0] = 7; char error[128];
    assert(c.store.snapshot(c.bot, backup, error, sizeof(error)));
    file(39).back() ^= 1; // Corruption outside the group this mutation would change.
    const auto durable = identity_test::durable;
    const auto files = filesystem_test::files;
    const auto reads = filesystem_test::readCalls;
    c.get("a"); assert(c.result.ok && !strcmp(c.result.value, "old"));
    assert(filesystem_test::readCalls == reads); // A warm read does not scrub flash.
    if (mutation == 0) c.put("a", "new");
    if (mutation == 1) { c.request.kind = BotIoRequest::Delete; c.run(); }
    if (mutation == 2) c.transaction();
    if (mutation == 3) {
      c.request.kind = BotIoRequest::Cas; c.request.mutations = 1;
      auto &op = c.request.mutation[0]; strcpy(op.key, "a"); strcpy(op.value, "new");
      op.compare = op.present = true; strcpy(op.expected, "old"); c.run();
    }
    if (mutation == 4) c.store.restore(c.bot, backup, c.result, 1, c.generation);
    assert(!c.result.ok && strstr(c.result.error, "corrupt"));
    c.get("a"); assert(!c.result.ok && strstr(c.result.error, "corrupt"));
    Client reboot; reboot.get("a"); assert(!reboot.result.ok);
    assert(identity_test::durable == durable && filesystem_test::files == files);
    file(39).back() ^= 1;
    c.get("a"); assert(c.result.ok && !strcmp(c.result.value, "old"));
  }
  puts("PASS warm reads retain verified RAM; all KV mutation paths and reboot detect latent file corruption before writing");
}
static void repeatedEmptyRecords() {
  reset();
  Client fresh;
  fresh.get("absent"); assert(fresh.result.ok && !fresh.result.found);
  fresh.put("present", "retained"); assert(fresh.result.ok);
  for (unsigned fault = 0; fault < 6; ++fault) {
    reset();
    Client seed;
    seed.put("present", "retained"); assert(seed.result.ok);
    auto empty = read(39);
    assert(!empty.entry.used);
    if (fault == 0) empty.digest[0] ^= 1;
    if (fault == 1) { empty.magic[0] ^= 1; seal(&empty, sizeof(empty)); }
    if (fault == 2) { empty.entry.scope = 4; seal(&empty, sizeof(empty)); }
    if (fault == 3) {
      memset(empty.entry.key, 'x', sizeof(empty.entry.key));
      seal(&empty, sizeof(empty));
    }
    if (fault == 4) { empty.bot[0] = 9; seal(&empty, sizeof(empty)); }
    if (fault == 5) { empty.entry.value[0] = 'x'; seal(&empty, sizeof(empty)); }
    replace(39, empty);
    const auto durable = identity_test::durable;
    const auto files = filesystem_test::files;
    Client cold;
    cold.get("present");
    assert(cold.result.ok == (fault >= 4));
    if (cold.result.ok) assert(!strcmp(cold.result.value, "retained"));
    assert(durable == identity_test::durable && files == filesystem_test::files);
  }
  puts("PASS repeated unused records: exact-byte digest reuse cannot conceal changed digest/magic/scope/text; distinct valid tombstones retained");
}
static void interruptedEmptyInitialization() {
  reset();
  Client c;
  filesystem_test::afterWrite = [] { now += 2000; };
  c.put("first", "unpublished");
  assert(!c.result.ok && c.result.outcome == BotIoResult::Rejected);
  assert(authority().size() == 48 && !memcmp(authority().data(), "BKI\1", 4));
  assert(!filesystem_test::files.empty());
  faultsOff();
  filesystem_test::readDelayMs = 500;
  recover(c.store);
  assert(now == 2500 && authority().size() == 200 && authority()[5] == 0);
  for (unsigned i = 0; i < 40; ++i) assert(!read(i).entry.used);
  faultsOff();
  c.get("first"); assert(c.result.ok && !c.result.found);
  c.put("next", "committed"); assert(c.result.ok);
  c.get("next"); assert(c.result.ok && !strcmp(c.result.value, "committed"));
  Client reboot;
  reboot.get("first"); assert(reboot.result.ok && !reboot.result.found);
  reboot.get("next"); assert(reboot.result.ok && !strcmp(reboot.result.value, "committed"));
  puts("PASS interrupted empty initialization: slow checked recovery completes all banks, discards unpublished value, preserves 2s request fence and next/restart behavior");
}
static void slowEmptyMedia() {
  reset();
  filesystem_test::existsDelayMs = 250;
  Client c;
  BotStore::Snapshot backup; backup.principal[0] = 7; char error[128]{};
  assert(c.store.snapshot(c.bot, backup, error, sizeof(error)) && !backup.count);
  assert(now == 2500 && identity_test::durable.empty() && filesystem_test::files.empty());
  c.get("absent"); assert(c.result.ok && !c.result.found && now == 5000);
  c.put("first", "retained");
  assert(c.result.ok && c.result.outcome == BotIoResult::Committed);
  faultsOff();
  c.get("first"); assert(c.result.ok && !strcmp(c.result.value, "retained"));
  assert(c.store.snapshot(c.bot, backup, error, sizeof(error)) && backup.count == 1);

  for (bool transaction : {false, true}) {
    reset();
    filesystem_test::existsDelayMs = 250;
    Client fresh;
    fresh.request.kind = transaction ? BotIoRequest::Transaction : BotIoRequest::Cas;
    fresh.request.mutations = 1;
    auto &op = fresh.request.mutation[0];
    strcpy(op.key, "first"); strcpy(op.value, "retained");
    op.compare = !transaction; op.present = false;
    fresh.run();
    assert(fresh.result.ok && fresh.result.outcome == BotIoResult::Committed && now > 2500);
    faultsOff();
    fresh.get("first"); assert(fresh.result.ok && !strcmp(fresh.result.value, "retained"));
  }
  reset();
  filesystem_test::existsDelayMs = 250;
  Client restored;
  restored.store.restore(restored.bot, backup, restored.result, 1, restored.generation);
  assert(restored.result.ok && restored.result.outcome == BotIoResult::Committed && now > 2500);
  faultsOff();
  restored.get("first"); assert(restored.result.ok && !strcmp(restored.result.value, "retained"));

  for (unsigned cost : {BotKvRecoveryBudgetMs / 10, 1200u}) {
    reset();
    filesystem_test::existsDelayMs = cost;
    Client expired;
    BotStore::Snapshot empty; empty.principal[0] = 7;
    assert(!expired.store.snapshot(expired.bot, empty, error, sizeof(error)));
    assert(strstr(error, "deadline") &&
           now == ((BotKvRecoveryBudgetMs + cost - 1) / cost) * cost);
    assert(identity_test::durable.empty() && filesystem_test::files.empty());
  }
  puts("PASS slow empty-media preflight: first read/write/CAS/transaction/export/restore succeed; recovery remains bounded without weakening write deadlines");
}
static void migrationDeadlines() {
  for (bool withRedo : {false, true}) {
    reset();
    for (unsigned i = 0; i < 40; ++i) {
      char key[16]; snprintf(key, sizeof(key), "legacy%u", i);
      seed(i, record(1 + i % 2, i >= 32 ? BotIoRequest::Bot : BotIoRequest::Caller,
                     i >= 32 ? 0 : 7 + i / 8, key, "old"));
    }
    if (withRedo) {
      Redo redo; redo.count = 1; redo.slots[0] = 0;
      redo.records[0] = record(1, BotIoRequest::Caller, 7, "legacy0", "redo");
      seedRedo(redo);
    }
    filesystem_test::readDelayMs = 500;
    identity_test::readHook = [](const char *) { now += 25; };
    identity_test::afterErase = [] { now += 25; };
    Client c; recover(c.store);
    assert(now > 2000 && now < BotKvRecoveryBudgetMs);
    assert(authority().size() == 200 && authority()[5] == 0 && identity_test::durable.size() == 1);
    assert(filesystem_test::readCalls == 5);
    for (unsigned i = 0; i < 40; ++i)
      assert(!strcmp(read(i).entry.value, withRedo && !i ? "redo" : "old"));
    faultsOff();
    const auto reads = filesystem_test::readCalls;
    c.get("legacy0"); assert(c.result.ok && !strcmp(c.result.value, withRedo ? "redo" : "old"));
    recover(c.store); assert(filesystem_test::readCalls == reads);
    Client reboot; reboot.get("legacy0");
    assert(reboot.result.ok && !strcmp(reboot.result.value, withRedo ? "redo" : "old"));
  }
  for (unsigned phase = 0; phase < 6; ++phase) {
    reset();
    seed(0, record(1, BotIoRequest::Caller, 7, "a", "old"));
    seed(39, record(2, BotIoRequest::Bot, 0, "retained", "other"));
    Redo redo; redo.count = 1; redo.slots[0] = 0;
    redo.records[0] = record(1, BotIoRequest::Caller, 7, "a", "redo"); seedRedo(redo);
    const auto legacy = identity_test::durable;
    static unsigned commits; commits = 0;
    if (phase == 0) identity_test::readHook = [](const char *key) {
      if (!strcmp(key, "r07")) now += BotKvRecoveryBudgetMs;
    };
    if (phase == 1) filesystem_test::readDelayMs = BotKvRecoveryBudgetMs / 5;
    if (phase == 2) identity_test::afterCommit = [] { now += BotKvRecoveryBudgetMs; };
    if (phase == 3) identity_test::afterErase = [] { now += BotKvRecoveryBudgetMs; };
    if (phase == 4) identity_test::afterCommit = [] {
      if (++commits == 4) now += BotKvRecoveryBudgetMs; // Authority, two erases, completion.
    };
    if (phase == 5) filesystem_test::readDelayMs = (BotKvRecoveryBudgetMs - 5) / 5;
    Client c; char error[128]{};
    const bool success = c.store.recover(error, sizeof(error));
    assert(success == (phase == 5));
    if (!success) assert(strstr(error, "deadline") && now == BotKvRecoveryBudgetMs);
    if (phase <= 1) {
      assert(identity_test::durable == legacy);
      assert(filesystem_test::files.size() == (phase == 1 ? 5 : 0));
    } else {
      assert(authority().size() == 200 && authority()[5] == (phase < 4));
      if (phase == 2) assert(identity_test::durable.count({"mc-bot-kv", "v00"}));
      if (phase == 3) assert(!identity_test::durable.count({"mc-bot-kv", "v00"}));
    }
    faultsOff();
    if (phase % 2) { BotStore reboot; recover(reboot); }
    recover(c.store);
    assert(authority()[5] == 0 && identity_test::durable.size() == 1);
    c.get("a"); assert(c.result.ok && !strcmp(c.result.value, "redo"));
    assert(!strcmp(read(39).entry.value, "other"));
  }
  reset();
  seed(0, record(1, BotIoRequest::Caller, 7, "a", "old"));
  filesystem_test::readDelayMs = 500;
  Client c; c.put("a", "not-applied");
  assert(!c.result.ok && strstr(c.result.error, "deadline") && c.result.outcome == BotIoResult::Rejected);
  assert(now == 2500 && authority().size() == 200 && authority()[5] == 0);
  faultsOff(); c.get("a"); assert(c.result.ok && !strcmp(c.result.value, "old"));
  c.put("a", "retry"); assert(c.result.ok);

  Client cold; filesystem_test::readDelayMs = BotKvRecoveryBudgetMs / 5;
  char error[128];
  assert(!cold.store.recover(error, sizeof(error)) && strstr(error, "deadline"));
  faultsOff(); recover(cold.store);
  identity_test::readHook = [](const char *) { now += BotKvRecoveryBudgetMs; };
  assert(!cold.store.recover(error, sizeof(error)) && strstr(error, "deadline"));
  faultsOff();
  const auto reads = filesystem_test::readCalls;
  cold.get("a"); assert(cold.result.ok && !strcmp(cold.result.value, "retry"));
  assert(filesystem_test::readCalls == reads + 5);
  puts("PASS migration/recovery 10s checked budget: slow full-bank/redo success, scan/files/authority/reclaim/completion deadlines, committed retry and unchanged 2s request fence");
}
static void cancellationAndWear() {
  static Client *client;
  for (bool eager : {false, true}) for (unsigned fence = 0; fence < 4; ++fence)
    for (bool published : {false, true}) {
      reset(); identity_test::eagerWrites = eager;
      Client c; client = &c;
      c.request.scope = BotIoRequest::Channel; c.request.eventEpoch = 1;
      c.put("a", "old"); assert(c.result.ok);
      static unsigned mode;
      mode = fence;
      const auto revoke = [] {
        if (mode == 0) ++client->generation;
        if (mode == 1) ++client->grant;
        if (mode == 2) ++client->event;
        if (mode == 3) now += 2000;
      };
      if (published) identity_test::afterWrite = revoke;
      else filesystem_test::afterFlush = revoke;
      c.put("a", "new"); assert(!c.result.ok);
      assert(c.result.outcome == (published ? BotIoResult::Unknown : BotIoResult::Rejected));
      faultsOff();
      c.request.token.generation = c.generation; c.request.grant = c.grant;
      c.request.eventEpoch = c.event;
      c.get("a"); assert(c.result.ok);
      // Deadline is checked after NVS commit; generation/grant/event revocation
      // checks between set and commit, where ESP may already have published.
      assert(!strcmp(c.result.value, published && (eager || fence == 3) ? "new" : "old"));
    }
  reset(); Client c; c.put("a", "old"); assert(c.result.ok);
  static unsigned writes; writes = 0;
  filesystem_test::afterWrite = [] { ++writes; };
  for (unsigned i = 0; i < 30; ++i) { c.put("a", "update"); assert(c.result.ok); }
  faultsOff();
  assert(writes == 30 && identity_test::durable.size() == 1 && authority().size() == 200);
  assert(filesystem_test::files.size() == 6 && SPIFFS.usedBytes() == 6 * 3136);
  puts("PASS cancellation/grant/event/deadline fences; steady scalar update writes one 3136-byte group and constant NVS authority");
}
int main() {
  atomicCuts(); legacyMigration(); restoreCuts(); restoreCancellation(); physicalMigration();
  initializationBudget(); corruptMigration(); ioFailures(); uncertainRetry(); cancellationAndWear();
  cachedRecovery(); cachedCorruption(); repeatedEmptyRecords(); interruptedEmptyInitialization(); slowEmptyMedia(); migrationDeadlines();
  reset();
  assert(psram_test::allocations.empty());
}
