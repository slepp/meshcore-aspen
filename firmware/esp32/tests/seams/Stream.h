#pragma once
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

class Stream {
public:
  virtual ~Stream() = default;
  virtual int available() { return 0; }
  virtual int read() { return -1; }
  virtual int peek() { return -1; }
  virtual void flush() {}
  virtual int availableForWrite() { return 4096; }
  virtual size_t write(uint8_t byte) { return write(&byte, 1); }
  virtual size_t write(const uint8_t *, size_t) = 0;
  size_t readBytes(uint8_t *data, size_t size) {
    size_t count = 0;
    while (count < size) {
      const int value = read();
      if (value < 0)
        break;
      data[count++] = value;
    }
    return count;
  }
  size_t print(char value) { return write(uint8_t(value)); }
  size_t print(const char *value) {
    return write(reinterpret_cast<const uint8_t *>(value), strlen(value));
  }
  size_t printf(const char *format, ...) {
    va_list args, copy;
    va_start(args, format);
    va_copy(copy, args);
    const int size = vsnprintf(nullptr, 0, format, copy);
    va_end(copy);
    if (size < 0) {
      va_end(args);
      return 0;
    }
    std::vector<char> text(size + 1);
    vsnprintf(text.data(), text.size(), format, args);
    va_end(args);
    return write(reinterpret_cast<const uint8_t *>(text.data()), size);
  }
  size_t print(int value, int = 10) { return printf("%d", value); }
  size_t print(unsigned value, int = 10) { return printf("%u", value); }
  size_t print(long value, int = 10) { return printf("%ld", value); }
  size_t print(unsigned long value, int = 10) { return printf("%lu", value); }
  size_t print(double value, int digits = 2) {
    return printf("%.*f", digits, value);
  }
  size_t println() { return print("\r\n"); }
  size_t println(const char *value) { return print(value) + println(); }
};
