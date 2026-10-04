#pragma once
#include "FS.h"
#include <cassert>
#include <limits>
#include <map>
#include <atomic>
namespace filesystem_test {
inline std::map<std::string, std::vector<uint8_t>> files;
inline bool failRemove = false, failOpen = false;
inline bool failReadAfterRename = false;
inline unsigned directoryReads = 0;
inline size_t writeLimit = std::numeric_limits<size_t>::max();
inline std::atomic<size_t> readLimit{std::numeric_limits<size_t>::max()};
inline unsigned long readDelayMs = 0;
inline size_t bytesRead = 0, readCalls = 0, largestRead = 0;
inline bool appendOnRead = false;
inline void (*afterWrite)() = nullptr, (*afterFlush)() = nullptr;
inline void (*step)(const char *) = nullptr;
inline void checkpoint(const char *name) { if (step) step(name); }
class StorageDriver : public fs::FSImpl {
public:
  virtual size_t totalBytes() const = 0;
  virtual size_t usedBytes() const = 0;
};
class FileImpl : public fs::FileImpl {
  std::string filename;
  bool writable, opened = true;
  size_t offset = 0, directoryOffset = 0;

public:
  FileImpl(std::string path, bool write) : filename(path), writable(write) {}
  size_t write(const uint8_t *data, size_t size) override {
    checkpoint("file.before-write");
    if (!opened || !writable)
      return 0;
    size = std::min(size, writeLimit);
    if (!size)
      return 0;
    auto &bytes = files[filename];
    if (offset + size > bytes.size())
      bytes.resize(offset + size);
    memcpy(bytes.data() + offset, data, size);
    offset += size;
    if (afterWrite) afterWrite();
    checkpoint("file.after-write");
    return size;
  }
  size_t read(uint8_t *data, size_t size) override {
    ++readCalls;
    largestRead = std::max(largestRead, size);
    if (readDelayMs) delay(readDelayMs);
    if (!opened)
      return 0;
    const auto &bytes = files.at(filename);
    size = std::min(std::min(size, bytes.size() - offset), readLimit.load());
    if (!size)
      return 0;
    memcpy(data, bytes.data() + offset, size);
    offset += size;
    bytesRead += size;
    if (appendOnRead) {
      appendOnRead = false;
      files.at(filename).push_back(0);
    }
    checkpoint("file.after-read");
    return size;
  }
  void flush() override {
    checkpoint("file.before-flush");
    if (afterFlush) afterFlush();
    checkpoint("file.after-flush");
  }
  bool seek(uint32_t position, SeekMode mode) override {
    size_t next = mode == SeekSet   ? position
                  : mode == SeekCur ? offset + position
                                    : size() + position;
    if (next > size())
      return false;
    offset = next;
    return true;
  }
  size_t position() const override { return offset; }
  size_t size() const override {
    return filename == "/" ? 0 : files.at(filename).size();
  }
  bool setBufferSize(size_t) override { return true; }
  void close() override { opened = false; checkpoint("file.after-close"); }
  time_t getLastWrite() override { return 0; }
  const char *path() const override { return filename.c_str(); }
  const char *name() const override { return filename.c_str(); }
  bool isDirectory() override { return filename == "/"; }
  fs::FileImplPtr openNextFile(const char *) override {
    ++directoryReads;
    if (!opened || filename != "/" || directoryOffset >= files.size())
      return {};
    auto next = files.begin();
    std::advance(next, directoryOffset++);
    return std::make_shared<FileImpl>(next->first, false);
  }
  bool seekDir(long value) override {
    directoryOffset = value;
    return true;
  }
  String getNextFileName() override { return {}; }
  String getNextFileName(bool *dir) override {
    *dir = false;
    return {};
  }
  void rewindDirectory() override { directoryOffset = 0; }
  operator bool() override { return opened; }
};
class Impl : public StorageDriver {
public:
  fs::FileImplPtr open(const char *path, const char *mode, bool) override {
    checkpoint(mode[0] == 'w' ? "file.before-open-write" : "file.before-open-read");
    if (failOpen)
      return {};
    const bool writable = mode[0] != 'r' || strchr(mode, '+');
    if (!writable && strcmp(path, "/") && !exists(path))
      return {};
    if (mode[0] == 'w')
      files[path].clear();
    auto result = std::make_shared<FileImpl>(path, writable);
    if (mode[0] == 'a') {
      files[path];
      result->seek(files[path].size(), SeekSet);
    }
    checkpoint(mode[0] == 'w' ? "file.after-open-write" : "file.after-open-read");
    return result;
  }
  bool exists(const char *path) override { return files.count(path); }
  bool remove(const char *path) override {
    return !failRemove && files.erase(path);
  }
  bool rename(const char *from, const char *to) override {
    if (!files.count(from))
      return false;
    files[to] = files[from];
    files.erase(from);
    if (failReadAfterRename) {
      failReadAfterRename = false;
      readLimit = 0;
    }
    return true;
  }
  bool mkdir(const char *) override { return true; }
  bool rmdir(const char *) override { return false; }
  size_t totalBytes() const override { return 1572864; }
  size_t usedBytes() const override {
    size_t size = 0;
    for (const auto &entry : files)
      size += entry.second.size();
    return size;
  }
};
} // namespace filesystem_test
namespace fs {
class SPIFFSFS : public FS {
  std::shared_ptr<filesystem_test::StorageDriver> driver_;

  explicit SPIFFSFS(std::shared_ptr<filesystem_test::StorageDriver> driver)
      : FS(driver), driver_(std::move(driver)) {}

public:
  SPIFFSFS() : SPIFFSFS(std::make_shared<filesystem_test::Impl>()) {}
  void setDriver(std::shared_ptr<filesystem_test::StorageDriver> driver) {
    if (!driver) driver = std::make_shared<filesystem_test::Impl>();
    driver_ = std::move(driver);
    FS::setDriver(driver_);
  }
  void useMemoryDriver() { setDriver({}); }
  bool begin(bool format) {
    assert(!format);
    return true;
  }
  bool format() {
    assert(false && "Global SPIFFS format is forbidden");
    return false;
  }
  size_t totalBytes() { return driver_->totalBytes(); }
  size_t usedBytes() { return driver_->usedBytes(); }
};
} // namespace fs
inline fs::SPIFFSFS SPIFFS;
