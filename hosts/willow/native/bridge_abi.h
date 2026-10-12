#ifndef WILLOW_BRIDGE_ABI_H
#define WILLOW_BRIDGE_ABI_H
#include <stdint.h>

/* Hew bytes ABI: borrowed inputs, runtime-allocated returned buffers. */
typedef struct { uint8_t *ptr; uint32_t offset, len; } Bytes;
extern uint8_t *hew_bytes_new(uint32_t capacity);
#endif
