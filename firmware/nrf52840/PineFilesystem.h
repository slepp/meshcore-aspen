// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <Arduino.h>
#include <littlefs/lfs.h>
#include <memory>
#include <atomic>
#include "NoteJournal.h"

namespace nrfmast {
class PineFilesystem;
class PineFile {
  struct Handle;
  std::shared_ptr<Handle> handle_;
  friend class PineFilesystem;
public:
  explicit operator bool() const;
  size_t size() const;
  int read();
  size_t read(uint8_t *bytes, size_t size);
  size_t write(const uint8_t *bytes, size_t size);
  bool seek(uint32_t position);
  void flush();
  void close();
  bool isDirectory() const { return false; }
};
class PineFilesystem {
  friend class PineFile;
  friend struct PineFile::Handle;
  NoteFlash *flash_ = nullptr;
  lfs_t fs_{};
  lfs_config config_{};
  StaticSemaphore_t mutexStorage_{};
  SemaphoreHandle_t mutex_ = nullptr;
  std::atomic<bool> ready_{false};
  bool mounted_ = false;
  unsigned handles_ = 0;
  alignas(4) uint8_t readBuffer_[256]{}, writeBuffer_[256]{}, lookahead_[48]{};
  static int readBlock(const lfs_config *, lfs_block_t, lfs_off_t, void *, lfs_size_t);
  static int programBlock(const lfs_config *, lfs_block_t, lfs_off_t, const void *, lfs_size_t);
  static int eraseBlock(const lfs_config *, lfs_block_t);
  static int syncBlock(const lfs_config *) { return 0; }
  void lock() const { xSemaphoreTake(mutex_, portMAX_DELAY); }
  void unlock() const { xSemaphoreGive(mutex_); }
  void check(int result) {
    if (result == LFS_ERR_IO || result == LFS_ERR_CORRUPT) ready_ = false;
  }
  static bool validPath(const char *);
public:
  static constexpr uint32_t Bytes = NoteJournal::BASE;
  bool begin(NoteFlash &flash, bool format);
  bool end();
  bool ready() const { return ready_; }
  PineFile open(const char *path, const char *mode = "r");
  bool exists(const char *path);
  bool remove(const char *path);
  bool rename(const char *from, const char *to);
  bool mkdir(const char *path);
  bool capacity(uint32_t &used, uint32_t &total);
};
extern PineFilesystem botFilesystem;
}
