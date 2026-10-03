#pragma once
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cctype>
#include "Stream.h"
#include "FS.h"
using std::min;
using std::max;
unsigned long millis();
long random(long min, long max);
void randomSeed(long);
inline char* ltoa(long value, char* output, int radix) {
  if (radix != 10) std::abort();
  std::sprintf(output, "%ld", value);
  return output;
}
