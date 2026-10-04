#pragma once
#include <Utils.h>

class RecordedRNG : public mesh::RNG {
public:
  uint32_t last_min = 0, last_max = 0;
  uint32_t nextInt(uint32_t min, uint32_t max) {
    last_min = min;
    last_max = max;
    return mesh::RNG::nextInt(min, max);
  }
};
