// SPDX-License-Identifier: Apache-2.0
#include "../sdk/meshcore.h"
MC_EXPORT("mc_init") int32_t init(void) {
  mc_subscribe(1, 1); mc_subscribe(4, 2); return MC_ABI_VERSION;
}
MC_EXPORT("mc_start") int32_t start(uint32_t job, uint32_t handler) {
  uint64_t uptime = 0;
  mc_read(job, MC_UPTIME, &uptime, 8);
  if (uptime != (((uint64_t)1 << 40) + 2)) return -1;
  mc_io(job, MC_SLEEP, MC_BOT, "", 0, "", 0, 1);
  (void)handler; return MC_PENDING;
}
MC_EXPORT("mc_resume") int32_t resume(uint32_t job, uint32_t operation, uint32_t ok) {
  uint32_t kind = 0;
  mc_read(operation, MC_OPERATION_KIND, &kind, 4);
  if (!ok) return MC_DONE;
  if (kind == MC_SLEEP) {
    mc_io(job, MC_GET, MC_BOT, "event-value", 11, "", 0, 0);
    return MC_PENDING;
  }
  return MC_DONE;
}
