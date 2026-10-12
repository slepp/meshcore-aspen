/* SPDX-License-Identifier: Apache-2.0 */
#ifndef HLUA_SESSION_H
#define HLUA_SESSION_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Session owns every Lua value. Returned bytes are borrowed until the next
   mutating call. A Session must only be used by one thread at a time. */
typedef struct HluaSession HluaSession;
typedef struct HluaLimits {
  uint64_t heap_bytes, instructions, parser_steps, active_us;
  uint32_t source_bytes, value_bytes, host_calls;
} HluaLimits;
enum HluaStatus { HLUA_READY=0, HLUA_COMPLETE=1, HLUA_HOST=2, HLUA_ERROR=3 };
enum HluaKind { HLUA_NIL=0, HLUA_BOOL=1, HLUA_INTEGER=2, HLUA_NUMBER=3, HLUA_BYTES=4 };
typedef struct HluaValue {
  int kind;
  int64_t integer;
  double number;
  const uint8_t *bytes;
  size_t size;
} HluaValue;
HluaSession *hlua_open(HluaLimits limits);
void hlua_close(HluaSession *session);
int hlua_status(const HluaSession *session);
int hlua_load(HluaSession *session, const uint8_t *source, size_t size);
/* Set up to eight copied scalar arguments before call or resume. */
int hlua_prepare_arguments(HluaSession *session, uint64_t token, int resume);
int hlua_argument(HluaSession *session, HluaValue value);
int hlua_call(HluaSession *session, const uint8_t *name, size_t size);
/* The current host request token must match; cancellation invalidates it. */
int hlua_resume(HluaSession *session, uint64_t token);
void hlua_cancel(HluaSession *session);
uint64_t hlua_token(const HluaSession *session);
size_t hlua_count(const HluaSession *session);
HluaValue hlua_value(const HluaSession *session, size_t index);
const uint8_t *hlua_operation(const HluaSession *session, size_t *size);
const char *hlua_error(const HluaSession *session);
size_t hlua_peak(const HluaSession *session);
uint64_t hlua_instructions(const HluaSession *session);
uint64_t hlua_parser_steps(const HluaSession *session);
#ifdef __cplusplus
}
#endif
#endif
