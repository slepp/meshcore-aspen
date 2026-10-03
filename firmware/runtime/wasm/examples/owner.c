// SPDX-License-Identifier: Apache-2.0
#include "../sdk/meshcore.h"
MC_EXPORT("mc_init") int32_t init(void) {
  mc_command(1, "wowner", 6, "", 0, "Native owner authority fixture", 30, MC_OWNER);
  return MC_ABI_VERSION;
}
MC_EXPORT("mc_start") int32_t start(uint32_t job, uint32_t command) {
  mc_reply(job, "Wasm owner", 10);
  return MC_DONE;
}
MC_EXPORT("mc_resume") int32_t resume(uint32_t job, uint32_t operation, uint32_t ok) {
  return -1;
}
