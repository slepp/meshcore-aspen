#include "NoteStore.h"
#include <cstdio>
#include <initializer_list>

namespace nrfmast {

static constexpr uint8_t MAGIC[8] = {'P', 'N', 'O', 'T', 'E', '0', '1', 0};
static constexpr const char* LIVE = "/pine-notes";
static constexpr const char* STAGE = "/pine-notes.new";
static constexpr const char* MARKER = "/pine-notes.qspi", *MARKER_STAGE = "/pine-notes.qspi.new";
static constexpr uint8_t MARKER_MAGIC[8] = {'P', 'N', 'Q', 'S', 'P', 'I', '1', 0};
enum MarkerVersion : uint32_t {
  Migrated = 1, Resetting = 2, Provisioning = 3, Retired = 4, RetiredResetting = 5
};
struct MigrationMarker {
  uint8_t magic[8];
  uint32_t base, bytes, payloadBytes, version, hash;
};
static_assert(sizeof(MigrationMarker) == 28, "migration marker layout changed");
static_assert(sizeof(NoteStore::Entry) == 178, "note record layout changed");

static uint32_t checksum(uint32_t hash, const void* data, size_t length) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  while (length--) hash = (hash ^ *bytes++) * 16777619U;
  return hash;
}

bool NoteStore::validKey(const char* key) {
  const size_t length = strlen(key);
  if (!length || length > KEY_BYTES) return false;
  for (size_t i = 0; i < length; ++i) {
    const char c = key[i];
    if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') &&
        !(c >= '0' && c <= '9') && c != '_' && c != '-' && c != '.') return false;
  }
  return true;
}

bool NoteStore::validEntry(const Entry& entry) {
  if (!entry.key[0]) {
    const Entry empty = {};
    return memcmp(&entry, &empty, sizeof(entry)) == 0;
  }
  if (!memchr(entry.key, 0, sizeof(entry.key)) || !memchr(entry.value, 0, sizeof(entry.value)) ||
      !validKey(entry.key) || !entry.value[0]) return false;
  for (const char* p = entry.value; *p; ++p)
    if (static_cast<uint8_t>(*p) < 32 || static_cast<uint8_t>(*p) == 127) return false;
  return true;
}

bool NoteStore::readFile(const char* path, bool load, uint32_t* digest) {
  auto file = fs.open(path);
  if (!file) return false;
  if (file.size() != sizeof(MAGIC) + 4 + sizeof(entries) + sizeof(principals)) {
    file.close();
    return false;
  }
  uint8_t magic[8];
  uint32_t expected = 0, hash = 2166136261U;
  bool ok = file.read(magic, sizeof(magic)) == sizeof(magic) &&
            !memcmp(magic, MAGIC, sizeof(MAGIC)) &&
            file.read(reinterpret_cast<uint8_t*>(&expected), sizeof(expected)) == sizeof(expected);
  for (size_t i = 0; ok && i < TOTAL; ++i) {
    Entry entry = {};
    ok = file.read(reinterpret_cast<uint8_t*>(&entry), sizeof(entry)) == sizeof(entry) &&
         validEntry(entry);
    hash = checksum(hash, &entry, sizeof(entry));
    if (load && ok) entries[i] = entry;
  }
  for (size_t i = 0; ok && i < PRINCIPALS; ++i) {
    Principal principal = {};
    ok = file.read(reinterpret_cast<uint8_t*>(&principal), sizeof(principal)) == sizeof(principal);
    hash = checksum(hash, &principal, sizeof(principal));
    if (load && ok) principals[i] = principal;
  }
  file.close();
  if (digest) *digest = hash;
  return ok && hash == expected;
}

bool NoteStore::begin() {
  memset(entries, 0, sizeof(entries));
  memset(principals, 0, sizeof(principals));
  storageIssue = journal ? "QSPI notes record invalid; USB operator recovery required" :
                          "storage unavailable; operator must restore /pine-notes";
  const bool migrated = journal && fs.exists(MARKER);
  const uint32_t version = migrated ? markerVersion() : 0;
  retiredVolume = version == Provisioning || version == Retired || version == RetiredResetting;
  const bool validMarker = !migrated || version == Migrated || version == Retired;
  available = !journal || (validMarker && journal->begin(retiredVolume));
  if (available) {
    if (journal && journal->hasSnapshot()) available = readSnapshot(true);
    else if (migrated) {
      storageIssue = "QSPI committed notes missing; old InternalFS replay state was not restored";
      available = false;
    } else available = !fs.exists(LIVE) || readFile(LIVE, true);
  } else if (validMarker) storageIssue = journal->error();
  if (available) available = validRecords();
  if (available && journal && !journal->hasSnapshot()) {
    Entry empty = {};
    Principal principal = {};
    uint32_t hash = checksum(2166136261U, entries, sizeof(entries));
    hash = checksum(hash, principals, sizeof(principals));
    packSnapshot(TOTAL, empty, PRINCIPALS, principal, hash);
    available = journal->commit() == NoteJournal::Result::Committed;
    if (!available) storageIssue = journal->error();
  }
  if (available && journal && !migrated) {
    available = saveMarker();
    if (!available) storageIssue = "QSPI migration marker commit failed; notes disabled; InternalFS snapshot retained";
  }
  if (!available) {
    memset(entries, 0, sizeof(entries));
    memset(principals, 0, sizeof(principals));
  }
  return available;
}

bool NoteStore::validRecords() const {
  for (size_t i = 0; i < PRINCIPALS; ++i) {
    if (!principals[i].lastWrite) {
      const Principal empty = {};
      if (memcmp(&principals[i], &empty, sizeof(empty))) return false;
    } else {
      for (size_t j = i + 1; j < PRINCIPALS; ++j)
        if (principals[j].lastWrite && !memcmp(principals[i].bot, principals[j].bot, PUB_KEY_SIZE) &&
            !memcmp(principals[i].key, principals[j].key, PUB_KEY_SIZE)) return false;
    }
  }
  for (size_t i = 0; i < TOTAL; ++i) {
    if (!entries[i].key[0]) continue;
    size_t count = 0;
    for (size_t j = 0; j < TOTAL; ++j) {
      if (!entries[j].key[0] || memcmp(entries[i].bot, entries[j].bot, PUB_KEY_SIZE) ||
          memcmp(entries[i].principal, entries[j].principal, PUB_KEY_SIZE)) continue;
      ++count;
      if (i != j && !strcmp(entries[i].key, entries[j].key)) return false;
    }
    if (count > PER_PRINCIPAL) return false;
    bool hasPrincipal = false;
    for (const auto& principal : principals)
      if (principal.lastWrite && !memcmp(principal.bot, entries[i].bot, PUB_KEY_SIZE) &&
          !memcmp(principal.key, entries[i].principal, PUB_KEY_SIZE)) hasPrincipal = true;
    if (!hasPrincipal) return false;
  }
  return true;
}

bool NoteStore::provisionRetiredVolume() {
  if (!journal) { storageIssue = "external note journal unavailable; provisioning refused"; return false; }
  const bool existing = fs.exists(MARKER);
  const uint32_t version = existing ? markerVersion() : 0;
  if (existing && version != Provisioning) {
    storageIssue = version == Retired || version == RetiredResetting ?
        "external volume already provisioned; use separate bot notes reset confirm to erase notes" :
        "migration/reset marker exists or is corrupt; external provisioning refused";
    return false;
  }
  available = false;
  if (!fs.exists(LIVE) || !readFile(LIVE, true) || !validRecords()) {
    storageIssue = "checked InternalFS /pine-notes and replay records required; note region not erased";
    return false;
  }
  if (!saveMarker(Provisioning)) {
    storageIssue = "owner provisioning intent not verified; note region not erased; reboot and inspect state";
    return false;
  }
  retiredVolume = true;
  storageIssue = "external provisioning incomplete; repeat USB bot notes provision retire-external migrate-internal confirm";
  if (!journal->clear(true)) { storageIssue = journal->error(); return false; }
  Entry empty = {};
  Principal principal = {};
  uint32_t hash = checksum(2166136261U, entries, sizeof(entries));
  hash = checksum(hash, principals, sizeof(principals));
  packSnapshot(TOTAL, empty, PRINCIPALS, principal, hash);
  if (journal->commit() != NoteJournal::Result::Committed) {
    storageIssue = journal->error();
    return false;
  }
  if (!readSnapshot(false)) {
    storageIssue = "imported QSPI notes/replay snapshot invalid; provisioning incomplete";
    return false;
  }
  if (!saveMarker(Retired)) {
    storageIssue = "provision completion uncertain; reboot and inspect bot state before repeating provisioning";
    return false;
  }
  return begin();
}

bool NoteStore::reset() {
  if (journal) {
    const bool existing = fs.exists(MARKER);
    const uint32_t version = existing ? markerVersion() : 0;
    if (version == Provisioning) {
      storageIssue = "provisioning incomplete; repeat owner provisioning before using notes reset";
      return false;
    }
    retiredVolume = version == Retired || version == RetiredResetting;
    available = false;
    storageIssue = "QSPI notes reset incomplete; USB operator recovery required";
    if (!journal->checkRegion(retiredVolume)) { storageIssue = journal->error(); return false; }
    // Fence all boots before the destructive clear; interrupted reset cannot revive older timestamps.
    if (!saveMarker(retiredVolume ? RetiredResetting : Resetting)) return false;
    if (!journal->clear(retiredVolume)) return false;
    memset(entries, 0, sizeof(entries));
    memset(principals, 0, sizeof(principals));
    Entry empty = {};
    Principal principal = {};
    uint32_t hash = checksum(2166136261U, entries, sizeof(entries));
    hash = checksum(hash, principals, sizeof(principals));
    packSnapshot(TOTAL, empty, PRINCIPALS, principal, hash);
    if (journal->commit() != NoteJournal::Result::Committed ||
        !saveMarker(retiredVolume ? Retired : Migrated)) {
      available = false;
      return false;
    }
    if (fs.exists(LIVE) && !fs.remove(LIVE)) return false;
    if (fs.exists(STAGE) && !fs.remove(STAGE)) return false;
    return begin();
  }
  if (fs.exists(LIVE) && !fs.remove(LIVE)) return false;
  fs.remove(STAGE);
  return begin();
}

uint32_t NoteStore::markerVersion() {
  auto file = fs.open(MARKER);
  if (!file) {
    storageIssue = "QSPI migration marker cannot be read; USB operator recovery required";
    return 0;
  }
  MigrationMarker marker = {};
  const bool read = file.size() == sizeof(marker) &&
      file.read(reinterpret_cast<uint8_t*>(&marker), sizeof(marker)) == sizeof(marker);
  file.close();
  const bool checked = read && !memcmp(marker.magic, MARKER_MAGIC, sizeof(MARKER_MAGIC)) &&
      marker.base == NoteJournal::BASE && marker.bytes == NoteJournal::BYTES &&
      marker.payloadBytes == NoteJournal::PAYLOAD_BYTES &&
      marker.hash == checksum(2166136261U, &marker, sizeof(marker) - sizeof(marker.hash));
  if (!checked || marker.version < Migrated || marker.version > RetiredResetting) {
    storageIssue = "QSPI migration marker corrupt; USB operator recovery required";
    return 0;
  }
  if (marker.version == Resetting || marker.version == RetiredResetting)
    storageIssue = "QSPI notes reset interrupted; repeat USB bot notes reset confirm (erases notes)";
  else if (marker.version == Provisioning)
    storageIssue = "external provisioning interrupted; repeat USB bot notes provision retire-external migrate-internal confirm";
  return marker.version;
}

bool NoteStore::saveMarker(uint32_t version) {
  MigrationMarker marker = {};
  memcpy(marker.magic, MARKER_MAGIC, sizeof(MARKER_MAGIC));
  marker.base = NoteJournal::BASE;
  marker.bytes = NoteJournal::BYTES;
  marker.payloadBytes = NoteJournal::PAYLOAD_BYTES;
  marker.version = version;
  marker.hash = checksum(2166136261U, &marker, sizeof(marker) - sizeof(marker.hash));
  fs.remove(MARKER_STAGE);
#if defined(NRF52_PLATFORM)
  auto file = fs.open(MARKER_STAGE, FILE_O_WRITE);
#else
  auto file = fs.open(MARKER_STAGE, "w");
#endif
  if (!file) return false;
  bool ok = file.write(reinterpret_cast<const uint8_t*>(&marker), sizeof(marker)) == sizeof(marker);
  file.close();
  file = fs.open(MARKER_STAGE);
  MigrationMarker readback = {};
  ok = ok && file && file.size() == sizeof(marker) &&
      file.read(reinterpret_cast<uint8_t*>(&readback), sizeof(readback)) == sizeof(readback) &&
      !memcmp(&marker, &readback, sizeof(marker));
  file.close();
  ok = ok && fs.rename(MARKER_STAGE, MARKER);
  if (ok) {
    file = fs.open(MARKER);
    ok = file && file.size() == sizeof(marker) &&
        file.read(reinterpret_cast<uint8_t*>(&readback), sizeof(readback)) == sizeof(readback) &&
        !memcmp(&marker, &readback, sizeof(marker));
    file.close();
  }
  if (!ok) fs.remove(MARKER_STAGE);
  return ok;
}

void NoteStore::packSnapshot(size_t slot, const Entry& replacement,
                             size_t principalSlot, const Principal& principal, uint32_t hash) {
  static_assert(sizeof(entries) + sizeof(principals) + 12 == NoteJournal::PAYLOAD_BYTES,
                "journal payload layout changed");
  auto* output = journal->data();
  memcpy(output, MAGIC, sizeof(MAGIC));
  memcpy(output + sizeof(MAGIC), &hash, sizeof(hash));
  output += sizeof(MAGIC) + sizeof(hash);
  for (size_t i = 0; i < TOTAL; ++i, output += sizeof(Entry))
    memcpy(output, i == slot ? &replacement : &entries[i], sizeof(Entry));
  for (size_t i = 0; i < PRINCIPALS; ++i, output += sizeof(Principal))
    memcpy(output, i == principalSlot ? &principal : &principals[i], sizeof(Principal));
}

bool NoteStore::readSnapshot(bool load) {
  const auto* input = journal->data();
  uint32_t expected = 0, hash = 2166136261U;
  memcpy(&expected, input + sizeof(MAGIC), sizeof(expected));
  if (memcmp(input, MAGIC, sizeof(MAGIC))) return false;
  input += sizeof(MAGIC) + sizeof(expected);
  for (size_t i = 0; i < TOTAL; ++i, input += sizeof(Entry)) {
    Entry entry = {};
    memcpy(&entry, input, sizeof(entry));
    if (!validEntry(entry)) return false;
    hash = checksum(hash, &entry, sizeof(entry));
    if (load) entries[i] = entry;
  }
  for (size_t i = 0; i < PRINCIPALS; ++i, input += sizeof(Principal)) {
    Principal principal = {};
    memcpy(&principal, input, sizeof(principal));
    hash = checksum(hash, &principal, sizeof(principal));
    if (load) principals[i] = principal;
  }
  return hash == expected;
}

void NoteStore::refill(WriteBudget& budget, uint8_t limit, uint32_t now) {
  if (budget.remaining == limit) {
    budget.refilledAt = now;
    return;
  }
  const uint32_t credits = uint32_t(now - budget.refilledAt) / WRITE_REFILL_MS;
  if (credits) {
    if (credits >= uint32_t(limit - budget.remaining)) {
      budget.remaining = limit;
      budget.refilledAt = now;
    } else {
      budget.remaining += credits;
      budget.refilledAt += credits * WRITE_REFILL_MS;
    }
  }
}

bool NoteStore::admitWrite(const uint8_t* bot, const uint8_t* key, char* reply, size_t capacity) {
  const uint32_t now = clock.getMillis();
  WriteBudget* selected = nullptr;
  WriteBudget* free = nullptr;
  refill(deviceWrites, DEVICE_WRITE_BURST, now);
  for (auto& budget : principalWrites) {
    refill(budget, PRINCIPAL_WRITE_BURST, now);
    if (budget.assigned && !memcmp(budget.bot, bot, PUB_KEY_SIZE) &&
        !memcmp(budget.principal, key, PUB_KEY_SIZE)) selected = &budget;
    if (!free && (!budget.assigned || budget.remaining == PRINCIPAL_WRITE_BURST)) free = &budget;
  }
  if (!selected) selected = free;
  const bool deviceLimited = !deviceWrites.remaining;
  if (deviceLimited || (selected && !selected->remaining)) {
    const auto& budget = deviceLimited ? deviceWrites : *selected;
    const uint32_t remaining = WRITE_REFILL_MS - uint32_t(now - budget.refilledAt);
    snprintf(reply, capacity, "Error: notes %s write budget exhausted; retry in %lus; timestamp not committed",
             deviceLimited ? "device" : "principal", static_cast<unsigned long>((remaining + 999) / 1000));
    return false;
  }
  if (!selected) {
    snprintf(reply, capacity, "Error: notes write principal slots busy; retry after refill; timestamp not committed");
    return false;
  }
  selected->assigned = true;
  memcpy(selected->bot, bot, PUB_KEY_SIZE);
  memcpy(selected->principal, key, PUB_KEY_SIZE);
  // Failed commits can still erase a sector; charge before touching storage.
  --deviceWrites.remaining;
  --selected->remaining;
  return true;
}

bool NoteStore::commit(size_t slot, const Entry& replacement,
                       size_t principalSlot, const Principal& principal) {
  uint32_t hash = 2166136261U;
  for (size_t i = 0; i < TOTAL; ++i)
    hash = checksum(hash, i == slot ? &replacement : &entries[i], sizeof(Entry));
  for (size_t i = 0; i < PRINCIPALS; ++i)
    hash = checksum(hash, i == principalSlot ? &principal : &principals[i], sizeof(Principal));
  if (journal) {
    packSnapshot(slot, replacement, principalSlot, principal, hash);
    const auto result = journal->commit();
    if (result == NoteJournal::Result::Uncertain) {
      available = false;
      storageIssue = journal->error();
    }
    if (result != NoteJournal::Result::Committed) return false;
    if (slot < TOTAL) entries[slot] = replacement;
    principals[principalSlot] = principal;
    return true;
  }
  fs.remove(STAGE);
#if defined(NRF52_PLATFORM)
  auto file = fs.open(STAGE, FILE_O_WRITE);
#else
  auto file = fs.open(STAGE, "w");
#endif
  if (!file) return false;
  bool ok = file.write(MAGIC, sizeof(MAGIC)) == sizeof(MAGIC) &&
            file.write(reinterpret_cast<const uint8_t*>(&hash), sizeof(hash)) == sizeof(hash);
  for (size_t i = 0; ok && i < TOTAL; ++i) {
    const auto& entry = i == slot ? replacement : entries[i];
    ok = file.write(reinterpret_cast<const uint8_t*>(&entry), sizeof(entry)) == sizeof(entry);
  }
  for (size_t i = 0; ok && i < PRINCIPALS; ++i) {
    const auto& record = i == principalSlot ? principal : principals[i];
    ok = file.write(reinterpret_cast<const uint8_t*>(&record), sizeof(record)) == sizeof(record);
  }
  file.close();
  uint32_t savedHash = 0;
  ok = ok && readFile(STAGE, false, &savedHash) && hash == savedHash && fs.rename(STAGE, LIVE);
  if (!ok) fs.remove(STAGE);
  if (ok) {
    if (slot < TOTAL) entries[slot] = replacement;
    principals[principalSlot] = principal;
  }
  return ok;
}

bool NoteStore::isCommand(const char* text) {
  for (const char* command : {"!remember", "!recall", "!forget", "!list"}) {
    const size_t length = strlen(command);
    if (!strncmp(text, command, length) && (text[length] == 0 || text[length] == ' ')) return true;
  }
  return false;
}

bool NoteStore::command(const uint8_t* bot, const uint8_t* principal, uint32_t timestamp, const char* text,
                        char* reply, size_t capacity) {
  if (!isCommand(text)) return false;
  if (!capacity) return true;
  auto error = [&](const char* message) { snprintf(reply, capacity, "Error: notes %s", message); };
  if (!available) { error(storageIssue); return true; }
  const char* args = strchr(text, ' ');
  const size_t verbLength = args ? size_t(args - text) : strlen(text);
  const bool list = verbLength == 5 && !strncmp(text, "!list", 5);
  const bool remember = verbLength == 9 && !strncmp(text, "!remember", 9);
  const bool recall = verbLength == 7 && !strncmp(text, "!recall", 7);
  char key[KEY_BYTES + 1] = {};
  const char* value = nullptr;
  if (list) {
    if (args) { error("usage: !list"); return true; }
  } else {
    if (!args || !args[1]) { error("usage: !remember key value | !recall key | !forget key"); return true; }
    ++args;
    const char* space = strchr(args, ' ');
    const size_t length = space ? size_t(space - args) : strlen(args);
    if (!length || length > KEY_BYTES) { error("key requires 1-16 ASCII letters/digits/_.-"); return true; }
    memcpy(key, args, length);
    if (!validKey(key)) { error("key requires 1-16 ASCII letters/digits/_.-"); return true; }
    if (remember) {
      if (!space || !space[1]) { error("remember requires a value (1-96 bytes)"); return true; }
      value = space + 1;
      if (strlen(value) > VALUE_BYTES) { error("value exceeds 96 bytes"); return true; }
      for (const char* p = value; *p; ++p) {
        if (static_cast<uint8_t>(*p) < 32 || static_cast<uint8_t>(*p) == 127) {
          error("value contains a control byte"); return true;
        }
      }
    } else if (space) { error("recall/forget accept one key"); return true; }
  }
  size_t found = TOTAL, free = TOTAL, count = 0;
  snprintf(reply, capacity, "notes 0/4:");
  for (size_t i = 0; i < TOTAL; ++i) {
    if (!entries[i].key[0]) { if (free == TOTAL) free = i; continue; }
    if (memcmp(entries[i].bot, bot, PUB_KEY_SIZE) || memcmp(entries[i].principal, principal, PUB_KEY_SIZE))
      continue;
    ++count;
    if (!strcmp(entries[i].key, key)) found = i;
    if (list) {
      const size_t used = strlen(reply);
      snprintf(reply + used, capacity > used ? capacity - used : 0, " %s", entries[i].key);
    }
  }
  if (list) {
    if (capacity > 7) reply[6] = '0' + count;
    return true;
  }
  if (recall) {
    if (found == TOTAL) error("key not found");
    else snprintf(reply, capacity, "%s=%s", key, entries[found].value);
    return true;
  }
  size_t principalSlot = PRINCIPALS, principalFree = PRINCIPALS;
  for (size_t i = 0; i < PRINCIPALS; ++i) {
    if (!principals[i].lastWrite && principalFree == PRINCIPALS) principalFree = i;
    if (principals[i].lastWrite && !memcmp(principals[i].bot, bot, PUB_KEY_SIZE) &&
        !memcmp(principals[i].key, principal, PUB_KEY_SIZE)) principalSlot = i;
  }
  if (!timestamp || (principalSlot < PRINCIPALS && timestamp <= principals[principalSlot].lastWrite)) {
    error("stale write timestamp; synchronize sender clock and send a fresh DM"); return true;
  }
  if ((!remember && found == TOTAL) ||
      (remember && found < TOTAL && !strcmp(entries[found].value, value))) {
    error("no change; write timestamp not committed"); return true;
  }
  if (principalSlot == PRINCIPALS) principalSlot = principalFree;
  if (principalSlot == PRINCIPALS) {
    error("device quota 8 full-key principals; USB operator reset required"); return true;
  }
  Principal watermark = {};
  memcpy(watermark.bot, bot, PUB_KEY_SIZE);
  memcpy(watermark.key, principal, PUB_KEY_SIZE);
  watermark.lastWrite = timestamp;
  if (remember && found == TOTAL && count == PER_PRINCIPAL) {
    error("principal quota 4 notes; !forget a key first"); return true;
  }
  if (remember && found == TOTAL && free == TOTAL) {
    error("device quota 16 notes; operator/user cleanup required"); return true;
  }
  Entry replacement = {};
  if (remember) {
    memcpy(replacement.bot, bot, PUB_KEY_SIZE);
    memcpy(replacement.principal, principal, PUB_KEY_SIZE);
    strcpy(replacement.key, key);
    strcpy(replacement.value, value);
  }
  const size_t slot = found == TOTAL ? (remember ? free : TOTAL) : found;
  if (!admitWrite(bot, principal, reply, capacity)) return true;
  if (!commit(slot, replacement, principalSlot, watermark)) {
    error(available ? "commit failed; previous notes retained" :
          "commit outcome uncertain; reboot and recall/list; do not replay the write");
  }
  else snprintf(reply, capacity, "%s %s", remember ? "remembered" : "forgot", key);
  return true;
}

}  // namespace nrfmast
