#pragma once
#include "Stream.h"
#include <algorithm>
#include <limits>
#include <map>
#include <memory>
#include <string>

struct MemoryVolume {
  std::map<std::string, std::vector<uint8_t>> files;
  size_t write_limit = std::numeric_limits<size_t>::max();
  bool fail_open = false;
};
class File : public Stream {
  std::shared_ptr<MemoryVolume> volume;
  std::string path;
  size_t position = 0;
  bool writable = false;
public:
  File() = default;
  File(std::shared_ptr<MemoryVolume> v, const std::string& p, bool w)
    : volume(v), path(p), writable(w) {}
  explicit operator bool() const { return bool(volume); }
  int available() override { return volume ? volume->files.at(path).size() - position : 0; }
  int read() override {
    if (!volume || position >= volume->files.at(path).size()) return -1;
    return volume->files.at(path)[position++];
  }
  size_t read(uint8_t* dst, size_t n) { return readBytes(dst, n); }
  size_t write(const uint8_t* src, size_t n) override {
    if (!volume || !writable) return 0;
    n = std::min(n, volume->write_limit);
    auto& bytes = volume->files[path];
    if (position + n > bytes.size()) bytes.resize(position+n);
    std::copy(src, src+n, bytes.begin()+position);
    position += n;
    if (volume->write_limit != std::numeric_limits<size_t>::max()) volume->write_limit -= n;
    return n;
  }
  void close() { volume.reset(); }
  bool seek(size_t n) {
    if (!volume || n > volume->files.at(path).size()) return false;
    position = n; return true;
  }
  size_t size() const { return volume ? volume->files.at(path).size() : 0; }
};
class MemoryFS {
public:
  std::shared_ptr<MemoryVolume> volume = std::make_shared<MemoryVolume>();
  File open(const char* path, const char* mode = "r", bool = false) {
    if (volume->fail_open) return {};
    bool writable = mode[0] != 'r';
    if (!writable && !exists(path)) return {};
    if (mode[0] == 'w') volume->files[path].clear();
    File file(volume, path, writable);
    if (mode[0] == 'a') { volume->files[path]; file.seek(file.size()); }
    return file;
  }
  bool exists(const char* path) { return volume->files.count(path); }
  bool remove(const char* path) { return volume->files.erase(path) != 0; }
  bool mkdir(const char*) { return true; }
  bool format() { volume->files.clear(); return true; }
};
#define FILESYSTEM MemoryFS
