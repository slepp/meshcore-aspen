#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

class Stream {
public:
  virtual ~Stream() = default;
  virtual int available() { return 0; }
  virtual int read() { return -1; }
  virtual int peek() { return -1; }
  virtual void flush() {}
  virtual int availableForWrite() { return 4096; }
  virtual size_t write(uint8_t b) { return write(&b, 1); }
  virtual size_t write(const uint8_t*, size_t) = 0;
  size_t readBytes(uint8_t* p, size_t n) {
    size_t count = 0;
    for (; count < n; ++count) {
      int b = read();
      if (b < 0) break;
      p[count] = b;
    }
    return count;
  }
  size_t print(char c) { return write(static_cast<uint8_t>(c)); }
  size_t print(const char* s) { return write(reinterpret_cast<const uint8_t*>(s), strlen(s)); }
  size_t println() { return print("\r\n"); }
};
