// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <FS.h>
#include <memory>

// The state directory must already exist at an absolute, symlink-free path,
// owned by the effective user with mode 0700. No directory is formatted or
// created. Only /command-bot/<bounded-name> is addressable. At most 32
// files of 4096 bytes each are stored, in private checksummed records.
// A leftover temporary file or corrupt record blocks initialization.
bool native_spiffs_init(const char *absolute_private_directory);
void native_spiffs_shutdown();
// Sticky until shutdown; also true when the adapter is not initialized.
// Callers must check this after void flush()/close(), before reporting success.
bool native_spiffs_faulted();

namespace hostfs {
class File {
public:
  struct Impl;
  File();
  ~File();
  File(File &&) noexcept;
  File &operator=(File &&) noexcept;
  File(const File &) = delete;
  File &operator=(const File &) = delete;

  explicit operator bool() const;
  size_t read(uint8_t *data, size_t length);
  int read();
  size_t write(const uint8_t *data, size_t length);
  size_t write(uint8_t data);
  size_t size() const;
  size_t position() const;
  bool seek(uint32_t position, SeekMode mode = SeekSet);
  bool isDirectory() const;
  void flush();
  void close();

private:
  explicit File(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
  friend class SPIFFSFS;
};

class SPIFFSFS {
public:
  File open(const char *path, const char *mode = "r");
  bool exists(const char *path);
  bool remove(const char *path);
  bool rename(const char *from, const char *to);
  size_t totalBytes() const;
  size_t usedBytes() const;
};
} // namespace hostfs

namespace fs {
class SPIFFSFS : public FS {
public:
  SPIFFSFS();
  bool begin(bool format = false) const { return !format && !native_spiffs_faulted(); }
  bool format() const { return false; }
  size_t totalBytes() const;
  size_t usedBytes() const;
};
} // namespace fs

extern fs::SPIFFSFS SPIFFS;
