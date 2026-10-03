#pragma once
#include "NoteJournal.h"

namespace nrfmast {
struct NotePartitionStatus {
  bool checked = false;
  bool retired = false;
  int mountResult = 0, traverseResult = 0;
  uint32_t overlapBlocks = 0;
  const char* label() const {
    if (retired) return "retired";
    if (!checked) return "not-checked";
    if (mountResult || traverseResult) return "unreadable";
    return overlapBlocks ? "occupied" : "clear";
  }
};
bool notePartitionFree(NoteFlash& flash, uint32_t address, uint32_t bytes,
                       NotePartitionStatus* status = nullptr);
struct NoteVolumeEntry {
  enum Kind { File, Directory, Allocation } kind;
  char path[96] = {};
  uint32_t bytes = 0, block = UINT32_MAX, secondBlock = UINT32_MAX;
};
struct NoteVolumeStatus {
  int mountResult = 0, traverseResult = 0, scanResult = 0;
  uint32_t entries = 0, blocks = 0;
  bool limited = false;
};
using NoteVolumeVisitor = void (*)(void*, const NoteVolumeEntry&);
bool inspectNoteVolume(NoteFlash& flash, NoteVolumeVisitor visitor, void* context,
                       NoteVolumeStatus& status);
}
