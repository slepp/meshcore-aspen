#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include "KissModem.h"
#include "QueuedTxProtocol.h"
#include "RadioDashboard.h"

#ifndef KISS_LOCAL_SOURCES
#define KISS_LOCAL_SOURCES 0
#endif
#ifndef KISS_STREAM_ENDPOINT
#define KISS_STREAM_ENDPOINT 0
#endif

class KissLocalSource {
public:
  virtual void received(const uint8_t*, uint16_t, float, float, bool) = 0;
  virtual void completed(uint32_t job, uint8_t state, uint8_t reason,
                         uint32_t queue_ms, uint32_t rf_ms, uint32_t estimate) = 0;
};

class KissPacketObserver {
public:
  virtual void packet(const uint8_t*, uint16_t, bool transmit, uint8_t state,
                      float rssi, float snr) = 0;
  virtual void transmitStarting(const uint8_t*, uint16_t, uint8_t, uint32_t,
                                uint32_t, uint8_t, uint32_t, uint32_t,
                                uint32_t) {}
  virtual void transmitCompleted(const uint8_t*, uint16_t, uint8_t, uint32_t,
                                 uint32_t, uint8_t, uint8_t, uint32_t,
                                 uint32_t, uint32_t) {}
  virtual void transmitted(const uint8_t* raw, uint16_t length, uint8_t state,
                           uint8_t, uint32_t, uint32_t) {
    if (state != queued_tx::ACCEPTED) packet(raw, length, true, state, 0, 0);
  }
};

class WifiKissMultiplexer : public Stream {
public:
  static constexpr int8_t LOCAL_LOOPBACK_SNR = INT8_MIN;
  static constexpr int8_t LOCAL_LOOPBACK_RSSI = INT8_MAX;

  WifiKissMultiplexer();

  void poll(WiFiServer& server);
  // Retire only network sources; native roles and a queued UART remain usable.
  void disconnectTcpClients();
  bool attachStream(Stream& stream);
  void detachStream();
  void pollStream();
  bool streamFaulted() const;
  uint32_t streamOutputOverflows() const;
  void afterModemLoop();
  size_t clientCount() const;
  // Publish native roles/keys for advisory discovery before accepting TCP clients.
  bool setNativeRolePresence(uint8_t mask, const uint8_t* keys, uint8_t count);
  void setActiveRolePresence(uint8_t mask);
  void attachRadio(mesh::Radio& radio, mesh::RNG& rng, SetRadioCallback configure,
                   SetTxPowerCallback power, bool (*setRxBoost)(bool) = nullptr,
                   bool (*getRxBoost)() = nullptr);
  bool setInitialConfiguration(const RadioConfig& config,
                               bool require_operator_phy = false);
  // Dispatch-task only, after authenticated mast administration acknowledges
  // the change on the old PHY. Queued old-generation jobs fail explicitly.
  bool applyMastConfiguration(const RadioConfig& config, bool persist);
  bool applyMastCAD(bool enabled);
  bool applyMastInterference(uint8_t threshold);
  bool applyMastAirtimeFactor(float factor);
  bool applyMastControls(uint16_t agcSeconds, bool configureRxBoost, bool rxBoost);
  bool cadEnabled() const { return _profile[15] != 0; }
  uint16_t agcResetIntervalSeconds() const { return _agc_seconds; }
  float airtimeFactor() const { return queued_tx::getFloat(_profile + 11); }
  bool rxBoostAvailable() const { return _set_rx_boost && _get_rx_boost; }
  bool rxBoostConfigured() const { return _rx_boost_configured; }
  bool rxBoostEnabled() const { return _get_rx_boost && _get_rx_boost(); }
  bool hasPersistedConfiguration() const { return queued_tx::get32(_persisted_profile) != 0; }
  bool restoreMastConfiguration();
  uint32_t configurationGeneration() const { return _configuration_generation; }
  bool isActuallyTransmitting() const { return _transmitting; }
  bool hasPendingTransmit() const;
  uint8_t sourceQueuedCount(uint8_t slot) const;
  bool sourceTransmitting(uint8_t slot) const;
#ifdef MESH_QUEUED_RADIO_API
  bool getQueuedRadioStats(uint8_t slot, mesh::QueuedRadioStats& stats) const;
#endif
  void serviceTransmit();
  void observeWith(RadioDashboard& dashboard) { _dashboard = &dashboard; }
  bool dashboardTotals(RadioDashboard::Totals& totals, RadioDashboard::RadioStatus* radio = nullptr) const {
    return _dashboard && _dashboard->totals(totals, radio);
  }
  void observePackets(KissPacketObserver& observer) { _packet_observer = &observer; }
  void dashboardStatus(RadioDashboard::RadioStatus& status) const;
  int attachLocal(KissLocalSource& source);
  void detachLocal(uint8_t slot);
  bool setLocalPolicy(uint8_t slot, float factor);
  bool submitLocal(uint8_t slot, const uint8_t* packet, uint16_t length,
                   uint32_t job, uint8_t priority, uint32_t delay, uint32_t expiry);
  void received(const uint8_t* packet, uint16_t length, float rssi, float snr);
  bool localReady() const { return _configuration_generation && !_configuration_fault; }
  RadioConfig currentConfiguration() const {
    return {queued_tx::get32(_profile), queued_tx::get32(_profile + 4),
            _profile[8], _profile[9], _profile[10]};
  }
  mesh::Radio& physicalRadio() const { return *_radio; }
  int interferenceThreshold() const {
    return static_cast<int16_t>(queued_tx::get16(_profile + 16));
  }

  int available() override;
  int read() override;
  int peek() override;
  void flush() override;
  int availableForWrite() override;
  size_t write(uint8_t byte) override;
  size_t write(const uint8_t* data, size_t size) override;

private:
  void configurationFailed();
  static constexpr uint16_t MAX_ENCODED_FRAME = KISS_MAX_ENCODED_FRAME_SIZE;
  static constexpr uint16_t CLIENT_OUTPUT_CAPACITY = 2 * MAX_ENCODED_FRAME;
  static constexpr uint16_t CLIENT_INPUT_BUDGET = 512;
  static constexpr uint8_t SESSION_BASE =
      KISS_MAX_TCP_CLIENTS + KISS_LOCAL_SOURCES + KISS_STREAM_ENDPOINT;
  static_assert(SESSION_BASE + queued_tx::SESSION_PORTS - 1 <= UINT8_MAX,
                "KISS source slots must fit uint8_t");

  struct ClientTarget {
    uint8_t slot;
    uint32_t generation;
  };

  struct SourceState {
    uint32_t generation;
    bool active;
    float source_factor;
    uint32_t source_credit;
    uint32_t source_updated;
    uint32_t source_airtime;
    uint32_t source_successes;
    uint32_t source_failures;
  };

  struct WireState : SourceState {
    bool signal_report;
    bool collecting;
    uint16_t input_length;
    uint8_t input[MAX_ENCODED_FRAME];
    uint16_t output_head;
    uint16_t output_length;
    uint8_t output[CLIENT_OUTPUT_CAPACITY];
    bool negotiated;
    uint32_t last_job;
    uint32_t configuration_seen;
    bool has_role_presence;
    uint8_t announced_role;
    uint8_t announced_key[queued_tx::ROLE_KEY_SIZE];
    uint8_t presence_warnings;
  };
  struct ClientState : WireState { WiFiClient socket; };

  struct QueuedFrame {
    ClientTarget source;
    uint16_t encoded_length;
    uint16_t decoded_length;
    uint8_t encoded[MAX_ENCODED_FRAME];
    uint8_t decoded[KISS_MAX_FRAME_SIZE];
  };

  ClientState _clients[KISS_MAX_TCP_CLIENTS];
  WireState _session_ports[queued_tx::SESSION_PORTS - 1]{};
  uint8_t _session_slot = KISS_MAX_TCP_CLIENTS;
  uint8_t _session_cursor = 0;
#if KISS_STREAM_ENDPOINT
  static constexpr uint8_t STREAM_SLOT = KISS_MAX_TCP_CLIENTS + KISS_LOCAL_SOURCES;
  static_assert(STREAM_SLOT < UINT8_MAX, "KISS source slots must fit uint8_t");
  WireState _stream{};
  Stream* _stream_io = nullptr;
  uint32_t _stream_overflows = 0;
  void faultStream();
#endif
#if KISS_LOCAL_SOURCES > 0
  struct LocalState : SourceState { KissLocalSource* sink; };
  LocalState _locals[KISS_LOCAL_SOURCES]{};
#endif
  SourceState& sourceState(uint8_t slot);
  const SourceState& sourceState(uint8_t slot) const;
  WireState& wireState(uint8_t slot);
  QueuedFrame _queue[KISS_REQUEST_QUEUE_DEPTH];
  uint8_t _queue_head;
  uint8_t _queue_tail;
  uint8_t _queue_count;
  uint32_t _next_generation;
  uint8_t _poll_cursor;
  static constexpr uint8_t MAX_NATIVE_KEYS = 12;
  uint8_t _native_role_mask = 0;
  uint8_t _active_role_mask = 0;
  uint8_t _native_key_count = 0;
  uint8_t _native_keys[MAX_NATIVE_KEYS][queued_tx::ROLE_KEY_SIZE]{};

  QueuedFrame _active;
  bool _active_valid;
  bool _active_waits_for_response;
  bool _active_is_data;
  bool _active_input_consumed;
  uint16_t _active_input_offset;

  bool _output_collecting;
  uint16_t _output_length;
  uint8_t _output[MAX_ENCODED_FRAME];

  struct TxJob {
    bool used;
    bool extended;
    ClientTarget source;
    uint32_t id;
    uint32_t sequence;
    uint32_t admitted;
    uint32_t eligible;
    uint32_t expiry;
    uint32_t expiry_delay;
    uint8_t priority;
    uint16_t length;
    uint8_t packet[KISS_MAX_PACKET_SIZE];
  };
  TxJob _jobs[KISS_REQUEST_QUEUE_DEPTH]{};
  TxJob _sending{};
  mesh::Radio* _radio = nullptr;
  RadioDashboard* _dashboard = nullptr;
  KissPacketObserver* _packet_observer = nullptr;
  mesh::RNG* _rng = nullptr;
  SetRadioCallback _configure = nullptr;
  SetTxPowerCallback _power = nullptr;
  bool (*_set_rx_boost)(bool) = nullptr;
  bool (*_get_rx_boost)() = nullptr;
  uint16_t _agc_seconds = 30;
  bool _rx_boost_configured = false, _rx_boost = false;
  ClientTarget _owner{};
  uint8_t _profile[queued_tx::PROFILE_SIZE]{};
  uint8_t _persisted_profile[queued_tx::PROFILE_SIZE]{};
  bool applyMastProfile(const uint8_t* profile, bool persist);
  uint32_t _configuration_generation = 0;
  bool _profile_committed = false;
  bool _configuration_fault = false;
  uint32_t _sequence = 0;
  uint32_t _credit = queued_tx::WINDOW_MS / 2;
  uint32_t _credit_updated = 0;
  uint32_t _airtime = 0;
  uint32_t _successes = 0;
  uint32_t _failures = 0;
  uint32_t _rf_start = 0;
  uint32_t _rf_estimate = 0;
  uint32_t _busy_since = 0;
  uint32_t _next_carrier = 0;
  bool _carrier_wait = false;
  bool _transmitting = false;

  bool extension(uint8_t slot, const uint8_t* decoded, uint16_t length);
  void submit(uint8_t slot, const uint8_t* packet, uint16_t length,
              bool extended, uint32_t id, uint8_t priority,
              uint32_t delay, uint32_t expiry);
  void notify(const TxJob& job, uint8_t state, uint8_t reason,
              uint32_t queue_ms = 0, uint32_t rf_ms = 0, uint32_t estimate = 0,
              const ClientTarget* delivery = nullptr);
  void finishTransmit(uint8_t state, uint8_t reason);
  TxJob* nextTransmit(uint32_t now);
  void reflect(const TxJob& job);
  void refill(uint32_t now);
  static void refillBudget(uint32_t now, float factor, uint32_t& credit,
                           uint32_t& updated);

  void acceptClient(WiFiClient client);
  void removeClient(uint8_t slot);
  void drainClientOutput(uint8_t slot);
  void drainSessionOutput();
  bool sessionPort(uint8_t slot) const {
    return slot >= SESSION_BASE &&
           slot < SESSION_BASE + queued_tx::SESSION_PORTS - 1;
  }
  uint8_t sessionNumber(uint8_t slot) const {
    return slot - SESSION_BASE + 1;
  }
  uint8_t sessionSource(uint8_t port) const {
    return SESSION_BASE + port - 1;
  }
  uint32_t newGeneration();
  bool isClientConnected(const ClientState& client) const;
  bool isTargetConnected(ClientTarget target) const;
  void processClientByte(uint8_t slot, uint8_t byte);
  void finishClientFrame(uint8_t slot);
  bool enqueueFrame(uint8_t slot, const uint8_t* encoded, uint16_t encoded_length);
  bool loadNextFrame();
  void completeActive();

  void processOutputByte(uint8_t byte);
  void finishOutputFrame();
  void routeOutputFrame(const uint8_t* encoded, uint16_t encoded_length,
                        const uint8_t* decoded, uint16_t decoded_length);

  void sendFrame(ClientTarget target, const uint8_t* encoded, uint16_t length,
                 bool unsolicited = false);
  void broadcastFrame(const uint8_t* encoded, uint16_t length,
                      const ClientTarget* exclude = nullptr, bool signal_only = false);
  void sendHardware(ClientTarget target, uint8_t subcommand,
                    const uint8_t* payload = nullptr, uint16_t payload_length = 0);
  void sendLocalReflection();

  static uint16_t decodeFrame(const uint8_t* encoded, uint16_t encoded_length,
                              uint8_t* decoded, uint16_t decoded_capacity);
  static uint16_t encodeFrame(uint8_t command, const uint8_t* payload,
                              uint16_t payload_length, uint8_t* encoded,
                              uint16_t encoded_capacity);
};
