// SPDX-License-Identifier: Apache-2.0
#include "BotTypes.h"
#include <algorithm>
#include <stdio.h>
#include <string.h>
#include <type_traits>

namespace onchip {
namespace {
bool fail(char *error, size_t size, const char *message) {
  snprintf(error, size, "%s", message);
  return false;
}
int hex(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
} // namespace

void resetBotIoResult(BotIoResult &result) {
  static_assert(std::is_trivially_copyable<BotIoResult>::value, "Bot I/O result must remain copied data");
  static const BotIoResult Empty{};
  memcpy(&result, &Empty, sizeof(result));
}

const char *botTimerStateName(BotTimerState state) {
  switch (state) {
  case BotTimerState::Missing: return "missing";
  case BotTimerState::Pending: return "pending";
  case BotTimerState::Claimed: return "claimed";
  case BotTimerState::Cancelled: return "cancelled";
  case BotTimerState::Overdue: return "overdue";
  }
  return "invalid";
}
const char *botReminderStateName(BotReminderState state) {
  switch (state) {
  case BotReminderState::Missing: return "missing";
  case BotReminderState::Pending: return "pending";
  case BotReminderState::Unknown: return "unknown";
  case BotReminderState::Sent: return "sent";
  case BotReminderState::Cancelled: return "cancelled";
  case BotReminderState::Overdue: return "overdue";
  }
  return "invalid";
}

bool formatBotPath(const BotPath &path, char *output, size_t capacity) {
  if (!path.valid() || capacity < 4 + 2 * path.size()) return false;
  snprintf(output, capacity, "%u:", path.width);
  for (size_t i = 0; i < path.size(); ++i)
    snprintf(output + 2 + 2 * i, capacity - 2 - 2 * i, "%02x", path.bytes[i]);
  return true;
}

namespace {
bool formatObservedPaths(const BotEvent &event, char *output, size_t capacity) {
  if (!capacity) return false;
  const int limit = int(std::min(capacity - 1, size_t(event.replyLimit)));
  if (!event.observationCount) {
    const int size = snprintf(output, capacity, "Error: no complete flood path observed; direct/local path unknown");
    return size > 0 && size <= limit;
  }
  if (event.observationCount > BotObservationLimit)
    return fail(output, capacity, "Invalid observation count");
  uint8_t hops[BotObservationLimit]{};
  for (unsigned i = 0; i < event.observationCount; ++i) {
    if (!event.observations[i].valid()) return fail(output, capacity, "Invalid observed path");
    hops[i] = event.observations[i].count;
  }
  const int header = snprintf(output, capacity, "%u unique paths in %u ms",
                              unsigned(event.observationCount), unsigned(event.windowMs));
  const auto length = [&](unsigned i, unsigned count) {
    const auto &path = event.observations[i];
    return 2 + 2 * path.width * int(count) + (!path.count ? 9 : count < path.count ? 3 : 0);
  };
  bool truncated = event.truncated;
  unsigned shown = event.observationCount;
  const auto required = [&] {
    int size = header + int(3 * shown) - 1 + (truncated ? 11 : 0);
    for (unsigned i = 0; i < shown; ++i) size += length(i, hops[i]);
    return size;
  };
  if (required() > limit) {
    truncated = true;
    for (unsigned i = 0; i < shown; ++i)
      if (hops[i] && length(i, 1) < length(i, hops[i])) hops[i] = 1;
    while (shown && required() > limit) --shown;
    if (!shown) return fail(output, capacity, "Observed paths exceed reply capacity");
    int used = required();
    // Share remaining space across routes, retaining whole observed hash segments.
    for (bool progress = true; progress;) {
      progress = false;
      for (unsigned i = 0; i < shown; ++i) {
        if (hops[i] == event.observations[i].count) continue;
        const int extra = length(i, hops[i] + 1) - length(i, hops[i]);
        if (used + extra <= limit) {
          ++hops[i]; used += extra; progress = true;
        }
      }
    }
  }
  if (header < 1 || header > limit) return fail(output, capacity, "Observed paths exceed reply capacity");
  size_t used = size_t(header);
  for (unsigned i = 0; i < shown; ++i) {
    auto path = event.observations[i];
    path.count = hops[i];
    char text[4 + 2 * BotPathLimit]{};
    if (!formatBotPath(path, text, sizeof(text))) return fail(output, capacity, "Invalid observed path");
    const char *suffix = !event.observations[i].count ? " (no-hop)" :
                         hops[i] < event.observations[i].count ? "..." : "";
    used += snprintf(output + used, capacity - used, "%s%s%s", i ? " | " : "; ", text, suffix);
  }
  if (truncated) used += snprintf(output + used, capacity - used, "; truncated");
  return used <= size_t(limit);
}
} // namespace

bool parseBotCommand(const char *text, size_t size, BotEvent &event,
                     char *error, size_t errorSize) {
  if (!text || size < 2 || size > BotTextLimit ||
      text[0] != '!')
    return fail(error, errorSize, "Invalid command length or prefix");
  for (size_t i = 0; i < size; ++i)
    if (static_cast<unsigned char>(text[i]) < 32 ||
        static_cast<unsigned char>(text[i]) > 126)
      return fail(error, errorSize, "Command must be printable ASCII");
  size_t end = 1;
  while (end < size && text[end] != ' ') ++end;
  if (end == 1 || end - 1 > BotNameLimit)
    return fail(error, errorSize, "Command name exceeds 24 bytes");
  for (size_t i = 1; i < end; ++i) {
    char c = text[i];
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
          c == '_' || c == '-'))
      return fail(error, errorSize, "Invalid command name");
    event.name[i - 1] = c;
  }
  event.name[end - 1] = 0;
  while (end < size && text[end] == ' ') ++end;
  while (size > end && text[size - 1] == ' ') --size;
  if (size - end > BotArgumentsLimit)
    return fail(error, errorSize, "Command arguments exceed native text capacity");
  memcpy(event.arguments, text + end, size - end);
  event.arguments[size - end] = 0;
  if (!strcmp(event.name, "mt")) {
    unsigned seconds = 5;
    if (event.arguments[0]) {
      seconds = 0;
      for (const char *p = event.arguments; *p; ++p) {
        if (*p < '0' || *p > '9' || seconds > 30)
          return fail(error, errorSize, "Use !mt [1..30 seconds]");
        seconds = seconds * 10 + unsigned(*p - '0');
      }
    }
    if (!seconds || seconds > 30)
      return fail(error, errorSize, "Use !mt [1..30 seconds]");
    event.windowMs = seconds * 1000;
  }
  if (!strcmp(event.name, "trace") && event.arguments[0]) {
    const char *p = event.arguments;
    const unsigned width = unsigned(p[0] - '0');
    if ((width != 1 && width != 2 && width != 4 && width != 8) || p[1] != ':')
      return fail(error, errorSize, "Use !trace width:hex (width 1,2,4,8)");
    const size_t digits = strlen(p + 2);
    if (!digits || digits > 2 * BotTraceLimit || digits % (2 * width) ||
        digits / (2 * width) > BotTraceHopLimit)
      return fail(error, errorSize, "Invalid TRACE route length");
    for (size_t i = 0; i < digits / 2; ++i) {
      const int hi = hex(p[2 + i * 2]), lo = hex(p[3 + i * 2]);
      if (hi < 0 || lo < 0)
        return fail(error, errorSize, "Invalid TRACE route hex");
      event.route[i] = uint8_t(hi * 16 + lo);
    }
    event.routeWidth = width;
    event.routeSize = digits / 2;
    event.routeExplicit = true;
  }
  return true;
}

const char BotDefaultSource[] = R"lua(
function ping() return "Pong" end
function path()
  if not ctx.packet.path_known then return "Error: received direct packet has no complete heard path" end
  return ctx.packet.path
end
function test()
  local path=ctx.packet.path
  if not ctx.packet.path_known then path="unknown (direct)" end
  local signal="RSSI/SNR unavailable"
  if ctx.radio.local_reflection then signal="local; "..signal end
  if ctx.radio.measured then
    signal="RSSI="..tostring(ctx.radio.rssi_dbm).." dBm SNR="..tostring(ctx.radio.snr_db).." dB"
  end
  local result="Connected; path "..path.."; "..signal
  if #result>ctx.limits.reply_bytes then return "Error: path too long for one !test reply; use !path" end
  return result
end
function mt(seconds)
  return node.report("mt")
end
function trace(route)
  if not route and not ctx.packet.trace_available then return "Error: TRACE requires an explicit route or a received flood path (width 1/2)" end
  return request_trace(route)
end
function remind(duration,text)
  local r=reminder.after(duration,text)
  return "#"..tostring(r.id).." pending; due UTC "..tostring(r.deadline_utc)
end
function reminders() return reminder.list() end
function cancel(id)
  local r=reminder.cancel(id)
  local advice=r.state=="unknown" and "; TX uncertain; do not reschedule blindly" or "; sent means TX, not delivery"
  return "#"..tostring(r.id).." "..r.state..advice
end
local function personal()
  return ctx.sender.authenticated and not ctx.channel.present and not ctx.radio.local_reflection
end
local private="Error: personal notes require an authenticated private DM"
function remember(key,text)
  if not personal() then return private end
  local ok,state=kv.put(key,text)
  return "Note "..key.." "..state.."; committed"
end
function recall(key)
  if not personal() then return private end
  local value,safe=kv.get(key)
  if not value then return "No note: "..key end
  if not safe then return "Error: note empty, non-ASCII or exceeds RF capacity; ask owner to inspect data" end
  return value
end
function forget(key)
  if not personal() then return private end
  local ok,state=kv.delete(key)
  if state=="absent" then return "No note: "..key end
  return "Note "..key.." deleted; committed"
end
function notes(prefix)
  if not personal() then return private end
  local r=kv.list(prefix)
  if not r.rf_safe then return "Error: non-ASCII note keys; ask owner to inspect stored data" end
  if r.count==0 then return "No notes" end
  local out="Notes ("..tostring(r.count).."):"
  local more="; truncated; narrow prefix"
  local size=#out
  for i=1,r.count do size=size+#r.keys[i]+3 end
  local reserve=0
  if size>ctx.limits.reply_bytes then reserve=#more end
  for i=1,r.count do
    local item=" ["..r.keys[i].."]"
    if #out+#item+reserve>ctx.limits.reply_bytes then return out..more end
    out=out..item
  end
  return out
end
command("ping","","Reply Pong")
command("test","","Heard path and measured RSSI/SNR; local marked")
command("path","","Received flood path; direct path may be unknown")
command("mt","seconds?:int:1:30","Observe unique paths; default 5s","mt","public","!mt 5")
command("trace","route?:string:154","TRACE width:hex or heard route")
command("remind","duration:string:10,text:text:120","DM; grant, trusted time and direct route")
command("reminders","","Personal IDs/states/deadlines; sent is TX only")
command("cancel","id:string:10","Cancel personal ID; unknown is uncertain TX")
command("remember","key:string:32,text:text:120","Private note; overwrites key")
command("recall","key:string:32","Read private key")
command("forget","key:string:32","Delete private key")
command("notes","prefix?:string:32","Private keys; narrow prefix if truncated")
command("list-memories","prefix?:string:32","Alias of notes","notes")
)lua";
static_assert(sizeof(BotDefaultSource) - 1 <= BotSourceLimit, "Bundled source exceeds staging capacity");
const char BotNetworkSource[] = R"lua(
local function home(name,args)
  local r=rpc.call("home",name,args)
  if not r.ok then
    local advice=r.error.code=="unknown" and "; do not retry blindly" or ""
    return "Home "..name.." error: "..r.error.code.."; "..rpc.text(r.error.message,64)..advice
  end
  local v=r.result
  if name=="health" then return "Home health: "..v.status end
  if name=="echo" then return "Home echo: "..rpc.text(v.text,120) end
  local out="Weather "..rpc.text(v.location,80)..": "..tostring(v.temperature_c)..
    " C; code "..tostring(v.weather_code).."; age "..tostring(v.source_age_seconds).."s"
  if #out>ctx.limits.reply_bytes then return "Error: weather reply exceeds RF capacity" end
  return out
end
function weather(place)
  if not place or #place==0 then return "Error: use !weather <place>; no default place configured" end
  if #place>80 then return "Error: weather place exceeds 80 bytes" end
  return home("weather",{place=place})
end
function service(name,args)
  if name=="weather" then return weather(args) end
  if name=="health" then
    if args then return "Error: service health takes no arguments" end
    return home("health",{})
  end
  if name=="echo" then
    if not args or #args==0 or #args>120 then return "Error: service echo requires 1..120 text bytes" end
    return home("echo",{text=args})
  end
  return "Error: service supports health, echo or weather only"
end
command("weather","place?:text:80","Configured home weather; DM/grant; place required","weather","home","!weather Calgary")
command("service","name:string:7,args?:text:120","DM/grant; health, echo TEXT or weather PLACE","service","home","!service health")
)lua";
static_assert(sizeof(BotNetworkSource) <= 2048, "Keep compiled RPC commands independently bounded");
const char BotBoardSource[] = R"lua(
function board(action,key,value)
  if not ctx.channel.present or not ctx.channel.verified or ctx.sender.authenticated or ctx.radio.local_reflection then
    return "Error: board requires the selected verified channel; DMs unsupported"
  end
  if key and #key>26 then return "Error: board key/prefix exceeds 26 bytes" end
  if action=="list" then
    if value then return "Error: use !board list [prefix]" end
    local prefix=key or ""
    local r=kv.list("board:"..prefix,"channel")
    if not r.rf_safe then return "Error: non-ASCII board keys; ask owner to inspect stored data" end
    if r.count==0 then return "No board entries" end
    local out="Board ("..tostring(r.count).."):"
    local more="; truncated; narrow prefix"
    local size=#out
    for i=1,r.count do
      if #prefix+#r.suffixes[i]==0 then return "Error: empty board key; ask owner to inspect stored data" end
      size=size+#prefix+#r.suffixes[i]+3
    end
    local reserve=0
    if size>ctx.limits.reply_bytes then reserve=#more end
    for i=1,r.count do
      local item=" ["..prefix..r.suffixes[i].."]"
      if #out+#item+reserve>ctx.limits.reply_bytes then return out..more end
      out=out..item
    end
    return out
  end
  if action~="put" and action~="get" and action~="delete" then
    return "Error: board supports put, get, list or delete"
  end
  if not key or #key==0 then return "Error: board requires a key" end
  local stored="board:"..key
  if action=="put" then
    if not value or #value==0 or #value>120 then return "Error: board put requires 1..120 text bytes" end
    local ok,state=kv.put(stored,value,"channel")
    return "Board "..key.." "..state.."; committed"
  end
  if value then return "Error: board get/delete take only a key" end
  if action=="get" then
    local text,safe=kv.get(stored,"channel")
    if not text then return "No board entry: "..key end
    if not safe then return "Error: board value empty, non-ASCII or exceeds RF capacity; ask owner to inspect data" end
    return text
  end
  local ok,state=kv.delete(stored,"channel")
  if state=="absent" then return "No board entry: "..key end
  return "Board "..key.." deleted; committed"
end
command("board","action:string:6,key?:string:26,text?:text:120","Target/grant: put KEY TEXT|get KEY|list [PREFIX]|delete KEY","board","shared","!@BOTKEY8 board put plan lunch")
)lua";
static_assert(sizeof(BotBoardSource) <= 3072, "Keep compiled board commands independently bounded");
const char BotDiagnosticSource[] = R"lua(
function about() return node.report("about") end
function version() return node.report("version") end
function uptime() return node.report("uptime") end
function status() return node.report("status") end
function signal() return node.report("signal") end
function air(page) return node.report("air",page) end
function help(name,page) return command_help(name,page) end
function plugins(page) return node.plugins(page) end
function neighbors(page) return node.neighbors(page) end
function admin(text) return node.admin(text or "bot help") end
command("about","","Bot full public key")
command("version","","Native/Lua build; image hash unavailable")
command("uptime","","Elapsed time since boot")
command("status","","Bot readiness, roles and WiFi snapshot")
command("signal","","Request RSSI/SNR/path; local reflection marked")
command("air","page?:int:1:4","Scheduler ms/queues/TX counters; not delivery","air","public","!air 2")
command("help","name?:string:24,page?:int:1:40","Commands/arguments; !help PAGE or NAME [PAGE]","help","public","!help remember")
command("plugins","page?:int:1:4","Active source/manifest/modules; shared Lua namespace","plugins","public","!plugins 2")
command("neighbors","page?:int:1:16","Signed adverts/cached routes; not live adjacency","neighbors","public","!neighbors 2")
command("admin","text?:text:155","Trusted-owner DM: bot status/help/cancel","admin","owner","!admin bot status")
)lua";
static_assert(sizeof(BotDiagnosticSource) <= 2048, "Keep compiled diagnostics independently bounded");
bool formatBotDiagnostic(const BotEvent &e, const char *name, unsigned page,
                         const char *luaVersion, char *output, size_t capacity) {
  const auto error = [&](const char *message) {
    snprintf(output, capacity, "%s", message); return false;
  };
  if (!(e.authenticated ? !e.channel[0] : e.channelVerified && e.channel[0]))
    return error("Diagnostics require authenticated DM or verified selected channel");
  if (!strcmp(name, "mt")) return formatObservedPaths(e, output, capacity);
  const auto &n = e.node;
  const auto &a = e.air;
  int size = 0;
  if (!strcmp(name, "signal")) {
    char signal[64], path[4 + 2 * BotPathLimit];
    if (e.local) strcpy(signal, "local reflection; RF unmeasured");
    else if (!e.signal) strcpy(signal, "RF measurement unavailable");
    else snprintf(signal, sizeof(signal), "RSSI=%.6gdBm SNR=%.6gdB", double(e.rssi), double(e.snr));
    if (!formatBotPath(e.path, path, sizeof(path))) strcpy(path, "unknown (direct)");
    size = snprintf(output, capacity, "Packet %s; path=%s", signal, path);
    if (size >= int(capacity) || size > e.replyLimit)
      size = snprintf(output, capacity, "Packet %s; path omitted; use !path", signal);
  } else if (!strcmp(name, "air")) {
    if (page < 1 || page > 4) return error("Use !air [1..4]");
    if (!a.available) size = snprintf(output, capacity, "Error: scheduler snapshot unavailable");
    else if (page == 1)
      size = snprintf(output, capacity, "Air snapshot ms: credit bot=%u all=%u; bot60s reserved=%u left=%u; more !air 2",
                      unsigned(a.creditMs), unsigned(a.aggregateCreditMs), unsigned(a.reservedMs), unsigned(a.remainingMs));
    else if (page == 2)
      size = snprintf(output, capacity, "Air snapshot q bot=%u all=%u tx=%s; RFms bot=%u all=%u; more !air 3",
                      unsigned(a.queued), unsigned(a.aggregateQueued), a.transmitting ? "yes" : "no",
                      unsigned(a.rfMs), unsigned(a.aggregateRfMs));
    else if (page == 3)
      size = snprintf(output, capacity, "Air snapshot TX ok/fail bot=%u/%u all=%u/%u; more !air 4",
                      unsigned(a.successes), unsigned(a.failures), unsigned(a.aggregateSuccesses), unsigned(a.aggregateFailures));
    else
      size = snprintf(output, capacity, "Air gen=%u cfg=%u; bot60s limit=%ums; u32 totals wrap/reset; history/duty unavailable",
                      unsigned(a.generation), unsigned(a.configurationGeneration), unsigned(a.limitMs));
  } else if (!strcmp(name, "about")) {
    if (!n.available || !n.hasIdentity) size = snprintf(output, capacity, "Error: bot identity unavailable");
    else {
      char key[65];
      for (unsigned i = 0; i < 32; ++i) snprintf(key + 2 * i, 3, "%02x", n.publicKey[i]);
      size = snprintf(output, capacity, "mc-onchip/command-bot; key=%s", key);
    }
  } else if (!strcmp(name, "version")) {
    if (!n.available || !n.nativeRevision[0] || !n.build[0])
      size = snprintf(output, capacity, "Error: build metadata unavailable");
    else size = snprintf(output, capacity, "MeshCore %s; Lua %s; compiled %s; image hash unavailable",
                         n.nativeRevision, luaVersion, n.build);
  } else if (!strcmp(name, "uptime")) {
    if (!n.available) size = snprintf(output, capacity, "Error: uptime unavailable");
    else {
      const uint64_t s = n.uptimeMs / 1000;
      size = snprintf(output, capacity, "Uptime snapshot since boot: %llud %uh %um %us",
                      static_cast<unsigned long long>(s / 86400), unsigned(s / 3600 % 24),
                      unsigned(s / 60 % 60), unsigned(s % 60));
    }
  } else if (!strcmp(name, "status")) {
    if (!n.available) size = snprintf(output, capacity, "Error: node snapshot unavailable");
    else {
      char roles[40] = "unavailable";
      if (n.rolesKnown) snprintf(roles, sizeof(roles), "%u/%u (R1 room2 C4 O8)", n.selectedRoles, n.readyRoles);
      size = snprintf(output, capacity, "Snapshot bot=%s fault=%s WiFi=%s; battery=unavailable; roles selected/ready=%s",
                      !n.enabled ? "off" : n.ready ? "ready" : "not-ready", n.fault ? "yes" : "no",
                      !n.wifiKnown ? "unavailable" : n.wifiConnected ? "connected" : "disconnected", roles);
    }
  } else return error("Unsupported diagnostic name");
  if (size < 1 || size >= int(capacity) || size > e.replyLimit)
    return error("Diagnostic exceeds RF capacity");
  for (int i = 0; i < size; ++i)
    if (output[i] < 32 || output[i] > 126) return error("Diagnostic contains non-printable metadata");
  return true;
}
} // namespace onchip
