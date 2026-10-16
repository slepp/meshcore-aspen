// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <helpers/ethernet/SerialEthernetInterface.h>
#include <rtos.h>
#include <atomic>
#include <cstring>

namespace nrfmast {
// The network task owns the W5100S driver, including blocking DHCP renewal.
// Only complete frames cross to the radio/application task.
class PineEthernetCompanion : public BaseSerialInterface {
  struct Frame { uint32_t generation; uint16_t length; uint8_t bytes[MAX_FRAME_SIZE]; };
  static constexpr size_t QueueSize = 4;
  SerialEthernetInterface& network;
  QueueHandle_t rx = nullptr, tx = nullptr;
  StaticQueue_t rxControl{}, txControl{};
  uint8_t rxStorage[QueueSize * sizeof(Frame)]{}, txStorage[QueueSize * sizeof(Frame)]{};
  std::atomic<bool> enabled{false}, connected{false};
  std::atomic<uint32_t> generation{0};
  uint32_t dispatchGeneration = 0;
  void (*resetSession)() = nullptr;
  void acceptGeneration(uint32_t current) {
    if (current != dispatchGeneration) {
      dispatchGeneration = current;
      if (resetSession) resetSession();
    }
  }
public:
  explicit PineEthernetCompanion(SerialEthernetInterface& driver) : network(driver) {}
  bool begin(void (*reset)()) {
    resetSession = reset;
    rx = xQueueCreateStatic(QueueSize, sizeof(Frame), rxStorage, &rxControl);
    tx = xQueueCreateStatic(QueueSize, sizeof(Frame), txStorage, &txControl);
    return rx && tx;
  }
  // Called by the driver on the network task before accepting a new client.
  void newConnection() {
    xQueueReset(rx); xQueueReset(tx);
    generation.fetch_add(1, std::memory_order_release);
  }
  // Called only by the network task; no driver calls occur in loop() below.
  void pollNetwork() {
    network.loop();
    const bool online = network.isConnected();
    connected.store(online, std::memory_order_release);
    if (!online || !enabled.load(std::memory_order_acquire)) return;
    const auto current = generation.load(std::memory_order_acquire);
    if (network.hasPendingOutput() || uxQueueSpacesAvailable(rx)) {
      Frame frame{};
      frame.generation = current;
      frame.length = network.checkRecvFrame(frame.bytes);
      if (frame.length) xQueueSend(rx, &frame, 0);
    }
    Frame frame{};
    if (xQueueReceive(tx, &frame, 0) == pdTRUE && frame.generation == current)
      network.writeFrame(frame.bytes, frame.length);
  }
  void enable() override { enabled.store(true, std::memory_order_release); }
  void disable() override { enabled.store(false, std::memory_order_release); }
  bool isEnabled() const override { return enabled.load(std::memory_order_acquire); }
  bool isConnected() const override {
    return isEnabled() && connected.load(std::memory_order_acquire);
  }
  bool isWriteBusy() const override { return !tx || !uxQueueSpacesAvailable(tx); }
  void loop() override {
    acceptGeneration(generation.load(std::memory_order_acquire));
  }
  size_t writeFrame(const uint8_t* bytes, size_t length) override {
    if (!isConnected() || !length || length > MAX_FRAME_SIZE) return 0;
    Frame frame{};
    frame.generation = dispatchGeneration;
    frame.length = length;
    memcpy(frame.bytes, bytes, length);
    return xQueueSend(tx, &frame, 0) == pdTRUE ? length : 0;
  }
  size_t checkRecvFrame(uint8_t* bytes) override {
    Frame frame{};
    while (xQueueReceive(rx, &frame, 0) == pdTRUE) {
      const auto current = generation.load(std::memory_order_acquire);
      if (frame.generation == current) {
        acceptGeneration(current);
        memcpy(bytes, frame.bytes, frame.length);
        return frame.length;
      }
    }
    return 0;
  }
};
}
