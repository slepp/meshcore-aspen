#pragma once
#include "FSImpl.h"
namespace fs {
class File : public Stream {
  FileImplPtr impl;

public:
  File() = default;
  explicit File(FileImplPtr value) : impl(value) {}
  operator bool() const { return impl && bool(*impl); }
  using Stream::write;
  size_t write(const uint8_t *data, size_t size) override {
    return impl ? impl->write(data, size) : 0;
  }
  size_t read(uint8_t *data, size_t size) {
    return impl ? impl->read(data, size) : 0;
  }
  int read() override {
    uint8_t data;
    return read(&data, 1) == 1 ? data : -1;
  }
  int available() override { return impl ? size() - position() : 0; }
  void flush() override {
    if (impl)
      impl->flush();
  }
  bool seek(uint32_t offset, SeekMode mode = SeekSet) {
    return impl && impl->seek(offset, mode);
  }
  size_t position() const { return impl ? impl->position() : 0; }
  size_t size() const { return impl ? impl->size() : 0; }
  bool setBufferSize(size_t size) { return impl && impl->setBufferSize(size); }
  void close() {
    if (impl)
      impl->close();
    impl.reset();
  }
  time_t getLastWrite() { return impl ? impl->getLastWrite() : 0; }
  const char *path() const { return impl ? impl->path() : nullptr; }
  const char *name() const { return impl ? impl->name() : nullptr; }
  bool isDirectory() { return impl && impl->isDirectory(); }
  File openNextFile(const char *mode = "r") {
    return File(impl ? impl->openNextFile(mode) : FileImplPtr{});
  }
  bool seekDir(long offset) { return impl && impl->seekDir(offset); }
  String getNextFileName() { return impl ? impl->getNextFileName() : ""; }
  String getNextFileName(bool *dir) {
    return impl ? impl->getNextFileName(dir) : "";
  }
  void rewindDirectory() {
    if (impl)
      impl->rewindDirectory();
  }
};
class FS {
  FSImplPtr impl;

public:
  explicit FS(FSImplPtr value) : impl(value) {}
  void setDriver(FSImplPtr value) { impl = std::move(value); }
  File open(const char *path, const char *mode = "r", bool create = false) {
    return File(impl->open(path, mode, create));
  }
  bool exists(const char *path) { return impl->exists(path); }
  bool remove(const char *path) { return impl->remove(path); }
  bool rename(const char *from, const char *to) {
    return impl->rename(from, to);
  }
  bool mkdir(const char *path) { return impl->mkdir(path); }
  bool rmdir(const char *path) { return impl->rmdir(path); }
};
} // namespace fs
using File = fs::File;
#define FILESYSTEM fs::FS
