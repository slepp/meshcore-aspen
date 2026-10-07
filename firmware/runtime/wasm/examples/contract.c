// SPDX-License-Identifier: Apache-2.0
#include "../sdk/meshcore.h"
#define ADDRESS(p) ((uint32_t)(uintptr_t)(p))
static unsigned phase;
static char output[2048];
static uint32_t number;
MC_EXPORT("mc_init") int32_t init(void) {
  mc_command(1, "wcontract", 9, "", 0, "Shared runtime contract fixture", 31, MC_PUBLIC);
  return MC_ABI_VERSION;
}
static int32_t request(uint32_t job) {
#if CONTRACT == 0
  static mc_mutation operations[2] = {
    {ADDRESS("a"), 1, ADDRESS(""), 0, ADDRESS("one"), 3, MC_COMPARE},
    {ADDRESS("b"), 1, ADDRESS(""), 0, ADDRESS("two"), 3, 0}
  };
  return mc_io(job, phase ? MC_TRANSACTION : MC_CAS, MC_CALLER, "", 0,
               (const char *)operations, (phase ? 2 : 1) * sizeof(mc_mutation), 0);
#elif CONTRACT == 1
  return mc_io(job, phase == 0 ? MC_REMINDER_SET : phase == 1 ? MC_REMINDER_LIST : MC_REMINDER_CANCEL,
               MC_CALLER, "", 0, phase ? "" : "later", phase ? 0 : 5, phase == 2 ? number : 30);
#elif CONTRACT == 2
  static mc_mesh_options options;
  return mc_io(job, phase == 0 ? MC_SEND : phase == 1 ? MC_WAIT : MC_FORWARD, MC_CALLER,
               phase == 2 ? "" : (const char *)&options, phase == 2 ? 0 : sizeof(options),
               phase == 0 ? "hello" : "", phase == 0 ? 5 : 0, phase == 2 ? number : 5000);
#elif CONTRACT == 3
  return mc_io(job, MC_UTILITY, MC_CALLER, "calc", 4, "6*7", 3, 0);
#elif CONTRACT == 4
  return mc_io(job, MC_HTTP_POST, MC_CALLER, "echo", 4, "{\"value\":42}", 12, 0);
#elif CONTRACT == 5
  static mc_rpc rpc = {ADDRESS("home"), 4, ADDRESS("echo"), 4,
                       ADDRESS("{\"text\":\"hello\"}"), 16};
  return mc_io(job, MC_RPC, MC_CALLER, (const char *)&rpc, sizeof(rpc), "", 0, 0);
#elif CONTRACT == 6
  static uint8_t route[] = {0xab, 0xcd};
  static mc_mesh_options options = {.route = ADDRESS(route), .route_len = 2, .route_width = 1};
  return mc_io(job, phase == 0 ? MC_TRACE : phase == 1 ? MC_INSPECT : phase == 2 ? MC_ADVERT : MC_ADMIN,
               MC_CALLER, phase == 0 ? (const char *)&options : phase == 1 ? "neighbors" : "",
               phase == 0 ? sizeof(options) : phase == 1 ? 9 : 0,
               phase == 3 ? "status" : "", phase == 3 ? 6 : 0, phase == 1 ? 2 : 0);
#elif CONTRACT == 7
  static mc_convert conversion = {ADDRESS("100"), 3, ADDRESS("C"), 1, ADDRESS("F"), 1};
  return mc_io(job, MC_UTILITY, MC_CALLER, phase == 0 ? "convert" : phase == 1 ? "roll" : "choose",
               phase == 0 ? 7 : phase == 1 ? 4 : 6,
               phase == 0 ? (const char *)&conversion : phase == 1 ? "1d2" : "red|blue",
               phase == 0 ? sizeof(conversion) : phase == 1 ? 3 : 8, 0);
#elif CONTRACT == 8
  static char key[33], a[33], b[33];
  int32_t size = mc_thread_key(key, "notes", 5, phase == 4 ? "" : phase >= 5 ? "wake" : "key",
                               phase == 4 ? 0 : phase >= 5 ? 4 : 3);
  if (size < 0 || mc_thread_key(a, "notes", 5, "a", 1) < 0 ||
      mc_thread_key(b, "notes", 5, "b", 1) < 0) return -1;
  if (phase == 2 || phase == 3) {
    static mc_mutation operations[2] = {
      {ADDRESS(a), 7, ADDRESS(""), 0, ADDRESS("one"), 3, MC_COMPARE},
      {ADDRESS(b), 7, ADDRESS(""), 0, ADDRESS("two"), 3, 0}
    };
    return mc_io(job, phase == 2 ? MC_CAS : MC_TRANSACTION, MC_CALLER_THREAD, "", 0,
                 (const char *)operations, (phase == 2 ? 1 : 2) * sizeof(mc_mutation), 0);
  }
  return mc_io(job, phase == 0 ? MC_GET : phase == 1 ? MC_PUT : phase == 4 ? MC_LIST :
               phase == 5 ? MC_TIMER_SET : phase == 6 ? MC_TIMER_GET : MC_TIMER_CANCEL,
               MC_CALLER_THREAD, key, (uint32_t)size, phase == 1 ? "v" : "",
               phase == 1 ? 1 : 0, phase == 5 ? 5 : 0);
#elif CONTRACT == 9
  static mc_mutation operations[2] = {
    {ADDRESS("notes/a"), 7, ADDRESS(""), 0, ADDRESS("one"), 3, 0},
    {ADDRESS("other/b"), 7, ADDRESS(""), 0, ADDRESS("two"), 3, 0}
  };
  return mc_io(job, MC_TRANSACTION, MC_CALLER_THREAD, "", 0,
               (const char *)operations, sizeof(operations), 0);
#else
  return -1;
#endif
}
MC_EXPORT("mc_start") int32_t start(uint32_t job, uint32_t id) {
  phase = 0;
  return request(job) > 0 ? MC_PENDING : MC_DONE;
}
MC_EXPORT("mc_resume") int32_t resume(uint32_t job, uint32_t operation, uint32_t ok) {
#if CONTRACT == 0
  if (mc_read(operation, MC_OUTCOME, &number, sizeof(number)) != 4) return -1;
  if (++phase < 2) return request(job) > 0 ? MC_PENDING : -1;
  mc_reply(job, number == 1 ? "committed" : number == 2 ? "conflict" : number == 3 ? "unknown" : "rejected",
           number == 1 ? 9 : number == 2 ? 8 : number == 3 ? 7 : 8);
#elif CONTRACT == 1
  if (!phase && mc_read(operation, MC_REVISION, &number, 4) != 4) return -1;
  if (++phase < 3) return request(job) > 0 ? MC_PENDING : -1;
  mc_read(operation, MC_REMINDER_STATE, &number, 4);
  mc_reply(job, number == 4 ? "cancelled" : "unknown", number == 4 ? 9 : 7);
#elif CONTRACT == 2
  if (phase == 1) {
    mc_packet packet;
    if (mc_read(operation, MC_PACKET, &packet, sizeof(packet)) != sizeof(packet)) return -1;
    number = packet.handle;
  }
  if (++phase < 3) return request(job) > 0 ? MC_PENDING : -1;
  mc_read(operation, MC_TRANSMITTED, &number, 4);
  mc_reply(job, number ? "transmitted" : "unknown", number ? 11 : 7);
#elif CONTRACT == 3
  int32_t length = mc_read(operation, ok ? MC_VALUE : MC_ERROR, output, 148);
  if (length <= 0) return -1;
  mc_reply(job, output, length);
#elif CONTRACT == 4 || CONTRACT == 5
  if (mc_read(operation, MC_JSON, output, sizeof(output)) < 0) return -1;
  if (ok) { mc_reply(job, "ok", 2); return MC_DONE; }
  int32_t length = mc_read(operation, MC_RPC_CODE, output, 40);
  if (length <= 0) return -1;
  mc_reply(job, output, length);
#elif CONTRACT == 6
  if (!ok) return -1;
  if (!phase) {
    mc_trace_result trace;
    if (mc_read(operation, MC_TRACE_RESULT, &trace, sizeof(trace)) != sizeof(trace) ||
        trace.width != 1 || trace.count != 2) return -1;
  }
  if (++phase < 4) return request(job) > 0 ? MC_PENDING : -1;
  mc_reply(job, "mesh complete", 13);
#elif CONTRACT == 7
  if (!ok) {
    int32_t length = mc_read(operation, MC_ERROR, output, sizeof(output));
    if (length > 0) mc_reply(job, output, length);
    return MC_DONE;
  }
  if (mc_read(operation, MC_VALUE, output, sizeof(output)) <= 0) return -1;
  if (++phase < 3) return request(job) > 0 ? MC_PENDING : -1;
  mc_reply(job, "utilities complete", 18);
#elif CONTRACT == 8
  if (!ok) return -1;
  if (++phase < 8) return request(job) > 0 ? MC_PENDING : -1;
  mc_reply(job, "thread complete", 15);
#endif
  return MC_DONE;
}
