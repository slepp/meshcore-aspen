// SPDX-License-Identifier: Apache-2.0
#if NRFMAST_PRODUCTION_LUA
#include "PineFilesystem.h"
#include <new>

namespace nrfmast {
PineFilesystem botFilesystem;
struct PineFile::Handle {
  PineFilesystem &owner;
  lfs_file_t file{};
  bool opened = false;
  explicit Handle(PineFilesystem &fs) : owner(fs) {}
  ~Handle() {
    owner.lock();
    if (opened) {
      if (lfs_file_close(&owner.fs_, &file) != 0) owner.ready_ = false;
      --owner.handles_;
    }
    owner.unlock();
  }
};
int PineFilesystem::readBlock(const lfs_config *c, lfs_block_t b, lfs_off_t offset, void *out, lfs_size_t size) {
  return b < c->block_count && offset <= c->block_size && size <= c->block_size - offset &&
      static_cast<NoteFlash *>(c->context)->read(b * c->block_size + offset, out, size) ? 0 : LFS_ERR_IO;
}
int PineFilesystem::programBlock(const lfs_config *c, lfs_block_t b, lfs_off_t offset, const void *in, lfs_size_t size) {
  return b < c->block_count && offset <= c->block_size && size <= c->block_size - offset &&
      static_cast<NoteFlash *>(c->context)->program(b * c->block_size + offset, in, size) ? 0 : LFS_ERR_IO;
}
int PineFilesystem::eraseBlock(const lfs_config *c, lfs_block_t b) {
  return b < c->block_count && static_cast<NoteFlash *>(c->context)->erase(b * c->block_size) ? 0 : LFS_ERR_IO;
}
bool PineFilesystem::begin(NoteFlash &flash, bool format) {
  if (mounted_) return ready_ && !format;
  if (!mutex_) mutex_ = xSemaphoreCreateMutexStatic(&mutexStorage_);
  if (!mutex_ || !flash.begin()) return false;
  flash_ = &flash;
  config_ = {};
  config_.context = flash_;
  config_.read = readBlock; config_.prog = programBlock; config_.erase = eraseBlock; config_.sync = syncBlock;
  config_.read_size = config_.prog_size = 256;
  config_.block_size = NoteJournal::SECTOR; config_.block_count = Bytes / NoteJournal::SECTOR;
  config_.lookahead = 384; config_.read_buffer = readBuffer_; config_.prog_buffer = writeBuffer_;
  config_.lookahead_buffer = lookahead_;
  lock();
  if (format && lfs_format(&fs_, &config_) != 0) { unlock(); return false; }
  mounted_ = ready_ = lfs_mount(&fs_, &config_) == 0;
  unlock();
  if (ready_) {
    ready_ = mkdir("/command-bot") && mkdir("/metadata");
  }
  return ready_;
}
bool PineFilesystem::validPath(const char *path) {
  if (!path || path[0] != '/' || strlen(path) > 95 || strstr(path, "..") ||
      (strncmp(path, "/command-bot/", 13) && strncmp(path, "/metadata/", 10) &&
       strcmp(path, "/command-bot") && strcmp(path, "/metadata"))) return false;
  for (const char *p = path; *p; ++p)
    if (*p < 32 || *p > 126 || *p == '\\') return false;
  return true;
}
bool PineFilesystem::end() {
  if (!mutex_) return true;
  lock();
  const bool ok = !handles_ && (!mounted_ || lfs_unmount(&fs_) == 0);
  if (ok) mounted_ = ready_ = false;
  unlock(); return ok;
}
PineFile PineFilesystem::open(const char *path, const char *mode) {
  PineFile result;
  if (!ready_ || !validPath(path) || !mode) return result;
  int flags = !strcmp(mode, "r") ? LFS_O_RDONLY : !strcmp(mode, "r+") ? LFS_O_RDWR :
      !strcmp(mode, "w") ? LFS_O_RDWR | LFS_O_CREAT | LFS_O_TRUNC : 0;
  if (!flags) return result;
  std::shared_ptr<PineFile::Handle> handle(new (std::nothrow) PineFile::Handle(*this));
  if (!handle) return result;
  lock();
  if (ready_ && handles_ < 12) {
    const int status = lfs_file_open(&fs_, &handle->file, path, flags);
    check(status);
    if (status == 0) {
      ++handles_; handle->opened = true; result.handle_ = handle;
    }
  }
  unlock();
  return result;
}
bool PineFilesystem::exists(const char *path) {
  if (!ready_ || !validPath(path)) return false;
  lfs_info info{};
  lock(); const int result = ready_ ? lfs_stat(&fs_, path, &info) : LFS_ERR_IO;
  check(result); unlock();
  return result == 0;
}
bool PineFilesystem::remove(const char *path) {
  if (!ready_ || !validPath(path)) return false;
  lock(); const int result = ready_ ? lfs_remove(&fs_, path) : LFS_ERR_IO;
  check(result); unlock(); return result == 0;
}
bool PineFilesystem::rename(const char *from, const char *to) {
  if (!ready_ || !validPath(from) || !validPath(to)) return false;
  lock(); const int result = ready_ ? lfs_rename(&fs_, from, to) : LFS_ERR_IO;
  check(result); unlock(); return result == 0;
}
bool PineFilesystem::mkdir(const char *path) {
  if (!ready_ || !validPath(path)) return false;
  lock(); const int result = ready_ ? lfs_mkdir(&fs_, path) : LFS_ERR_IO;
  check(result); unlock(); return result == 0 || result == LFS_ERR_EXIST;
}
bool PineFilesystem::capacity(uint32_t &used, uint32_t &total) {
  used = total = 0;
  if (!ready_) return false;
  lock();
  uint32_t blocks = 0;
  const int result = lfs_traverse(&fs_, [](void *context, lfs_block_t) {
    ++*static_cast<uint32_t *>(context); return 0;
  }, &blocks);
  check(result); unlock();
  if (result || blocks > config_.block_count) return false;
  used = blocks * config_.block_size; total = Bytes; return true;
}
PineFile::operator bool() const { return handle_ && handle_->opened && handle_->owner.ready_; }
size_t PineFile::size() const {
  if (!*this) return 0;
  auto &h = *handle_; h.owner.lock();
  const auto size = lfs_file_size(&h.owner.fs_, &h.file);
  h.owner.check(size); h.owner.unlock();
  return size < 0 ? 0 : size;
}
int PineFile::read() { uint8_t byte; return read(&byte, 1) == 1 ? byte : -1; }
size_t PineFile::read(uint8_t *bytes, size_t size) {
  if (!*this || !bytes || size > INT32_MAX) return 0;
  auto &h = *handle_; h.owner.lock();
  const auto count = lfs_file_read(&h.owner.fs_, &h.file, bytes, size);
  h.owner.check(count); h.owner.unlock();
  return count < 0 ? 0 : count;
}
size_t PineFile::write(const uint8_t *bytes, size_t size) {
  if (!*this || !bytes || size > INT32_MAX) return 0;
  auto &h = *handle_; h.owner.lock();
  const auto count = lfs_file_write(&h.owner.fs_, &h.file, bytes, size);
  h.owner.check(count); h.owner.unlock();
  return count < 0 ? 0 : count;
}
bool PineFile::seek(uint32_t position) {
  if (!*this || position > INT32_MAX) return false;
  auto &h = *handle_; h.owner.lock();
  const auto result = lfs_file_seek(&h.owner.fs_, &h.file, position, LFS_SEEK_SET);
  h.owner.check(result); h.owner.unlock(); return result == int32_t(position);
}
void PineFile::flush() {
  if (!*this) return;
  auto &h = *handle_; h.owner.lock();
  if (lfs_file_sync(&h.owner.fs_, &h.file) != 0) h.owner.ready_ = false;
  h.owner.unlock();
}
void PineFile::close() { handle_.reset(); }
}
#endif
