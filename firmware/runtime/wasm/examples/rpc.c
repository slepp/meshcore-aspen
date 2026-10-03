// SPDX-License-Identifier: Apache-2.0
#include "../sdk/meshcore.h"
static char value[151];
MC_EXPORT("mc_init") int32_t init(void) {
  mc_command(1, "wecho", 5, "text:text:150", 13, "Echo through configured home service", 36, MC_HOME);
  return MC_ABI_VERSION;
}
MC_EXPORT("mc_start") int32_t start(uint32_t job, uint32_t command) {
  (void)command;
  int32_t size = mc_read(job, MC_ARG0, value, 150);
  mc_io(job, MC_RPC, MC_CALLER, "echo", 4, value, size, 0);
  return MC_PENDING;
}
MC_EXPORT("mc_resume") int32_t resume(uint32_t job, uint32_t operation, uint32_t ok) {
  int32_t size = mc_read(operation, ok ? MC_VALUE : MC_ERROR, value, 150);
  mc_reply(job, value, size); return MC_DONE;
}
