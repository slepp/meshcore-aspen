#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

unsigned long millis();

class SerialMock {
public:
  template <typename... Args>
  void printf(const char*, Args...) {}
  void println(const char*) {}
};

inline SerialMock Serial;

class Stream {
public:
  virtual ~Stream() = default;
  virtual int available() = 0;
  virtual int read() = 0;
  virtual int peek() { return -1; }
  virtual void flush() {}
  virtual int availableForWrite() { return 0; }
  virtual std::size_t write(uint8_t byte) = 0;
  virtual std::size_t write(const uint8_t* data, std::size_t length) {
    std::size_t written = 0;
    while (written < length) written += write(data[written]);
    return written;
  }
};
