#pragma once

#include "QueuedTxProtocol.h"
#include <atomic>
#include <stddef.h>
#include <stdint.h>

#ifdef ARDUINO_ARCH_ESP32
#include <esp_http_server.h>
#endif

// Mutating methods belong exclusively to the radio loop. HTTP only reads
// published copies; neither side waits for the other to release the copy gate.
class RadioDashboard {
public:
  static constexpr size_t HISTORY = 32;
  static constexpr size_t TRAFFIC_SECONDS = 60;
  static constexpr size_t PREVIEW_BYTES = 16;
#if defined(MESHCORE_ONCHIP) && defined(ESP32) && !defined(NRF52_PLATFORM)
  static constexpr size_t JSON_CAPACITY = 40960;
  static constexpr size_t CONTACT_CAPACITY = 32;
  struct ContactStatus {
    uint8_t public_key[32]{};
    char name[32]{};
    uint8_t type = 0;
  };
#else
  static constexpr size_t JSON_CAPACITY = 24576;
#endif
  static constexpr size_t HTTP_INTERNAL_SOCKETS = 3;
  static constexpr size_t HTTP_CLIENTS = 3;
  static constexpr size_t LIVE_CLIENTS = 2;
#if defined(MESHCORE_ONCHIP_BOT) && MESHCORE_ONCHIP_BOT
  static constexpr size_t ROLE_CAPACITY = 7;
#else
  static constexpr size_t ROLE_CAPACITY = 6;
#endif

  struct RoleStatus {
    char role[12]{}, name[32]{}, state[16]{}, fault[48]{};
    uint8_t public_key[32]{};
    bool has_identity = false;
    bool ready = false;
    int16_t source_slot = -1;
    uint32_t source_generation = 0;
    uint64_t profile_generation = 0;
  };

  struct Client {
    bool connected = false;
    bool negotiated = false;
    uint32_t generation = 0;
    float factor = 0;
    uint32_t credit_ms = 0;
    uint32_t rf_ms = 0;
  };

  struct RadioStatus {
    struct StreamStatus {
      bool enabled = false;
      uint8_t slot = 0;
      uint32_t generation = 0;
      bool connected = false;
      bool negotiated = false;
      bool fault = false;
      uint32_t output_overflows = 0;
    } stream;
    struct SessionStatus {
      bool connected = false;
      uint8_t physical_slot = 0;
      Client port[queued_tx::SESSION_PORTS - 1]{};
      uint16_t output_queued_bytes[queued_tx::SESSION_PORTS - 1]{};
    } session;
    uint8_t profile[queued_tx::PROFILE_SIZE]{};
    uint32_t generation = 0;
    bool committed = false;
    bool fault = false;
    bool transmitting = false;
    bool carrier_wait = false;
    uint8_t queued = 0;
    uint8_t clients = 0;
    int16_t owner_slot = -1;
    uint32_t credit_ms = 0;
    Client client[KISS_MAX_TCP_CLIENTS]{};
    bool wifi_connected = false;
    int16_t wifi_rssi = 0;
    uint32_t free_heap = 0;
    uint32_t minimum_heap = 0;
    uint32_t dma_free_heap = 0;
    uint32_t dma_largest_heap = 0;
    uint32_t dma_minimum_heap = 0;
    uint32_t rx_errors = 0;
#ifdef MESHCORE_ONCHIP
    char device_name[32]{};
    uint8_t role_count = 0;
    RoleStatus roles[ROLE_CAPACITY]{};
#if defined(ESP32) && !defined(NRF52_PLATFORM)
    bool contacts_available = false;
    uint16_t contact_count = 0, contact_total = 0;
    ContactStatus contacts[CONTACT_CAPACITY]{};
#endif
#endif
  };

  struct Event {
    uint64_t sequence = 0;
    uint64_t at_ms = 0;
    bool rx = false;
    bool has_signal = false;
    float rssi = 0;
    float snr = 0;
    uint8_t state = 0;
    uint8_t reason = 0;
    uint8_t source_slot = 0;
    uint32_t source_generation = 0;
    uint32_t job_id = 0;
    uint16_t length = 0;
    uint8_t preview_length = 0;
    uint8_t preview[PREVIEW_BYTES]{};
    uint32_t queue_ms = 0;
    uint32_t rf_ms = 0;
    uint32_t estimated_ms = 0;
  };

  struct Bucket {
    uint64_t second = 0;
    uint32_t rx_packets = 0;
    uint32_t tx_packets = 0;
    uint32_t tx_rf_ms = 0;
    uint32_t rx_estimated_ms = 0;
  };

  struct Totals {
    uint64_t publication = 0;
    uint64_t uptime_ms = 0;
    uint64_t wifi_uptime_ms = 0;
    uint64_t publications_skipped = 0;
    uint64_t events_total = 0;
    uint64_t events_overwritten = 0;
    uint64_t rx_packets = 0;
    uint64_t rx_estimated_ms = 0;
    uint64_t tx_accepted = 0;
    uint64_t tx_rejected = 0;
    uint64_t tx_succeeded = 0;
    uint64_t tx_failed = 0;
    uint64_t tx_unknown = 0;
    uint64_t tx_rf_ms = 0;
  };

  struct Snapshot : Totals {
    RadioStatus radio{};
    uint8_t event_count = 0;
    uint8_t event_next = 0;
    Event events[HISTORY]{};
    Bucket traffic[TRAFFIC_SECONDS]{};
  };

  void received(uint32_t now, const uint8_t *packet, uint16_t length,
                float rssi, float snr, uint32_t estimated_ms);
  void transmitted(uint32_t now, const uint8_t *packet, uint16_t length,
                   uint8_t slot, uint32_t generation, uint32_t job,
                   uint8_t state, uint8_t reason, uint32_t queue_ms,
                   uint32_t rf_ms, uint32_t estimated_ms);
  bool publish(uint32_t now, const RadioStatus &status);
  bool snapshot(Snapshot &destination) const;
  bool totals(Totals &destination, RadioStatus *radio = nullptr) const;
  static size_t formatJSON(const Snapshot &snapshot, const char *device_name,
                           char *output, size_t capacity);
  static size_t formatRoleJSON(const RoleStatus &, char *output, size_t capacity);

#ifdef ARDUINO_ARCH_ESP32
  bool beginHTTP();
#endif

private:
  friend struct DashboardStreamTest;
  Snapshot _live{};
  Snapshot _published{};
  mutable std::atomic_flag _copying = ATOMIC_FLAG_INIT;
  uint32_t _last_clock = 0;
  uint64_t _wifi_since = 0;
  void advance(uint32_t now);
  Event &event(const uint8_t *packet, uint16_t length);
  Bucket &bucket(uint64_t second);
  void occupancy(uint32_t duration, bool rx);

  static constexpr size_t LIVE_FRAGMENT = 1024;
  struct LiveClient {
    int fd = -1;
    size_t offset = 0;
    uint64_t delivered = 0;
    uint64_t started_ms = 0;
    uint64_t ping_ms = 0;
    uint64_t pong_deadline_ms = 0;
    bool sending = false;
    bool closing = false;
    uint8_t input[131]{};
    size_t input_length = 0;
    uint64_t input_since_ms = 0;
    uint8_t output[LIVE_FRAGMENT + 4]{};
    size_t output_length = 0, output_offset = 0;
    uint64_t output_since_ms = 0;
    uint8_t output_opcode = 0;
    uint8_t reply[125]{};
    size_t reply_length = 0;
    bool reply_pending = false;
    uint32_t pings_sent = 0, pongs_received = 0;
    const char *close_reason = "peer closed";
  };
  LiveClient _clients[LIVE_CLIENTS]{};
  Snapshot _stream_snapshot{};
  char _stream_json[JSON_CAPACITY]{};
  size_t _stream_length = 0;
  uint64_t _stream_publication = 0;
  using LiveSend = bool (*)(void *, int, uint8_t, const uint8_t *, size_t,
                            bool);
  using LiveDrop = void (*)(void *, int);
  LiveClient *subscribe(int fd, uint64_t now);
  void pumpLive(uint64_t now, const char *name, LiveSend send, LiveDrop drop,
                void *context);
  static int sendAvailable(int fd, const char *data, size_t length, int flags);

#ifdef ARDUINO_ARCH_ESP32
  httpd_handle_t _server = nullptr;
  Snapshot _http_snapshot{};
  char _json[JSON_CAPACITY]{};
  int64_t _last_api_us = 0;
  std::atomic<bool> _live_work_pending{false};
  static void liveTask(void *context);
  static void liveWork(void *context);
  static bool liveSend(void *context, int fd, uint8_t opcode,
                       const uint8_t *data, size_t length, bool final);
  static void liveDrop(void *context, int fd);
  static void releaseLive(void *client);
  static int liveReceive(httpd_handle_t server, int fd, char *data,
                         size_t length, int flags);
  static bool receiveControl(LiveClient &client, uint64_t now);
  static bool flushOutput(LiveClient &client, uint64_t now);
  static esp_err_t pageRequest(httpd_req_t *request);
  static esp_err_t statusRequest(httpd_req_t *request);
  static esp_err_t liveRequest(httpd_req_t *request);
#endif
};
