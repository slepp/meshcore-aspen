#pragma once
#include <Stream.h>
#include <cmath>
#include <cstdio>

unsigned long millis();
void delay(unsigned long);
class SerialSink {
public:
  template <typename... Args> void printf(const char*, Args...) {}
  void println(const char*) {}
};
inline SerialSink Serial;
