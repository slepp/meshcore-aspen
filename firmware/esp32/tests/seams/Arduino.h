#pragma once
#include "Stream.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>
#include <cassert>
using std::max;
using std::min;
template <class T, class U> auto min(T a, U b) -> decltype(a < b ? a : b) {
  return a < b ? a : b;
}
template <class T, class U> auto max(T a, U b) -> decltype(a > b ? a : b) {
  return a > b ? a : b;
}
using String = std::string;
using boolean = bool;
template <class T, class L, class H> T constrain(T value, L low, H high) {
  return value < low ? T(low) : value > high ? T(high) : value;
}
unsigned long millis();
void delay(unsigned long);
long random(long, long);
void randomSeed(long);
inline char *ltoa(long value, char *buffer, int radix) {
  if (radix != 10)
    std::abort();
  std::sprintf(buffer, "%ld", value);
  return buffer;
}
namespace serial_test {
inline thread_local bool forbidWrites = false;
}
class TestSerial : public Stream {
public:
  using Stream::write;
  size_t write(const uint8_t *data, size_t size) override {
    assert(!serial_test::forbidWrites && "Dispatch must not wait for diagnostic Serial output");
    return fwrite(data, 1, size, stderr);
  }
};
inline TestSerial Serial;
struct TestESP {
  uint32_t getFreeHeap() const { return 100000; }
};
inline TestESP ESP;
