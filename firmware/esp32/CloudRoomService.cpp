// SPDX-License-Identifier: Apache-2.0
#include "CloudRoomService.h"
#include <stdio.h>
#include <string.h>
#if defined(MESHCORE_CLOUD_ROOM) && MESHCORE_CLOUD_ROOM
#include "LocalRadio.h"
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
  std::atomic<uint32_t> stackMinimumBytes{0};
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
  void poll(unsigned work) override {
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
            lost(alias); continue;
          }
          c.connected = true; c.backoffMs = 2000;
          connectedAliases.fetch_or(1u << alias, std::memory_order_relaxed);
          driver->opened(alias, bridge.generation(alias));
        }
        const int size = c.socket->receive(buffers->inbound, sizeof(buffers->inbound));
        if (size < 0 || (size && !driver->frame(alias, buffers->inbound, size_t(size)))) {
          lost(alias); continue;
        }
        const uint32_t bit=1u<<alias;
        if ((advertisements.load(std::memory_order_relaxed)&bit) && driver->advertise(alias))
          advertisements.fetch_and(~bit,std::memory_order_relaxed);
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
  bool begin(WifiKissMultiplexer &mux,NativeNetworkHost &worker) {
    if (active.load()) return true;
    if (!worker.ensureNativeHttps()) return false;
    driver = createCloudRoomDriver(bridge);
    if (!driver) return false;
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
    const auto registration = worker.attachNetworkService(
        *this, {"cloud-room", CLOUD_ROOM_SOCKETS, 2 * cloudroom::QueueDepth + aliases});
    if (registration != NativeServiceRegistration::Attached) {
      Serial.printf("Cloud room registration failed: %s\n", nativeServiceRegistrationText(registration));
      active.store(false,std::memory_order_release);
      discard(); return false;
    }
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
} // namespace
bool beginCloudRoom(WifiKissMultiplexer &mux,NativeNetworkHost &worker) { return service.begin(mux,worker); }
void loopCloudRoom() { service.dispatch(); }
unsigned cloudRoomAliases() { return service.aliases; }
const uint8_t *cloudRoomPublicKey(unsigned alias) { return alias < service.aliases ? service.keys[alias] : nullptr; }
bool requestCloudRoomAdvertisement(unsigned alias) {
  if (alias>=service.aliases) return false;
  service.advertisements.fetch_or(1u<<alias,std::memory_order_relaxed);return true;
}
void cloudRoomCommand(const char *command, char *reply, size_t capacity) {
  if (!*command || !strcmp(command, "status")) {
    snprintf(reply, capacity, "Cloud room aliases=%u sockets=%u wss-mask=%u advert-pending=%u",
             service.aliases, CLOUD_ROOM_SOCKETS,
             unsigned(service.connectedAliases.load(std::memory_order_relaxed)),
             unsigned(service.advertisements.load(std::memory_order_relaxed)));
    return;
  }
  if (!strcmp(command, "error")) {
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
  if (!strncmp(command, "advertise ", 10)) {
    for (unsigned alias = 0; alias < service.aliases; ++alias)
      if (!strcmp(command + 10, service.driver->peer(alias).alias)) {
        const bool queued = requestCloudRoomAdvertisement(alias);
        snprintf(reply, capacity, queued ? "Queued room advert %s; RF delivery unconfirmed" :
                 "Error: room advert %s unavailable", command + 10);
        return;
      }
    snprintf(reply, capacity, "Error: cloud room alias is not active; inspect private configuration and cloudroom status");
    return;
  }
  snprintf(reply, capacity, "Error: cloudroom status|error|advertise ALIAS");
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
