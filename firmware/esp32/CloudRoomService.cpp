// SPDX-License-Identifier: Apache-2.0
#include "CloudRoomService.h"
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
CloudRoomDriver *__attribute__((weak)) createCloudRoomDriver(cloudroom::RadioBridge &) { return nullptr; }
namespace {
constexpr unsigned MaxJobs = cloudroom::QueueDepth;
constexpr uint32_t TxExpiryMs = 30000;
constexpr unsigned NetworkStackBytes = 6144;
struct Buffers {
  cloudroom::RadioBuffers radio;
  char inbound[cloudroom::FrameLimit], outbound[cloudroom::FrameLimit];
};
// Only payloads/dispatch-owned radio live in PSRAM. SPSC atomics and the task
// stack remain in internal memory; no shared LocalRadio calls cross tasks.
struct Service {
  cloudroom::RadioBridge bridge;
  Buffers *buffers = nullptr;
  LocalRadio *radio = nullptr;
  CloudRoomDriver *driver = nullptr;
  TaskHandle_t task = nullptr;
  StaticSemaphore_t admissionControl;
  SemaphoreHandle_t admission = nullptr;
  unsigned aliases = 0;
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
    xSemaphoreGive(admission);
    c.socket->close(); c.connected = false;
    driver->disconnected(alias);
    c.retryAt = millis() + c.backoffMs;
    c.backoffMs = c.backoffMs < 16000 ? c.backoffMs * 2 : 30000;
  }
  void network() {
    for (;;) {
      cloudroom::Reception packet;
      // Bounded work per iteration lets every alias drain its incoming socket.
      for (unsigned n = 0; n < cloudroom::QueueDepth && bridge.rx.pop(packet); ++n)
        driver->received(packet);
      cloudroom::Receipt result;
      for (unsigned n = 0; n < cloudroom::QueueDepth && bridge.results.pop(result); ++n)
        if (result.generation == bridge.generation(result.alias)) driver->receipt(result);
      for (unsigned alias = 0; alias < aliases; ++alias) {
        auto &c = connections[alias];
        if (!c.connected) {
          if (int32_t(millis() - c.retryAt) < 0) continue;
          char error[96]{};
          if (!c.socket->open(driver->peer(alias), error, sizeof(error))) {
            // Deliberately fixed text: TLS/library diagnostics may include peer
            // data, but no token or packet plaintext enters public diagnostics.
            Serial.println("Cloud-room connection unavailable");
            lost(alias); continue;
          }
          c.connected = true; c.backoffMs = 2000;
          driver->opened(alias, bridge.generation(alias));
        }
        const int size = c.socket->receive(buffers->inbound, sizeof(buffers->inbound));
        if (size < 0 || (size && !driver->frame(alias, buffers->inbound, size_t(size)))) {
          lost(alias); continue;
        }
        const size_t out = driver->operation(alias, buffers->outbound, sizeof(buffers->outbound));
        if (out > sizeof(buffers->outbound) || (out && !c.socket->send(buffers->outbound, out)))
          lost(alias);
      }
      vTaskDelay(pdMS_TO_TICKS(10)); // Local task idle; sends no keepalive/DO wake-up.
    }
  }
  static void run(void *self) { static_cast<Service *>(self)->network(); }
  void discard() { // Only before starting the network task.
    if (radio) radio->detach();
    for (auto &c : connections) { delete c.socket; c.socket = nullptr; }
    delete driver; driver = nullptr;
    releaseRoleStorage(radio); releaseRoleStorage(buffers); aliases = 0;
  }
  bool begin(WifiKissMultiplexer &mux) {
    if (task) return true;
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
    if (xTaskCreate(run, "cloud-room", NetworkStackBytes, this, 1, &task) != pdPASS) {
      discard(); return false;
    }
    return true;
  }
  void dispatch() {
    if (!task) return;
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
bool beginCloudRoom(WifiKissMultiplexer &mux) { return service.begin(mux); }
void loopCloudRoom() { service.dispatch(); }
unsigned cloudRoomAliases() { return service.aliases; }
const uint8_t *cloudRoomPublicKey(unsigned alias) { return alias < service.aliases ? service.keys[alias] : nullptr; }
} // namespace onchip
#else
namespace onchip {
CloudRoomDriver *createCloudRoomDriver(cloudroom::RadioBridge &) { return nullptr; }
bool beginCloudRoom(WifiKissMultiplexer &) { return false; }
void loopCloudRoom() {}
unsigned cloudRoomAliases() { return 0; }
const uint8_t *cloudRoomPublicKey(unsigned) { return nullptr; }
}
#endif
