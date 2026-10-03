// SPDX-License-Identifier: Apache-2.0
#include "../sdk/meshcore.h"
static char text[163];
MC_EXPORT("mc_init") int32_t init(void) {
  if (mc_command(1, "wguard", 6, "text:text:155", 13, "Native owner administration fixture", 35, MC_OWNER) < 0)
    return -1;
  return MC_ABI_VERSION;
}
MC_EXPORT("mc_start") int32_t start(uint32_t job, uint32_t command) {
  mc_authority authority;
  if (mc_read(job, MC_AUTHORITY, &authority, sizeof(authority)) != sizeof(authority) ||
      !authority.owner || !authority.authenticated || authority.local)
    return -1;
  int32_t length = mc_read(job, MC_ARGUMENTS, text, sizeof(text) - 1);
  if (length <= 0 || length >= sizeof(text)) return -1;
  return mc_io(job, MC_ADMIN, MC_CALLER, "", 0, text, length, 0) > 0 ? MC_PENDING : -1;
}
MC_EXPORT("mc_resume") int32_t resume(uint32_t job, uint32_t operation, uint32_t ok) {
  int32_t length = mc_read(operation, ok ? MC_VALUE : MC_ERROR, text, sizeof(text));
  if (length <= 0) return -1;
  return mc_reply(job, text, length) < 0 ? -1 : MC_DONE;
}
