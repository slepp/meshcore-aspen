// SPDX-License-Identifier: Apache-2.0
#include "../sdk/meshcore.h"
#define ADDRESS(p) ((uint32_t)(uintptr_t)(p))
static char args[2048], result[148];
static mc_rpc rpc;
MC_EXPORT("mc_init") int32_t init(void) {
  mc_command(1, "warg_echo", 9, "json:text:150", 13, "Configured home echo arguments", 29, MC_PUBLIC);
  mc_command(2, "warg_weather", 12, "json:text:150", 13, "Configured home weather arguments", 32, MC_PUBLIC);
  mc_command(3, "warg_health", 11, "json:text:150", 13, "Configured home health arguments", 31, MC_PUBLIC);
  mc_command(4, "warg_repeat", 11, "size:int:1:300", 14, "Echo boundary", 13, MC_PUBLIC);
  return MC_ABI_VERSION;
}
MC_EXPORT("mc_start") int32_t start(uint32_t job, uint32_t id) {
  int32_t size = mc_read(job, MC_ARGUMENTS, args, sizeof(args));
  if (size <= 0) return -1;
  if (id == 4) {
    uint32_t count;
    if (mc_read(job, MC_ARG0, &count, sizeof(count)) != 4 || count > 300) return -1;
    const char prefix[] = "{\"text\":\"";
    for (unsigned i = 0; i < sizeof(prefix) - 1; ++i) args[i] = prefix[i];
    for (unsigned i = 0; i < count; ++i) args[sizeof(prefix) - 1 + i] = 'x';
    size = sizeof(prefix) - 1 + count;
    args[size++] = '"'; args[size++] = '}';
  }
  rpc.service = ADDRESS("home"); rpc.service_len = 4;
  rpc.operation = ADDRESS(id == 1 || id == 4 ? "echo" : id == 2 ? "weather" : "health");
  rpc.operation_len = id == 1 || id == 4 ? 4 : id == 2 ? 7 : 6;
  rpc.args = ADDRESS(args); rpc.args_len = size;
  return mc_io(job, MC_RPC, MC_CALLER, (const char *)&rpc, sizeof(rpc), "", 0, 0) > 0 ?
      MC_PENDING : -1;
}
MC_EXPORT("mc_resume") int32_t resume(uint32_t job, uint32_t operation, uint32_t ok) {
  if (ok) { mc_reply(job, "ok", 2); return MC_DONE; }
  int32_t size = mc_read(operation, MC_RPC_CODE, result, sizeof(result));
  if (size <= 0) return -1;
  mc_reply(job, result, size);
  return MC_DONE;
}
