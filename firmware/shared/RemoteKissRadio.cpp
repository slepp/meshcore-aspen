#include "RemoteKissRadio.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#if defined(ARDUINO_ARCH_ESP32)
#include <esp_system.h>
#elif defined(NRF52_PLATFORM)
#include <nrf.h>
#include <nrf_soc.h>
#endif

RemoteKissRadio::RemoteKissRadio(Stream &link, Mode mode)
    : _link(link), _mode(mode), _frame_length(0), _escaped(false),
      _inside_frame(false), _rx_length(0), _rx_ready(false), _rx_started_ms(0),
      _tx_active(false), _tx_success(false), _tx_quarantined(false),
      _noise_floor(-120), _last_rssi(NAN), _last_snr(NAN), _frequency_hz(0),
      _bandwidth_hz(0), _sf(0), _cr(0), _tx_power(0), _packets_received(0),
      _packets_sent(0), _receive_errors(0) {
  queued_tx::putFloat(_expected_profile + 11, 1);
  if (_mode == Mode::Queued)
    _noise_floor = INT16_MIN;
}

void RemoteKissRadio::begin() {
  if (_mode == Mode::Legacy || _join == Join::Offline)
    onLinkConnected();
}

void RemoteKissRadio::onLinkConnected() {
  if (_mode == Mode::Queued)
    onLinkDisconnected();
  _frame_length = 0;
  _inside_frame = _escaped = false;
  _rx_length = 0;
  _rx_ready = false;
  _tx_active = _tx_success = _tx_quarantined = false;
  _rx_local = _last_local = false;
  if (_mode == Mode::Queued) {
    _effective_factor = -1;
    _airtime_index = 0;
    _join = Join::Hello;
    const uint8_t hello[] = {queued_tx::VERSION, 0};
    control(queued_tx::HELLO, hello, sizeof(hello));
    return;
  }
  const uint8_t enabled = 1;
  writeHardware(0x19, &enabled, 1);
  if (_frequency_hz != 0) {
    uint8_t radio[10];
    memcpy(radio, &_frequency_hz, 4);
    memcpy(radio + 4, &_bandwidth_hz, 4);
    radio[8] = _sf;
    radio[9] = _cr;
    writeHardware(0x09, radio, sizeof(radio));
  }
  if (_tx_power != 0) {
    const uint8_t power = static_cast<uint8_t>(_tx_power);
    writeHardware(0x0A, &power, 1);
  }
  _tx_active = false;
}

void RemoteKissRadio::setParams(float freq_mhz, float bw_khz, uint8_t sf,
                                uint8_t cr) {
  _requested_frequency_mhz = freq_mhz;
  _frequency_hz = static_cast<uint32_t>(freq_mhz * 1000000.0f);
  _bandwidth_hz = static_cast<uint32_t>(bw_khz * 1000.0f);
  _sf = sf;
  _cr = cr;
  if (_mode == Mode::Queued) {
    if (_profile_pinned &&
        (_requested_frequency_mhz !=
             static_cast<float>(queued_tx::get32(_expected_profile) /
                                1000000.0) ||
         _bandwidth_hz != queued_tx::get32(_expected_profile + 4) ||
         sf != _expected_profile[8] || cr != _expected_profile[9]))
      failQueued("radio settings differ from committed profile");
    return;
  }
  uint8_t radio[10];
  memcpy(radio, &_frequency_hz, 4);
  memcpy(radio + 4, &_bandwidth_hz, 4);
  radio[8] = sf;
  radio[9] = cr;
  writeHardware(0x09, radio, sizeof(radio));
}

void RemoteKissRadio::setTxPower(int8_t dbm) {
  _tx_power = dbm;
  _power_set = true;
  if (_mode == Mode::Queued) {
    if (_profile_pinned && static_cast<uint8_t>(dbm) != _expected_profile[10])
      failQueued("TX power differs from committed profile");
    return;
  }
  const uint8_t power = static_cast<uint8_t>(dbm);
  writeHardware(0x0A, &power, 1);
}

void RemoteKissRadio::loop() {
  for (unsigned n = 0; n < 1024 && _link.available() > 0; ++n) {
    const int value = _link.read();
    if (value >= 0)
      processByte(static_cast<uint8_t>(value));
  }
  if (_rx_length > 0 && !_rx_ready &&
      millis() - _rx_started_ms >= META_WAIT_MS) {
    if (_mode == Mode::Queued) {
      rejectRx("RX metadata missing");
    } else
      _rx_ready = true;
  }
  if (_mode == Mode::Queued)
    serviceQueued();
}

int RemoteKissRadio::recvRaw(uint8_t *bytes, int size) {
  loop();
  if (!_rx_ready || size < _rx_length)
    return 0;
  const int length = _rx_length;
  _last_local = _rx_local;
  memcpy(bytes, _rx_packet, length);
  _rx_length = 0;
  _rx_ready = false;
  if (!_last_local)
    ++_packets_received;
  return length;
}

bool RemoteKissRadio::startSendRaw(const uint8_t *bytes, int length) {
  loop();
  if (_mode == Mode::Queued)
    return false;
  if (_tx_active || _tx_quarantined || length <= 0 || length > 255)
    return false;
  _tx_active = true;
  _tx_success = false;
  if (!writeFrame(0x00, bytes, static_cast<uint16_t>(length))) {
    _tx_active = false;
    _tx_quarantined = true;
    return false;
  }
  return true;
}

bool RemoteKissRadio::isSendComplete() {
  loop();
  return !_tx_active && _tx_success;
}

void RemoteKissRadio::onSendFinished() {
  if (_tx_active)
    _tx_quarantined = true;
  _tx_active = false;
  _tx_success = false;
}

bool RemoteKissRadio::isInRecvMode() const { return !_tx_active; }
bool RemoteKissRadio::isReceiving() { return false; }
float RemoteKissRadio::getLastRSSI() const { return _last_rssi; }
float RemoteKissRadio::getLastSNR() const { return _last_snr; }
int RemoteKissRadio::getNoiseFloor() const { return _noise_floor; }

float RemoteKissRadio::packetScore(float snr, int packet_length) {
  if (_sf < 7 || _sf > 12)
    return 0;
  const float minimum_snr = -5.0f - 2.5f * (_sf - 6);
  if (snr < minimum_snr)
    return 0;
  const float score =
      (snr - minimum_snr) / 10.0f * (1 - packet_length / 256.0f);
  return fmaxf(0, fminf(1, score));
}

uint32_t RemoteKissRadio::getEstAirtimeFor(int length) {
  if (_mode == Mode::Queued)
    return queuedReady() && length >= 0 && length <= 255 ? _airtime[length] : 0;
  if (_bandwidth_hz == 0 || _sf < 5 || _sf > 12 || _cr < 5 || _cr > 8)
    return 0;
  const double symbol_ms = (double)(1UL << _sf) * 1000.0 / _bandwidth_hz;
  const int low_data_rate = symbol_ms >= 16.0 ? 1 : 0;
  const int numerator = 8 * length - 4 * _sf + (_sf <= 6 ? 0 : 8) + 20 + 16;
  const int denominator = 4 * (_sf - 2 * low_data_rate);
  const int payload_term =
      numerator > 0 ? (numerator + denominator - 1) / denominator : 0;
  const int preamble = _sf <= 8 ? 32 : 16;
  const double sync = _sf <= 6 ? 6.25 : 4.25;
  return static_cast<uint32_t>(
      ceil((preamble + 8 + sync + payload_term * _cr) * symbol_ms));
}

void RemoteKissRadio::rejectRx(const char* reason) {
  ++_receive_errors;
  _error = reason;
  if (!_rx_ready) {
    _rx_length = 0;
    _rx_local = false;
  }
  RemoteKissDiagnostics::record(RemoteKissDiagnostics::Receive, reason);
}

void RemoteKissRadio::processByte(uint8_t byte) {
  if (byte == FEND) {
    if (_mode == Mode::Queued && _inside_frame && _escaped)
      rejectRx("frame aborted during escape");
    if (_inside_frame && !_escaped && _frame_length > 0)
      processFrame();
    _inside_frame = true;
    _escaped = false;
    _frame_length = 0;
    return;
  }
  if (!_inside_frame)
    return;
  if (_escaped) {
    if (byte == TFEND)
      byte = FEND;
    else if (byte == TFESC)
      byte = FESC;
    else {
      if (_mode == Mode::Queued)
        rejectRx("invalid frame escape");
      _inside_frame = false;
      _frame_length = 0;
      _escaped = false;
      return;
    }
    _escaped = false;
  } else if (byte == FESC) {
    _escaped = true;
    return;
  }
  if (_frame_length < sizeof(_frame)) {
    _frame[_frame_length++] = byte;
  } else {
    if (_mode == Mode::Queued)
      rejectRx("frame exceeds receive capacity");
    _inside_frame = false;
    _frame_length = 0;
  }
}

void RemoteKissRadio::processFrame() {
  if (_mode == Mode::Queued && _rx_length > 0 && !_rx_ready &&
      !(_frame_length == 4 && _frame[0] == 0x06 && _frame[1] == 0xf9))
    rejectRx("RX metadata is not the adjacent frame");
  if ((_frame[0] >> 4) != 0)
    return;
  const uint8_t command = _frame[0] & 0x0F;
  if (command == 0x00 && _frame_length > 1 && _frame_length <= 256) {
    if (_rx_length == 0) {
      _rx_length = _frame_length - 1;
      memcpy(_rx_packet, _frame + 1, _rx_length);
      _rx_started_ms = millis();
      _rx_ready = false;
      _rx_local = false;
      _last_snr = _last_rssi = NAN;
    } else {
      if (_mode == Mode::Queued) {
        rejectRx("receive packet queue full");
      } else {
        ++_receive_errors;
        _rx_ready = true;
      }
    }
    return;
  }
  if (command != 0x06 || _frame_length < 2)
    return;
  const uint8_t subcommand = _frame[1];
  if (_mode == Mode::Queued &&
      (subcommand == 0xA0 || subcommand == 0xA2 || subcommand == 0xA3 ||
       subcommand == 0xA4 ||
       subcommand == 0xFA || subcommand == 0x8F || subcommand == 0x90 ||
       subcommand == 0x9A || subcommand == 0xF0 || subcommand == 0xF1)) {
    processQueued(subcommand, _frame + 2, _frame_length - 2);
    return;
  }
  if (subcommand == 0xF8 && _frame_length == 3 && _tx_active &&
      !_tx_quarantined) {
    _tx_success = _frame[2] != 0;
    if (_tx_success)
      ++_packets_sent;
    _tx_active = false;
  } else if (subcommand == 0xF9 &&
             (_mode == Mode::Queued ? _frame_length == 4 : _frame_length >= 4)) {
    if (_rx_length > 0 && !_rx_ready) {
      _last_snr = static_cast<int8_t>(_frame[2]) / 4.0f;
      _last_rssi = static_cast<int8_t>(_frame[3]);
      _rx_local = _frame[2] == 0x80 && _frame[3] == 0x7f;
      if (_rx_local)
        _last_snr = _last_rssi = NAN;
      _rx_ready = true;
    }
  } else if (subcommand == 0x90 && _frame_length >= 4) {
    memcpy(&_noise_floor, _frame + 2, 2);
  } else if (subcommand == 0xF1 && _frame_length >= 3 && _frame[2] == 0x07) {
    _tx_active = false;
    _tx_success = false;
    ++_receive_errors;
  }
}

void RemoteKissRadio::writeHardware(uint8_t subcommand, const uint8_t *payload,
                                    uint16_t length) {
  uint8_t buffer[280];
  if (static_cast<size_t>(length) + 1 > sizeof(buffer))
    return;
  buffer[0] = subcommand;
  if (length > 0)
    memcpy(buffer + 1, payload, length);
  writeFrame(0x06, buffer, length + 1);
}

bool RemoteKissRadio::writeFrame(uint8_t command, const uint8_t *payload,
                                 uint16_t length) {
  uint8_t encoded[2 * MAX_FRAME + 4];
  if (length > MAX_FRAME)
    return false;
  size_t size = 0;
  encoded[size++] = FEND;
  const auto escapedWrite = [&](uint8_t byte) {
    if (byte == FEND) {
      encoded[size++] = FESC;
      encoded[size++] = TFEND;
    } else if (byte == FESC) {
      encoded[size++] = FESC;
      encoded[size++] = TFESC;
    } else {
      encoded[size++] = byte;
    }
  };
  escapedWrite(command);
  for (uint16_t i = 0; i < length; ++i)
    escapedWrite(payload[i]);
  encoded[size++] = FEND;
  return _link.write(encoded, size) == size;
}

bool RemoteKissRadio::queuedReady() const {
  return _mode == Mode::Queued && _join == Join::Ready &&
         _effective_factor == _source_factor;
}

bool RemoteKissRadio::getQueuedRadioStats(mesh::QueuedRadioStats &stats) const {
  if (!queuedReady() || !_stats_valid || _stats.generation != _generation ||
      static_cast<uint32_t>(millis() - _stats.captured_ms) >= STATS_MAX_AGE_MS)
    return false;
  stats = _stats;
  return true;
}

void RemoteKissRadio::failQueued(const char *error) {
  _error = error;
  RemoteKissDiagnostics::record(RemoteKissDiagnostics::Offline, error,
                                static_cast<uint8_t>(_join), _generation);
  _join = Join::Fault;
  _stats_valid = _stats_pending = false;
  for (auto &job : _jobs) {
    if (!job.used || job.terminal)
      continue;
    job.result.state = queued_tx::UNKNOWN;
    job.result.reason = queued_tx::DISCONNECTED;
    job.result.rf_ms =
        0; // No measured result is available, not an RF estimate.
    job.result.has_rf_ms = false;
    job.terminal = job.pending = true;
  }
}

void RemoteKissRadio::onLinkDisconnected() {
  _stats_valid = _stats_pending = false;
  if (_mode == Mode::Queued) {
    bool outstanding = false;
    for (const auto &job : _jobs)
      outstanding |= job.used && !job.terminal;
    if (outstanding)
      failQueued("link disconnected; TX outcome unknown");
    _join = Join::Offline;
    _error = "link disconnected";
  }
  _frame_length = _rx_length = 0;
  _inside_frame = _escaped = _rx_ready = false;
}

bool RemoteKissRadio::control(uint8_t command, const uint8_t *data,
                              uint16_t length) {
  uint8_t buffer[280];
  if (length + 1u > sizeof(buffer))
    return false;
  buffer[0] = command;
  if (length)
    memcpy(buffer + 1, data, length);
  _control_deadline = static_cast<uint32_t>(millis()) + 3000;
  if (writeFrame(6, buffer, length + 1))
    return true;
  failQueued("uncertain control write");
  return false;
}

void RemoteKissRadio::setExpectedPHYPolicy(float factor, bool cad,
                                           int16_t threshold) {
  if (!isfinite(factor) || factor < 0 || _profile_pinned) {
    failQueued("invalid or late expected PHY policy");
    return;
  }
  queued_tx::putFloat(_expected_profile + 11, factor);
  _expected_profile[15] = cad;
  queued_tx::put16(_expected_profile + 16, static_cast<uint16_t>(threshold));
}

bool RemoteKissRadio::setQueuedSourcePolicy(float factor) {
  if (!isfinite(factor) || factor < 0) {
    failQueued("invalid source airtime factor");
    return false;
  }
  _source_factor = factor;
  return true;
}

void RemoteKissRadio::requestSource() {
  _stats_valid = false;
  uint8_t data[9] = {queued_tx::VERSION};
  queued_tx::put32(data + 1, _generation);
  _requested_factor = _source_factor;
  queued_tx::putFloat(data + 5, _requested_factor);
  _join = Join::Source;
  control(queued_tx::SOURCE_POLICY, data, sizeof(data));
}

void RemoteKissRadio::requestStats() {
  const uint8_t version = queued_tx::VERSION;
  _stats_pending = true;
  control(queued_tx::STATS, &version, 1);
}

void RemoteKissRadio::serviceQueued() {
  const uint32_t now = millis();
  if (_join == Join::Ready && !_stats_pending && _source_factor != _effective_factor)
    requestSource();
  if ((_stats_pending || (_join != Join::Ready && _join != Join::Offline &&
                           _join != Join::Fault)) &&
      static_cast<int32_t>(now - _control_deadline) >= 0)
    failQueued("control deadline; reconnect required");
  for (const auto &job : _jobs) {
    if (!job.used || job.terminal)
      continue;
    if (!job.accepted &&
        static_cast<int32_t>(now - job.admitted_deadline) >= 0) {
      failQueued("TX admission deadline; no replay");
      break;
    }
  }
  if (_join == Join::Ready && !_stats_pending &&
      static_cast<int32_t>(now - _next_stats) >= 0)
    requestStats();
}

bool RemoteKissRadio::queueTransmit(const uint8_t *bytes, int length,
                                    uint8_t priority, uint32_t delay_ms,
                                    uint32_t expiry_ms, uint32_t &id) {
  if (!queuedReady() || length < 1 || length > 255 ||
      delay_ms > queued_tx::MAX_DELAY_MS ||
      expiry_ms > queued_tx::MAX_DELAY_MS || _next_job == UINT32_MAX) {
    RemoteKissDiagnostics::record(RemoteKissDiagnostics::Admission,
                                  "Remote KISS TX not admitted: offline or invalid request",
                                  static_cast<uint8_t>(_join), length, _next_job);
    return false;
  }
  for (auto &job : _jobs) {
    if (job.used)
      continue;
    job = {};
    job.used = true;
    job.result.job = id = ++_next_job;
    job.admitted_deadline = static_cast<uint32_t>(millis()) + 3000;
    uint8_t data[274] = {queued_tx::SUBMIT, queued_tx::VERSION};
    queued_tx::put32(data + 2, _generation);
    queued_tx::put32(data + 6, id);
    data[10] = priority;
    queued_tx::put32(data + 11, delay_ms);
    queued_tx::put32(data + 15, expiry_ms);
    memcpy(data + 19, bytes, length);
    if (!writeFrame(6, data, 19 + length))
      failQueued("uncertain TX write; no replay");
    return true; // Even uncertain writes own a correlated terminal outcome.
  }
  RemoteKissDiagnostics::record(RemoteKissDiagnostics::Admission,
                                "Remote KISS TX not admitted: correlation queue full");
  return false;
}

bool RemoteKissRadio::pollQueuedResult(mesh::QueuedTransmitResult &result) {
  for (auto &job : _jobs) {
    if (job.used && job.accepted_pending) {
      result = job.admission;
      job.accepted_pending = false;
      return true;
    }
    if (!job.used || !job.pending)
      continue;
    result = job.result;
    job.pending = false;
    if (job.terminal)
      job = {};
    return true;
  }
  return false;
}

void RemoteKissRadio::processQueued(uint8_t command, const uint8_t *data,
                                    uint16_t length) {
  if (command == queued_tx::EVENT) {
    if (length != 23 || data[0] != queued_tx::VERSION) {
      failQueued("malformed TX lifecycle");
      return;
    }
    if (queued_tx::get32(data + 1) != _generation)
      return;
    for (auto &job : _jobs) {
      if (!job.used || job.terminal ||
          job.result.job != queued_tx::get32(data + 5))
        continue;
      if (data[9] > queued_tx::UNKNOWN ||
          data[10] > queued_tx::NOT_CONFIGURED) {
        failQueued("invalid TX lifecycle state");
        return;
      }
      job.result.state = data[9];
      job.result.reason = data[10];
      job.result.queue_ms = queued_tx::get32(data + 11);
      job.result.rf_ms = queued_tx::get32(data + 15);
      job.result.has_rf_ms = data[9] != queued_tx::ACCEPTED;
      job.result.estimated_ms = queued_tx::get32(data + 19);
      if (data[9] == queued_tx::ACCEPTED) {
        if (!job.accepted) {
          job.accepted_pending = true;
          job.admission = job.result;
        }
        job.accepted = true;
        return;
      }
      job.terminal = data[9] != queued_tx::ACCEPTED;
      job.pending = true;
      if (data[9] == queued_tx::SUCCEEDED)
        ++_packets_sent;
      else
        RemoteKissDiagnostics::record(RemoteKissDiagnostics::Terminal,
                                      "Remote KISS TX terminal failure/unknown",
                                      job.result.job, (uint32_t(data[9]) << 8) | data[10],
                                      job.result.rf_ms);
      if (data[10] == queued_tx::STALE || data[10] == queued_tx::NOT_CONFIGURED)
        failQueued("physical profile/generation invalidated");
      return;
    }
    return;
  }
  if (_join == Join::Offline || _join == Join::Fault)
    return;
  const uint8_t get_config[] = {queued_tx::VERSION, 0};
  if (command == 0xA4 && _stats_pending) {
    if (length != 39 || data[0] != queued_tx::VERSION ||
        queued_tx::get32(data + 1) != _config_generation ||
        queued_tx::get32(data + 5) > queued_tx::WINDOW_MS ||
        queued_tx::get32(data + 21) > queued_tx::WINDOW_MS ||
        data[37] > _queue_capacity || data[38] > 1) {
      failQueued("invalid or changed physical statistics; reconnect required");
      return;
    }
    mesh::QueuedRadioStats stats;
    stats.generation = _generation;
    stats.configuration_generation = queued_tx::get32(data + 1);
    stats.captured_ms = millis();
    stats.aggregate_credit_ms = queued_tx::get32(data + 5);
    stats.aggregate_rf_ms = queued_tx::get32(data + 9);
    stats.aggregate_successes = queued_tx::get32(data + 13);
    stats.aggregate_failures = queued_tx::get32(data + 17);
    stats.source_credit_ms = queued_tx::get32(data + 21);
    stats.source_rf_ms = queued_tx::get32(data + 25);
    stats.source_successes = queued_tx::get32(data + 29);
    stats.source_failures = queued_tx::get32(data + 33);
    stats.aggregate_queued = data[37];
    stats.aggregate_transmitting = data[38] != 0;
    _stats = stats;
    _stats_valid = true;
    _stats_pending = false;
    _next_stats = stats.captured_ms + STATS_INTERVAL_MS;
    _join = Join::Ready;
    _error = nullptr;
  } else if (command == 0xA0 && _join == Join::Hello) {
    if (length != 12 || data[0] != queued_tx::VERSION ||
        data[1] != queued_tx::NONE || !queued_tx::get32(data + 2) ||
        !data[10]) {
      failQueued("queued PHY negotiation rejected");
      return;
    }
    _generation = queued_tx::get32(data + 2);
    _queue_capacity = data[10];
    _join = Join::Profile;
    control(queued_tx::CONFIG, get_config, sizeof(get_config));
  } else if (command == 0xA2 &&
             (_join == Join::Profile || _join == Join::Verify)) {
    if (length != 24 || data[0] != queued_tx::VERSION ||
        data[1] != queued_tx::NONE) {
      failQueued("physical profile unavailable");
      return;
    }
    const uint8_t *profile = data + 6;
    if ((_profile_pinned &&
         memcmp(profile, _expected_profile, sizeof(_expected_profile))) ||
        (!_profile_pinned &&
         ((_requested_frequency_mhz &&
           (static_cast<float>(queued_tx::get32(profile) / 1000000.0) !=
                _requested_frequency_mhz ||
            queued_tx::get32(profile + 4) != _bandwidth_hz ||
            profile[8] != _sf || profile[9] != _cr)) ||
          (_power_set && profile[10] != static_cast<uint8_t>(_tx_power)) ||
          memcmp(profile + 11, _expected_profile + 11, 7)))) {
      failQueued("committed PHY profile differs; join did not retune");
      return;
    }
    if (_join == Join::Verify) {
      if (_config_generation != queued_tx::get32(data + 2)) {
        failQueued("profile changed during airtime capture");
        return;
      }
      _join = Join::Statistics;
      requestStats();
      return;
    }
    memcpy(_expected_profile, profile, sizeof(_expected_profile));
    _profile_pinned = true;
    _config_generation = queued_tx::get32(data + 2);
    _frequency_hz = queued_tx::get32(profile);
    _bandwidth_hz = queued_tx::get32(profile + 4);
    _sf = profile[8];
    _cr = profile[9];
    _tx_power = static_cast<int8_t>(profile[10]);
    // Negotiated v1 guarantees this exact-match setter is a no-op, and only
    // acknowledges it for a durable, non-quarantined profile. CONFIG GET alone
    // cannot distinguish fresh compiled defaults from committed settings.
    _join = Join::CommitCheck;
    control(0x09, _expected_profile, 10);
  } else if (command == 0xF0 && _join == Join::CommitCheck) {
    if (length != 0) {
      failQueued("malformed committed-profile confirmation");
      return;
    }
    requestSource();
  } else if (command == 0xA3 && _join == Join::Source) {
    uint8_t expected[4];
    queued_tx::putFloat(expected, _requested_factor);
    if (length != 6 || data[0] != queued_tx::VERSION ||
        data[1] != queued_tx::NONE || memcmp(data + 2, expected, 4)) {
      failQueued("source policy readback mismatch");
      return;
    }
    _effective_factor = _requested_factor;
    if (_airtime_index == 256) {
      _join = Join::Statistics;
      requestStats();
    } else {
      const uint8_t enable = 1;
      _join = Join::Metadata;
      control(0x19, &enable, 1);
    }
  } else if (command == 0x9A && _join == Join::Metadata) {
    if (length != 1 || data[0] != 1) {
      failQueued("malformed metadata acknowledgement");
      return;
    }
    _join = Join::Noise;
    control(0x10, nullptr, 0);
  } else if (command == 0x90 && _join == Join::Noise) {
    if (length != 2) {
      failQueued("malformed noise floor response");
      return;
    }
    _noise_floor = static_cast<int16_t>(queued_tx::get16(data));
    const uint8_t packet_length = 0;
    _join = Join::Airtime;
    control(0x0f, &packet_length, 1);
  } else if (command == 0x8f && _join == Join::Airtime) {
    if (length != 4 || (_airtime_index && queued_tx::get32(data) <
                                              _airtime[_airtime_index - 1])) {
      failQueued("malformed native airtime response");
      return;
    }
    _airtime[_airtime_index++] = queued_tx::get32(data);
    if (_airtime_index == 256) {
      _join = Join::Verify;
      control(queued_tx::CONFIG, get_config, sizeof(get_config));
    } else {
      const uint8_t packet_length = _airtime_index;
      control(0x0f, &packet_length, 1);
    }
  } else if (command == 0xF1) {
    failQueued("physical control rejected; queued mode requires support");
  }
}

namespace {
class DeviceEntropy : public mesh::RNG {
public:
  void random(uint8_t *data, size_t length) override {
#if defined(ARDUINO_ARCH_ESP32)
    esp_fill_random(data, length);
#elif defined(NRF52_PLATFORM)
    uint8_t enabled = 0;
    if (sd_softdevice_is_enabled(&enabled) != NRF_SUCCESS)
      abort();
    const uint32_t start = millis();
    if (enabled) {
      while (length) {
        const uint8_t count = length > 16 ? 16 : length;
        if (sd_rand_application_vector_get(data, count) == NRF_SUCCESS) {
          data += count;
          length -= count;
        } else if (static_cast<uint32_t>(millis()) - start > 2000) {
          RemoteKissDiagnostics::record(RemoteKissDiagnostics::Entropy, "Hardware entropy unavailable");
          abort();
        }
      }
    } else {
      NRF_RNG->CONFIG = 1;
      NRF_RNG->EVENTS_VALRDY = 0;
      NRF_RNG->TASKS_START = 1;
      while (length) {
        if (NRF_RNG->EVENTS_VALRDY) {
          *data++ = NRF_RNG->VALUE;
          --length;
          NRF_RNG->EVENTS_VALRDY = 0;
        } else if (static_cast<uint32_t>(millis()) - start > 2000) {
          NRF_RNG->TASKS_STOP = 1;
          RemoteKissDiagnostics::record(RemoteKissDiagnostics::Entropy, "Hardware entropy unavailable");
          abort();
        }
      }
      NRF_RNG->TASKS_STOP = 1;
    }
#else
    (void)data;
    (void)length;
    abort(); // No production entropy fallback on unsupported platforms.
#endif
  }
};
} // namespace

uint32_t RemoteKissRadio::getRngSeed() const {
  uint32_t seed;
  DeviceEntropy entropy;
  entropy.random(reinterpret_cast<uint8_t *>(&seed), sizeof(seed));
  return seed;
}

mesh::LocalIdentity RemoteKissRadio::newIdentity() {
  DeviceEntropy entropy;
  return mesh::LocalIdentity(&entropy);
}
