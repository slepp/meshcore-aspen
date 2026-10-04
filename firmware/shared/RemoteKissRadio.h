#pragma once

#include "QueuedTxProtocol.h"
#include <Arduino.h>
#include <Dispatcher.h>
#include <Identity.h>
#include <string.h>

#ifndef MESH_QUEUED_RADIO_API
#error "RemoteKissRadio requires firmware/shared/queued-dispatch.patch"
#endif

#define MESH_REMOTE_KISS_DIAGNOSTICS 1

// Dispatch-loop diagnostics stay separate from the framed USB interface.
class RemoteKissDiagnostics {
public:
  enum Code : uint8_t {
    None = 0, Receive = 1, Offline = 2, Admission = 3, Terminal = 4,
    Entropy = 5, WiFi = 6, Update = 7
  };
  static constexpr uint8_t STATS_TYPE = 0x80;
  static constexpr size_t PAYLOAD_CAPACITY = 105;

  static void record(Code code, const char* message, uint32_t a = 0,
                     uint32_t b = 0, uint32_t c = 0) {
    auto& s = state();
    auto& event = s.events[s.next];
    event = {};
    if (s.total != UINT32_MAX) ++s.total;
    if (s.count < 8) ++s.count;
    else if (s.overwritten != UINT32_MAX) ++s.overwritten;
    event.sequence = s.total;
    event.at_ms = millis();
    event.code = code;
    event.a = a; event.b = b; event.c = c;
    while (event.length < sizeof(event.message) - 1 && message[event.length]) {
      event.message[event.length] = message[event.length];
      ++event.length;
    }
    event.truncated = message[event.length] != 0;
    s.next = (s.next + 1) % 8;
#ifndef ENABLE_USB_INTERFACE
    Serial.printf("%s [code=%u a=%lu b=%lu c=%lu]\n", event.message,
                  unsigned(code), static_cast<unsigned long>(a),
                  static_cast<unsigned long>(b), static_cast<unsigned long>(c));
#endif
  }

  // CMD_GET_STATS subtype 0x80; index zero is newest. No unsolicited USB output.
  static size_t format(uint8_t* out, size_t capacity, uint8_t index = 0) {
    const auto& s = state();
    if (capacity < PAYLOAD_CAPACITY || (index >= s.count && (s.count || index)))
      return 0;
    memset(out, 0, 34);
    out[0] = 1;
    queued_tx::put32(out + 2, s.total);
    queued_tx::put32(out + 6, s.overwritten);
    out[10] = s.count;
    out[11] = index;
    if (!s.count) return 34;
    const auto& event = s.events[(s.next + 7 - index) % 8];
    out[1] = 1 | (event.truncated ? 2 : 0);
    queued_tx::put32(out + 12, event.sequence);
    queued_tx::put32(out + 16, event.at_ms);
    out[20] = event.code;
    queued_tx::put32(out + 21, event.a);
    queued_tx::put32(out + 25, event.b);
    queued_tx::put32(out + 29, event.c);
    out[33] = event.length;
    memcpy(out + 34, event.message, event.length);
    return 34 + event.length;
  }

private:
  struct Event {
    uint32_t sequence = 0, at_ms = 0, a = 0, b = 0, c = 0;
    Code code = None;
    uint8_t length = 0;
    bool truncated = false;
    char message[72]{};
  };
  struct State {
    Event events[8]{};
    uint32_t total = 0, overwritten = 0;
    uint8_t count = 0, next = 0;
  };
  static State& state() { static State storage; return storage; }
};

/**
 * A mesh::Radio backed by a duplex KISS Stream.
 *
 * Required queued mode verifies the operator profile without retuning.
 * UART users must signal both link transitions when their transport exposes
 * them. Uncorrelated control timeouts quarantine the session until reconnect.
 */
class RemoteKissRadio : public mesh::Radio {
public:
  enum class Mode { Queued, Legacy };
  explicit RemoteKissRadio(Stream &link, Mode mode = Mode::Queued);

  void begin() override;
  void init() { begin(); }
  void onLinkConnected();
  void onLinkDisconnected();
  bool supportsQueuedTransmit() const override { return _mode == Mode::Queued; }
  bool queuedReady() const override;
  bool setQueuedSourcePolicy(float factor) override;
  bool queueTransmit(const uint8_t *, int, uint8_t, uint32_t, uint32_t,
                     uint32_t &) override;
  bool pollQueuedResult(mesh::QueuedTransmitResult &) override;
  bool lastReceiveWasLocal() const override { return _last_local; }
  bool getQueuedRadioStats(mesh::QueuedRadioStats&) const override;
  const char *lastError() const { return _error; }
  bool linkFault() const { return _join == Join::Fault; }
  void setExpectedPHYPolicy(float aggregate_factor, bool cad,
                            int16_t interference);
  void setParams(float freq_mhz, float bw_khz, uint8_t sf, uint8_t cr);
  void setTxPower(int8_t dbm);
  void powerOff() {}
  uint32_t getRngSeed() const;
  static mesh::LocalIdentity newIdentity();
  uint32_t getPacketsRecv() const { return _packets_received; }
  uint32_t getPacketsSent() const { return _packets_sent; }
  uint32_t getPacketsRecvErrors() const { return _receive_errors; }
  void resetStats() { _packets_received = _packets_sent = _receive_errors = 0; }
  bool setRxBoostedGainMode(bool) { return false; }
  bool getRxBoostedGainMode() const { return false; }

  int recvRaw(uint8_t *bytes, int size) override;
  uint32_t getEstAirtimeFor(int length) override;
  float packetScore(float snr, int packet_length) override;
  bool startSendRaw(const uint8_t *bytes, int length) override;
  bool isSendComplete() override;
  void onSendFinished() override;
  void loop() override;
  int getNoiseFloor() const override;
  bool isInRecvMode() const override;
  bool isReceiving() override;
  float getLastRSSI() const override;
  float getLastSNR() const override;

private:
  static constexpr uint8_t FEND = 0xC0;
  static constexpr uint8_t FESC = 0xDB;
  static constexpr uint8_t TFEND = 0xDC;
  static constexpr uint8_t TFESC = 0xDD;
  static constexpr uint16_t MAX_FRAME = 280;
  static constexpr uint32_t META_WAIT_MS = 100;

  Stream &_link;
  Mode _mode;
  enum class Join {
    Offline,
    Hello,
    Profile,
    CommitCheck,
    Source,
    Metadata,
    Noise,
    Airtime,
    Verify,
    Statistics,
    Ready,
    Fault
  };
  Join _join = Join::Offline;
  uint32_t _generation = 0, _config_generation = 0, _next_job = 0,
           _control_deadline = 0;
  uint32_t _airtime[256]{};
  static constexpr uint32_t STATS_INTERVAL_MS = 1000;
  static constexpr uint32_t STATS_MAX_AGE_MS = 4000;
  mesh::QueuedRadioStats _stats{};
  uint32_t _next_stats = 0;
  uint8_t _queue_capacity = 0;
  bool _stats_valid = false, _stats_pending = false;
  uint16_t _airtime_index = 0;
  uint8_t _expected_profile[queued_tx::PROFILE_SIZE]{};
  bool _profile_pinned = false, _power_set = false, _last_local = false,
       _rx_local = false;
  float _source_factor = 1, _effective_factor = -1, _requested_factor = 1;
  const char *_error = "not connected";
  struct Job {
    bool used = false, accepted = false, accepted_pending = false;
    bool pending = false, terminal = false;
    uint32_t admitted_deadline = 0;
    mesh::QueuedTransmitResult result{};
    mesh::QueuedTransmitResult admission{};
  };
  Job _jobs[12]{};
  void serviceQueued();
  void processQueued(uint8_t subcommand, const uint8_t *data, uint16_t length);
  void failQueued(const char *error);
  bool control(uint8_t subcommand, const uint8_t *data, uint16_t length);
  void requestSource();
  void requestStats();
  uint8_t _frame[MAX_FRAME];
  uint16_t _frame_length;
  bool _escaped;
  bool _inside_frame;

  uint8_t _rx_packet[255];
  uint16_t _rx_length;
  bool _rx_ready;
  uint32_t _rx_started_ms;

  bool _tx_active;
  bool _tx_success;
  bool _tx_quarantined;
  int16_t _noise_floor;
  float _last_rssi;
  float _last_snr;

  uint32_t _frequency_hz;
  float _requested_frequency_mhz = 0;
  uint32_t _bandwidth_hz;
  uint8_t _sf;
  uint8_t _cr;
  int8_t _tx_power;
  uint32_t _packets_received;
  uint32_t _packets_sent;
  uint32_t _receive_errors;

  void processByte(uint8_t byte);
  void processFrame();
  void rejectRx(const char* reason);
  bool writeFrame(uint8_t command, const uint8_t *payload, uint16_t length);
  void writeHardware(uint8_t subcommand, const uint8_t *payload = nullptr,
                     uint16_t length = 0);
};
