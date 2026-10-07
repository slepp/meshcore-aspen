// SPDX-License-Identifier: Apache-2.0
#ifndef MESHCORE_WASM_V1_H
#define MESHCORE_WASM_V1_H
#include <stdint.h>
// All addresses designate bounded guest linear-memory ranges, never host pointers.
#ifdef __wasm__
#define MC_IMPORT(name) __attribute__((import_module("meshcore_v1"), import_name(name)))
#else
#define MC_IMPORT(name)
#endif
#define MC_EXPORT(name) __attribute__((export_name(name)))
enum mc_field {
  MC_ARGUMENTS = 0, MC_SENDER = 1, MC_CHANNEL_KEY = 2, MC_TIMESTAMP = 3,
  MC_UPTIME = 4, MC_EVENT_KIND = 5, MC_MESSAGE = 6,
  MC_NICKNAME = 7, MC_PATH = 8, MC_SIGNAL = 9, MC_NODE = 10, MC_AIR = 11,
  MC_ARG_TYPES = 12, MC_AUTHORITY = 13,
  MC_ARG0 = 16, MC_ARG1 = 17, MC_ARG2 = 18, MC_ARG3 = 19,
  MC_VALUE = 32, MC_ERROR = 33, MC_FOUND = 34, MC_HTTP_STATUS = 35,
  MC_RPC_CODE = 36, MC_OUTCOME = 37, MC_TIMER_STATE = 38, MC_REVISION = 39,
  MC_DEADLINE_UTC = 40, MC_TIME_TRUSTED = 41, MC_KEYS_COUNT = 42,
  MC_OPERATION_KIND = 43, MC_COUNTRY = 44, MC_TEMPERATURE_C = 45,
  MC_WEATHER_CODE = 46, MC_SOURCE = 47, MC_OBSERVED_AT = 48,
  MC_SOURCE_AGE = 49, MC_JSON = 50, MC_NETWORK_SUBMITTED = 51,
  MC_REMINDER_STATE = 52, MC_PENDING_STATE = 53, MC_REPLACED = 54,
  MC_RADIO_JOB = 55, MC_QUEUED = 56, MC_TRANSMITTED = 57,
  MC_ACKNOWLEDGED = 58, MC_TRUNCATED = 59, MC_PACKET = 60, MC_TRACE_RESULT = 61,
  MC_KEY0 = 64
};
enum mc_permission { MC_PUBLIC, MC_PRIVATE, MC_OWNER, MC_CHANNEL, MC_SHARED, MC_REMINDER, MC_HOME };
enum mc_scope { MC_CALLER, MC_CONVERSATION, MC_BOT, MC_CHANNEL_SCOPE,
                MC_CALLER_THREAD, MC_CONVERSATION_THREAD, MC_BOT_THREAD, MC_CHANNEL_THREAD };
static inline int32_t mc_thread_key(char out[33], const char *thread, uint32_t thread_len,
                                    const char *key, uint32_t key_len) {
  if (!out || !thread || !thread_len || thread_len > 24 ||
      key_len > 31 - thread_len || (!key && key_len) ||
      thread[0] < 'a' || thread[0] > 'z') return -1;
  for (uint32_t i = 0; i < thread_len; ++i) {
    const char c = thread[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return -1;
    out[i] = c;
  }
  out[thread_len] = '/';
  for (uint32_t i = 0; i < key_len; ++i) {
    if (!key[i]) return -1;
    out[thread_len + 1 + i] = key[i];
  }
  out[thread_len + 1 + key_len] = 0;
  return (int32_t)(thread_len + 1 + key_len);
}
enum mc_io {
  MC_SLEEP = 0, MC_GET = 1, MC_PUT = 2, MC_DELETE = 3,
  MC_SEND = 4, MC_WAIT = 5, MC_TRACE = 6, MC_ADVERT = 7, MC_RPC = 8,
  MC_TIMER_SET = 9, MC_TIMER_GET = 10, MC_TIMER_CANCEL = 11, MC_TIMER_WAIT = 12,
  MC_FORWARD = 13, MC_REMINDER_SET = 14, MC_REMINDER_LIST = 15,
  MC_REMINDER_CANCEL = 16, MC_LIST = 17, MC_CAS = 18, MC_TRANSACTION = 19,
  MC_INSPECT = 20, MC_ADMIN = 21, MC_HTTP_GET = 22, MC_HTTP_POST = 23,
  MC_UTILITY = 25
};
enum { MC_COMPARE = 1, MC_PRESENT = 2, MC_REMOVE = 4 };
typedef struct {
  uint32_t key, key_len, expected, expected_len, value, value_len, flags;
} mc_mutation;
typedef struct {
  uint32_t destination, route, route_len, route_width, packet_kind, wait_kind;
  uint32_t exact, path_width, route_type, trace_send_only;
} mc_mesh_options;
typedef struct { uint32_t value, value_len, from, from_len, to, to_len; } mc_convert;
typedef struct {
  uint32_t service, service_len, operation, operation_len, args, args_len;
} mc_rpc;
typedef struct { uint32_t width, count, known; uint8_t bytes[64]; } mc_path;
typedef struct { uint32_t available; float rssi, snr; } mc_signal;
typedef struct {
  uint32_t authenticated, channel_verified, targeted, owner, shared, home, reminders, local;
} mc_authority;
typedef struct {
  uint32_t available, identity, enabled, ready, fault, roles_known, wifi_known, wifi_connected;
  uint32_t selected_roles, ready_roles, generation;
  uint64_t uptime;
  uint8_t public_key[32];
  char revision[13], build[24], name[33];
} mc_node;
typedef struct {
  uint32_t available, transmitting, queued, aggregate_queued, generation, configuration_generation;
  uint32_t captured_ms, credit_ms, aggregate_credit_ms, rf_ms, aggregate_rf_ms;
  uint32_t successes, failures, aggregate_successes, aggregate_failures, reserved_ms, limit_ms, remaining_ms;
} mc_air;
typedef struct {
  uint32_t handle, timestamp, route_type, authenticated, forwardable, channel;
  mc_path path;
  mc_signal signal;
  uint8_t sender[32];
  char nickname[33];
} mc_packet;
typedef struct { uint32_t width, count; int8_t snr[64]; } mc_trace_result;
#ifdef __cplusplus
static_assert(sizeof(mc_node) == 160 && sizeof(mc_packet) == 180,
              "ABI v1 copied records require fixed wire sizes");
#endif
enum { MC_DONE = 0, MC_PENDING = 1, MC_ABI_VERSION = 1 };
MC_IMPORT("command") int32_t mc_command(uint32_t id, const char *name, uint32_t name_len,
    const char *schema, uint32_t schema_len, const char *help, uint32_t help_len, uint32_t permission);
MC_IMPORT("subscribe") int32_t mc_subscribe(uint32_t kind, uint32_t id);
MC_IMPORT("read") int32_t mc_read(uint32_t handle, uint32_t field, void *out, uint32_t capacity);
MC_IMPORT("reply") int32_t mc_reply(uint32_t job, const char *text, uint32_t length);
MC_IMPORT("io") int32_t mc_io(uint32_t job, uint32_t kind, uint32_t scope,
    const char *key, uint32_t key_len, const char *value, uint32_t value_len, uint32_t delay);
// Required exports: mc_init()->i32 ABI version, mc_start(job,id)->i32 disposition,
// mc_resume(job,operation,ok)->i32 disposition. State machines must return PENDING
// after exactly one queued I/O; only its completion may invoke mc_resume.
#endif
