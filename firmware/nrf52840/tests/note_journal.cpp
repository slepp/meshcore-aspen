#include "NoteJournal.h"
#include "NotePartition.h"
#include "NoteDiagnostics.h"
#include "NoteStore.h"
#include <littlefs/lfs.h>
#include <algorithm>
#include <cassert>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <numeric>
#include <vector>

extern "C" void* pvPortMalloc(size_t bytes) { return std::malloc(bytes); }
extern "C" void vPortFree(void* pointer) { std::free(pointer); }

namespace {
using Journal = nrfmast::NoteJournal;
struct Clock : mesh::MillisecondClock {
  uint32_t now = 1000;
  unsigned long getMillis() override { return now; }
};
struct Nor : nrfmast::NoteFlash {
  std::vector<uint8_t> bytes = std::vector<uint8_t>(0x200000, 0xff);
  unsigned erases[512] = {}, programs = 0;
  int operations = 0, fault = -1, partial = -1, publication = -1, corruptReadAt = -1;
  std::vector<std::pair<uint32_t, uint8_t>> eraseDamage;
  bool detected = true, free = true, applyFault = false, wholeVolume = false, actualGuard = false;
  unsigned guardCalls = 0, retiredCalls = 0;
  bool fail() { return ++operations == fault; }
  void power() { fault = partial = corruptReadAt = -1; operations = 0; applyFault = false; eraseDamage.clear(); }
  bool begin() override { return detected; }
  bool partitionFree(uint32_t address, uint32_t count) override {
    assert(address == Journal::BASE && count == Journal::BYTES);
    ++guardCalls;
    return actualGuard ? nrfmast::notePartitionFree(*this, address, count) : free;
  }
  bool retiredPartition(uint32_t address, uint32_t count) override {
    assert(address == Journal::BASE && count == Journal::BYTES);
    ++retiredCalls;
    return detected;
  }
  bool read(uint32_t address, void* output, size_t count) override {
    assert(address + count <= bytes.size() && !(count & 3));
    assert(!(reinterpret_cast<uintptr_t>(output) & 3));
    if (fail()) return false;
    memcpy(output, bytes.data() + address, count);
    if (operations == corruptReadAt) static_cast<uint8_t*>(output)[0] ^= 1;
    return true;
  }
  bool program(uint32_t address, const void* input, size_t count) override {
    ++programs;
    assert(address + count <= bytes.size());
    assert(wholeVolume || address >= Journal::BASE);
    assert(!(reinterpret_cast<uintptr_t>(input) & 3) && !(count & 3));
    const auto* data = static_cast<const uint8_t*>(input);
    while (count) {
      const size_t chunk = std::min(count, size_t(256 - (address & 255)));
      const bool failed = fail();
      if ((address % Journal::SECTOR) == Journal::SECTOR - 8 ||
          (address % Journal::SECTOR) == Journal::SECTOR - 28 - 32) publication = operations;
      if (failed && !applyFault) return false;
      const size_t written = failed && partial >= 0 ? std::min(chunk, size_t(partial)) : chunk;
      for (size_t i = 0; i < written; ++i) {
        assert((bytes[address + i] & data[i]) == data[i]);
        bytes[address + i] &= data[i];
      }
      if (failed) return false;
      address += chunk;
      data += chunk;
      count -= chunk;
    }
    return true;
  }
  bool erase(uint32_t address) override {
    assert(address % Journal::SECTOR == 0 && address + Journal::SECTOR <= bytes.size());
    assert(wholeVolume || address >= Journal::BASE);
    const bool failed = fail();
    if (failed && !applyFault) return false;
    if (failed && !eraseDamage.empty()) {
      for (const auto& damage : eraseDamage) bytes[address + damage.first] |= damage.second;
    } else std::fill(bytes.begin() + address, bytes.begin() + address + Journal::SECTOR, 0xff);
    ++erases[address / Journal::SECTOR];
    return !failed;
  }
};

void testCutsAndWear() {
  Nor nor;
  Journal journal(nor);
  assert(journal.begin() && !journal.hasSnapshot());
  memset(journal.data(), 0x11, Journal::PAYLOAD_BYTES);
  assert(journal.commit() == Journal::Result::Committed);
  const auto baseline = nor.bytes;
  nor.operations = 0;
  memset(journal.data(), 0x77, Journal::PAYLOAD_BYTES);
  assert(journal.commit() == Journal::Result::Committed);
  const int steps = nor.operations, publication = nor.publication;
  for (int cut = 1; cut <= steps; ++cut) {
    for (bool applied : {false, true}) {
      nor.bytes = baseline;
      nor.power();
      Journal candidate(nor);
      assert(candidate.begin());
      memset(candidate.data(), 0x77, Journal::PAYLOAD_BYTES);
      nor.operations = 0;
      nor.fault = cut;
      nor.applyFault = applied;
      assert(candidate.commit() != Journal::Result::Committed);
      nor.power();
      Journal boot(nor);
      if (!boot.begin()) {
        assert(cut >= publication - 2);  // Complete preparation / torn publication is intentionally fail-closed.
        continue;
      }
      assert(boot.generation() == 1 || boot.generation() == 2);
      assert(boot.data()[0] == (boot.generation() == 2 ? 0x77 : 0x11));
      if (boot.generation() == 2) assert(cut >= publication);
      const uint64_t generation = boot.generation();
      memset(boot.data(), 0x55, Journal::PAYLOAD_BYTES);
      assert(boot.commit() == Journal::Result::Committed && boot.generation() == generation + 1);
    }
  }
  // Half-published redundant marker is not mistaken for an older durable state.
  nor.bytes = baseline;
  nor.power();
  Journal torn(nor);
  assert(torn.begin());
  memset(torn.data(), 0x77, Journal::PAYLOAD_BYTES);
  nor.operations = 0;
  nor.fault = publication;
  nor.applyFault = true;
  nor.partial = 28;
  assert(torn.commit() == Journal::Result::Uncertain && !torn.ready());
  nor.power();
  Journal tornBoot(nor);
  assert(!tornBoot.begin());

  nor.bytes = baseline;
  nor.power();
  Journal wrap(nor);
  assert(wrap.begin());
  memset(nor.erases, 0, sizeof(nor.erases));
  for (unsigned i = 0; i < 300; ++i) {
    memset(wrap.data(), i & 255, Journal::PAYLOAD_BYTES);
    assert(wrap.commit() == Journal::Result::Committed);
  }
  assert(wrap.generation() == 301 && wrap.usedSlots() == 128);
  unsigned total = 0, minimum = 999, maximum = 0;
  for (unsigned i = 0; i < 512; ++i) {
    if (i < Journal::BASE / Journal::SECTOR) assert(nor.erases[i] == 0);
    else {
      total += nor.erases[i];
      minimum = std::min(minimum, nor.erases[i]);
      maximum = std::max(maximum, nor.erases[i]);
    }
  }
  assert(total == 300 && maximum - minimum == 1);
  Journal boot(nor);
  assert(boot.begin() && boot.generation() == 301 && boot.data()[0] == (299 & 255));
  const auto committed = nor.bytes;
  const uint32_t latest = Journal::BASE + (300 % Journal::SLOTS) * Journal::SECTOR;
  for (uint32_t offset : {0U, 36U, Journal::SECTOR - 16, Journal::SECTOR - 8}) {
    nor.bytes = committed;
    nor.bytes[latest + offset] ^= 0x80;
    Journal corrupted(nor);
    assert(!corrupted.begin());  // Never adopt the preceding replay watermark.
  }
  std::printf("QSPI journal: %d power-cut phases; 300 commits / 300 erases, 128 sectors, min=%u max=%u\n",
              steps, minimum, maximum);
}

void testWrappedPartialErase() {
  Nor nor;
  Journal journal(nor);
  assert(journal.begin());
  for (unsigned generation = 1; generation <= Journal::SLOTS; ++generation) {
    memset(journal.data(), generation & 255, Journal::PAYLOAD_BYTES);
    assert(journal.commit() == Journal::Result::Committed);
  }
  const auto wrapped = nor.bytes;
  const std::vector<uint8_t> latest(journal.data(), journal.data() + Journal::PAYLOAD_BYTES);
  auto damagedHash = [&](uint32_t address) {
    for (uint32_t offset = 28; offset < 32; ++offset) {
      const uint8_t clear = ~nor.bytes[address + offset];
      if (clear) return std::make_pair(offset, uint8_t(clear & -clear));
    }
    assert(false);
    return std::make_pair(28U, uint8_t(1));
  };
  const auto hashDamage = damagedHash(Journal::BASE);
  const std::pair<uint32_t, uint8_t> damages[] = {
      {0, 0xff}, {Journal::SECTOR - 8, 4}, hashDamage};
  for (const auto& damage : damages) {
    nor.bytes = wrapped;
    nor.power();
    Journal candidate(nor);
    assert(candidate.begin() && candidate.generation() == Journal::SLOTS);
    memset(candidate.data(), 0x99, Journal::PAYLOAD_BYTES);
    nor.operations = 0;
    nor.fault = 1;
    nor.applyFault = true;
    nor.eraseDamage.push_back(damage);
    assert(candidate.commit() == Journal::Result::Failed);
    assert(nor.bytes != wrapped);
    nor.power();
    Journal recovered(nor);
    assert(recovered.begin() && recovered.generation() == Journal::SLOTS);
    assert(!memcmp(recovered.data(), latest.data(), latest.size()));
    memset(recovered.data(), 0x99, Journal::PAYLOAD_BYTES);
    assert(recovered.commit() == Journal::Result::Committed);
    Journal boot(nor);
    assert(boot.begin() && boot.generation() == Journal::SLOTS + 1 && boot.data()[0] == 0x99);
    const auto committed = nor.bytes;

    // The same damaged fields in the actually committed newest target cannot become "staging".
    for (auto newestDamage : damages) {
      nor.bytes = committed;
      if (newestDamage.first >= 28 && newestDamage.first < 32)
        newestDamage = damagedHash(Journal::BASE);
      nor.bytes[Journal::BASE + newestDamage.first] |= newestDamage.second;
      Journal corruptNewest(nor);
      assert(!corruptNewest.begin());
    }
    // Its publication is in the predecessor, not in the sector that the next erase reuses.
    nor.bytes = committed;
    const uint32_t predecessor = Journal::BASE + (Journal::SLOTS - 1) * Journal::SECTOR;
    for (uint32_t offset : {0U, 8U, 20U, 24U}) {
      nor.bytes = committed;
      nor.bytes[predecessor + Journal::SECTOR - 28 - 32 + offset] ^= 0x80;
      Journal corruptPublication(nor);
      assert(!corruptPublication.begin());
    }
    nor.bytes = committed;
    std::fill(nor.bytes.begin() + predecessor + Journal::SECTOR - 28 - 32,
              nor.bytes.begin() + predecessor + Journal::SECTOR - 28, 0xff);
    Journal lostPublication(nor);
    assert(!lostPublication.begin());  // Intact newer snapshot must not be reclassified as old staging.
  }
  std::puts("QSPI wrapped erase: partial magic/marker/header erasure retains generation 128; "
            "next commit succeeds; newest target/publication corruption fails closed");
}

void testWrappedReplayFence() {
  Clock clock;
  NativeFilesystem fs;
  Nor nor;
  Journal journal(nor);
  nrfmast::NoteStore notes(fs, clock, &journal);
  uint8_t bot[32] = {1}, sender[32] = {2};
  char reply[161];
  assert(notes.begin());
  const unsigned opens = fs.writeOpens;
  for (unsigned i = 0; i < Journal::SLOTS - 1; ++i) {
    clock.now += nrfmast::NoteStore::WRITE_REFILL_MS;
    const std::string text = "!remember a value" + std::to_string(i);
    assert(notes.command(bot, sender, 100 + i, text.c_str(), reply, sizeof(reply)));
    assert(!strcmp(reply, "remembered a"));
  }
  assert(journal.generation() == Journal::SLOTS);
  nor.operations = 0;
  nor.fault = 1;
  nor.applyFault = true;
  nor.eraseDamage.push_back({0, 0xff});
  assert(notes.command(bot, sender, 227, "!remember a failed", reply, sizeof(reply)));
  assert(strstr(reply, "commit failed"));
  nor.power();
  Journal after(nor);
  nrfmast::NoteStore restarted(fs, clock, &after);
  assert(restarted.begin() && after.generation() == Journal::SLOTS);
  assert(restarted.command(bot, sender, 230, "!recall a", reply, sizeof(reply)));
  assert(!strcmp(reply, "a=value126"));
  assert(restarted.command(bot, sender, 226, "!remember a stale", reply, sizeof(reply)));
  assert(strstr(reply, "stale write"));
  assert(restarted.command(bot, sender, 228, "!remember a fresh", reply, sizeof(reply)));
  assert(!strcmp(reply, "remembered a") && after.generation() == Journal::SLOTS + 1);
  assert(fs.writeOpens == opens && restarted.countPrincipals() == 1);
  std::puts("QSPI wrapped notes: retained full snapshot and durable timestamp 226; stale replay rejected; fresh write accepted");
}

void testMigration() {
  Clock clock;
  NativeFilesystem fs;
  uint8_t bot[32] = {1}, sender[32] = {2};
  char reply[161];
  nrfmast::NoteStore legacy(fs, clock);
  assert(legacy.begin());
  assert(legacy.command(bot, sender, 100, "!remember a before", reply, sizeof(reply)));
  assert(legacy.command(bot, sender, 101, "!forget a", reply, sizeof(reply)));
  const auto internal = *fs.files.at("/pine-notes");
  Nor nor;
  Journal journal(nor);
  nrfmast::NoteStore migrated(fs, clock, &journal);
  assert(migrated.begin() && migrated.countPrincipals() == 1 && fs.exists("/pine-notes.qspi"));
  assert(*fs.files.at("/pine-notes") == internal && migrated.countNotes() == 0);
  const unsigned opens = fs.writeOpens;
  assert(migrated.command(bot, sender, 100, "!remember a replay", reply, sizeof(reply)));
  assert(strstr(reply, "stale write"));
  assert(migrated.command(bot, sender, 102, "!remember a new", reply, sizeof(reply)));
  assert(!strcmp(reply, "remembered a") && fs.writeOpens == opens);
  assert(migrated.command(bot, sender, 103, "!forget a", reply, sizeof(reply)));
  assert(!strcmp(reply, "forgot a") && fs.writeOpens == opens);
  Journal next(nor);
  nrfmast::NoteStore reboot(fs, clock, &next);
  assert(reboot.begin());
  assert(reboot.command(bot, sender, 102, "!remember a delayed", reply, sizeof(reply)));
  assert(strstr(reply, "stale write") && fs.writeOpens == opens);
  assert(reboot.command(bot, sender, 200, "!forget a", reply, sizeof(reply)));
  assert(strstr(reply, "no change; write timestamp not committed") && fs.writeOpens == opens);
  assert(reboot.command(bot, sender, 104, "!remember a after", reply, sizeof(reply)));
  assert(!strcmp(reply, "remembered a"));

  const auto external = nor.bytes;
  std::fill(nor.bytes.begin() + Journal::BASE, nor.bytes.end(), 0xff);
  Journal lost(nor);
  nrfmast::NoteStore lostStore(fs, clock, &lost);
  assert(!lostStore.begin() && strstr(lostStore.error(), "old InternalFS replay state was not restored"));
  assert(fs.writeOpens == opens);
  nor.bytes = external;
  fs.files.at("/pine-notes.qspi")->at(0) ^= 0x80;
  Journal damaged(nor);
  nrfmast::NoteStore damagedStore(fs, clock, &damaged);
  assert(!damagedStore.begin() && fs.writeOpens == opens);
  assert(damagedStore.reset() && damagedStore.ready() && !damagedStore.countPrincipals());

  NativeFilesystem failedFS;
  Nor interrupted;
  failedFS.failRename = true;
  Journal initial(interrupted);
  nrfmast::NoteStore incomplete(failedFS, clock, &initial);
  assert(!incomplete.begin() && !failedFS.exists("/pine-notes.qspi") && initial.hasSnapshot());
  failedFS.failRename = false;
  Journal resumed(interrupted);
  nrfmast::NoteStore resumedStore(failedFS, clock, &resumed);
  const auto beforeRetry = interrupted.bytes;
  assert(resumedStore.begin() && interrupted.bytes == beforeRetry);
  assert(failedFS.exists("/pine-notes.qspi"));
  interrupted.free = false;
  const int ops = interrupted.operations;
  Journal occupied(interrupted);
  nrfmast::NoteStore occupiedStore(failedFS, clock, &occupied);
  assert(!occupiedStore.begin() && interrupted.operations == ops);
  interrupted.free = true;
  interrupted.detected = false;
  assert(!occupiedStore.begin());

  NativeFilesystem uncertainFS;
  Nor uncertain;
  Journal active(uncertain);
  nrfmast::NoteStore activeStore(uncertainFS, clock, &active);
  assert(activeStore.begin());
  uncertain.operations = 0;
  assert(activeStore.command(bot, sender, 10, "!remember a first", reply, sizeof(reply)));
  const int publication = uncertain.publication;
  uncertain.operations = 0;
  uncertain.fault = publication;
  uncertain.applyFault = true;
  assert(activeStore.command(bot, sender, 11, "!remember a second", reply, sizeof(reply)));
  assert(strstr(reply, "commit outcome uncertain") && !activeStore.ready());
  assert(activeStore.command(bot, sender, 12, "!recall a", reply, sizeof(reply)));
  assert(strstr(reply, "do not replay"));
  uncertain.power();
  Journal resolved(uncertain);
  nrfmast::NoteStore resolvedStore(uncertainFS, clock, &resolved);
  assert(resolvedStore.begin());
  assert(resolvedStore.command(bot, sender, 12, "!recall a", reply, sizeof(reply)));
  assert(!strcmp(reply, "a=second"));
  assert(resolvedStore.command(bot, sender, 11, "!remember a replay", reply, sizeof(reply)));
  assert(strstr(reply, "stale write"));
  uncertain.operations = 0;
  uncertain.fault = 1;
  assert(!resolvedStore.reset() && !resolvedStore.ready());
  uncertain.power();
  Journal resetting(uncertain);
  nrfmast::NoteStore resetStore(uncertainFS, clock, &resetting);
  assert(!resetStore.begin() && strstr(resetStore.error(), "reset interrupted"));
  assert(resetStore.reset() && resetStore.ready() && resetStore.countPrincipals() == 0);
  assert(uncertainFS.activeHandles == 0);
  std::puts("QSPI migration: checked marker, retired internal snapshot, zero later InternalFS writes, durable replay fences");
}

using InternalFiles = std::map<std::string, std::vector<uint8_t>>;
InternalFiles snapshotFiles(const NativeFilesystem& fs) {
  InternalFiles files;
  for (const auto& item : fs.files) files[item.first] = *item.second;
  return files;
}
void restoreFiles(NativeFilesystem& fs, const InternalFiles& files) {
  fs = NativeFilesystem();
  for (const auto& item : files)
    fs.files[item.first] = std::make_shared<std::vector<uint8_t>>(item.second);
}
void unchangedFiles(const NativeFilesystem& fs, const InternalFiles& files) {
  for (const auto& item : files) assert(*fs.files.at(item.first) == item.second);
  for (const auto& item : fs.files)
    assert(files.count(item.first) || item.first == "/pine-notes.qspi" ||
           item.first == "/pine-notes.qspi.new");
  assert(!fs.activeHandles);
}
uint32_t markerVersion(const NativeFilesystem& fs) {
  if (!fs.exists("/pine-notes.qspi")) return 0;
  uint32_t version;
  const auto& marker = *fs.files.at("/pine-notes.qspi");
  assert(marker.size() == 28);
  memcpy(&version, marker.data() + 20, sizeof(version));
  return version;
}
void retainedProvisionedNotes(nrfmast::NoteStore& notes) {
  const uint8_t bot[32] = {3}, deleted[32] = {7}, kept[32] = {8};
  char reply[161];
  assert(notes.ready() && notes.externalVolumeRetired());
  assert(notes.countNotes() == 1 && notes.countPrincipals() == 2);
  assert(notes.command(bot, kept, 200, "!recall b", reply, sizeof(reply)) && !strcmp(reply, "b=kept"));
  assert(notes.command(bot, deleted, 200, "!recall a", reply, sizeof(reply)) && strstr(reply, "key not found"));
  assert(notes.command(bot, deleted, 100, "!remember a replay", reply, sizeof(reply)) && strstr(reply, "stale write"));
  assert(notes.command(bot, kept, 102, "!remember b replay", reply, sizeof(reply)) && strstr(reply, "stale write"));
}

void testOwnerProvisioning() {
  Clock clock;
  NativeFilesystem fs;
  const uint8_t bot[32] = {3}, deleted[32] = {7}, kept[32] = {8};
  char reply[161];
  nrfmast::NoteStore legacy(fs, clock);
  assert(legacy.begin());
  assert(legacy.command(bot, deleted, 100, "!remember a deleted", reply, sizeof(reply)));
  assert(legacy.command(bot, deleted, 101, "!forget a", reply, sizeof(reply)));
  assert(legacy.command(bot, kept, 102, "!remember b kept", reply, sizeof(reply)));
  for (const char* path : {"/_main.id", "/_nrfbot.id", "/prefs.json", "/nrf-runtime", "/ble"})
    fs.files[path] = std::make_shared<std::vector<uint8_t>>(64, 0x35);
  const auto internal = snapshotFiles(fs);
  Nor nor;
  nor.free = false;
  std::fill(nor.bytes.begin(), nor.bytes.end(), 0x52);
  const auto external = nor.bytes;
  auto clean = [&]() {
    restoreFiles(fs, internal);
    nor.bytes = external;
    nor.power();
    nor.free = false;
    nor.detected = true;
    nor.programs = nor.guardCalls = nor.retiredCalls = 0;
    std::fill(std::begin(nor.erases), std::end(nor.erases), 0);
  };
  clean();
  Journal occupied(nor);
  nrfmast::NoteStore original(fs, clock, &occupied);
  assert(!original.begin() && !original.ready() && !nor.retiredCalls);
  assert(!original.reset() && !fs.exists("/pine-notes.qspi") && nor.bytes == external);
  assert(original.provisionRetiredVolume());
  retainedProvisionedNotes(original);
  assert(markerVersion(fs) == 4 && !memcmp(nor.bytes.data(), external.data(), Journal::BASE));
  unchangedFiles(fs, internal);
  const int phases = nor.operations;
  const auto complete = nor.bytes;
  const auto completeFiles = snapshotFiles(fs);
  const auto opens = fs.writeOpens;
  assert(!original.provisionRetiredVolume() && original.ready());
  assert(nor.bytes == complete && fs.writeOpens == opens);
  assert(original.command(bot, kept, 103, "!remember b updated", reply, sizeof(reply)));
  assert(!strcmp(reply, "remembered b"));
  const auto updated = nor.bytes;
  assert(!original.provisionRetiredVolume() && nor.bytes == updated);
  assert(original.command(bot, kept, 200, "!recall b", reply, sizeof(reply)) && !strcmp(reply, "b=updated"));

  for (int cut = 1; cut <= phases; ++cut) {
    for (bool applied : {false, true}) {
      clean();
      nor.fault = cut;
      nor.applyFault = applied;
      nor.partial = 1;
      nor.eraseDamage = {{0, 0x80}};
      Journal interrupted(nor);
      nrfmast::NoteStore interruptedStore(fs, clock, &interrupted);
      const bool done = interruptedStore.provisionRetiredVolume();
      assert(done || !interruptedStore.ready());
      assert(!memcmp(nor.bytes.data(), external.data(), Journal::BASE));
      unchangedFiles(fs, internal);
      nor.power();
      Journal rebooted(nor);
      nrfmast::NoteStore rebootedStore(fs, clock, &rebooted);
      if (markerVersion(fs) == 4) {
        assert(rebootedStore.begin());
        retainedProvisionedNotes(rebootedStore);
        const auto durable = nor.bytes;
        assert(!rebootedStore.provisionRetiredVolume() && nor.bytes == durable);
      } else {
        assert(markerVersion(fs) == 3 && !rebootedStore.begin());
        assert(strstr(rebootedStore.error(), "provisioning interrupted") && !nor.guardCalls);
        const auto partial = nor.bytes;
        assert(!rebootedStore.reset() && nor.bytes == partial);
        assert(rebootedStore.command(bot, kept, 200, "!remember b changed", reply, sizeof(reply)));
        assert(strstr(reply, "provision") && nor.bytes == partial);
        assert(rebootedStore.provisionRetiredVolume());
        retainedProvisionedNotes(rebootedStore);
      }
      unchangedFiles(fs, internal);
      assert(!memcmp(nor.bytes.data(), external.data(), Journal::BASE));
    }
  }

  for (int mode = 0; mode < 9; ++mode) {
    clean();
    if (mode == 0) fs.writeBudget = 27;
    if (mode == 1) fs.failRename = true;
    if (mode == 2) { fs.failRename = true; fs.applyRenameFailure = true; }
    if (mode == 3) fs.writeBudget = 28;
    if (mode >= 4) {
      unsigned renamed = 0;
      fs.afterRename = [&, renamed](const char*) mutable {
        ++renamed;
        if ((mode == 4 && renamed == 1) || (mode == 5 && renamed == 2)) fs.failOpen = true;
        if (mode == 6 && renamed == 1) fs.files.at("/pine-notes.qspi")->at(24) ^= 1;
        if (mode == 7 && renamed == 1) { fs.failRename = true; fs.applyRenameFailure = true; }
        if (mode == 8 && renamed == 2) fs.files.at("/pine-notes.qspi")->at(24) ^= 1;
      };
    }
    Journal faulted(nor);
    nrfmast::NoteStore faultedStore(fs, clock, &faulted);
    assert(!faultedStore.provisionRetiredVolume() && !faultedStore.ready());
    if (mode == 0 || mode == 1 || mode == 2 || mode == 4 || mode == 6)
      assert(nor.bytes == external && !nor.programs);
    unchangedFiles(fs, internal);
    fs.writeBudget = -1;
    fs.failOpen = fs.failRename = fs.applyRenameFailure = false;
    fs.afterRename = {};
    nor.power();
    Journal restarted(nor);
    nrfmast::NoteStore restartedStore(fs, clock, &restarted);
    const bool ready = restartedStore.begin();
    if (mode == 5 || mode == 7) {
      assert(ready && markerVersion(fs) == 4);
      retainedProvisionedNotes(restartedStore);
      const auto durable = nor.bytes;
      assert(!restartedStore.provisionRetiredVolume() && nor.bytes == durable);
    } else {
      assert(!ready);
      if (mode == 6 || mode == 8) {
        const auto damaged = nor.bytes;
        assert(!restartedStore.provisionRetiredVolume() && !restartedStore.reset() && nor.bytes == damaged);
      } else {
        assert(restartedStore.provisionRetiredVolume());
        retainedProvisionedNotes(restartedStore);
      }
    }
    unchangedFiles(fs, internal);
    assert(!memcmp(nor.bytes.data(), external.data(), Journal::BASE));
  }

  for (int invalid = 0; invalid < 3; ++invalid) {
    clean();
    if (invalid == 0) fs.remove("/pine-notes");
    if (invalid == 1) fs.files.at("/pine-notes")->at(0) ^= 1;
    if (invalid == 2) {
      auto& record = *fs.files.at("/pine-notes");
      memset(record.data() + 12 + 16 * 178 + 64, 0, 4);
      uint32_t digest = 2166136261U;
      for (size_t i = 12; i < record.size(); ++i) digest = (digest ^ record[i]) * 16777619U;
      memcpy(record.data() + 8, &digest, sizeof(digest));
    }
    Journal invalidJournal(nor);
    nrfmast::NoteStore invalidStore(fs, clock, &invalidJournal);
    assert(!invalidStore.provisionRetiredVolume());
    assert(!fs.exists("/pine-notes.qspi") && nor.bytes == external && !nor.programs);
  }
  clean();
  fs.writeBudget = 28;
  Journal pending(nor);
  nrfmast::NoteStore pendingStore(fs, clock, &pending);
  assert(!pendingStore.provisionRetiredVolume() && markerVersion(fs) == 3);
  const auto pendingBytes = nor.bytes;
  fs.writeBudget = -1;
  fs.files.at("/pine-notes")->at(0) ^= 1;
  Journal pendingBoot(nor);
  nrfmast::NoteStore pendingBootStore(fs, clock, &pendingBoot);
  assert(!pendingBootStore.begin() && !pendingBootStore.provisionRetiredVolume());
  assert(nor.bytes == pendingBytes && !pendingBootStore.ready());
  clean();
  nor.corruptReadAt = 2;
  Journal badErase(nor);
  nrfmast::NoteStore badEraseStore(fs, clock, &badErase);
  assert(!badEraseStore.provisionRetiredVolume() && markerVersion(fs) == 3);
  assert(strstr(badEraseStore.error(), "erase verification failed") && !nor.programs);
  assert(!memcmp(nor.bytes.data(), external.data(), Journal::BASE));
  clean();
  nor.detected = false;
  Journal wrongChip(nor);
  nrfmast::NoteStore wrongChipStore(fs, clock, &wrongChip);
  assert(!wrongChipStore.provisionRetiredVolume() && nor.bytes == external && !nor.programs);

  restoreFiles(fs, completeFiles);
  nor.bytes = complete;
  nor.power();
  nor.detected = true;
  Journal resetting(nor);
  nrfmast::NoteStore resettingStore(fs, clock, &resetting);
  assert(resettingStore.begin());
  nor.operations = 0;
  nor.fault = 1;
  assert(!resettingStore.reset() && markerVersion(fs) == 5);
  nor.power();
  Journal resetBoot(nor);
  nrfmast::NoteStore resetBootStore(fs, clock, &resetBoot);
  assert(!resetBootStore.begin() && strstr(resetBootStore.error(), "reset interrupted"));
  assert(!resetBootStore.provisionRetiredVolume());
  assert(resetBootStore.reset() && resetBootStore.ready() && markerVersion(fs) == 4);
  assert(!resetBootStore.countNotes() && !resetBootStore.countPrincipals() && !fs.exists("/pine-notes"));
  assert(!resetBootStore.provisionRetiredVolume());
  assert(!memcmp(nor.bytes.data(), external.data(), Journal::BASE));
  for (const auto& item : internal)
    if (item.first != "/pine-notes") assert(*fs.files.at(item.first) == item.second);
  for (bool absent : {false, true}) {
    restoreFiles(fs, completeFiles);
    nor.bytes = complete;
    nor.power();
    if (absent) std::fill(nor.bytes.begin() + Journal::BASE, nor.bytes.end(), 0xff);
    else nor.bytes[Journal::BASE + 64] ^= 1;
    const auto damaged = nor.bytes;
    Journal lost(nor);
    nrfmast::NoteStore lostStore(fs, clock, &lost);
    assert(!lostStore.begin() && !lostStore.ready());
    assert(!lostStore.provisionRetiredVolume() && nor.bytes == damaged);
    unchangedFiles(fs, internal);
  }
  std::printf("owner QSPI provisioning: %d operation-cut phases x applied/not-applied, marker faults/restarts, full notes/replay retention; outside-region/current keys/config unchanged\n", phases);
}

int fsRead(const lfs_config* config, lfs_block_t block, lfs_off_t offset,
           void* output, lfs_size_t bytes) {
  return static_cast<Nor*>(config->context)->read(block * 4096 + offset, output, bytes) ? 0 : -1;
}
int fsProgram(const lfs_config* config, lfs_block_t block, lfs_off_t offset,
              const void* input, lfs_size_t bytes) {
  return static_cast<Nor*>(config->context)->program(block * 4096 + offset, input, bytes) ? 0 : -1;
}
int fsErase(const lfs_config* config, lfs_block_t block) {
  return static_cast<Nor*>(config->context)->erase(block * 4096) ? 0 : -1;
}
int fsSync(const lfs_config*) { return 0; }
void collectVolume(void* context, const nrfmast::NoteVolumeEntry& entry) {
  static_cast<std::vector<nrfmast::NoteVolumeEntry>*>(context)->push_back(entry);
}

void testActualLegacyVolume() {
  Nor nor;
  nor.wholeVolume = true;
  nrfmast::NotePartitionStatus status;
  assert(!nrfmast::notePartitionFree(nor, Journal::BASE, Journal::BYTES, &status));
  assert(status.checked && status.mountResult == LFS_ERR_CORRUPT &&
         !strcmp(status.label(), "unreadable") && !status.overlapBlocks);
  lfs_config config = {};
  config.context = &nor;
  config.read = fsRead;
  config.prog = fsProgram;
  config.erase = fsErase;
  config.sync = fsSync;
  config.read_size = config.prog_size = 256;
  config.block_size = 4096;
  config.block_count = 512;
  config.lookahead = 32;
  lfs_t filesystem = {};
  assert(lfs_format(&filesystem, &config) == 0 && lfs_mount(&filesystem, &config) == 0);
  lfs_file_t file = {};
  assert(lfs_file_open(&filesystem, &file, "/legacy", LFS_O_WRONLY | LFS_O_CREAT) == 0);
  alignas(4) uint8_t data[256];
  memset(data, 0x51, sizeof(data));
  assert(lfs_file_write(&filesystem, &file, data, sizeof(data)) == sizeof(data));
  assert(lfs_file_close(&filesystem, &file) == 0);
  assert(lfs_mkdir(&filesystem, "/bl") == 0);
  assert(lfs_file_open(&filesystem, &file, "/bl/0001", LFS_O_WRONLY | LFS_O_CREAT) == 0);
  assert(lfs_file_write(&filesystem, &file, data, 64) == 64);
  assert(lfs_file_close(&filesystem, &file) == 0);
  lfs_unmount(&filesystem);
  const auto before = nor.bytes;
  const auto programCount = nor.programs;
  const auto eraseCount = std::accumulate(std::begin(nor.erases), std::end(nor.erases), 0U);
  nrfmast::NoteVolumeStatus volume;
  std::vector<nrfmast::NoteVolumeEntry> entries;
  assert(nrfmast::inspectNoteVolume(nor, collectVolume, &entries, volume));
  assert(volume.entries == 3 && !volume.limited && volume.blocks > 0);
  assert(nor.bytes == before && nor.programs == programCount &&
         std::accumulate(std::begin(nor.erases), std::end(nor.erases), 0U) == eraseCount);
  bool found = false;
  for (const auto& entry : entries)
    if (!strcmp(entry.path, "/bl/0001")) {
      assert(entry.kind == nrfmast::NoteVolumeEntry::File && entry.bytes == 64 && entry.block < 512);
      found = true;
    }
  assert(found);
  assert(nrfmast::notePartitionFree(nor, Journal::BASE, Journal::BYTES, &status) && nor.bytes == before);
  assert(status.checked && !status.mountResult && !status.traverseResult &&
         !status.overlapBlocks && !strcmp(status.label(), "clear"));
  nor.fault = nor.operations + 1;
  assert(!nrfmast::notePartitionFree(nor, Journal::BASE, Journal::BYTES, &status));
  assert(status.mountResult == LFS_ERR_IO && !strcmp(status.label(), "unreadable"));
  nor.power();
  nor.fault = 1;
  assert(!nrfmast::inspectNoteVolume(nor, collectVolume, &entries, volume));
  assert(volume.mountResult == LFS_ERR_IO && nor.bytes == before && nor.programs == programCount);
  nor.power();
  assert(lfs_mount(&filesystem, &config) == 0);
  const auto longName = "/" + std::string(96, 'n');
  assert(lfs_file_open(&filesystem, &file, longName.c_str(), LFS_O_WRONLY | LFS_O_CREAT) == 0);
  assert(lfs_file_close(&filesystem, &file) == 0);
  assert(lfs_unmount(&filesystem) == 0);
  const auto limited = nor.bytes;
  assert(!nrfmast::inspectNoteVolume(nor, collectVolume, &entries, volume));
  assert(volume.limited && volume.scanResult == LFS_ERR_INVAL && nor.bytes == limited);
  assert(lfs_mount(&filesystem, &config) == 0);
  assert(lfs_remove(&filesystem, longName.c_str()) == 0);
  assert(lfs_file_open(&filesystem, &file, "/line\nbreak", LFS_O_WRONLY | LFS_O_CREAT) == 0);
  assert(lfs_file_close(&filesystem, &file) == 0);
  assert(lfs_unmount(&filesystem) == 0);
  const auto invalid = nor.bytes;
  assert(!nrfmast::inspectNoteVolume(nor, collectVolume, &entries, volume));
  assert(!volume.limited && volume.scanResult == LFS_ERR_INVAL && nor.bytes == invalid);
  assert(lfs_mount(&filesystem, &config) == 0);
  assert(lfs_remove(&filesystem, "/line\nbreak") == 0);
  assert(lfs_file_open(&filesystem, &file, "/allocated", LFS_O_WRONLY | LFS_O_CREAT) == 0);
  for (unsigned i = 0; i < 7000; ++i)
    assert(lfs_file_write(&filesystem, &file, data, sizeof(data)) == sizeof(data));
  assert(lfs_file_close(&filesystem, &file) == 0);
  lfs_unmount(&filesystem);
  const auto occupied = nor.bytes;
  assert(!nrfmast::notePartitionFree(nor, Journal::BASE, Journal::BYTES, &status) && nor.bytes == occupied);
  assert(!status.mountResult && !status.traverseResult && status.overlapBlocks &&
         !strcmp(status.label(), "occupied"));
  entries.clear();
  const auto occupiedPrograms = nor.programs;
  const auto occupiedErases = std::accumulate(std::begin(nor.erases), std::end(nor.erases), 0U);
  assert(nrfmast::inspectNoteVolume(nor, collectVolume, &entries, volume));
  uint32_t overlaps = 0;
  for (const auto& entry : entries)
    if (entry.kind == nrfmast::NoteVolumeEntry::Allocation && entry.block >= Journal::BASE / 4096)
      ++overlaps;
  assert(overlaps == status.overlapBlocks && nor.bytes == occupied &&
         nor.programs == occupiedPrograms &&
         std::accumulate(std::begin(nor.erases), std::end(nor.erases), 0U) == occupiedErases);
  Clock clock;
  NativeFilesystem internalFS;
  const uint8_t bot[32] = {3}, sender[32] = {8};
  char reply[161];
  nrfmast::NoteStore legacyStore(internalFS, clock);
  assert(legacyStore.begin());
  assert(legacyStore.command(bot, sender, 102, "!remember b retained", reply, sizeof(reply)));
  const auto internalRecord = *internalFS.files.at("/pine-notes");
  nor.actualGuard = true;
  Journal actualJournal(nor);
  nrfmast::NoteStore actualStore(internalFS, clock, &actualJournal);
  assert(!actualStore.begin());
  assert(actualStore.provisionRetiredVolume() && actualStore.countPrincipals() == 1);
  assert(!memcmp(nor.bytes.data(), occupied.data(), Journal::BASE));
  assert(*internalFS.files.at("/pine-notes") == internalRecord);
  const unsigned guarded = nor.guardCalls;
  Journal actualReboot(nor);
  nrfmast::NoteStore actualRebootStore(internalFS, clock, &actualReboot);
  assert(actualRebootStore.begin() && nor.guardCalls == guarded);
  assert(actualRebootStore.command(bot, sender, 200, "!recall b", reply, sizeof(reply)) &&
         !strcmp(reply, "b=retained"));
  std::puts("QSPI actual occupied LittleFS: explicit note-region retirement/migration, outside bytes unchanged, no retired-volume traversal on reboot");
  std::puts("QSPI legacy volume: actual pinned LittleFS allocation traversal, no formatting or writes on mount/guard failure");
  std::puts("QSPI metadata inventory: bounded nested paths, file/directory block metadata, exact allocation map; no programs/erases/payload output");
}

struct BoundedPrint {
  std::string output;
  size_t calls = 0;
  template <typename... Args> void printf(const char* format, Args... args) {
    char buffer[256];
    const int bytes = std::snprintf(buffer, sizeof(buffer), format, args...);
    assert(bytes >= 0 && size_t(bytes) < sizeof(buffer));
    output.append(buffer, bytes);
    ++calls;
  }
};

void testStateDiagnostics() {
  Clock clock;
  NativeFilesystem fs;
  nrfmast::NoteStore notes(fs, clock);
  nrfmast::NotePartitionStatus status;
  status.checked = true;
  status.mountResult = status.traverseResult = std::numeric_limits<int>::min();
  status.overlapBlocks = UINT32_MAX;
  BoundedPrint print;
  nrfmast::printNoteState(print, notes, UINT32_MAX, UINT32_MAX, true, UINT32_MAX,
                         UINT32_MAX, UINT32_MAX, status);
  assert(print.calls == 3 && print.output.size() > 256);
  assert(std::count(print.output.begin(), print.output.end(), '\n') == 1 &&
         print.output.back() == '\n' && print.output.find('\0') == std::string::npos);
  assert(print.output.find("notes=unavailable") != std::string::npos &&
         print.output.find("notes_jedec=ffffffff") != std::string::npos &&
         print.output.find("notes_guard=unreadable") != std::string::npos &&
         print.output.find("notes_mount_result=-2147483648") != std::string::npos &&
         print.output.find("notes_overlap_blocks=4294967295") != std::string::npos);
  nrfmast::NoteVolumeEntry entry = {};
  memset(entry.path, 'n', sizeof(entry.path) - 1);
  entry.bytes = entry.block = entry.secondBlock = UINT32_MAX;
  for (auto kind : {nrfmast::NoteVolumeEntry::File, nrfmast::NoteVolumeEntry::Directory,
                    nrfmast::NoteVolumeEntry::Allocation}) {
    entry.kind = kind;
    nrfmast::printNoteVolumeEntry(print, entry);
  }
  nrfmast::NoteVolumeStatus volume;
  volume.mountResult = volume.traverseResult = volume.scanResult = std::numeric_limits<int>::min();
  volume.entries = volume.blocks = UINT32_MAX;
  volume.limited = true;
  nrfmast::printNoteVolumeStatus(print, volume);
  nrfmast::printNoteProvisionWarning(print);
  assert(print.output.find(nrfmast::NOTE_PROVISION_CONFIRM) != std::string::npos &&
         print.output.find("CURRENT InternalFS") != std::string::npos);
  status.retired = true;
  nrfmast::printNoteState(print, notes, 7, 28, true, 0, 1, 0x856015, status);
  assert(print.output.find("notes_guard=retired") != std::string::npos);
  std::puts("notes diagnostics: each actual formatter call fits pinned 256-byte Print buffer; one complete ASCII line");
}
}

void testNoteJournal() {
  testCutsAndWear();
  testWrappedPartialErase();
  testWrappedReplayFence();
  testMigration();
  testOwnerProvisioning();
  testActualLegacyVolume();
  testStateDiagnostics();
}
