// SPDX-License-Identifier: Apache-2.0
#include "CloudRoomService.h"
#include "CloudRoomLifecycle.h"
#include "CloudRoomPreferences.h"
#include "CloudRoomRegistration.h"
#include <stdio.h>
#include <string.h>
#if defined(MESHCORE_CLOUD_ROOM) && MESHCORE_CLOUD_ROOM
#include "LocalRadio.h"
#if ONCHIP_CLOUD_ROOM_LINK
#include "BotHttps.h"
#include "Clock.h"
#include "CloudRoomLink.h"
#include "Runtime.h"
#include <WiFi.h>
#include <esp_heap_caps.h>
#endif
#include "RoleStorage.h"
#include "Capacity.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <math.h>
#include <new>

namespace onchip {
const CloudRoomConfiguration *__attribute__((weak)) cloudRoomConfiguration() { return nullptr; }
namespace {
class OpaqueDriver final : public CloudRoomDriver {
  const CloudRoomConfiguration &config_;
  cloudroom::OpaqueBuffers *buffers_;
  cloudroom::OpaqueFrontend frontend_;
public:
  OpaqueDriver(cloudroom::RadioBridge &radio, const CloudRoomConfiguration &config, cloudroom::OpaqueBuffers *buffers)
    : config_(config), buffers_(buffers), frontend_(radio,config.aliases,config.count,*buffers) {}
  ~OpaqueDriver() override { releaseRoleStorage(buffers_); }
  unsigned aliases() const override { return config_.count; }
  const CloudRoomPeer &peer(unsigned a) const override { return config_.peers[a]; }
  const uint8_t *publicKey(unsigned a) const override { return config_.aliases[a].publicKey; }
  void opened(unsigned a,uint32_t g) override { frontend_.opened(a,g); }
  void disconnected(unsigned a) override { frontend_.disconnected(a); }
  void received(const cloudroom::Reception &r) override { frontend_.received(r); }
  bool frame(unsigned a,const char *p,size_t n) override { return frontend_.frame(a,p,n); }
  void receipt(const cloudroom::Receipt &r) override { frontend_.receipt(r); }
  bool advertise(unsigned a) override { return frontend_.advertise(a); }
  size_t operation(unsigned a,char *p,size_t n) override { return frontend_.operation(a,p,n); }
};
}
CloudRoomDriver *__attribute__((weak)) createCloudRoomDriver(cloudroom::RadioBridge &radio) {
  const auto *config=cloudRoomConfiguration();
  if (!config || !config->count || config->count>cloudroom::AliasLimit || config->count>CLOUD_ROOM_SOCKETS) return nullptr;
  for (unsigned a=0;a<config->count;++a) {
    const auto &alias=config->aliases[a];
    if (!alias.id || !alias.name || !config->peers[a].alias || strcmp(alias.id,config->peers[a].alias)) return nullptr;
  }
  auto *buffers=allocateRoleStorage<cloudroom::OpaqueBuffers>("cloud-room operations");
  if (!buffers) return nullptr;
  auto *driver=new(std::nothrow) OpaqueDriver(radio,*config,buffers);
  if (!driver) releaseRoleStorage(buffers);
  return driver;
}
namespace {
constexpr unsigned MaxJobs = cloudroom::QueueDepth;
constexpr uint32_t TxExpiryMs = 30000;
struct Buffers {
  cloudroom::RadioBuffers radio;
  char inbound[cloudroom::FrameLimit], outbound[cloudroom::FrameLimit];
};
// Only payloads/dispatch-owned radio live in PSRAM. SPSC atomics and the task
// stack remain in internal memory; no shared LocalRadio calls cross tasks.
constexpr uint32_t aliasBit(unsigned alias) { return 1u << alias; }
struct Service final : NativeNetworkService {
  cloudroom::RadioBridge bridge;
  Buffers *buffers = nullptr;
  LocalRadio *radio = nullptr;
  CloudRoomDriver *driver = nullptr;
  std::atomic<bool> active{false};
  StaticSemaphore_t admissionControl;
  SemaphoreHandle_t admission = nullptr;
  unsigned aliases = 0;
  std::atomic<uint32_t> advertisements{0};
  std::atomic<uint32_t> connectedAliases{0};
  std::atomic<uint32_t> revokedAliases{0};      // upgrade refused with 401 or 403: stop retrying
  std::atomic<uint32_t> flaggedAdverts{0};      // aliases whose saved advert-once flag is being served
  std::atomic<uint32_t> stackMinimumBytes{0};
  CloudRoomRegistration registration;
  char lastError[96]{};
  uint8_t keys[cloudroom::AliasLimit][32]{};
  struct Job { uint32_t native = 0; cloudroom::Receipt receipt; } jobs[MaxJobs];
  struct Connection {
    CloudRoomSocket *socket = nullptr;
    bool connected = false;
    uint32_t retryAt = 0, backoffMs = 2000;
  } connections[cloudroom::AliasLimit];
  void lost(unsigned alias) {
    auto &c = connections[alias];
    // Linearize invalidation with physical admission. The dispatch task takes
    // this tiny mutex without waiting; LocalRadio itself never crosses tasks.
    xSemaphoreTake(admission, portMAX_DELAY);
    bridge.disconnect(alias);
    connectedAliases.fetch_and(~(1u << alias), std::memory_order_relaxed);
    xSemaphoreGive(admission);
    c.socket->close(); c.connected = false;
    driver->disconnected(alias);
    c.retryAt = millis() + c.backoffMs;
    c.backoffMs = c.backoffMs < 16000 ? c.backoffMs * 2 : 30000;
  }
#if ONCHIP_CLOUD_ROOM_LINK
  // A link job needs the one TLS context: close the live sockets first and let their
  // normal backoff reopen them afterwards.
  void pauseForLink() {
    for (unsigned alias = 0; alias < aliases; ++alias) {
      auto &c = connections[alias];
      if (!c.connected) continue;
      xSemaphoreTake(admission, portMAX_DELAY);
      bridge.disconnect(alias);
      connectedAliases.fetch_and(~(1u << alias), std::memory_order_relaxed);
      xSemaphoreGive(admission);
      c.socket->close(); c.connected = false;
      driver->disconnected(alias);
      c.retryAt = millis() + 2000; c.backoffMs = 2000;
    }
  }
#endif
  void poll(unsigned work) override {
#if ONCHIP_CLOUD_ROOM_LINK
      if (cloudRoomLinkWork()) {
        if (cloudRoomLinkPending() && active.load(std::memory_order_acquire)) pauseForLink();
        cloudRoomLinkRun();
        stackMinimumBytes.store(uxTaskGetStackHighWaterMark(nullptr),std::memory_order_relaxed);
        return;
      }
#endif
      if (!active.load(std::memory_order_acquire)) return;
      cloudroom::Reception packet;
      // Bounded work per iteration lets every alias drain its incoming socket.
      for (unsigned n = 0; n < cloudroom::QueueDepth && work && bridge.rx.pop(packet); ++n, --work)
        driver->received(packet);
      cloudroom::Receipt result;
      for (unsigned n = 0; n < cloudroom::QueueDepth && work && bridge.results.pop(result); ++n, --work)
        if (result.generation == bridge.generation(result.alias)) driver->receipt(result);
      for (unsigned alias = 0; alias < aliases && work; ++alias, --work) {
        auto &c = connections[alias];
        if (revokedAliases.load(std::memory_order_relaxed) & (1u << alias)) continue;
        if (!c.connected) {
          if (int32_t(millis() - c.retryAt) < 0) continue;
          char error[96]{};
          const bool opened=c.socket->open(driver->peer(alias),error,sizeof(error));
          stackMinimumBytes.store(uxTaskGetStackHighWaterMark(nullptr),std::memory_order_relaxed);
          if (!opened) {
            xSemaphoreTake(admission, portMAX_DELAY);
            snprintf(lastError,sizeof(lastError),"%s",error);
            xSemaphoreGive(admission);
            Serial.printf("Cloud room %s WSS unavailable: %s\n",driver->peer(alias).alias,error);
            const unsigned refused = c.socket->upgradeStatus();
            if (refused == 401 || refused == 403) {
              revokedAliases.fetch_or(1u << alias, std::memory_order_relaxed);
              Serial.printf("Cloud room %s refused by the server (HTTP %u); unlink it\n",driver->peer(alias).alias,refused);
            }
            lost(alias); continue;
          }
          c.connected = true; c.backoffMs = 2000;
          connectedAliases.fetch_or(1u << alias, std::memory_order_relaxed);
          driver->opened(alias, bridge.generation(alias));
#if ONCHIP_CLOUD_ROOM_LINK
          // A created or converted room announces itself once, on its first connection.
          CloudRoomAliasInfo info;
          if (cloudRoomAliasInfo(alias, info) && (info.flags & CloudFlagAdvertOnce) &&
              !(flaggedAdverts.load(std::memory_order_relaxed) & aliasBit(alias))) {
            flaggedAdverts.fetch_or(aliasBit(alias), std::memory_order_relaxed);
            advertisements.fetch_or(aliasBit(alias), std::memory_order_relaxed);
          }
#endif
        }
        const int size = c.socket->receive(buffers->inbound, sizeof(buffers->inbound));
        if (size < 0 || (size && !driver->frame(alias, buffers->inbound, size_t(size)))) {
          lost(alias); continue;
        }
        const uint32_t bit=1u<<alias;
        if ((advertisements.load(std::memory_order_relaxed)&bit) && driver->advertise(alias)) {
          advertisements.fetch_and(~bit,std::memory_order_relaxed);
#if ONCHIP_CLOUD_ROOM_LINK
          if (flaggedAdverts.load(std::memory_order_relaxed) & bit) cloudRoomLinkAdvertised(alias);
#endif
        }
        const size_t out = driver->operation(alias, buffers->outbound, sizeof(buffers->outbound));
        if (out > sizeof(buffers->outbound) || (out && !c.socket->send(buffers->outbound, out)))
          lost(alias);
      }
  }
  void close() override {
    active.store(false,std::memory_order_release);
    for (unsigned alias=0;alias<aliases;++alias) lost(alias);
  }
  void discard() { // Only before starting the network task.
    if (radio) radio->detach();
    for (auto &c : connections) { delete c.socket; c.socket = nullptr; }
    delete driver; driver = nullptr;
    releaseRoleStorage(radio); releaseRoleStorage(buffers); aliases = 0;
  }
#if ONCHIP_CLOUD_ROOM_LINK
  // Link jobs run on the network task through this service. With no alias linked it stays
  // unregistered, so the registry's slots and sockets are untouched until the first job.
  bool ensureRegistered() {
    NativeServiceRegistration reason = NativeServiceRegistration::Attached;
    if (registration.ensure(*this, {"cloud-room", CLOUD_ROOM_SOCKETS, 1}, &reason)) return true;
    Serial.printf("Cloud room link service registration failed: %s\n", nativeServiceRegistrationText(reason));
    return false;
  }
#endif
  bool begin(WifiKissMultiplexer &mux,NativeNetworkHost &worker) {
    if (active.load() || registration.registered()) return true;
    registration.bind(worker);
    if (!worker.ensureNativeHttps()) return false;
    driver = createCloudRoomDriver(bridge);
    if (!driver) {
#if ONCHIP_CLOUD_ROOM_LINK
      // Nothing linked (or more aliases than this build has sockets): the lifecycle verbs still work.
      if (cloudRoomConfiguration()) Serial.println("Cloud room aliases do not fit this build's sockets; only link commands run");
      return true;
#else
      return false;
#endif
    }
    aliases = driver->aliases();
    if (!aliases || aliases > cloudroom::AliasLimit || aliases > CLOUD_ROOM_SOCKETS) { discard(); return false; }
    for (unsigned alias = 0; alias < aliases; ++alias) {
      const auto *key = driver->publicKey(alias);
      if (!key) { discard(); return false; }
      memcpy(keys[alias], key, 32);
      connections[alias].socket = createCloudRoomSocket();
      if (!connections[alias].socket) { discard(); return false; }
    }
    buffers = allocateRoleStorage<Buffers>("cloud-room queues");
    radio = allocateRoleStorage<LocalRadio>("cloud-room radio");
    if (!buffers || !radio || !radio->attach(mux)) { discard(); return false; }
    bridge.bind(buffers->radio);
    admission = xSemaphoreCreateMutexStatic(&admissionControl);
    if (!admission) { discard(); return false; }
    active.store(true,std::memory_order_release);
    const auto attached = worker.attachNetworkService(
        *this, {"cloud-room", CLOUD_ROOM_SOCKETS, 2 * cloudroom::QueueDepth + aliases});
    if (attached != NativeServiceRegistration::Attached) {
      Serial.printf("Cloud room registration failed: %s\n", nativeServiceRegistrationText(attached));
      active.store(false,std::memory_order_release);
      discard(); return false;
    }
    registration.adopted();
    return true;
  }
  void dispatch() {
    if (!active.load(std::memory_order_acquire)) return;
    uint8_t packet[cloudroom::RadioLimit];
    for (unsigned n = 0; n < cloudroom::QueueDepth; ++n) {
      const int size = radio->recvRaw(packet, sizeof(packet));
      if (!size) break;
      const float rssi = radio->getLastRSSI(), snr = radio->getLastSNR();
      bridge.received(packet, size_t(size), isfinite(rssi) ? int16_t(rssi) : 0,
                      isfinite(snr) ? int16_t(snr * 4) : 0, radio->lastReceiveWasLocal());
    }
    mesh::QueuedTransmitResult result;
    while (!bridge.results.full() && radio->pollQueuedResult(result)) {
      if (result.state == queued_tx::ACCEPTED) continue;
      for (auto &job : jobs) if (job.native == result.job) {
        job.receipt.outcome = result.state == queued_tx::SUCCEEDED ? cloudroom::Receipt::Sent
          : result.state == queued_tx::UNKNOWN || result.reason == queued_tx::DISCONNECTED
            ? cloudroom::Receipt::Unknown : cloudroom::Receipt::Failed;
        bridge.results.push(job.receipt); job.native = 0; break;
      }
    }
    // One physical source and its existing scheduler serve all aliases. Leave
    // backpressure queued, rather than dropping an operation after permission.
    for (unsigned n = 0; n < cloudroom::QueueDepth && !bridge.results.full(); ++n) {
      Job *slot = nullptr;
      for (auto &job : jobs) if (!job.native) { slot = &job; break; }
      if (!slot) break;
      if (xSemaphoreTake(admission, 0) != pdTRUE) break;
      cloudroom::Transmission tx;
      if (!bridge.tx.pop(tx)) { xSemaphoreGive(admission); break; }
      slot->receipt = {}; slot->receipt.cookie = tx.cookie;
      slot->receipt.alias = tx.alias; slot->receipt.generation = tx.generation;
      const bool admitted = bridge.current(tx) && radio->queueTransmit(tx.bytes, tx.size, tx.priority,
            tx.delayMs, TxExpiryMs, slot->native);
      xSemaphoreGive(admission);
      if (!admitted) {
        slot->receipt.outcome = cloudroom::Receipt::Failed;
        bridge.results.push(slot->receipt);
      }
    }
  }
};
Service service;
#if ONCHIP_CLOUD_ROOM_LINK
// Production side of the link client's seams.
class TlsStream final : public CloudTransport {
  BotHttpsTransport *tls_ = createPersistentBotTlsTransport();
public:
  ~TlsStream() override { delete tls_; }
  bool open(const CloudTlsTarget &target, CloudTlsFailure &failure) override {
    if (!tls_) { failure = CloudTlsFailure::Memory; return false; }
    char error[96]{};
    tls_->resetDiagnostics();
    const BotHttpsConfig config(target.address, target.host, target.ca ? target.ca : "", "", 443, 0, target.systemRoots);
    if (tls_->open(config, error, sizeof(error))) return true;
    const auto point = tls_->diagnostics().point;
    failure = point == BotHttpsFailurePoint::Clock ? CloudTlsFailure::Clock
        : tls_->failure() == BotHttpsTransport::Failure::Heap ? CloudTlsFailure::Memory
        : (point == BotHttpsFailurePoint::Connect || point == BotHttpsFailurePoint::CaParse ||
           point == BotHttpsFailurePoint::PeerCertificate || point == BotHttpsFailurePoint::CertificateTime)
            ? CloudTlsFailure::Verify : CloudTlsFailure::Network;
    return false;
  }
  int write(const uint8_t *data, size_t size) override { return tls_->write(data, size); }
  int read(uint8_t *data, size_t size) override { return tls_->read(data, size); }
  void close() override { tls_->close(); }
  uint32_t now() const override { return tls_->now(); }
  void idle() override { tls_->idle(); }
};
CloudTransport *linkTransport() { return new(std::nothrow) TlsStream; }
bool linkResolve(const char *host, char address[16]) {
  IPAddress ip;
  if (WiFi.hostByName(host, ip) != 1) return false;
  snprintf(address, 16, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
  return true;
}
bool linkNow(uint32_t &utc) {
  uint32_t earliest, latest;
  if (!trustedNetworkTime(earliest, latest)) return false;
  utc = earliest + (latest - earliest) / 2;
  return true;
}
size_t linkFree() { return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }
size_t linkBlock() { return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }
uint32_t linkMillis() { return millis(); }
CloudAliasState linkAliasState(unsigned alias) {
  if (alias >= service.aliases) return CloudAliasState::Off;
  if (service.revokedAliases.load(std::memory_order_relaxed) & aliasBit(alias)) return CloudAliasState::Revoked;
  return service.connectedAliases.load(std::memory_order_relaxed) & aliasBit(alias) ? CloudAliasState::Live : CloudAliasState::Down;
}
bool linkWake() { return service.ensureRegistered(); }
bool linkRoomName(char name[32]) { return nativeRoleName(Role::Room, name); }
void installLinkPorts() {
  CloudRoomLinkPorts ports;
  ports.transport = linkTransport; ports.resolve = linkResolve; ports.now = linkNow;
  ports.freeInternal = linkFree; ports.largestBlock = linkBlock; ports.millis = linkMillis;
  ports.aliasState = linkAliasState; ports.wake = linkWake; ports.roomName = linkRoomName;
  ports.roomGuestPassword = roomGuestPassword; ports.roomSetGuestPassword = roomSetGuestPassword;
  cloudRoomLinkSetPorts(ports);
}
#endif
} // namespace
bool beginCloudRoom(WifiKissMultiplexer &mux,NativeNetworkHost &worker) {
#if ONCHIP_CLOUD_ROOM_LINK
  installLinkPorts();
#endif
  return service.begin(mux,worker);
}
void loopCloudRoom() {
  service.dispatch();
#if ONCHIP_CLOUD_ROOM_LINK
  cloudRoomLinkLoop();
#endif
}
unsigned cloudRoomAliases() { return service.aliases; }
const uint8_t *cloudRoomPublicKey(unsigned alias) { return alias < service.aliases ? service.keys[alias] : nullptr; }
bool requestCloudRoomAdvertisement(unsigned alias) {
  if (alias>=service.aliases) return false;
  service.advertisements.fetch_or(1u<<alias,std::memory_order_relaxed);return true;
}
void cloudRoomCommand(const char *command, char *reply, size_t capacity) {
#if ONCHIP_CLOUD_ROOM_LINK
  if (cloudRoomLinkCommand(command, reply, capacity)) return;
#endif
  if (cloudRoomPreferencesCommand(command, reply, capacity)) return;
  if (!*command || !strcmp(command, "status")) {
    int n = snprintf(reply, capacity, "Cloud room aliases=%u sockets=%u wss-mask=%u advert-pending=%u",
             service.aliases, CLOUD_ROOM_SOCKETS,
             unsigned(service.connectedAliases.load(std::memory_order_relaxed)),
             unsigned(service.advertisements.load(std::memory_order_relaxed)));
#if ONCHIP_CLOUD_ROOM_LINK
    if (n > 0 && size_t(n) < capacity) cloudRoomLinkStatus(reply + n, capacity - size_t(n));
#endif
    (void)n;
    return;
  }
  if (!strcmp(command, "error")) {
#if ONCHIP_CLOUD_ROOM_LINK
    if (service.revokedAliases.load(std::memory_order_relaxed)) {
      snprintf(reply,capacity,"Server revoked this node. Run \"cloudroom unlink ALIAS local\".");return;
    }
#endif
    if (!service.active.load()) {
      snprintf(reply,capacity,"Cloud room is not configured");return;
    }
    if (xSemaphoreTake(service.admission,0)!=pdTRUE) {
      snprintf(reply,capacity,"Error: cloud room status busy; try cloudroom error again");return;
    }
    snprintf(reply,capacity,"Cloud room stack-min=%u last-open-error: %s",
             unsigned(service.stackMinimumBytes.load(std::memory_order_relaxed)),
             *service.lastError?service.lastError:"none");
    xSemaphoreGive(service.admission);return;
  }
  if (!strncmp(command, "advertise", 9) && (command[9] == ' ' || !command[9])) {
    const char *name = command[9] ? command + 10 : "";
    // An alias id, its 1-based number, or nothing when exactly one alias is running.
    for (unsigned alias = 0; alias < service.aliases; ++alias) {
      const char *id = service.driver->peer(alias).alias;
      const bool numbered = name[0] == char('1' + alias) && !name[1];
      if (!strcmp(name, id) || numbered || (!*name && service.aliases == 1)) {
        const bool queued = requestCloudRoomAdvertisement(alias);
        snprintf(reply, capacity, queued ? "Queued room advert %s" :
                 "Error: room advert %s unavailable", id);
        return;
      }
    }
    snprintf(reply, capacity, "Error: cloud room alias is not active; inspect cloudroom config status and cloudroom status");
    return;
  }
  snprintf(reply, capacity, "Error: cloudroom status|error|advertise ALIAS|config|enable on|off");
}
} // namespace onchip
#else
namespace onchip {
const CloudRoomConfiguration *cloudRoomConfiguration() { return nullptr; }
CloudRoomDriver *createCloudRoomDriver(cloudroom::RadioBridge &) { return nullptr; }
bool beginCloudRoom(WifiKissMultiplexer &,NativeNetworkHost &) { return false; }
void loopCloudRoom() {}
unsigned cloudRoomAliases() { return 0; }
const uint8_t *cloudRoomPublicKey(unsigned) { return nullptr; }
bool requestCloudRoomAdvertisement(unsigned) { return false; }
void cloudRoomCommand(const char *command, char *reply, size_t capacity) {
  snprintf(reply, capacity, !*command || !strcmp(command, "status") ?
           "Cloud room aliases=0 sockets=0 wss-mask=0 advert-pending=0" :
           "Error: cloud room is disabled in this image; use the privately configured cloudroom profile");
}
}
#endif
