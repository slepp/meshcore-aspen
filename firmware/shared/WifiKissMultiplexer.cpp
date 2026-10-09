#include "WifiKissMultiplexer.h"

#include <errno.h>
#include <math.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <nvs.h>
#include <string.h>
#include <sys/socket.h>

#ifdef ARDUINO_ARCH_ESP32
#include <lwip/opt.h>
static_assert(LWIP_TCP_KEEPALIVE == 1,
              "MKISS requires lwIP TCP keepalive socket options");
#endif

namespace {
constexpr char PROFILE_NAMESPACE[] = "mesh-phy";
constexpr char PROFILE_KEY[] = "profile";
constexpr uint8_t RECORD_PREFIX[] = {'M', 'C', 'P', 1};
constexpr size_t RECORD_SIZE = sizeof(RECORD_PREFIX) + queued_tx::PROFILE_SIZE;
constexpr char CONTROLS_KEY[] = "controls";
constexpr uint8_t CONTROLS_PREFIX[] = {'M', 'C', 'C', 1};
constexpr size_t CONTROLS_SIZE = 8;

bool restoreControls(uint16_t &agcSeconds, bool &rxConfigured, bool &rxBoost) {
  nvs_handle_t handle;
  esp_err_t error = nvs_open(PROFILE_NAMESPACE, NVS_READONLY, &handle);
  if (error == ESP_ERR_NVS_NOT_FOUND) return true;
  if (error != ESP_OK) {
    Serial.printf("Shared-radio controls storage open failed: %s\n", esp_err_to_name(error));
    return false;
  }
  uint8_t record[CONTROLS_SIZE];
  size_t size = sizeof(record);
  error = nvs_get_blob(handle, CONTROLS_KEY, record, &size);
  nvs_close(handle);
  if (error == ESP_ERR_NVS_NOT_FOUND) return true;
  if (error != ESP_OK || size != sizeof(record) ||
      memcmp(record, CONTROLS_PREFIX, sizeof(CONTROLS_PREFIX)) ||
      queued_tx::get16(record + 4) > 1020 || record[6] > 1 || record[7] > 1) {
    Serial.println("Shared-radio controls record unreadable or invalid; radio startup refused");
    return false;
  }
  agcSeconds = queued_tx::get16(record + 4);
  rxConfigured = record[6];
  rxBoost = record[7];
  return true;
}

bool persistControls(uint16_t agcSeconds, bool rxConfigured, bool rxBoost) {
  uint8_t record[CONTROLS_SIZE], actual[CONTROLS_SIZE];
  memcpy(record, CONTROLS_PREFIX, sizeof(CONTROLS_PREFIX));
  queued_tx::put16(record + 4, agcSeconds);
  record[6] = rxConfigured;
  record[7] = rxBoost;
  nvs_handle_t handle;
  esp_err_t error = nvs_open(PROFILE_NAMESPACE, NVS_READWRITE, &handle);
  if (error != ESP_OK) {
    Serial.printf("Shared-radio controls storage open failed: %s\n", esp_err_to_name(error));
    return false;
  }
  error = nvs_set_blob(handle, CONTROLS_KEY, record, sizeof(record));
  if (error == ESP_OK) error = nvs_commit(handle);
  size_t size = sizeof(actual);
  if (error == ESP_OK) error = nvs_get_blob(handle, CONTROLS_KEY, actual, &size);
  const bool verified = error == ESP_OK && size == sizeof(actual) &&
                        !memcmp(record, actual, sizeof(record));
  nvs_close(handle);
  if (!verified)
    Serial.println("Shared-radio controls commit/readback failed; saved state may differ; radio disabled");
  return verified;
}

bool enableSessionKeepalive(int fd, uint8_t slot) {
  constexpr int idle = 45, interval = 10, probes = 3, enabled = 1;
  const struct {
    int level, option, value;
    const char* name;
  } options[] = {
      {IPPROTO_TCP, TCP_KEEPIDLE, idle, "TCP_KEEPIDLE"},
      {IPPROTO_TCP, TCP_KEEPINTVL, interval, "TCP_KEEPINTVL"},
      {IPPROTO_TCP, TCP_KEEPCNT, probes, "TCP_KEEPCNT"},
      {SOL_SOCKET, SO_KEEPALIVE, enabled, "SO_KEEPALIVE"},
  };
  for (const auto& setting : options) {
    if (setsockopt(fd, setting.level, setting.option, &setting.value,
                   sizeof(setting.value)) != 0) {
      Serial.printf("KISS client %u aggregate %s failed (errno %d); "
                    "disconnecting\n", slot, setting.name, errno);
      return false;
    }
  }
  return true;
}

bool validProfile(const uint8_t *profile) {
  using namespace queued_tx;
  return get32(profile) && get32(profile + 4) && profile[8] >= 5 &&
         profile[8] <= 12 && profile[9] >= 5 && profile[9] <= 8 &&
         profile[10] <= 30 && isfinite(getFloat(profile + 11)) &&
         getFloat(profile + 11) >= 0 && profile[15] <= 1;
}

bool restoreProfile(uint8_t *profile, bool &committed) {
  nvs_handle_t handle;
  esp_err_t error = nvs_open(PROFILE_NAMESPACE, NVS_READONLY, &handle);
  if (error == ESP_ERR_NVS_NOT_FOUND) {
    committed = false;
    return true;
  }
  if (error != ESP_OK) {
    Serial.printf("PHY profile storage open failed: %s\n",
                  esp_err_to_name(error));
    return false;
  }
  uint8_t record[RECORD_SIZE];
  size_t length = sizeof(record);
  error = nvs_get_blob(handle, PROFILE_KEY, record, &length);
  nvs_close(handle);
  if (error != ESP_OK) {
    Serial.printf("PHY profile storage read failed: %s\n",
                  esp_err_to_name(error));
    return false;
  }
  if (length != sizeof(record) ||
      memcmp(record, RECORD_PREFIX, sizeof(RECORD_PREFIX)) != 0 ||
      !validProfile(record + sizeof(RECORD_PREFIX))) {
    Serial.println(
        "PHY profile record is invalid or has an unsupported version");
    return false;
  }
  memcpy(profile, record + sizeof(RECORD_PREFIX), queued_tx::PROFILE_SIZE);
  committed = true;
  return true;
}

bool persistProfile(const uint8_t *profile) {
  uint8_t record[RECORD_SIZE];
  memcpy(record, RECORD_PREFIX, sizeof(RECORD_PREFIX));
  memcpy(record + sizeof(RECORD_PREFIX), profile, queued_tx::PROFILE_SIZE);
  nvs_handle_t handle;
  esp_err_t error = nvs_open(PROFILE_NAMESPACE, NVS_READWRITE, &handle);
  if (error != ESP_OK) {
    Serial.printf("PHY profile storage open failed: %s\n",
                  esp_err_to_name(error));
    return false;
  }
  error = nvs_set_blob(handle, PROFILE_KEY, record, sizeof(record));
  if (error == ESP_OK)
    error = nvs_commit(handle);
  nvs_close(handle);
  if (error != ESP_OK) {
    Serial.printf("PHY profile commit failed; TX disabled: %s\n",
                  esp_err_to_name(error));
    return false;
  }
  return true;
}
} // namespace

WifiKissMultiplexer::WifiKissMultiplexer()
    : _queue_head(0), _queue_tail(0), _queue_count(0), _next_generation(1),
      _poll_cursor(0), _active_valid(false), _active_waits_for_response(false),
      _active_is_data(false), _active_input_consumed(false),
      _active_input_offset(0), _output_collecting(false), _output_length(0) {
  _profile[15] = 1;
  for (auto &client : _clients) {
    client.generation = 0;
    client.active = false;
    client.signal_report = true;
    client.collecting = false;
    client.input_length = 0;
    client.output_head = 0;
    client.output_length = 0;
  }
}

bool WifiKissMultiplexer::setNativeRolePresence(uint8_t mask,
                                                const uint8_t* keys,
                                                uint8_t count) {
  if (mask >= (1u << queued_tx::ROLE_COUNT) || count > MAX_NATIVE_KEYS ||
      (count && !keys)) {
    Serial.println("KISS native role presence invalid");
    return false;
  }
  if (count)
    memcpy(_native_keys, keys, size_t(count) * queued_tx::ROLE_KEY_SIZE);
  _native_key_count = count;
  _native_role_mask = mask;
  _active_role_mask = mask;
  return true;
}

void WifiKissMultiplexer::setActiveRolePresence(uint8_t mask) {
  _active_role_mask = mask & _native_role_mask;
}

WifiKissMultiplexer::SourceState& WifiKissMultiplexer::sourceState(uint8_t slot) {
  if (slot == ENGINE_SLOT) return _engine_source;
  if (sessionPort(slot)) return _session_ports[slot - SESSION_BASE];
#if KISS_STREAM_ENDPOINT
  if (slot == STREAM_SLOT) return _stream;
#endif
#if KISS_LOCAL_SOURCES > 0
  if (slot >= KISS_MAX_TCP_CLIENTS) return _locals[slot - KISS_MAX_TCP_CLIENTS];
#endif
  return _clients[slot];
}

const WifiKissMultiplexer::SourceState&
WifiKissMultiplexer::sourceState(uint8_t slot) const {
  if (slot == ENGINE_SLOT) return _engine_source;
  if (sessionPort(slot)) return _session_ports[slot - SESSION_BASE];
#if KISS_STREAM_ENDPOINT
  if (slot == STREAM_SLOT) return _stream;
#endif
#if KISS_LOCAL_SOURCES > 0
  if (slot >= KISS_MAX_TCP_CLIENTS) return _locals[slot - KISS_MAX_TCP_CLIENTS];
#endif
  return _clients[slot];
}

WifiKissMultiplexer::WireState& WifiKissMultiplexer::wireState(uint8_t slot) {
  if (sessionPort(slot)) return _session_ports[slot - SESSION_BASE];
#if KISS_STREAM_ENDPOINT
  if (slot == STREAM_SLOT) return _stream;
#endif
  return _clients[slot];
}

bool WifiKissMultiplexer::attachStream(Stream& stream) {
#if KISS_STREAM_ENDPOINT
  if (_stream_io) {
    Serial.println("KISS stream already attached");
    return false;
  }
  _stream_io = &stream;
  _stream.active = true;
  _stream.signal_report = true;
  _stream.source_factor = 1;
  _stream.source_credit = queued_tx::WINDOW_MS / 2;
  _stream.source_updated = millis();
  return true;
#else
  (void)stream;
  Serial.println("KISS stream endpoint disabled");
  return false;
#endif
}

void WifiKissMultiplexer::detachStream() {
#if KISS_STREAM_ENDPOINT
  if (!_stream_io) return;
  removeClient(STREAM_SLOT);
  _stream_io = nullptr;
  _stream = {};
#endif
}

bool WifiKissMultiplexer::streamFaulted() const {
#if KISS_STREAM_ENDPOINT
  return _stream_io && !_stream.active;
#else
  return false;
#endif
}

uint32_t WifiKissMultiplexer::streamOutputOverflows() const {
#if KISS_STREAM_ENDPOINT
  return _stream_overflows;
#else
  return 0;
#endif
}

#if KISS_STREAM_ENDPOINT
void WifiKissMultiplexer::faultStream() {
  ++_stream_overflows;
  Serial.println("KISS stream output overflow; fresh HELLO required");
  removeClient(STREAM_SLOT);
}
#endif

void WifiKissMultiplexer::pollStream() {
#if KISS_STREAM_ENDPOINT
  if (!_stream_io) return;
  drainClientOutput(STREAM_SLOT);
  for (uint16_t i = 0; i < CLIENT_INPUT_BUDGET && _stream_io->available() > 0; ++i) {
    const int byte = _stream_io->read();
    if (byte < 0) break;
    processClientByte(STREAM_SLOT, static_cast<uint8_t>(byte));
  }
  drainClientOutput(STREAM_SLOT);
#endif
}

int WifiKissMultiplexer::attachLocal(KissLocalSource& sink) {
#if KISS_LOCAL_SOURCES > 0
  for (uint8_t i = 0; i < KISS_LOCAL_SOURCES; ++i) {
    auto& local = _locals[i];
    if (local.active) continue;
    local = {};
    local.active = true;
    local.sink = &sink;
    local.generation = _next_generation++;
    if (!local.generation) local.generation = _next_generation++;
    local.source_factor = 1;
    local.source_credit = queued_tx::WINDOW_MS / 2;
    local.source_updated = millis();
    return KISS_MAX_TCP_CLIENTS + i;
  }
#else
  (void)sink;
#endif
  return -1;
}

bool WifiKissMultiplexer::beginEngineSource(float factor) {
  if (_engine_source.active || !isfinite(factor) || factor < 0) {
    Serial.println("Packet engine source already active or airtime factor invalid");
    return false;
  }
  _engine_source = {};
  _engine_source.active = true;
  _engine_source.generation = newGeneration();
  _engine_source.source_factor = factor;
  _engine_source.source_credit = queued_tx::WINDOW_MS * (1.0f / (1.0f + factor));
  _engine_source.source_updated = millis();
  _engine_job = 0;
  return true;
}

void WifiKissMultiplexer::stopEngineSource() {
  _engine_source.active = false;
  for (auto &job : _jobs) {
    if (!job.used || job.source.slot != ENGINE_SLOT) continue;
    job.used = false;
    notify(job, queued_tx::FAILED, queued_tx::DISCONNECTED, millis() - job.admitted);
  }
}

bool WifiKissMultiplexer::admitEnginePackets(const packet_engine::Emission* packets,
                                            uint8_t count) {
  if (!packets || !count || count > packet_engine::EmissionLimit ||
      !_engine_source.active || !localReady() ||
      _engine_job > UINT32_MAX - count) return false;
  unsigned free = 0;
  for (const auto &job : _jobs) if (!job.used) ++free;
  if (free < count) return false;
  for (unsigned i = 0; i < count; ++i) {
    const auto &packet = packets[i];
    if (!packet.length || packet.length > KISS_MAX_PACKET_SIZE ||
        packet.delayMs > queued_tx::MAX_DELAY_MS ||
        packet.expiryMs > queued_tx::MAX_DELAY_MS) return false;
  }
  const uint32_t now = millis();
  unsigned next = 0;
  for (auto &job : _jobs) {
    if (job.used) continue;
    const auto &packet = packets[next++];
    job = {};
    job.used = job.extended = job.engineOrigin = true;
    job.source = {ENGINE_SLOT, _engine_source.generation};
    job.id = ++_engine_job;
    job.sequence = ++_sequence;
    job.admitted = now;
    job.eligible = now + packet.delayMs;
    job.expiry = now + packet.expiryMs;
    job.expiry_delay = packet.expiryMs;
    job.priority = packet.priority;
    job.length = packet.length;
    memcpy(job.packet, packet.bytes, packet.length);
    if (next == count) break;
  }
  return true;
}

void WifiKissMultiplexer::detachLocal(uint8_t slot) {
#if KISS_LOCAL_SOURCES > 0
  if (slot < KISS_MAX_TCP_CLIENTS || slot >= KISS_MAX_TCP_CLIENTS + KISS_LOCAL_SOURCES) return;
  auto& local = _locals[slot - KISS_MAX_TCP_CLIENTS];
  local.active = false;
  local.sink = nullptr;
  for (auto& job : _jobs) if (job.used && job.source.slot == slot) job.used = false;
#else
  (void)slot;
#endif
}

bool WifiKissMultiplexer::setLocalPolicy(uint8_t slot, float factor) {
  if (slot < KISS_MAX_TCP_CLIENTS || slot >= KISS_MAX_TCP_CLIENTS + KISS_LOCAL_SOURCES ||
      !isfinite(factor) || factor < 0) return false;
  auto& source = sourceState(slot);
  if (!source.active) return false;
  refill(millis());
  source.source_factor = factor;
  const uint32_t maximum = queued_tx::WINDOW_MS * (1.0f / (1.0f + factor));
  if (source.source_credit > maximum) source.source_credit = maximum;
  return true;
}

bool WifiKissMultiplexer::submitLocal(uint8_t slot, const uint8_t* packet, uint16_t length,
                                    uint32_t job, uint8_t priority, uint32_t delay,
                                    uint32_t expiry, bool engineOrigin,
                                    bool reflectionOrigin) {
  if (slot < KISS_MAX_TCP_CLIENTS || slot >= KISS_MAX_TCP_CLIENTS + KISS_LOCAL_SOURCES ||
      !sourceState(slot).active || !localReady() || !job || !packet ||
      !length || length > KISS_MAX_PACKET_SIZE ||
      delay > queued_tx::MAX_DELAY_MS || expiry > queued_tx::MAX_DELAY_MS) return false;
  // False means unadmitted; ownership remains with the caller.
  bool room = false;
  for (const auto& entry : _jobs) if (!entry.used) { room = true; break; }
  if (!room) return false;
  submit(slot, packet, length, true, job, priority, delay, expiry,
         engineOrigin, reflectionOrigin);
  return true;
}

void WifiKissMultiplexer::received(const uint8_t* packet, uint16_t length,
                                 float rssi, float snr) {
  if (!packet || !length || length > KISS_MAX_PACKET_SIZE) return;
  uint8_t candidate[KISS_MAX_PACKET_SIZE];
  memcpy(candidate, packet, length);
  receiveRaw(candidate, length, sizeof(candidate), rssi, snr);
}

bool WifiKissMultiplexer::filterPacket(
    packet_engine::Stage stage, uint8_t* packet, uint16_t& length,
    uint16_t capacity, uint8_t source, uint32_t generation, uint32_t job,
    uint8_t destination, bool local, float rssi, float snr, bool engineOrigin,
    bool reflectionOrigin) {
  if (!_packet_pipeline) return true;
  packet_engine::Metadata metadata;
  metadata.stage = stage;
  metadata.source = source;
  metadata.generation = generation;
  metadata.job = job;
  metadata.destination = destination;
  metadata.local = local;
  metadata.engineOrigin = engineOrigin;
  metadata.reflectionOrigin = reflectionOrigin;
  metadata.rssi = isfinite(rssi) ? int16_t(fminf(fmaxf(rssi, -32768.0f), 32767.0f)) : 0;
  metadata.snrQuarterDb = isfinite(snr) ? int16_t(fminf(fmaxf(snr * 4, -32768.0f), 32767.0f)) : 0;
  return _packet_pipeline->process(metadata, packet, length, capacity) !=
         packet_engine::Decision::Drop;
}

bool WifiKissMultiplexer::receiveRaw(uint8_t* packet, uint16_t& length,
                                    uint16_t capacity, float rssi, float snr) {
  static_assert(KISS_MAX_PACKET_SIZE <= packet_engine::Capacity, "packet engine frame capacity");
  if (!packet || !length || length > capacity || capacity > KISS_MAX_PACKET_SIZE) return false;
  if (!filterPacket(packet_engine::Stage::Receive, packet, length, capacity,
                    UINT8_MAX, 0, 0, UINT8_MAX, false, rssi, snr)) return false;
  deliverReceived(packet, length, rssi, snr);
  return true;
}

bool WifiKissMultiplexer::processNativePacket(
    uint8_t slot, packet_engine::Metadata metadata, uint8_t* packet,
    uint16_t& length, uint16_t capacity, packet_engine::Validator validate) {
  if (!_packet_pipeline) return true;
  const auto& source = sourceState(slot);
  metadata.generation = source.generation;
  return _packet_pipeline->process(metadata, packet, length, capacity, validate) !=
         packet_engine::Decision::Drop;
}

void WifiKissMultiplexer::deliverReceived(const uint8_t* packet, uint16_t length,
                                         float rssi, float snr) {
  if (_packet_observer) _packet_observer->packet(packet, length, false, 0, rssi, snr);
#if KISS_LOCAL_SOURCES > 0
  for (uint8_t i = 0; i < KISS_LOCAL_SOURCES; ++i) {
    auto& local = _locals[i];
    if (!local.active) continue;
    uint8_t candidate[KISS_MAX_PACKET_SIZE];
    uint16_t candidateLength = length;
    memcpy(candidate, packet, length);
    if (filterPacket(packet_engine::Stage::LocalDelivery, candidate, candidateLength,
                     sizeof(candidate), UINT8_MAX, 0, 0, KISS_MAX_TCP_CLIENTS + i,
                     false, rssi, snr))
      local.sink->receivedWithOrigin(candidate, candidateLength, rssi, snr,
                                     false, false, false);
  }
#else
  (void)packet; (void)length; (void)rssi; (void)snr;
#endif
}

void WifiKissMultiplexer::poll(WiFiServer &server) {
  for (uint8_t offset = 0; offset < KISS_MAX_TCP_CLIENTS; ++offset) {
    const uint8_t slot = (_poll_cursor + offset) % KISS_MAX_TCP_CLIENTS;
    auto &client = _clients[slot];
    if (!client.active)
      continue;
    if (!isClientConnected(client)) {
      removeClient(slot);
      continue;
    }
    drainClientOutput(slot);
    if (slot == _session_slot) drainSessionOutput();
    if (!client.active)
      continue;
    uint8_t input[CLIENT_INPUT_BUDGET];
    const int count =
        recv(client.socket.fd(), input, sizeof(input), MSG_DONTWAIT);
    if (count == 0 || (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
                       errno != EINTR)) {
      removeClient(slot);
      continue;
    }
    for (int i = 0; i < count && client.active; ++i) {
      processClientByte(slot, input[i]);
    }
    if (slot == _session_slot) drainSessionOutput();
  }
  _poll_cursor = (_poll_cursor + 1) % KISS_MAX_TCP_CLIENTS;

  for (uint8_t accepted = 0; accepted < KISS_MAX_TCP_CLIENTS; ++accepted) {
    WiFiClient client = server.accept();
    if (!client)
      break;
    acceptClient(client);
  }
}

void WifiKissMultiplexer::afterModemLoop() {
  if (_active_valid && _active_input_consumed && !_active_waits_for_response) {
    completeActive();
  }
  serviceTransmit();
}

size_t WifiKissMultiplexer::clientCount() const {
  size_t count = 0;
  for (const auto &client : _clients) {
    if (client.active)
      ++count;
  }
  return count;
}

int WifiKissMultiplexer::available() {
  if (_active_valid && _active_input_offset == 0 &&
      !isTargetConnected(_active.source)) {
    completeActive();
  }
  if (!_active_valid && !loadNextFrame())
    return 0;
  if (_active_input_offset >= _active.encoded_length)
    return 0;
  return _active.encoded_length - _active_input_offset;
}

int WifiKissMultiplexer::read() {
  if (available() <= 0)
    return -1;
  const uint8_t byte = _active.encoded[_active_input_offset++];
  if (_active_input_offset >= _active.encoded_length) {
    _active_input_consumed = true;
  }
  return byte;
}

int WifiKissMultiplexer::peek() {
  if (available() <= 0)
    return -1;
  return _active.encoded[_active_input_offset];
}

void WifiKissMultiplexer::flush() {
  // Reboot calls flush(); never let a stalled TCP peer block the radio loop.
  for (uint8_t slot = 0; slot < KISS_MAX_TCP_CLIENTS; ++slot) {
    drainClientOutput(slot);
  }
  drainSessionOutput();
}

int WifiKissMultiplexer::availableForWrite() { return 1460; }

size_t WifiKissMultiplexer::write(uint8_t byte) {
  processOutputByte(byte);
  return 1;
}

size_t WifiKissMultiplexer::write(const uint8_t *data, size_t size) {
  for (size_t i = 0; i < size; ++i)
    processOutputByte(data[i]);
  return size;
}

uint32_t WifiKissMultiplexer::newGeneration() {
  uint32_t generation = _next_generation++;
  if (!generation) generation = _next_generation++;
  return generation;
}

void WifiKissMultiplexer::disconnectTcpClients() {
  for (uint8_t slot = 0; slot < KISS_MAX_TCP_CLIENTS; ++slot)
    removeClient(slot);
}

void WifiKissMultiplexer::acceptClient(WiFiClient socket) {
  for (uint8_t slot = 0; slot < KISS_MAX_TCP_CLIENTS; ++slot) {
    auto &client = _clients[slot];
    if (client.active)
      continue;
    client.socket = socket;
    client.socket.setNoDelay(true);
    client.generation = newGeneration();
    client.active = true;
    client.signal_report = true;
    client.collecting = false;
    client.input_length = 0;
    client.output_head = 0;
    client.output_length = 0;
    client.negotiated = false;
    client.last_job = 0;
    client.configuration_seen = 0;
    client.has_role_presence = false;
    client.presence_warnings = 0;
    client.source_factor = 1.0f;
    client.source_credit = queued_tx::WINDOW_MS / 2;
    client.source_updated = millis();
    client.source_airtime = 0;
    client.source_successes = 0;
    client.source_failures = 0;
    Serial.printf("KISS client %u connected from %s\n", slot,
                  client.socket.remoteIP().toString().c_str());
    return;
  }
  Serial.println("KISS client rejected: all slots are occupied");
  socket.stop();
}

void WifiKissMultiplexer::removeClient(uint8_t slot) {
  if (_session_slot < KISS_MAX_TCP_CLIENTS && slot == _session_slot) {
    for (uint8_t port = 1; port < queued_tx::SESSION_PORTS; ++port)
      removeClient(sessionSource(port));
    _session_slot = KISS_MAX_TCP_CLIENTS;
  }
  auto &client = wireState(slot);
  if (!client.active)
    return;
  const bool negotiated = client.negotiated;
  if (_owner.slot == slot && _owner.generation == client.generation)
    _owner = {};
  if (!sessionPort(slot)
#if KISS_STREAM_ENDPOINT
      && slot != STREAM_SLOT
#endif
  )
    _clients[slot].socket.stop();
  client.active = false;
  client.negotiated = false;
  client.has_role_presence = false;
  client.presence_warnings = 0;
  client.collecting = false;
  client.input_length = 0;
  client.output_head = 0;
  client.output_length = 0;
  for (auto &job : _jobs) {
    if (job.used && job.source.slot == slot) {
      if (_dashboard)
        _dashboard->transmitted(millis(), job.packet, job.length, slot,
                                job.source.generation, job.id,
                                queued_tx::FAILED, queued_tx::DISCONNECTED,
                                millis() - job.admitted, 0, 0);
      job.used = false;
    }
  }
  // Pending work belongs to the connection, not the reusable slot. Keep any
  // already-started transaction until its modem response arrives.
  const uint8_t pending = _queue_count;
  for (uint8_t i = 0; i < pending; ++i) {
    const uint8_t head = _queue_head;
    _queue_head = (_queue_head + 1) % KISS_REQUEST_QUEUE_DEPTH;
    --_queue_count;
    if (_queue[head].source.slot == slot)
      continue;
    if (_queue_tail != head)
      _queue[_queue_tail] = _queue[head];
    _queue_tail = (_queue_tail + 1) % KISS_REQUEST_QUEUE_DEPTH;
    ++_queue_count;
  }
  if (sessionPort(slot)) {
    if (negotiated)
      Serial.printf("KISS session port %u disconnected\n", sessionNumber(slot));
  } else
    Serial.printf("KISS client %u disconnected\n", slot);
}

void WifiKissMultiplexer::drainClientOutput(uint8_t slot) {
  auto &client = wireState(slot);
  for (uint8_t attempt = 0; attempt < 2; ++attempt) {
    if (!client.active || client.output_length == 0)
      return;
    const uint16_t contiguous = CLIENT_OUTPUT_CAPACITY - client.output_head;
    uint16_t length =
        client.output_length < contiguous ? client.output_length : contiguous;
#if KISS_STREAM_ENDPOINT
    if (slot == STREAM_SLOT) {
      const int capacity = _stream_io->availableForWrite();
      if (capacity <= 0) return;
      if (length > capacity) length = capacity;
      const size_t sent = _stream_io->write(client.output + client.output_head, length);
      if (sent > length) {
        faultStream();
        return;
      }
      client.output_head = (client.output_head + sent) % CLIENT_OUTPUT_CAPACITY;
      client.output_length -= sent;
      if (!sent) return;
      continue;
    }
#endif
    int flags = MSG_DONTWAIT;
#ifdef MSG_NOSIGNAL
    flags |= MSG_NOSIGNAL;
#endif
    const int sent = send(_clients[slot].socket.fd(),
                          client.output + client.output_head, length, flags);
    if (sent > 0) {
      client.output_head = (client.output_head + sent) % CLIENT_OUTPUT_CAPACITY;
      client.output_length -= sent;
      continue;
    }
    if (sent == 0 || (errno != EAGAIN && errno != EWOULDBLOCK &&
                      errno != EINTR && errno != ENOBUFS && errno != ENOMEM)) {
      Serial.printf("KISS client %u output failed\n", slot);
      removeClient(slot);
    }
    return;
  }
}

void WifiKissMultiplexer::drainSessionOutput() {
  if (_session_slot >= KISS_MAX_TCP_CLIENTS ||
      !_clients[_session_slot].active)
    return;
  auto &physical = _clients[_session_slot];
  for (uint8_t attempt = 0; attempt < 2 * (queued_tx::SESSION_PORTS - 1);
       ++attempt) {
    drainClientOutput(_session_slot);
    if (_session_slot >= KISS_MAX_TCP_CLIENTS) return;
    WireState *next = nullptr;
    uint16_t length = 0;
    for (uint8_t offset = 0; offset < queued_tx::SESSION_PORTS - 1;
         ++offset) {
      const uint8_t index =
          (_session_cursor + offset) % (queued_tx::SESSION_PORTS - 1);
      auto &port = _session_ports[index];
      if (!port.active || !port.output_length) continue;
      uint16_t frame_length = 1;
      while (frame_length < port.output_length &&
             port.output[(port.output_head + frame_length) %
                         CLIENT_OUTPUT_CAPACITY] != KISS_FEND)
        ++frame_length;
      if (frame_length == port.output_length || frame_length < 2 ||
          port.output[port.output_head] != KISS_FEND) {
        Serial.println("KISS session output frame incomplete; disconnecting");
        removeClient(_session_slot);
        return;
      }
      ++frame_length;
      // Leave a full frame for port-0 control replies even if children stall.
      if (frame_length + MAX_ENCODED_FRAME >
          CLIENT_OUTPUT_CAPACITY - physical.output_length)
        continue;
      next = &port;
      length = frame_length;
      _session_cursor = (index + 1) % (queued_tx::SESSION_PORTS - 1);
      break;
    }
    if (!next) return;
    const uint16_t tail =
        (physical.output_head + physical.output_length) % CLIENT_OUTPUT_CAPACITY;
    for (uint16_t i = 0; i < length; ++i)
      physical.output[(tail + i) % CLIENT_OUTPUT_CAPACITY] =
          next->output[(next->output_head + i) % CLIENT_OUTPUT_CAPACITY];
    physical.output_length += length;
    next->output_head = (next->output_head + length) % CLIENT_OUTPUT_CAPACITY;
    next->output_length -= length;
    drainClientOutput(_session_slot);
    if (_session_slot >= KISS_MAX_TCP_CLIENTS) return;
  }
}

bool WifiKissMultiplexer::isClientConnected(const ClientState &client) const {
  if (!client.active || client.socket.fd() < 0)
    return false;
  uint8_t byte;
  const int result =
      recv(client.socket.fd(), &byte, sizeof(byte), MSG_PEEK | MSG_DONTWAIT);
  if (result > 0)
    return true;
  if (result == 0)
    return false;
  return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
}

bool WifiKissMultiplexer::isTargetConnected(ClientTarget target) const {
  if (target.slot == ENGINE_SLOT)
    return _engine_source.active && _engine_source.generation == target.generation;
  if (sessionPort(target.slot))
    return _session_slot < KISS_MAX_TCP_CLIENTS &&
           _clients[_session_slot].active &&
           _session_ports[target.slot - SESSION_BASE].active &&
           _session_ports[target.slot - SESSION_BASE].generation ==
               target.generation;
#if KISS_STREAM_ENDPOINT
  if (target.slot == STREAM_SLOT)
    return _stream.active && _stream.generation == target.generation;
#endif
#if KISS_LOCAL_SOURCES > 0
  if (target.slot >= KISS_MAX_TCP_CLIENTS &&
      target.slot < KISS_MAX_TCP_CLIENTS + KISS_LOCAL_SOURCES) {
    const auto& local = _locals[target.slot - KISS_MAX_TCP_CLIENTS];
    return local.active && local.generation == target.generation;
  }
#endif
  if (target.slot >= KISS_MAX_TCP_CLIENTS)
    return false;
  const auto &client = _clients[target.slot];
  return client.active && client.generation == target.generation;
}

void WifiKissMultiplexer::processClientByte(uint8_t slot, uint8_t byte) {
  auto &client = wireState(slot);
  if (byte == KISS_FEND) {
    if (client.collecting && client.input_length > 1)
      finishClientFrame(slot);
    if (!client.active
#if KISS_STREAM_ENDPOINT
        && slot != STREAM_SLOT
#endif
    )
      return;
    client.collecting = true;
    client.input_length = 1;
    client.input[0] = KISS_FEND;
    return;
  }
  if (!client.collecting)
    return;
  if (client.input_length >= MAX_ENCODED_FRAME - 1) {
    client.collecting = false;
    client.input_length = 0;
    return;
  }
  client.input[client.input_length++] = byte;
}

void WifiKissMultiplexer::finishClientFrame(uint8_t slot) {
  auto &client = wireState(slot);
  client.input[client.input_length++] = KISS_FEND;
  enqueueFrame(slot, client.input, client.input_length);
}

bool WifiKissMultiplexer::enqueueFrame(uint8_t slot, const uint8_t *encoded,
                                       uint16_t encoded_length) {
  uint8_t decoded[KISS_MAX_FRAME_SIZE];
  const uint16_t decoded_length =
      decodeFrame(encoded, encoded_length, decoded, sizeof(decoded));
  if (decoded_length == 0)
    return false;

  const uint8_t command = decoded[0] & 0x0F;
  uint8_t port = (decoded[0] >> 4) & 0x0F;
  if (_session_slot < KISS_MAX_TCP_CLIENTS &&
      slot == _session_slot && port) {
    if (port >= queued_tx::SESSION_PORTS) {
      Serial.printf("KISS session rejected port %u\n", port);
      return false;
    }
    slot = sessionSource(port);
    const auto &source = wireState(slot);
    if (!source.negotiated &&
        !(command == KISS_CMD_SETHARDWARE && decoded_length >= 2 &&
          (decoded[1] == queued_tx::HELLO ||
           decoded[1] == HW_CMD_SET_SIGNAL_REPORT ||
           decoded[1] == HW_CMD_GET_SIGNAL_REPORT))) {
      const uint8_t error = HW_ERR_INVALID_PARAM;
      sendHardware({slot, source.generation}, HW_RESP_ERROR, &error, 1);
      return false;
    }
    decoded[0] = command;
    port = 0;
  }
  auto &client = wireState(slot);
#if KISS_STREAM_ENDPOINT
  if (slot == STREAM_SLOT && (!client.active || !client.negotiated) &&
      !(port == 0 && command == KISS_CMD_SETHARDWARE &&
        decoded_length >= 2 &&
        (decoded[1] == queued_tx::HELLO ||
         decoded[1] == queued_tx::ROLE_PRESENCE))) {
    const uint8_t error = HW_ERR_INVALID_PARAM;
    sendHardware({slot, client.generation}, HW_RESP_ERROR, &error, 1);
    return false;
  }
#endif
  if (_radio && port == 0) {
    if (command == KISS_CMD_DATA) {
      submit(slot, decoded + 1, decoded_length - 1, false, 0,
             queued_tx::LEGACY_PRIORITY, 0, 0);
      return true;
    }
    if (command == KISS_CMD_SETHARDWARE && decoded_length >= 2 &&
        extension(slot, decoded, decoded_length))
      return true;
    if (command >= 1 && command <= 5) {
      const uint8_t error = HW_ERR_INVALID_PARAM;
      sendHardware({slot, client.generation}, HW_RESP_ERROR, &error, 1);
      return false;
    }
  }
  if (port == 0 && command == KISS_CMD_DATA &&
      (decoded_length <= 1 || decoded_length - 1 > KISS_MAX_PACKET_SIZE)) {
    const uint8_t error = HW_ERR_INVALID_LENGTH;
    sendHardware({slot, client.generation}, HW_RESP_ERROR, &error, 1);
    return false;
  }

  if (port == 0 && command == KISS_CMD_SETHARDWARE && decoded_length >= 2) {
    const ClientTarget target = {slot, client.generation};
    if (decoded[1] == HW_CMD_SET_SIGNAL_REPORT) {
      if (decoded_length < 3) {
        const uint8_t error = HW_ERR_INVALID_LENGTH;
        sendHardware(target, HW_RESP_ERROR, &error, 1);
      } else {
        client.signal_report = decoded[2] != 0;
        const uint8_t enabled = client.signal_report ? 1 : 0;
        sendHardware(target, HW_RESP(HW_CMD_GET_SIGNAL_REPORT), &enabled, 1);
      }
      return true;
    }
    if (decoded[1] == HW_CMD_GET_SIGNAL_REPORT) {
      const uint8_t enabled = client.signal_report ? 1 : 0;
      sendHardware(target, HW_RESP(HW_CMD_GET_SIGNAL_REPORT), &enabled, 1);
      return true;
    }
  }
  if (_queue_count >= KISS_REQUEST_QUEUE_DEPTH) {
    const uint8_t error = HW_ERR_TX_BUSY;
    sendHardware({slot, client.generation}, HW_RESP_ERROR, &error, 1);
    return false;
  }
  auto &queued = _queue[_queue_tail];
  queued.source = {slot, client.generation};
  queued.encoded_length = encoded_length;
  queued.decoded_length = decoded_length;
  memcpy(queued.encoded, encoded, encoded_length);
  if (sessionPort(slot)) queued.encoded[1] = command;
  memcpy(queued.decoded, decoded, decoded_length);
  _queue_tail = (_queue_tail + 1) % KISS_REQUEST_QUEUE_DEPTH;
  ++_queue_count;
  return true;
}

void WifiKissMultiplexer::attachRadio(mesh::Radio &radio, mesh::RNG &rng,
                                      SetRadioCallback configure,
                                      SetTxPowerCallback power,
                                      bool (*setRxBoost)(bool),
                                      bool (*getRxBoost)()) {
  _radio = &radio;
  _rng = &rng;
  _configure = configure;
  _power = power;
  _set_rx_boost = setRxBoost;
  _get_rx_boost = getRxBoost;
  queued_tx::putFloat(_profile + 11, 1.0f);
  _credit_updated = millis();
}

bool WifiKissMultiplexer::applyMastConfiguration(const RadioConfig &config,
                                                bool persist) {
  if (!_radio || !_configure || !_power || !localReady() || _transmitting ||
      _radio->isReceiving() || config.freq_hz < 150000000 ||
      config.freq_hz > 960000000 || config.bw_hz == 0 || config.bw_hz > 500000 ||
      config.sf < 5 || config.sf > 12 || config.cr < 5 || config.cr > 8 ||
      config.tx_power > 22)
    return false;
  uint8_t profile[queued_tx::PROFILE_SIZE];
  memcpy(profile, _profile, sizeof(profile));
  queued_tx::put32(profile, config.freq_hz);
  queued_tx::put32(profile + 4, config.bw_hz);
  profile[8] = config.sf; profile[9] = config.cr; profile[10] = config.tx_power;
  return applyMastProfile(profile, persist);
}

bool WifiKissMultiplexer::restoreMastConfiguration() {
  return hasPersistedConfiguration() && applyMastProfile(_persisted_profile, false);
}

bool WifiKissMultiplexer::applyMastCAD(bool enabled) {
  uint8_t profile[queued_tx::PROFILE_SIZE];
  memcpy(profile, _profile, sizeof(profile));
  profile[15] = enabled;
  return applyMastProfile(profile, true);
}

bool WifiKissMultiplexer::applyMastInterference(uint8_t threshold) {
  uint8_t profile[queued_tx::PROFILE_SIZE];
  memcpy(profile, _profile, sizeof(profile));
  queued_tx::put16(profile + 16, threshold);
  return applyMastProfile(profile, true);
}

bool WifiKissMultiplexer::applyMastAirtimeFactor(float factor) {
  if (!isfinite(factor) || factor < 0 || factor > 9) return false;
  uint8_t profile[queued_tx::PROFILE_SIZE];
  memcpy(profile, _profile, sizeof(profile));
  queued_tx::putFloat(profile + 11, factor);
  return applyMastProfile(profile, true);
}

bool WifiKissMultiplexer::applyMastControls(uint16_t agcSeconds, bool configureRxBoost,
                                           bool rxBoost) {
  if (!localReady() || _transmitting || _radio->isReceiving() || agcSeconds > 1020 ||
      (configureRxBoost && !rxBoostAvailable())) return false;
  if (!persistControls(agcSeconds, configureRxBoost, rxBoost)) {
    configurationFailed();
    return false;
  }
  _agc_seconds = agcSeconds;
  _rx_boost_configured = configureRxBoost;
  _rx_boost = rxBoost;
  if (configureRxBoost &&
      (!_set_rx_boost(rxBoost) || _get_rx_boost() != rxBoost)) {
    Serial.println("Saved RX boost could not be applied/read back; radio disabled");
    configurationFailed();
    return false;
  }
  uint8_t profile[queued_tx::PROFILE_SIZE];
  memcpy(profile, _profile, sizeof(profile));
  return applyMastProfile(profile, false);
}

void WifiKissMultiplexer::configurationFailed() {
  _configuration_fault = true;
  for (auto &job : _jobs)
    if (job.used) {
      notify(job, queued_tx::FAILED, queued_tx::NOT_CONFIGURED,
             uint32_t(millis() - job.admitted));
      job.used = false;
    }
}

bool WifiKissMultiplexer::applyMastProfile(const uint8_t *profile, bool persist) {
  if (!_radio || !_configure || !_power || !localReady() || _transmitting || _radio->isReceiving())
    return false;
  if (persist && !persistProfile(profile)) {
    configurationFailed();
    return false;
  }
  if (persist) memcpy(_persisted_profile, profile, queued_tx::PROFILE_SIZE);
  for (auto &job : _jobs)
    if (job.used) {
      notify(job, queued_tx::FAILED, queued_tx::STALE,
             uint32_t(millis() - job.admitted));
      job.used = false;
    }
  refill(millis());
  _configure(queued_tx::get32(profile) / 1000000.0, queued_tx::get32(profile + 4) / 1000.0,
             profile[8], profile[9]);
  _power(profile[10]);
  memcpy(_profile, profile, queued_tx::PROFILE_SIZE);
  const uint32_t maximum = static_cast<uint32_t>(
      queued_tx::WINDOW_MS * (1.0f / (1.0f + queued_tx::getFloat(_profile + 11))));
  if (_credit > maximum) _credit = maximum;
  _radio->setCADEnabled(_profile[15] != 0);
  _radio->triggerNoiseFloorCalibrate(static_cast<int16_t>(queued_tx::get16(_profile + 16)));
  if (++_configuration_generation == 0) ++_configuration_generation;
  _profile_committed = memcmp(profile, _persisted_profile, queued_tx::PROFILE_SIZE) == 0;
  _carrier_wait = false;
  return true;
}

bool WifiKissMultiplexer::setInitialConfiguration(const RadioConfig &config,
                                                 bool require_operator_phy) {
  configurationFailed();
  queued_tx::put32(_profile, config.freq_hz);
  queued_tx::put32(_profile + 4, config.bw_hz);
  _profile[8] = config.sf;
  _profile[9] = config.cr;
  _profile[10] = config.tx_power;
  if (require_operator_phy && !validProfile(_profile)) {
    Serial.println("Operator PHY configuration is invalid; startup refused");
    return false;
  }
  uint8_t expected_phy[11];
  memcpy(expected_phy, _profile, sizeof(expected_phy));
  if (!restoreProfile(_profile, _profile_committed))
    return false;
  if (!restoreControls(_agc_seconds, _rx_boost_configured, _rx_boost))
    return false;
  if (require_operator_phy) {
#if !defined(MESHCORE_MAST_ADMIN) || !MESHCORE_MAST_ADMIN
    if (memcmp(expected_phy, _profile, sizeof(expected_phy)) != 0) {
      Serial.println("Saved PHY conflicts with operator configuration; startup refused");
      return false;
    }
#endif
    if (!_profile_committed) {
      if (!persistProfile(_profile)) return false;
      _profile_committed = true;
      Serial.println("Explicit operator PHY provisioned in NVS");
    }
  }
  if (_profile_committed) memcpy(_persisted_profile, _profile, sizeof(_profile));
  else memset(_persisted_profile, 0, sizeof(_persisted_profile));
  _configure(queued_tx::get32(_profile) / 1000000.0,
             queued_tx::get32(_profile + 4) / 1000.0, _profile[8],
             _profile[9]);
  _power(_profile[10]);
  if (_rx_boost_configured &&
      (!rxBoostAvailable() || !_set_rx_boost(_rx_boost) || _get_rx_boost() != _rx_boost)) {
    Serial.println("Saved shared-radio RX boost unavailable; radio startup refused");
    return false;
  }
  _radio->setCADEnabled(_profile[15] != 0);
  _radio->triggerNoiseFloorCalibrate(
      static_cast<int16_t>(queued_tx::get16(_profile + 16)));
  _credit = static_cast<uint32_t>(
      queued_tx::WINDOW_MS *
      (1.0f / (1.0f + queued_tx::getFloat(_profile + 11))));
  _credit_updated = millis();
  _configuration_generation = 1;
  _configuration_fault = false;
  Serial.println(_profile_committed
                     ? "PHY operator profile restored from NVS"
                     : "PHY has no saved profile; using compiled defaults");
  return true;
}

void WifiKissMultiplexer::dashboardStatus(
    RadioDashboard::RadioStatus &status) const {
  status = RadioDashboard::RadioStatus{};
#if KISS_STREAM_ENDPOINT
  status.stream.enabled = true;
  status.stream.slot = STREAM_SLOT;
  status.stream.generation = _stream.generation;
  status.stream.connected = _stream_io && _stream.active;
  status.stream.negotiated = status.stream.connected && _stream.negotiated;
  status.stream.fault = streamFaulted();
  status.stream.output_overflows = streamOutputOverflows();
#endif
  memcpy(status.profile, _profile, sizeof(_profile));
  status.generation = _configuration_generation;
  status.committed = _profile_committed;
  status.fault = _configuration_fault;
  status.transmitting = _transmitting;
  status.carrier_wait = _carrier_wait;
  status.credit_ms = _credit;
  status.owner_slot = isTargetConnected(_owner) ? _owner.slot : -1;
  for (const auto &job : _jobs)
    if (job.used)
      ++status.queued;
  for (size_t i = 0; i < KISS_MAX_TCP_CLIENTS; ++i) {
    const auto &client = _clients[i];
    if (!client.active)
      continue;
    ++status.clients;
    auto &entry = status.client[i];
    entry.connected = true;
    entry.negotiated = client.negotiated;
    entry.generation = client.generation;
    entry.factor = client.source_factor;
    entry.credit_ms = client.source_credit;
    entry.rf_ms = client.source_airtime;
  }
  if (_session_slot < KISS_MAX_TCP_CLIENTS) {
    status.session.connected = true;
    status.session.physical_slot = _session_slot;
    for (uint8_t i = 0; i < queued_tx::SESSION_PORTS - 1; ++i) {
      const auto &port = _session_ports[i];
      auto &entry = status.session.port[i];
      entry.connected = port.active && port.negotiated;
      entry.negotiated = port.negotiated;
      entry.generation = port.generation;
      entry.factor = port.source_factor;
      entry.credit_ms = port.source_credit;
      entry.rf_ms = port.source_airtime;
      status.session.output_queued_bytes[i] = port.output_length;
    }
  }
}

bool WifiKissMultiplexer::hasPendingTransmit() const {
  if (_transmitting)
    return true;
  for (const auto &job : _jobs)
    if (job.used)
      return true;
  return false;
}

uint8_t WifiKissMultiplexer::sourceQueuedCount(uint8_t slot) const {
  if (slot > ENGINE_SLOT) {
    Serial.printf("KISS queue query: invalid source slot %u\n", slot);
    return 0;
  }
  uint8_t count = 0;
  for (const auto& job : _jobs)
    if (job.used && job.source.slot == slot && isTargetConnected(job.source))
      ++count;
  return count;
}

bool WifiKissMultiplexer::sourceTransmitting(uint8_t slot) const {
  if (slot > ENGINE_SLOT) {
    Serial.printf("KISS transmit query: invalid source slot %u\n", slot);
    return false;
  }
  return _transmitting && _sending.source.slot == slot &&
         isTargetConnected(_sending.source);
}

#ifdef MESH_QUEUED_RADIO_API
bool WifiKissMultiplexer::getQueuedRadioStats(
    uint8_t slot, mesh::QueuedRadioStats& stats) const {
  if (slot > ENGINE_SLOT) {
    Serial.printf("KISS stats query: invalid source slot %u\n", slot);
    return false;
  }
  const auto& source = sourceState(slot);
  if (!_radio || !localReady() || !source.generation ||
      !isTargetConnected({slot, source.generation}))
    return false;
  mesh::QueuedRadioStats result;
  result.generation = source.generation;
  result.configuration_generation = _configuration_generation;
  result.captured_ms = millis();
  result.aggregate_credit_ms = _credit;
  result.source_credit_ms = source.source_credit;
  result.aggregate_rf_ms = _airtime;
  result.source_rf_ms = source.source_airtime;
  result.aggregate_successes = _successes;
  result.aggregate_failures = _failures;
  result.source_successes = source.source_successes;
  result.source_failures = source.source_failures;
  for (const auto& job : _jobs)
    if (job.used) ++result.aggregate_queued;
  result.aggregate_transmitting = _transmitting;
  stats = result;
  return true;
}
#endif

bool WifiKissMultiplexer::extension(uint8_t slot, const uint8_t *decoded,
                                    uint16_t length) {
  using namespace queued_tx;
  auto &client = wireState(slot);
  auto target = ClientTarget{slot, client.generation};
  const uint8_t command = decoded[1];
  const uint8_t *data = decoded + 2;
  const uint16_t n = length - 2;
  uint8_t reply[40]{};
  reply[0] = VERSION;
  if (command == HELLO) {
    uint8_t reason =
        n == 2 && data[0] == VERSION && data[1] <= 1 ? NONE : INVALID;
    if (reason == NONE && data[1]) {
      if (isTargetConnected(_owner) &&
          (_owner.slot != slot || _owner.generation != target.generation)) {
        reason = NOT_OWNER;
      }
    }
    if (reason == NONE) {
      if (_session_slot < KISS_MAX_TCP_CLIENTS && slot == _session_slot) {
        for (uint8_t port = 1; port < SESSION_PORTS; ++port)
          removeClient(sessionSource(port));
        _session_slot = KISS_MAX_TCP_CLIENTS;
      }
      if (sessionPort(slot)
#if KISS_STREAM_ENDPOINT
          || slot == STREAM_SLOT
#endif
      ) {
        const bool report = client.signal_report;
        if (!sessionPort(slot) || client.negotiated)
          removeClient(slot);
        client = {};
        client.active = true;
        client.signal_report = sessionPort(slot) ? report : true;
        client.source_factor = 1;
        client.source_credit = WINDOW_MS / 2;
        client.source_updated = millis();
        client.generation = newGeneration();
        target.generation = client.generation;
      }
      if (data[1]) _owner = target;
      client.negotiated = true;
    }
    reply[1] = reason;
    put32(reply + 2, target.generation);
    put32(reply + 6, _configuration_generation);
    reply[10] = KISS_REQUEST_QUEUE_DEPTH;
    reply[11] = _owner.slot == slot && _owner.generation == target.generation;
    sendHardware(target, HW_RESP(HELLO), reply, 12);
    return true;
  }
  if (command == CAPACITY) {
    if (!client.negotiated || (n != 1 && n != 2) ||
        data[0] != VERSION || (n == 2 && (data[1] != 1 ||
        slot >= KISS_MAX_TCP_CLIENTS))) {
      const uint8_t error = HW_ERR_INVALID_PARAM;
      sendHardware(target, HW_RESP_ERROR, &error, 1);
      return true;
    }
    if (n == 2) {
      if (_session_slot >= KISS_MAX_TCP_CLIENTS) {
        if (!enableSessionKeepalive(_clients[slot].socket.fd(), slot)) {
          removeClient(slot);
          return true;
        }
        _session_slot = slot;
        _session_cursor = 0;
        for (uint8_t port = 1; port < SESSION_PORTS; ++port) {
          auto &child = wireState(sessionSource(port));
          child = {};
          child.active = true;
          child.signal_report = true;
          child.source_factor = 1;
          child.source_credit = WINDOW_MS / 2;
          child.source_updated = millis();
          child.generation = newGeneration();
        }
      }
    }
    reply[1] = KISS_MAX_TCP_CLIENTS;
    reply[2] = KISS_LOCAL_SOURCES;
    if (n == 2) {
      reply[3] = SESSION_PORTS;
      reply[4] = _session_slot == slot ? 1 : 0;
    }
    sendHardware(target, HW_RESP(CAPACITY), reply, n == 2 ? 5 : 3);
    return true;
  }
  if (command == ROLE_PRESENCE) {
    uint8_t reason = NONE;
    uint8_t warnings = 0;
    bool empty_key = true;
    if (n == 2 + ROLE_KEY_SIZE)
      for (uint8_t i = 0; i < ROLE_KEY_SIZE; ++i)
        empty_key &= data[2 + i] == 0;
    if ((slot >= KISS_MAX_TCP_CLIENTS && !sessionPort(slot)) ||
        !client.negotiated ||
        n != 2 + ROLE_KEY_SIZE || data[0] != VERSION ||
        data[1] >= ROLE_COUNT || empty_key)
      reason = INVALID;
    else {
      client.has_role_presence = true;
      client.announced_role = data[1];
      memcpy(client.announced_key, data + 2, ROLE_KEY_SIZE);
      const uint8_t bit = 1u << data[1];
      if (_active_role_mask & bit)
        warnings |= NATIVE_ROLE_PRESENT;
      for (uint8_t i = 0; i < _native_key_count; ++i)
        if (!memcmp(data + 2, _native_keys[i], ROLE_KEY_SIZE))
          warnings |= NATIVE_KEY_PRESENT;
      for (uint8_t i = 0; i < KISS_MAX_TCP_CLIENTS; ++i) {
        if (i == slot) continue;
        const auto& other = _clients[i];
        if (!other.active || !other.has_role_presence) continue;
        if (other.announced_role == data[1])
          warnings |= TCP_ROLE_PRESENT;
        if (!memcmp(other.announced_key, data + 2, ROLE_KEY_SIZE))
          warnings |= TCP_KEY_PRESENT;
      }
      for (uint8_t i = 0; i < SESSION_PORTS - 1; ++i) {
        const auto &other = _session_ports[i];
        if (SESSION_BASE + i == slot || !other.active ||
            !other.has_role_presence) continue;
        if (other.announced_role == data[1])
          warnings |= TCP_ROLE_PRESENT;
        if (!memcmp(other.announced_key, data + 2, ROLE_KEY_SIZE))
          warnings |= TCP_KEY_PRESENT;
      }
      if (warnings && warnings != client.presence_warnings) {
        const char *roles[] = {"repeater", "room", "companion", "observer"};
        constexpr char hexDigits[] = "0123456789abcdef";
        char keyHex[2 * ROLE_KEY_SIZE + 1];
        for (uint8_t i = 0; i < ROLE_KEY_SIZE; ++i) {
          keyHex[2 * i] = hexDigits[data[2 + i] >> 4];
          keyHex[2 * i + 1] = hexDigits[data[2 + i] & 0x0f];
        }
        keyHex[2 * ROLE_KEY_SIZE] = 0;
        Serial.printf("KISS client %u %s identity %s overlap (advisory): "
                      "%s%s%s%s; "
                      "continuing\n",
                      slot, roles[data[1]], keyHex,
                      warnings & NATIVE_ROLE_PRESENT ? "native role " : "",
                      warnings & TCP_ROLE_PRESENT ? "TCP role " : "",
                      warnings & NATIVE_KEY_PRESENT ? "native identity " : "",
                      warnings & TCP_KEY_PRESENT ? "TCP identity" : "");
      }
      client.presence_warnings = warnings;
    }
    reply[1] = reason;
    put32(reply + 2, target.generation);
    reply[6] = _native_role_mask;
    reply[7] = warnings;
    sendHardware(target, HW_RESP(ROLE_PRESENCE), reply, 8);
    return true;
  }
  if (command == SUBMIT) {
    const uint32_t id = n >= 9 ? get32(data + 5) : 0;
    uint8_t reason = NONE;
    if (!client.negotiated || n < 19 || n > 18 + KISS_MAX_PACKET_SIZE ||
        data[0] != VERSION || !id || get32(data + 10) > MAX_DELAY_MS ||
        get32(data + 14) > MAX_DELAY_MS)
      reason = INVALID;
    else if (get32(data + 1) != target.generation || id <= client.last_job ||
             client.configuration_seen != _configuration_generation)
      reason = STALE;
    if (reason != NONE) {
      TxJob job{};
      job.extended = true;
      job.source = target;
      job.id = id;
      if (n >= 9 && (sessionPort(slot)
#if KISS_STREAM_ENDPOINT
                     || slot == STREAM_SLOT
#endif
                     )) {
        // A late submission must not complete a reused ID in a new generation.
        job.source.generation = get32(data + 1);
        notify(job, REJECTED, reason, 0, 0, 0, &target);
      } else {
        notify(job, REJECTED, reason);
      }
    } else {
      client.last_job = id;
      submit(slot, data + 18, n - 18, true, id, data[9], get32(data + 10),
             get32(data + 14));
    }
    return true;
  }
  if (command == CONFIG) {
    uint8_t reason = NONE;
    if (!client.negotiated || n < 2 || data[0] != VERSION ||
        (data[1] == 0 ? n != 2 : data[1] != 1 || n != 6 + PROFILE_SIZE))
      reason = INVALID;
    else if (data[1] == 1) {
      const uint8_t *profile = data + 6;
      if (_owner.slot != slot || _owner.generation != target.generation)
        reason = NOT_OWNER;
      else if (get32(data + 2) != _configuration_generation)
        reason = STALE;
      else if (!validProfile(profile) || !_configure || !_power)
        reason = INVALID;
      else if (_profile_committed && !_configuration_fault &&
               memcmp(profile, _profile, PROFILE_SIZE) == 0) {
        // An owner reconnect may verify/reassert an unchanged profile.
#if defined(MESHCORE_ONCHIP) && MESHCORE_ONCHIP
      } else {
        reason = NOT_OWNER;
#else
      } else if (hasPendingTransmit())
        reason = queued_tx::BUSY;
      else if (!persistProfile(profile)) {
        _configuration_fault = true;
        reason = NOT_CONFIGURED;
      } else {
        refill(millis());
        _configure(get32(profile) / 1000000.0, get32(profile + 4) / 1000.0,
                   profile[8], profile[9]);
        _power(profile[10]);
        memcpy(_profile, profile, PROFILE_SIZE);
        memcpy(_persisted_profile, profile, PROFILE_SIZE);
        _radio->setCADEnabled(_profile[15] != 0);
        _radio->triggerNoiseFloorCalibrate(
            static_cast<int16_t>(get16(_profile + 16)));
        const uint32_t maximum = static_cast<uint32_t>(
            WINDOW_MS * (1.0f / (1.0f + getFloat(_profile + 11))));
        if (!_configuration_generation || _credit > maximum)
          _credit = maximum;
        ++_configuration_generation;
        if (!_configuration_generation)
          ++_configuration_generation;
        _profile_committed = true;
        _configuration_fault = false;
#endif
      }
    } else if (_configuration_fault)
      reason = NOT_CONFIGURED;
    reply[1] = reason;
    put32(reply + 2, _configuration_generation);
    memcpy(reply + 6, _profile, PROFILE_SIZE);
    if (reason == NONE)
      client.configuration_seen = _configuration_generation;
    sendHardware(target, HW_RESP(CONFIG), reply, 6 + PROFILE_SIZE);
    return true;
  }
  if (command == SOURCE_POLICY) {
    uint8_t reason = NONE;
    const float factor = n == 9 ? getFloat(data + 5) : NAN;
    if (!client.negotiated || n != 9 || data[0] != VERSION ||
        !isfinite(factor) || factor < 0)
      reason = INVALID;
    else if (get32(data + 1) != target.generation)
      reason = STALE;
    else {
      refill(millis());
      client.source_factor = factor;
      const uint32_t maximum = static_cast<uint32_t>(
          WINDOW_MS * (1.0f / (1.0f + client.source_factor)));
      if (client.source_credit > maximum)
        client.source_credit = maximum;
    }
    reply[1] = reason;
    putFloat(reply + 2, client.source_factor);
    sendHardware(target, HW_RESP(SOURCE_POLICY), reply, 6);
    return true;
  }
  if (command == STATS) {
    if (n != 1 || data[0] != VERSION || !client.negotiated) {
      const uint8_t error = HW_ERR_INVALID_PARAM;
      sendHardware(target, HW_RESP_ERROR, &error, 1);
      return true;
    }
    refill(millis());
    put32(reply + 1, _configuration_generation);
    put32(reply + 5, _credit);
    put32(reply + 9, _airtime);
    put32(reply + 13, _successes);
    put32(reply + 17, _failures);
    put32(reply + 21, client.source_credit);
    put32(reply + 25, client.source_airtime);
    put32(reply + 29, client.source_successes);
    put32(reply + 33, client.source_failures);
    for (const auto &job : _jobs)
      if (job.used)
        ++reply[37];
    reply[38] = _transmitting;
    sendHardware(target, HW_RESP(STATS), reply, 39);
    return true;
  }
  if (command == HW_CMD_GET_RADIO) {
    sendHardware(target, HW_RESP(command), _profile, 10);
    return true;
  }
  if (command == HW_CMD_GET_TX_POWER) {
    sendHardware(target, HW_RESP(command), _profile + 10, 1);
    return true;
  }
  if (command == HW_CMD_SET_RADIO || command == HW_CMD_SET_TX_POWER ||
      command == HW_CMD_REBOOT) {
    const bool same_radio = command == HW_CMD_SET_RADIO && n == 10 &&
                            memcmp(data, _profile, 10) == 0;
    const bool same_power =
        command == HW_CMD_SET_TX_POWER && n == 1 && data[0] == _profile[10];
    if (_profile_committed && !_configuration_fault &&
        (same_radio || same_power)) {
      sendHardware(target, HW_RESP_OK, nullptr, 0);
      return true;
    }
    const uint8_t error = HW_ERR_INVALID_PARAM;
    sendHardware(target, HW_RESP_ERROR, &error, 1);
    return true;
  }
  return false;
}

void WifiKissMultiplexer::submit(uint8_t slot, const uint8_t *packet,
                                 uint16_t length, bool extended, uint32_t id,
                                 uint8_t priority, uint32_t delay,
                                 uint32_t expiry, bool engineOrigin,
                                 bool reflectionOrigin) {
  using namespace queued_tx;
  TxJob job{};
  job.source = {slot, sourceState(slot).generation};
  job.extended = extended;
  job.engineOrigin = engineOrigin;
  job.reflectionOrigin = reflectionOrigin;
  job.id = id;
  if (!length || length > KISS_MAX_PACKET_SIZE || !_configuration_generation ||
      _configuration_fault) {
    notify(job, REJECTED,
           !_configuration_generation || _configuration_fault ? NOT_CONFIGURED
                                                              : INVALID);
    return;
  }
  job.length = length;
  memcpy(job.packet, packet, length);
  if (!filterPacket(packet_engine::Stage::Admission, job.packet, job.length,
                    sizeof(job.packet), slot, job.source.generation, id,
                    UINT8_MAX, false, 0, 0, engineOrigin, reflectionOrigin)) {
    notify(job, REJECTED, ENGINE_DROP);
    return;
  }
  for (auto &entry : _jobs) {
    if (entry.used)
      continue;
    job.used = true;
    job.admitted = millis();
    job.eligible = job.admitted + delay;
    job.expiry = job.admitted + expiry;
    job.expiry_delay = expiry;
    job.sequence = ++_sequence;
    job.priority = priority;
    entry = job;
    notify(job, ACCEPTED, NONE);
    return;
  }
  notify(job, REJECTED, FULL);
}

void WifiKissMultiplexer::notify(const TxJob &job, uint8_t state,
                                 uint8_t reason, uint32_t queue_ms,
                                 uint32_t rf_ms, uint32_t estimate,
                                 const ClientTarget *delivery) {
  using namespace queued_tx;
  if (_packet_observer && job.length) {
    _packet_observer->transmitCompleted(
        job.packet, job.length, job.source.slot, job.source.generation, job.id,
        state, reason, queue_ms, rf_ms, estimate);
    _packet_observer->transmitted(job.packet, job.length, state,
                                 job.source.slot, job.source.generation, job.id);
  }
  if (_dashboard)
    _dashboard->transmitted(millis(), job.packet, job.length, job.source.slot,
                            job.source.generation, job.id, state, reason,
                            queue_ms, rf_ms, estimate);
  if (job.source.slot == ENGINE_SLOT) return;
#if KISS_LOCAL_SOURCES > 0
  if (job.source.slot >= KISS_MAX_TCP_CLIENTS &&
      job.source.slot < KISS_MAX_TCP_CLIENTS + KISS_LOCAL_SOURCES) {
    if (state != ACCEPTED && isTargetConnected(job.source))
      _locals[job.source.slot - KISS_MAX_TCP_CLIENTS].sink->completed(
          job.id, state, reason, queue_ms, rf_ms, estimate);
    return;
  }
#endif
  if (!job.extended) {
    if (state == ACCEPTED)
      return;
    const uint8_t result = state == SUCCEEDED;
    sendHardware(job.source, HW_RESP_TX_DONE, &result, 1);
    return;
  }
  uint8_t event[23] = {VERSION};
  put32(event + 1, job.source.generation);
  put32(event + 5, job.id);
  event[9] = state;
  event[10] = reason;
  put32(event + 11, queue_ms);
  put32(event + 15, rf_ms);
  put32(event + 19, estimate);
  sendHardware(delivery ? *delivery : job.source, EVENT, event, sizeof(event));
}

void WifiKissMultiplexer::refillBudget(uint32_t now, float factor,
                                       uint32_t &credit, uint32_t &updated) {
  const float duty = 1.0f / (1.0f + factor);
  const uint32_t maximum = static_cast<uint32_t>(queued_tx::WINDOW_MS * duty);
  const uint32_t elapsed = now - updated;
  if (elapsed >= queued_tx::WINDOW_MS) {
    credit = maximum;
    updated = now;
    return;
  }
  const uint32_t refill = static_cast<uint32_t>(elapsed * duty);
  if (refill) {
    const uint64_t amount = credit + refill;
    credit = amount > maximum ? maximum : static_cast<uint32_t>(amount);
    updated = now;
  }
}

void WifiKissMultiplexer::refill(uint32_t now) {
  refillBudget(now, queued_tx::getFloat(_profile + 11), _credit,
               _credit_updated);
  if (_engine_source.active)
    refillBudget(now, _engine_source.source_factor, _engine_source.source_credit,
                 _engine_source.source_updated);
  for (auto &client : _clients) {
    if (client.active)
      refillBudget(now, client.source_factor, client.source_credit,
                   client.source_updated);
  }
  for (auto &port : _session_ports)
    if (port.active)
      refillBudget(now, port.source_factor, port.source_credit,
                   port.source_updated);
#if KISS_STREAM_ENDPOINT
  if (_stream.active)
    refillBudget(now, _stream.source_factor, _stream.source_credit,
                 _stream.source_updated);
#endif
#if KISS_LOCAL_SOURCES > 0
  for (auto& local : _locals)
    if (local.active) refillBudget(now, local.source_factor, local.source_credit,
                                  local.source_updated);
#endif
}

WifiKissMultiplexer::TxJob *WifiKissMultiplexer::nextTransmit(uint32_t now) {
  using namespace queued_tx;
  TxJob *best = nullptr;
  const uint32_t threshold = _radio->getEstAirtimeFor(KISS_MAX_PACKET_SIZE) / 2;
  for (auto &job : _jobs) {
    if (!job.used)
      continue;
    if (!isTargetConnected(job.source)) {
      job.used = false;
      continue;
    }
    if (job.expiry_delay && static_cast<int32_t>(now - job.expiry) >= 0) {
      // A failed notification may remove the client; this job is already final.
      job.used = false;
      notify(job, FAILED, EXPIRED, now - job.admitted);
      continue;
    }
    if (static_cast<int32_t>(now - job.eligible) < 0)
      continue;
    const auto &client = sourceState(job.source.slot);
    if (_credit < threshold || _credit < 100 ||
        client.source_credit < threshold || client.source_credit < 100)
      continue;
    if (!best || job.priority < best->priority ||
        (job.priority == best->priority &&
         static_cast<int32_t>(job.sequence - best->sequence) < 0))
      best = &job;
  }
  // Expiry notification can disconnect a slow client and invalidate its jobs.
  return best && best->used && isTargetConnected(best->source) ? best : nullptr;
}

void WifiKissMultiplexer::serviceTransmit() {
  using namespace queued_tx;
  if (!_radio || _configuration_fault)
    return;
  const uint32_t now = millis();
  refill(now);
  if (_transmitting) {
    if (_radio->isSendComplete())
      finishTransmit(SUCCEEDED, NONE);
    else if (static_cast<uint32_t>(now - _rf_start) >= _rf_estimate * 3 / 2)
      finishTransmit(UNKNOWN, RF_TIMEOUT);
    else
      return;
  }
  if (!nextTransmit(now)) {
    _carrier_wait = false;
    return;
  }
  if (_carrier_wait && static_cast<int32_t>(now - _next_carrier) <= 0)
    return;
  if (_radio->isReceiving()) {
    const uint32_t checked = millis();
    if (!_carrier_wait) {
      _carrier_wait = true;
      _busy_since = checked;
    }
    if (static_cast<uint32_t>(checked - _busy_since) <= 4000) {
      _next_carrier = checked + _rng->nextInt(1, 4) * 120;
      return;
    }
  }
  _carrier_wait = false;
  const uint32_t ready = millis();
  refill(ready);
  TxJob *best = nextTransmit(ready);
  if (!best)
    return;
  _sending = *best;
  best->used = false;
  if (!filterPacket(packet_engine::Stage::Transmit, _sending.packet, _sending.length,
                    sizeof(_sending.packet), _sending.source.slot,
                    _sending.source.generation, _sending.id, UINT8_MAX, false,
                    0, 0, _sending.engineOrigin, _sending.reflectionOrigin)) {
    notify(_sending, FAILED, ENGINE_DROP, ready - _sending.admitted);
    return;
  }
  _rf_estimate = _radio->getEstAirtimeFor(_sending.length);
  // Hardware CAD may block; its time belongs to queueing, not RF occupancy.
  _rf_start = millis();
  if (_sending.expiry_delay &&
      static_cast<int32_t>(_rf_start - _sending.expiry) >= 0) {
    notify(_sending, FAILED, EXPIRED, _rf_start - _sending.admitted);
    return;
  }
  if (_packet_observer && _sending.length)
    _packet_observer->transmitStarting(
        _sending.packet, _sending.length, _sending.source.slot,
        _sending.source.generation, _sending.id, _sending.priority,
        _sending.eligible - _sending.admitted, _sending.eligible,
        _sending.expiry_delay);
  if (!_radio->startSendRaw(_sending.packet, _sending.length)) {
    ++_failures;
    if (isTargetConnected(_sending.source))
      ++sourceState(_sending.source.slot).source_failures;
    notify(_sending, FAILED, START_FAILED, _rf_start - _sending.admitted, 0,
           _rf_estimate);
    return;
  }
  _transmitting = true;
}

void WifiKissMultiplexer::finishTransmit(uint8_t state, uint8_t reason) {
  const uint32_t duration = static_cast<uint32_t>(millis()) - _rf_start;
  _radio->onSendFinished();
  _transmitting = false;
  _airtime += duration;
  _credit = duration > _credit ? 0 : _credit - duration;
  if (state == queued_tx::SUCCEEDED)
    ++_successes;
  else
    ++_failures;
  if (isTargetConnected(_sending.source)) {
    auto &client = sourceState(_sending.source.slot);
    client.source_airtime += duration;
    client.source_credit =
        duration > client.source_credit ? 0 : client.source_credit - duration;
    if (state == queued_tx::SUCCEEDED)
      ++client.source_successes;
    else
      ++client.source_failures;
  }
  notify(_sending, state, reason, _rf_start - _sending.admitted, duration,
         _rf_estimate);
  if (state == queued_tx::SUCCEEDED)
    reflect(_sending);
}

void WifiKissMultiplexer::reflect(const TxJob &job) {
  TxJob reflection = job;
  if (!filterPacket(packet_engine::Stage::Reflection, reflection.packet, reflection.length,
                    sizeof(reflection.packet), job.source.slot, job.source.generation,
                    job.id, UINT8_MAX, true, 0, 0, job.engineOrigin, true)) return;
#if KISS_LOCAL_SOURCES > 0
  for (uint8_t i = 0; i < KISS_LOCAL_SOURCES; ++i) {
    auto& local = _locals[i];
    if (local.active && !(job.source.slot == KISS_MAX_TCP_CLIENTS + i &&
                           job.source.generation == local.generation))
      {
        uint8_t candidate[KISS_MAX_PACKET_SIZE];
        uint16_t length = reflection.length;
        memcpy(candidate, reflection.packet, length);
        if (filterPacket(packet_engine::Stage::LocalDelivery, candidate, length,
                         sizeof(candidate), job.source.slot, job.source.generation,
                         job.id, KISS_MAX_TCP_CLIENTS + i, true,
                         LOCAL_LOOPBACK_RSSI, LOCAL_LOOPBACK_SNR / 4.0f,
                         job.engineOrigin, true))
          local.sink->receivedWithOrigin(candidate, length, LOCAL_LOOPBACK_RSSI,
                                         LOCAL_LOOPBACK_SNR / 4.0f, true,
                                         job.engineOrigin, true);
      }
  }
#endif
  uint8_t encoded[MAX_ENCODED_FRAME];
  uint16_t n = encodeFrame(KISS_CMD_DATA, reflection.packet, reflection.length, encoded,
                           sizeof(encoded));
  broadcastFrame(encoded, n, &job.source);
  const uint8_t metadata[] = {HW_RESP_RX_META,
                              static_cast<uint8_t>(LOCAL_LOOPBACK_SNR),
                              static_cast<uint8_t>(LOCAL_LOOPBACK_RSSI)};
  n = encodeFrame(KISS_CMD_SETHARDWARE, metadata, sizeof(metadata), encoded,
                  sizeof(encoded));
  broadcastFrame(encoded, n, &job.source, true);
}
bool WifiKissMultiplexer::loadNextFrame() {
  while (_queue_count > 0) {
    _active = _queue[_queue_head];
    _queue_head = (_queue_head + 1) % KISS_REQUEST_QUEUE_DEPTH;
    --_queue_count;
    if (!isTargetConnected(_active.source))
      continue;

    _active_valid = true;
    _active_input_offset = 0;
    _active_input_consumed = false;
    const uint8_t port = (_active.decoded[0] >> 4) & 0x0F;
    const uint8_t command = _active.decoded[0] & 0x0F;
    _active_is_data =
        port == 0 && command == KISS_CMD_DATA && _active.decoded_length > 1;
    _active_waits_for_response =
        _active_is_data || (port == 0 && command == KISS_CMD_SETHARDWARE &&
                            _active.decoded_length > 1);
    return true;
  }
  return false;
}

void WifiKissMultiplexer::completeActive() {
  _active_valid = false;
  _active_waits_for_response = false;
  _active_is_data = false;
  _active_input_consumed = false;
  _active_input_offset = 0;
}

void WifiKissMultiplexer::processOutputByte(uint8_t byte) {
  if (byte == KISS_FEND) {
    if (_output_collecting && _output_length > 1)
      finishOutputFrame();
    _output_collecting = true;
    _output_length = 1;
    _output[0] = KISS_FEND;
    return;
  }
  if (!_output_collecting)
    return;
  if (_output_length >= MAX_ENCODED_FRAME - 1) {
    _output_collecting = false;
    _output_length = 0;
    return;
  }
  _output[_output_length++] = byte;
}

void WifiKissMultiplexer::finishOutputFrame() {
  _output[_output_length++] = KISS_FEND;
  uint8_t decoded[KISS_MAX_FRAME_SIZE + 2];
  const uint16_t decoded_length =
      decodeFrame(_output, _output_length, decoded, sizeof(decoded));
  if (decoded_length > 0) {
    routeOutputFrame(_output, _output_length, decoded, decoded_length);
  }
}

void WifiKissMultiplexer::routeOutputFrame(const uint8_t *encoded,
                                           uint16_t encoded_length,
                                           const uint8_t *decoded,
                                           uint16_t decoded_length) {
  const uint8_t command = decoded[0] & 0x0F;
  if (command == KISS_CMD_DATA) {
    broadcastFrame(encoded, encoded_length);
    return;
  }
  if (command != KISS_CMD_SETHARDWARE || decoded_length < 2) {
    broadcastFrame(encoded, encoded_length);
    return;
  }

  const uint8_t subcommand = decoded[1];
  if (subcommand == HW_RESP_RX_META) {
    broadcastFrame(encoded, encoded_length, nullptr, true);
    return;
  }

  if (subcommand == HW_RESP_TX_DONE) {
    if (_active_valid && _active_input_consumed && _active_is_data &&
        decoded_length == 3) {
      sendFrame(_active.source, encoded, encoded_length);
      if (decoded[2] == 1) {
        sendLocalReflection();
      }
      completeActive();
    } else {
      broadcastFrame(encoded, encoded_length);
    }
    return;
  }

  if (_active_valid && _active_input_consumed && _active_waits_for_response) {
    sendFrame(_active.source, encoded, encoded_length);
    completeActive();
  } else {
    broadcastFrame(encoded, encoded_length);
  }
}

void WifiKissMultiplexer::sendFrame(ClientTarget target, const uint8_t *encoded,
                                    uint16_t length, bool unsolicited) {
  if (target.slot >= KISS_MAX_TCP_CLIENTS && !sessionPort(target.slot)
#if KISS_STREAM_ENDPOINT
      && target.slot != STREAM_SLOT
#endif
  ) return;
  if (!isTargetConnected(target))
    return;
  auto &client = wireState(target.slot);
  if (sessionPort(target.slot)) {
    if (length > CLIENT_OUTPUT_CAPACITY - client.output_length)
      drainSessionOutput();
    if (!isTargetConnected(target)) return;
    if (length > CLIENT_OUTPUT_CAPACITY - client.output_length) {
      Serial.printf("KISS session port %u output full; disconnecting session\n",
                    sessionNumber(target.slot));
      removeClient(_session_slot);
      return;
    }
    const uint16_t tail =
        (client.output_head + client.output_length) % CLIENT_OUTPUT_CAPACITY;
    for (uint16_t i = 0; i < length; ++i)
      client.output[(tail + i) % CLIENT_OUTPUT_CAPACITY] =
          i == 1 ? encoded[i] | (sessionNumber(target.slot) << 4)
                 : encoded[i];
    client.output_length += length;
    drainSessionOutput();
    return;
  }
  uint16_t reserve = 0;
#if KISS_STREAM_ENDPOINT
  if (target.slot == STREAM_SLOT && unsolicited) reserve = MAX_ENCODED_FRAME;
#endif
  if (length + reserve > CLIENT_OUTPUT_CAPACITY - client.output_length) {
    drainClientOutput(target.slot);
    if (!client.active)
      return;
  }
  if (length + reserve > CLIENT_OUTPUT_CAPACITY - client.output_length) {
#if KISS_STREAM_ENDPOINT
    if (target.slot == STREAM_SLOT) {
      faultStream();
      return;
    }
#endif
    Serial.printf("KISS client %u output queue overflow; disconnecting\n",
                  target.slot);
    removeClient(target.slot);
    return;
  }
  const uint16_t tail =
      (client.output_head + client.output_length) % CLIENT_OUTPUT_CAPACITY;
  const uint16_t contiguous = CLIENT_OUTPUT_CAPACITY - tail;
  const uint16_t first = length < contiguous ? length : contiguous;
  memcpy(client.output + tail, encoded, first);
  if (first < length)
    memcpy(client.output, encoded + first, length - first);
  client.output_length += length;
  drainClientOutput(target.slot);
}

void WifiKissMultiplexer::broadcastFrame(const uint8_t *encoded,
                                         uint16_t length,
                                         const ClientTarget *exclude,
                                         bool signal_only) {
  for (uint8_t slot = 0; slot < KISS_MAX_TCP_CLIENTS; ++slot) {
    auto &client = _clients[slot];
    if (!client.active)
      continue;
    if (exclude && exclude->slot == slot &&
        exclude->generation == client.generation) {
      continue;
    }
    if (signal_only && !client.signal_report)
      continue;
    const ClientTarget target = {slot, client.generation};
    sendFrame(target, encoded, length);
  }
  if (_session_slot < KISS_MAX_TCP_CLIENTS)
    for (uint8_t port = 1; port < queued_tx::SESSION_PORTS &&
                           _session_slot < KISS_MAX_TCP_CLIENTS; ++port) {
      const uint8_t slot = sessionSource(port);
      auto &child = wireState(slot);
      if (!child.active || !child.negotiated ||
          (signal_only && !child.signal_report) ||
          (exclude && exclude->slot == slot &&
           exclude->generation == child.generation))
        continue;
      sendFrame({slot, child.generation}, encoded, length, true);
    }
#if KISS_STREAM_ENDPOINT
  if (_stream.active && _stream.negotiated &&
      !(exclude && exclude->slot == STREAM_SLOT &&
        exclude->generation == _stream.generation) &&
      (!signal_only || _stream.signal_report))
    sendFrame({STREAM_SLOT, _stream.generation}, encoded, length, true);
#endif
}

void WifiKissMultiplexer::sendHardware(ClientTarget target, uint8_t subcommand,
                                       const uint8_t *payload,
                                       uint16_t payload_length) {
  uint8_t encoded[MAX_ENCODED_FRAME];
  uint8_t hardware[KISS_HW_MAX_PAYLOAD_SIZE];
  if (static_cast<size_t>(payload_length) + 1 > sizeof(hardware))
    return;
  hardware[0] = subcommand;
  if (payload_length > 0)
    memcpy(hardware + 1, payload, payload_length);
  const uint16_t encoded_length =
      encodeFrame(KISS_CMD_SETHARDWARE, hardware, payload_length + 1, encoded,
                  sizeof(encoded));
  if (encoded_length > 0)
    sendFrame(target, encoded, encoded_length);
}

void WifiKissMultiplexer::sendLocalReflection() {
  broadcastFrame(_active.encoded, _active.encoded_length, &_active.source);
  const uint8_t metadata[] = {
      static_cast<uint8_t>(LOCAL_LOOPBACK_SNR),
      static_cast<uint8_t>(LOCAL_LOOPBACK_RSSI),
  };
  uint8_t hardware[] = {HW_RESP_RX_META, metadata[0], metadata[1]};
  uint8_t encoded[MAX_ENCODED_FRAME];
  const uint16_t encoded_length =
      encodeFrame(KISS_CMD_SETHARDWARE, hardware, sizeof(hardware), encoded,
                  sizeof(encoded));
  if (encoded_length > 0) {
    broadcastFrame(encoded, encoded_length, &_active.source, true);
  }
}

uint16_t WifiKissMultiplexer::decodeFrame(const uint8_t *encoded,
                                          uint16_t encoded_length,
                                          uint8_t *decoded,
                                          uint16_t decoded_capacity) {
  if (encoded_length < 3 || encoded[0] != KISS_FEND ||
      encoded[encoded_length - 1] != KISS_FEND) {
    return 0;
  }
  uint16_t output = 0;
  bool escaped = false;
  for (uint16_t i = 1; i + 1 < encoded_length; ++i) {
    uint8_t byte = encoded[i];
    if (escaped) {
      escaped = false;
      if (byte == KISS_TFEND)
        byte = KISS_FEND;
      else if (byte == KISS_TFESC)
        byte = KISS_FESC;
      else
        return 0;
    } else if (byte == KISS_FESC) {
      escaped = true;
      continue;
    }
    if (output >= decoded_capacity)
      return 0;
    decoded[output++] = byte;
  }
  return escaped ? 0 : output;
}

uint16_t WifiKissMultiplexer::encodeFrame(uint8_t command,
                                          const uint8_t *payload,
                                          uint16_t payload_length,
                                          uint8_t *encoded,
                                          uint16_t encoded_capacity) {
  if (encoded_capacity < 3)
    return 0;
  uint16_t output = 0;
  encoded[output++] = KISS_FEND;
  auto append = [&](uint8_t byte) -> bool {
    if (byte == KISS_FEND || byte == KISS_FESC) {
      if (output + 2 >= encoded_capacity)
        return false;
      encoded[output++] = KISS_FESC;
      encoded[output++] = byte == KISS_FEND ? KISS_TFEND : KISS_TFESC;
    } else {
      if (output + 1 >= encoded_capacity)
        return false;
      encoded[output++] = byte;
    }
    return true;
  };
  if (!append(command))
    return 0;
  for (uint16_t i = 0; i < payload_length; ++i) {
    if (!append(payload[i]))
      return 0;
  }
  encoded[output++] = KISS_FEND;
  return output;
}
