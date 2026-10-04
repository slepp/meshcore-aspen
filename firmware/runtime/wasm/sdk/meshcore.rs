// SPDX-License-Identifier: Apache-2.0
#![allow(dead_code)]
pub const ARGUMENTS: u32 = 0;
pub const SENDER: u32 = 1;
pub const CHANNEL_KEY: u32 = 2;
pub const TIMESTAMP: u32 = 3;
pub const UPTIME: u32 = 4;
pub const EVENT_KIND: u32 = 5;
pub const MESSAGE: u32 = 6;
pub const NICKNAME: u32 = 7;
pub const PATH: u32 = 8;
pub const SIGNAL: u32 = 9;
pub const NODE: u32 = 10;
pub const AIR: u32 = 11;
pub const ARG_TYPES: u32 = 12;
pub const AUTHORITY: u32 = 13;
pub const ARG0: u32 = 16;
pub const ARG1: u32 = 17;
pub const ARG2: u32 = 18;
pub const ARG3: u32 = 19;
pub const VALUE: u32 = 32;
pub const ERROR: u32 = 33;
pub const FOUND: u32 = 34;
pub const HTTP_STATUS: u32 = 35;
pub const RPC_CODE: u32 = 36;
pub const OUTCOME: u32 = 37;
pub const TIMER_STATE: u32 = 38;
pub const REVISION: u32 = 39;
pub const DEADLINE_UTC: u32 = 40;
pub const TIME_TRUSTED: u32 = 41;
pub const KEYS_COUNT: u32 = 42;
pub const OPERATION_KIND: u32 = 43;
pub const COUNTRY: u32 = 44;
pub const TEMPERATURE_C: u32 = 45;
pub const WEATHER_CODE: u32 = 46;
pub const SOURCE: u32 = 47;
pub const OBSERVED_AT: u32 = 48;
pub const SOURCE_AGE: u32 = 49;
pub const JSON: u32 = 50;
pub const NETWORK_SUBMITTED: u32 = 51;
pub const REMINDER_STATE: u32 = 52;
pub const PENDING_STATE: u32 = 53;
pub const REPLACED: u32 = 54;
pub const RADIO_JOB: u32 = 55;
pub const QUEUED: u32 = 56;
pub const TRANSMITTED: u32 = 57;
pub const ACKNOWLEDGED: u32 = 58;
pub const TRUNCATED: u32 = 59;
pub const PACKET: u32 = 60;
pub const TRACE_RESULT: u32 = 61;
pub const KEY0: u32 = 64;
pub const SLEEP: u32 = 0;
pub const GET: u32 = 1;
pub const PUT: u32 = 2;
pub const DELETE: u32 = 3;
pub const SEND: u32 = 4;
pub const WAIT: u32 = 5;
pub const TRACE: u32 = 6;
pub const ADVERT: u32 = 7;
pub const RPC: u32 = 8;
pub const TIMER_SET: u32 = 9;
pub const TIMER_GET: u32 = 10;
pub const TIMER_CANCEL: u32 = 11;
pub const TIMER_WAIT: u32 = 12;
pub const LIST: u32 = 17;
pub const FORWARD: u32 = 13;
pub const REMINDER_SET: u32 = 14;
pub const REMINDER_LIST: u32 = 15;
pub const REMINDER_CANCEL: u32 = 16;
pub const CAS: u32 = 18;
pub const TRANSACTION: u32 = 19;
pub const INSPECT: u32 = 20;
pub const ADMIN: u32 = 21;
pub const HTTP_GET: u32 = 22;
pub const HTTP_POST: u32 = 23;
pub const UTILITY: u32 = 25;
pub const COMPARE: u32 = 1;
pub const PRESENT: u32 = 2;
pub const REMOVE: u32 = 4;
#[repr(C)]
pub struct Mutation {
    pub key: u32, pub key_len: u32, pub expected: u32, pub expected_len: u32,
    pub value: u32, pub value_len: u32, pub flags: u32,
}
#[repr(C)]
pub struct MeshOptions {
    pub destination: u32, pub route: u32, pub route_len: u32, pub route_width: u32,
    pub packet_kind: u32, pub wait_kind: u32, pub exact: u32, pub path_width: u32,
    pub route_type: u32, pub trace_send_only: u32,
}
#[repr(C)]
pub struct Convert {
    pub value: u32, pub value_len: u32, pub from: u32, pub from_len: u32,
    pub to: u32, pub to_len: u32,
}
#[repr(C)]
pub struct Rpc {
    pub service: u32, pub service_len: u32, pub operation: u32, pub operation_len: u32,
    pub args: u32, pub args_len: u32,
}
#[repr(C)]
pub struct Path {
    pub width: u32, pub count: u32, pub known: u32, pub bytes: [u8; 64],
}
#[repr(C)]
pub struct Signal { pub available: u32, pub rssi: f32, pub snr: f32 }
#[repr(C)]
pub struct Authority {
    pub authenticated: u32, pub channel_verified: u32, pub targeted: u32, pub owner: u32,
    pub shared: u32, pub home: u32, pub reminders: u32, pub local: u32,
}
#[repr(C)]
pub struct Node {
    pub available: u32, pub identity: u32, pub enabled: u32, pub ready: u32,
    pub fault: u32, pub roles_known: u32, pub wifi_known: u32, pub wifi_connected: u32,
    pub selected_roles: u32, pub ready_roles: u32, pub generation: u32,
    pub uptime: u64, pub public_key: [u8; 32],
    pub revision: [u8; 13], pub build: [u8; 24], pub name: [u8; 33],
}
#[repr(C)]
pub struct Air {
    pub available: u32, pub transmitting: u32, pub queued: u32, pub aggregate_queued: u32,
    pub generation: u32, pub configuration_generation: u32, pub captured_ms: u32,
    pub credit_ms: u32, pub aggregate_credit_ms: u32, pub rf_ms: u32, pub aggregate_rf_ms: u32,
    pub successes: u32, pub failures: u32, pub aggregate_successes: u32, pub aggregate_failures: u32,
    pub reserved_ms: u32, pub limit_ms: u32, pub remaining_ms: u32,
}
#[repr(C)]
pub struct Packet {
    pub handle: u32, pub timestamp: u32, pub route_type: u32, pub authenticated: u32,
    pub forwardable: u32, pub channel: u32, pub path: Path, pub signal: Signal,
    pub sender: [u8; 32], pub nickname: [u8; 33],
}
#[repr(C)]
pub struct TraceResult { pub width: u32, pub count: u32, pub snr: [i8; 64] }
pub const CALLER: u32 = 0;
pub const CONVERSATION: u32 = 1;
pub const BOT: u32 = 2;
pub const CHANNEL_SCOPE: u32 = 3;
pub const PUBLIC: u32 = 0;
pub const PRIVATE: u32 = 1;
pub const OWNER: u32 = 2;
pub const CHANNEL: u32 = 3;
pub const SHARED: u32 = 4;
pub const REMINDER: u32 = 5;
pub const HOME: u32 = 6;
pub const DONE: i32 = 0;
pub const PENDING: i32 = 1;
pub const ABI_VERSION: i32 = 1;
#[link(wasm_import_module = "meshcore_v1")]
unsafe extern "C" {
    #[link_name = "command"]
    pub fn command(
        id: u32,
        name: *const u8,
        name_len: u32,
        schema: *const u8,
        schema_len: u32,
        help: *const u8,
        help_len: u32,
        permission: u32,
    ) -> i32;
    #[link_name = "subscribe"]
    pub fn subscribe(kind: u32, handler_id: u32) -> i32;
    #[link_name = "read"]
    pub fn read(handle: u32, field: u32, out: *mut u8, capacity: u32) -> i32;
    #[link_name = "reply"]
    pub fn reply(job: u32, text: *const u8, length: u32) -> i32;
    #[link_name = "io"]
    pub fn io(
        job: u32,
        kind: u32,
        scope: u32,
        key: *const u8,
        key_len: u32,
        value: *const u8,
        value_len: u32,
        delay: u32,
    ) -> i32;
}
