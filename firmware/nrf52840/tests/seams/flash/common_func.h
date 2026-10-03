#pragma once
#include <stdint.h>
static inline uint32_t min32(uint32_t a, uint32_t b) { return a < b ? a : b; }
#define minof(a, b) ((a) < (b) ? (a) : (b))
