#pragma once

#include <Arduino.h>
#include <helpers/IdentityStore.h>
#include <Dispatcher.h>
#include "NoteJournal.h"

namespace nrfmast {

class NoteStore {
public:
  static constexpr size_t TOTAL = 16, PRINCIPALS = 8, PER_PRINCIPAL = 4, KEY_BYTES = 16, VALUE_BYTES = 96;
  static constexpr uint8_t DEVICE_WRITE_BURST = 32, PRINCIPAL_WRITE_BURST = 8;
  static constexpr uint32_t WRITE_REFILL_MS = 15000;
  struct Entry {
    uint8_t bot[PUB_KEY_SIZE], principal[PUB_KEY_SIZE];
    char key[KEY_BYTES + 1], value[VALUE_BYTES + 1];
  };

private:
  FILESYSTEM& fs;
  mesh::MillisecondClock& clock;
  NoteJournal* journal;
  struct WriteBudget {
    uint32_t refilledAt;
    uint8_t remaining;
    bool assigned = false;
    uint8_t bot[PUB_KEY_SIZE] = {}, principal[PUB_KEY_SIZE] = {};
    WriteBudget(uint8_t limit = PRINCIPAL_WRITE_BURST) : refilledAt(0), remaining(limit) {}
  };
  WriteBudget deviceWrites{DEVICE_WRITE_BURST}, principalWrites[PRINCIPALS];
  Entry entries[TOTAL] = {};
  struct Principal {
    uint8_t bot[PUB_KEY_SIZE], key[PUB_KEY_SIZE];
    uint32_t lastWrite;
  };
  Principal principals[PRINCIPALS] = {};
  bool available = false;
  bool retiredVolume = false;
  const char* storageIssue = "storage unavailable; operator must restore /pine-notes";
  uint32_t markerVersion();
  bool saveMarker(uint32_t version = 1);
  bool validRecords() const;
  bool readSnapshot(bool load);
  void packSnapshot(size_t slot, const Entry& replacement, size_t principalSlot,
                    const Principal& principal, uint32_t hash);
  static void refill(WriteBudget& budget, uint8_t limit, uint32_t now);
  bool admitWrite(const uint8_t* bot, const uint8_t* principal, char* reply, size_t capacity);
  bool commit(size_t slot, const Entry& replacement, size_t principalSlot, const Principal& principal);
  bool readFile(const char* path, bool load, uint32_t* checksum = nullptr);
  static bool validKey(const char* key);
  static bool validEntry(const Entry& entry);

public:
  NoteStore(FILESYSTEM& storage, mesh::MillisecondClock& monotonicClock, NoteJournal* external = nullptr)
      : fs(storage), clock(monotonicClock), journal(external) {}
  bool begin();
  bool ready() const { return available; }
  const char* error() const { return storageIssue; }
  size_t countNotes() const {
    size_t count = 0;
    for (const auto& entry : entries) if (entry.key[0]) ++count;
    return count;
  }
  size_t countPrincipals() const {
    size_t count = 0;
    for (const auto& principal : principals) if (principal.lastWrite) ++count;
    return count;
  }
  bool visitEntries(bool (*visitor)(void*, const Entry&), void* context) const {
    if (!available || !visitor) return false;
    for (const auto& entry : entries)
      if (entry.key[0] && !visitor(context, entry)) return false;
    return true;
  }
  bool reset();
  bool provisionRetiredVolume();
  bool externalVolumeRetired() const { return retiredVolume; }
  static bool isCommand(const char* text);
  // Only the authenticated normal-DM callback supplies these full keys.
  bool command(const uint8_t* bot, const uint8_t* principal, uint32_t timestamp, const char* text,
               char* reply, size_t capacity);
};

}  // namespace nrfmast
