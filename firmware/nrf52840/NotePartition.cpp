#include "NotePartition.h"
#include <littlefs/lfs.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace nrfmast {
namespace {
int read(const lfs_config* config, lfs_block_t block, lfs_off_t offset,
         void* output, lfs_size_t bytes) {
  auto* flash = static_cast<NoteFlash*>(config->context);
  return flash->read(block * config->block_size + offset, output, bytes) ? 0 : LFS_ERR_IO;
}
int denyProgram(const lfs_config*, lfs_block_t, lfs_off_t, const void*, lfs_size_t) { return -1; }
int denyErase(const lfs_config*, lfs_block_t) { return -1; }
int sync(const lfs_config*) { return 0; }
lfs_config readOnlyConfig(NoteFlash& flash) {
  lfs_config config = {};
  config.context = &flash;
  config.read = read;
  config.prog = denyProgram;
  config.erase = denyErase;
  config.sync = sync;
  config.read_size = config.prog_size = 256;
  config.block_size = NoteJournal::SECTOR;
  config.block_count = 512;
  config.lookahead = 32;
  return config;
}
struct Region { uint32_t first, end, overlaps; uint8_t seen[NoteJournal::SLOTS / 8]; };
int allocated(void* context, lfs_block_t block) {
  auto& region = *static_cast<Region*>(context);
  if (block >= region.first && block < region.end) {
    const uint32_t index = block - region.first;
    const uint8_t bit = 1U << (index % 8);
    if (!(region.seen[index / 8] & bit)) ++region.overlaps;
    region.seen[index / 8] |= bit;
  }
  return 0;
}
struct Inventory {
  NoteVolumeVisitor visitor;
  void* context;
  NoteVolumeStatus* status;
  uint8_t seen[64];
};
int inventoryBlock(void* context, lfs_block_t block) {
  auto& inventory = *static_cast<Inventory*>(context);
  if (block >= 512) return LFS_ERR_CORRUPT;
  const uint8_t bit = 1U << (block % 8);
  if (inventory.seen[block / 8] & bit) return 0;
  inventory.seen[block / 8] |= bit;
  ++inventory.status->blocks;
  NoteVolumeEntry entry = {};
  entry.kind = NoteVolumeEntry::Allocation;
  entry.block = block;
  inventory.visitor(inventory.context, entry);
  return 0;
}
bool printableName(const char* name) {
  for (size_t i = 0; i <= LFS_NAME_MAX; ++i) {
    const auto byte = static_cast<unsigned char>(name[i]);
    if (!byte) return i != 0;
    if (byte < 32 || byte > 126 || byte == '/') return false;
  }
  return false;
}
}

bool notePartitionFree(NoteFlash& flash, uint32_t address, uint32_t bytes, NotePartitionStatus* status) {
  NotePartitionStatus result;
  if (status) *status = result;
  if (address != NoteJournal::BASE || bytes != NoteJournal::BYTES) return false;
  result.checked = true;
  lfs_config config = readOnlyConfig(flash);
  lfs_t legacy = {};
  result.mountResult = lfs_mount(&legacy, &config);
  if (result.mountResult != 0) { if (status) *status = result; return false; }
  Region region = {address / config.block_size, (address + bytes) / config.block_size, 0, {}};
  result.traverseResult = lfs_traverse(&legacy, allocated, &region);
  result.overlapBlocks = region.overlaps;
  lfs_unmount(&legacy);
  if (status) *status = result;
  return result.traverseResult == 0 && !result.overlapBlocks;
}

bool inspectNoteVolume(NoteFlash& flash, NoteVolumeVisitor visitor, void* context,
                       NoteVolumeStatus& status) {
  status = {};
  if (!visitor) { status.scanResult = LFS_ERR_INVAL; return false; }
  auto config = readOnlyConfig(flash);
  lfs_t filesystem = {};
  status.mountResult = lfs_mount(&filesystem, &config);
  if (status.mountResult) return false;
  Inventory inventory = {visitor, context, &status, {}};
  status.traverseResult = lfs_traverse(&filesystem, inventoryBlock, &inventory);
  // Keep bounded directory paths off the small firmware loop stack.
  using Path = char[sizeof(NoteVolumeEntry::path)];
  auto* paths = static_cast<Path*>(std::calloc(9, sizeof(Path)));
  if (!paths) status.scanResult = LFS_ERR_NOMEM;
  if (paths && !status.traverseResult) {
    std::strcpy(paths[0], "/");
    unsigned directories = 1;
    for (unsigned index = 0; index < directories && !status.scanResult; ++index) {
      lfs_dir_t directory = {};
      status.scanResult = lfs_dir_open(&filesystem, &directory, paths[index]);
      if (status.scanResult) break;
      NoteVolumeEntry entry = {};
      entry.kind = NoteVolumeEntry::Directory;
      std::strcpy(entry.path, paths[index]);
      entry.block = directory.head[0];
      entry.secondBlock = directory.head[1];
      visitor(context, entry);
      lfs_info info = {};
      int result;
      while ((result = lfs_dir_read(&filesystem, &directory, &info)) > 0) {
        if (!std::strcmp(info.name, ".") || !std::strcmp(info.name, "..")) continue;
        if (!printableName(info.name)) { status.scanResult = LFS_ERR_INVAL; break; }
        if (++status.entries > 32) { status.limited = true; break; }
        entry = {};
        const int length = std::snprintf(entry.path, sizeof(entry.path), "%s%s%s",
                                        paths[index], index ? "/" : "", info.name);
        if (length < 0 || size_t(length) >= sizeof(entry.path)) { status.limited = true; break; }
        if (info.type == LFS_TYPE_DIR) {
          if (directories == 9) { status.limited = true; break; }
          std::strcpy(paths[directories++], entry.path);
        } else if (info.type == LFS_TYPE_REG) {
          lfs_file_t file = {};
          status.scanResult = lfs_file_open(&filesystem, &file, entry.path, LFS_O_RDONLY);
          if (status.scanResult) break;
          entry.kind = NoteVolumeEntry::File;
          entry.bytes = file.size;
          entry.block = file.size ? file.head : UINT32_MAX;
          status.scanResult = lfs_file_close(&filesystem, &file);
          if (status.scanResult) break;
          visitor(context, entry);
        } else { status.scanResult = LFS_ERR_CORRUPT; break; }
      }
      if (result < 0 && !status.scanResult) status.scanResult = result;
      const int closed = lfs_dir_close(&filesystem, &directory);
      if (!status.scanResult) status.scanResult = closed;
      if (status.limited && !status.scanResult) status.scanResult = LFS_ERR_INVAL;
    }
  }
  std::free(paths);
  lfs_unmount(&filesystem);
  return !status.mountResult && !status.traverseResult && !status.scanResult;
}
}
