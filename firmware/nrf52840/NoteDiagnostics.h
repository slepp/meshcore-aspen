#pragma once
#include "NotePartition.h"
#include "NoteStore.h"

namespace nrfmast {
static constexpr const char* NOTE_PROVISION_CONFIRM =
    "bot notes provision retire-external migrate-internal confirm";
template <typename Output>
void printNoteProvisionWarning(Output& output) {
  output.printf("WARNING: retires the old external filesystem; erases only QSPI 0x180000..0x200000. Retired external files may be lost.\n");
  output.printf("Preserves CURRENT InternalFS role keys/preferences/BLE; imports checked InternalFS notes and principal replay timestamps.\n");
  output.printf("USB owner confirmation: %s\n", NOTE_PROVISION_CONFIRM);
}
template <typename Output>
void printNoteVolumeEntry(Output& output, const NoteVolumeEntry& entry) {
  if (entry.kind == NoteVolumeEntry::Allocation)
    output.printf("notes volume block=%lu address=%06lx reserved=%u\n",
                  static_cast<unsigned long>(entry.block),
                  static_cast<unsigned long>(entry.block * NoteJournal::SECTOR),
                  unsigned(entry.block >= NoteJournal::BASE / NoteJournal::SECTOR));
  else if (entry.kind == NoteVolumeEntry::Directory)
    output.printf("notes volume dir=%s metadata_blocks=%lu,%lu\n", entry.path,
                  static_cast<unsigned long>(entry.block), static_cast<unsigned long>(entry.secondBlock));
  else
    output.printf("notes volume file=%s bytes=%lu head_block=%lu\n", entry.path,
                  static_cast<unsigned long>(entry.bytes), static_cast<unsigned long>(entry.block));
}
template <typename Output>
void printNoteVolumeStatus(Output& output, const NoteVolumeStatus& status) {
  output.printf("notes volume readonly=1 mount_result=%d traverse_result=%d scan_result=%d entries=%lu blocks=%lu limited=%u\n",
                status.mountResult, status.traverseResult, status.scanResult,
                static_cast<unsigned long>(status.entries), static_cast<unsigned long>(status.blocks),
                unsigned(status.limited));
}
template <typename Output>
void printNoteState(Output& output, const NoteStore& notes, uint32_t storageUsed,
                    uint32_t storageTotal, bool bleActive, uint32_t inboxDrops,
                    size_t slots, uint32_t jedec, const NotePartitionStatus& partition) {
  // The pinned Print::printf has a 256-byte buffer; keep every call below that limit.
  output.printf("bot state notes=%s notes_used=%u notes_limit=16 principal_used=%u principal_limit=8 per_principal=4 value_bytes=96 ",
                notes.ready() ? "ready" : "unavailable", unsigned(notes.countNotes()),
                unsigned(notes.countPrincipals()));
  output.printf("storage_used_kb=%lu storage_total_kb=%lu BLE_active=%u inbox_drops=%lu write_device_burst=%u write_principal_burst=%u write_refill_s=%lu ",
                static_cast<unsigned long>(storageUsed), static_cast<unsigned long>(storageTotal),
                unsigned(bleActive), static_cast<unsigned long>(inboxDrops),
                unsigned(NoteStore::DEVICE_WRITE_BURST), unsigned(NoteStore::PRINCIPAL_WRITE_BURST),
                static_cast<unsigned long>(NoteStore::WRITE_REFILL_MS / 1000));
  output.printf("notes_backend=qspi-ring notes_partition_kb=512 notes_slots=%u notes_jedec=%06lx notes_guard=%s notes_mount_result=%d notes_traverse_result=%d notes_overlap_blocks=%lu\n",
                unsigned(slots), static_cast<unsigned long>(jedec), partition.label(),
                partition.mountResult, partition.traverseResult,
                static_cast<unsigned long>(partition.overlapBlocks));
}
}
