#pragma once
#include <Stream.h>
#include <cstdio>
#include <cstdlib>

unsigned long millis();
struct LinkSerial {
  template<typename... Args> void printf(const char* fmt, Args... args) {
    std::fprintf(stderr, fmt, args...);
  }
  void println(const char* text) { std::fprintf(stderr, "%s\n", text); }
};
inline LinkSerial Serial;
