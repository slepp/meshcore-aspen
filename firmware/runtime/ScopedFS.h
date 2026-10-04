// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FS.h>
#include <FSImpl.h>
#include <errno.h>
#include <memory>

namespace onchip {

struct ScopedIOState {
  bool failed = false;
};
class ScopedFile final : public fs::FileImpl {
  File file;
  std::shared_ptr<ScopedIOState> state;

public:
  ScopedFile(File value, std::shared_ptr<ScopedIOState> status)
      : file(value), state(status) {}
  size_t write(const uint8_t *p, size_t n) override {
    const size_t written = file.write(p, n);
    if (written != n)
      state->failed = true;
    return written;
  }
  size_t read(uint8_t *p, size_t n) override { return file.read(p, n); }
  void flush() override { file.flush(); }
  bool seek(uint32_t p, SeekMode m) override { return file.seek(p, m); }
  size_t position() const override { return file.position(); }
  size_t size() const override { return file.size(); }
  bool setBufferSize(size_t n) override { return file.setBufferSize(n); }
  void close() override { file.close(); }
  time_t getLastWrite() override { return file.getLastWrite(); }
  const char *path() const override { return file.path(); }
  const char *name() const override { return file.name(); }
  boolean isDirectory() override { return false; }
  fs::FileImplPtr openNextFile(const char *) override { return {}; }
  boolean seekDir(long) override { return false; }
  String getNextFileName() override { return {}; }
  String getNextFileName(bool *dir) override {
    *dir = false;
    return {};
  }
  void rewindDirectory() override {}
  operator bool() override { return bool(file); }
};

class ScopedFSImpl final : public fs::FSImpl {
  fs::FS &base;
  const char *prefix;
  std::shared_ptr<ScopedIOState> state;
  bool path(const char *input, char (&output)[96]) const {
    if (!input || input[0] != '/' || strstr(input, ".."))
      return false;
    const int n = snprintf(output, sizeof(output), "%s%s", prefix, input);
    return n > 0 && size_t(n) < sizeof(output);
  }

public:
  ScopedFSImpl(fs::FS &fs, const char *p, std::shared_ptr<ScopedIOState> status)
      : base(fs), prefix(p), state(status) {}
  fs::FileImplPtr open(const char *p, const char *mode, bool create) override {
    char full[96];
    if (!path(p, full)) {
      state->failed = true;
      return {};
    }
    auto f = base.open(full, mode, create);
    if (!f && (mode[0] != 'r' || strchr(mode, '+')))
      state->failed = true;
    return f ? fs::FileImplPtr(new ScopedFile(f, state)) : fs::FileImplPtr{};
  }
  bool exists(const char *p) override {
    char full[96];
    return path(p, full) && base.exists(full);
  }
  bool remove(const char *p) override {
    char full[96];
    return path(p, full) && base.remove(full);
  }
  bool rename(const char *a, const char *b) override {
    char from[96], to[96];
    return path(a, from) && path(b, to) && base.rename(from, to);
  }
  bool mkdir(const char *p) override {
    char full[96];
    return path(p, full) && base.mkdir(full);
  }
  bool rmdir(const char *p) override {
    char full[96];
    return path(p, full) && base.rmdir(full);
  }
};

class ScopedFS final : public fs::FS {
  std::shared_ptr<ScopedIOState> state;
  ScopedFS(fs::FS &base, const char *prefix,
           std::shared_ptr<ScopedIOState> status)
      : fs::FS(fs::FSImplPtr(new ScopedFSImpl(base, prefix, status))),
        state(status) {}

public:
  ScopedFS(fs::FS &base, const char *prefix)
      : ScopedFS(base, prefix, std::make_shared<ScopedIOState>()) {}
  void clearErrors() { state->failed = false; }
  bool failed() const { return state->failed; }
};

class ScopedErase {
  fs::FS &base;
  const char *prefix;
  File directory;

public:
  ScopedErase(fs::FS &fs, const char *scope) : base(fs), prefix(scope) {}
  bool step(bool &done) {
    done = false;
    if (!directory) {
      directory = base.open("/");
      if (!directory || !directory.isDirectory()) {
        Serial.println("Role erase could not open the filesystem directory");
        directory.close();
        return false;
      }
    }
    for (unsigned n = 0; n < 8; ++n) {
      errno = 0;
      File file = directory.openNextFile();
      if (!file) {
        const int traversalError = errno;
        directory.close();
        if (traversalError) {
          Serial.println("Role erase directory traversal failed");
          return false;
        }
        done = true;
        return true;
      }
      const char *path = file.path();
      if (!path) {
        file.close();
        directory.close();
        Serial.println("Role erase received an invalid file path");
        return false;
      }
      const size_t scopeLength = strlen(prefix);
      if (strncmp(path, prefix, scopeLength) == 0 && path[scopeLength] == '/') {
        char owned[96];
        const int length = snprintf(owned, sizeof(owned), "%s", path);
        file.close();
        directory.close();
        if (length < 0 || size_t(length) >= sizeof(owned) ||
            !base.remove(owned)) {
          Serial.println("Role erase failed to remove an owned file");
          return false;
        }
        return true;
      }
      file.close();
    }
    return true;
  }
};
} // namespace onchip
