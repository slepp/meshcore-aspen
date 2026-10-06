// SPDX-License-Identifier: Apache-2.0
#include "Observer.h"
#include "Clock.h"
#include <fstream>

#ifdef ONCHIP_CLOCK_TEST_BUSY
static unsigned observerClockBusyReads = 0;
static bool observerClockBusy = false;
bool onchipClockTestBusy() {
  if (observerClockBusy) return true;
  if (!observerClockBusyReads) return false;
  --observerClockBusyReads;
  return true;
}
#endif

namespace onchip {
struct ObserverTest {
  static void service(Observer &observer) { observer.service(); }
  static void connect(Observer &observer, bool online) {
    auto c = observer.client;
    c->event(c->context, nullptr,
             online ? MQTT_EVENT_CONNECTED : MQTT_EVENT_DISCONNECTED, nullptr);
  }
  static void close(Observer &observer) {
    esp_mqtt_client_destroy(observer.client);
    vQueueDelete(observer.queue);
    vQueueDelete(observer.statusQueue);
    observer.client = nullptr;
    observer.queue = observer.statusQueue = nullptr;
  }
  static unsigned drops(const Observer &observer) {
    return observer.dropped.load();
  }
  static TestMQTT *client(const Observer &observer) { return observer.client; }
  static uint32_t epoch(const Observer &observer, uint32_t at) {
    return observer.observationEpoch(at);
  }
  static uint32_t sampleAt(const Observer &observer) {
    return uint32_t(observer.acceptedClock.load());
  }
  static uint32_t issued(const Observer &observer) { return observer.tokenIssued; }
  static void sourced(Observer &observer, const uint8_t *raw, uint16_t length,
                      const RadioDashboard::RoleStatus &source) {
    auto &entry = observer.pending[0];
    assert(!entry.used);
    entry.used = true;
    entry.slot = 250;
    entry.generation = 42;
    entry.job = 21;
    entry.source = source;
    observer.transmitted(raw, length, queued_tx::SUCCEEDED, 250, 42, 21);
  }
  static void will(const Observer &observer, const std::string &base) {
    assert(observer.client->willTopic == base + "/status");
    assert(observer.client->will == "offline" && observer.client->retain);
    assert(observer.client->willQos == 0);
    assert(observer.client->identity == base.substr(9));
  }
};
} // namespace onchip
static void public_observer_service() {
  onchip::beginClocks();
  onchip::Observer observer;
  Fixture f;
  assert(observer.begin(f.mux));
  assert(!onchip::ObserverTest::client(observer));
  onchip::ObserverTest::service(observer);
  assert(!onchip::ObserverTest::client(observer));
  const uint8_t raw[] = {0x17, 1, 2, 3, 4, 0x82, 0xaa, 0xbb, 0xcc,
                         0x12, 0x34, 0x56, 0x51, 0x52, 0x53};
  observer.packet(raw, sizeof(raw), false, 0, -90, 4.5);
  onchip::receiveNetworkTime(1767225600u);
  onchip::loopClocks();
  now += 1500;
  onchip::loopClocks();
  assert(onchip::ObserverTest::epoch(observer, now - 1) == 1767225601u);
  assert(onchip::ObserverTest::epoch(observer, now + 999) == 1767225602u);
  RadioDashboard::RadioStatus status{};
  observer.observeRoles(status);
  const auto firstIssued = onchip::ObserverTest::epoch(observer, now);
  onchip::ObserverTest::service(observer);
  auto client = onchip::ObserverTest::client(observer);
  assert(client && client->username.find("v1_") == 0);
  const auto firstToken = client->password;
  assert(firstToken.find("eyJhbGciOiJFZDI1NTE5IiwidHlwIjoiSldUIn0.") == 0);
  assert(client->will.find("\"status\":\"offline\"") != std::string::npos);
  assert(client->willQos == 1 && client->retain);
  assert(client->willTopic.find("meshcore/YYC/") == 0);
  onchip::ObserverTest::connect(observer, true);
  mqtt_test::messages.clear();
  onchip::ObserverTest::service(observer);
  assert(mqtt_test::messages.size() == 1);
  assert(mqtt_test::messages.back().data.find("\"status\":\"online\"") != std::string::npos);
  assert(mqtt_test::messages.back().qos == 1 && mqtt_test::messages.back().retained);
  for (bool tx : {false, true}) {
    observer.packet(raw, sizeof(raw), tx, queued_tx::SUCCEEDED, -90, 4.5);
    onchip::ObserverTest::service(observer);
  }
  assert(mqtt_test::messages.size() == 2);
  assert(mqtt_test::messages.back().data.find("\"hash\":\"D20D9B3795B107AC\"") != std::string::npos);
  assert(mqtt_test::messages.back().qos == 0 && !mqtt_test::messages.back().retained);
  observer.packet(raw, sizeof(raw), false, 0, 127, -32);
  onchip::ObserverTest::service(observer);
  assert(mqtt_test::messages.size() == 2);
#ifdef ONCHIP_CLOCK_TEST_BUSY
  const uint32_t capturedAt = now;
  ++now;
  onchip::receiveNetworkTime(firstIssued + 1);
  onchip::loopClocks();
  assert(onchip::ObserverTest::epoch(observer, capturedAt) == firstIssued);
  assert(onchip::ObserverTest::epoch(observer, now) == firstIssued + 1);
  // Retry a contended reader, then retain the accepted sample while busy.
  observerClockBusyReads = 2;
  onchip::ObserverTest::service(observer);
  assert(observerClockBusyReads == 0);
  assert(onchip::ObserverTest::client(observer) == client);
  observerClockBusy = true;
  observer.packet(raw, sizeof(raw), false, 0, -90, 4.5);
  onchip::ObserverTest::service(observer);
  assert(onchip::ObserverTest::client(observer) == client);
  assert(mqtt_test::messages.size() == 3);
  assert(mqtt_test::messages.back().data.find("\"time\":\"00:00:02\"") != std::string::npos);
  now += 3501;
  observer.packet(raw, sizeof(raw), false, 0, -90, 4.5);
  onchip::ObserverTest::service(observer);
  assert(onchip::ObserverTest::client(observer) == client);
  assert(mqtt_test::messages.size() == 4);
  observerClockBusy = false;
  // A readable but briefly stale dispatch publication has the same grace.
  onchip::ObserverTest::service(observer);
  assert(onchip::ObserverTest::client(observer) == client);
  assert(onchip::ObserverTest::epoch(observer, now) >= firstIssued + 3);
  observerClockBusy = true;
  now += 15001;
  onchip::ObserverTest::service(observer);
  assert(!onchip::ObserverTest::client(observer));
  assert(!onchip::ObserverTest::epoch(observer, now));
  observerClockBusy = false;
  onchip::loopClocks();
  onchip::ObserverTest::service(observer);
  client = onchip::ObserverTest::client(observer);
  assert(client);
  onchip::ObserverTest::connect(observer, true);
  onchip::ObserverTest::service(observer);
  // A real accepted backward correction revokes the existing token.
  const auto beforeCorrection = client->password;
  now += 5001;
  onchip::receiveNetworkTime(1767225600u);
  onchip::loopClocks();
  onchip::ObserverTest::service(observer);
  client = onchip::ObserverTest::client(observer);
  assert(client && client->password != beforeCorrection);
  assert(onchip::ObserverTest::issued(observer) == 1767225600u);
  onchip::ObserverTest::connect(observer, true);
  onchip::ObserverTest::service(observer);
  puts("PASS observer clock: busy retries/cache, capture race, short stale publication, sustained unavailability, backward correction");
#endif
  // Refresh UTC rather than advancing a stale SNTP sample for a whole day.
  now += 5001;
  onchip::receiveNetworkTime(firstIssued + 86400u - 300u);
  onchip::loopClocks();
  onchip::ObserverTest::service(observer);
  client = onchip::ObserverTest::client(observer);
  assert(client && client->password != firstToken);
  onchip::ObserverTest::connect(observer, true);
  onchip::ObserverTest::service(observer);
  assert(mqtt_test::messages.back().data.find("\"status\":\"online\"") != std::string::npos);
  now += 7201000;
  onchip::loopClocks();
  onchip::ObserverTest::service(observer);
  assert(!onchip::ObserverTest::client(observer));
  assert(mqtt_test::messages.back().data.find("\"status\":\"offline\"") != std::string::npos);
  assert(mqtt_test::messages.back().qos == 1 && mqtt_test::messages.back().retained);
  onchip::ObserverTest::close(observer);
  puts("PASS public observer service: clock gate, identity CONNECT, renewal, JSON status/LWT, RF-only output");
}
static std::string publicHex(const RadioDashboard::RoleStatus &role) {
  char hex[65];
  for (unsigned i = 0; i < 32; ++i)
    snprintf(hex + 2 * i, 3, "%02x", role.public_key[i]);
  return hex;
}
static onchip::Management *dashboardManagement;
static void role_snapshot(RadioDashboard::RadioStatus &status,
                          onchip::Observer &observer) {
  status.role_count = 6;
  for (unsigned i = 0; i < 3; ++i)
    onchip::roleStatus(Role(i), status.roles[i]);
  observer.dashboardStatus(status.roles[3]);
  mesh::LocalIdentity bot;
  assert(onchip::loadIdentity("modem", bot));
  auto &entry = status.roles[4];
  strcpy(entry.role, "bot");
  strcpy(entry.name, "MeshCore KISS");
  strcpy(entry.state, "running");
  entry.ready = true;
  entry.has_identity = true;
  memcpy(entry.public_key, bot.pub_key, 32);
  dashboardManagement->dashboardStatus(status.roles[5]);
}
static void observer_and_dashboard(const char *outputPath) {
  onchip::beginClocks();
  onchip::Observer observer;
  onchip::Observer restored;
  Fixture f;
  onchip::Management receiver;
  assert(receiver.begin(f.mux, ""));
  dashboardManagement = &receiver;
  assert(observer.begin(f.mux));
  RadioDashboard::RadioStatus status;
  f.mux.dashboardStatus(status);
  role_snapshot(status, observer);
  const auto base = std::string("meshcore/") + publicHex(status.roles[3]);
  onchip::ObserverTest::will(observer, base);
  assert(!status.roles[3].ready &&
         !strcmp(status.roles[3].state, "connecting"));
  const auto persisted = stored("observer"), bot = stored("modem");
  for (unsigned i = 0; i < 6; ++i) {
    assert(status.roles[i].has_identity);
    for (unsigned j = 0; j < i; ++j)
      assert(
          memcmp(status.roles[i].public_key, status.roles[j].public_key, 32));
  }
  observer.observeRoles(status);
  onchip::ObserverTest::connect(observer, true);
  onchip::ObserverTest::service(observer);
  assert(mqtt_test::messages.size() == 7);
  assert(mqtt_test::messages.front().topic == base + "/status" &&
         mqtt_test::messages.front().data == "online" &&
         mqtt_test::messages.front().retained &&
         mqtt_test::messages.front().qos == 0);
  for (unsigned i = 0; i < 6; ++i) {
    const auto &m = mqtt_test::messages[i + 1];
    assert(m.retained && m.topic == base + "/roles/" + status.roles[i].role);
    assert(m.data.find("\"public_key\":\"" + publicHex(status.roles[i]) +
                       "\"") != std::string::npos);
  }
  role_snapshot(status, observer);
  assert(status.roles[3].ready);
  observer.observeRoles(status);
  onchip::ObserverTest::service(observer);
  const auto stable = mqtt_test::messages.size();
  observer.observeRoles(status);
  onchip::ObserverTest::service(observer);
  assert(mqtt_test::messages.size() == stable);

  // Capture attribution at admission, not from a replacement role at
  // completion.
  const auto original = status.roles[0];
  const uint8_t packet[255] = {0x42};
  uint32_t job;
  assert(onchip::repeaterRadio().queueTransmit(packet, sizeof(packet), 0, 0, 0,
                                               job));
  f.radio.complete = false;
  f.radio.airtime = 60000;
  f.mux.serviceTransmit();
  mesh::LocalIdentity replacement(&f.rng);
  assert(onchip::stageIdentity(Role::Repeater, replacement));
  assert(onchip::requestLifecycle(Role::Repeater, LifecycleAction::Reboot));
  f.step(2);
  RadioDashboard::RoleStatus current;
  onchip::roleStatus(Role::Repeater, current);
  assert(current.ready &&
         current.source_generation != original.source_generation &&
         memcmp(current.public_key, original.public_key, 32));
  f.radio.complete = true;
  f.mux.serviceTransmit();
  onchip::ObserverTest::service(observer);
  const auto &tx = mqtt_test::messages.back();
  assert(tx.topic == base + "/packets" && !tx.retained);
  assert(tx.data.find("\"observer_identity\":\"" + publicHex(status.roles[3]) +
                      "\"") != std::string::npos);
  assert(tx.data.find("\"public_key\":\"" + publicHex(original) + "\"") !=
         std::string::npos);
  assert(tx.data.find("\"source_generation\":" +
                      std::to_string(original.source_generation)) !=
         std::string::npos);
  assert(tx.data.find("\"raw_packet_hex\":\"42" + std::string(508, '0') +
                      "\"") != std::string::npos);
  assert(tx.data.find("\"rssi\":null,\"snr\":null") != std::string::npos);
  assert(tx.data.find("\"timestamp\":null") != std::string::npos);
  assert(tx.data.size() < 3072);
  assert(tx.data.find("\"decode_error\":\"unsupported payload version 1\"") !=
         std::string::npos);
  assert(tx.data.find("\"packet\":") == std::string::npos);
  const auto eventStart = tx.data.find("\"event_id\":\"");
  assert(eventStart != std::string::npos);
  const auto eventID = tx.data.substr(eventStart + 12, 34);
  assert(eventID.size() == 34 && eventID[32] == '-' &&
         eventID.substr(0, 32).find_first_not_of("0123456789abcdef") ==
             std::string::npos && eventID[33] == '1');
  observer.packet(packet, 2, false, 0, -93, 4.5);
  onchip::receiveNetworkTime(1767225600u);
  onchip::loopClocks();
  onchip::ObserverTest::service(observer);
  assert(mqtt_test::messages.back().data.find("\"rssi\":-93,\"snr\":4.50") !=
         std::string::npos);
  assert(mqtt_test::messages.back().data.find("\"timestamp\":null") !=
         std::string::npos);
  assert(mqtt_test::messages.back().data.find("\"source\":null") !=
         std::string::npos);
  assert(mqtt_test::messages.back().data.find("\"event_id\":\"" +
              eventID.substr(0, 33) + "2\"") != std::string::npos);
  const uint8_t valid[] = {0x15, 0x42, 0xaa, 0xbb, 0xcc, 0xdd, 0x77, 0x88};
  observer.packet(valid, sizeof(valid), false, 0, -97.3f, 6.25f);
  onchip::ObserverTest::service(observer);
  const auto &decoded = mqtt_test::messages.back().data;
  assert(decoded.find("\"timestamp\":\"2026-01-01T00:00:00Z\"") !=
         std::string::npos);
  assert(decoded.find("\"rssi\":-97,\"snr\":6.25") != std::string::npos);
  assert(decoded.find("\"packet\":{\"header\":21,\"type\":5,"
                      "\"type_name\":\"GRP_TXT\",\"route\":1,"
                      "\"route_name\":\"FLOOD\",\"version\":0,"
                      "\"path_hex\":\"aabbccdd\",\"path_length\":66,"
                      "\"path_hash_size\":2,\"path_hop_count\":2}") !=
         std::string::npos);
  assert(decoded.find("\"decode_error\":") == std::string::npos);
  const uint8_t reflected[] = {0x0e, 0, 1, 2, 3, 4};
  observer.packet(reflected, sizeof(reflected), false, 0, 127, -32);
  onchip::ObserverTest::service(observer);
  const auto &loopback = mqtt_test::messages.back().data;
  assert(loopback.find("\"local_loopback\":true") != std::string::npos &&
         loopback.find("\"rssi\":null,\"snr\":null") != std::string::npos &&
         loopback.find("\"type_name\":\"ACK\"") != std::string::npos);
  const uint8_t shortPath[] = {0x15, 3, 0xaa};
  observer.packet(shortPath, sizeof(shortPath), false, 0, -90, 0);
  onchip::ObserverTest::service(observer);
  assert(mqtt_test::messages.back().data.find(
             "\"decode_error\":\"not enough data for path: need 3 bytes, have 1\"") !=
         std::string::npos);
  const uint8_t shortTransport[] = {0, 1, 2};
  observer.packet(shortTransport, sizeof(shortTransport), false, 0, -90, 0);
  onchip::ObserverTest::service(observer);
  assert(mqtt_test::messages.back().data.find(
             "\"decode_error\":\"reading transport codes: short frame\"") !=
         std::string::npos);
  uint8_t large[255]{};
  large[0] = 0x3b; // TRANSPORT_DIRECT, multipart; four transport bytes.
  large[5] = 0x79; // Two-byte hashes, 57 hops: invalid length (>64).
  observer.packet(large, sizeof(large), false, 0, -90, 0);
  onchip::ObserverTest::service(observer);
  assert(mqtt_test::messages.back().data.find(
             "\"decode_error\":\"invalid path length byte: 0x79\"") !=
         std::string::npos);
  large[5] = 0;
  observer.packet(large, sizeof(large), false, 0, -90, 0);
  onchip::ObserverTest::service(observer);
  assert(mqtt_test::messages.back().data.find(
             "\"decode_error\":\"payload too large: 249 bytes, max 184\"") !=
         std::string::npos);
  for (unsigned i = 6; i < 70; ++i)
    large[i] = uint8_t(i);
  large[5] = 0x3f; // 63 path bytes; 186-byte payload, still too large.
  observer.packet(large, sizeof(large), false, 0, -90, 0);
  onchip::ObserverTest::service(observer);
  assert(mqtt_test::messages.back().data.find(
             "\"decode_error\":\"payload too large: 186 bytes, max 184\"") !=
         std::string::npos);
  observer.packet(large, 253, false, 0, -90, 0);
  onchip::ObserverTest::service(observer);
  assert(mqtt_test::messages.back().data.size() < 3072);
  assert(mqtt_test::messages.back().data.find("\"type_name\":\"TRACE\"") ==
         std::string::npos);
  assert(mqtt_test::messages.back().data.find(
             "\"packet\":{\"header\":59,\"type\":14,"
             "\"type_name\":\"\",\"route\":3,"
             "\"route_name\":\"TRANSPORT_DIRECT\"") != std::string::npos);
  assert(mqtt_test::messages.back().data.find("\"path_hop_count\":63}") !=
         std::string::npos);
  auto verbose = original;
  memset(verbose.name, 'n', sizeof(verbose.name) - 1);
  verbose.name[sizeof(verbose.name) - 1] = 0;
  memset(verbose.fault, 'f', sizeof(verbose.fault) - 1);
  verbose.fault[sizeof(verbose.fault) - 1] = 0;
  const auto beforeLargeTx = onchip::ObserverTest::drops(observer);
  onchip::ObserverTest::sourced(observer, large, 253, verbose);
  onchip::ObserverTest::service(observer);
  const auto &largeTx = mqtt_test::messages.back().data;
  assert(onchip::ObserverTest::drops(observer) == beforeLargeTx &&
         largeTx.size() < 3072);
  assert(largeTx.find("\"direction\":\"tx\"") != std::string::npos);
  assert(largeTx.find("\"source\":{\"role\":\"repeater\"") !=
         std::string::npos);
  assert(largeTx.find("\"path_hop_count\":63}") != std::string::npos);
  assert(onchip::ObserverTest::epoch(observer, onchip::ObserverTest::sampleAt(observer) - 1) == 0);
  onchip::ObserverTest::connect(observer, false);
  observer.dashboardStatus(current);
  assert(!current.ready && !strcmp(current.state, "connecting"));
  for (unsigned i = 0; i < 9; ++i)
    observer.packet(packet, 2, false, 0, -93, 4);
  assert(onchip::ObserverTest::drops(observer) == 1);
  for (unsigned i = 0; i < 8; ++i)
    onchip::ObserverTest::service(observer);
  assert(onchip::ObserverTest::drops(observer) == 9);
  const auto beforeReconnect = mqtt_test::messages.size();
  onchip::ObserverTest::connect(observer, true);
  onchip::ObserverTest::service(observer);
  assert(mqtt_test::messages.size() == beforeReconnect + 7);
  mqtt_test::failPublish = true;
  observer.packet(packet, 2, false, 0, -93, 4);
  onchip::ObserverTest::service(observer);
  assert(onchip::ObserverTest::drops(observer) == 10);
  mqtt_test::failPublish = false;
  onchip::ObserverTest::connect(observer, false);
  onchip::ObserverTest::connect(observer, true);
  mqtt_test::failPublish = true;
  onchip::ObserverTest::service(observer);
  observer.dashboardStatus(current);
  assert(!current.ready && onchip::ObserverTest::drops(observer) == 10);
  mqtt_test::failPublish = false;
  mqtt_test::reconnectDuringPublish = true;
  onchip::ObserverTest::service(observer);
  observer.dashboardStatus(current);
  assert(!current.ready);
  onchip::ObserverTest::service(observer);
  observer.dashboardStatus(current);
  assert(current.ready);
  const auto messagesBeforeSignalError = mqtt_test::messages.size();
  observer.packet(packet, 2, false, 0, std::numeric_limits<float>::max(),
                  std::numeric_limits<float>::max());
  onchip::ObserverTest::service(observer);
  assert(onchip::ObserverTest::drops(observer) == 10 &&
         mqtt_test::messages.size() == messagesBeforeSignalError + 1);
  assert(mqtt_test::messages.back().data.find("\"rssi\":null,\"snr\":") !=
         std::string::npos);
  assert(mqtt_test::messages.back().data.find(
             "340282346638528859811704183484516925440.00") !=
         std::string::npos);

  identity_test::failRead = true;
  assert(onchip::requestLifecycle(Role::Room, LifecycleAction::Reboot));
  f.step(2);
  onchip::roleStatus(Role::Room, current);
  assert(!current.ready && !current.has_identity && current.source_slot == -1 &&
         !strcmp(current.state, "fault") &&
         !strcmp(current.fault, "identity read failed"));
  identity_test::failRead = false;
  f.step(253);
  onchip::roleStatus(Role::Room, current);
  assert(current.ready && current.has_identity && !current.fault[0]);
  Client names;
  names.handshake(f);
  names.command({8, 'c', 'a', 'f', 0xc3, 0xa9});
  assert(names.response(f) == Bytes{0});

  role_snapshot(status, observer);
  RadioDashboard dashboard;
  dashboard.publish(millis(), status);
  RadioDashboard::Snapshot snapshot;
  assert(dashboard.snapshot(snapshot));
  char json[RadioDashboard::JSON_CAPACITY];
  auto size =
      RadioDashboard::formatJSON(snapshot, "onchip-test", json, sizeof(json));
  assert(size && strstr(json, "\"roles\":[") &&
         strstr(json, "\"role\":\"bot\"") &&
         strstr(json, "\"profile_generation\":\"0\""));
  assert(strstr(json, "\"name\":\"caf\xc3\xa9\""));
  std::ofstream file(outputPath);
  file.write(json, size);
  assert(file.good());
  // Worst-size snapshots retain all bounded history, traffic, names and fault
  // text.
  snapshot.event_count = RadioDashboard::HISTORY;
  snapshot.uptime_ms = UINT32_MAX;
  snapshot.rx_packets = snapshot.rx_estimated_ms = snapshot.tx_accepted =
      snapshot.tx_rejected = snapshot.tx_succeeded = snapshot.tx_failed =
          snapshot.tx_unknown = snapshot.tx_rf_ms = UINT64_MAX;
  for (uint64_t second = snapshot.uptime_ms / 1000 - 59;
       second <= snapshot.uptime_ms / 1000; ++second) {
    auto &bucket = snapshot.traffic[second % RadioDashboard::TRAFFIC_SECONDS];
    bucket.second = second;
    bucket.rx_packets = bucket.tx_packets = bucket.tx_rf_ms =
        bucket.rx_estimated_ms = UINT32_MAX;
  }
  snapshot.radio.clients = KISS_MAX_TCP_CLIENTS;
  for (auto &client : snapshot.radio.client) {
    client.connected = client.negotiated = true;
    client.generation = client.credit_ms = client.rf_ms = UINT32_MAX;
    client.factor = std::numeric_limits<float>::max();
  }
  for (auto &e : snapshot.events) {
    e.sequence = e.at_ms = UINT64_MAX;
    e.length = 255;
    e.preview_length = RadioDashboard::PREVIEW_BYTES;
    e.source_generation = e.queue_ms = e.rf_ms = e.estimated_ms = UINT32_MAX;
  }
  for (auto &r : snapshot.radio.roles) {
    memset(r.name, '\n', sizeof(r.name) - 1);
    memset(r.fault, '\n', sizeof(r.fault) - 1);
    r.profile_generation = UINT64_MAX;
  }
  size =
      RadioDashboard::formatJSON(snapshot, "onchip-test", json, sizeof(json));
  assert(size && strstr(json, "\"profile_generation\":\"18446744073709551615\""));
  char small[5] = "test";
  assert(!RadioDashboard::formatRoleJSON(current, small, sizeof(small)) &&
         !small[0]);
  onchip::ObserverTest::close(observer);
  assert(restored.begin(f.mux));
  restored.dashboardStatus(current);
  assert(publicHex(current) == base.substr(9) &&
         stored("observer") == persisted && stored("modem") == bot);
  onchip::ObserverTest::close(restored);
  receiver.stop();
  dashboardManagement = nullptr;
  puts("PASS six-service public identities, bounded snapshots and "
       "source-attributed MQTT");
}
