#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdarg>
#include <cstdio>
#include <vector>

// Only byte I/O needed by upstream Identity and Utils; no protocol behaviour.
class Stream {
public:
  virtual ~Stream() = default;
  virtual int read() { return -1; }
  virtual int available() { return 0; }
  virtual int peek() { return -1; }
  virtual void flush() {}
  virtual size_t write(uint8_t b) { return write(&b, 1); }
  virtual size_t write(const uint8_t*, size_t) = 0;
  size_t readBytes(uint8_t* bytes, size_t size) {
    size_t n = 0;
    for (; n < size; ++n) {
      int b = read();
      if (b < 0) break;
      bytes[n] = static_cast<uint8_t>(b);
    }
    return n;
  }
  size_t print(char c) { return write(reinterpret_cast<const uint8_t*>(&c), 1); }
  size_t print(const char* s) { return write(reinterpret_cast<const uint8_t*>(s), strlen(s)); }
  size_t printf(const char* format, ...) {
    va_list args, copy;
    va_start(args, format); va_copy(copy, args);
    int size = vsnprintf(nullptr, 0, format, copy); va_end(copy);
    if (size < 0) { va_end(args); return 0; }
    std::vector<char> text(size + 1);
    vsnprintf(text.data(), text.size(), format, args); va_end(args);
    return write(reinterpret_cast<const uint8_t*>(text.data()), size);
  }
  size_t print(int32_t n, int = 10) { return printf("%d", n); }
  size_t print(uint32_t n, int = 10) { return printf("%u", n); }
  size_t print(double n, int digits = 2) { return printf("%.*f", digits, n); }
  size_t print(float n, int digits = 2) { return print(double(n), digits); }
  size_t println() { return print("\r\n"); }
};
