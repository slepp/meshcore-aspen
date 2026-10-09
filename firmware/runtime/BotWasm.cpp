// SPDX-License-Identifier: Apache-2.0
#if defined(MESHCORE_ONCHIP_BOT) && MESHCORE_ONCHIP_BOT && ONCHIP_BOT_WASM
#include "BotWasm.h"
#include "BotTimers.h"
#include "BotReminders.h"
#include "WamrRuntime.h"
#include "wasm_export.h"
#include "wasm/sdk/meshcore.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <climits>
#include <cmath>
#include <new>
#include <string.h>
#ifdef ARDUINO_ARCH_ESP32
#include <esp_heap_caps.h>
#endif

namespace {
using onchip::wamr::Meter;
using onchip::wamr::nowUs;
using onchip::wamr::StackBytes;
using onchip::wamr::PoolBytes;
static_assert(uint32_t(onchip::BotIoRequest::Sleep) == MC_SLEEP &&
    uint32_t(onchip::BotIoRequest::Get) == MC_GET && uint32_t(onchip::BotIoRequest::Put) == MC_PUT &&
    uint32_t(onchip::BotIoRequest::Delete) == MC_DELETE && uint32_t(onchip::BotIoRequest::Rpc) == MC_RPC &&
    uint32_t(onchip::BotIoRequest::TimerSet) == MC_TIMER_SET &&
    uint32_t(onchip::BotIoRequest::TimerGet) == MC_TIMER_GET &&
    uint32_t(onchip::BotIoRequest::TimerCancel) == MC_TIMER_CANCEL &&
    uint32_t(onchip::BotIoRequest::TimerWait) == MC_TIMER_WAIT &&
    uint32_t(onchip::BotIoRequest::List) == MC_LIST, "Native I/O must preserve Wasm ABI v1 kinds");
static_assert(uint32_t(onchip::BotIoRequest::Send) == MC_SEND &&
    uint32_t(onchip::BotIoRequest::Wait) == MC_WAIT && uint32_t(onchip::BotIoRequest::Trace) == MC_TRACE &&
    uint32_t(onchip::BotIoRequest::Advert) == MC_ADVERT && uint32_t(onchip::BotIoRequest::Forward) == MC_FORWARD &&
    uint32_t(onchip::BotIoRequest::ReminderSet) == MC_REMINDER_SET &&
    uint32_t(onchip::BotIoRequest::ReminderList) == MC_REMINDER_LIST &&
    uint32_t(onchip::BotIoRequest::ReminderCancel) == MC_REMINDER_CANCEL &&
    uint32_t(onchip::BotIoRequest::Cas) == MC_CAS && uint32_t(onchip::BotIoRequest::Transaction) == MC_TRANSACTION &&
    uint32_t(onchip::BotIoRequest::Inspect) == MC_INSPECT && uint32_t(onchip::BotIoRequest::Admin) == MC_ADMIN &&
    uint32_t(onchip::BotIoRequest::HttpGet) == MC_HTTP_GET && uint32_t(onchip::BotIoRequest::HttpPost) == MC_HTTP_POST &&
    uint32_t(onchip::BotIoRequest::Utility) == MC_UTILITY, "Extended I/O must preserve native ABI IDs");
static_assert(uint32_t(onchip::BotIoRequest::Caller) == MC_CALLER &&
    uint32_t(onchip::BotIoRequest::Conversation) == MC_CONVERSATION &&
    uint32_t(onchip::BotIoRequest::Bot) == MC_BOT &&
    uint32_t(onchip::BotIoRequest::Channel) == MC_CHANNEL_SCOPE &&
    uint32_t(onchip::BotIoRequest::ChannelThread) == MC_CHANNEL_THREAD,
    "Native scopes must preserve Wasm ABI v1");
static_assert(uint32_t(onchip::BotCommand::Public) == MC_PUBLIC &&
    uint32_t(onchip::BotCommand::Private) == MC_PRIVATE &&
    uint32_t(onchip::BotCommand::Owner) == MC_OWNER &&
    uint32_t(onchip::BotCommand::Channel) == MC_CHANNEL &&
    uint32_t(onchip::BotCommand::Shared) == MC_SHARED &&
    uint32_t(onchip::BotCommand::Reminder) == MC_REMINDER &&
    uint32_t(onchip::BotCommand::Home) == MC_HOME, "Native permissions must preserve Wasm ABI v1");
void memoryStats(onchip::BotVmStats &stats, size_t sessionBytes, bool linearAllocated = true) {
  mem_alloc_info_t info{};
  wasm_runtime_get_mem_alloc_info(&info);
  stats.peakBytes = info.highmark_size;
  stats.wasmLinearBytes = linearAllocated ? 65536 : 0;
  stats.wasmPoolBytes = PoolBytes;
  stats.wasmPoolHighWaterBytes = info.highmark_size;
  stats.wasmSessionBytes = uint32_t(sessionBytes);
}

// The configured home codec takes one native text argument, not raw JSON.
// Decode only its flat schema here; never allocate or recurse on guest JSON.
struct HomeArguments {
  const unsigned char *p, *end;
  void whitespace() {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
  }
  bool take(unsigned char c) {
    whitespace();
    if (p == end || *p != c) return false;
    ++p; return true;
  }
  bool hex(uint32_t &value) {
    value = 0;
    for (unsigned i = 0; i < 4; ++i) {
      if (p == end) return false;
      const unsigned char c = *p++;
      const int digit = c >= '0' && c <= '9' ? c - '0' :
                        c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                        c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
      if (digit < 0) return false;
      value = (value << 4) | unsigned(digit);
    }
    return true;
  }
  bool string(char *out, size_t capacity) {
    if (!take('"')) return false;
    size_t size = 0;
    while (p < end) {
      uint32_t c = *p++;
      if (c == '"') { out[size] = 0; return true; }
      if (c < 0x20) return false;
      if (c == '\\') {
        if (p == end) return false;
        c = *p++;
        switch (c) {
          case '"': case '\\': case '/': break;
          case 'b': c = '\b'; break;
          case 'f': c = '\f'; break;
          case 'n': c = '\n'; break;
          case 'r': c = '\r'; break;
          case 't': c = '\t'; break;
          case 'u':
            if (!hex(c)) return false;
            if (c >= 0xd800 && c <= 0xdbff) {
              uint32_t low;
              if (end - p < 2 || *p++ != '\\' || *p++ != 'u' ||
                  !hex(low) || low < 0xdc00 || low > 0xdfff) return false;
              c = 0x10000 + ((c - 0xd800) << 10) + low - 0xdc00;
            } else if (c >= 0xdc00 && c <= 0xdfff) return false;
            break;
          default: return false;
        }
      } else if (c >= 0x80) {
        const unsigned continuation = c >= 0xc2 && c <= 0xdf ? 1 :
            c >= 0xe0 && c <= 0xef ? 2 : c >= 0xf0 && c <= 0xf4 ? 3 : 0;
        if (!continuation || size_t(end - p) < continuation) return false;
        c &= (1u << (6 - continuation)) - 1;
        for (unsigned i = 0; i < continuation; ++i) {
          const unsigned char next = *p++;
          if ((next & 0xc0) != 0x80) return false;
          c = (c << 6) | (next & 0x3f);
        }
        const uint32_t minimum = continuation == 1 ? 0x80 : continuation == 2 ? 0x800 : 0x10000;
        if (c < minimum || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) return false;
      }
      if (!c) return false;
      const unsigned width = c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
      if (size + width >= capacity) return false;
      if (width == 1) out[size++] = char(c);
      else {
        out[size++] = char((width == 2 ? 0xc0 : width == 3 ? 0xe0 : 0xf0) |
                           (c >> (6 * (width - 1))));
        for (unsigned i = width - 1; i; --i)
          out[size++] = char(0x80 | ((c >> (6 * (i - 1))) & 0x3f));
      }
    }
    return false;
  }
  bool object(const char *field, char *out, size_t capacity) {
    if (!take('{')) return false;
    if (field) {
      char name[6]{};
      if (!string(name, sizeof(name)) || strcmp(name, field) || !take(':') ||
          !string(out, capacity) || !out[0]) return false;
    }
    if (!take('}')) return false;
    whitespace(); return p == end;
  }
};
bool normalizeHomeRpc(onchip::BotIoRequest &request) {
  if (strcmp(request.endpoint, "home")) return true;
  const bool health = !strcmp(request.key, "health");
  const char *field = !strcmp(request.key, "echo") ? "text" :
                      !strcmp(request.key, "weather") ? "place" : nullptr;
  if (!health && !field) return true;
  const auto *json = reinterpret_cast<const unsigned char *>(request.json);
  HomeArguments args{json, json + strlen(request.json)};
  return args.object(field, request.value, !strcmp(request.key, "weather") ? 81 : sizeof(request.value));
}

bool profile(const uint8_t *bytes, size_t size, char *error, size_t capacity) {
  static const char *const imports[] = {"command", "subscribe", "read", "reply", "io"};
  return onchip::wamr::profile(bytes, size, "meshcore_v1", imports, 5, error, capacity);
}
} // namespace

namespace onchip {
bool botWasmBytes(const char *source, size_t size, const uint8_t *&bytes, size_t &length) {
  if (!source) return false;
  if (size >= 8 && !memcmp(source, "\0asm\1\0\0\0", 8)) {
    bytes = reinterpret_cast<const uint8_t *>(source); length = size; return true;
  }
  constexpr char prefix[] = "--@meshcore-bot/1;";
  if (size < sizeof(prefix) || memcmp(source, prefix, sizeof(prefix) - 1)) return false;
  const auto *newline = static_cast<const char *>(memchr(source, '\n', std::min(size, size_t(256))));
  if (!newline || size_t(newline + 1 - source) + 8 > size ||
      memcmp(newline + 1, "\0asm\1\0\0\0", 8)) return false;
  char header[256]{};
  memcpy(header, source, size_t(newline - source));
  if (!strstr(header, ";runtime=wamr-2.4.1;api=meshcore-v1;")) return false;
  bytes = reinterpret_cast<const uint8_t *>(newline + 1);
  length = size - size_t(newline + 1 - source); return true;
}
struct BotWasmSession::Impl {
  Meter meter;
  wamr::Context context{wamr::Kind::Bot, &meter, this};
  wasm_module_t module = nullptr;
  wasm_module_inst_t instance = nullptr;
  wasm_exec_env_t env = nullptr;
  wasm_function_inst_t init = nullptr, start = nullptr, resume = nullptr;
  uint8_t bytes[BotSourceLimit]{};
  BotManifest manifest{};
  uint32_t ids[BotCommandLimit]{}, events[4]{};
  uint32_t generation = 0, nextHandle = 0;
  BotVmLimits limits;
  std::atomic<uint32_t> *epoch = nullptr;
  bool initializing = true;
  struct Job {
    enum State { Free, Running, Waiting, Done } state = Free;
    uint32_t handle = 0, operation = 0, operations = 0, native = 0, fuel = 0;
    uint32_t packetHandles[BotIoLimit]{}, packetIds[BotIoLimit]{};
    bool dispatched = false, completed = false;
    BotEvent event{};
    BotArguments arguments{};
    BotIoRequest request{};
    BotIoRequest::Kind completionKind = BotIoRequest::Sleep;
    BotIoResult completion{};
    BotSession::Result result{};
  } jobs[BotJobLimit];
  Job *current = nullptr;
  static Impl *self(wasm_exec_env_t e) {
    auto *context = wamr::context(e, wamr::Kind::Bot);
    return context ? static_cast<Impl *>(context->owner) : nullptr;
  }
  int32_t fail(const char *message) {
    wasm_runtime_set_exception(instance, message); return -1;
  }
  bool native() {
    if (meter.eventEpoch && epoch && meter.eventEpoch != epoch->load()) {
      fail("Wasm event epoch revoked"); return false;
    }
    if (++meter.native > 64 || nowUs() >= meter.deadline) {
      fail("Wasm native-work budget exceeded (64 calls)"); return false;
    }
    return true;
  }
  void *range(uint32_t offset, uint32_t size) {
    if (!wasm_runtime_validate_app_addr(instance, offset, size)) {
      fail("Wasm import linear-memory range out of bounds"); return nullptr;
    }
    return wasm_runtime_addr_app_to_native(instance, offset);
  }
  bool text(uint32_t offset, uint32_t size, char *out, size_t capacity, bool empty = false) {
    if ((!size && !empty) || size >= capacity) { fail("Wasm import text length exceeds bound"); return false; }
    const auto *p = static_cast<const char *>(range(offset, size));
    if (!p || memchr(p, 0, size)) { fail("Wasm import text contains NUL/out-of-bounds"); return false; }
    memcpy(out, p, size); out[size] = 0; return true;
  }
  template<class T> bool copied(uint32_t offset, uint32_t size, T &out) {
    if (size != sizeof(T)) { fail("Wasm descriptor size invalid"); return false; }
    const auto *p = range(offset, size);
    if (!p) return false;
    memcpy(&out, p, size); return true;
  }
  static mc_path path(const BotPath &p) {
    static_assert(BotPathLimit <= 64, "ABI v1 path capacity");
    mc_path out{}; out.width = p.width; out.count = p.count; out.known = p.known;
    memcpy(out.bytes, p.bytes, BotPathLimit); return out;
  }
  Job *job(uint32_t handle) {
    if (!current || current->handle != handle || current->state != Job::Running) {
      fail("Wasm job handle invalid or stale"); return nullptr;
    }
    if (current->event.eventEpoch && epoch && current->event.eventEpoch != epoch->load()) {
      fail("Wasm event epoch revoked"); return nullptr;
    }
    return current;
  }
  static int32_t command(wasm_exec_env_t e, uint32_t id, uint32_t name, uint32_t nameSize,
      uint32_t schema, uint32_t schemaSize, uint32_t help, uint32_t helpSize, uint32_t permission) {
    auto *owner = self(e); if (!owner) return -1;
    auto &s = *owner;
    if (!s.native()) return -1;
    if (!s.initializing || !id || id > 65535 || s.manifest.count == BotCommandLimit ||
        permission > BotCommand::Home) return s.fail("Wasm command registration unavailable/invalid");
    BotCommand c;
    if (!s.text(name, nameSize, c.name, sizeof(c.name)) ||
        !s.text(schema, schemaSize, c.schema, sizeof(c.schema), true) ||
        !s.text(help, helpSize, c.help, sizeof(c.help))) return -1;
    char error[128]{};
    if (!botCommandName(c.name) || botReservedCommand(c.name) || s.manifest.find(c.name) ||
        !validateBotSchema(c.schema, error, sizeof(error))) return s.fail("Wasm command name/schema invalid or reserved");
    for (unsigned i = 0; i < s.manifest.count; ++i)
      if (s.ids[i] == id) return s.fail("Wasm duplicate command identifier");
    c.permission = BotCommand::Permission(permission);
    s.ids[s.manifest.count] = id; s.manifest.writable()[s.manifest.count++] = c;
    return 0;
  }
  static int32_t subscribe(wasm_exec_env_t e, uint32_t kind, uint32_t id) {
    auto *owner = self(e); if (!owner) return -1;
    auto &s = *owner;
    if (!s.native()) return -1;
    if (!s.initializing || kind < 1 || kind > 4 || !id || id > 65535 || s.events[kind - 1])
      return s.fail("Wasm event registration invalid");
    s.events[kind - 1] = id; s.manifest.eventMask |= 1u << (kind - 1); return 0;
  }
  static int32_t read(wasm_exec_env_t e, uint32_t handle, uint32_t field, uint32_t out, uint32_t capacity) {
    auto *owner = self(e); if (!owner) return -1;
    auto &s = *owner;
    if (!s.native()) return -1;
    if (capacity > BotNetworkResponseLimit) return s.fail("Wasm read capacity exceeds 2048 bytes");
    const void *data = nullptr; size_t size = 0;
    uint32_t number = 0; uint64_t wide = 0;
    mc_path copiedPath{}; mc_signal signal{};
    mc_authority authority{};
    uint32_t argTypes[BotParameterLimit]{};
    mc_node node{}; mc_air air{}; mc_packet packet{}; mc_trace_result trace{};
    auto *j = s.current;
    if (!j) return s.fail("Wasm read requires an invocation");
    if (handle == j->handle && s.job(handle)) {
      switch (field) {
      case MC_ARGUMENTS: data = j->event.arguments; size = strlen(j->event.arguments); break;
      case MC_MESSAGE: data = j->event.message; size = strlen(j->event.message); break;
      case MC_SENDER: data = j->event.sender; size = 32; break;
      case MC_CHANNEL_KEY: data = j->event.channelId; size = 32; break;
      case MC_TIMESTAMP: number = j->event.timestamp; data = &number; size = 4; break;
      case MC_UPTIME: wide = j->event.node.uptimeMs; data = &wide; size = 8; break;
      case MC_EVENT_KIND: number = j->event.kind; data = &number; size = 4; break;
      case MC_NICKNAME: data = j->event.nickname; size = strlen(j->event.nickname); break;
      case MC_PATH: copiedPath = path(j->event.path); data = &copiedPath; size = sizeof(copiedPath); break;
      case MC_SIGNAL:
        signal = {uint32_t(j->event.signal), j->event.rssi, j->event.snr};
        data = &signal; size = sizeof(signal); break;
      case MC_AUTHORITY:
        authority = {j->event.authenticated, j->event.channelVerified, j->event.targeted, j->event.owner,
                     j->event.sharedState, j->event.homeAccess, j->event.reminderAccess, j->event.local};
        data = &authority; size = sizeof(authority); break;
      case MC_ARG_TYPES:
        for (unsigned i = 0; i < j->arguments.count; ++i) argTypes[i] = j->arguments.values[i].type;
        data = argTypes; size = j->arguments.count * 4; break;
      case MC_NODE: {
        const auto &n = j->event.node;
        memset(&node, 0, sizeof(node));
        node.available = n.available; node.identity = n.hasIdentity; node.enabled = n.enabled;
        node.ready = n.ready; node.fault = n.fault; node.roles_known = n.rolesKnown;
        node.wifi_known = n.wifiKnown; node.wifi_connected = n.wifiConnected;
        node.selected_roles = n.selectedRoles; node.ready_roles = n.readyRoles;
        node.generation = n.sourceGeneration; node.uptime = n.uptimeMs;
        memcpy(node.public_key, n.publicKey, sizeof(node.public_key));
        memcpy(node.revision, n.nativeRevision, sizeof(node.revision));
        memcpy(node.build, n.build, sizeof(node.build)); memcpy(node.name, n.name, sizeof(node.name));
        data = &node; size = sizeof(node); break;
      }
      case MC_AIR: {
        const auto &a = j->event.air;
        air = {a.available, a.transmitting, a.queued, a.aggregateQueued, a.generation, a.configurationGeneration,
               a.capturedMs, a.creditMs, a.aggregateCreditMs, a.rfMs, a.aggregateRfMs,
               a.successes, a.failures, a.aggregateSuccesses, a.aggregateFailures, a.reservedMs, a.limitMs, a.remainingMs};
        data = &air; size = sizeof(air); break;
      }
      default:
        if (field < MC_ARG0 || field >= uint32_t(MC_ARG0) + j->arguments.count)
          return s.fail("Wasm event field unavailable");
        const auto &v = j->arguments.values[field - MC_ARG0];
        if (v.type == BotValue::String) { data = v.text; size = strlen(v.text); }
        else if (v.type != BotValue::Missing) { number = uint32_t(v.integer); data = &number; size = 4; }
      }
    } else if (j->completed && handle == j->operation) {
      const auto &r = j->completion;
      switch (field) {
      case MC_VALUE: data = r.value; size = strlen(r.value); break;
      case MC_ERROR: data = r.error; size = strlen(r.error); break;
      case MC_RPC_CODE: data = r.rpcCode; size = strlen(r.rpcCode); break;
      case MC_FOUND: number = r.found; data = &number; size = 4; break;
      case MC_HTTP_STATUS: number = r.httpStatus; data = &number; size = 4; break;
      case MC_OUTCOME: number = r.outcome; data = &number; size = 4; break;
      case MC_OPERATION_KIND: number = j->completionKind; data = &number; size = 4; break;
      case MC_TIMER_STATE: number = uint32_t(r.timerState); data = &number; size = 4; break;
      case MC_REVISION: number = r.revision; data = &number; size = 4; break;
      case MC_DEADLINE_UTC: number = r.deadlineUtc; data = &number; size = 4; break;
      case MC_TIME_TRUSTED: number = r.timeTrusted; data = &number; size = 4; break;
      case MC_KEYS_COUNT: number = r.keys.count; data = &number; size = 4; break;
      case MC_COUNTRY: data = r.country; size = strlen(r.country); break;
      case MC_TEMPERATURE_C: data = &r.temperatureC; size = 8; break;
      case MC_WEATHER_CODE: number = r.weatherCode; data = &number; size = 4; break;
      case MC_SOURCE: data = r.source; size = strlen(r.source); break;
      case MC_OBSERVED_AT: data = r.observedAt; size = strlen(r.observedAt); break;
      case MC_SOURCE_AGE: number = r.sourceAgeSeconds; data = &number; size = 4; break;
      case MC_JSON: data = r.json; size = strlen(r.json); break;
      case MC_NETWORK_SUBMITTED: number = r.networkSubmitted; data = &number; size = 4; break;
      case MC_REMINDER_STATE: number = uint32_t(r.reminderState); data = &number; size = 4; break;
      case MC_PENDING_STATE: number = r.pending; data = &number; size = 4; break;
      case MC_REPLACED: number = r.replaced; data = &number; size = 4; break;
      case MC_RADIO_JOB: number = r.radioJob; data = &number; size = 4; break;
      case MC_QUEUED: number = r.queued; data = &number; size = 4; break;
      case MC_TRANSMITTED: number = r.transmitted; data = &number; size = 4; break;
      case MC_ACKNOWLEDGED: number = r.acknowledged; data = &number; size = 4; break;
      case MC_TRUNCATED: number = r.truncated; data = &number; size = 4; break;
      case MC_PACKET:
        memset(&packet, 0, sizeof(packet));
        packet.handle = r.packet.id ? j->operation : 0; packet.timestamp = r.packet.timestamp;
        packet.route_type = r.packet.routeType; packet.authenticated = r.packet.authenticated;
        packet.forwardable = r.packet.forwardable; packet.channel = r.packet.channel;
        packet.path = path(r.packet.path); packet.signal = {r.packet.signal, r.packet.rssi, r.packet.snr};
        memcpy(packet.sender, r.packet.sender, 32);
        memcpy(packet.nickname, r.packet.nickname, sizeof(packet.nickname));
        data = &packet; size = sizeof(packet); break;
      case MC_TRACE_RESULT:
        static_assert(BotTraceHopLimit <= 64, "ABI v1 trace capacity");
        trace.width = r.trace.width; trace.count = r.trace.count;
        memcpy(trace.snr, r.trace.snr, r.trace.count); data = &trace; size = sizeof(trace); break;
      default:
        if (field < MC_KEY0 || field >= uint32_t(MC_KEY0) + r.keys.count)
          return s.fail("Wasm completion field unavailable");
        data = r.keys.keys[field - MC_KEY0]; size = strlen(r.keys.keys[field - MC_KEY0]);
      }
    } else return s.fail("Wasm read handle invalid or stale");
    if (size > capacity) return s.fail("Wasm read output capacity too small");
    void *destination = s.range(out, uint32_t(size));
    if (!destination) return -1;
    if (size) memcpy(destination, data, size);
    return int32_t(size);
  }
  static int32_t reply(wasm_exec_env_t e, uint32_t handle, uint32_t text, uint32_t size) {
    auto *owner = self(e); if (!owner) return -1;
    auto &s = *owner; if (!s.native()) return -1;
    auto *j = s.job(handle); if (!j) return -1;
    if (j->event.kind != BotEvent::Command || j->result.action.kind != BotAction::None ||
        size > j->event.replyLimit) return s.fail("Wasm reply unavailable/already produced/exceeds RF bound");
    if (!s.text(text, size, j->result.action.text, sizeof(j->result.action.text))) return -1;
    for (uint32_t i = 0; i < size; ++i)
      if (uint8_t(j->result.action.text[i]) < 32 || uint8_t(j->result.action.text[i]) > 126)
        return s.fail("Wasm reply requires printable ASCII");
    j->result.action.kind = BotAction::Reply; return 0;
  }
  bool scope(Job &j, BotIoRequest &r, uint32_t value) {
    if (value > BotIoRequest::ChannelThread) { fail("Wasm storage scope unavailable"); return false; }
    r.scope = BotIoRequest::Scope(value);
    char error[128]{};
    if (!botStorageScope(j.event, r, error, sizeof(error))) { fail(error); return false; }
    return true;
  }
  static int32_t io(wasm_exec_env_t e, uint32_t handle, uint32_t kind, uint32_t scope,
      uint32_t key, uint32_t keySize, uint32_t value, uint32_t valueSize, uint32_t delay) {
    auto *owner = self(e); if (!owner) return -1;
    auto &s = *owner; if (!s.native()) return -1;
    auto *j = s.job(handle); if (!j) return -1;
    if (j->request.token.operation || ++j->operations > BotIoLimit || s.nextHandle == INT32_MAX)
      return s.fail("Wasm pending I/O/operation handle/native-operation limit exceeded");
    const bool atomic = kind == BotIoRequest::Cas || kind == BotIoRequest::Transaction;
    const bool storage = kind == BotIoRequest::Get || kind == BotIoRequest::Put ||
        kind == BotIoRequest::Delete || kind == BotIoRequest::List || atomic;
    const bool timer = kind >= BotIoRequest::TimerSet && kind <= BotIoRequest::TimerWait;
    const bool reminder = kind >= BotIoRequest::ReminderSet && kind <= BotIoRequest::ReminderCancel;
    const bool mesh = (kind >= BotIoRequest::Send && kind <= BotIoRequest::Advert) ||
        kind == BotIoRequest::Forward || kind == BotIoRequest::Inspect || kind == BotIoRequest::Admin;
    const bool network = kind == BotIoRequest::Rpc || kind == BotIoRequest::HttpGet || kind == BotIoRequest::HttpPost;
    const bool utility = kind == BotIoRequest::Utility;
    if (!storage && !timer && !reminder && !mesh && !network && !utility && kind != BotIoRequest::Sleep)
      return s.fail("Wasm I/O kind unavailable in ABI v1");
    const auto &event = j->event;
    if (event.kind != BotEvent::Command && !(storage || timer || kind == BotIoRequest::Sleep))
      return s.fail("Subscriptions have no radio, reminder or private network authority");
    auto &r = j->request;
    r.~BotIoRequest(); new (&r) BotIoRequest{};
    r.kind = BotIoRequest::Kind(kind);
    if (atomic) {
      if (keySize || !valueSize || valueSize % sizeof(mc_mutation) ||
          valueSize / sizeof(mc_mutation) > BotTransactionLimit ||
          (kind == BotIoRequest::Cas && valueSize != sizeof(mc_mutation)))
        return s.fail("Wasm atomic operation requires 1..4 mutation descriptors");
      r.mutations = valueSize / sizeof(mc_mutation);
      const auto *input = s.range(value, valueSize); if (!input) return -1;
      mc_mutation descriptors[BotTransactionLimit]{};
      memcpy(descriptors, input, valueSize);
      for (unsigned i = 0; i < r.mutations; ++i) {
        const auto &d = descriptors[i]; auto &m = r.mutation[i];
        if (d.flags & ~7u || (!(d.flags & MC_COMPARE) && (d.flags & MC_PRESENT)) ||
            (kind == BotIoRequest::Cas && !(d.flags & MC_COMPARE)))
          return s.fail("Wasm mutation flags invalid");
        m.compare = d.flags & MC_COMPARE; m.present = d.flags & MC_PRESENT; m.remove = d.flags & MC_REMOVE;
        if (!s.text(d.key, d.key_len, m.key, sizeof(m.key)) ||
            !s.text(d.expected, d.expected_len, m.expected, sizeof(m.expected), true) ||
            !s.text(d.value, d.value_len, m.value, sizeof(m.value), true)) return -1;
        if ((!m.present && d.expected_len) || (m.remove && d.value_len))
          return s.fail("Wasm absent mutation value must be empty");
        for (unsigned k = 0; k < i; ++k)
          if (!strcmp(m.key, r.mutation[k].key)) return s.fail("Wasm transaction duplicate key");
      }
      if (!s.scope(*j, r, scope)) return -1;
    } else if (mesh) {
      if (event.local || !(event.authenticated ? !event.channel[0] : event.channelVerified && event.channel[0]))
        return s.fail("Mesh requires authenticated DM or verified native channel");
      if (kind == BotIoRequest::Send || kind == BotIoRequest::Wait || kind == BotIoRequest::Trace) {
        mc_mesh_options d{};
        if (!s.copied(key, keySize, d) || d.packet_kind > BotIoRequest::TracePacket ||
            d.wait_kind > BotIoRequest::ChannelWait || d.exact > 1 || d.trace_send_only > 1 ||
            d.path_width > 3 || d.route_type > 2)
          return s.fail("Wasm mesh descriptor invalid");
        r.packetKind = BotIoRequest::PacketKind(d.packet_kind); r.waitKind = BotIoRequest::WaitKind(d.wait_kind);
        r.exact = d.exact; r.waitAck = r.waitKind == BotIoRequest::AckWait;
        r.pathWidth = d.path_width; r.routeType = d.route_type; r.traceSendOnly = d.trace_send_only;
        r.delayMs = delay ? delay : 5000;
        if (r.delayMs > 30000) return s.fail("Wasm mesh timeout requires 1..30000 ms");
        r.grant = event.meshGrant;
        memcpy(r.principal, event.sender, 32);
        if (d.destination) {
          const auto *destination = static_cast<const uint8_t *>(s.range(d.destination, 32));
          if (!destination) return -1;
          bool allowed = !memcmp(destination, event.sender, 32);
          bool nonzero = false; for (unsigned i = 0; i < 32; ++i) nonzero |= destination[i] != 0;
          if (event.authenticated && nonzero) for (const auto &target : event.destinations)
            allowed |= !memcmp(destination, target, 32);
          if (!event.authenticated || !allowed) return s.fail("Wasm DM destination not granted by owner");
          memcpy(r.principal, destination, 32);
        }
        if (kind == BotIoRequest::Send) {
          if (d.route_len || d.route || d.packet_kind == BotIoRequest::TracePacket ||
              (r.packetKind == BotIoRequest::ChannelPacket ?
               (!event.channelVerified || !event.channel[0] || !event.targeted || d.destination) :
               (!event.authenticated || event.channel[0])))
            return s.fail("Wasm packet kind requires native DM/targeted-channel authority");
          if (!s.text(value, valueSize, r.value, event.replyLimit + 1)) return -1;
          for (uint32_t i = 0; i < valueSize; ++i)
            if (uint8_t(r.value[i]) < 32 || uint8_t(r.value[i]) > 126)
              return s.fail("Wasm mesh text requires printable ASCII");
        } else if (kind == BotIoRequest::Wait) {
          if (d.route_len || d.route || d.trace_send_only ||
              (r.waitKind == BotIoRequest::ChannelWait &&
               (!event.channelVerified || !event.channel[0] || !event.channelWait || d.destination)) ||
              (r.waitKind != BotIoRequest::TextWait && r.waitKind != BotIoRequest::ChannelWait &&
               (valueSize || d.destination || d.exact || d.path_width || d.route_type)) ||
              (r.waitKind == BotIoRequest::TextWait && !event.authenticated))
            return s.fail("Wasm follow-up wait/filter requires native authority");
          if (!s.text(value, valueSize, r.key, sizeof(r.key), true)) return -1;
        } else {
          if (d.destination || valueSize || !d.route_len || d.route_len > BotTraceLimit ||
              (d.route_width != 1 && d.route_width != 2 && d.route_width != 4 && d.route_width != 8) ||
              d.route_len % d.route_width || d.route_len / d.route_width > BotTraceHopLimit)
            return s.fail("Wasm TRACE requires explicit 1/2/4/8-byte route");
          const auto *route = s.range(d.route, d.route_len); if (!route) return -1;
          r.routeSize = d.route_len; r.routeWidth = d.route_width; memcpy(r.route, route, d.route_len);
        }
      } else if (kind == BotIoRequest::Forward) {
        if (keySize || valueSize || !event.authenticated || !event.forwardAccess)
          return s.fail("Wasm forward requires owned received DM and pair grant");
        for (unsigned i = 0; i < BotIoLimit; ++i)
          if (j->packetHandles[i] == delay) r.packetId = j->packetIds[i];
        if (!r.packetId) return s.fail("Wasm received packet handle invalid or stale");
        r.grant = event.forwardGrant; r.delayMs = 5000;
      } else if (kind == BotIoRequest::Admin) {
        if (keySize || !event.owner || !event.authenticated)
          return s.fail("Admin requires authenticated trusted-owner DM");
        if (!s.text(value, valueSize, r.value, event.replyLimit + 1)) return -1;
        r.delayMs = 5000;
      } else {
        if (valueSize || (kind == BotIoRequest::Advert && keySize))
          return s.fail("Wasm advert/inspection arguments invalid");
        if (kind == BotIoRequest::Inspect) {
          if (!s.text(key, keySize, r.key, sizeof(r.key))) return -1;
          if (strcmp(r.key, "neighbors") || delay > 16)
            return s.fail("Wasm inspection requires neighbors and page 1..16");
          r.revision = delay ? delay : 1;
        }
        r.delayMs = 5000;
      }
    } else if (utility) {
      if (event.local || !(event.authenticated ? !event.channel[0] : event.channelVerified && event.channel[0]))
        return s.fail("Utilities require authenticated DM or verified selected channel");
      char operation[9]{};
      if (!s.text(key, keySize, operation, sizeof(operation))) return -1;
      if (!strcmp(operation, "convert")) {
        mc_convert d{};
        if (!s.copied(value, valueSize, d) ||
            !s.text(d.value, d.value_len, r.value, 33) ||
            !s.text(d.from, d.from_len, r.key, 5) ||
            !s.text(d.to, d.to_len, r.endpoint, 5)) return -1;
        r.delaySeconds = 1;
      } else {
        r.delaySeconds = !strcmp(operation, "calc") ? 0 : !strcmp(operation, "roll") ? 2 :
                         !strcmp(operation, "choose") ? 3 : UINT32_MAX;
        if (r.delaySeconds == UINT32_MAX) return s.fail("Wasm utility operation unavailable");
        if (!s.text(value, valueSize, r.value, r.delaySeconds == 0 ? 121 : r.delaySeconds == 2 ? 25 : 149,
                    r.delaySeconds == 2)) return -1;
      }
    } else if (reminder) {
      if (!event.authenticated || event.channel[0] || event.local || keySize)
        return s.fail("Personal reminders require authenticated private DM");
      memcpy(r.principal, event.sender, 32);
      if (kind == BotIoRequest::ReminderSet) {
        if (!event.reminderAccess || !delay || delay > BotTimerMaximumSeconds)
          return s.fail("Personal reminder requires native grant and 1..86400 seconds");
        if (!s.text(value, valueSize, r.value, BotReminderTextLimit + 1)) return -1;
        r.delaySeconds = delay; r.grant = event.reminderGrant;
      } else {
        if (valueSize || (kind == BotIoRequest::ReminderCancel && !delay))
          return s.fail("Wasm reminder cancel requires positive ID; list has no text");
        r.revision = delay;
      }
    } else if (storage || timer) {
      if (!s.text(key, keySize, r.key, sizeof(r.key), kind == BotIoRequest::List) ||
          !s.text(value, valueSize, r.value, sizeof(r.value), true)) return -1;
      if (!s.scope(*j, r, scope)) return -1;
      if (kind != BotIoRequest::Put && valueSize) return s.fail("Wasm unexpected storage/timer value");
      if (kind == BotIoRequest::TimerSet) {
        if (!delay || delay > BotTimerMaximumSeconds) return s.fail("Wasm timer delay out of range");
        r.delaySeconds = delay;
      }
    } else if (kind == BotIoRequest::Sleep) {
      if (!delay || delay > 30000 || keySize || valueSize) return s.fail("Wasm sleep requires 1..30000 ms");
      r.delayMs = delay;
    } else {
      if (event.kind != BotEvent::Command || !event.authenticated || !event.homeAccess ||
          event.channel[0] || event.local) return s.fail("Home RPC requires private authenticated DM and native grant");
      if (kind == BotIoRequest::Rpc) {
        if (keySize == sizeof(mc_rpc)) {
          mc_rpc d{};
          if (valueSize || !s.copied(key, keySize, d) ||
              !s.text(d.service, d.service_len, r.endpoint, sizeof(r.endpoint)) ||
              !s.text(d.operation, d.operation_len, r.key, BotNameLimit + 1) ||
              !s.text(d.args, d.args_len, r.json, sizeof(r.json)))
            return s.fail("Wasm RPC requires copied service/operation/JSON descriptor");
          if (!botIdentifier(r.endpoint) || r.endpoint[0] == '_' ||
              !botIdentifier(r.key) || r.key[0] == '_')
            return s.fail("Wasm RPC service/operation name invalid");
          if (!normalizeHomeRpc(r))
            return s.fail("Home RPC requires health {}, echo {text:string} or weather {place:string} within native text limits");
        } else {
          if (!s.text(key, keySize, r.key, sizeof(r.key)) ||
              !s.text(value, valueSize, r.value, sizeof(r.value), true)) return -1;
          if (strcmp(r.key, "health") && strcmp(r.key, "echo") && strcmp(r.key, "weather"))
            return s.fail("Wasm home RPC operation unavailable");
        }
      } else {
        if (!s.text(key, keySize, r.key, sizeof(r.key))) return -1;
        if (!botIdentifier(r.key) || r.key[0] == '_') return s.fail("Wasm HTTP requires configured endpoint name");
        if (kind == BotIoRequest::HttpGet && valueSize) return s.fail("Wasm HTTP GET has no payload");
        if (!s.text(value, valueSize, r.json, sizeof(r.json), kind == BotIoRequest::HttpGet)) return -1;
      }
      memcpy(r.principal, event.sender, 32);
      r.grant = event.homeGrant;
    }
    r.eventEpoch = j->event.eventEpoch;
    r.token = {s.generation, j->result.job, 0x80000000u | j->operations};
    j->operation = ++s.nextHandle; j->completed = false;
    return int32_t(j->operation);
  }
  bool call(wasm_function_inst_t function, uint32_t argc, uint32_t *argv, Job *j = nullptr) {
    meter = {};
    meter.fuel = j ? limits.instructions - std::min(limits.instructions, j->fuel) : limits.instructions;
    meter.native = j ? j->native : 0;
    meter.deadline = nowUs() + (j ? limits.wallUs -
        std::min(limits.wallUs, j->result.stats.elapsedUs) : limits.initWallUs);
    meter.epoch = epoch; meter.eventEpoch = j ? j->event.eventEpoch : 0;
    wasm_runtime_set_instruction_count_limit(env, int(meter.fuel));
    wasm_runtime_clear_exception(instance);
    const uint64_t started = nowUs();
    current = j;
    bool ok = wasm_runtime_call_wasm(env, function, argc, argv);
    if (nowUs() >= meter.deadline) {
      fail(j ? "Wasm invocation wall deadline exceeded" : "Wasm initialization wall deadline exceeded");
      ok = false;
    }
    if (j) {
      j->fuel += meter.used; j->native = meter.native;
      j->result.stats.instructions = j->fuel;
      j->result.stats.elapsedUs += nowUs() - started;
      j->result.stats.invokeUs = j->result.stats.elapsedUs;
      memoryStats(j->result.stats, sizeof(Impl));
      if (ok && argv[0] == MC_PENDING && j->request.token.operation) {
        j->state = Job::Waiting; j->dispatched = false;
      } else if (ok && argv[0] == MC_DONE && !j->request.token.operation &&
                 (j->event.kind != BotEvent::Command || j->result.action.kind != BotAction::None)) {
        j->state = Job::Done; j->result.ok = true;
      } else {
        j->result.action = {}; j->result.ok = false; j->state = Job::Done;
        snprintf(j->result.error, sizeof(j->result.error), "%s", ok ?
            "Wasm disposition requires a reply or one pending operation" :
            (wasm_runtime_get_exception(instance) ? wasm_runtime_get_exception(instance) : "Wasm execution failed"));
      }
    }
    current = nullptr; return ok;
  }
  bool signature(wasm_function_inst_t function, uint32_t argc) {
    return wamr::signature(instance, function, argc);
  }
};

BotWasmSession::~BotWasmSession() { clear(); }
void BotWasmSession::clear() {
  wamr::RuntimeLock lock;
  if (!impl_) return;
  if (impl_->env) wasm_runtime_destroy_exec_env(impl_->env);
  if (impl_->instance) wasm_runtime_deinstantiate(impl_->instance);
  if (impl_->module) wasm_runtime_unload(impl_->module);
  impl_->~Impl();
#ifdef ARDUINO_ARCH_ESP32
  heap_caps_free(impl_);
#else
  free(impl_);
#endif
  impl_ = nullptr;
}
bool BotWasmSession::load(const char *source, size_t size, uint32_t generation, BotVmStats &stats,
    char *error, size_t capacity, BotVmLimits limits) {
  wamr::RuntimeLock lock;
  clear(); stats = {};
  if (capacity) error[0] = 0;
  const uint8_t *bytes; size_t length;
  if (!generation || size > BotSourceLimit || !botWasmBytes(source, size, bytes, length) ||
      !profile(bytes, length, error, capacity)) {
    if (!error[0]) snprintf(error, capacity, "Expected bounded Wasm package and nonzero generation");
    return false;
  }
  if (limits.heapBytes < 65536 + StackBytes) {
    snprintf(error, capacity, "Wasm requires 64KiB linear memory and 8KiB interpreter stack"); return false;
  }
  if (!limits.instructions || !limits.initWallUs || !limits.wallUs) {
    snprintf(error, capacity, "Wasm requires nonzero instruction, initialization and invocation budgets"); return false;
  }
  static NativeSymbol symbols[] = {
    {"command", reinterpret_cast<void *>(Impl::command), "(iiiiiiii)i", nullptr},
    {"subscribe", reinterpret_cast<void *>(Impl::subscribe), "(ii)i", nullptr},
    {"read", reinterpret_cast<void *>(Impl::read), "(iiii)i", nullptr},
    {"reply", reinterpret_cast<void *>(Impl::reply), "(iii)i", nullptr},
    {"io", reinterpret_cast<void *>(Impl::io), "(iiiiiiii)i", nullptr}
  };
  if (!wamr::ensure("meshcore_v1", symbols, sizeof(symbols) / sizeof(symbols[0]),
                    error, capacity)) return false;
#ifdef ARDUINO_ARCH_ESP32
  void *memory = heap_caps_calloc(1, sizeof(Impl), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
  void *memory = calloc(1, sizeof(Impl));
#endif
  if (!memory) { snprintf(error, capacity, "Wasm session storage unavailable"); return false; }
  impl_ = new (memory) Impl;
  auto &s = *impl_; s.limits = limits; s.generation = generation;
  memcpy(s.bytes, bytes, length);
  const uint64_t started = nowUs();
  s.module = wasm_runtime_load(s.bytes, uint32_t(length), error, uint32_t(capacity));
  if (s.module) s.instance = wasm_runtime_instantiate(s.module, StackBytes, 0, error, uint32_t(capacity));
  if (s.instance) {
    wasm_runtime_set_custom_data(s.instance, &s.context);
    s.env = wasm_runtime_create_exec_env(s.instance, StackBytes);
    s.init = wasm_runtime_lookup_function(s.instance, "mc_init");
    s.start = wasm_runtime_lookup_function(s.instance, "mc_start");
    s.resume = wasm_runtime_lookup_function(s.instance, "mc_resume");
  }
  bool ok = s.env && s.signature(s.init, 0) && s.signature(s.start, 2) && s.signature(s.resume, 3);
  if (s.instance && !ok) snprintf(error, capacity, "Wasm requires mc_init()->i32, mc_start(i32,i32)->i32, mc_resume(i32,i32,i32)->i32");
  stats.loadUs = nowUs() - started;
  if (stats.loadUs >= limits.loadWallUs) { ok = false; snprintf(error, capacity, "Wasm loader wall deadline exceeded"); }
  if (ok) {
    uint32_t argv[1]{};
    const uint64_t init = nowUs();
    ok = s.call(s.init, 0, argv) && argv[0] == MC_ABI_VERSION && (s.manifest.count || s.manifest.eventMask);
    stats.initUs = nowUs() - init; stats.instructions = s.meter.used;
    if (stats.initUs >= limits.initWallUs) {
      ok = false;
      snprintf(error, capacity, "Wasm initialization wall deadline exceeded (%llu/%lluus)",
               static_cast<unsigned long long>(stats.initUs),
               static_cast<unsigned long long>(limits.initWallUs));
    } else if (!ok) snprintf(error, capacity, "%s", wasm_runtime_get_exception(s.instance) ?
        wasm_runtime_get_exception(s.instance) : "Wasm ABI/init registration failed");
  }
  stats.elapsedUs = stats.loadUs + stats.initUs;
  memoryStats(stats, sizeof(Impl), s.instance != nullptr);
  if (!ok) clear();
  else s.initializing = false;
  return ok;
}
const BotManifestView &BotWasmSession::manifest() const {
  static constexpr BotManifestView empty{nullptr, 0};
  return impl_ ? impl_->manifest : empty;
}
void BotWasmSession::setGeneration(uint32_t value) { if (impl_) impl_->generation = value; }
void BotWasmSession::setEventEpoch(std::atomic<uint32_t> *value) { if (impl_) impl_->epoch = value; }
bool BotWasmSession::start(uint32_t id, const BotEvent &event, char *error, size_t capacity) {
  const auto fail = [&](const char *message) { snprintf(error, capacity, "%s", message); return false; };
  if (!impl_ || impl_->nextHandle == INT32_MAX || !id ||
      event.kind > BotEvent::NodeStatus || !event.replyLimit || event.replyLimit > BotReplyLimit ||
      !memchr(event.name, 0, sizeof(event.name)) ||
      !memchr(event.arguments, 0, sizeof(event.arguments)) ||
      !memchr(event.message, 0, sizeof(event.message)) || !memchr(event.channel, 0, sizeof(event.channel)))
    return fail("Wasm session/event/job unavailable or invalid");
  auto &s = *impl_;
  Impl::Job *job = nullptr;
  for (auto &j : s.jobs) {
    if (j.state != Impl::Job::Free && j.result.job == id) return fail("Duplicate Wasm invocation ID");
    if (j.state == Impl::Job::Free && !job) job = &j;
  }
  if (!job) return fail("Wasm invocation capacity exhausted");
  uint32_t commandId = 0;
  if (event.kind == BotEvent::Command) {
    const auto *c = s.manifest.find(event.name);
    if (!c) return fail("Wasm command unavailable");
    if (!botCommandAllowed(*c, event)) return fail("Wasm command permission not granted");
    if (!parseBotArguments(*c, event.arguments, job->arguments, error, capacity)) return false;
    commandId = s.ids[c - s.manifest.commands];
  } else {
    commandId = s.events[unsigned(event.kind) - 1];
    if (!commandId) return fail("Wasm event subscription unavailable");
  }
  job->event = event; job->result.job = id; job->handle = ++s.nextHandle;
  job->state = Impl::Job::Running;
  uint32_t argv[2]{job->handle, commandId};
  s.call(s.start, 2, argv, job); return true;
}
bool BotWasmSession::hasPendingIo() const {
  if (impl_) for (const auto &job : impl_->jobs)
    if (job.state == Impl::Job::Waiting && !job.dispatched) return true;
  return false;
}
bool BotWasmSession::nextIo(BotIoRequest &request) {
  if (impl_) for (auto &j : impl_->jobs) if (j.state == Impl::Job::Waiting && !j.dispatched) {
    request = j.request; j.dispatched = true; return true;
  }
  return false;
}
bool BotWasmSession::complete(const BotIoResult &result) {
  if (!memchr(result.value, 0, sizeof(result.value)) || !memchr(result.error, 0, sizeof(result.error)) ||
      !memchr(result.rpcCode, 0, sizeof(result.rpcCode)) ||
      !memchr(result.country, 0, sizeof(result.country)) ||
      !memchr(result.source, 0, sizeof(result.source)) ||
      !memchr(result.observedAt, 0, sizeof(result.observedAt)) ||
      !memchr(result.json, 0, sizeof(result.json)) || !memchr(result.packet.nickname, 0, sizeof(result.packet.nickname)) ||
      !std::isfinite(result.temperatureC) || !std::isfinite(result.packet.rssi) ||
      !std::isfinite(result.packet.snr) || result.keys.count > BotKeysPerScope ||
      result.outcome > BotIoResult::Unknown || result.timerState > BotTimerState::Overdue ||
      result.reminderState > BotReminderState::Overdue ||
      result.trace.count > BotTraceHopLimit || result.packet.path.count > 63 ||
      (result.packet.path.known && !result.packet.path.valid())) return false;
  for (unsigned i = 0; i < result.keys.count; ++i)
    if (!result.keys.keys[i][0] || !memchr(result.keys.keys[i], 0, sizeof(result.keys.keys[i])) ||
        (i && strcmp(result.keys.keys[i - 1], result.keys.keys[i]) >= 0)) return false;
  if (impl_) for (auto &j : impl_->jobs)
    if (j.state == Impl::Job::Waiting && j.dispatched && j.request.token == result.token) {
      if (result.ok && j.request.kind == BotIoRequest::List)
        for (unsigned i = 0; i < result.keys.count; ++i)
          if (strncmp(result.keys.keys[i], j.request.key, strlen(j.request.key))) return false;
      j.completion = result; j.completionKind = j.request.kind;
      if (result.ok && result.packet.id && result.packet.forwardable && result.packet.authenticated && !result.packet.channel &&
          j.request.kind == BotIoRequest::Wait) {
        j.packetHandles[j.operations - 1] = j.operation;
        j.packetIds[j.operations - 1] = result.packet.id;
      }
      j.completed = true;
      j.request.~BotIoRequest(); new (&j.request) BotIoRequest{};
      j.state = Impl::Job::Running;
      uint32_t argv[3]{j.handle, j.operation, result.ok ? 1u : 0u};
      impl_->call(impl_->resume, 3, argv, &j); return true;
    }
  return false;
}
bool BotWasmSession::poll(BotSession::Result &result) {
  if (impl_) for (auto &j : impl_->jobs) if (j.state == Impl::Job::Done) {
    result = j.result;
    // Reset retained storage in place, without a full Job temporary on the VM stack.
    j.~Job(); new (&j) Impl::Job{};
    return true;
  }
  return false;
}
void BotWasmSession::cancel(uint32_t except) {
  if (impl_) for (auto &j : impl_->jobs) if (j.state != Impl::Job::Free && j.result.job != except) {
    j.state = Impl::Job::Done; j.result.ok = false; j.result.action = {};
    strcpy(j.result.error, "Wasm source cancelled; admitted operation outcome may be unknown");
  }
}
void BotWasmSession::cancelEvents() {
  if (impl_) for (auto &j : impl_->jobs) if (j.state != Impl::Job::Free && j.event.kind != BotEvent::Command) {
    j.state = Impl::Job::Done; j.result.ok = false; j.result.action = {};
    strcpy(j.result.error, "Wasm event grant revoked; admitted effects may have committed");
  }
}
} // namespace onchip
#endif
