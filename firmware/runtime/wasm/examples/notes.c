// SPDX-License-Identifier: Apache-2.0
#include "../sdk/meshcore.h"
static char value[151];
MC_EXPORT("mc_init") int32_t init(void) {
  mc_command(1, "wnote", 5, "text?:text:150", 14, "Read or save your note", 22, MC_PRIVATE);
  return MC_ABI_VERSION;
}
MC_EXPORT("mc_start") int32_t start(uint32_t job, uint32_t command) {
  (void)command;
  int32_t size = mc_read(job, MC_ARGUMENTS, value, 150);
  mc_io(job, size ? MC_PUT : MC_GET, MC_CALLER, "wasm-note", 9, value, size, 0);
  return MC_PENDING;
}
MC_EXPORT("mc_resume") int32_t resume(uint32_t job, uint32_t operation, uint32_t ok) {
  uint32_t kind = 0, found = 0;
  mc_read(operation, MC_OPERATION_KIND, &kind, 4);
  mc_read(operation, MC_FOUND, &found, 4);
  int32_t size = mc_read(operation, ok ? MC_VALUE : MC_ERROR, value, 150);
  if (!ok) mc_reply(job, size ? value : "Storage failed", size ? size : 14);
  else if (kind == MC_PUT) mc_reply(job, "Note saved", 10);
  else if (!found) mc_reply(job, "No note saved", 13);
  else mc_reply(job, value, size);
  return MC_DONE;
}
