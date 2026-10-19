// SPDX-License-Identifier: Apache-2.0
// Shared-room lifecycle verbs against a scripted service: link, create, convert, invite,
// unlink and the failure paths. Uses the real record, identity and grammar code.
#include "CloudRoomLifecycle.h"
#include "CloudRoomLink.h"
#include "CloudRoomPreferences.h"
#include "CloudRoomService.h"
#include "CloudRoomSettings.h"
#include "RoleIdentity.h"
#include <SPIFFS.h>
#include <Utils.h>
#include <esp_heap_caps.h>
#include <mbedtls/x509_crt.h>
#include <nvs.h>
#include <cassert>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

unsigned long millis() { return 0; }
void delay(unsigned long) {}
using namespace onchip;
namespace onchip { void cloudRoomBootResetForTest(); }
namespace {
// ---- fake platform ----------------------------------------------------------------------
std::map<std::string, std::vector<uint8_t>> journals;
bool failJournalWrite;
std::vector<uint32_t> restarts;
bool restartWorks = true;
// True while a command or the dispatch-task loop runs. The restart scheduler waits for that
// task to flush state, so calling it from there would stall the node.
bool dispatchContext;
struct Dispatch {
  Dispatch() { dispatchContext = true; }
  ~Dispatch() { dispatchContext = false; }
};
uint32_t utc = 1790000000;
bool clockTrusted = true;
size_t freeHeap = 200 * 1024, largestBlock = 100 * 1024;
std::map<unsigned, CloudAliasState> aliasStates;
bool roomRunning = true;
char guestPassword[16] = "FIXTUREPW";
char roomLabel[32] = "Backyard Room";
std::string writtenBack;
bool writeBackWorks = true;
std::string hex(const uint8_t *bytes, size_t size) {
  const char alphabet[] = "0123456789abcdef";
  std::string text;
  for (size_t i = 0; i < size; ++i) { text += alphabet[bytes[i] >> 4]; text += alphabet[bytes[i] & 15]; }
  return text;
}
struct FixedRng : mesh::RNG {
  uint8_t value;
  explicit FixedRng(uint8_t value) : value(value) {}
  void random(uint8_t *data, size_t size) override { memset(data, value, size); }
};
std::vector<uint8_t> privateKey(uint8_t seed) {
  FixedRng rng(seed);
  mesh::LocalIdentity identity(&rng);
  assert(identity.pub_key[0] != 0 && identity.pub_key[0] != 255);
  std::vector<uint8_t> key(PRV_KEY_SIZE);
  identity.writeTo(key.data(), key.size());
  return key;
}
uint8_t privateKeyFirst(uint8_t seed) {
  FixedRng rng(seed);
  mesh::LocalIdentity identity(&rng);
  return identity.pub_key[0];
}
std::string publicHex(uint8_t seed) {
  FixedRng rng(seed);
  mesh::LocalIdentity identity(&rng);
  return hex(identity.pub_key, 32);
}
void putIdentity(const char *role, uint8_t seed) {
  identity_test::durable[{"mc-onchip", role}] = privateKey(seed);
}
void stageIdentity(const char *role, uint8_t seed, uint8_t flags) {
  // Record layout: "MCI\1", flags, active key, pending key.
  const auto active = identity_test::durable.at({"mc-onchip", role}), pending = privateKey(seed);
  std::vector<uint8_t> record = {'M', 'C', 'I', 1, flags};
  record.insert(record.end(), active.begin(), active.end());
  record.insert(record.end(), pending.begin(), pending.end());
  identity_test::durable[{"mc-onchip", role}] = record;
}
std::string log;
}
namespace onchip {
unsigned cloudRoomAliases() { return 0; }
bool mastRecord(const char *key, void *data, size_t size, bool write, bool &present) {
  assert(!strcmp(key, "cloud-room") && size == 72);
  if (write) {
    if (failJournalWrite) return false;
    auto *bytes = static_cast<const uint8_t *>(data);
    journals[key] = {bytes, bytes + size};
  }
  present = journals.count(key);
  if (!write && present) memcpy(data, journals.at(key).data(), size);
  return true;
}
bool cloudRoomScheduleRestart(uint32_t delay) {
  assert(!dispatchContext && "cloudRoomScheduleRestart waits for the dispatch task");
  if (!restartWorks) return false;
  restarts.push_back(delay);
  return true;
}
}
namespace {
// ---- scripted service -------------------------------------------------------------------
struct Seen { std::string method, path, bearer, body; };
struct Response {
  int status;
  std::string body;
  bool chunked;
  std::string headers;
  Response(int status = 200, std::string body = "", bool chunked = false, std::string headers = "")
      : status(status), body(std::move(body)), chunked(chunked), headers(std::move(headers)) {}
};
enum class Wire { Normal, OpenClock, OpenVerify, OpenMemory, OpenOther, Stall, Lost };
std::vector<Seen> seen;
std::function<Response(const Seen &)> server;
Wire wire = Wire::Normal;
bool dnsWorks = true, transportAvailable = true, wakeWorks = true;
unsigned wakes, opens, closes, transportsLive;
std::vector<std::string> resolved;
std::string lastCa, lastHost;
bool lastSystemRoots;
class FakeTransport final : public CloudTransport {
  std::string sent_, response_;
  size_t offset_ = 0;
  bool built_ = false;
  uint32_t clock_ = 0;
public:
  FakeTransport() { ++transportsLive; }
  ~FakeTransport() override { --transportsLive; }
  bool open(const CloudTlsTarget &target, CloudTlsFailure &failure) override {
    ++opens;
    sent_.clear(); response_.clear(); offset_ = 0; built_ = false;
    lastHost = target.host; lastCa = target.ca ? target.ca : ""; lastSystemRoots = target.systemRoots;
    assert(!strcmp(target.address, "192.0.2.9"));
    switch (wire) {
    case Wire::OpenClock: failure = CloudTlsFailure::Clock; return false;
    case Wire::OpenVerify: failure = CloudTlsFailure::Verify; return false;
    case Wire::OpenMemory: failure = CloudTlsFailure::Memory; return false;
    case Wire::OpenOther: failure = CloudTlsFailure::Network; return false;
    default: return true;
    }
  }
  int write(const uint8_t *data, size_t size) override {
    size = std::min<size_t>(size, 13);
    sent_.append(reinterpret_cast<const char *>(data), size);
    return int(size);
  }
  bool complete() const {
    const auto end = sent_.find("\r\n\r\n");
    if (end == std::string::npos) return false;
    const auto length = sent_.find("Content-Length: ");
    const size_t need = length == std::string::npos ? 0 : size_t(atoi(sent_.c_str() + length + 16));
    return sent_.size() >= end + 4 + need;
  }
  int read(uint8_t *data, size_t size) override {
    if (wire == Wire::Stall || !complete()) return 0;
    if (wire == Wire::Lost) return -1;
    if (!built_) {
      built_ = true;
      Seen request;
      const auto eol = sent_.find(' ');
      request.method = sent_.substr(0, eol);
      request.path = sent_.substr(eol + 1, sent_.find(' ', eol + 1) - eol - 1);
      const auto auth = sent_.find("Authorization: Bearer ");
      if (auth != std::string::npos) request.bearer = sent_.substr(auth + 22, sent_.find("\r\n", auth) - auth - 22);
      request.body = sent_.substr(sent_.find("\r\n\r\n") + 4);
      assert(sent_.find("Host: ") != std::string::npos && sent_.find("Connection: close") != std::string::npos);
      seen.push_back(request);
      const Response r = server ? server(request) : Response{404, "{}"};
      response_ = "HTTP/1.1 " + std::to_string(r.status) + " X\r\nContent-Type: application/json\r\n" + r.headers;
      if (r.chunked) {
        response_ += "Transfer-Encoding: chunked\r\n\r\n";
        for (size_t at = 0; at < r.body.size(); at += 7) {
          const auto part = r.body.substr(at, 7);
          char head[32];
          snprintf(head, sizeof(head), "%zx\r\n", part.size());
          response_ += head + part + "\r\n";
        }
        response_ += "0\r\n\r\n";
      } else {
        response_ += "Content-Length: " + std::to_string(r.body.size()) + "\r\n\r\n" + r.body;
      }
    }
    if (offset_ == response_.size()) return -1;
    size = std::min({size, size_t(11), response_.size() - offset_});
    memcpy(data, response_.data() + offset_, size);
    offset_ += size;
    return int(size);
  }
  void close() override { ++closes; }
  uint32_t now() const override { return clock_; }
  void idle() override { const_cast<FakeTransport *>(this)->clock_ += 500; }
};
CloudTransport *newTransport() { return transportAvailable ? new FakeTransport : nullptr; }
bool resolve(const char *host, char address[16]) {
  resolved.push_back(host);
  if (!dnsWorks) return false;
  strcpy(address, "192.0.2.9");
  return true;
}
bool trustedNow(uint32_t &value) { if (!clockTrusted) return false; value = utc; return true; }
size_t heapFree() { return freeHeap; }
size_t heapBlock() { return largestBlock; }
uint32_t extraMs;
uint32_t fakeMillis() { static uint32_t now = 1000; return (now += 10) + extraMs; }
CloudAliasState state(unsigned alias) { return aliasStates.count(alias) ? aliasStates[alias] : CloudAliasState::Live; }
bool wake() { ++wakes; return wakeWorks; }
bool roomName(char name[32]) { if (!roomRunning) return false; strcpy(name, roomLabel); return true; }
bool roomPassword(char out[16]) { if (!roomRunning) return false; strcpy(out, guestPassword); return true; }
bool roomSetPassword(const char *password) {
  if (!roomRunning || !writeBackWorks) return false;
  writtenBack = password;
  return true;
}

// ---- fixtures ---------------------------------------------------------------------------
constexpr char Code[] = "WXYZ-2345-6789-ABCD", Canonical[] = "WXYZ23456789ABCD", Golden[] = "ABCDEFGHJKMNPQRS";
constexpr char Alias[] = "r2q7x3bndkamw4qzfxe7abcdmn2xyz";
constexpr char OtherAlias[] = "rbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
constexpr char Host[] = "rooms.example.workers.dev";
const std::string Token = "ar1_" + std::string(64, 'a');
const std::string OtherToken = "ar1_" + std::string(64, 'b');
std::vector<std::string> secrets;

void begin() {
  cloudRoomBootResetForTest(); cloudRoomLinkResetForTest();
  assert(psram_test::allocations.empty());
  journals.clear(); filesystem_test::files.clear(); identity_test::durable.clear();
  restarts.clear(); restartWorks = true; seen.clear(); server = nullptr; wire = Wire::Normal;
  dnsWorks = transportAvailable = wakeWorks = true; clockTrusted = true; roomRunning = true;
  freeHeap = 200 * 1024; largestBlock = 100 * 1024; aliasStates.clear(); resolved.clear();
  writtenBack.clear(); writeBackWorks = true; failJournalWrite = false; extraMs = 0;
  wakes = opens = closes = 0;
  strcpy(guestPassword, "FIXTUREPW"); strcpy(roomLabel, "Backyard Room");
  putIdentity("management", 1); putIdentity("room", 2); putIdentity("repeater", 3);
  putIdentity("companion", 4); putIdentity("observer", 5);
  CloudRoomLinkPorts ports;
  ports.transport = newTransport; ports.resolve = resolve; ports.now = trustedNow;
  ports.freeInternal = heapFree; ports.largestBlock = heapBlock; ports.millis = fakeMillis;
  ports.aliasState = state; ports.wake = wake; ports.roomName = roomName;
  ports.roomGuestPassword = roomPassword; ports.roomSetGuestPassword = roomSetPassword;
  cloudRoomLinkSetPorts(ports);
  secrets = {Canonical, Code, Token, OtherToken, "FIXTUREPW", hex(privateKey(2).data(), 64), hex(privateKey(1).data(), 64)};
}
void leakCheck(size_t retained = 0) {
  assert(psram_test::allocations.size() == retained);
  assert(transportsLive == 0);
}
bool containsSecret(const std::string &text) {
  for (const auto &secret : secrets) if (text.find(secret) != std::string::npos) return true;
  return false;
}
std::string say(const std::string &line, bool secretAllowed = false) {
  char reply[160]{};
  const Dispatch dispatch;
  assert(cloudRoomLinkCommand(line.c_str(), reply, 146));
  if (strlen(reply) > 145) { fprintf(stderr, "too long (%zu): %s\n", strlen(reply), reply); abort(); }
  if (!secretAllowed && containsSecret(reply)) { fprintf(stderr, "secret in reply to %s: %s\n", line.c_str(), reply); abort(); }
  return reply;
}
// The network task runs a queued job; the dispatch task applies it and asks for the restart;
// the next network pass schedules the restart.
void pump() {
  cloudRoomLinkRun();
  { const Dispatch dispatch; cloudRoomLinkLoop(); }
  cloudRoomLinkRun();
}
CloudRoomSettingsV2 current() {
  CloudRoomSettingsV2 settings{};
  assert(cloudRoomLoadSettings(settings) == CloudRoomLoad::Ready);
  return settings;
}
std::string savedHash() { char reply[100]; assert(cloudRoomPreferencesCommand("config hash", reply, sizeof(reply))); return reply; }
// Saves `settings`, then simulates a restart so the alias is part of the running configuration.
void bootWith(CloudRoomSettingsV2 settings) {
  char why[100];
  assert(cloudRoomPublishSettings(settings, why, sizeof(why)));
  cloudRoomBootResetForTest();
  assert(cloudRoomConfiguration());
}
CloudRoomSettingsV2 linked(uint8_t role, bool systemRoots = true, uint8_t flags = 0, const char *id = Alias,
                           const char *host = Host, uint8_t seed = 9) {
  CloudRoomSettingsV2 s{};
  memcpy(s.magic, "CRC\2", 4); s.enabled = 1; s.count = 1;
  auto &a = s.aliases[0];
  snprintf(a.base.host, sizeof(a.base.host), "%s", host);
  snprintf(a.base.token, sizeof(a.base.token), "%s", Token.c_str());
  snprintf(a.base.id, sizeof(a.base.id), "%s", id);
  snprintf(a.base.name, sizeof(a.base.name), "Fleet");
  FixedRng rng(seed);
  mesh::LocalIdentity identity(&rng);
  memcpy(a.base.publicKey, identity.pub_key, 32);
  a.role = role; a.caMode = systemRoots ? CloudCaSystem : CloudCaPinned; a.flags = flags;
  if (!systemRoots) snprintf(a.base.ca, sizeof(a.base.ca), "-----BEGIN CERTIFICATE-----\nfixture\n-----END CERTIFICATE-----\n");
  return s;
}
std::string redeemOk(const std::string &alias, const std::string &name, const std::string &pub, const std::string &token,
                     const std::string &role) {
  return "{\"ok\":1,\"alias\":\"" + alias + "\",\"name\":\"" + name + "\",\"pub\":\"" + pub + "\",\"token\":\"" + token +
         "\",\"role\":\"" + role + "\"}";
}
std::string error(const std::string &code) { return "{\"ok\":0,\"e\":\"" + code + "\",\"m\":\"x\"}"; }
const std::string QueuedHost = std::string("Contacting ") + Host +
    " (up to 20 s). Send \"cloudroom result\" for the outcome; the node restarts when linked.";
}
namespace {
// ---- signing and key export -------------------------------------------------------------
void identitySeams() {
  begin();
  // Golden vectors from the protocol specification: seed 0x01 x 32, public values only.
  assert(publicHex(1) == "8a88e3dd7409f195fd52db2d3cba5d72ca6709bf1d94121bf3748801b40f6f5c");
  char node[65], signature[129];
  assert(cloudnode::signRedeem("rooms.example.workers.dev", "join", Golden, "-", 1790000000, node, signature));
  assert(std::string(node) == publicHex(1));
  assert(std::string(signature) == "ccdfb5be2047595999ec3a80c81c47475e7ad28f4ad33ecce160a8c18e570b0adda433994dadefa1f15717d19fe2843a27edb17a075831cb8c13606505a03f08");
  char subject[17];
  cloudnode::sha256Hex("Fleet", 5, 8, subject);
  assert(std::string(subject) == "2bb4f92ced331c66");
  assert(cloudnode::signRedeem("rooms.example.workers.dev", "create", Golden, subject, 1790000000, node, signature));
  assert(std::string(signature) == "f3c17a27e16ca34f2d994dca31d3ce4772ebb396e66294a2f067dddf2c7918598b73b9202794faf50d39ed4ffb8c4231aa25a6d64c5b46f410e7ff23859cab09");
  char hash[65];
  cloudnode::codeHash(Golden, hash);
  assert(std::string(hash) == "375c09ce57a687a702bf9c4115b93cd0a0e86597a0feed49facca786589e50be");
  // The signature verifies with the repo's Ed25519 over the exact message bytes.
  const std::string message = std::string("aspen-node.v1\nhttps://rooms.example.workers.dev\ncreate\n") +
      std::string(hash, 32) + "\n" + publicHex(1) + "\n1790000000\n" + subject;
  uint8_t raw[64];
  for (unsigned i = 0; i < 64; ++i) raw[i] = uint8_t(std::stoi(std::string(signature + i * 2, 2), nullptr, 16));
  FixedRng rng(1);
  mesh::LocalIdentity identity(&rng);
  assert(identity.verify(raw, reinterpret_cast<const uint8_t *>(message.data()), int(message.size())));
  raw[7] ^= 1;
  assert(!identity.verify(raw, reinterpret_cast<const uint8_t *>(message.data()), int(message.size())));
  // "OPEN" signs the literal word instead of a code hash.
  assert(cloudnode::signRedeem("rooms.example.workers.dev", "create", "OPEN", subject, 1790000000, node, signature));

  // An absent management identity or a staged rotation never signs.
  identity_test::durable.erase({"mc-onchip", "management"});
  assert(!cloudnode::signRedeem(Host, "join", Golden, "-", 1790000000, node, signature));
  putIdentity("management", 1);
  stageIdentity("management", 6, 1);
  assert(!cloudnode::signRedeem(Host, "join", Golden, "-", 1790000000, node, signature));
  putIdentity("management", 1);
  stageIdentity("management", 6, 3);
  assert(!cloudnode::signRedeem(Host, "join", Golden, "-", 1790000000, node, signature));

  // exportRoleKey: Ok for an active key; refuses absent, staged, reset and invalid records and
  // never creates one.
  uint8_t key[PRV_KEY_SIZE]{};
  const auto untouched = [&] { for (uint8_t b : key) if (b) return false; return true; };
  assert(exportRoleKey("room", key) == ExportResult::Ok && hex(key, 64) == hex(privateKey(2).data(), 64));
  volatile uint8_t *scrub = key;
  for (unsigned i = 0; i < sizeof(key); ++i) scrub[i] = 0;
  identity_test::durable.erase({"mc-onchip", "room"});
  assert(exportRoleKey("room", key) == ExportResult::Absent && untouched());
  assert(!identity_test::durable.count({"mc-onchip", "room"}));
  putIdentity("room", 2);
  stageIdentity("room", 7, 1);
  assert(exportRoleKey("room", key) == ExportResult::Staged && untouched());
  putIdentity("room", 2);
  stageIdentity("room", 7, 3);
  assert(exportRoleKey("room", key) == ExportResult::Reset && untouched());
  identity_test::durable[{"mc-onchip", "room"}] = std::vector<uint8_t>(10, 1);
  assert(exportRoleKey("room", key) == ExportResult::Invalid && untouched());
  assert(exportRoleKey("unknown", key) == ExportResult::Invalid && untouched());
  uint8_t first;
  assert(identityFirstByte("repeater", first) && first == privateKeyFirst(3));
  assert(!identityFirstByte("command-bot", first));
}
}
namespace {
// ---- link ------------------------------------------------------------------------------
std::string jsonField(const std::string &json, const std::string &name) {
  const auto at = json.find("\"" + name + "\":");
  if (at == std::string::npos) return "";
  size_t from = at + name.size() + 3;
  if (json[from] == '"') return json.substr(from + 1, json.find('"', from + 1) - from - 1);
  return json.substr(from, json.find_first_of(",}", from) - from);
}
void joinFrontend() {
  begin();
  server = [&](const Seen &) { return Response{200, redeemOk(Alias, "Fleet", publicHex(9), Token, "frontend")}; };
  assert(say(std::string("join ") + Code + "@" + Host) == QueuedHost);
  assert(wakes == 1 && cloudRoomLinkPending());
  assert(say("result") == "Link working.");
  assert(say(std::string("join ") + Code + "@" + Host) == "Error: link busy; send \"cloudroom result\"");
  pump();
  assert(!cloudRoomLinkPending() && transportsLive == 0 && opens == 1 && closes >= 1);
  assert(say("result") == "Linked. Restarts in 10 s. Ask the room owner for the password. If the room is not in your app, run \"cloudroom advertise 1\".");
  assert(restarts == std::vector<uint32_t>{10000});
  // One signed request, to the right path, with the documented fields and no token or key.
  assert(seen.size() == 1 && seen[0].method == "POST" && seen[0].path == "/v1/node/redeem" && seen[0].bearer.empty());
  const auto &body = seen[0].body;
  assert(jsonField(body, "p") == "aspen-node.v1" && jsonField(body, "op") == "join" && jsonField(body, "code") == Canonical);
  assert(jsonField(body, "node") == publicHex(1) && jsonField(body, "ts") == "1790000000" && body.find("\"key\"") == std::string::npos);
  assert(lastHost == Host && lastSystemRoots && lastCa.empty() && resolved == std::vector<std::string>{Host});
  // avoid: first bytes of the other role identities, in a fixed order, without duplicates.
  const std::string avoid = jsonField(body, "avoid");
  char expected[32];
  snprintf(expected, sizeof(expected), "%02x%02x%02x%02x%02x", privateKeyFirst(3), privateKeyFirst(4), privateKeyFirst(5),
           privateKeyFirst(1), privateKeyFirst(2));
  assert(avoid == expected);
  // The signature verifies against the documented message.
  char hash[65];
  cloudnode::codeHash(Canonical, hash);
  const std::string message = std::string("aspen-node.v1\nhttps://") + Host + "\njoin\n" + std::string(hash, 32) + "\n" +
      publicHex(1) + "\n1790000000\n-";
  const std::string sigHex = jsonField(body, "sig");
  uint8_t sig[64];
  for (unsigned i = 0; i < 64; ++i) sig[i] = uint8_t(std::stoi(sigHex.substr(i * 2, 2), nullptr, 16));
  FixedRng rng(1);
  mesh::LocalIdentity node(&rng);
  assert(node.verify(sig, reinterpret_cast<const uint8_t *>(message.data()), int(message.size())));
  // Saved: a version 2 frontend alias on system roots, sharing enabled; nothing else changed.
  const auto s = current();
  assert(s.enabled && s.count == 1 && s.aliases[0].role == CloudRoleFrontend && s.aliases[0].caMode == CloudCaSystem &&
         s.aliases[0].flags == 0 && !strcmp(s.aliases[0].base.id, Alias) && !strcmp(s.aliases[0].base.host, Host) &&
         !strcmp(s.aliases[0].base.token, Token.c_str()) && !strcmp(s.aliases[0].base.name, "Fleet") &&
         !s.aliases[0].base.ca[0] && !strcmp(s.aliases[0].base.address, ""));
  assert(hex(s.aliases[0].base.publicKey, 32) == publicHex(9));
  leakCheck();
}
}
namespace {
const char *Wide = "a23456789.b23456789.c23456789.exampl";
void signedBody(const std::string &body, const std::string &host, const std::string &op, const std::string &codeHash,
                const std::string &subject, uint8_t seed = 1) {
  const std::string message = std::string("aspen-node.v1\nhttps://") + host + "\n" + op + "\n" + codeHash + "\n" +
      publicHex(seed) + "\n" + jsonField(body, "ts") + "\n" + subject;
  const std::string sigHex = jsonField(body, "sig");
  assert(sigHex.size() == 128);
  uint8_t sig[64];
  for (unsigned i = 0; i < 64; ++i) sig[i] = uint8_t(std::stoi(sigHex.substr(i * 2, 2), nullptr, 16));
  FixedRng rng(seed);
  mesh::LocalIdentity node(&rng);
  assert(node.verify(sig, reinterpret_cast<const uint8_t *>(message.data()), int(message.size())));
}
std::string codeHashHex(const std::string &code) {
  char hash[65];
  cloudnode::codeHash(code.c_str(), hash);
  return std::string(hash, 32);
}
void createRoom() {
  begin();
  server = [&](const Seen &) { return Response{200, redeemOk(Alias, "Fleet", publicHex(9), Token, "owner")}; };
  assert(say(std::string("create ") + Code + "@" + Host + " Fleet") == QueuedHost);
  pump();
  assert(say("result") == "Linked. Restarts in 10 s. Add the room in your companion app (cloudroom advertise 1 re-announces it), then send \"cloudroom password\".");
  assert(seen.size() == 1 && jsonField(seen[0].body, "op") == "create" && jsonField(seen[0].body, "name") == "Fleet");
  signedBody(seen[0].body, Host, "create", codeHashHex(Canonical), "2bb4f92ced331c66");
  const auto s = current();
  assert(s.count == 1 && s.aliases[0].role == CloudRoleOwner && s.aliases[0].flags == CloudFlagAdvertOnce &&
         s.aliases[0].caMode == CloudCaSystem && restarts.size() == 1);
  // An open server takes the word OPEN in place of a code, and the signature says so.
  begin();
  server = [&](const Seen &) { return Response{200, redeemOk(Alias, "Fleet", publicHex(9), Token, "owner")}; };
  assert(say(std::string("create open@") + Host + " Fleet") == QueuedHost);
  pump();
  assert(jsonField(seen[0].body, "code") == "OPEN");
  signedBody(seen[0].body, Host, "create", "open", "2bb4f92ced331c66");
  // A name with a quote is escaped in the request and echoed back unescaped.
  begin();
  server = [&](const Seen &) { return Response{200, redeemOk(Alias, "Say \\\"hi\\\"", publicHex(9), Token, "owner")}; };
  assert(say(std::string("create ") + Code + "@" + Host + " Say \"hi\"") == QueuedHost);
  pump();
  assert(seen[0].body.find("\"name\":\"Say \\\"hi\\\"\"") != std::string::npos);
  assert(!strcmp(current().aliases[0].base.name, "Say \"hi\""));
  leakCheck();
}
void convertRoom() {
  begin();
  const std::string preview = std::string("Key goes to ") + Host + "; it can speak as the room. Admin logins stay. Add \"yes ";
  assert(say(std::string("convert ") + Code + "@" + Host) == preview + Host + "\".");
  assert(say(std::string("convert ") + Code + "@" + Host + " yes") == preview + Host + "\".");
  assert(say(std::string("convert ") + Code + "@" + Host + " yes other.example") == preview + Host + "\".");
  assert(say(std::string("convert ") + Code) == "Key goes to rooms.ve6slp.ca; it can speak as the room. Admin logins stay. Add \"yes\".");
  assert(opens == 0 && seen.empty() && wakes == 0);
  server = [&](const Seen &) { return Response{200, redeemOk(Alias, "Backyard Room", publicHex(2), Token, "owner")}; };
  assert(say(std::string("convert ") + Code + "@" + Host + " yes " + Host) == QueuedHost);
  pump();
  assert(say("result") == "Linked. Restarts in 10 s. Add the room in your companion app (cloudroom advertise 1 re-announces it), then send \"cloudroom password\".");
  const auto &body = seen[0].body;
  assert(jsonField(body, "op") == "convert" && jsonField(body, "pub") == publicHex(2) && jsonField(body, "name") == "Backyard Room");
  assert(jsonField(body, "key") == hex(privateKey(2).data(), 64) && jsonField(body, "pw") == "FIXTUREPW");
  signedBody(body, Host, "convert", codeHashHex(Canonical), publicHex(2));
  // The avoid list leaves out the Room role's own byte.
  char expected[32];
  snprintf(expected, sizeof(expected), "%02x%02x%02x%02x", privateKeyFirst(3), privateKeyFirst(4), privateKeyFirst(5), privateKeyFirst(1));
  assert(jsonField(body, "avoid") == expected);
  const auto s = current();
  assert(s.count == 1 && s.aliases[0].role == CloudRoleOwner && s.aliases[0].caMode == CloudCaSystem &&
         s.aliases[0].flags == (CloudFlagConverted | CloudFlagAdvertOnce) && hex(s.aliases[0].base.publicKey, 32) == publicHex(2));
  assert(restarts.size() == 1 && cloudRoomOwnsLocalRoom());
  leakCheck();

  // Default host: "yes" alone confirms it.
  begin();
  server = [&](const Seen &) { return Response{200, redeemOk(Alias, "Backyard Room", publicHex(2), Token, "owner")}; };
  assert(say(std::string("convert ") + Code + " yes").rfind("Contacting rooms.ve6slp.ca", 0) == 0);
  pump();
  assert(lastHost == "rooms.ve6slp.ca" && !strcmp(current().aliases[0].base.host, "rooms.ve6slp.ca"));

  // The response must name this room's key and name; anything else saves nothing.
  const std::string refusals[] = {
    redeemOk(Alias, "Backyard Room", publicHex(8), Token, "owner"),
    redeemOk(Alias, "Another Name", publicHex(2), Token, "owner"),
    redeemOk(Alias, "Backyard Room", publicHex(2), Token, "frontend"),
  };
  for (const auto &answer : refusals) {
    begin();
    server = [&](const Seen &) { return Response{200, answer}; };
    assert(say(std::string("convert ") + Code + "@" + Host + " yes " + Host) == QueuedHost);
    pump();
    assert(say("result") == std::string("Server and node versions differ. Update the Worker (cloudroom probe ") + Host + ").");
    assert(savedHash() == "none" && restarts.empty());
  }

  // Preconditions are checked at the preview and again on "yes"; no request leaves the node.
  struct Refusal { const char *text; std::function<void()> arrange; };
  const Refusal cases[] = {
    {"The Room role is off or has no key. Use \"cloudroom create\" for a new room.", [] { roomRunning = false; }},
    {"Room role has no key. Use \"cloudroom create\".", [] { identity_test::durable.erase({"mc-onchip", "room"}); }},
    {"Room key change pending; restart, then retry.", [] { stageIdentity("room", 7, 1); }},
    {"Room key was reset; restart, then retry.", [] { stageIdentity("room", 7, 3); }},
  };
  for (const auto &refusal : cases) {
    begin();
    refusal.arrange();
    assert(say(std::string("convert ") + Code + "@" + Host) == refusal.text);
    assert(say(std::string("convert ") + Code + "@" + Host + " yes " + Host) == refusal.text);
    assert(wakes == 0 && opens == 0);
  }
  // Slots: a full node refuses; the alias that already holds this key is replaced.
  begin();
  auto two = linked(CloudRoleFrontend, true, 0, Alias, Host, 10);
  two.count = 2; two.aliases[1] = linked(CloudRoleOwner, true, 0, OtherAlias, Host, 11).aliases[0];
  bootWith(two);
  assert(say(std::string("convert ") + Code + "@" + Host + " yes " + Host) ==
         std::string("All 2 room slots are in use (") + "r2q7x3bn.., rbbbbbbb..). Free one: cloudroom unlink ALIAS");
  begin();
  two = linked(CloudRoleFrontend, true, 0, Alias, Host, 10);
  two.count = 2; two.aliases[1] = linked(CloudRoleOwner, true, 0, OtherAlias, Host, 2).aliases[0];
  bootWith(two);
  const char *fresh = "rddddddddddddddddddddddddddddd";
  server = [&](const Seen &) { return Response{200, redeemOk(fresh, "Backyard Room", publicHex(2), Token, "owner")}; };
  assert(say(std::string("convert ") + Code + "@" + Host + " yes " + Host) == QueuedHost);
  pump();
  const auto replaced = current();
  assert(replaced.count == 2 && !strcmp(replaced.aliases[1].base.id, fresh) && replaced.aliases[1].flags == 3 &&
         !strcmp(replaced.aliases[0].base.id, Alias) && restarts.size() == 1);
}
}
namespace {
void joinWith(const std::string &answer, int status, const std::string &headers = "") {
  begin();
  server = [=](const Seen &) { return Response{status, answer, false, headers}; };
  assert(say(std::string("join ") + Code + "@" + Host) == QueuedHost);
  pump();
}
void serverErrors() {
  const std::string h = Host;
  struct Case { int status; std::string e, text; std::string headers; };
  const Case table[] = {
    {400, "code_invalid", "Link failed: code not valid for create or join on " + h + ". Check the code and the @host.", ""},
    {400, "code_expired", "Link failed: code expired. Ask the owner for a new one (cloudroom invite).", ""},
    {400, "code_used", "Link failed: code already used. Ask the owner for a new one (cloudroom invite).", ""},
    {400, "bad_request", "Server and node versions differ. Update the Worker (cloudroom probe " + h + ").", ""},
    {400, "name", "Server rejected the room name. Choose another name.", ""},
    {400, "key", "Server rejected the room key. Send \"reboot\", then retry.", ""},
    {401, "bad_sig", "Link failed: node signature rejected", ""},
    {403, "cap", "Server limit reached (rooms, nodes or codes). Ask the server operator.", ""},
    {403, "banned", "Server refused this node. Ask the server operator.", ""},
    {409, "clock", "Clock not set. Wait for SNTP (get sntp.server), then retry.", ""},
    {409, "collision", "This node already serves a room with the same ID byte. Ask the owner for a new room or unlink the other.", ""},
    {409, "exists", "That room is already hosted.", ""},
    {429, "rate", "Link failed: rate limited, retry in 42 s", "Retry-After: 42\r\n"},
    {503, "closed", "Server is not accepting new rooms. Ask the operator.", ""},
    {500, "internal", "Link failed: server unavailable. Run cloudroom probe " + h + ".", ""},
  };
  for (const auto &entry : table) {
    joinWith(error(entry.e), entry.status, entry.headers);
    const auto text = say("result");
    if (text != entry.text) { fprintf(stderr, "%s\n got  %s\n want %s\n", entry.e.c_str(), text.c_str(), entry.text.c_str()); abort(); }
    assert(savedHash() == "none" && restarts.empty() && !cloudRoomLinkPending());
    leakCheck();
  }
  // Responses that are not the documented JSON error.
  const std::pair<int, std::string> odd[] = {{500, "<html>bad gateway</html>"}, {404, ""}, {302, "{}"}, {200, "{\"ok\":0}"},
                                              {200, "[]"}, {200, "not json"}};
  for (const auto &entry : odd) {
    joinWith(entry.second, entry.first);
    const auto text = say("result");
    assert(text.rfind("Link failed: server unavailable", 0) == 0 || text.rfind("Server and node versions differ", 0) == 0);
    assert(savedHash() == "none" && restarts.empty());
  }
  // The default host adds a pointer for a code it does not know and for a closed signup.
  begin();
  server = [&](const Seen &) { return Response{400, error("code_invalid")}; };
  assert(say(std::string("join ") + Code).rfind("Contacting rooms.ve6slp.ca", 0) == 0);
  pump();
  assert(say("result") == "Link failed: code not valid for create or join on rooms.ve6slp.ca. Check the code and the @host."
                          " Request one at github.com/slepp/meshcore-aspen");
  begin();
  server = [&](const Seen &) { return Response{503, error("closed")}; };
  assert(say(std::string("join ") + Code).rfind("Contacting", 0) == 0);
  pump();
  assert(say("result") == "Server is not accepting new rooms. Ask the operator. Request one at github.com/slepp/meshcore-aspen");
}
void networkFaults() {
  struct Fault { Wire wire; const char *text; };
  const std::string h = Host;
  const Fault faults[] = {
    {Wire::OpenClock, "Clock not set. Wait for SNTP (get sntp.server), then retry."},
    {Wire::OpenVerify, nullptr}, {Wire::OpenMemory, "Low memory: run \"cloudroom share off\", restart, then retry."},
    {Wire::OpenOther, nullptr}, {Wire::Stall, nullptr}, {Wire::Lost, nullptr},
  };
  for (const auto &fault : faults) {
    begin();
    wire = fault.wire;
    assert(say(std::string("join ") + Code + "@" + Host) == QueuedHost);
    pump();
    const std::string expected = fault.text ? fault.text
        : fault.wire == Wire::OpenVerify ? "TLS verify failed for " + h + ": certificate not trusted. Check the host name."
        : "Link failed: server unavailable. Run cloudroom probe " + h + ".";
    assert(say("result") == expected);
    assert(savedHash() == "none" && restarts.empty() && !cloudRoomLinkPending());
    leakCheck();
  }
  // DNS failure, no transport, no trusted clock, low memory: nothing is sent.
  begin(); dnsWorks = false;
  say(std::string("join ") + Code + "@" + Host); pump();
  assert(say("result") == "Link failed: server unavailable. Run cloudroom probe " + h + "." && opens == 0);
  begin(); transportAvailable = false;
  say(std::string("join ") + Code + "@" + Host); pump();
  assert(say("result").rfind("Low memory", 0) == 0 && opens == 0);
  begin(); clockTrusted = false;
  say(std::string("join ") + Code + "@" + Host); pump();
  assert(say("result") == "Clock not set. Wait for SNTP (get sntp.server), then retry." && opens == 0);
  begin(); freeHeap = CloudLinkMinFreeInternal - 1;
  say(std::string("join ") + Code + "@" + Host); pump();
  assert(say("result").rfind("Low memory", 0) == 0 && opens == 0 && resolved.empty());
  begin(); largestBlock = CloudLinkMinLargestBlock - 1;
  say(std::string("join ") + Code + "@" + Host); pump();
  assert(say("result").rfind("Low memory", 0) == 0 && opens == 0);
  leakCheck();

  // Lost reply, then the same command again: the node signs a new request and the saved
  // state ends up as if the first reply had arrived.
  begin();
  wire = Wire::Lost;
  say(std::string("join ") + Code + "@" + Host); pump();
  assert(seen.empty() && savedHash() == "none");
  wire = Wire::Normal; utc += 2;
  server = [&](const Seen &) { return Response{200, redeemOk(Alias, "Fleet", publicHex(9), Token, "frontend")}; };
  assert(say(std::string("join ") + Code + "@" + Host) == QueuedHost);
  pump();
  assert(say("result").rfind("Linked.", 0) == 0 && current().count == 1 && jsonField(seen[0].body, "ts") == "1790000002");
}
void guards() {
  // One job at a time, a staged upload, a missing identity, the failure cap and slots.
  begin();
  server = [&](const Seen &) { return Response{400, error("code_used")}; };
  for (unsigned i = 0; i < CloudLinkFailureLimit; ++i) {
    assert(say(std::string("join ") + Code + "@" + Host) == QueuedHost);
    pump();
    assert(cloudRoomLinkFailures() == i + 1);
  }
  assert(say(std::string("join ") + Code + "@" + Host) == "Too many failed attempts. Send \"reboot\", then retry.");
  assert(say(std::string("create ") + Code + "@" + Host + " Fleet") == "Too many failed attempts. Send \"reboot\", then retry.");
  assert(seen.size() == CloudLinkFailureLimit);

  begin();
  char reply[160];
  assert(cloudRoomPreferencesCommand(("config begin " + std::string(64, '0')).c_str(), reply, sizeof(reply)));
  assert(std::string(reply) == "Cloud room upload ready received=0");
  assert(say(std::string("join ") + Code + "@" + Host) == "Error: cloud room upload in progress; commit or abort it first");
  assert(say("share off") == "Nothing linked. Use cloudroom join or create.");
  assert(say(std::string("convert ") + Code + "@" + Host + " yes " + Host) == "Error: cloud room upload in progress; commit or abort it first");
  assert(wakes == 0);
  assert(cloudRoomPreferencesCommand("config abort", reply, sizeof(reply)));

  begin();
  identity_test::durable.erase({"mc-onchip", "management"});
  assert(say(std::string("join ") + Code + "@" + Host) == "Management identity unavailable");
  putIdentity("management", 1);
  stageIdentity("management", 6, 1);
  assert(say(std::string("join ") + Code + "@" + Host) == "Management identity unavailable");
  assert(say("probe") == "Management identity unavailable" && wakes == 0);

  begin();
  wakeWorks = false;
  assert(say(std::string("join ") + Code + "@" + Host).rfind("Error: the shared-room service cannot run", 0) == 0);
  assert(!cloudRoomLinkPending());
  wakeWorks = true;
  assert(say(std::string("join ") + Code + "@" + Host) == QueuedHost);
  cloudRoomLinkResetForTest();

  // Both slots used: join and create answer with the slot message.
  begin();
  auto two = linked(CloudRoleFrontend, true, 0, Alias, Host, 10);
  two.count = 2; two.aliases[1] = linked(CloudRoleOwner, true, 0, OtherAlias, Host, 11).aliases[0];
  bootWith(two);
  const std::string full = "All 2 room slots are in use (r2q7x3bn.., rbbbbbbb..). Free one: cloudroom unlink ALIAS";
  assert(say(std::string("join ") + Code + "@" + Host) == full);
  assert(say(std::string("create ") + Code + "@" + Host + " Fleet") == full);
  assert(wakes == 0);
}
void malformedAnswers() {
  const std::string pub = publicHex(9);
  const std::string good = redeemOk(Alias, "Fleet", pub, Token, "frontend");
  auto edit = [&](const std::string &from, const std::string &to) {
    std::string text = good;
    const auto at = text.find(from);
    assert(at != std::string::npos);
    return text.replace(at, from.size(), to);
  };
  const std::string bad[] = {
    edit(Alias, "r2q7x3bndkamw4qzfxe7abcdmn2xy"),                         // short id
    edit(Alias, "R2q7x3bndkamw4qzfxe7abcdmn2xyz"),                        // upper case
    edit(Alias, "r2q7x3bndkamw4qzfxe7abcdmn2xy1"),                        // outside the alphabet
    edit(Alias, "r2q7x3bndkamw4qzfxe7abcdmn2xyza"),                       // long id
    edit("Fleet", ""),                                                     // empty name
    edit("Fleet", std::string(32, 'n')),                                   // name over 31 bytes
    edit("Fleet", "A\\u0007B"),                                            // control character
    edit(pub, std::string(64, 'z')),                                       // not hex
    edit(pub, "00" + pub.substr(2)),                                       // first byte 00
    edit(pub, "ff" + pub.substr(2)),                                       // first byte ff
    edit(pub, pub.substr(2)),                                              // short key
    edit(Token, "ar1_" + std::string(63, 'a')),                            // short token
    edit(Token, "ar2_" + std::string(64, 'a')),                            // wrong prefix
    edit(Token, "ar1_" + std::string(64, 'A')),                            // upper case token
    edit("frontend", "admin"),                                             // unknown role
    "{\"ok\":1}",                                                          // nothing else
    "{\"ok\":2,\"alias\":\"x\"}",
    "{\"ok\":1,\"alias\":5,\"name\":\"Fleet\",\"pub\":\"" + pub + "\",\"token\":\"" + Token + "\",\"role\":\"frontend\"}",
  };
  for (const auto &answer : bad) {
    joinWith(answer, 200);
    const auto text = say("result");
    if (text.rfind("Server and node versions differ", 0) != 0) { fprintf(stderr, "accepted %s -> %s\n", answer.c_str(), text.c_str()); abort(); }
    assert(savedHash() == "none" && restarts.empty() && !cloudRoomLinkPending());
    leakCheck();
  }
  // A create answer must echo the name and come back as an owner.
  begin();
  server = [&](const Seen &) { return Response{200, redeemOk(Alias, "Other", pub, Token, "owner")}; };
  say(std::string("create ") + Code + "@" + Host + " Fleet"); pump();
  assert(say("result").rfind("Server and node versions differ", 0) == 0 && savedHash() == "none");
  begin();
  server = [&](const Seen &) { return Response{200, redeemOk(Alias, "Fleet", pub, Token, "frontend")}; };
  say(std::string("create ") + Code + "@" + Host + " Fleet"); pump();
  assert(say("result").rfind("Server and node versions differ", 0) == 0 && savedHash() == "none");
  // Chunked bodies, extra fields and a long reply are handled by size, not by guesswork.
  begin();
  server = [&](const Seen &) {
    Response r{200, redeemOk(Alias, "Fleet", pub, Token, "frontend")};
    r.chunked = true;
    return r;
  };
  say(std::string("join ") + Code + "@" + Host); pump();
  assert(say("result").rfind("Linked.", 0) == 0);
  begin();
  server = [&](const Seen &) {
    return Response{200, std::string(2000, ' ') + redeemOk(Alias, "Fleet", pub, Token, "frontend")};
  };
  say(std::string("join ") + Code + "@" + Host); pump();
  assert(say("result").rfind("Link failed: server unavailable", 0) == 0 || say("result").rfind("Server and node", 0) == 0);
  assert(savedHash() == "none");
  // A failure to save after a good answer reports it and asks for the same command again.
  begin();
  server = [&](const Seen &) { return Response{200, good}; };
  failJournalWrite = true;
  say(std::string("join ") + Code + "@" + Host); pump();
  assert(say("result") == "Linked on the server, but saving failed. Send the same command again.");
  assert(restarts.empty() && savedHash() == "none");
  failJournalWrite = false;
  // The restart cannot be scheduled: saved, with the reboot instruction.
  begin();
  server = [&](const Seen &) { return Response{200, good}; };
  restartWorks = false;
  say(std::string("join ") + Code + "@" + Host); pump();
  assert(say("result") == "Linked and saved, but the state flush failed. Run \"reboot\" when ready.");
  assert(current().count == 1);
}
}
namespace {
Response okJson(const std::string &body) { return Response{200, body}; }
void ownerControls() {
  begin();
  bootWith(linked(CloudRoleOwner));
  // invite
  server = [&](const Seen &) { return okJson("{\"ok\":1,\"code\":\"ABCD-EFGH-JKMN-PQRS\",\"host\":\"rooms.example.workers.dev\",\"ttl\":600}"); };
  assert(say("invite") == QueuedHost);
  pump();
  assert(say("result", true) == std::string("Code ABCD-EFGH-JKMN-PQRS (10 min, once). Run: cloudroom join ABCD-EFGH-JKMN-PQRS@") + Host);
  assert(seen.size() == 1 && seen[0].path == std::string("/v1/aliases/") + Alias + "/control" && seen[0].bearer == Token &&
         seen[0].body == "{\"op\":\"invite\",\"role\":\"frontend\"}" && lastSystemRoots);
  server = [&](const Seen &) { return okJson("{\"ok\":1,\"code\":\"ABCD-EFGH-JKMN-PQRS\",\"host\":\"x\",\"ttl\":120}"); };
  say("invite owner"); pump();
  assert(seen.back().body == "{\"op\":\"invite\",\"role\":\"owner\"}");
  assert(say("result", true) == std::string("Owner code ABCD-EFGH-JKMN-PQRS (120 s, once). Run: cloudroom join ABCD-EFGH-JKMN-PQRS@") + Host);
  server = [&](const Seen &) { return okJson("{\"ok\":1,\"code\":\"ABCD-EFGH-JKMN-PQRS\",\"host\":\"x\",\"ttl\":86400}"); };
  say("invite"); pump();
  assert(say("result", true).find("(24 h, once)") != std::string::npos);
  // A server answer with a bad code is refused.
  server = [&](const Seen &) { return okJson("{\"ok\":1,\"code\":\"ABCD-EFGH-JKMN\",\"host\":\"x\",\"ttl\":600}"); };
  say("invite"); pump();
  assert(say("result").rfind("Server and node versions differ", 0) == 0);
  // The code is wiped from the result after five minutes.
  server = [&](const Seen &) { return okJson("{\"ok\":1,\"code\":\"ABCD-EFGH-JKMN-PQRS\",\"host\":\"x\",\"ttl\":600}"); };
  say("invite"); pump();
  assert(say("result", true).rfind("Code ", 0) == 0);
  extraMs = CloudLinkSecretMs + 5000;
  assert(say("result") == "No link job since restart. 1/1 r2q7x3bn.. \"Fleet\" owner live rooms.example.workers.dev");
  extraMs = 0;
  // nodes
  server = [&](const Seen &) {
    return okJson("{\"ok\":1,\"nodes\":[{\"id\":\"ab12cd34\",\"r\":\"o\",\"seen\":7200},{\"id\":\"77e1f0aa\",\"r\":\"f\",\"seen\":259200},"
                  "{\"id\":\"0a1b2c3d\",\"r\":\"f\",\"seen\":300},{\"id\":\"ffeeddcc\",\"r\":\"f\",\"seen\":777600},"
                  "{\"id\":\"11223344\",\"r\":\"f\",\"seen\":3600}],\"total\":12,\"next\":5}");
  };
  say("nodes"); pump();
  assert(say("result") == "Nodes 1-5 of 12: ab12cd34 owner 2h; 77e1f0aa node 3d; 0a1b2c3d node 5m; ffeeddcc node 9d; 11223344 node 1h");
  assert(seen.back().body == "{\"op\":\"nodes\",\"offset\":0}");
  say("nodes 3"); pump();
  assert(seen.back().body == "{\"op\":\"nodes\",\"offset\":10}" && say("result").rfind("Nodes 11-15 of 12:", 0) == 0);
  assert(say("nodes 0").rfind("Usage:", 0) == 0);
  // revoke and its refusals
  server = [&](const Seen &) { return okJson("{\"ok\":1}"); };
  say("revoke ab12cd34"); pump();
  assert(seen.back().body == "{\"op\":\"revoke\",\"id\":\"ab12cd34\"}" && say("result") == "Revoked ab12cd34.");
  const std::pair<std::pair<int, const char *>, const char *> refusals[] = {
    {{409, "self"}, "Use \"cloudroom unlink\" for this node."},
    {{409, "last_owner"}, "Other nodes still use this room. Run \"cloudroom invite owner\" for a node that stays, or \"cloudroom unlink ALIAS delete\"."},
    {{403, "too_new"}, "Owner added less than 24 h ago; try later."},
    {{403, "forbidden"}, "Only an owner node can do that. Ask the owner for \"cloudroom invite owner\"."},
    {{401, "auth"}, "Server revoked this node. Run \"cloudroom unlink ALIAS local\"."},
    {{403, "disabled"}, "Server disabled this room. Ask the server operator."},
    {{404, "not_found"}, std::string(std::string("Room is gone from ") + Host + ". Run \"cloudroom unlink ALIAS local\".").c_str()},
    {{429, "rate"}, "Link failed: rate limited, retry in 60 s"},
    {{400, "bad_request"}, std::string(std::string("Server and node versions differ. Update the Worker (cloudroom probe ") + Host + ").").c_str()},
  };
  const std::string gone = std::string("Room is gone from ") + Host + ". Run \"cloudroom unlink ALIAS local\".";
  const std::string versions = std::string("Server and node versions differ. Update the Worker (cloudroom probe ") + Host + ").";
  for (const auto &entry : refusals) {
    const std::string e = entry.first.second;
    server = [=](const Seen &) { return Response{entry.first.first, error(e)}; };
    say("revoke 11223344"); pump();
    const std::string want = e == "not_found" ? gone : e == "bad_request" ? versions : entry.second;
    assert(say("result") == want);
  }
  // password
  server = [&](const Seen &) { return okJson("{\"ok\":1,\"pw\":\"7QX2M9KDAB\"}"); };
  say("password"); pump();
  assert(seen.back().body == "{\"op\":\"password\",\"mode\":\"get\"}");
  assert(say("result", true) == "Room password 7QX2M9KDAB (open: cloudroom password open; change: cloudroom password PW|new)");
  say("password new"); pump();
  assert(seen.back().body == "{\"op\":\"password\",\"mode\":\"new\"}");
  say("password open"); pump();
  assert(seen.back().body == "{\"op\":\"password\",\"mode\":\"open\"}");
  say("password MYPW"); pump();
  assert(seen.back().body == "{\"op\":\"password\",\"mode\":\"set\",\"pw\":\"MYPW\"}");
  server = [&](const Seen &) { return okJson("{\"ok\":1,\"pw\":\"\"}"); };
  say("password open"); pump();
  assert(say("result") == "Room is open (no password). Set one: cloudroom password PW|new");
  // list and the idle view
  assert(say("list") == std::string("1/1 r2q7x3bn.. \"Fleet\" owner live ") + Host);
  aliasStates[0] = CloudAliasState::Revoked;
  assert(say("list") == std::string("1/1 r2q7x3bn.. \"Fleet\" owner revoked ") + Host);
  aliasStates[0] = CloudAliasState::Down;
  assert(say("list 1").find(" owner down ") != std::string::npos);
  assert(say("list 2") == "Error: no alias 2; cloudroom list 1..1");
  leakCheck(1);
}
void frontendRefusals() {
  begin();
  for (const char *verb : {"invite", "invite owner", "nodes", "revoke 11223344", "password", "password open"})
    assert(say(verb) == "Nothing linked. Use cloudroom join or create.");
  bootWith(linked(CloudRoleFrontend));
  for (const char *verb : {"invite", "invite owner", "nodes", "revoke 11223344", "password", "password open"})
    assert(say(verb) == "Only an owner node can do that. Ask the owner for \"cloudroom invite owner\".");
  assert(wakes == 0 && seen.empty());
  // A pinned CA host keeps using its pinned certificate; other hosts use the public roots.
  begin();
  bootWith(linked(CloudRoleOwner, false));
  server = [&](const Seen &) { return okJson("{\"ok\":1,\"code\":\"ABCD-EFGH-JKMN-PQRS\",\"host\":\"x\",\"ttl\":600}"); };
  say("invite"); pump();
  assert(!lastSystemRoots && lastCa.find("BEGIN CERTIFICATE") != std::string::npos);
  server = [&](const Seen &) { return Response{200, redeemOk(OtherAlias, "Fleet", publicHex(12), OtherToken, "frontend")}; };
  say(std::string("join ") + Code + "@" + Host); pump();
  assert(!lastSystemRoots && current().count == 2 && current().aliases[1].caMode == CloudCaPinned &&
         current().aliases[1].base.ca[0]);
  say("probe"); pump();
  server = [&](const Seen &) { return Response{200, redeemOk(OtherAlias, "Fleet", publicHex(12), OtherToken, "frontend")}; };
  begin();
  server = [&](const Seen &) { return Response{200, redeemOk(Alias, "Fleet", publicHex(12), Token, "frontend")}; };
  say(std::string("join ") + Code + "@elsewhere.example"); pump();
  assert(lastSystemRoots && lastHost == "elsewhere.example");
}
void unlinking() {
  // A frontend leaves; the record becomes "nothing linked", which only version 2 can hold.
  begin();
  bootWith(linked(CloudRoleFrontend));
  server = [&](const Seen &) { return okJson("{\"ok\":1,\"deleted\":0}"); };
  assert(say("unlink") == QueuedHost);
  pump();
  assert(seen.back().body == "{\"op\":\"leave\"}" && say("result") == "Unlinked. Restarting in 10 s." && restarts.size() == 1);
  assert(current().count == 0);
  {
    char reply[100];
    cloudRoomBootResetForTest();
    assert(!cloudRoomConfiguration() && cloudRoomPreferencesCommand("config status", reply, sizeof(reply)));
    assert(std::string(reply).find("saved=1 enabled=1 aliases=0") != std::string::npos);
  }
  // The last owner with other nodes still present is refused and nothing changes.
  begin();
  bootWith(linked(CloudRoleOwner));
  const auto hash = savedHash();
  server = [&](const Seen &) { return Response{409, error("last_owner")}; };
  say("unlink"); pump();
  assert(say("result").rfind("Other nodes still use this room.", 0) == 0 && savedHash() == hash && restarts.empty());
  // delete: owners only; with another owner the deletion is deferred and nothing changes locally.
  server = [&](const Seen &) { return okJson("{\"ok\":1,\"pending\":86400}"); };
  assert(say("unlink delete") == QueuedHost);
  pump();
  assert(seen.back().body == std::string("{\"op\":\"delete\",\"confirm\":\"") + Alias + "\"}");
  assert(say("result") == "Delete scheduled in 24 h; another owner can cancel it. Unlink this node now with \"cloudroom unlink\"." &&
         savedHash() == hash);
  server = [&](const Seen &) { return okJson("{\"ok\":1}"); };
  say("unlink delete"); pump();
  assert(say("result") == "Unlinked. Restarting in 10 s." && current().count == 0);
  begin();
  bootWith(linked(CloudRoleFrontend));
  assert(say("unlink delete") == "Only an owner node can do that. Ask the owner for \"cloudroom invite owner\".");

  // A converted node reads the node list first and leaves only when nobody else uses the room.
  begin();
  bootWith(linked(CloudRoleOwner, true, CloudFlagConverted, Alias, Host, 2));
  const auto convertedHash = savedHash();
  server = [&](const Seen &r) {
    if (r.body.find("\"nodes\"") != std::string::npos)
      return okJson("{\"ok\":1,\"nodes\":[{\"id\":\"ab12cd34\",\"r\":\"o\",\"seen\":1},{\"id\":\"77e1f0aa\",\"r\":\"f\",\"seen\":1}],\"total\":2,\"next\":null}");
    return okJson("{\"ok\":1,\"deleted\":0}");
  };
  say("unlink"); pump();
  const std::string brain = std::string("Room stays live on ") + Host + " for other nodes. Run \"cloudroom unlink ALIAS delete\" first, or add \"force\".";
  assert(say("result") == brain && savedHash() == convertedHash && seen.size() == 1);
  assert(say("unlink local") == brain && say("share off") == brain);
  assert(savedHash() == convertedHash && restarts.empty());
  // force leaves without asking and the Room role resumes after the restart.
  say("unlink force"); pump();
  assert(seen.size() == 2 && seen.back().body == "{\"op\":\"leave\"}" && current().count == 0);
  assert(say("result") == "Unlinked. Restarting in 10 s. Local Room password is the one it had before converting.");
  // Sole owner: the room is purged; its password goes back to the Room role when that is running.
  begin();
  bootWith(linked(CloudRoleOwner, true, CloudFlagConverted, Alias, Host, 2));
  server = [&](const Seen &r) {
    if (r.body.find("\"nodes\"") != std::string::npos)
      return okJson("{\"ok\":1,\"nodes\":[{\"id\":\"ab12cd34\",\"r\":\"o\",\"seen\":1}],\"total\":1,\"next\":null}");
    return okJson("{\"ok\":1,\"deleted\":1,\"pw\":\"7QX2M9KDAB\"}");
  };
  secrets.push_back("7QX2M9KDAB");
  say("unlink"); pump();
  assert(seen.size() == 2 && say("result") == "Unlinked. Restarting in 10 s." && writtenBack == "7QX2M9KDAB");
  begin();
  bootWith(linked(CloudRoleOwner, true, CloudFlagConverted, Alias, Host, 2));
  server = [&](const Seen &r) {
    if (r.body.find("\"nodes\"") != std::string::npos)
      return okJson("{\"ok\":1,\"nodes\":[{\"id\":\"ab12cd34\",\"r\":\"o\",\"seen\":1}],\"total\":1,\"next\":null}");
    return okJson("{\"ok\":1,\"deleted\":1,\"pw\":\"7QX2M9KDAB\"}");
  };
  roomRunning = true; writeBackWorks = false;
  say("unlink"); pump();
  assert(say("result") == "Unlinked. Restarting in 10 s. Local Room password is the one it had before converting." && writtenBack.empty());

  // Without the server: unlink local keeps the room slot free; a converted alias needs force.
  begin();
  bootWith(linked(CloudRoleFrontend));
  assert(say("unlink local") == "Alias removed locally. The server still lists this node; an owner can revoke 8a88e3dd.");
  pump();
  assert(current().count == 0 && restarts.size() == 1 && seen.empty());
  begin();
  bootWith(linked(CloudRoleOwner, true, CloudFlagConverted, Alias, Host, 2));
  assert(say("unlink local force") == "Local Room resumes; the server copy stays live. Alias removed locally. The server still lists this node; an owner can revoke 8a88e3dd.");
  assert(current().count == 0);
  // Two aliases: the name decides which one goes.
  begin();
  auto two = linked(CloudRoleFrontend, true, 0, Alias, Host, 10);
  two.count = 2; two.aliases[1] = linked(CloudRoleOwner, true, 0, OtherAlias, Host, 11).aliases[0];
  bootWith(two);
  assert(say("unlink local") == "Two rooms are linked. Name one: cloudroom unlink ALIAS (see cloudroom list)");
  assert(say("unlink local nosuch").rfind("Error: no such alias", 0) == 0);
  say("unlink local 2");
  assert(current().count == 1 && !strcmp(current().aliases[0].base.id, Alias));
  assert(say("list") == std::string("1/1 r2q7x3bn.. \"Fleet\" frontend ") + (aliasStates.count(0) ? "down" : "live") + " " + Host);
  begin();
  bootWith(two);
  server = [&](const Seen &) { return okJson("{\"ok\":1,\"deleted\":0}"); };
  assert(say(std::string("unlink ") + OtherAlias) == QueuedHost);
  pump();
  assert(seen.back().path == std::string("/v1/aliases/") + OtherAlias + "/control" && current().count == 1 &&
         !strcmp(current().aliases[0].base.id, Alias));
  // A freshly linked alias is not running until the restart.
  begin();
  server = [&](const Seen &) { return Response{200, redeemOk(Alias, "Fleet", publicHex(9), Token, "frontend")}; };
  say(std::string("join ") + Code + "@" + Host); pump();
  assert(say("unlink") == "That room is not running yet. Restart, or use \"cloudroom unlink ALIAS local\".");
  assert(say("unlink local").rfind("Alias removed locally", 0) == 0);
  assert(say("unlink") == "Nothing linked. Use cloudroom join or create.");
  // A room from a provider or a profile upload has no hosted id: the server's control route
  // does not know it, so unlinking removes it locally without a request.
  begin();
  bootWith(linked(CloudRoleFrontend, false, 0, "A"));
  assert(say("unlink delete") == "This room is not hosted on a shared-room server. Run \"cloudroom unlink ALIAS\" without delete.");
  assert(say("unlink") == "Alias removed locally." && opens == 0 && seen.empty());
  pump();
  assert(current().count == 0 && restarts.size() == 1);
}
void sharing() {
  begin();
  assert(say("share on") == "Nothing linked. Use cloudroom join or create.");
  assert(say("list") == "Nothing linked. Use cloudroom join or create.");
  bootWith(linked(CloudRoleFrontend));
  assert(say("share on") == "Sharing is already on.");
  assert(say("share off") == "Sharing off. Restarting in 10 s; send \"cloudroom share on\" to resume." && restarts.empty());
  pump();
  assert(restarts.size() == 1);
  assert(!current().enabled && current().count == 1 && current().aliases[0].role == CloudRoleFrontend);
  assert(say("share off") == "Sharing is already off.");
  assert(say("list") == std::string("1/1 r2q7x3bn.. \"Fleet\" frontend off ") + Host || say("list").find(" off ") != std::string::npos);
  assert(say("share on") == "Sharing on. Restarting in 10 s." && current().enabled);
  pump();
  assert(restarts.size() == 2);
  // A converted alias hands the room back only with force.
  begin();
  bootWith(linked(CloudRoleOwner, true, CloudFlagConverted, Alias, Host, 2));
  assert(cloudRoomOwnsLocalRoom());
  assert(say("share off").rfind("Room stays live on", 0) == 0 && cloudRoomOwnsLocalRoom());
  assert(say("share off force") == "Local Room resumes; the server copy stays live. Restarting in 10 s; send \"cloudroom share on\" to resume.");
  assert(!cloudRoomOwnsLocalRoom() && current().aliases[0].flags == CloudFlagConverted);
  assert(say("share on") == "Sharing on. Restarting in 10 s." && cloudRoomOwnsLocalRoom());
  // Save failures and a missing restart service are reported.
  failJournalWrite = true;
  assert(say("share off force").find("outcome unknown") != std::string::npos);
  failJournalWrite = false;
  // The restart is scheduled after the reply; when it cannot be, `result` says so.
  restartWorks = false;
  assert(say("share off force").find("Restarting in 10 s") != std::string::npos);
  pump();
  assert(say("result") == "Saved, but the state flush failed. Run \"reboot\" when ready.");
  assert(say("apply") == "Restarting in 10 s.");
  pump();
  assert(say("result") == "Saved, but the state flush failed. Run \"reboot\" when ready.");
  restartWorks = true; wakeWorks = false;
  assert(say("apply") == "Error: the restart could not be scheduled; send \"reboot\"");
  wakeWorks = true;
  assert(say("apply") == "Restarting in 10 s.");
  pump();
  assert(restarts.size() == 1 && say("result") == "No link job since restart. 1/1 r2q7x3bn.. \"Fleet\" owner off " + std::string(Host));
}
void probing() {
  begin();
  server = [&](const Seen &) {
    return okJson("{\"service\":\"aspen-shared-room\",\"version\":\"0.1.20\",\"protocol\":\"aspen-node.v1\",\"signup\":\"code\",\"time\":1790000001}");
  };
  assert(say(std::string("probe ") + Host) == QueuedHost.substr(0, 0) + std::string("Contacting ") + Host +
         " (up to 20 s). Send \"cloudroom result\" for the outcome; the node restarts when linked.");
  pump();
  assert(say("result") == std::string("Server ") + Host + " ok: aspen-shared-room 0.1.20 signup=code clock+1s");
  assert(seen.back().method == "GET" && seen.back().path == "/v1/info" && seen.back().bearer.empty());
  assert(say("probe").rfind("Contacting rooms.ve6slp.ca", 0) == 0);
  pump();
  assert(lastHost == "rooms.ve6slp.ca");
  server = [&](const Seen &) { return Response{404, "nope"}; };
  say(std::string("probe ") + Host); pump();
  assert(say("result") == std::string("Server ") + Host + " failed: HTTP 404 not an Aspen server");
  server = [&](const Seen &) { return okJson("{\"service\":\"aspen-shared-room\",\"version\":\"9\",\"protocol\":\"aspen-node.v9\",\"signup\":\"code\",\"time\":1}"); };
  say(std::string("probe ") + Host); pump();
  assert(say("result").find("failed: protocol aspen-node.v9 differs") != std::string::npos);
  dnsWorks = false;
  say(std::string("probe ") + Host); pump();
  assert(say("result") == std::string("Server ") + Host + " failed: DNS");
  dnsWorks = true;
  for (const auto &entry : std::vector<std::pair<Wire, const char *>>{{Wire::OpenClock, "TLS clock"}, {Wire::OpenVerify, "TLS verify"}, {Wire::Lost, "no response"}}) {
    wire = entry.first;
    say(std::string("probe ") + Host); pump();
    assert(say("result") == std::string("Server ") + Host + " failed: " + entry.second);
  }
  // Probing needs no trusted clock for its own sake; the skew is omitted.
  wire = Wire::Normal; clockTrusted = false;
  server = [&](const Seen &) {
    return okJson("{\"service\":\"aspen-shared-room\",\"version\":\"0.1.20\",\"protocol\":\"aspen-node.v1\",\"signup\":\"open\",\"time\":1790000001}");
  };
  say(std::string("probe ") + Host); pump();
  assert(say("result") == std::string("Server ") + Host + " ok: aspen-shared-room 0.1.20 signup=open");
}
void advertOnce() {
  // The first connection of a created room announces it; the saved flag then clears, once.
  begin();
  bootWith(linked(CloudRoleOwner, true, CloudFlagAdvertOnce));
  const auto before = savedHash();
  { const Dispatch dispatch; cloudRoomLinkLoop(); }
  assert(savedHash() == before);
  cloudRoomLinkAdvertised(0);
  { const Dispatch dispatch; cloudRoomLinkLoop(); }
  assert(current().aliases[0].flags == 0 && current().aliases[0].role == CloudRoleOwner && savedHash() != before);
  const auto after = savedHash();
  cloudRoomLinkAdvertised(0);
  { const Dispatch dispatch; cloudRoomLinkLoop(); }
  assert(savedHash() == after);
  // A converted room keeps its converted flag when the advert flag clears.
  begin();
  bootWith(linked(CloudRoleOwner, true, CloudFlagAdvertOnce | CloudFlagConverted, Alias, Host, 2));
  cloudRoomLinkAdvertised(0);
  { const Dispatch dispatch; cloudRoomLinkLoop(); }
  assert(current().aliases[0].flags == CloudFlagConverted && cloudRoomOwnsLocalRoom());
  // A save failure leaves the flag for the next boot.
  begin();
  bootWith(linked(CloudRoleOwner, true, CloudFlagAdvertOnce));
  failJournalWrite = true;
  cloudRoomLinkAdvertised(0);
  { const Dispatch dispatch; cloudRoomLinkLoop(); }
  failJournalWrite = false;
  assert(current().aliases[0].flags == CloudFlagAdvertOnce);
}
void idleView() {
  begin();
  assert(say("result") == "No link job since restart.");
  char suffix[80];
  cloudRoomLinkStatus(suffix, sizeof(suffix));
  assert(std::string(suffix) == " link=idle room=off node=8a88e3dd");
  cloudRoomReportRoom(CloudRoomRoomMode::Local);
  cloudRoomLinkStatus(suffix, sizeof(suffix));
  assert(std::string(suffix) == " link=idle room=local node=8a88e3dd");
  cloudRoomReportRoom(CloudRoomRoomMode::Cloud);
  server = [&](const Seen &) { return Response{200, redeemOk(Alias, "Fleet", publicHex(9), Token, "frontend")}; };
  say(std::string("join ") + Code + "@" + Host);
  cloudRoomLinkStatus(suffix, sizeof(suffix));
  assert(std::string(suffix) == " link=working room=cloud node=8a88e3dd");
  pump();
  cloudRoomLinkStatus(suffix, sizeof(suffix));
  assert(std::string(suffix) == " link=ok room=cloud node=8a88e3dd");
  server = [&](const Seen &) { return Response{400, error("code_used")}; };
  begin();
  say(std::string("join ") + Code + "@" + Host); pump();
  cloudRoomLinkStatus(suffix, sizeof(suffix));
  assert(std::string(suffix) == " link=failed room=off node=8a88e3dd");
  identity_test::durable.erase({"mc-onchip", "management"});
  cloudRoomLinkStatus(suffix, sizeof(suffix));
  assert(std::string(suffix) == " link=failed room=off node=none");
  // Replies for verbs the lifecycle does not own fall through to the existing handlers.
  char reply[160];
  for (const char *line : {"status", "error", "advertise x", "config api", "enable on", ""})
    assert(!cloudRoomLinkCommand(line, reply, sizeof(reply)));
  // Usage and parse failures name the verb and echo nothing.
  assert(say("join").rfind("Usage: cloudroom join", 0) == 0);
  assert(say("join !!!!").find("!!!!") == std::string::npos);
}
}
namespace {
// Every reply at the widest host and name a DM can carry fits one tagged reply.
void widths() {
  const std::string host = Wide, name(31, 'N');
  assert(host.size() == 36);
  const char *codes[] = {"code_invalid", "code_expired", "code_used", "bad_request", "name", "key", "bad_sig", "cap", "banned",
                         "clock", "collision", "exists", "rate", "closed", "internal"};
  for (const char *code : codes) {
    begin();
    server = [=](const Seen &) { return Response{code == std::string("rate") ? 429 : 400, error(code), false, "Retry-After: 86400\r\n"}; };
    say(std::string("join ") + Code + "@" + host); pump();
    say("result");
    begin();
    server = [=](const Seen &) { return Response{400, error(code)}; };
    say(std::string("create ") + Code + "@" + host + " " + name); pump();
    say("result");
  }
  for (Wire fault : {Wire::OpenClock, Wire::OpenVerify, Wire::OpenMemory, Wire::OpenOther, Wire::Stall, Wire::Lost}) {
    begin();
    wire = fault;
    say(std::string("create ") + Code + "@" + host + " " + name); pump();
    say("result");
    say(std::string("probe ") + host); pump();
    say("result");
  }
  begin();
  dnsWorks = false;
  say(std::string("probe ") + host); pump(); say("result");
  say(std::string("join ") + Code + "@" + host); pump(); say("result");
  // Convert preview and confirmation with the widest host and a 31-byte room name.
  begin();
  strcpy(roomLabel, name.c_str());
  say(std::string("convert ") + Code + "@" + host);
  server = [&](const Seen &) { return Response{200, redeemOk(Alias, name, publicHex(2), Token, "owner")}; };
  say(std::string("convert ") + Code + "@" + host + " yes " + host); pump();
  say("result");
  const auto s = current();
  assert(strlen(s.aliases[0].base.name) == 31);
  // Controls on that alias, including a split-brain refusal and the list line.
  bootWith(s);
  server = [&](const Seen &) { return okJson("{\"ok\":1,\"code\":\"ABCD-EFGH-JKMN-PQRS\",\"host\":\"x\",\"ttl\":600}"); };
  say("invite"); pump(); say("result", true);
  say("invite owner"); pump(); say("result", true);
  server = [&](const Seen &) { return okJson("{\"ok\":1,\"nodes\":[{\"id\":\"ab12cd34\",\"r\":\"o\",\"seen\":1},{\"id\":\"77e1f0aa\",\"r\":\"f\",\"seen\":1}],\"total\":2,\"next\":null}"); };
  say("unlink"); pump(); say("result");
  say("unlink local"); say("share off"); say("list"); say("result");
  say("revoke 77e1f0aa"); pump(); say("result");
  server = [&](const Seen &) { return Response{404, error("not_found")}; };
  say("nodes 99999"); pump(); say("result");
  server = [&](const Seen &) { return Response{200, "{\"ok\":1,\"pw\":\"7QX2M9KDAB\"}"}; };
  say("password new"); pump(); say("result", true);
  // A host from a profile upload can be 127 bytes: replies leave the host out, never cut it.
  const std::string longHost = std::string(2, 'a') + std::string(60, 'b') + "." + std::string(63, 'c');
  begin();
  bootWith(linked(CloudRoleOwner, true, CloudFlagConverted, Alias, longHost.c_str(), 2));
  server = [&](const Seen &) { return okJson("{\"ok\":1,\"code\":\"ABCD-EFGH-JKMN-PQRS\",\"host\":\"x\",\"ttl\":600}"); };
  assert(say("invite").find(longHost) == std::string::npos); pump();
  assert(say("result", true).find("too long for a DM") != std::string::npos);
  assert(say("list").find(longHost) == std::string::npos);
  assert(say("unlink local").find(longHost) == std::string::npos);
  server = [&](const Seen &) { return Response{404, error("not_found")}; };
  say("nodes"); pump();
  assert(say("result").find(longHost) == std::string::npos);
  wire = Wire::OpenVerify;
  say("nodes"); pump();
  assert(say("result").find(longHost) == std::string::npos);
  say("probe " + std::string(Wide)); pump(); say("result");
  // Alias ids and the node id: two slots.
  begin();
  auto two = linked(CloudRoleFrontend, true, 0, Alias, Wide, 10);
  two.count = 2; two.aliases[1] = linked(CloudRoleOwner, true, 0, OtherAlias, Wide, 11).aliases[0];
  strcpy(two.aliases[0].base.name, name.c_str());
  bootWith(two);
  say("list"); say("list 2"); say("result");
  say(std::string("join ") + Code + "@" + host);
}
}
// Everything the node printed must be free of codes, tokens, keys and passwords.
void scanLog(const std::string &printed) {
  std::vector<std::string> forbidden = {Canonical, Code, Token, OtherToken, "FIXTUREPW", "7QX2M9KDAB", "MYPW",
                                        hex(privateKey(2).data(), 64), hex(privateKey(1).data(), 64),
                                        hex(privateKey(2).data(), 32), hex(privateKey(1).data(), 32)};
  for (const auto &secret : forbidden)
    if (printed.find(secret) != std::string::npos) { fprintf(stderr, "log contains a secret: %.8s...\n", secret.c_str()); abort(); }
}
int main(int argc, char **argv) {
  const std::string mode = argc > 1 ? argv[1] : "all";
  std::string printed;
  serial_test::capture = &printed;
  if (mode == "identity") identitySeams();
  else if (mode == "join") joinFrontend();
  else if (mode == "create") createRoom();
  else if (mode == "convert") convertRoom();
  else if (mode == "errors") serverErrors();
  else if (mode == "faults") networkFaults();
  else if (mode == "guards") guards();
  else if (mode == "malformed") malformedAnswers();
  else if (mode == "owner") ownerControls();
  else if (mode == "refusals") frontendRefusals();
  else if (mode == "unlink") unlinking();
  else if (mode == "share") sharing();
  else if (mode == "probe") probing();
  else if (mode == "idle") idleView();
  else if (mode == "advert") advertOnce();
  else if (mode == "widths") widths();
  else { fprintf(stderr, "unknown mode\n"); return 2; }
  scanLog(printed);
  return 0;
}
