// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "WifiKissMultiplexer.h"
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <mqtt_client.h>

namespace onchip {
class Observer final : public KissPacketObserver {
  friend struct ObserverTest;
  struct Event {
    uint32_t at = 0, epoch = 0;
    uint64_t sequence = 0;
    uint16_t length = 0;
    uint8_t raw[255]{}, state = 0;
    bool tx = false;
    float rssi = 0, snr = 0;
    RadioDashboard::RoleStatus source{};
  };
  struct Pending {
    bool used = false;
    uint8_t slot = 0;
    uint32_t generation = 0, job = 0;
    RadioDashboard::RoleStatus source{};
  };
  using Roles = RadioDashboard::RoleStatus[RadioDashboard::ROLE_CAPACITY];
  struct Snapshot {
    Roles roles{};
    char radio[64]{};
  };
  Pending pending[KISS_REQUEST_QUEUE_DEPTH + 1]{};
  Roles latest{}, published{};
  WifiKissMultiplexer *modem = nullptr;
  char radio[64] = "unknown";
  bool haveRoles = false, rolesPublished = false;
  RadioDashboard::RoleStatus identity{};
  RadioDashboard::RoleStatus botSource{};
  QueueHandle_t queue = nullptr, statusQueue = nullptr;
  esp_mqtt_client_handle_t client = nullptr;
  std::atomic<bool> connected{false};
  std::atomic<const char *> connectionFault{nullptr};
  std::atomic<uint32_t> session{0}, announcedSession{0}, dropped{0};
  std::atomic<uint32_t> publicationErrors{0};
  std::atomic<uint64_t> nextSequence{0};
  char identityHex[65]{}, eventPrefix[33]{}, baseTopic[192]{}, packetTopic[224]{},
      statusTopic[224]{};
  char username[69]{};
  char json[3072]{};
  uint32_t tokenIssued = 0, tokenExpires = 0, nextConnect = 0;
  uint32_t lastStatusAt = 0;
  uint32_t lastReadyEpoch = 0;
  mutable std::atomic<uint64_t> acceptedClock{0};
  mutable std::atomic<uint32_t> clockPublicationAt{0};
  std::atomic<uint32_t> loopStackMinimum{UINT32_MAX};
  bool stackReported = false;
  static void event(void *, esp_event_base_t, int32_t, void *);
  static void worker(void *);
  void service();
  void enqueue(const Event &);
  uint32_t observationEpoch(uint32_t at, bool current = false) const;
  bool publish(const char *, const char *, int, bool retain);
  bool fail(const char *);
  bool startClient(uint32_t epoch);
  void publishPacket(const Event &);

public:
  bool begin(WifiKissMultiplexer &mux);
  void dashboardStatus(RadioDashboard::RoleStatus &) const;
  void statistics(char *reply, size_t capacity) const;
  void observeRoles(const RadioDashboard::RadioStatus &);
  void packet(const uint8_t *, uint16_t, bool, uint8_t, float, float) override;
  void transmitted(const uint8_t *, uint16_t, uint8_t, uint8_t, uint32_t,
                   uint32_t) override;
};
} // namespace onchip
