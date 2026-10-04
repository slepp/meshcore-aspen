#pragma once
#include "Arduino.h"
#include <ctime>
#include <memory>
enum SeekMode { SeekSet, SeekCur, SeekEnd };
namespace fs {
class FileImpl;
using FileImplPtr = std::shared_ptr<FileImpl>;
class FileImpl {
public:
  virtual ~FileImpl() = default;
  virtual size_t write(const uint8_t *, size_t) = 0;
  virtual size_t read(uint8_t *, size_t) = 0;
  virtual void flush() = 0;
  virtual bool seek(uint32_t, SeekMode) = 0;
  virtual size_t position() const = 0;
  virtual size_t size() const = 0;
  virtual bool setBufferSize(size_t) = 0;
  virtual void close() = 0;
  virtual time_t getLastWrite() = 0;
  virtual const char *path() const = 0;
  virtual const char *name() const = 0;
  virtual boolean isDirectory() = 0;
  virtual FileImplPtr openNextFile(const char *) = 0;
  virtual boolean seekDir(long) = 0;
  virtual String getNextFileName() = 0;
  virtual String getNextFileName(bool *) = 0;
  virtual void rewindDirectory() = 0;
  virtual operator bool() = 0;
};
class FSImpl {
public:
  virtual ~FSImpl() = default;
  virtual FileImplPtr open(const char *, const char *, bool) = 0;
  virtual bool exists(const char *) = 0;
  virtual bool remove(const char *) = 0;
  virtual bool rename(const char *, const char *) = 0;
  virtual bool mkdir(const char *) = 0;
  virtual bool rmdir(const char *) = 0;
};
using FSImplPtr = std::shared_ptr<FSImpl>;
} // namespace fs
