#pragma once
#include <Stream.h>
#include <cmath>
#include <cstdio>
#include <cassert>
#include <map>
#include <memory>
#include <functional>
#include <string>
#include <vector>

inline char* ltoa(long value, char* output, int base) {
  assert(base == 10);
  std::sprintf(output, "%ld", value);
  return output;
}

class SerialLog {
public:
  void println(const char* text) { std::puts(text); }
  template <typename... Args> void printf(const char* format, Args... args) {
    std::printf(format, args...);
  }
};
inline SerialLog Serial;

class File : public Stream {
  std::shared_ptr<std::vector<uint8_t>> bytes;
  size_t position = 0;
  int* writeBudget = nullptr;
  int* openCount = nullptr;
public:
  File() = default;
  File(std::shared_ptr<std::vector<uint8_t>> data, int* budget = nullptr, int* count = nullptr)
      : bytes(data), writeBudget(budget), openCount(count) { if (openCount) ++*openCount; }
  explicit operator bool() const { return bool(bytes); }
  size_t size() const { return bytes ? bytes->size() : 0; }
  int read() override { return bytes && position < bytes->size() ? (*bytes)[position++] : -1; }
  size_t read(uint8_t* output, size_t count) { return readBytes(output, count); }
  size_t write(const uint8_t* input, size_t count) override {
    if (!bytes || !writeBudget) return 0;
    if (*writeBudget >= 0 && count > size_t(*writeBudget)) count = *writeBudget;
    if (*writeBudget >= 0) *writeBudget -= count;
    bytes->insert(bytes->end(), input, input + count);
    return count;
  }
  void close() {
    if (bytes && openCount) --*openCount;
    bytes.reset();
    openCount = nullptr;
  }
};

class NativeFilesystem {
public:
  std::map<std::string, std::shared_ptr<std::vector<uint8_t>>> files;
  int writeBudget = -1;
  int activeHandles = 0;
  unsigned writeOpens = 0;
  bool failOpen = false, failRename = false;
  bool applyRenameFailure = false;
  std::function<void(const char*)> afterRename;
  void mkdir(const char*) {}
  bool exists(const char* path) const { return files.count(path) != 0; }
  bool remove(const char* path) { return files.erase(path) != 0; }
  File open(const char* path, const char* mode = "r", bool = false) {
    if (!strcmp(mode, "w")) ++writeOpens;
    if (failOpen) return {};
    if (!strcmp(mode, "w")) {
      auto bytes = std::make_shared<std::vector<uint8_t>>();
      files[path] = bytes;
      return File(bytes, &writeBudget, &activeHandles);
    }
    auto found = files.find(path);
    return found == files.end() ? File() : File(found->second, nullptr, &activeHandles);
  }
  bool rename(const char* from, const char* to) {
    auto found = files.find(from);
    if (found == files.end() || (failRename && !applyRenameFailure)) return false;
    const bool failed = failRename;
    files[to] = found->second;
    files.erase(found);
    if (afterRename) afterRename(to);
    return !failed;
  }
};
#define FILESYSTEM NativeFilesystem
