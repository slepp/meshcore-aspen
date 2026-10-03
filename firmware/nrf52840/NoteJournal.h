#pragma once
#include <cstddef>
#include <cstdint>

namespace nrfmast {

class NoteFlash {
public:
  virtual ~NoteFlash() = default;
  virtual bool begin() = 0;
  virtual bool partitionFree(uint32_t address, uint32_t bytes) = 0;
  virtual bool retiredPartition(uint32_t, uint32_t) { return false; }
  virtual bool read(uint32_t address, void* output, size_t bytes) = 0;
  virtual bool program(uint32_t address, const void* input, size_t bytes) = 0;
  virtual bool erase(uint32_t address) = 0;
};

class NoteJournal {
public:
  static constexpr uint32_t BASE = 0x180000, BYTES = 0x80000, SECTOR = 4096;
  static constexpr size_t SLOTS = BYTES / SECTOR, PAYLOAD_BYTES = 3404;
  enum class Result { Committed, Failed, Uncertain };

private:
  NoteFlash& flash;
  uint64_t sequence = 0;
  size_t current = SLOTS, used = 0;
  bool mounted = false;
  const char* failure = "QSPI note journal not initialized";
  alignas(4) uint8_t payload[PAYLOAD_BYTES] = {};
  bool verify(uint32_t address, const void* expected, size_t bytes);

public:
  explicit NoteJournal(NoteFlash& storage) : flash(storage) {}
  // Only NoteStore's checked owner marker authorizes retiring the legacy volume.
  bool checkRegion(bool retiredVolume = false);
  bool begin(bool retiredVolume = false);
  bool clear(bool retiredVolume = false);
  Result commit();
  bool ready() const { return mounted; }
  bool hasSnapshot() const { return current != SLOTS; }
  uint8_t* data() { return payload; }
  const uint8_t* data() const { return payload; }
  uint64_t generation() const { return sequence; }
  size_t usedSlots() const { return used; }
  const char* error() const { return failure; }
};

}  // namespace nrfmast
