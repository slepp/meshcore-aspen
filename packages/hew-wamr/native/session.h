/* SPDX-License-Identifier: Apache-2.0 */
#ifndef HWAMR_SESSION_H
#define HWAMR_SESSION_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct HwamrInstance HwamrInstance;
typedef struct HwamrLimits {
  uint32_t source_bytes, memory_pages, stack_bytes, instructions;
  uint64_t active_us;
} HwamrLimits;
enum HwamrKind { HWAMR_I32=0, HWAMR_I64=1, HWAMR_F32=2, HWAMR_F64=3 };
typedef struct HwamrValue { int kind; int64_t integer; double number; } HwamrValue;
/* Pool selection is immutable after configuration or first use. */
int hwamr_pool(uint32_t bytes);
const char *hwamr_pool_error(void);
HwamrInstance *hwamr_open(const uint8_t *bytes, size_t size, HwamrLimits limits);
void hwamr_close(HwamrInstance *instance);
/* Numeric arguments are copied before synchronous execution. */
int hwamr_arguments(HwamrInstance *instance);
int hwamr_argument(HwamrInstance *instance, HwamrValue value);
int hwamr_call(HwamrInstance *instance, const uint8_t *name, size_t size);
size_t hwamr_count(const HwamrInstance *instance);
HwamrValue hwamr_value(const HwamrInstance *instance, size_t index);
int hwamr_readable(HwamrInstance *instance, uint64_t offset, size_t size);
int hwamr_read(HwamrInstance *instance, uint64_t offset, uint8_t *out, size_t size);
int hwamr_write(HwamrInstance *instance, uint64_t offset, const uint8_t *bytes, size_t size);
uint64_t hwamr_memory_size(HwamrInstance *instance);
uint32_t hwamr_instructions(const HwamrInstance *instance);
const char *hwamr_error(const HwamrInstance *instance);
/* Permanently retire execution. Native callers may cancel concurrently with a
   call while retaining instance lifetime; close/read/write/call are serialized
   by the owner. No concurrent close is permitted. */
void hwamr_cancel(HwamrInstance *instance);
#ifdef __cplusplus
}
#endif
#endif
