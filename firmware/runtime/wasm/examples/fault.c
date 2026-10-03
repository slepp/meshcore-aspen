// SPDX-License-Identifier: Apache-2.0
#include "../sdk/meshcore.h"
#ifndef FAULT
#define FAULT 0
#endif
static volatile uint32_t count;
MC_EXPORT("mc_init") int32_t init(void) {
#if FAULT == 9
  for (;;) {}
#endif
  mc_command(1, "wfault", 6, "", 0, "Containment test", 16, MC_PUBLIC);
  return MC_ABI_VERSION;
}
__attribute__((noinline)) static int32_t recursive(uint32_t n) {
  count += n; return recursive(n + 1) + count;
}
MC_EXPORT("mc_start") int32_t start(uint32_t job, uint32_t command) {
  (void)command;
#if FAULT == 0
  for (;;) ++count;
#elif FAULT == 1
  mc_reply(job + 100, "bad", 3);
#elif FAULT == 2
  mc_reply(job, (const char *)65535, 9);
#elif FAULT == 3
  return recursive(1);
#elif FAULT == 4
  uint32_t previous = __builtin_wasm_memory_grow(0, 1);
  mc_reply(job, previous == (uint32_t)-1 ? "growth rejected" : "BAD growth", previous == (uint32_t)-1 ? 15 : 10);
#elif FAULT == 5
  for (unsigned i = 0; i < 80; ++i) mc_read(job, MC_ARGUMENTS, (void *)&count, 4);
  mc_reply(job, "BAD native limit", 16);
#elif FAULT == 6
  *(volatile uint32_t *)65535 = 1;
#elif FAULT == 7
  mc_io(job, MC_SLEEP, 0, "", 0, "", 0, 1);
  return MC_PENDING;
#elif FAULT == 8
  for (;;) {}
#endif
  return MC_DONE;
}
MC_EXPORT("mc_resume") int32_t resume(uint32_t job, uint32_t operation, uint32_t ok) {
  (void)operation; (void)ok;
#if FAULT == 7
  mc_io(job, MC_SLEEP, 0, "", 0, "", 0, 1); return MC_PENDING;
#else
  mc_reply(job, "finished", 8); return MC_DONE;
#endif
}
