// SPDX-License-Identifier: Apache-2.0
#include "CloudRoomLifecycle.h"
#include "CloudRoomLink.h"
#if ONCHIP_CLOUD_ROOM_LINK
#include "CloudRoomPreferences.h"
#include "CloudRoomService.h"
#include "CloudRoomSettings.h"
#include "RoleIdentity.h"
#include "RoleStorage.h"
#include "cloudroom/CloudRoomGrammar.h"
#include "cloudroom/CloudRoomWire.h"
#include <Arduino.h>
#include <atomic>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

namespace onchip {
namespace {
using cloudnode::Net;
constexpr size_t ReplyLimit = 145;
constexpr unsigned PageSize = 5;
constexpr char DefaultHost[] = ONCHIP_CLOUD_ROOM_DEFAULT_HOST;
constexpr unsigned SlotLimit = ONCHIP_CLOUD_ROOM_CONNECTIONS < 2 ? ONCHIP_CLOUD_ROOM_CONNECTIONS : 2;

void wipe(void *data, size_t size) {
  auto *bytes = static_cast<volatile uint8_t *>(data);
  while (size--) *bytes++ = 0;
}
template <class T> struct Wiped {
  T value;
  Wiped() : value{} {}
  ~Wiped() { wipe(&value, sizeof(value)); }
};
struct Hold {
  CloudRoomSettingsV2 *value = allocateRoleStorage<CloudRoomSettingsV2>("cloud-room link settings");
  ~Hold() {
    if (value) wipe(value, sizeof(*value));
    releaseRoleStorage(value);
  }
};

enum class Kind : uint8_t { None, Probe, Join, Create, Convert, Invite, Nodes, Revoke, Password, Unlink };
enum class Phase : uint8_t { Idle, Queued, Running, Finishing, Done };
enum class Fail : uint8_t {
  None, CodeExpired, CodeUsed, CodeInvalid, Rate, BadSig, Unavailable, Cap, Closed, Exists, Banned,
  BadRequest, Collision, Clock, Tls, Memory, Name, Key, OwnerOnly, LastOwner, SelfRevoke, TooNew,
  Revoked, Gone, Disabled, Identity, SplitBrain, Dns
};
struct Job {
  Kind kind = Kind::None;
  char code[17]{}, host[128]{}, name[32]{}, arg[17]{}, alias[65]{}, pw[16]{};
  uint16_t page = 0;
  bool owner = false, remove = false, force = false, open = false, generate = false, show = false;
  bool systemRoots = true, converted = false;
  const char *ca = "", *token = "";
  uint8_t role = 0;
};
struct Outcome {
  char alias[31]{}, name[32]{}, token[69]{}, pw[16]{};
  uint8_t publicKey[32]{}, role = 0;
  bool deleted = false;
};

struct Link {
  CloudRoomLinkPorts ports;
  std::atomic<uint8_t> phase{uint8_t(Phase::Idle)};
  Job job;
  Outcome outcome;
  char text[160]{};
  uint32_t textAt = 0;
  bool secret = false, ok = false, any = false;
  unsigned failures = 0;
  std::atomic<uint32_t> advertised{0};
  // A saved change restarts the node: the dispatch task asks, the network task schedules (the
  // scheduler waits for the dispatch task to flush state, so it must not run there).
  std::atomic<bool> restartRequested{false}, restartFailed{false};
  char flushText[112]{};
  Phase get() const { return Phase(phase.load(std::memory_order_acquire)); }
  void set(Phase next) { phase.store(uint8_t(next), std::memory_order_release); }
} link;

void say(char *reply, size_t capacity, const char *format, ...) {
  va_list args;
  va_start(args, format);
  vsnprintf(reply, capacity < ReplyLimit + 1 ? capacity : ReplyLimit + 1, format, args);
  va_end(args);
}
size_t limit(size_t capacity) { return capacity < ReplyLimit + 1 ? capacity : ReplyLimit + 1; }
// Formats with the host, or without it when the text would not fit a DM; a host is never cut.
void sayHost(char *reply, size_t capacity, const char *host, const char *with, const char *without) {
  if (snprintf(reply, limit(capacity), with, host) >= int(limit(capacity))) snprintf(reply, limit(capacity), "%s", without);
}
uint32_t clockMs() { return link.ports.millis ? link.ports.millis() : uint32_t(::millis()); }
void clearText() {
  wipe(link.text, sizeof(link.text));
  link.secret = false;
}
void setText(bool ok, bool secret, const char *format, ...) {
  va_list args;
  va_start(args, format);
  vsnprintf(link.text, sizeof(link.text), format, args);
  va_end(args);
  link.ok = ok; link.secret = secret; link.textAt = clockMs(); link.any = true;
}
const char *hostOr(const char *host) { return *host ? host : DefaultHost; }
bool sameHost(const char *a, const char *b) { return !strcasecmp(a, b); }
void shortId(const char *id, char (&out)[12]) {
  if (strlen(id) > 8) snprintf(out, sizeof(out), "%.8s..", id);
  else snprintf(out, sizeof(out), "%s", id);
}
const char *roleWord(uint8_t role) { return role == CloudRoleOwner ? "owner" : "frontend"; }

// ---- failure texts -----------------------------------------------------------------------
void formatFail(Fail fail, const char *host, uint32_t retry, char (&t)[160]) {
  const bool hosted = sameHost(host, DefaultHost);
  char extra[64]{};
  if (hosted) snprintf(extra, sizeof(extra), " Request one at %s", ONCHIP_CLOUD_ROOM_REQUEST_POINTER);
  t[0] = 0;
  const auto with = [&](const char *format) {
    char base[320], full[400];
    snprintf(base, sizeof(base), format, host);
    snprintf(full, sizeof(full), "%s%s", base, extra);
    const char *chosen = strlen(full) > ReplyLimit ? base : full;
    strncpy(t, chosen, sizeof(t) - 1);
    t[sizeof(t) - 1] = 0;
  };
  switch (fail) {
  case Fail::CodeExpired: snprintf(t, sizeof(t), "Link failed: code expired. Ask the owner for a new one (cloudroom invite)."); break;
  case Fail::CodeUsed: snprintf(t, sizeof(t), "Link failed: code already used. Ask the owner for a new one (cloudroom invite)."); break;
  case Fail::CodeInvalid: with("Link failed: code not valid for create or join on %s. Check the code and the @host."); break;
  case Fail::Rate: snprintf(t, sizeof(t), "Link failed: rate limited, retry in %lu s", static_cast<unsigned long>(retry ? retry : 60)); break;
  case Fail::BadSig: snprintf(t, sizeof(t), "Link failed: node signature rejected"); break;
  case Fail::Unavailable: case Fail::Dns:
    snprintf(t, sizeof(t), "Link failed: server unavailable. Run cloudroom probe %s.", host); break;
  case Fail::Cap: snprintf(t, sizeof(t), "Server limit reached (rooms, nodes or codes). Ask the server operator."); break;
  case Fail::Closed: with("Server is not accepting new rooms. Ask the operator."); break;
  case Fail::Exists: snprintf(t, sizeof(t), "That room is already hosted."); break;
  case Fail::Banned: snprintf(t, sizeof(t), "Server refused this node. Ask the server operator."); break;
  case Fail::BadRequest: snprintf(t, sizeof(t), "Server and node versions differ. Update the Worker (cloudroom probe %s).", host); break;
  case Fail::Collision: snprintf(t, sizeof(t), "This node already serves a room with the same ID byte. Ask the owner for a new room or unlink the other."); break;
  case Fail::Clock: snprintf(t, sizeof(t), "Clock not set. Wait for SNTP (get sntp.server), then retry."); break;
  case Fail::Tls: snprintf(t, sizeof(t), "TLS verify failed for %s: certificate not trusted. Check the host name.", host); break;
  case Fail::Memory: snprintf(t, sizeof(t), "Low memory: run \"cloudroom share off\", restart, then retry."); break;
  case Fail::Name: snprintf(t, sizeof(t), "Server rejected the room name. Choose another name."); break;
  case Fail::Key: snprintf(t, sizeof(t), "Server rejected the room key. Send \"reboot\", then retry."); break;
  case Fail::OwnerOnly: snprintf(t, sizeof(t), "Only an owner node can do that. Ask the owner for \"cloudroom invite owner\"."); break;
  case Fail::LastOwner: snprintf(t, sizeof(t), "Other nodes still use this room. Run \"cloudroom invite owner\" for a node that stays, or \"cloudroom unlink ALIAS delete\"."); break;
  case Fail::SelfRevoke: snprintf(t, sizeof(t), "Use \"cloudroom unlink\" for this node."); break;
  case Fail::TooNew: snprintf(t, sizeof(t), "Owner added less than 24 h ago; try later."); break;
  case Fail::Revoked: snprintf(t, sizeof(t), "Server revoked this node. Run \"cloudroom unlink ALIAS local\"."); break;
  case Fail::Gone: snprintf(t, sizeof(t), "Room is gone from %s. Run \"cloudroom unlink ALIAS local\".", host); break;
  case Fail::Disabled: snprintf(t, sizeof(t), "Server disabled this room. Ask the server operator."); break;
  case Fail::Identity: snprintf(t, sizeof(t), "Management identity unavailable"); break;
  case Fail::SplitBrain:
    snprintf(t, sizeof(t), "Room stays live on %s for other nodes. Run \"cloudroom unlink ALIAS delete\" first, or add \"force\".", host); break;
  case Fail::None: break;
  }
  // A long host that does not fit a DM is dropped rather than cut.
  if (strlen(t) > ReplyLimit) {
    switch (fail) {
    case Fail::Unavailable: case Fail::Dns: snprintf(t, sizeof(t), "Link failed: server unavailable. Run cloudroom probe."); break;
    case Fail::BadRequest: snprintf(t, sizeof(t), "Server and node versions differ. Update the Worker."); break;
    case Fail::Tls: snprintf(t, sizeof(t), "TLS verify failed: certificate not trusted. Check the host name."); break;
    case Fail::Gone: snprintf(t, sizeof(t), "Room is gone from the server. Run \"cloudroom unlink ALIAS local\"."); break;
    case Fail::SplitBrain: snprintf(t, sizeof(t), "Room stays live for other nodes. Run \"cloudroom unlink ALIAS delete\" first, or add \"force\"."); break;
    default: break;
    }
  }
}
void failText(Fail fail, const char *host, uint32_t retry) {
  char t[160];
  formatFail(fail, host, retry, t);
  setText(false, false, "%s", t);
}

// ---- response parsing --------------------------------------------------------------------
using cloudroom::Document;
using cloudroom::View;
bool asText(View v, char *out, size_t capacity) {
  size_t n = 0;
  return cloudroom::decodeString(v, out, capacity, n);
}
bool printable(const char *s, size_t min, size_t max) {
  const size_t n = strlen(s);
  if (n < min || n > max) return false;
  for (; *s; ++s) if (uint8_t(*s) < 32 || uint8_t(*s) > 126) return false;
  return true;
}
bool hexBytes(View v, uint8_t *out, size_t bytes) {
  if (!v.hex(bytes)) return false;
  for (size_t i = 0; i < bytes; ++i) {
    const auto digit = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    out[i] = uint8_t(digit(v.data[i * 2]) * 16 + digit(v.data[i * 2 + 1]));
  }
  return true;
}
bool hostedId(View v) {
  if (v.type != JSONString || v.size != 30) return false;
  for (size_t i = 0; i < v.size; ++i) {
    const char c = v.data[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '2' && c <= '7'))) return false;
  }
  return true;
}
// An alias id minted by a hosted server; anything else is a room the node got from a provider
// or a profile upload, which the shared-room server's control route does not know.
bool hostedAliasId(const char *id) {
  if (strlen(id) != 30) return false;
  for (size_t i = 0; i < 30; ++i) {
    const char c = id[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '2' && c <= '7'))) return false;
  }
  return true;
}
bool tokenText(View v) {
  if (v.type != JSONString || v.size != 68 || memcmp(v.data, "ar1_", 4)) return false;
  for (size_t i = 4; i < v.size; ++i) {
    const char c = v.data[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}
Fail mapError(const char *body, size_t size, uint16_t status, bool control) {
  Document document;
  if (document.parse(body, size)) {
    const auto code = Document::field(document.root(), "e");
    const struct { const char *name; Fail fail; } table[] = {
      {"code_invalid", Fail::CodeInvalid}, {"code_expired", Fail::CodeExpired}, {"code_used", Fail::CodeUsed},
      {"bad_request", Fail::BadRequest}, {"name", Fail::Name}, {"key", Fail::Key}, {"bad_sig", Fail::BadSig},
      {"cap", Fail::Cap}, {"clock", Fail::Clock}, {"collision", Fail::Collision}, {"exists", Fail::Exists},
      {"banned", Fail::Banned}, {"rate", Fail::Rate}, {"closed", Fail::Closed}, {"auth", Fail::Revoked},
      {"forbidden", Fail::OwnerOnly}, {"disabled", Fail::Disabled}, {"too_new", Fail::TooNew},
      {"not_found", Fail::Gone}, {"gone", Fail::Gone}, {"last_owner", Fail::LastOwner}, {"self", Fail::SelfRevoke},
      {"internal", Fail::Unavailable}};
    for (const auto &entry : table) if (code.equals(entry.name)) return entry.fail;
    return Fail::BadRequest;
  }
  if (control && (status == 401 || status == 403)) return Fail::Revoked;
  return status == 429 ? Fail::Rate : Fail::Unavailable;
}
bool okBody(const cloudnode::Reply &reply, View &root) {
  Document document;
  if (reply.status != 200 || !document.parse(reply.body, reply.size)) return false;
  uint64_t ok = 0;
  if (!Document::field(document.root(), "ok").unsignedNumber(ok, 1) || ok != 1) return false;
  root = document.root();
  return true;
}

// ---- one network job ---------------------------------------------------------------------
struct Work {
  char exchange[cloudnode::ExchangeBuffer];
  char body[1280];
  char path[96];
};
Fail netFail(Net net, bool probe = false) {
  switch (net) {
  case Net::Clock: return Fail::Clock;
  case Net::Verify: return Fail::Tls;
  case Net::Memory: return Fail::Memory;
  case Net::Dns: return Fail::Dns;
  default: return probe ? Fail::Unavailable : Fail::Unavailable;
  }
}
void hexText(const uint8_t *bytes, size_t n, char *out) {
  cloudnode::Writer w(out, n * 2 + 1);
  w.hex(bytes, n);
}
// Hex bytes that other IDs on this node already use, so the server avoids them.
void avoidList(const Job &job, const uint8_t *skip, char (&out)[17]) {
  uint8_t first[8];
  unsigned count = 0;
  const auto add = [&](uint8_t byte) {
    for (unsigned i = 0; i < count; ++i) if (first[i] == byte) return;
    if (count < 8) first[count++] = byte;
  };
  static const char *const roles[] = {"repeater", "companion", "observer", "management", "command-bot", "room"};
  for (const char *role : roles) {
    uint8_t byte;
    if (job.kind == Kind::Convert && !strcmp(role, "room")) continue;
    if (identityFirstByte(role, byte)) add(byte);
  }
  if (const auto *running = cloudRoomConfiguration())
    for (unsigned i = 0; i < running->count; ++i)
      if (!skip || memcmp(running->aliases[i].publicKey, skip, 32)) add(running->aliases[i].publicKey[0]);
  cloudnode::Writer w(out, sizeof(out));
  w.hex(first, count);
}
void runRedeem(Job &job, CloudTransport &transport, Work &work, const char *address, uint32_t now) {
  const char *op = job.kind == Kind::Join ? "join" : job.kind == Kind::Create ? "create" : "convert";
  uint8_t secret[PRV_KEY_SIZE]{}, publicKey[32]{};
  char name[32]{}, nodeHex[65]{}, sigHex[129]{}, subject[65]{}, avoid[17]{};
  const struct Scrub { uint8_t *secret; ~Scrub() { wipe(secret, PRV_KEY_SIZE); } } scrub{secret};
  if (job.kind == Kind::Convert) {
    // The name and password were read from the Room role when the command was accepted.
    snprintf(name, sizeof(name), "%s", job.name);
    if (!identityPublicKey("room", publicKey) || exportRoleKey("room", secret) != ExportResult::Ok) {
      setText(false, false, "Room role has no key. Use \"cloudroom create\".");
      return;
    }
    hexText(publicKey, 32, subject);
  } else if (job.kind == Kind::Create) {
    snprintf(name, sizeof(name), "%s", job.name);
    cloudnode::sha256Hex(name, strlen(name), 8, subject);
  } else {
    strcpy(subject, "-");
  }
  avoidList(job, job.kind == Kind::Convert ? publicKey : nullptr, avoid);
  if (!cloudnode::signRedeem(job.host, op, job.code, subject, now, nodeHex, sigHex)) {
    failText(Fail::Identity, job.host, 0);
    return;
  }
  cloudnode::Writer w(work.body, sizeof(work.body));
  w.text("{\"p\":\""); w.text(cloudnode::Protocol); w.text("\",\"op\":\""); w.text(op);
  w.text("\",\"code\":\""); w.text(job.code); w.text("\",\"node\":\""); w.text(nodeHex);
  w.text("\",\"ts\":"); w.number(now); w.text(",\"sig\":\""); w.text(sigHex); w.text("\"");
  if (job.kind != Kind::Join) { w.text(",\"name\":"); w.quoted(name); }
  if (job.kind == Kind::Convert) {
    w.text(",\"pub\":\""); w.text(subject); w.text("\",\"key\":\""); w.hex(secret, PRV_KEY_SIZE);
    w.text("\",\"pw\":"); w.quoted(job.pw);
  }
  wipe(secret, sizeof(secret));
  w.text(",\"avoid\":\""); w.text(avoid); w.text("\"}");
  if (!w.good() || w.size() > cloudnode::RedeemLimit) { failText(Fail::BadRequest, job.host, 0); wipe(work.body, sizeof(work.body)); return; }
  const CloudTlsTarget target{address, job.host, job.ca, job.systemRoots};
  cloudnode::Reply reply;
  const Net net = cloudnode::exchange(transport, target, "POST", "/v1/node/redeem", nullptr, work.body, w.size(),
                                      work.exchange, sizeof(work.exchange), reply, CloudLinkTimeoutMs);
  wipe(work.body, sizeof(work.body));
  if (net != Net::Ok) { failText(netFail(net), job.host, 0); return; }
  View root;
  if (!okBody(reply, root)) {
    const Fail fail = mapError(reply.body, reply.size, reply.status, false);
    failText(fail, job.host, reply.retryAfter);
    if (fail == Fail::CodeExpired || fail == Fail::CodeUsed || fail == Fail::CodeInvalid || fail == Fail::Rate ||
        fail == Fail::BadSig) ++link.failures;
    return;
  }
  // The response must echo what this node asked for; anything else is saved nowhere.
  Outcome &o = link.outcome;
  o = {};
  char aliasId[31]{}, echoedName[32]{}, tokenValue[69]{};
  uint8_t echoedKey[32];
  const auto id = Document::field(root, "alias"), nameField = Document::field(root, "name"),
             pub = Document::field(root, "pub"), token = Document::field(root, "token"), role = Document::field(root, "role");
  const bool shape = hostedId(id) && asText(nameField, echoedName, sizeof(echoedName)) &&
      printable(echoedName, 1, 31) && hexBytes(pub, echoedKey, 32) && echoedKey[0] != 0 && echoedKey[0] != 255 &&
      tokenText(token) && (role.equals("frontend") || role.equals("owner"));
  const bool echoes = shape &&
      (job.kind == Kind::Join || (!strcmp(echoedName, name) &&
        (job.kind != Kind::Convert || !memcmp(echoedKey, publicKey, 32)))) &&
      (job.kind == Kind::Join || role.equals("owner"));
  if (!echoes) { failText(Fail::BadRequest, job.host, 0); return; }
  memcpy(aliasId, id.data, 30);
  memcpy(tokenValue, token.data, 68);
  memcpy(o.alias, aliasId, sizeof(o.alias)); memcpy(o.name, echoedName, sizeof(o.name));
  memcpy(o.token, tokenValue, sizeof(o.token)); memcpy(o.publicKey, echoedKey, 32);
  o.role = role.equals("owner") ? CloudRoleOwner : CloudRoleFrontend;
  wipe(tokenValue, sizeof(tokenValue));
  link.ok = true;
}
const char *ttlText(uint32_t ttl, bool owner, char (&out)[24]) {
  if (owner || ttl % 60) snprintf(out, sizeof(out), "%lu s", static_cast<unsigned long>(ttl));
  else if (ttl % 3600) snprintf(out, sizeof(out), "%lu min", static_cast<unsigned long>(ttl / 60));
  else snprintf(out, sizeof(out), "%lu h", static_cast<unsigned long>(ttl / 3600));
  return out;
}
const char *seenText(uint32_t seconds, char (&out)[8]) {
  if (seconds < 3600) snprintf(out, sizeof(out), "%lum", static_cast<unsigned long>(seconds / 60));
  else if (seconds < 172800) snprintf(out, sizeof(out), "%luh", static_cast<unsigned long>(seconds / 3600));
  else snprintf(out, sizeof(out), "%lud", static_cast<unsigned long>(seconds / 86400));
  return out;
}
// A control call to the alias in `job`. Returns true with `root` set on success; otherwise the
// failure text is set. `body` is the request JSON.
bool control(Job &job, CloudTransport &transport, Work &work, const char *address, const char *body,
             cloudnode::Reply &reply, View &root) {
  snprintf(work.path, sizeof(work.path), "/v1/aliases/%s/control", job.alias);
  const CloudTlsTarget target{address, job.host, job.ca, job.systemRoots};
  const Net net = cloudnode::exchange(transport, target, "POST", work.path, job.token, body, strlen(body),
                                      work.exchange, sizeof(work.exchange), reply, CloudLinkTimeoutMs);
  if (net != Net::Ok) { failText(netFail(net), job.host, 0); return false; }
  if (!okBody(reply, root)) { failText(mapError(reply.body, reply.size, reply.status, true), job.host, reply.retryAfter); return false; }
  return true;
}
void runControl(Job &job, CloudTransport &transport, Work &work, const char *address) {
  Outcome &o = link.outcome;
  o = {};
  cloudnode::Reply reply;
  View root;
  char body[200];
  cloudnode::Writer w(body, sizeof(body));
  switch (job.kind) {
  case Kind::Invite: {
    w.text("{\"op\":\"invite\",\"role\":\""); w.text(job.owner ? "owner" : "frontend"); w.text("\"}");
    if (!control(job, transport, work, address, body, reply, root)) return;
    char code[24]{};
    const auto codeField = Document::field(root, "code");
    uint64_t ttl = 0;
    size_t n = 0;
    if (!cloudroom::decodeString(codeField, code, sizeof(code), n) || n != 19 || !Document::field(root, "ttl").unsignedNumber(ttl, 2592000)) {
      failText(Fail::BadRequest, job.host, 0); return;
    }
    char canonical[17];
    if (!cloudroom::canonicalCode(code, n, canonical)) { failText(Fail::BadRequest, job.host, 0); return; }
    char when[24];
    ttlText(uint32_t(ttl), job.owner, when);
    char at[136]{};
    if (!sameHost(job.host, DefaultHost)) snprintf(at, sizeof(at), "@%s", job.host);
    char text[320];
    if (snprintf(text, sizeof(text), "%s %s (%s, once). Run: cloudroom join %s%s", job.owner ? "Owner code" : "Code",
                 code, when, code, at) > int(ReplyLimit))
      snprintf(text, sizeof(text), "%s %s (%s, once). The server host is too long for a DM; send it with the code.",
               job.owner ? "Owner code" : "Code", code, when);
    setText(true, true, "%s", text);
    wipe(text, sizeof(text)); wipe(code, sizeof(code));
    return;
  }
  case Kind::Nodes: {
    w.text("{\"op\":\"nodes\",\"offset\":"); w.number(uint32_t(job.page) * PageSize); w.text("}");
    if (!control(job, transport, work, address, body, reply, root)) return;
    const auto nodes = Document::field(root, "nodes");
    uint64_t total = 0;
    if (nodes.type != JSONArray || !Document::field(root, "total").unsignedNumber(total, 100000)) { failText(Fail::BadRequest, job.host, 0); return; }
    char text[256];
    unsigned first = job.page * PageSize + 1, shown = 0;
    size_t used = 0;
    // coreJSON search by array index: "nodes[0]" ... "nodes[4]".
    for (unsigned i = 0; i < PageSize; ++i) {
      char path[16];
      snprintf(path, sizeof(path), "[%u]", i);
      View entry;
      if (JSON_SearchConst(nodes.data, nodes.size, path, strlen(path), &entry.data, &entry.size, &entry.type) != JSONSuccess) break;
      char id[9]{};
      uint64_t seen = 0;
      const auto idField = Document::field(entry, "id"), roleField = Document::field(entry, "r");
      if (!idField.hex(4) || roleField.type != JSONString || !Document::field(entry, "seen").unsignedNumber(seen, 4000000000ull)) {
        failText(Fail::BadRequest, job.host, 0); return;
      }
      memcpy(id, idField.data, 8);
      char age[8];
      const int n = snprintf(text + used, sizeof(text) - used, "%s%s %s %s", shown ? "; " : "", id,
                             roleField.equals("o") ? "owner" : "node", seenText(uint32_t(seen), age));
      if (n < 0 || used + size_t(n) >= sizeof(text)) break;
      used += size_t(n); ++shown;
    }
    char all[320];
    if (!shown) snprintf(all, sizeof(all), "No nodes on this page of %lu.", static_cast<unsigned long>(total));
    else snprintf(all, sizeof(all), "Nodes %u-%u of %lu: %s", first, first + shown - 1, static_cast<unsigned long>(total), text);
    setText(true, false, "%s", all);
    return;
  }
  case Kind::Revoke:
    w.text("{\"op\":\"revoke\",\"id\":\""); w.text(job.arg); w.text("\"}");
    if (!control(job, transport, work, address, body, reply, root)) return;
    setText(true, false, "Revoked %s.", job.arg);
    return;
  case Kind::Password: {
    w.text("{\"op\":\"password\",\"mode\":\"");
    w.text(job.open ? "open" : job.generate ? "new" : job.show ? "get" : "set"); w.text("\"");
    if (!job.open && !job.generate && !job.show) { w.text(",\"pw\":"); w.quoted(job.arg); }
    w.text("}");
    const bool ok = control(job, transport, work, address, body, reply, root);
    wipe(body, sizeof(body));
    if (!ok) return;
    char pw[16]{};
    const auto field = Document::field(root, "pw");
    size_t n = 0;
    if (!cloudroom::decodeString(field, pw, sizeof(pw), n) || n > 15) { failText(Fail::BadRequest, job.host, 0); return; }
    if (n) setText(true, true, "Room password %s (open: cloudroom password open; change: cloudroom password PW|new)", pw);
    else setText(true, false, "Room is open (no password). Set one: cloudroom password PW|new");
    wipe(pw, sizeof(pw));
    return;
  }
  case Kind::Unlink: {
    if (job.remove) {
      w.text("{\"op\":\"delete\",\"confirm\":\""); w.text(job.alias); w.text("\"}");
      if (!control(job, transport, work, address, body, reply, root)) return;
      uint64_t pending = 0;
      if (Document::field(root, "pending").unsignedNumber(pending, 2592000)) {
        if (job.converted) setText(true, false, "Delete scheduled in 24 h. Unlink this node now with \"cloudroom unlink %s force\".", job.alias);
        else setText(true, false, "Delete scheduled in 24 h. Unlink this node now with \"cloudroom unlink\".");
        job.kind = Kind::None;   // nothing to change locally
        return;
      }
      o.deleted = true;
      link.ok = true;
      return;
    }
    if (job.converted && !job.force) {
      if (!control(job, transport, work, address, "{\"op\":\"nodes\",\"offset\":0}", reply, root)) return;
      uint64_t total = 0;
      if (!Document::field(root, "total").unsignedNumber(total, 100000)) { failText(Fail::BadRequest, job.host, 0); return; }
      if (total > 1) { failText(Fail::SplitBrain, job.host, 0); return; }
    }
    if (!control(job, transport, work, address, "{\"op\":\"leave\"}", reply, root)) return;
    uint64_t deleted = 0;
    Document::field(root, "deleted").unsignedNumber(deleted, 1);
    o.deleted = deleted == 1;
    size_t n = 0;
    if (o.deleted) cloudroom::decodeString(Document::field(root, "pw"), o.pw, sizeof(o.pw), n);
    link.ok = true;
    return;
  }
  default: return;
  }
}
void runProbe(Job &job, CloudTransport &transport, Work &work, const char *address, uint32_t now, bool clockKnown) {
  const CloudTlsTarget target{address, job.host, job.ca, job.systemRoots};
  cloudnode::Reply reply;
  const Net net = cloudnode::exchange(transport, target, "GET", "/v1/info", nullptr, nullptr, 0, work.exchange,
                                      sizeof(work.exchange), reply, CloudLinkTimeoutMs);
  const char *step = nullptr;
  switch (net) {
  case Net::Clock: step = "TLS clock"; break;
  case Net::Verify: step = "TLS verify"; break;
  case Net::Memory: step = "low memory"; break;
  case Net::Ok: break;
  default: step = "no response"; break;
  }
  char detail[48]{};
  View root;
  Document document;
  if (!step) {
    uint64_t server = 0;
    char service[24]{}, version[16]{}, signup[8]{}, protocol[16]{};
    size_t n;
    const bool parsed = reply.status == 200 && document.parse(reply.body, reply.size) &&
        cloudroom::decodeString(Document::field(document.root(), "service"), service, sizeof(service), n) &&
        !strcmp(service, "aspen-shared-room") &&
        cloudroom::decodeString(Document::field(document.root(), "protocol"), protocol, sizeof(protocol), n) &&
        cloudroom::decodeString(Document::field(document.root(), "version"), version, sizeof(version), n) &&
        printable(version, 1, 15) &&
        cloudroom::decodeString(Document::field(document.root(), "signup"), signup, sizeof(signup), n) &&
        Document::field(document.root(), "time").unsignedNumber(server, 4000000000ull);
    if (!parsed) snprintf(detail, sizeof(detail), "HTTP %u not an Aspen server", unsigned(reply.status));
    else if (strcmp(protocol, cloudnode::Protocol)) snprintf(detail, sizeof(detail), "protocol %.15s differs", protocol);
    else {
      char skew[16] = "";
      if (clockKnown) snprintf(skew, sizeof(skew), " clock%+lds", long(int64_t(server) - int64_t(now)));
      setText(true, false, "Server %s ok: aspen-shared-room %s signup=%s%s", job.host, version, signup, skew);
      if (strlen(link.text) > ReplyLimit)
        setText(true, false, "Server ok: aspen-shared-room %s signup=%s%s", version, signup, skew);
      return;
    }
    step = detail;
  }
  setText(false, false, "Server %s failed: %s", job.host, step);
  if (strlen(link.text) > ReplyLimit) setText(false, false, "Server failed: %s", step);
}
void execute() {
  Job &job = link.job;
  link.outcome = {};
  if (!link.ports.freeInternal || !link.ports.largestBlock ||
      link.ports.freeInternal() < CloudLinkMinFreeInternal || link.ports.largestBlock() < CloudLinkMinLargestBlock) {
    failText(Fail::Memory, job.host, 0); return;
  }
  uint32_t now = 0;
  const bool clockKnown = link.ports.now && link.ports.now(now);
  if (!clockKnown && job.kind != Kind::Probe && (job.kind == Kind::Join || job.kind == Kind::Create || job.kind == Kind::Convert)) {
    failText(Fail::Clock, job.host, 0); return;
  }
  auto *work = allocateRoleStorage<Work>("cloud-room link buffers");
  if (!work) { failText(Fail::Memory, job.host, 0); return; }
  struct Release { Work *w; ~Release() { wipe(w, sizeof(*w)); releaseRoleStorage(w); } } release{work};
  CloudTransport *transport = link.ports.transport ? link.ports.transport() : nullptr;
  if (!transport) { failText(Fail::Memory, job.host, 0); return; }
  struct Drop { CloudTransport *t; ~Drop() { t->close(); delete t; } } drop{transport};
  char address[16]{};
  if (!link.ports.resolve || !link.ports.resolve(job.host, address)) {
    if (job.kind == Kind::Probe) setText(false, false, "Server %s failed: DNS", job.host);
    else failText(Fail::Dns, job.host, 0);
    return;
  }
  link.ok = false;
  switch (job.kind) {
  case Kind::Probe: runProbe(job, *transport, *work, address, now, clockKnown); break;
  case Kind::Join: case Kind::Create: case Kind::Convert:
    runRedeem(job, *transport, *work, address, now); break;
  default: runControl(job, *transport, *work, address); break;
  }
}

// ---- saved record edits (dispatch task) -------------------------------------------------
bool signable() {
  uint8_t signature[64], publicKey[32];
  static const uint8_t probe[1] = {0};
  const bool ok = signManagement(probe, sizeof(probe), signature, publicKey);
  wipe(signature, sizeof(signature));
  return ok;
}
bool nodeId(char (&out)[9]) {
  uint8_t publicKey[32];
  if (!identityPublicKeyIfPresent("management", publicKey)) return false;
  cloudnode::Writer w(out, sizeof(out));
  w.hex(publicKey, 4);
  return true;
}
// Running alias (index in the boot configuration) with this id, or -1.
int runningIndex(const char *id) {
  const auto *running = cloudRoomConfiguration();
  if (running)
    for (unsigned i = 0; i < running->count; ++i)
      if (running->peers[i].alias && !strcmp(running->peers[i].alias, id)) return int(i);
  return -1;
}
// Alias in `settings` named by an id or a 1-based index; empty names the only alias.
int recordIndex(const CloudRoomSettingsV2 &settings, const char *name, bool &ambiguous) {
  ambiguous = false;
  if (!*name) {
    if (settings.count == 1) return 0;
    ambiguous = settings.count > 1;
    return -1;
  }
  for (unsigned i = 0; i < settings.count; ++i) if (!strcmp(settings.aliases[i].base.id, name)) return int(i);
  if (name[0] >= '1' && name[0] <= '9' && !name[1]) {
    const unsigned index = unsigned(name[0] - '0');
    if (index <= settings.count) return int(index - 1);
  }
  return -1;
}
void listLine(const CloudRoomSettingsV2 &settings, unsigned index, char *out, size_t capacity) {
  const auto &alias = settings.aliases[index];
  char id[12];
  shortId(alias.base.id, id);
  const char *state = "off";
  if (settings.enabled) {
    const int running = runningIndex(alias.base.id);
    if (running >= 0 && link.ports.aliasState) {
      switch (link.ports.aliasState(unsigned(running))) {
      case CloudAliasState::Live: state = "live"; break;
      case CloudAliasState::Down: state = "down"; break;
      case CloudAliasState::Revoked: state = "revoked"; break;
      default: state = "off"; break;
      }
    }
  }
  const unsigned total = settings.count;
  if (snprintf(out, capacity, "%u/%u %s \"%s\" %s %s %s", index + 1, total, id, alias.base.name, roleWord(alias.role), state,
               alias.base.host) >= int(capacity))
    snprintf(out, capacity, "%u/%u %s \"%s\" %s %s", index + 1, total, id, alias.base.name, roleWord(alias.role), state);
}
bool lifecycleBlocked(char *reply, size_t capacity) {
  const Phase phase = link.get();
  if (phase == Phase::Queued || phase == Phase::Running || phase == Phase::Finishing) {
    say(reply, capacity, "Error: link busy; send \"cloudroom result\"");
    return true;
  }
  if (cloudRoomUploadStaged()) {
    say(reply, capacity, "Error: cloud room upload in progress; commit or abort it first");
    return true;
  }
  return false;
}
bool settingsHold(Hold &hold, char *reply, size_t capacity) {
  if (!hold.value) { say(reply, capacity, "Error: cloud room settings allocation failed"); return false; }
  if (cloudRoomLoadSettings(*hold.value) != CloudRoomLoad::Ready) {
    say(reply, capacity, "Error: saved cloud room settings invalid; restore the private node backup");
    return false;
  }
  return true;
}
// Asks the network task to flush state and schedule the restart. False when it cannot run;
// `flushText` is what `cloudroom result` says if the flush or the restart then fails.
bool requestRestart(const char *flushText) {
  snprintf(link.flushText, sizeof(link.flushText), "%s", flushText);
  link.restartFailed.store(false, std::memory_order_relaxed);
  if (link.ports.wake && !link.ports.wake()) return false;
  link.restartRequested.store(true, std::memory_order_release);
  return true;
}
// Saves, then restarts. The save is final before the restart is requested.
enum class Saved { Failed, Restarting, FlushFailed };
Saved saveAndRestart(CloudRoomSettingsV2 &settings, char *why, size_t capacity, const char *flushText) {
  char reason[100];
  if (!cloudRoomPublishSettings(settings, reason, sizeof(reason))) {
    snprintf(why, capacity, "%s", reason);
    return Saved::Failed;
  }
  return requestRestart(flushText) ? Saved::Restarting : Saved::FlushFailed;
}
constexpr char SavedFlushText[] = "Saved, but the state flush failed. Run \"reboot\" when ready.";
void slotsFullText(const CloudRoomSettingsV2 &settings, char *reply, size_t capacity) {
  char a[12], b[12] = "";
  shortId(settings.aliases[0].base.id, a);
  if (settings.count > 1) shortId(settings.aliases[1].base.id, b);
  if (settings.count > 1) say(reply, capacity, "All %u room slots are in use (%s, %s). Free one: cloudroom unlink ALIAS", settings.count, a, b);
  else say(reply, capacity, "All %u room slot is in use (%s). Free it: cloudroom unlink ALIAS", settings.count, a);
}
// Trust and certificate for a host: a saved alias on that host with a pinned CA keeps using it
// (a private-CA server); every other host verifies against the built-in public roots.
void chooseTrust(Job &job) {
  job.systemRoots = true;
  job.ca = "";
  if (const auto *running = cloudRoomConfiguration())
    for (unsigned i = 0; i < running->count; ++i)
      if (running->peers[i].host && sameHost(running->peers[i].host, job.host) && !running->peers[i].systemRoots) {
        job.systemRoots = false;
        job.ca = running->peers[i].ca;
        return;
      }
}
bool queueJob(Job &job, char *reply, size_t capacity, bool redeem, bool trust = true) {
  if (lifecycleBlocked(reply, capacity)) return true;
  if (redeem && link.failures >= CloudLinkFailureLimit) {
    say(reply, capacity, "Too many failed attempts. Send \"reboot\", then retry.");
    return true;
  }
  if (!signable()) { say(reply, capacity, "Management identity unavailable"); return true; }
  if (trust) chooseTrust(job);
  clearText();
  link.any = false;
  link.job = job;
  link.set(Phase::Queued);
  if (link.ports.wake && !link.ports.wake()) {
    link.set(Phase::Idle);
    wipe(&link.job, sizeof(link.job));
    say(reply, capacity, "Error: the shared-room service cannot run a link job; send \"cloudroom status\"");
    return true;
  }
  sayHost(reply, capacity, job.host,
          "Contacting %s (up to 20 s). Send \"cloudroom result\" for the outcome; the node restarts when linked.",
          "Contacting the server (up to 20 s). Send \"cloudroom result\" for the outcome; the node restarts when linked.");
  return true;
}
// The first owner alias in the running configuration (owner verbs have no alias argument).
bool ownerAlias(Job &job, char *reply, size_t capacity) {
  const auto *running = cloudRoomConfiguration();
  if (!running || !running->count) {
    Hold hold;
    if (hold.value && cloudRoomLoadSettings(*hold.value) == CloudRoomLoad::Ready && hold.value->count)
      say(reply, capacity, "Sharing is off or not running yet. Send \"cloudroom share on\", then restart.");
    else
      say(reply, capacity, "Nothing linked. Use cloudroom join or create.");
    return false;
  }
  for (unsigned i = 0; i < running->count; ++i) {
    CloudRoomAliasInfo info;
    if (cloudRoomAliasInfo(i, info) && info.role == CloudRoleOwner) {
      snprintf(job.alias, sizeof(job.alias), "%s", running->peers[i].alias);
      snprintf(job.host, sizeof(job.host), "%s", running->peers[i].host);
      job.systemRoots = running->peers[i].systemRoots;
      job.ca = running->peers[i].ca;
      job.token = running->peers[i].token;
      job.role = info.role;
      return true;
    }
  }
  say(reply, capacity, "Only an owner node can do that. Ask the owner for \"cloudroom invite owner\".");
  return false;
}
bool lifecycle(const cloudroom::Command &cmd, char *reply, size_t capacity) {
  using cloudroom::Verb;
  Job job;
  switch (cmd.verb) {
  case Verb::List: {
    Hold hold;
    if (!settingsHold(hold, reply, capacity)) return true;
    const unsigned index = cmd.arg[0] ? unsigned(atoi(cmd.arg)) : 1;
    if (!hold.value->count) { say(reply, capacity, "Nothing linked. Use cloudroom join or create."); return true; }
    if (index < 1 || index > hold.value->count) { say(reply, capacity, "Error: no alias %u; cloudroom list 1..%u", index, hold.value->count); return true; }
    listLine(*hold.value, index - 1, reply, limit(capacity));
    return true;
  }
  case Verb::Result: {
    const Phase phase = link.get();
    if (phase == Phase::Queued || phase == Phase::Running || phase == Phase::Finishing) { say(reply, capacity, "Link working."); return true; }
    if (link.any && link.secret && uint32_t(clockMs() - link.textAt) > CloudLinkSecretMs) { clearText(); link.any = false; }
    if (link.restartFailed.load(std::memory_order_acquire)) { say(reply, capacity, "%s", link.flushText); return true; }
    if (link.any) { say(reply, capacity, "%s", link.text); return true; }
    Hold hold;
    char line[ReplyLimit + 1]{};
    if (hold.value && cloudRoomLoadSettings(*hold.value) == CloudRoomLoad::Ready && hold.value->count) {
      listLine(*hold.value, 0, line, sizeof(line) - 28);
      say(reply, capacity, "No link job since restart. %s", line);
    } else {
      say(reply, capacity, "No link job since restart.");
    }
    return true;
  }
  case Verb::Apply:
    if (!requestRestart(SavedFlushText)) say(reply, capacity, "Error: the restart could not be scheduled; send \"reboot\"");
    else say(reply, capacity, "Restarting in 10 s.");
    return true;
  case Verb::Probe:
    snprintf(job.host, sizeof(job.host), "%s", hostOr(cmd.host));
    job.kind = Kind::Probe;
    return queueJob(job, reply, capacity, false);
  case Verb::Join: case Verb::Create: case Verb::Convert: {
    Hold hold;
    if (!settingsHold(hold, reply, capacity)) return true;
    job.kind = cmd.verb == Verb::Join ? Kind::Join : cmd.verb == Verb::Create ? Kind::Create : Kind::Convert;
    snprintf(job.host, sizeof(job.host), "%s", hostOr(cmd.host));
    snprintf(job.code, sizeof(job.code), "%s", cmd.code);
    if (hold.value->count >= SlotLimit && job.kind != Kind::Convert) { slotsFullText(*hold.value, reply, capacity); return true; }
    if (job.kind == Kind::Create) snprintf(job.name, sizeof(job.name), "%s", cmd.name);
    if (job.kind == Kind::Convert) {
      char name[32]{}, pw[16]{};
      const struct Scrub { char *pw; ~Scrub() { wipe(pw, 16); } } scrub{pw};
      uint8_t key[PRV_KEY_SIZE]{}, publicKey[32]{};
      const struct ScrubKey { uint8_t *key; ~ScrubKey() { wipe(key, PRV_KEY_SIZE); } } scrubKey{key};
      if (!link.ports.roomName || !link.ports.roomGuestPassword || !link.ports.roomName(name) ||
          !link.ports.roomGuestPassword(pw)) {
        say(reply, capacity, "The Room role is off or has no key. Use \"cloudroom create\" for a new room."); return true;
      }
      switch (exportRoleKey("room", key)) {
      case ExportResult::Ok: break;
      case ExportResult::Staged: say(reply, capacity, "Room key change pending; restart, then retry."); return true;
      case ExportResult::Reset: say(reply, capacity, "Room key was reset; restart, then retry."); return true;
      default: say(reply, capacity, "Room role has no key. Use \"cloudroom create\"."); return true;
      }
      wipe(key, sizeof(key));
      bool replaces = false;
      if (identityPublicKey("room", publicKey))
        for (unsigned i = 0; i < hold.value->count; ++i)
          if (!memcmp(hold.value->aliases[i].base.publicKey, publicKey, 32)) replaces = true;
      if (hold.value->count >= SlotLimit && !replaces) { slotsFullText(*hold.value, reply, capacity); return true; }
      snprintf(job.name, sizeof(job.name), "%s", name);
      snprintf(job.pw, sizeof(job.pw), "%s", pw);
      const bool defaultHost = !cmd.host[0] || sameHost(cmd.host, DefaultHost);
      const bool confirmed = cmd.yes && (defaultHost ? !cmd.yesHost[0] || sameHost(cmd.yesHost, job.host) : sameHost(cmd.yesHost, job.host));
      if (!confirmed) {
        if (!cmd.host[0]) say(reply, capacity, "Key goes to %s; it can speak as the room. Admin logins stay. Add \"yes\".", job.host);
        else say(reply, capacity, "Key goes to %s; it can speak as the room. Admin logins stay. Add \"yes %s\".", job.host, job.host);
        return true;
      }
    }
    return queueJob(job, reply, capacity, true);
  }
  case Verb::Invite:
    if (!ownerAlias(job, reply, capacity)) return true;
    job.kind = Kind::Invite; job.owner = cmd.owner;
    return queueJob(job, reply, capacity, false, false);
  case Verb::Nodes:
    if (!ownerAlias(job, reply, capacity)) return true;
    job.kind = Kind::Nodes;
    if (cmd.arg[0] && atoi(cmd.arg) < 1) { say(reply, capacity, "Usage: cloudroom nodes [N]"); return true; }
    job.page = uint16_t(cmd.arg[0] ? atoi(cmd.arg) - 1 : 0);
    return queueJob(job, reply, capacity, false, false);
  case Verb::Revoke:
    if (!ownerAlias(job, reply, capacity)) return true;
    job.kind = Kind::Revoke;
    snprintf(job.arg, sizeof(job.arg), "%s", cmd.arg);
    return queueJob(job, reply, capacity, false, false);
  case Verb::Password:
    if (!ownerAlias(job, reply, capacity)) return true;
    job.kind = Kind::Password; job.open = cmd.open; job.generate = cmd.generate;
    job.show = !cmd.open && !cmd.generate && !cmd.arg[0];
    snprintf(job.arg, sizeof(job.arg), "%s", cmd.arg);
    return queueJob(job, reply, capacity, false, false);
  case Verb::Share: {
    Hold hold;
    if (!settingsHold(hold, reply, capacity)) return true;
    auto &s = *hold.value;
    if (!s.count) { say(reply, capacity, "Nothing linked. Use cloudroom join or create."); return true; }
    if (lifecycleBlocked(reply, capacity)) return true;
    if (cmd.on == (s.enabled != 0)) { say(reply, capacity, cmd.on ? "Sharing is already on." : "Sharing is already off."); return true; }
    bool converted = false;
    const char *host = "";
    for (unsigned i = 0; i < s.count; ++i)
      if (s.aliases[i].flags & CloudFlagConverted) { converted = true; host = s.aliases[i].base.host; }
    if (!cmd.on && converted && !cmd.force) {
      char text[160];
      formatFail(Fail::SplitBrain, host, 0, text);
      say(reply, capacity, "%s", text);
      return true;
    }
    s.enabled = cmd.on;
    char why[100];
    const Saved saved = saveAndRestart(s, why, sizeof(why), SavedFlushText);
    if (saved == Saved::Failed) { say(reply, capacity, "%s", why); return true; }
    if (saved == Saved::FlushFailed) say(reply, capacity, "%s", SavedFlushText);
    else if (cmd.on) say(reply, capacity, "Sharing on. Restarting in 10 s.");
    else if (converted) say(reply, capacity, "Local Room resumes; the server copy stays live. Restarting in 10 s; send \"cloudroom share on\" to resume.");
    else say(reply, capacity, "Sharing off. Restarting in 10 s; send \"cloudroom share on\" to resume.");
    return true;
  }
  case Verb::Unlink: {
    Hold hold;
    if (!settingsHold(hold, reply, capacity)) return true;
    auto &s = *hold.value;
    if (!s.count) { say(reply, capacity, "Nothing linked. Use cloudroom join or create."); return true; }
    bool ambiguous;
    const int index = recordIndex(s, cmd.alias, ambiguous);
    if (index < 0) {
      if (ambiguous) say(reply, capacity, "Two rooms are linked. Name one: cloudroom unlink ALIAS (see cloudroom list)");
      else say(reply, capacity, "Error: no such alias; see cloudroom list");
      return true;
    }
    const auto alias = s.aliases[index];
    const bool converted = alias.flags & CloudFlagConverted;
    if (lifecycleBlocked(reply, capacity)) return true;
    const bool hosted = hostedAliasId(alias.base.id);
    if (!hosted && cmd.remove) {
      say(reply, capacity, "This room is not hosted on a shared-room server. Run \"cloudroom unlink ALIAS\" without delete.");
      return true;
    }
    if (cmd.local || !hosted) {
      if (converted && !cmd.force) {
        char text[160];
        formatFail(Fail::SplitBrain, alias.base.host, 0, text);
        say(reply, capacity, "%s", text);
        return true;
      }
      for (unsigned i = unsigned(index); i + 1 < s.count; ++i) s.aliases[i] = s.aliases[i + 1];
      memset(&s.aliases[s.count - 1], 0, sizeof(s.aliases[0]));
      --s.count;
      char why[100], id[9] = "-";
      const Saved saved = saveAndRestart(s, why, sizeof(why), SavedFlushText);
      if (saved == Saved::Failed) { say(reply, capacity, "%s", why); return true; }
      nodeId(id);
      if (!hosted) say(reply, capacity, "Alias removed locally.");
      else say(reply, capacity, "%sAlias removed locally. The server still lists this node; an owner can revoke %s.",
               converted ? "Local Room resumes; the server copy stays live. " : "", id);
      return true;
    }
    if (cmd.remove && alias.role != CloudRoleOwner) { say(reply, capacity, "Only an owner node can do that. Ask the owner for \"cloudroom invite owner\"."); return true; }
    const int running = runningIndex(alias.base.id);
    const auto *config = cloudRoomConfiguration();
    if (running < 0 || !config) {
      say(reply, capacity, "That room is not running yet. Restart, or use \"cloudroom unlink ALIAS local\".");
      return true;
    }
    job.kind = Kind::Unlink; job.remove = cmd.remove; job.force = cmd.force; job.converted = converted;
    snprintf(job.alias, sizeof(job.alias), "%s", alias.base.id);
    snprintf(job.host, sizeof(job.host), "%s", config->peers[running].host);
    job.systemRoots = config->peers[running].systemRoots;
    job.ca = config->peers[running].ca;
    job.token = config->peers[running].token;
    job.role = alias.role;
    return queueJob(job, reply, capacity, false, false);
  }
  default: return false;
  }
}

// Applies a finished network job to the saved record.
void apply() {
  Job &job = link.job;
  Outcome &o = link.outcome;
  struct Scrub { ~Scrub() { wipe(&link.outcome, sizeof(link.outcome)); } } scrub;
  if (job.kind == Kind::None) return;
  Hold hold;
  char why[100];
  if (!hold.value || cloudRoomLoadSettings(*hold.value) != CloudRoomLoad::Ready) {
    setText(false, false, "Linked on the server, but the saved settings are unavailable. Send the same command again.");
    return;
  }
  auto &s = *hold.value;
  bool converted = false;
  unsigned slotNumber = 0;
  if (job.kind == Kind::Unlink) {
    int index = -1;
    for (unsigned i = 0; i < s.count; ++i) if (!strcmp(s.aliases[i].base.id, job.alias)) index = int(i);
    if (index < 0) { setText(true, false, "Unlinked."); return; }
    converted = s.aliases[index].flags & CloudFlagConverted;
    for (unsigned i = unsigned(index); i + 1 < s.count; ++i) s.aliases[i] = s.aliases[i + 1];
    memset(&s.aliases[s.count - 1], 0, sizeof(s.aliases[0]));
    --s.count;
  } else {
    int slot = -1;
    for (unsigned i = 0; i < s.count; ++i) if (!memcmp(s.aliases[i].base.publicKey, o.publicKey, 32)) slot = int(i);
    if (slot < 0) {
      if (s.count >= SlotLimit) { slotsFullText(s, link.text, sizeof(link.text)); link.ok = false; return; }
      slot = int(s.count++);
    }
    auto &alias = s.aliases[slot];
    memset(&alias, 0, sizeof(alias));
    if (strlen(job.host) >= sizeof(alias.base.host) || (!job.systemRoots && strlen(job.ca) >= sizeof(alias.base.ca))) {
      setText(false, false, "Link failed: server host or certificate does not fit the saved settings.");
      return;
    }
    snprintf(alias.base.host, sizeof(alias.base.host), "%s", job.host);
    if (!job.systemRoots) snprintf(alias.base.ca, sizeof(alias.base.ca), "%s", job.ca);
    snprintf(alias.base.token, sizeof(alias.base.token), "%s", o.token);
    snprintf(alias.base.id, sizeof(alias.base.id), "%s", o.alias);
    snprintf(alias.base.name, sizeof(alias.base.name), "%s", o.name);
    memcpy(alias.base.publicKey, o.publicKey, 32);
    alias.role = o.role;
    alias.caMode = job.systemRoots ? CloudCaSystem : CloudCaPinned;
    alias.flags = uint8_t((job.kind == Kind::Convert ? CloudFlagConverted : 0) | (job.kind != Kind::Join ? CloudFlagAdvertOnce : 0));
    s.enabled = 1;
    slotNumber = unsigned(slot) + 1;
  }
  const char *flushed = job.kind == Kind::Unlink ? "Unlinked and saved, but the state flush failed. Run \"reboot\" when ready."
                                                  : "Linked and saved, but the state flush failed. Run \"reboot\" when ready.";
  const Saved saved = saveAndRestart(s, why, sizeof(why), flushed);
  if (saved == Saved::Failed) {
    setText(false, false, job.kind == Kind::Unlink ? "Unlinked on the server, but saving failed. Run \"cloudroom unlink ALIAS local\"."
                                                   : "Linked on the server, but saving failed. Send the same command again.");
    return;
  }
  if (saved == Saved::FlushFailed) {
    setText(true, false, "%s", flushed);
    return;
  }
  if (job.kind == Kind::Unlink) {
    bool written = false;
    if (o.deleted && o.pw[0] && converted && link.ports.roomSetGuestPassword) written = link.ports.roomSetGuestPassword(o.pw);
    setText(true, false, converted && !written ? "Unlinked. Restarting in 10 s. Local Room password is the one it had before converting."
                                               : "Unlinked. Restarting in 10 s.");
  } else if (o.role == CloudRoleOwner) {
    setText(true, false, "Linked. Restarts in 10 s. Add the room in your companion app (cloudroom advertise %u re-announces it), then send \"cloudroom password\".", slotNumber);
  } else {
    setText(true, false, "Linked. Restarts in 10 s. Ask the room owner for the password. If the room is not in your app, run \"cloudroom advertise %u\".", slotNumber);
  }
}
} // namespace

void cloudRoomLinkSetPorts(const CloudRoomLinkPorts &ports) { link.ports = ports; }
bool cloudRoomLinkCommand(const char *line, char *reply, size_t capacity) {
  using namespace cloudroom;
  Wiped<Command> command;
  const Parse parsed = parse(line, command.value);
  if (parsed == Parse::NotCloudroom) return false;
  if (parsed == Parse::Ok && command.value.verb == Verb::NotNew) return false;
  if (parsed != Parse::Ok) { say(reply, capacity, "%s", parseMessage(parsed, command.value.verb)); return true; }
  return lifecycle(command.value, reply, capacity);
}
bool cloudRoomLinkPending() {
  const Phase phase = link.get();
  return phase == Phase::Queued || phase == Phase::Running;
}
bool cloudRoomLinkWork() {
  return cloudRoomLinkPending() || link.restartRequested.load(std::memory_order_acquire);
}
void cloudRoomLinkRun() {
  if (link.restartRequested.exchange(false, std::memory_order_acq_rel) &&
      !cloudRoomScheduleRestart(CloudLinkRestartMs))
    link.restartFailed.store(true, std::memory_order_release);
  uint8_t expected = uint8_t(Phase::Queued);
  if (!link.phase.compare_exchange_strong(expected, uint8_t(Phase::Running), std::memory_order_acq_rel)) return;
  execute();
  const bool changes = link.ok && link.job.kind != Kind::None &&
      (link.job.kind == Kind::Join || link.job.kind == Kind::Create || link.job.kind == Kind::Convert ||
       link.job.kind == Kind::Unlink);
  if (changes) {
    link.set(Phase::Finishing);
  } else {
    if (link.ok && !link.any) setText(true, false, "Done.");
    wipe(&link.outcome, sizeof(link.outcome));
    wipe(&link.job, sizeof(link.job));
    link.set(Phase::Done);
  }
}
void cloudRoomLinkLoop() {
  if (link.get() == Phase::Finishing) {
    apply();
    wipe(&link.outcome, sizeof(link.outcome));
    const Job done = link.job;
    wipe(&link.job, sizeof(link.job));
    (void)done;
    link.set(Phase::Done);
  }
  if (link.secret && link.any && uint32_t(clockMs() - link.textAt) > CloudLinkSecretMs) { clearText(); link.any = false; }
  const uint32_t mask = link.advertised.exchange(0);
  if (mask) {
    Hold hold;
    if (hold.value && cloudRoomLoadSettings(*hold.value) == CloudRoomLoad::Ready) {
      bool changed = false;
      const auto *running = cloudRoomConfiguration();
      for (unsigned bit = 0; bit < 2 && running; ++bit)
        if ((mask & (1u << bit)) && bit < running->count)
          for (unsigned i = 0; i < hold.value->count; ++i)
            if (!strcmp(hold.value->aliases[i].base.id, running->peers[bit].alias) &&
                (hold.value->aliases[i].flags & CloudFlagAdvertOnce)) {
              hold.value->aliases[i].flags &= uint8_t(~CloudFlagAdvertOnce);
              changed = true;
            }
      char why[100];
      if (changed && !cloudRoomPublishSettings(*hold.value, why, sizeof(why))) Serial.println("Cloud room advert flag not cleared");
    }
  }
}
void cloudRoomLinkAdvertised(unsigned alias) {
  if (alias < 2) link.advertised.fetch_or(1u << alias, std::memory_order_relaxed);
}
void cloudRoomLinkStatus(char *out, size_t capacity) {
  const Phase phase = link.get();
  const char *state = phase == Phase::Idle && !link.any ? "idle"
      : phase == Phase::Queued || phase == Phase::Running || phase == Phase::Finishing ? "working"
      : link.ok ? "ok" : "failed";
  const auto mode = cloudRoomRoomMode();
  const char *room = mode == CloudRoomRoomMode::Cloud ? "cloud" : mode == CloudRoomRoomMode::Local ? "local" : "off";
  char node[9] = "none";
  nodeId(node);
  snprintf(out, capacity, " link=%s room=%s node=%s", state, room, node);
}
unsigned cloudRoomLinkFailures() { return link.failures; }
void cloudRoomLinkResetForTest() {
  wipe(&link.job, sizeof(link.job)); wipe(&link.outcome, sizeof(link.outcome));
  clearText();
  link.set(Phase::Idle); link.any = false; link.ok = false; link.failures = 0; link.advertised = 0;
  link.restartRequested = false; link.restartFailed = false; wipe(link.flushText, sizeof(link.flushText));
  cloudRoomReportRoom(CloudRoomRoomMode::Off);
}
} // namespace onchip
#endif // ONCHIP_CLOUD_ROOM_LINK
