// SPDX-License-Identifier: Apache-2.0
#include "../sdk/meshcore.h"
MC_EXPORT("mc_init") int32_t init(void) {
  mc_command(1, "wwait", 5, "", 0, "Wait for a caller message", 25, MC_PRIVATE);
  mc_command(2, "wsleep", 6, "", 0, "Sleep before replying", 21, MC_PUBLIC);
  return MC_ABI_VERSION;
}
MC_EXPORT("mc_start") int32_t start(uint32_t job, uint32_t handler) {
  static mc_mesh_options options;
  return mc_io(job, handler == 1 ? MC_WAIT : MC_SLEEP, MC_CALLER,
               handler == 1 ? (const char *)&options : "",
               handler == 1 ? sizeof(options) : 0, "", 0,
               handler == 1 ? 5000 : 500) > 0 ? MC_PENDING : -1;
}
MC_EXPORT("mc_resume") int32_t resume(uint32_t job, uint32_t operation, uint32_t ok) {
  if (!ok) return -1;
  return mc_reply(job, "Wasm completed", 14) >= 0 ? MC_DONE : -1;
}
