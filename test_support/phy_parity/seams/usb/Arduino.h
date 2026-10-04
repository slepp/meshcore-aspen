#pragma once
#include "../../../../firmware/esp32/tests/seams/Stream.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <string>
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
inline char* ltoa(long value, char* buffer, int radix) {
  if (radix != 10) std::abort();
  std::sprintf(buffer, "%ld", value);
  return buffer;
}
class CapturedSerial : public Stream {
public:
  std::deque<uint8_t> input;
  std::vector<uint8_t> output;
  int available() override { return input.size(); }
  int read() override {
    if (input.empty()) return -1;
    int value = input.front();
    input.pop_front();
    return value;
  }
  using Stream::write;
  size_t write(const uint8_t* data, size_t size) override {
    output.insert(output.end(), data, data + size);
    return size;
  }
};
inline CapturedSerial Serial;
