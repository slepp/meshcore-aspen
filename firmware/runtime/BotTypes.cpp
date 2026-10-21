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

bool splitBotSources(const char *source, size_t size, BotSourcePart *parts,
                     unsigned &count, char *error, size_t capacity) {
  count = 0;
  constexpr char prefix[] = "--@meshcore-sources/1\n";
  if (!source || !size || size > BotLuaSourceLimit || memchr(source, 0, size))
    return fail(error, capacity, "Lua sources require bounded nonempty text");
  if (size < sizeof(prefix) - 1 || memcmp(source, prefix, sizeof(prefix) - 1)) {
    constexpr char marker[] = "--@meshcore-sources";
    if (size >= sizeof(marker) - 1 && !memcmp(source, marker, sizeof(marker) - 1))
      return fail(error, capacity, "Unsupported Lua source-set version");
    strcpy(parts[0].name, "main");
    parts[0].text = source; parts[0].size = size; count = 1;
    return true;
  }
  size_t at = sizeof(prefix) - 1;
  while (at < size) {
    if (count == BotSourcePartLimit)
      return fail(error, capacity, "Lua sources exceed the " ONCHIP_BOT_TEXT(ONCHIP_BOT_MODULE_LIMIT) "-source limit");
    const char *end = static_cast<const char *>(memchr(source + at, '\n', size - at));
    if (!end || size_t(end - source - at) > 64)
      return fail(error, capacity, "Lua source header missing or too long");
    const size_t length = size_t(end - source - at);
    if (length >= 12 && !memcmp(source + at, "--@builtin ", 11)) {
      auto &part = parts[count];
      part = {};
      const size_t nameSize = length - 11;
      if (!nameSize || nameSize > BotNameLimit)
        return fail(error, capacity, "Invalid builtin source name");
      for (size_t i = 0; i < nameSize; ++i) {
        const char c = source[at + 11 + i];
        if (!((c >= 'a' && c <= 'z') || (i && c >= '0' && c <= '9') ||
              (i && (c == '_' || c == '-'))))
          return fail(error, capacity, "Invalid builtin source name");
        part.name[i] = c;
      }
      for (unsigned i = 0; i < count; ++i)
        if (parts[i].builtin || !strcmp(parts[i].name, part.name))
          return fail(error, capacity, "Duplicate builtin or Lua source name");
      part.builtin = true;
      ++count; at = size_t(end - source) + 1;
      continue;
    }
    if (length < 13 || memcmp(source + at, "--@source ", 10))
      return fail(error, capacity, "Expected Lua source name and byte length");
    auto &part = parts[count];
    part = {};
    size_t cursor = at + 10, nameSize = 0;
    while (cursor < size_t(end - source) && source[cursor] != ' ') {
      const char c = source[cursor++];
      if (nameSize == BotNameLimit ||
          !((c >= 'a' && c <= 'z') || (nameSize && c >= '0' && c <= '9') ||
            (nameSize && (c == '_' || c == '-'))))
        return fail(error, capacity, "Invalid Lua source name");
      part.name[nameSize++] = c;
    }
    if (!nameSize || cursor >= size_t(end - source))
      return fail(error, capacity, "Lua source header requires a length");
    ++cursor;
    const size_t start = cursor;
    size_t bytes = 0;
    while (cursor < size_t(end - source)) {
      const char c = source[cursor++];
      if (c < '0' || c > '9' || bytes > BotLuaSourceLimit / 10)
        return fail(error, capacity, "Invalid Lua source byte length");
      bytes = bytes * 10 + unsigned(c - '0');
    }
    at = size_t(end - source) + 1;
    if (cursor == start || !bytes || bytes > size - at ||
        bytes == size - at || source[at + bytes] != '\n')
      return fail(error, capacity, "Lua source length or separator mismatch");
    for (unsigned i = 0; i < count; ++i)
      if (!strcmp(part.name, parts[i].name))
        return fail(error, capacity, "Duplicate Lua source name");
    part.text = source + at; part.size = bytes;
    if (static_cast<unsigned char>(part.text[0]) == 27)
      return fail(error, capacity, "Lua bytecode is not a source");
    ++count; at += bytes + 1;
  }
  return count != 0 || fail(error, capacity, "Lua source set is empty");
}

bool decodeBotRepeaterStats(const uint8_t *bytes, size_t size, BotRepeaterStats &stats) {
  stats = {};
  if (!bytes || size != 56) return false;
  const auto u16 = [&](size_t at) { return uint16_t(bytes[at] | uint16_t(bytes[at + 1]) << 8); };
  const auto u32 = [&](size_t at) {
    return uint32_t(bytes[at]) | uint32_t(bytes[at + 1]) << 8 |
           uint32_t(bytes[at + 2]) << 16 | uint32_t(bytes[at + 3]) << 24;
  };
  stats.batteryMv = u16(0); stats.queued = u16(2);
  stats.noise = int16_t(u16(4)); stats.rssi = int16_t(u16(6));
  stats.received = u32(8); stats.sent = u32(12); stats.txSeconds = u32(16);
  stats.uptimeSeconds = u32(20); stats.sentFlood = u32(24); stats.sentDirect = u32(28);
  stats.receivedFlood = u32(32); stats.receivedDirect = u32(36);
  stats.errors = u16(40); stats.snrQuarterDb = int16_t(u16(42));
  stats.directDuplicates = u16(44); stats.floodDuplicates = u16(46);
  if (size >= 52) stats.rxSeconds = u32(48);
  if (size >= 56) stats.receiveErrors = u32(52);
  return true;
}

const char *botRepeaterErrorName(BotRepeaterError error) {
  switch (error) {
  case BotRepeaterError::None: return "none";
  case BotRepeaterError::Unavailable: return "unavailable";
  case BotRepeaterError::Disabled: return "disabled";
  case BotRepeaterError::NotDue: return "not_due";
  case BotRepeaterError::Busy: return "busy";
  case BotRepeaterError::Clock: return "clock";
  case BotRepeaterError::Capacity: return "capacity";
  case BotRepeaterError::Timeout: return "timeout";
  case BotRepeaterError::Malformed: return "malformed";
  case BotRepeaterError::Cancelled: return "cancelled";
  case BotRepeaterError::Permission: return "permission";
  case BotRepeaterError::Transmission: return "transmission";
  case BotRepeaterError::Frequency: return "frequency";
  }
  return "invalid";
}

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

bool formatDisplayPath(const BotPath &path, char *output, size_t capacity,
                       const uint8_t *widths) {
  if (!path.valid()) return false;
  if (!path.count) {
    constexpr char empty[] = "no repeaters";
    if (capacity < sizeof(empty)) return false;
    strcpy(output, empty);
    return true;
  }
  if (capacity < 2 * path.size() + path.count) return false;
  size_t used = 0;
  for (unsigned hop = 0; hop < path.count; ++hop) {
    if (hop) output[used++] = ',';
    const unsigned width = widths ? widths[hop] : path.width;
    for (unsigned byte = 0; byte < width; ++byte) {
      snprintf(output + used, capacity - used, "%02x", path.bytes[hop * path.width + byte]);
      used += 2;
    }
  }
  return true;
}
bool formatBotTrace(const BotTraceInfo &trace, char *output, size_t capacity,
                    size_t replyLimit, bool &truncated) {
  if (!capacity || trace.count > BotTraceHopLimit) return false;
  const size_t limit = std::min(capacity - 1, replyLimit);
  const int header = snprintf(output, capacity, "Trace %u hops; SNR dB:", unsigned(trace.count));
  if (header < 1 || size_t(header) > limit) return false;
  const auto measurement = [&](unsigned i, char (&text)[16]) {
    snprintf(text, sizeof(text), "%.2f", double(trace.snr[i]) / 4);
    size_t size = strlen(text);
    while (size && text[size - 1] == '0') text[--size] = 0;
    if (size && text[size - 1] == '.') text[--size] = 0;
    return size;
  };
  size_t required = size_t(header);
  for (unsigned i = 0; i < trace.count; ++i) {
    char text[16]; required += 1 + measurement(i, text);
  }
  truncated = required > limit;
  constexpr char suffix[] = "; truncated";
  const size_t reserve = truncated ? sizeof(suffix) - 1 : 0;
  if (size_t(header) + reserve > limit) return false;
  size_t used = size_t(header);
  for (unsigned i = 0; i < trace.count; ++i) {
    char text[16]; const size_t size = measurement(i, text);
    if (used + 1 + size + reserve > limit) break;
    output[used++] = i ? ',' : ' ';
    memcpy(output + used, text, size); used += size;
  }
  if (truncated) { memcpy(output + used, suffix, reserve); used += reserve; }
  output[used] = 0;
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
  uint8_t widths[BotObservationLimit][BotPathLimit]{};
  for (unsigned i = 0; i < event.observationCount; ++i) {
    if (!event.observations[i].valid()) return fail(output, capacity, "Invalid observed path");
    hops[i] = event.observations[i].count;
    memset(widths[i], event.observations[i].width, hops[i]);
  }
  const int header = snprintf(output, capacity, "Paths: %u in %us",
                              unsigned(event.observationCount), unsigned(event.windowMs / 1000));
  const auto length = [&](unsigned i, unsigned count) {
    const auto &path = event.observations[i];
    if (!path.count) return 12;
    int size = int(count) - 1 + (count < path.count ? 3 : 0);
    for (unsigned hop = 0; hop < count; ++hop) size += 2 * widths[i][hop];
    return size;
  };
  bool truncated = event.truncated;
  unsigned shown = event.observationCount;
  const auto required = [&] {
    int size = header + int(3 * shown) - 1 + (truncated ? 11 : 0);
    for (unsigned i = 0; i < shown; ++i) size += length(i, hops[i]);
    return size;
  };
  if (required() > limit) {
    struct Hop { uint8_t path, hop; } prefixes[BotObservationLimit * (BotPathLimit / 2)];
    unsigned count = 0;
    for (unsigned i = 0; i < shown; ++i) {
      const auto &path = event.observations[i];
      if (path.width < 2) continue;
      for (unsigned hop = 0; hop < path.count; ++hop)
        prefixes[count++] = {uint8_t(i), uint8_t(hop)};
    }
    const auto bytes = [&](const Hop &hop) {
      const auto &path = event.observations[hop.path];
      return path.bytes + path.width * hop.hop;
    };
    const auto key = [&](const Hop &hop) {
      const auto *p = bytes(hop);
      return unsigned(p[0]) * 256 + p[1];
    };
    std::sort(prefixes, prefixes + count, [&](const Hop &a, const Hop &b) { return key(a) < key(b); });
    for (unsigned first = 0; first < count;) {
      unsigned end = first + 1;
      while (end < count && key(prefixes[end]) == key(prefixes[first])) ++end;
      bool collision = false;
      for (unsigned at = first; at < end; ++at)
        if (event.observations[prefixes[at].path].width == 2 ||
            bytes(prefixes[at])[2] != bytes(prefixes[first])[2]) {
          collision = true; break;
        }
      if (!collision)
        for (unsigned at = first; at < end; ++at)
          widths[prefixes[at].path][prefixes[at].hop] = 2;
      first = end;
    }
  }
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
    char text[3 * BotPathLimit + 1]{};
    if (!formatDisplayPath(path, text, sizeof(text), widths[i])) return fail(output, capacity, "Invalid observed path");
    const char *suffix = hops[i] < event.observations[i].count ? "..." : "";
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
function path() return node.report("path") end
function test() return node.report("test") end
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
  local advice=r.state=="unknown" and "; TX uncertain; inspect !reminders" or ""
  return "#"..tostring(r.id).." "..r.state..advice
end
local function personal()
  return ctx.sender.authenticated and not ctx.channel.present and not ctx.radio.local_reflection
end
local private="Error: personal notes require an authenticated private DM"
function remember(key,text)
  if not personal() then return private end
  local ok,state=kv.put(key,text)
  return "Saved note "..key
end
function recall(key)
  if not personal() then return private end
  local value,safe=kv.get(key)
  if not value then return "No note: "..key end
  if not safe then return "Error: note contains unsupported text or exceeds the reply size" end
  return value
end
function forget(key)
  if not personal() then return private end
  local ok,state=kv.delete(key)
  if state=="absent" then return "No note: "..key end
  return "Deleted note "..key
end
function notes(prefix)
  if not personal() then return private end
  local r=kv.list(prefix)
  if not r.rf_safe then return "Error: note names contain unsupported text" end
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
command("test","","Received route, SNR and RSSI")
command("path","","Repeater hops in the received flood path")
command("mt","seconds?:int:1:30","Observe unique paths; default 5s","mt","public","!mt 5")
command("trace","route?:string:154","Send TRACE along width:hex or the received route")
command("remind","duration:string:10,text:text:120","Private reminder; needs trusted time and a route")
command("reminders","","List your reminders; sent means radio transmission")
command("cancel","id:string:10","Cancel a reminder by ID; uncertain transmissions stay unknown")
command("remember","key:string:32,text:text:120","Save a private note by key; replaces an existing note")
command("recall","key:string:32","Read a private note by key")
command("forget","key:string:32","Delete a private note by key")
command("notes","prefix?:string:32","List your notes; use PREFIX to shorten the list")
command("list-memories","prefix?:string:32","Alias of notes","notes")
)lua";
static_assert(sizeof(BotDefaultSource) - 1 <= BotLuaSourceLimit, "Bundled source exceeds the Lua source limit");
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
command("weather","place?:text:80","Weather for PLACE from the configured home service","weather","home","!weather Calgary")
command("service","name:string:7,args?:text:120","Home service: health, echo TEXT or weather PLACE","service","home","!service health")
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
    if not r.rf_safe then return "Error: board entry names contain unsupported text" end
    if r.count==0 then return "No board entries" end
    local out="Board ("..tostring(r.count).."):"
    local more="; truncated; narrow prefix"
    local size=#out
    for i=1,r.count do
      if #prefix+#r.suffixes[i]==0 then return "Error: board entry has an empty name" end
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
    return "Saved board entry "..key
  end
  if value then return "Error: board get/delete take only a key" end
  if action=="get" then
    local text,safe=kv.get(stored,"channel")
    if not text then return "No board entry: "..key end
    if not safe then return "Error: board entry contains unsupported text or exceeds the reply size" end
    return text
  end
  local ok,state=kv.delete(stored,"channel")
  if state=="absent" then return "No board entry: "..key end
  return "Deleted board entry "..key
end
command("board","action:string:6,key?:string:26,text?:text:120","Channel board: put KEY TEXT, get KEY, list [PREFIX], delete KEY","board","shared","!@BOTKEY8 board put plan lunch")
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
command("about","","Bot introduction and command help")
command("version","","Product release, MeshCore and Lua versions, build date")
command("uptime","","Elapsed time since boot")
command("status","","Bot readiness, radio roles and WiFi")
command("signal","","Received signal strength and repeater hops")
command("air","page?:int:1:4","Radio airtime, queues and transmission counters","air","public","!air 2")
command("help","name?:string:24,page?:int:1:40","List commands or show usage; !help NAME [PAGE]","help","public","!help remember")
command("plugins","page?:int:1:4","Loaded commands, Lua modules and scheduled events","plugins","public","!plugins 2")
command("neighbors","page?:int:1:16","Heard adverts, signal strength and cached routes","neighbors","public","!neighbors 2")
command("find-path","prefix:string:64","Fresh route; waits up to 30s, probes 10s","find_path","public","!find-path d2")
command("admin","text?:text:155","Trusted-owner DM: bot status/help/cancel","admin","owner","!admin bot status")
)lua"
#if !ONCHIP_BOT_COMPACT_PROFILE
R"lua(
function find_best_path(prefix) return node.find_best_path(prefix) end
command("find-best-path","prefix:string:64","Compare routes; two probes each, up to 5min","find_best_path","public","!find-best-path d2")
)lua"
#endif
;
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
  if (!strcmp(name, "path")) {
    if (!e.path.valid()) {
      size = snprintf(output, capacity, "Error: received direct/local packet has no complete flood path");
      return size > 0 && size < int(capacity) && size <= e.replyLimit;
    }
    auto path = e.path;
    const size_t available = std::min(capacity, size_t(e.replyLimit) + 1);
    size_t reserve = 0;
    while (!formatDisplayPath(path, output, available > reserve ? available - reserve : 0)) {
      if (path.count <= 1) return error("Path exceeds the reply size");
      --path.count; reserve = 3;
    }
    if (reserve) strcat(output, "...");
    return true;
  }
  if (!strcmp(name, "signal") || !strcmp(name, "test")) {
    const bool test = !strcmp(name, "test");
    char signal[64], path[3 * BotPathLimit + 1];
    if (e.local) strcpy(signal, "local reflection; RF unmeasured");
    else if (!e.signal) strcpy(signal, "RF measurement unavailable");
    else snprintf(signal, sizeof(signal), "SNR %.6g dB; RSSI %.6g dBm", double(e.snr), double(e.rssi));
    if (!formatDisplayPath(e.path, path, sizeof(path)))
      strcpy(path, "unknown (direct)");
    size = snprintf(output, capacity, "%s: %s; path %s", test ? "Connected" : "Signal", signal, path);
    if (size >= int(capacity) || size > e.replyLimit)
      size = snprintf(output, capacity, "%s: %s; path omitted; use !path", test ? "Connected" : "Signal", signal);
  } else if (!strcmp(name, "air")) {
    if (page < 1 || page > 4) return error("Use !air [1..4]");
    if (!a.available) size = snprintf(output, capacity, "Error: scheduler snapshot unavailable");
    else if (page == 1)
      size = snprintf(output, capacity, "Airtime ms: credit bot=%u all=%u; 60s reserved=%u left=%u; !air 2",
                      unsigned(a.creditMs), unsigned(a.aggregateCreditMs), unsigned(a.reservedMs), unsigned(a.remainingMs));
    else if (page == 2)
      size = snprintf(output, capacity, "Airtime: queue bot=%u all=%u TX=%s; RF ms bot=%u all=%u; !air 3",
                      unsigned(a.queued), unsigned(a.aggregateQueued), a.transmitting ? "yes" : "no",
                      unsigned(a.rfMs), unsigned(a.aggregateRfMs));
    else if (page == 3)
      size = snprintf(output, capacity, "Airtime TX ok/fail: bot=%u/%u all=%u/%u; !air 4",
                      unsigned(a.successes), unsigned(a.failures), unsigned(a.aggregateSuccesses), unsigned(a.aggregateFailures));
    else
      size = snprintf(output, capacity, "Airtime: gen=%u cfg=%u; 60s limit=%u ms; counters wrap/reset",
                      unsigned(a.generation), unsigned(a.configurationGeneration), unsigned(a.limitMs));
  } else if (!strcmp(name, "about")) {
    size = snprintf(output, capacity, "Mesh command bot; !help lists commands; !help NAME shows usage; !plugins shows loaded commands");
  } else if (!strcmp(name, "version")) {
    if (!n.available || !n.nativeRevision[0] || !n.build[0])
      size = snprintf(output, capacity, "Error: build metadata unavailable");
    else if (n.product[0])
      size = snprintf(output, capacity, "Version: %s; MeshCore %s; Lua %s; built %s",
                      n.product, n.nativeRevision, luaVersion, n.build);
    else size = snprintf(output, capacity, "Version: MeshCore %s; Lua %s; built %s",
                         n.nativeRevision, luaVersion, n.build);
  } else if (!strcmp(name, "uptime")) {
    if (!n.available) size = snprintf(output, capacity, "Error: uptime unavailable");
    else {
      const uint64_t s = n.uptimeMs / 1000;
      size = snprintf(output, capacity, "Uptime: %llud %uh %um %us",
                      static_cast<unsigned long long>(s / 86400), unsigned(s / 3600 % 24),
                      unsigned(s / 60 % 60), unsigned(s % 60));
    }
  } else if (!strcmp(name, "status")) {
    if (!n.available) size = snprintf(output, capacity, "Error: node snapshot unavailable");
    else {
      char roles[40] = "roles unavailable", waiting[40]{};
      if (n.rolesKnown) {
        unsigned selected = 0, ready = 0;
        const char *names[] = {"repeater", "room", "companion", "observer"};
        for (unsigned i = 0; i < 4; ++i) if (n.selectedRoles & (1u << i)) {
          ++selected;
          if (n.readyRoles & (1u << i)) ++ready;
          else {
            if (waiting[0]) strcat(waiting, ",");
            strcat(waiting, names[i]);
          }
        }
        if (selected) snprintf(roles, sizeof(roles), "roles %u/%u ready", ready, selected);
        else strcpy(roles, "no roles selected");
      }
      size = snprintf(output, capacity, "Bot %s%s; WiFi %s; %s",
                      !n.enabled ? "off" : n.ready ? "ready" : "not ready", n.fault ? "; fault" : "",
                      !n.wifiKnown ? "unavailable" : n.wifiConnected ? "connected" : "disconnected", roles);
      if (waiting[0] && size > 0 && size_t(size) + 10 + strlen(waiting) < capacity &&
          size_t(size) + 10 + strlen(waiting) <= e.replyLimit)
        size += snprintf(output + size, capacity - size, "; waiting %s", waiting);
    }
  } else return error("Unsupported diagnostic name");
  if (size < 1 || size >= int(capacity) || size > e.replyLimit)
    return error("Diagnostic exceeds RF capacity");
  for (int i = 0; i < size; ++i)
    if (output[i] < 32 || output[i] > 126) return error("Diagnostic contains non-printable metadata");
  return true;
}
} // namespace onchip
