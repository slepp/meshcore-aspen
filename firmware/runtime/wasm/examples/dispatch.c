// SPDX-License-Identifier: Apache-2.0
#include "../sdk/meshcore.h"
MC_EXPORT("mc_init") int32_t init(void) {
  mc_subscribe(1, 2);
  mc_subscribe(4, 2);
  mc_command(1, "wseen", 5, "", 0, "Read Wasm event marker", 22, MC_PUBLIC);
  return MC_ABI_VERSION;
}
MC_EXPORT("mc_start") int32_t start(uint32_t job, uint32_t handler) {
  mc_io(job, handler == 2 ? MC_SLEEP : MC_GET, MC_BOT,
        handler == 2 ? "" : "wasm-events", handler == 2 ? 0 : 11, "", 0, handler == 2 ? 20 : 0);
  return MC_PENDING;
}
MC_EXPORT("mc_resume") int32_t resume(uint32_t job, uint32_t operation, uint32_t ok) {
  uint32_t kind;
  mc_read(operation, MC_OPERATION_KIND, &kind, 4);
  if (!ok) return -1;
  if (kind == MC_SLEEP) {
    mc_io(job, MC_PUT, MC_BOT, "wasm-events", 11, "seen", 4, 0);
    return MC_PENDING;
  }
  if (kind == MC_GET) {
    char text[32]; int32_t length = mc_read(operation, MC_VALUE, text, sizeof(text));
    mc_reply(job, length ? text : "missing", length ? length : 7);
  }
  return MC_DONE;
}
