// SPDX-License-Identifier: Apache-2.0
#include "PacketPrograms.h"
#include <cstdio>
#include <cstring>
#if MESHCORE_ONCHIP_BOT
#include "PacketLua.h"
#if ONCHIP_BOT_WASM
#include "PacketWasm.h"
#endif
#include <new>
#endif

namespace onchip {
namespace {
#if MESHCORE_ONCHIP_BOT
class VmProgram final : public PacketProgram {
  PacketProgramRuntime runtime_ = PacketProgramRuntime::Lua;
  PacketLua lua_;
#if ONCHIP_BOT_WASM
  PacketWasm wasm_;
#endif
public:
  bool load(PacketProgramRuntime runtime, const uint8_t *bytes, size_t size,
            char *error, size_t capacity) override {
    runtime_ = runtime;
    if (runtime == PacketProgramRuntime::Lua)
      return lua_.load(reinterpret_cast<const char *>(bytes), size, error, capacity);
#if ONCHIP_BOT_WASM
    return wasm_.load(bytes, size, error, capacity);
#else
    snprintf(error, capacity, "packet Wasm runtime unavailable in this image"); return false;
#endif
  }
  packet_engine::Decision process(const packet_engine::Metadata &m, packet_engine::Call &c) override {
    if (runtime_ == PacketProgramRuntime::Lua) return lua_.process(m, c);
#if ONCHIP_BOT_WASM
    return wasm_.process(m, c);
#else
    c.fail(packet_engine::Fault::Unavailable); return packet_engine::Decision::Failed;
#endif
  }
  void status(char *reply, size_t capacity, bool timing) const override {
    if (runtime_ == PacketProgramRuntime::Lua) {
      const auto &s = lua_.stats();
      if (timing)
        snprintf(reply, capacity, "lua load_us=%llu invoke_us=%llu instructions=%u native_calls=%u parser_steps=%u",
                 static_cast<unsigned long long>(s.loadUs), static_cast<unsigned long long>(s.invokeUs),
                 s.instructions, s.nativeCalls, s.parserSteps);
      else
        snprintf(reply, capacity, "lua source=%u session=%u live=%u peak=%u cap=%u",
                 s.sourceBytes, s.sessionBytes, s.liveBytes, s.peakBytes, s.heapLimit);
    }
#if ONCHIP_BOT_WASM
    else {
      const auto &s = wasm_.stats();
      if (timing)
        snprintf(reply, capacity, "wasm load_us=%llu init_us=%llu invoke_us=%llu instructions=%u native_calls=%u",
                 static_cast<unsigned long long>(s.loadUs), static_cast<unsigned long long>(s.initUs),
                 static_cast<unsigned long long>(s.invokeUs), s.instructions, s.nativeCalls);
      else
        snprintf(reply, capacity, "wasm source=%u session=%u linear=%u stack=%u pool=%u pool_peak=%u",
                 s.sourceBytes, s.sessionBytes, s.linearBytes, s.stackBytes, s.poolBytes, s.poolHighWaterBytes);
    }
#endif
  }
};
#endif
bool number(const char *s, uint32_t &value) {
  if (!s || !*s) return false;
  value = 0;
  for (; *s; ++s) {
    if (*s < '0' || *s > '9' || value > (UINT32_MAX - unsigned(*s - '0')) / 10) return false;
    value = value * 10 + unsigned(*s - '0');
  }
  return true;
}
bool hex(const char *s, uint8_t *output, size_t size) {
  if (!s || strlen(s) != size * 2) return false;
  for (size_t i = 0; i < size; ++i) {
    unsigned value = 0;
    for (unsigned j = 0; j < 2; ++j) {
      const char c = s[2 * i + j];
      const int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
      if (digit < 0) return false;
      value = value * 16 + unsigned(digit);
    }
    output[i] = uint8_t(value);
  }
  return true;
}
void encode(const uint8_t *bytes, size_t size, char *output) {
  constexpr char digits[] = "0123456789abcdef";
  for (size_t i = 0; i < size; ++i) {
    output[2 * i] = digits[bytes[i] >> 4]; output[2 * i + 1] = digits[bytes[i] & 15];
  }
  output[2 * size] = 0;
}
}
#if MESHCORE_ONCHIP_BOT
PacketProgram *makePacketVmProgram() { return new (std::nothrow) VmProgram; }
#endif
const char *PacketPrograms::name(uint8_t slot) { return slot ? "packet1" : "packet0"; }
bool PacketPrograms::valid(const PacketProgramRecord &r) {
  if (r.version != 1 || r.active > PacketProgramEmpty || r.previous > PacketProgramEmpty ||
      r.enabled > 1 || r.reserved || (r.enabled && r.active == PacketProgramEmpty) ||
      !r.stages || r.stages > 255 || !r.fuel || r.fuel > 100000 ||
      !r.microseconds || r.microseconds > 20000 || (r.capabilities & ~packet_engine::AllCapabilities))
    return false;
  for (const auto &s : r.sources)
    if (s.size > PacketProgramSourceLimit || unsigned(s.runtime) > unsigned(PacketProgramRuntime::Wasm) ||
        s.reserved[0] || s.reserved[1] || s.reserved[2]) return false;
  return (r.active == PacketProgramEmpty || r.sources[r.active].size) &&
         (r.previous == PacketProgramEmpty || r.sources[r.previous].size) &&
         (r.active == PacketProgramEmpty || r.active != r.previous);
}
packet_engine::Budget PacketPrograms::budget(uint8_t slot, const PacketProgramRecord &r) const {
  return {name(slot), r.stages, r.fuel, r.microseconds, r.capabilities};
}
PacketPrograms::~PacketPrograms() {
  for (auto &slot : slots_) delete slot.program;
}
void PacketPrograms::fail(uint8_t slot, const char *error, bool seal) {
  auto &s = slots_[slot];
  snprintf(s.outcome, sizeof(s.outcome), "Error: %.120s", error);
  store_.report(slot, s.outcome);
  if (seal) {
    s.sealed = true; s.bootPending = false;
    pipeline_.enable(slot, false);
  }
}
bool PacketPrograms::save(uint8_t slot, const PacketProgramRecord &next) {
  auto record = next;
  if (!store_.journal(slot, record, true)) {
    fail(slot, "packet journal save failed; slot disabled; use retry to read saved selection", true);
    return false;
  }
  slots_[slot].saved = next;
  return true;
}
bool PacketPrograms::begin() {
  if (attached_ || pipeline_.size()) return false;
  for (uint8_t i = 0; i < packet_engine::EngineLimit; ++i) {
    auto &s = slots_[i];
    if (pipeline_.attach(s, budget(i, s.saved)) != packet_engine::Registration::Attached ||
        !pipeline_.enable(i, false)) {
      fail(i, "packet slot registration failed", true); return false;
    }
  }
  attached_ = true;
  for (uint8_t i = 0; i < packet_engine::EngineLimit; ++i) {
    auto &s = slots_[i];
    if (!store_.journal(i, s.saved, false) || !valid(s.saved))
      fail(i, "packet journal invalid; use retry or explicit remove", true);
    else {
      s.bootPending = s.saved.active != PacketProgramEmpty;
      snprintf(s.outcome, sizeof(s.outcome), "%s", s.bootPending ? "saved program waiting for loader" : "empty");
    }
  }
  return true;
}
bool PacketPrograms::start(uint8_t slot, const PacketProgramRecord &next, uint8_t from,
                           bool copy, bool boot) {
  PacketProgramLoader::Request request;
  request.slot = slot; request.from = from; request.to = next.active;
  request.copy = copy; request.source = next.sources[next.active];
  if (pending_ || !loader_.submit(request)) {
    fail(slot, "packet loader busy or unavailable; use retry", boot); return false;
  }
  candidate_ = next; pendingSlot_ = slot; bootJob_ = boot; pending_ = true;
  snprintf(slots_[slot].outcome, sizeof(slots_[slot].outcome),
           "loading %s program; prior selection retained",
           request.source.runtime == PacketProgramRuntime::Lua ? "Lua" : "Wasm");
  return true;
}
void PacketPrograms::service() {
  if (!attached_) return;
  for (uint8_t i = 0; i < packet_engine::EngineLimit; ++i) {
    auto &s = slots_[i];
    if (!s.sealed && !s.bootPending && s.program && s.saved.enabled && !pipeline_.enabled(i)) {
      auto next = s.saved; next.enabled = 0;
      if (pending_ && pendingSlot_ == i) candidate_.enabled = 0;
      if (save(i, next)) fail(i, "packet fault disabled program; inspect stats; enable on to retry");
    }
  }
  if (pending_) {
    PacketProgramLoader::Result result;
    if (!loader_.poll(result)) return;
    pending_ = false;
    auto &s = slots_[pendingSlot_];
    s.bootPending = false;
    if (!result.ok || !result.program) {
      fail(pendingSlot_, result.error[0] ? result.error : "packet loader returned no program", bootJob_);
      loader_.release(result.program); return;
    }
    if (s.sealed || (!bootJob_ && !save(pendingSlot_, candidate_))) {
      loader_.release(result.program); return;
    }
    if (!pipeline_.configure(pendingSlot_, budget(pendingSlot_, candidate_))) {
      fail(pendingSlot_, "packet budget publication failed; slot disabled; use retry", true);
      loader_.release(result.program); return;
    }
    auto *retired = s.program; s.program = result.program;
    s.saved = candidate_; s.upload = {}; s.sealed = false;
    if (!pipeline_.enable(pendingSlot_, s.saved.enabled != 0))
      fail(pendingSlot_, "packet activation failed; slot disabled; use retry", true);
    else snprintf(s.outcome, sizeof(s.outcome), "saved program %s", s.saved.enabled ? "enabled" : "disabled");
    loader_.release(retired);
    return;
  }
  if (loader_.busy()) return;
  for (uint8_t i = 0; i < packet_engine::EngineLimit; ++i)
    if (slots_[i].bootPending) { start(i, slots_[i].saved, slots_[i].saved.active, false, true); return; }
}
void PacketPrograms::command(const char *input, char *reply, size_t capacity) {
  if (!reply || !capacity) return;
  const auto respond = [&](const char *text) { snprintf(reply, capacity, "%s", text); };
  if (!attached_) { respond("Error: packet controller unavailable"); return; }
  if (!input || strlen(input) > 162) { respond("Error: packet command exceeds 162 bytes"); return; }
  if (!*input || !strcmp(input, "help")) {
    respond("packet api|phy; packet SLOT status|hash|stats|memory|timing|enable on|off|budget|rollback|remove|retry; help packet 2 for budgets and upload");
    return;
  }
  char text[163]; strcpy(text, input);
  const char *args[8]{}; unsigned count = 0;
  char *cursor = text;
  while (*cursor) {
    while (*cursor == ' ') ++cursor;
    if (!*cursor) break;
    if (count == 8) { respond("Error: too many packet command arguments"); return; }
    args[count++] = cursor;
    while (*cursor && *cursor != ' ') ++cursor;
    if (*cursor) *cursor++ = 0;
  }
  uint32_t index;
  if (count < 2 || !number(args[0], index) || index >= packet_engine::EngineLimit) {
    respond("Error: usage: packet SLOT OPERATION; SLOT is 0 or 1"); return;
  }
  const uint8_t slot = uint8_t(index);
  auto &s = slots_[slot]; const char *op = args[1];
  if (!strcmp(op, "status") && count == 2) {
    snprintf(reply, capacity, "slot=%u runtime=%s saved=%u live=%u sealed=%u bytes=%u; %.80s",
             slot, s.saved.active == PacketProgramEmpty ? "none" :
             s.saved.sources[s.saved.active].runtime == PacketProgramRuntime::Lua ? "lua" : "wasm",
             s.saved.enabled, pipeline_.enabled(slot), s.sealed,
             s.saved.active == PacketProgramEmpty ? 0 : s.saved.sources[s.saved.active].size, s.outcome);
    return;
  }
  if (!strcmp(op, "hash") && count == 2) {
    if (s.sealed) { respond(s.outcome); return; }
    if (s.saved.active == PacketProgramEmpty) { respond("empty"); return; }
    char hash[65]; encode(s.saved.sources[s.saved.active].hash, 32, hash); respond(hash); return;
  }
  if (!strcmp(op, "stats") && count == 2) {
    snprintf(reply, capacity, "stages=%u fuel=%u us=%u caps=%u calls=%u faults=%u drops=%u",
             s.saved.stages, s.saved.fuel, s.saved.microseconds, s.saved.capabilities,
             pipeline_.calls(slot), pipeline_.faults(slot), pipeline_.drops(slot));
    return;
  }
  if ((!strcmp(op, "memory") || !strcmp(op, "timing")) && count == 2) {
    if (s.program) s.program->status(reply, capacity, !strcmp(op, "timing"));
    else respond("Error: packet program not loaded");
    return;
  }
  if (pending_ || loader_.busy() || s.bootPending) { respond("Error: packet loader busy; inspect status"); return; }
  if (!strcmp(op, "retry") && count == 2) {
    PacketProgramRecord record;
    if (!store_.journal(slot, record, false) || !valid(record)) {
      fail(slot, "packet journal invalid; explicit remove clears selection", true); respond(s.outcome); return;
    }
    pipeline_.enable(slot, false); s.saved = record; s.sealed = false;
    s.bootPending = record.active != PacketProgramEmpty;
    snprintf(s.outcome, sizeof(s.outcome), "%s", s.bootPending ? "saved program waiting for loader" : "empty");
    respond(s.outcome); return;
  }
  if (!strcmp(op, "remove") && count == 2) {
    PacketProgramRecord next = s.sealed ? PacketProgramRecord{} : s.saved;
    next.previous = next.active; next.active = PacketProgramEmpty; next.enabled = 0;
    if (!save(slot, next)) { respond(s.outcome); return; }
    pipeline_.enable(slot, false); s.sealed = false; s.upload = {};
    delete s.program; s.program = nullptr;
    strcpy(s.outcome, "packet selection removed"); respond(s.outcome); return;
  }
  if (s.sealed) { respond(s.outcome); return; }
  if (!strcmp(op, "enable") && count == 3 && (!strcmp(args[2], "on") || !strcmp(args[2], "off"))) {
    const bool enable = !strcmp(args[2], "on");
    if (enable && (!s.program || s.saved.active == PacketProgramEmpty)) {
      respond("Error: install a packet program before enabling the slot"); return;
    }
    auto next = s.saved; next.enabled = enable;
    if (!save(slot, next)) { respond(s.outcome); return; }
    if (!pipeline_.enable(slot, enable)) { fail(slot, "packet enable publication failed", true); respond(s.outcome); return; }
    snprintf(s.outcome, sizeof(s.outcome), "saved program %s", enable ? "enabled" : "disabled");
    respond(s.outcome); return;
  }
  if (!strcmp(op, "budget") && count == 2) {
    snprintf(reply, capacity, "stages=%u fuel=%u us=%u caps=%u",
             s.saved.stages, s.saved.fuel, s.saved.microseconds, s.saved.capabilities); return;
  }
  if (!strcmp(op, "budget") && count == 6) {
    auto next = s.saved;
    if (!number(args[2], next.stages) || !number(args[3], next.fuel) ||
        !number(args[4], next.microseconds) || !number(args[5], next.capabilities) || !valid(next)) {
      respond("Error: budget requires STAGES 1..255 FUEL 1..100000 US 1..20000 CAPS 0..7"); return;
    }
    if (!save(slot, next)) { respond(s.outcome); return; }
    if (!pipeline_.configure(slot, budget(slot, next))) { fail(slot, "packet budget publication failed", true); respond(s.outcome); return; }
    respond("Packet execution budget saved and applied"); return;
  }
  if (!strcmp(op, "begin") && count == 6) {
    PacketProgramSource source; uint8_t id[8];
    if (!hex(args[2], id, sizeof(id)) || (strcmp(args[3], "lua") && strcmp(args[3], "wasm")) ||
        !number(args[4], source.size) || !source.size || source.size > PacketProgramSourceLimit ||
        !hex(args[5], source.hash, sizeof(source.hash))) {
      respond("Error: begin requires ID16 lua|wasm SIZE 1..16384 SHA256"); return;
    }
    source.runtime = !strcmp(args[3], "lua") ? PacketProgramRuntime::Lua : PacketProgramRuntime::Wasm;
    if (s.upload.active) {
      if (strcmp(s.upload.id, args[2]) || memcmp(&source, &s.upload.source, sizeof(source))) {
        respond("Error: another packet upload is staged; cancel its ID first"); return;
      }
    } else {
      if (!store_.truncate(slot, PacketProgramUpload)) { fail(slot, "packet upload file could not be opened"); respond(s.outcome); return; }
      s.upload.active = true; strcpy(s.upload.id, args[2]); s.upload.source = source; s.upload.received = 0;
    }
    snprintf(reply, capacity, "Packet upload ready received=%u size=%u", s.upload.received, source.size); return;
  }
  if (!strcmp(op, "chunk") && count == 5) {
    uint32_t chunk; uint8_t bytes[48], actual[48];
    const size_t size = strlen(args[4]) / 2;
    if (!s.upload.active || strcmp(s.upload.id, args[2]) || !number(args[3], chunk) ||
        chunk > PacketProgramSourceLimit / 48 || !size || size > sizeof(bytes) ||
        !hex(args[4], bytes, size)) { respond("Error: packet chunk ID, index or hex bytes invalid"); return; }
    const uint32_t offset = chunk * 48;
    const uint32_t expected = offset < s.upload.source.size ?
        (s.upload.source.size - offset < 48 ? s.upload.source.size - offset : 48) : 0;
    if (size != expected || offset > s.upload.received ||
        (offset < s.upload.received && (size > s.upload.received - offset ||
         !store_.read(slot, PacketProgramUpload, offset, actual, size) || memcmp(bytes, actual, size)))) {
      respond("Error: packet chunk is out of sequence or differs from saved bytes"); return;
    }
    if (offset == s.upload.received) {
      if (!store_.append(slot, PacketProgramUpload, bytes, size)) {
        s.upload.active = false;
        fail(slot, "packet chunk write failed; begin a new upload"); respond(s.outcome); return;
      }
      s.upload.received += uint32_t(size);
    }
    snprintf(reply, capacity, "Packet chunk saved received=%u", s.upload.received); return;
  }
  if (!strcmp(op, "cancel") && count == 3) {
    if (!s.upload.active || strcmp(args[2], s.upload.id)) { respond("Error: packet upload ID not staged"); return; }
    s.upload = {}; respond("Packet upload cancelled"); return;
  }
  if (!strcmp(op, "commit") && count == 3) {
    if (!s.upload.active || strcmp(args[2], s.upload.id) || s.upload.received != s.upload.source.size) {
      respond("Error: packet upload ID is incomplete or not staged"); return;
    }
    auto next = s.saved;
    for (uint8_t file = 0; file < 3; ++file)
      if (file != next.active && file != next.previous) { next.active = file; break; }
    next.previous = s.saved.active; next.sources[next.active] = s.upload.source;
    if (!start(slot, next, PacketProgramUpload, true, false)) { respond(s.outcome); return; }
    respond("Packet validation accepted; inspect status then hash"); return;
  }
  if (!strcmp(op, "rollback") && count == 2) {
    if (s.saved.previous == PacketProgramEmpty) { respond("Error: packet slot has no previous source"); return; }
    auto next = s.saved; next.active = s.saved.previous; next.previous = s.saved.active;
    if (!start(slot, next, next.active, false, false)) { respond(s.outcome); return; }
    respond("Packet rollback accepted; inspect status then hash"); return;
  }
  if (!strcmp(op, "read") && count == 3) {
    uint32_t chunk;
    if (s.saved.active == PacketProgramEmpty || !number(args[2], chunk) || chunk > PacketProgramSourceLimit / 48) {
      respond("Error: packet read requires an installed program and INDEX"); return;
    }
    const auto size = s.saved.sources[s.saved.active].size;
    const uint32_t offset = chunk * 48;
    if (offset >= size) { respond("Error: packet read index exceeds source"); return; }
    const size_t length = size - offset < 48 ? size - offset : 48;
    uint8_t bytes[48]; char encoded[97];
    if (!store_.read(slot, s.saved.active, offset, bytes, length)) {
      fail(slot, "packet source read failed"); respond(s.outcome); return;
    }
    encode(bytes, length, encoded); snprintf(reply, capacity, "%u %s", chunk, encoded); return;
  }
  respond("Error: invalid packet operation or arguments; use packet help");
}
} // namespace onchip
