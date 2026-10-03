#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

class Stream {
public:
  virtual ~Stream() = default;
  virtual int available() { return 0; }
  virtual int read() { return -1; }
  virtual size_t write(const uint8_t*, size_t) = 0;
  size_t readBytes(uint8_t* bytes, size_t count) {
    size_t n = 0;
    while (n < count) {
      int value = read();
      if (value < 0) break;
      bytes[n++] = value;
    }
    return n;
  }
  size_t print(char c) { return write(reinterpret_cast<const uint8_t*>(&c), 1); }
  size_t print(const char* text) { return write(reinterpret_cast<const uint8_t*>(text), strlen(text)); }
  size_t println() { return print("\r\n"); }
};
