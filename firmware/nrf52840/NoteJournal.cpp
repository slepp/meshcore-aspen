#include "NoteJournal.h"
#include <cstring>
#include <limits>

namespace nrfmast {
namespace {
constexpr uint8_t MAGIC[8] = {'P', 'N', 'J', 'R', 'N', 'L', '2', 0};
constexpr uint8_t LINK_MAGIC[8] = {'P', 'N', 'P', 'U', 'B', 'L', '2', 0};
constexpr uint32_t COMMITTED = 0x54494d43;
struct Header {
  uint8_t magic[8];
  uint64_t sequence;
  uint32_t payloadHash, bytes, slot, hash;
};
struct Footer {
  uint32_t sequenceLow, sequenceHigh, payloadHash, headerHash, bytes, marker, markerInverse;
};
struct Publication {
  uint8_t magic[8];
  uint32_t sequenceLow, sequenceHigh, slot, hash, marker, markerInverse;
};
static_assert(sizeof(Header) == 32 && sizeof(Footer) == 28, "journal layout changed");
static_assert(sizeof(Publication) == 32, "publication layout changed");
constexpr uint32_t PUBLICATION_OFFSET = NoteJournal::SECTOR - sizeof(Footer) - sizeof(Publication);
uint32_t hash(const void* data, size_t bytes) {
  uint32_t value = 2166136261U;
  const auto* input = static_cast<const uint8_t*>(data);
  while (bytes--) value = (value ^ *input++) * 16777619U;
  return value;
}
bool snapshotValid(const Header& header, const Footer& footer, size_t slot) {
  return !memcmp(header.magic, MAGIC, sizeof(MAGIC)) && header.sequence &&
      header.bytes == NoteJournal::PAYLOAD_BYTES && header.slot == slot &&
      header.slot == (header.sequence - 1) % NoteJournal::SLOTS &&
      header.hash == hash(&header, sizeof(header) - sizeof(header.hash)) &&
      (uint64_t(footer.sequenceHigh) << 32 | footer.sequenceLow) == header.sequence &&
      footer.payloadHash == header.payloadHash && footer.headerHash == header.hash &&
      footer.bytes == NoteJournal::PAYLOAD_BYTES &&
      footer.marker == COMMITTED && footer.markerInverse == ~COMMITTED;
}
}

bool NoteJournal::checkRegion(bool retiredVolume) {
  if (!flash.begin()) { failure = "QSPI flash detection failed; notes disabled"; return false; }
  if (!(retiredVolume ? flash.retiredPartition(BASE, BYTES) : flash.partitionFree(BASE, BYTES))) {
    failure = retiredVolume ? "QSPI owner-authorized note region unavailable; notes disabled" :
                             "QSPI note region overlaps an allocated or unreadable legacy volume; not erased";
    return false;
  }
  return true;
}

bool NoteJournal::begin(bool retiredVolume) {
  mounted = false;
  sequence = used = 0;
  current = SLOTS;
  if (!checkRegion(retiredVolume)) return false;
  uint8_t invalid[SLOTS / 8] = {};
  uint64_t newestPrepared = 0;
  auto reject = [&](size_t slot) { invalid[slot / 8] |= 1U << (slot % 8); };
  for (size_t slot = 0; slot < SLOTS; ++slot) {
    Header header = {};
    Footer footer = {};
    Publication publication = {};
    const uint32_t address = BASE + slot * SECTOR;
    if (!flash.read(address, &header, sizeof(header)) ||
        !flash.read(address + SECTOR - sizeof(footer), &footer, sizeof(footer)) ||
        !flash.read(address + PUBLICATION_OFFSET, &publication, sizeof(publication))) {
      failure = "QSPI journal metadata read failed; notes disabled";
      return false;
    }
    if (snapshotValid(header, footer, slot)) {
      ++used;
      if (header.sequence > newestPrepared) newestPrepared = header.sequence;
      if (header.sequence == 1 && slot == 0 && !sequence) {
        current = slot;
        sequence = 1;
      }
    } else if ((footer.marker != 0xffffffff || footer.markerInverse != 0xffffffff) &&
               (!memcmp(header.magic, MAGIC, sizeof(MAGIC)) ||
                footer.marker == COMMITTED || footer.markerInverse == ~COMMITTED)) reject(slot);

    if (publication.marker == 0xffffffff && publication.markerInverse == 0xffffffff) {
      const auto* bytes = reinterpret_cast<const uint8_t*>(&publication);
      for (size_t i = 0; i < sizeof(publication); ++i)
        if (bytes[i] != 0xff) { reject(slot); break; }
      continue;
    }
    const uint64_t published = uint64_t(publication.sequenceHigh) << 32 | publication.sequenceLow;
    if (memcmp(publication.magic, LINK_MAGIC, sizeof(LINK_MAGIC)) ||
        publication.marker != COMMITTED || publication.markerInverse != ~COMMITTED ||
        publication.hash != hash(&publication, sizeof(publication) - 12) ||
        published < 2 || publication.slot != (slot + 1) % SLOTS ||
        publication.slot != (published - 1) % SLOTS) {
      if (!memcmp(publication.magic, LINK_MAGIC, sizeof(LINK_MAGIC)) ||
          publication.marker == COMMITTED || publication.markerInverse == ~COMMITTED) reject(slot);
    } else if (published > sequence) {
      sequence = published;
      current = publication.slot;
    }
  }
  if (newestPrepared > sequence) {
    failure = "QSPI newer snapshot lacks a valid publication; USB operator recovery required";
    return false;
  }
  for (size_t slot = 0; slot < SLOTS; ++slot) {
    if ((invalid[slot / 8] & (1U << (slot % 8))) &&
        (!hasSnapshot() || slot != (current + 1) % SLOTS)) {
      failure = "QSPI committed metadata or publication corrupt; USB operator recovery required";
      return false;
    }
  }
  if (hasSnapshot()) {
    Header header = {};
    Footer footer = {};
    const uint32_t address = BASE + current * SECTOR;
    // The predecessor's publication anchors this generation even if its target is damaged.
    if (!flash.read(address, &header, sizeof(header)) ||
        !flash.read(address + SECTOR - sizeof(footer), &footer, sizeof(footer)) ||
        !snapshotValid(header, footer, current) || header.sequence != sequence ||
        !flash.read(address + sizeof(Header), payload, sizeof(payload)) ||
        hash(payload, sizeof(payload)) != header.payloadHash) {
      failure = "QSPI latest committed snapshot corrupt; no older replay state adopted";
      return false;
    }
  }
  mounted = true;
  failure = "none";
  return true;
}

bool NoteJournal::verify(uint32_t address, const void* expected, size_t bytes) {
  alignas(4) uint8_t readback[256];
  const auto* input = static_cast<const uint8_t*>(expected);
  while (bytes) {
    const size_t count = bytes < sizeof(readback) ? bytes : sizeof(readback);
    if (!flash.read(address, readback, count) || memcmp(readback, input, count)) return false;
    address += count;
    input += count;
    bytes -= count;
  }
  return true;
}

NoteJournal::Result NoteJournal::commit() {
  if (!mounted || sequence == std::numeric_limits<uint64_t>::max()) return Result::Failed;
  const size_t slot = hasSnapshot() ? (current + 1) % SLOTS : 0;
  const uint32_t address = BASE + slot * SECTOR;
  Header header = {};
  memcpy(header.magic, MAGIC, sizeof(MAGIC));
  header.sequence = sequence + 1;
  header.payloadHash = hash(payload, sizeof(payload));
  header.bytes = PAYLOAD_BYTES;
  header.slot = slot;
  header.hash = hash(&header, sizeof(header) - sizeof(header.hash));
  Footer footer = {};
  footer.sequenceLow = header.sequence;
  footer.sequenceHigh = header.sequence >> 32;
  footer.payloadHash = header.payloadHash;
  footer.headerHash = header.hash;
  footer.bytes = PAYLOAD_BYTES;
  footer.marker = footer.markerInverse = 0xffffffff;
  const uint32_t footerAddress = address + SECTOR - sizeof(footer);
  uint32_t erasedMarker[2] = {};
  Publication erasedPublication;
  memset(&erasedPublication, 0xff, sizeof(erasedPublication));
  if (!flash.erase(address) ||
      !flash.read(address + SECTOR - 8, erasedMarker, sizeof(erasedMarker)) ||
      erasedMarker[0] != 0xffffffff || erasedMarker[1] != 0xffffffff ||
      !verify(address + PUBLICATION_OFFSET, &erasedPublication, sizeof(erasedPublication)) ||
      !flash.program(address, &header, sizeof(header)) ||
      !flash.program(address + sizeof(header), payload, sizeof(payload)) ||
      !flash.program(footerAddress, &footer, sizeof(footer) - 8) ||
      !verify(address, &header, sizeof(header)) ||
      !verify(address + sizeof(header), payload, sizeof(payload)) ||
      !verify(footerAddress, &footer, sizeof(footer))) {
    failure = "QSPI staged snapshot failed; previous committed snapshot retained";
    return Result::Failed;
  }
  // This footer finishes preparation. Except for bootstrap, the predecessor publishes it.
  footer.marker = COMMITTED;
  footer.markerInverse = ~COMMITTED;
  if (!flash.program(address + SECTOR - 8, &footer.marker, 8) ||
      !verify(footerAddress, &footer, sizeof(footer))) {
    mounted = false;
    failure = "QSPI commit outcome uncertain; reboot and recall/list; do not replay the write";
    return Result::Uncertain;
  }
  if (hasSnapshot()) {
    Publication publication = {};
    memcpy(publication.magic, LINK_MAGIC, sizeof(LINK_MAGIC));
    publication.sequenceLow = header.sequence;
    publication.sequenceHigh = header.sequence >> 32;
    publication.slot = slot;
    publication.hash = hash(&publication, sizeof(publication) - 12);
    publication.marker = COMMITTED;
    publication.markerInverse = ~COMMITTED;
    const uint32_t linkAddress = BASE + current * SECTOR + PUBLICATION_OFFSET;
    if (!flash.program(linkAddress, &publication, sizeof(publication)) ||
        !verify(linkAddress, &publication, sizeof(publication))) {
      mounted = false;
      failure = "QSPI commit outcome uncertain; reboot and recall/list; do not replay the write";
      return Result::Uncertain;
    }
  }
  current = slot;
  sequence = header.sequence;
  if (used < SLOTS) ++used;
  failure = "none";
  return Result::Committed;
}

bool NoteJournal::clear(bool retiredVolume) {
  mounted = false;
  if (!checkRegion(retiredVolume)) return false;
  alignas(4) uint8_t readback[256];
  for (size_t slot = 0; slot < SLOTS; ++slot) {
    if (!flash.erase(BASE + slot * SECTOR)) {
      failure = "QSPI note-region erase incomplete; notes disabled";
      return false;
    }
    for (uint32_t offset = 0; offset < SECTOR; offset += sizeof(readback)) {
      if (!flash.read(BASE + slot * SECTOR + offset, readback, sizeof(readback))) {
        failure = "QSPI erased note-region readback failed; notes disabled";
        return false;
      }
      for (uint8_t byte : readback)
        if (byte != 0xff) {
          failure = "QSPI note-region erase verification failed; notes disabled";
          return false;
        }
    }
  }
  sequence = used = 0;
  current = SLOTS;
  mounted = true;
  return true;
}

}  // namespace nrfmast
