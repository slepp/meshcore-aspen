// SPDX-License-Identifier: Apache-2.0
// Device agent for the shared-room end-to-end test (services/shared-room/test/e2e).
//
// Runs the real lifecycle code (grammar, link engine, saved records, identities, signing)
// against a real Worker over plain TCP on this machine. TLS is the only stand-in: the
// transport connects to a loopback port chosen by `host` and sends the same HTTP request the
// device would send over TLS. Several devices live in one process; each owns its saved
// records, identities, Room role and restart state, and `device NAME` switches between them.
//
// Input is one command per line, output is one reply line per command. A test drives this
// through stdin and stdout; every secret the test needs is read from the saved record by
// `saved`, never from a reply. Fixture identities only.
//
//   host HOST PORT        route HOST to 127.0.0.1:PORT
//   device NAME           switch to (and boot) a device; new devices get a fresh identity set
//   room NAME PASSWORD    give the device a running local Room role with this name and guest password
//   say TEXT              one `cloudroom TEXT` Management command; prints the reply
//   pump                  run the queued link job and the dispatch work that follows it
//   restart               apply a requested restart: boot again from the saved record
//   saved                 key=value fields of the saved aliases, including the token
//   room?                 whether the local Room role runs this boot, and its saved password
//   serial                everything the node printed on its console so far, one line
//   node?                 the management public key (the id `cloudroom nodes` shows is its first 8 hex)
//   state                 restart requested, link failures, record version
//   clock SECONDS         set the trusted UTC time (default: real time)
//   quit
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
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cassert>
#include <ctime>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

unsigned long millis() { return 0; }
void delay(unsigned long) {}
using namespace onchip;
namespace onchip { void cloudRoomBootResetForTest(); }
namespace {
struct Device {
  std::map<std::string, std::vector<uint8_t>> journals;
  decltype(filesystem_test::files) files;
  decltype(identity_test::durable) identities;
  bool hasRoom = false;
  std::string roomName, roomPassword;
  uint8_t roomSeed = 0, managementSeed = 0;
  unsigned restartsRequested = 0;
  bool booted = false;
};
std::map<std::string, Device> devices;
std::string currentName;
Device *dev = nullptr;
std::map<std::string, int> hostPorts;
bool dispatchContext;
struct Dispatch {
  Dispatch() { dispatchContext = true; }
  ~Dispatch() { dispatchContext = false; }
};
bool clockFixed;
uint32_t fixedClock;
bool roomRunning;

struct FixedRng : mesh::RNG {
  uint8_t value;
  explicit FixedRng(uint8_t value) : value(value) {}
  void random(uint8_t *data, size_t size) override { memset(data, value, size); }
};
std::vector<uint8_t> privateKey(uint8_t seed) {
  FixedRng rng(seed);
  mesh::LocalIdentity identity(&rng);
  std::vector<uint8_t> key(PRV_KEY_SIZE);
  identity.writeTo(key.data(), key.size());
  return key;
}
std::string hex(const uint8_t *bytes, size_t size) {
  const char alphabet[] = "0123456789abcdef";
  std::string text;
  for (size_t i = 0; i < size; ++i) { text += alphabet[bytes[i] >> 4]; text += alphabet[bytes[i] & 15]; }
  return text;
}
std::string publicHex(uint8_t seed) {
  FixedRng rng(seed);
  mesh::LocalIdentity identity(&rng);
  return hex(identity.pub_key, 32);
}
}
namespace onchip {
unsigned cloudRoomAliases() { return 0; }
bool mastRecord(const char *key, void *data, size_t size, bool write, bool &present) {
  assert(!strcmp(key, "cloud-room") && size == 72);
  auto &journals = dev->journals;
  if (write) {
    auto *bytes = static_cast<const uint8_t *>(data);
    journals[key] = {bytes, bytes + size};
  }
  present = journals.count(key);
  if (!write && present) memcpy(data, journals.at(key).data(), size);
  return true;
}
bool cloudRoomScheduleRestart(uint32_t) {
  assert(!dispatchContext && "cloudRoomScheduleRestart waits for the dispatch task");
  ++dev->restartsRequested;
  return true;
}
#ifdef E2E_PROVIDER
// A legacy compile-time provider: one pinned-CA alias, as a private build would link it.
const CloudRoomConfiguration *cloudRoomCompiledConfiguration() {
  static CloudRoomConfiguration configuration;
  static std::string id, name, token, host;
  static bool filled;
  if (!filled) {
    filled = true;
    auto env = [](const char *key, const std::string &fallback) { const char *v = getenv(key); return std::string(v ? v : fallback); };
    id = env("E2E_PROVIDER_ALIAS", "room-1");
    name = env("E2E_PROVIDER_NAME", "Legacy Room");
    token = env("E2E_PROVIDER_TOKEN", "fixture-token");
    host = env("E2E_PROVIDER_HOST", "legacy.example.test");
    configuration.count = 1;
    configuration.peers[0] = {host.c_str(), "127.0.0.1", "-----BEGIN CERTIFICATE-----\nfixture\n-----END CERTIFICATE-----\n",
                              token.c_str(), id.c_str(), false};
    configuration.aliases[0] = {id.c_str(), name.c_str(), {}};
    const std::string pub = env("E2E_PROVIDER_PUB", std::string(64, '1'));
    for (unsigned i = 0; i < 32; ++i) configuration.aliases[0].publicKey[i] = uint8_t(std::stoi(pub.substr(i * 2, 2), nullptr, 16));
  }
  return &configuration;
}
#endif
}
namespace {
// ---- loopback transport -----------------------------------------------------------------
uint32_t monotonicMs() {
  timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return uint32_t(t.tv_sec * 1000u + t.tv_nsec / 1000000u);
}
class SocketTransport final : public CloudTransport {
  int fd_ = -1;
public:
  ~SocketTransport() override { close(); }
  bool open(const CloudTlsTarget &target, CloudTlsFailure &failure) override {
    const auto port = hostPorts.find(target.host);
    if (port == hostPorts.end()) { failure = CloudTlsFailure::Network; return false; }
    fd_ = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port->second);
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (connect(fd_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) { failure = CloudTlsFailure::Network; return false; }
    fcntl(fd_, F_SETFL, fcntl(fd_, F_GETFL) | O_NONBLOCK);
    return true;
  }
  int write(const uint8_t *data, size_t size) override {
    const auto n = send(fd_, data, size, MSG_NOSIGNAL);
    if (n >= 0) return int(n);
    return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
  }
  int read(uint8_t *data, size_t size) override {
    const auto n = recv(fd_, data, size, 0);
    if (n > 0) return int(n);
    if (n == 0) return -1;
    return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
  }
  void close() override { if (fd_ >= 0) ::close(fd_); fd_ = -1; }
  uint32_t now() const override { return monotonicMs(); }
  void idle() override { pollfd p{fd_, POLLIN, 0}; poll(&p, 1, 5); }
};
CloudTransport *newTransport() { return new SocketTransport; }
bool resolve(const char *host, char address[16]) {
  if (!hostPorts.count(host)) return false;
  strcpy(address, "127.0.0.1");
  return true;
}
bool trustedNow(uint32_t &value) { value = clockFixed ? fixedClock : uint32_t(time(nullptr)); return true; }
size_t heapFree() { return 200 * 1024; }
size_t heapBlock() { return 100 * 1024; }
uint32_t agentMillis() { return monotonicMs(); }
CloudAliasState aliasState(unsigned) { return CloudAliasState::Live; }
bool wake() { return true; }
bool roomName(char name[32]) {
  if (!roomRunning) return false;
  snprintf(name, 32, "%s", dev->roomName.c_str());
  return true;
}
bool roomPassword(char out[16]) {
  if (!roomRunning) return false;
  snprintf(out, 16, "%s", dev->roomPassword.c_str());
  return true;
}
bool roomSetPassword(const char *password) {
  if (!roomRunning) return false;
  dev->roomPassword = password;
  return true;
}
void setPorts() {
  CloudRoomLinkPorts ports;
  ports.transport = newTransport; ports.resolve = resolve; ports.now = trustedNow;
  ports.freeInternal = heapFree; ports.largestBlock = heapBlock; ports.millis = agentMillis;
  ports.aliasState = aliasState; ports.wake = wake; ports.roomName = roomName;
  ports.roomGuestPassword = roomPassword; ports.roomSetGuestPassword = roomSetPassword;
  cloudRoomLinkSetPorts(ports);
}
// ---- devices ----------------------------------------------------------------------------
void saveCurrent() {
  if (!dev) return;
  dev->files = filesystem_test::files;
  dev->identities = identity_test::durable;
}
void boot() {
  cloudRoomBootResetForTest();
  cloudRoomLinkResetForTest();
  setPorts();
  cloudRoomConfiguration();
  roomRunning = dev->hasRoom && !cloudRoomOwnsLocalRoom();
  dev->booted = true;
}
void useDevice(const std::string &name) {
  saveCurrent();
  const bool fresh = !devices.count(name);
  dev = &devices[name];
  currentName = name;
  if (fresh) {
    // Distinct, public fixture seeds per device (first public-key byte is never 00 or FF).
    const uint8_t base = uint8_t(20 + 10 * (devices.size() - 1));
    const char *roles[] = {"management", "room", "repeater", "companion", "observer"};
    for (unsigned i = 0; i < 5; ++i) dev->identities[{"mc-onchip", roles[i]}] = privateKey(uint8_t(base + i));
    dev->managementSeed = base;
    dev->roomSeed = uint8_t(base + 1);
  }
  filesystem_test::files = dev->files;
  identity_test::durable = dev->identities;
  boot();
}
std::string say(const std::string &line) {
  char reply[160]{};
  const Dispatch dispatch;
  if (!cloudRoomLinkCommand(line.c_str(), reply, 146)) return "NOT-A-LIFECYCLE-VERB";
  return reply;
}
void pump() {
  cloudRoomLinkRun();
  { const Dispatch dispatch; cloudRoomLinkLoop(); }
  cloudRoomLinkRun();
}
std::string saved() {
  CloudRoomSettingsV2 s{};
  const auto load = cloudRoomLoadSettings(s);
  if (load != CloudRoomLoad::Ready) return "invalid";
  char hash[100]{};
  cloudRoomPreferencesCommand("config hash", hash, sizeof(hash));
  if (!strcmp(hash, "none")) return "none";
  std::ostringstream out;
  // The loader up-converts, so the file on flash says which record version a 0.1.19 node would read.
  const auto &files = filesystem_test::files;
  std::string version = "?";
  for (const char *name : {"/cloudroom-0.bin", "/cloudroom-1.bin"}) {
    const auto file = files.find(name);
    if (file == files.end() || file->second.size() < 32 || hex(file->second.data() + file->second.size() - 32, 32) != hash) continue;
    version = file->second.size() == sizeof(CloudRoomSettings) ? "v1" : file->second.size() == sizeof(CloudRoomSettingsV2) ? "v2" : "?";
  }
  out << "enabled=" << int(s.enabled) << " count=" << int(s.count) << " magic=" << version;
  for (unsigned i = 0; i < s.count; ++i) {
    const auto &a = s.aliases[i];
    out << " | id=" << a.base.id << " host=" << a.base.host << " name=" << a.base.name << " role=" << int(a.role)
        << " ca=" << int(a.caMode) << " flags=" << int(a.flags) << " pub=" << hex(a.base.publicKey, 32)
        << " token=" << a.base.token;
  }
  return out.str();
}
}
int main() {
  std::string serial;
  serial_test::capture = &serial;
  std::string line;
  while (std::getline(std::cin, line)) {
    std::istringstream in(line);
    std::string verb;
    in >> verb;
    std::string rest;
    std::getline(in, rest);
    if (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
    std::string reply = "OK";
    if (verb == "quit") break;
    else if (verb == "host") { std::istringstream a(rest); std::string h; int p; a >> h >> p; hostPorts[h] = p; }
    else if (verb == "device") useDevice(rest);
    else if (verb == "room") {
      std::istringstream a(rest);
      std::string name, password;
      std::getline(a, name, ' ');
      std::getline(a, password);
      dev->hasRoom = true; dev->roomName = name; dev->roomPassword = password;
      boot();
    }
    else if (verb == "say") reply = say(rest);
    else if (verb == "pump") pump();
    else if (verb == "restart") { dev->restartsRequested = 0; boot(); }
    else if (verb == "saved") reply = saved();
    else if (verb == "room?") reply = std::string(roomRunning ? "local" : dev->hasRoom ? "cloud" : "none") + " pw=" + dev->roomPassword + " pub=" + publicHex(dev->roomSeed);
    else if (verb == "serial") {
      reply = serial;
      for (auto &c : reply) if (c == '\n' || c == '\r') c = '|';
      if (reply.empty()) reply = "-";
    }
    else if (verb == "node?") reply = publicHex(dev->managementSeed);
    else if (verb == "state") reply = "restarts=" + std::to_string(dev->restartsRequested) + " failures=" + std::to_string(cloudRoomLinkFailures());
    else if (verb == "clock") { clockFixed = !rest.empty(); fixedClock = uint32_t(atol(rest.c_str())); }
    else reply = "ERR unknown command";
    std::cout << reply << std::endl;
  }
  return 0;
}
