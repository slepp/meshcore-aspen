// SPDX-License-Identifier: Apache-2.0
#include "Observer.h"
#include "Clock.h"
#include "Config.h"
#include "Lifecycle.h"
#include "Runtime.h"
#include "ObserverWire.h"
#include <cmath>
#include <ctime>
#include <esp_system.h>
#include <freertos/task.h>

namespace onchip {
namespace {
const char *typeName(uint8_t type) {
  static const char *const names[] = {
      "REQ", "RESPONSE", "TXT_MSG", "ACK", "ADVERT", "GRP_TXT",
      "GRP_DATA", "ANON_REQ", "PATH", "TRACE", "MULTI_PART", "CONTROL"};
  if (type < sizeof(names) / sizeof(names[0]))
    return names[type];
  return type == 15 ? "RAW_CUSTOM" : "";
}
const char *routeName(uint8_t route) {
  static const char *const names[] = {
      "TRANSPORT_FLOOD", "FLOOD", "DIRECT", "TRANSPORT_DIRECT"};
  return names[route];
}
bool formatEnvelope(const uint8_t *raw, size_t length, char *output,
                    size_t capacity) {
  char error[96]{};
  size_t offset = 0;
  uint8_t header = 0, pathLength = 0, hashSize = 0, hops = 0;
  size_t pathBytes = 0;
  if (!length)
    snprintf(error, sizeof(error), "reading header: EOF");
  else {
    header = raw[offset++];
    if (header >> 6)
      snprintf(error, sizeof(error), "unsupported payload version %u",
               header >> 6);
    else {
      const uint8_t route = header & 3;
      if (route == 0 || route == 3) {
        if (length - offset < 4)
          snprintf(error, sizeof(error), "reading transport codes: short frame");
        else
          offset += 4;
      }
      if (!error[0]) {
        if (offset == length)
          snprintf(error, sizeof(error), "reading path length: EOF");
        else {
          pathLength = raw[offset++];
          hashSize = (pathLength >> 6) + 1;
          hops = pathLength & 63;
          pathBytes = size_t(hashSize) * hops;
          if (hashSize == 4 || pathBytes > 64)
            snprintf(error, sizeof(error), "invalid path length byte: 0x%02x",
                     pathLength);
          else if (length - offset < pathBytes)
            snprintf(error, sizeof(error),
                     "not enough data for path: need %u bytes, have %u",
                     unsigned(pathBytes), unsigned(length - offset));
          else if (length - offset - pathBytes > 184)
            snprintf(error, sizeof(error),
                     "payload too large: %u bytes, max 184",
                     unsigned(length - offset - pathBytes));
        }
      }
    }
  }
  int size;
  if (error[0])
    size = snprintf(output, capacity, "\"decode_error\":\"%s\"", error);
  else {
    char pathHex[129]{};
    for (size_t i = 0; i < pathBytes; ++i)
      snprintf(pathHex + 2 * i, 3, "%02x", raw[offset + i]);
    const uint8_t route = header & 3, type = (header >> 2) & 15;
    size = snprintf(output, capacity,
                    "\"packet\":{\"header\":%u,\"type\":%u,"
                    "\"type_name\":\"%s\",\"route\":%u,\"route_name\":\"%s\","
                    "\"version\":%u,\"path_hex\":\"%s\",\"path_length\":%u,"
                    "\"path_hash_size\":%u,\"path_hop_count\":%u}",
                    header, type, typeName(type), route, routeName(route),
                    header >> 6, pathHex, pathLength, hashSize, hops);
  }
  return size > 0 && size_t(size) < capacity;
}
} // namespace

void Observer::event(void *arg, esp_event_base_t, int32_t id, void *) {
  auto &self = *static_cast<Observer *>(arg);
  if (id == MQTT_EVENT_CONNECTED) {
    self.connectionFault.store(nullptr);
    if (self.session.fetch_add(1) == UINT32_MAX)
      self.session.fetch_add(1);
    self.connected.store(true);
  }
  if (id == MQTT_EVENT_DISCONNECTED || id == MQTT_EVENT_ERROR) {
    self.connected.store(false);
    self.announcedSession.store(0);
    if (id == MQTT_EVENT_ERROR)
      self.connectionFault.store("MQTT connection failed: check broker/auth/TLS");
  }
}
bool Observer::fail(const char *message) {
  connectionFault.store(message);
  Serial.printf("On-chip observer: %s\n", message);
  return false;
}
bool Observer::begin(WifiKissMultiplexer &mux) {
  strcpy(identity.role, "observer");
  strcpy(identity.name, ONCHIP_OBSERVER_NAME);
  mesh::LocalIdentity loaded;
  if (!loadIdentity("observer", loaded))
    return fail("Identity unavailable");
  memcpy(identity.public_key, loaded.pub_key, sizeof(identity.public_key));
  identity.has_identity = true;
  auto secret = reinterpret_cast<volatile uint8_t *>(&loaded);
  for (size_t i = 0; i < sizeof(loaded); ++i)
    secret[i] = 0;
  if (!loadObserverConfig(settings))
    return fail("Saved MQTT settings invalid; inspect mqtt status");
  strcpy(identity.name, settings.name);
  for (unsigned i = 0; i < sizeof(identity.public_key); ++i)
    snprintf(identityHex + 2 * i, 3, settings.format ? "%02X" : "%02x",
             identity.public_key[i]);
  if (!settings.uri[0])
    return true;
  uint8_t nonce[16];
  esp_fill_random(nonce, sizeof(nonce));
  for (unsigned i = 0; i < sizeof(nonce); ++i)
    snprintf(eventPrefix + 2 * i, 3, "%02x", nonce[i]);
  const char *prefix = settings.prefix;
  const size_t length = strlen(prefix);
  if (!length || length > 120 || prefix[0] == '/' ||
      prefix[length - 1] == '/' || strpbrk(prefix, "+#") != nullptr)
    return fail("Invalid MQTT topic prefix");
  for (const unsigned char *p = reinterpret_cast<const unsigned char *>(prefix);
       *p; ++p)
    if (*p < 32 || *p >= 127)
      return fail("Invalid MQTT topic prefix");
  if (settings.format) {
    const char *iata = settings.iata;
    if (strlen(iata) != 3)
      return fail("Configure MQTT IATA as three uppercase letters");
    for (unsigned i = 0; i < 3; ++i)
      if (iata[i] < 'A' || iata[i] > 'Z')
        return fail("Configure MQTT IATA as three uppercase letters");
    if (strchr(settings.uri, '@') && settings.audience[0])
      return fail("JWT MQTT URI must not contain credentials");
    const bool secure = !strncmp(settings.uri, "mqtts://", 8) ||
                        !strncmp(settings.uri, "wss://", 6);
    if (secure && !settings.ca[0])
      return fail("Secure MQTT connection requires a CA certificate");
    if (strncmp(settings.uri, "mqtt://", 7) &&
        strncmp(settings.uri, "ws://", 5) && !secure)
      return fail("MQTT URI must use mqtt, mqtts, ws or wss");
    snprintf(baseTopic, sizeof(baseTopic), "%s/%s/%s", prefix, iata, identityHex);
  } else {
    if (settings.audience[0])
      return fail("JWT authentication requires public observer format");
    snprintf(baseTopic, sizeof(baseTopic), "%s/%s", prefix, identityHex);
  }
  snprintf(statusTopic, sizeof(statusTopic), "%s/status", baseTopic);
#ifdef ONCHIP_MQTT_TOPIC
  const int topicSize =
      snprintf(packetTopic, sizeof(packetTopic), "%s", ONCHIP_MQTT_TOPIC);
  if (topicSize <= 0 || size_t(topicSize) >= sizeof(packetTopic) ||
      strpbrk(packetTopic, "+#") != nullptr)
    return fail("Invalid MQTT packet topic");
#else
  snprintf(packetTopic, sizeof(packetTopic), "%s/packets", baseTopic);
#endif
  queue = xQueueCreate(8, sizeof(Event));
  modem = &mux;
  statusQueue = xQueueCreate(1, sizeof(Snapshot));
  if (!queue || !statusQueue) {
    if (queue)
      vQueueDelete(queue);
    if (statusQueue)
      vQueueDelete(statusQueue);
    queue = statusQueue = nullptr;
    return fail("MQTT queue allocation failed");
  }
  if (!settings.format && !startClient(0)) {
    vQueueDelete(queue);
    vQueueDelete(statusQueue);
    queue = statusQueue = nullptr;
    return false;
  }
  if (xTaskCreate(worker, "mesh-observer", 8192, this, 1, nullptr) != pdPASS) {
    if (client) {
      esp_mqtt_client_stop(client);
      esp_mqtt_client_destroy(client);
    }
    vQueueDelete(queue);
    vQueueDelete(statusQueue);
    queue = statusQueue = nullptr;
    client = nullptr;
    return fail("MQTT worker startup failed");
  }
  mux.observePackets(*this);
  return true;
}
bool Observer::startClient(uint32_t epoch) {
  esp_mqtt_client_config_t config{};
  char *will = json, *password = json + 1024;
  config.uri = settings.uri;
  config.username = settings.username[0] ? settings.username : nullptr;
  config.password = settings.password[0] ? settings.password : nullptr;
  config.cert_pem = settings.ca[0] ? settings.ca : nullptr;
  config.client_id = identityHex;
  config.lwt_topic = statusTopic;
  config.lwt_msg = "offline";
  if (settings.format) {
    if (!observerWire::status("offline", identity.name, identityHex,
         ONCHIP_MQTT_MODEL, ONCHIP_MQTT_FIRMWARE_VERSION, radio, epoch, will, 1024))
      return fail("MQTT status invalid: check model/version/time");
    config.lwt_msg = will;
    if (settings.audience[0]) {
      mesh::LocalIdentity loaded;
      if (!loadIdentity("observer", loaded)) return false;
      const bool same = !memcmp(loaded.pub_key, identity.public_key, 32);
      const bool signedToken = same && observerWire::token(
          loaded, settings.audience, epoch, password, 1024);
      auto secret = reinterpret_cast<volatile uint8_t *>(&loaded);
      for (size_t i = 0; i < sizeof(loaded); ++i) secret[i] = 0;
      if (!signedToken) return fail("Observer identity changed or JWT signing failed");
      snprintf(username, sizeof(username), "v1_%s", identityHex);
      config.username = username;
      config.password = password;
      tokenIssued = epoch;
      tokenExpires = epoch + observerWire::TOKEN_LIFETIME;
    }
  }
  config.lwt_qos = settings.format ? 1 : 0;
  config.lwt_retain = true;
  config.buffer_size = 3072;
  config.network_timeout_ms = 1000;
  config.reconnect_timeout_ms = 5000;
  client = esp_mqtt_client_init(&config);
  // ESP-MQTT copies CONNECT credentials and will data during init.
  memset(password, 0, 1024);
  if (!client ||
      esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, event, this) !=
          ESP_OK ||
      esp_mqtt_client_start(client) != ESP_OK) {
    if (client)
      esp_mqtt_client_destroy(client);
    client = nullptr;
    return fail("MQTT client startup failed");
  }
  connectionFault.store(nullptr);
  return true;
}
void Observer::dashboardStatus(RadioDashboard::RoleStatus &status) const {
  status = identity;
  status.ready =
      connected.load() && announcedSession.load() == session.load();
  if (const auto fault = connectionFault.load())
    snprintf(status.fault, sizeof(status.fault), "%s", fault);
  strcpy(status.state, status.fault[0]     ? "fault"
                       : !settings.uri[0] ? "disabled"
                       : status.ready        ? "running"
                       : settings.format && !observationEpoch(0, true)
                                             ? "waiting-time"
                                             : "connecting");
}
void Observer::observeRoles(const RadioDashboard::RadioStatus &status) {
  botSource = status.roles[4];
#ifdef ARDUINO_ARCH_ESP32
  loopStackMinimum.store(uxTaskGetStackHighWaterMark(nullptr));
#endif
  if (statusQueue) {
    Snapshot snapshot;
    memcpy(snapshot.roles, status.roles, sizeof(Roles));
    if (status.committed && !status.fault) {
      const auto *p = status.profile;
      snprintf(snapshot.radio, sizeof(snapshot.radio), "%.6f,%.1f,%u,%u",
               queued_tx::get32(p) / 1000000.0,
               queued_tx::get32(p + 4) / 1000.0, p[8], p[9]);
    } else strcpy(snapshot.radio, "unknown");
    xQueueOverwrite(statusQueue, &snapshot);
  }
}
void Observer::enqueue(const Event &e) {
  if (queue && xQueueSend(queue, &e, 0) != pdTRUE)
    dropped.fetch_add(1);
}
uint32_t Observer::observationEpoch(uint32_t at, bool current) const {
  ClockSnapshot snapshot;
  bool captured = false;
  for (unsigned attempt = 0; attempt < 4; ++attempt) {
    if (clockSnapshot(snapshot)) { captured = true; break; }
    if (attempt < 3) delay(1);
  }
  const uint32_t now = millis();
  if (current) at = now;
  const auto previous = acceptedClock.load();
  auto sample = previous;
  const uint64_t lifetime = uint64_t(ONCHIP_SNTP_INTERVAL_SECONDS) * 2000;
  if (captured && (int32_t(snapshot.sampled_at_ms - clockPublicationAt.load()) < 0 ||
                   (previous && int32_t(snapshot.sampled_at_ms - uint32_t(previous)) < 0)))
    captured = false;
  if (captured) {
    if (!snapshot.network_enabled || !snapshot.network_epoch ||
        snapshot.network_age_ms > lifetime) {
      auto invalid = previous;
      acceptedClock.compare_exchange_strong(invalid, 0);
      return 0;
    }
    const int32_t lag = int32_t(now - snapshot.sampled_at_ms);
    if (lag >= 0 && lag <= 3000) {
      const uint32_t received = snapshot.sampled_at_ms - uint32_t(snapshot.network_age_ms);
      sample = uint64_t(snapshot.network_epoch) << 32 | received;
      auto cached = previous;
      for (unsigned attempt = 0; attempt < 4; ++attempt) {
        if (cached && int32_t(received - uint32_t(cached)) < 0) break;
        if (acceptedClock.compare_exchange_weak(cached, sample)) break;
      }
      auto publication = clockPublicationAt.load();
      for (unsigned attempt = 0; attempt < 4; ++attempt) {
        if (int32_t(snapshot.sampled_at_ms - publication) < 0) break;
        if (clockPublicationAt.compare_exchange_weak(publication, snapshot.sampled_at_ms)) break;
      }
    }
  }
  // Extrapolate only an accepted SNTP sample through brief dispatch contention.
  // Stop after 15 seconds without a fresh publication, or real sample expiry.
  if (!sample || uint32_t(now - clockPublicationAt.load()) > 15000) {
    auto invalid = sample;
    acceptedClock.compare_exchange_strong(invalid, 0);
    return 0;
  }
  int32_t age = int32_t(at - uint32_t(sample));
  if (age < 0 && previous) {
    sample = previous;
    age = int32_t(at - uint32_t(sample));
  }
  if (age < 0 || uint64_t(age) > lifetime) return 0;
  const uint64_t epoch = (sample >> 32) + uint32_t(age) / 1000;
  return epoch <= UINT32_MAX ? uint32_t(epoch) : 0;
}
void Observer::packet(const uint8_t *raw, uint16_t length, bool tx,
                      uint8_t state, float rssi, float snr) {
  if (!queue)
    return;
  if (settings.format && (tx || (rssi == 127 && snr == -32)))
    return;
  if (settings.format && length &&
      !(settings.filter & (1u << ((raw[0] >> 2) & 15)))) {
    dropped.fetch_add(1);
    return;
  }
  if (!length || length > 255) {
    dropped.fetch_add(1);
    return;
  }
  Event e{};
  e.at = millis();
  e.epoch = observationEpoch(e.at);
  e.sequence = nextSequence.fetch_add(1) + 1;
  e.length = length;
  e.tx = tx;
  e.state = state;
  e.rssi = rssi;
  e.snr = snr;
  memcpy(e.raw, raw, length);
  enqueue(e);
}
void Observer::transmitted(const uint8_t *raw, uint16_t length, uint8_t state,
                           uint8_t slot, uint32_t generation, uint32_t job) {
  if (!queue || settings.format)
    return;
  Pending *entry = nullptr, *free = nullptr;
  for (auto &item : pending) {
    if (item.used && item.slot == slot && item.generation == generation &&
        item.job == job)
      entry = &item;
    if (!item.used && !free)
      free = &item;
  }
  RadioDashboard::RoleStatus source{};
  if (entry)
    source = entry->source;
  else {
    if (slot < KISS_MAX_TCP_CLIENTS ||
        slot >= KISS_MAX_TCP_CLIENTS + KISS_LOCAL_SOURCES)
      source = botSource;
    source.source_slot = slot;
    source.source_generation = generation;
    localTransmitSource(slot, generation, source);
  }
  if (state == queued_tx::ACCEPTED) {
    if (!entry)
      entry = free;
    if (!entry) {
      dropped.fetch_add(1);
      Serial.println("Observer source attribution capacity exceeded");
      return;
    }
    entry->used = true;
    entry->slot = slot;
    entry->generation = generation;
    entry->job = job;
    entry->source = source;
    return;
  }
  if (entry)
    entry->used = false;
  if (!length || length > 255) {
    dropped.fetch_add(1);
    return;
  }
  Event e{};
  e.at = millis();
  e.epoch = observationEpoch(e.at);
  e.sequence = nextSequence.fetch_add(1) + 1;
  e.length = length;
  e.tx = true;
  e.state = state;
  e.source = source;
  memcpy(e.raw, raw, length);
  enqueue(e);
}
bool Observer::publish(const char *topic, const char *json, int size,
                       bool retain, int qos) {
  if (esp_mqtt_client_publish(client, topic, json, size, qos, retain) >= 0)
    return true;
  publicationErrors.fetch_add(1);
  Serial.println("Observer MQTT publication failed");
  return false;
}
void Observer::statistics(char *reply, size_t capacity) const {
  const int size = snprintf(reply, capacity,
      "schema=1 scope=observer connected=%u events=%llu dropped=%u publish_errors=%u",
      unsigned(connected.load()), static_cast<unsigned long long>(nextSequence.load()),
      dropped.load(), publicationErrors.load());
  if (size < 0 || size > 130 || size_t(size) >= capacity)
    snprintf(reply, capacity, "Error: observer stats response exceeds encrypted CLI capacity");
}
bool Observer::mintToken(const char *audience, char *output, size_t capacity,
                         char *error, size_t errorCapacity) const {
  const auto reject = [&](const char *message) { snprintf(error, errorCapacity, "%s", message); return false; };
  if (!output || !capacity) return reject("Observer token reply storage unavailable");
  output[0] = 0;
  if (!observerWire::dnsAudience(audience))
    return reject("Observer token audience must be a broker DNS name");
  const auto epoch = observationEpoch(0, true);
  if (!epoch) return reject("Observer token requires fresh network UTC; inspect get sntp.current");
  mesh::LocalIdentity loaded;
  const bool available = loadIdentity("observer", loaded);
  const bool same = available && !memcmp(loaded.pub_key, identity.public_key, sizeof(identity.public_key));
  const bool signedToken = same && observerWire::token(loaded, audience, epoch, output, capacity);
  auto *secret = reinterpret_cast<volatile uint8_t *>(&loaded);
  for (size_t i = 0; i < sizeof(loaded); ++i) secret[i] = 0;
  return signedToken || reject("Observer identity unavailable/changed or token signing failed");
}
void Observer::service() {
  Event e;
  const bool received = xQueueReceive(queue, &e, pdMS_TO_TICKS(100)) == pdTRUE;
  Snapshot snapshot;
  bool radioChanged = false;
  if (xQueueReceive(statusQueue, &snapshot, 0) == pdTRUE) {
    memcpy(latest, snapshot.roles, sizeof(latest));
    radioChanged = strcmp(radio, snapshot.radio) != 0;
    memcpy(radio, snapshot.radio, sizeof(radio));
    haveRoles = true;
  }
  const auto epoch = observationEpoch(0, true);
  if (settings.format) {
    if (epoch) lastReadyEpoch = epoch;
    if (client && (!epoch || (tokenExpires &&
        (epoch < tokenIssued ||
         epoch >= tokenExpires - observerWire::RENEWAL_MARGIN)))) {
      if (connected.load()) {
        const auto size = observerWire::status(
            "offline", identity.name, identityHex, ONCHIP_MQTT_MODEL,
            ONCHIP_MQTT_FIRMWARE_VERSION, radio, lastReadyEpoch,
            json, sizeof(json));
        if (size) publish(statusTopic, json, size, true, 1);
      }
      connected.store(false);
      announcedSession.store(0);
      esp_mqtt_client_stop(client);
      esp_mqtt_client_destroy(client);
      client = nullptr;
    }
    if (!client && epoch &&
        (!nextConnect || int32_t(millis() - nextConnect) >= 0)) {
      nextConnect = millis() + 5000;
      startClient(epoch);
    }
  }
  if (!connected.load()) {
    if (received)
      dropped.fetch_add(1);
    return;
  }
  const auto currentSession = session.load();
  const bool newSession = announcedSession.load() != currentSession;
  if (newSession || (settings.format &&
      (radioChanged || uint32_t(millis() - lastStatusAt) >= 60000u))) {
    const auto length = settings.format ?
        observerWire::status("online", identity.name, identityHex,
          ONCHIP_MQTT_MODEL, ONCHIP_MQTT_FIRMWARE_VERSION, radio, epoch,
          json, sizeof(json)) : 6;
    if (!length || !publish(statusTopic, settings.format ?
                           json : "online", length, true,
                           settings.format ? 1 : 0)) {
      if (received)
        dropped.fetch_add(1);
      return;
    }
    if (newSession) rolesPublished = false;
    lastStatusAt = millis();
    announcedSession.store(currentSession);
  }
  if (haveRoles && !settings.audience[0]) {
    bool success = true;
    char metadataIdentity[65];
    for (unsigned i = 0; i < 32; ++i)
      snprintf(metadataIdentity + 2 * i, 3, "%02x", identity.public_key[i]);
    for (unsigned i = 0; i < RadioDashboard::ROLE_CAPACITY; ++i) {
      if (rolesPublished &&
          !memcmp(&latest[i], &published[i], sizeof(latest[i])))
        continue;
      char topic[224];
      snprintf(topic, sizeof(topic), "%s/%s/roles/%s",
               settings.prefix, metadataIdentity, latest[i].role);
      const auto size =
          RadioDashboard::formatRoleJSON(latest[i], json, sizeof(json));
      if (!size) {
        publicationErrors.fetch_add(1);
        Serial.println("Observer role serialization overflow");
        success = false;
      } else if (!publish(topic, json, size, true))
        success = false;
      else
        published[i] = latest[i];
    }
    rolesPublished = success;
  }
  if (!received)
    return;
  publishPacket(e);
}
void Observer::publishPacket(const Event &e) {
  if (settings.format) {
    // Local transmissions and modem reflection remain internal observations,
    // never RF receptions on the public packet feed.
    if (e.tx) return;
    const auto size = observerWire::packet(
        e.raw, e.length, identity.name, identityHex, e.epoch, e.rssi, e.snr,
        settings.filter, json, sizeof(json), settings.format == 2);
    if (!size || !publish(packetTopic, json, size, false))
      dropped.fetch_add(1);
#ifdef ARDUINO_ARCH_ESP32
    else if (!stackReported) {
      Serial.printf("Observer stack minimum: worker=%u loop=%u bytes\n",
                    unsigned(uxTaskGetStackHighWaterMark(nullptr)),
                    unsigned(loopStackMinimum.load()));
      stackReported = true;
    }
#endif
    return;
  }
  char hex[511], signal[96], source[768], envelope[512], timestamp[40];
  for (unsigned i = 0; i < e.length; ++i)
    snprintf(hex + 2 * i, 3, "%02x", e.raw[i]);
  const bool loopback = !e.tx && e.snr == -32 && e.rssi == 127;
  const bool validRSSI = !e.tx && !loopback && std::isfinite(e.rssi) &&
                         e.rssi >= -128 && e.rssi <= 127;
  const bool validSNR = !e.tx && !loopback && std::isfinite(e.snr);
  char rssi[16], snr[64];
  if (validRSSI)
    snprintf(rssi, sizeof(rssi), "%d", int(std::lround(e.rssi)));
  else
    strcpy(rssi, "null");
  if (validSNR) {
    const int length = snprintf(snr, sizeof(snr), "%.2f", e.snr);
    if (length < 0 || size_t(length) >= sizeof(snr)) {
      dropped.fetch_add(1);
      Serial.println("Observer SNR serialization overflow");
      return;
    }
  } else
    strcpy(snr, "null");
  const int signalSize =
      snprintf(signal, sizeof(signal), "\"rssi\":%s,\"snr\":%s", rssi, snr);
  if (signalSize < 0 || size_t(signalSize) >= sizeof(signal)) {
    dropped.fetch_add(1);
    Serial.println("Observer signal serialization overflow");
    return;
  }
  strcpy(timestamp, "null");
  if (e.epoch) {
    const time_t seconds = e.epoch;
    struct tm utc;
    if (!gmtime_r(&seconds, &utc) ||
        !strftime(timestamp, sizeof(timestamp), "\"%Y-%m-%dT%H:%M:%SZ\"",
                  &utc)) {
      dropped.fetch_add(1);
      Serial.println("Observer timestamp serialization failed");
      return;
    }
  }
  if (!formatEnvelope(e.raw, e.length, envelope, sizeof(envelope))) {
    dropped.fetch_add(1);
    Serial.println("Observer packet envelope serialization overflow");
    return;
  }
  if (!RadioDashboard::formatRoleJSON(e.source, source, sizeof(source))) {
    dropped.fetch_add(1);
    Serial.println("Observer packet source serialization overflow");
    return;
  }
  const int size = snprintf(
      json, sizeof(json),
      "{\"event_id\":\"%s-%llu\",\"timestamp\":%s,"
      "\"observer_identity\":\"%s\",\"uptime_ms\":%u,\"direction\":\"%s\","
      "\"tx_state\":%u,%s,\"local_loopback\":%s,\"dropped\":%u,"
      "\"publication_errors\":%u,\"source\":%s,\"raw_packet_hex\":\"%s\","
      "\"raw\":\"%s\",%s}",
      eventPrefix, static_cast<unsigned long long>(e.sequence), timestamp,
      identityHex, e.at, e.tx ? "tx" : "rx", e.state, signal,
      loopback ? "true" : "false", dropped.load(), publicationErrors.load(),
      e.tx ? source : "null", hex, hex, envelope);
  if (size <= 0 || size_t(size) >= sizeof(json)) {
    dropped.fetch_add(1);
    Serial.println("Observer packet serialization overflow");
  } else if (!publish(packetTopic, json, size, false))
    dropped.fetch_add(1);
}
void Observer::worker(void *arg) {
  auto &self = *static_cast<Observer *>(arg);
  for (;;)
    self.service();
}
} // namespace onchip
