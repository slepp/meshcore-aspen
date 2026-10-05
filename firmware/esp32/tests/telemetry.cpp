// SPDX-License-Identifier: Apache-2.0
#include "Telemetry.h"
#include <nvs.h>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <limits>
using namespace onchip;
unsigned long millis() { return 0; }
void delay(unsigned long) {}

struct Sink : TelemetrySink {
  bool accept = true, done = false;
  unsigned submits = 0, cancels = 0;
  std::string body;
  TelemetryCompletion result;
  bool submit(const char *text, size_t size) override {
    if (!accept) return false;
    ++submits; body.assign(text, size); return true;
  }
  bool poll(TelemetryCompletion &completion) override {
    if (!done) return false;
    completion = result; done = false; return true;
  }
  void cancel() override { ++cancels; }
};
static TelemetrySample sample() {
  TelemetrySample s;
  s.totals.uptime_ms = 123456;
  s.totals.rx_packets = 17;
  s.totals.tx_succeeded = 11;
  s.totals.tx_rejected = 3;
  s.totals.tx_unknown = 1;
  s.radio.rx_errors = 2;
  s.radio.generation = 1;
  queued_tx::put32(s.radio.profile, 912525000);
  queued_tx::put32(s.radio.profile + 4, 250000);
  s.radio.profile[8] = 7;
  s.radio.profile[9] = 5;
  s.radio.profile[10] = 2;
  s.radio.role_count = RadioDashboard::ROLE_CAPACITY;
  s.radio.roles[0].ready = true;
  strcpy(s.radio.roles[0].name, "PRIVATE NAME");
  memset(s.radio.roles[0].public_key, 42, 32);
  strcpy(s.radio.roles[0].fault, "PRIVATE ERROR");
  s.role[0].available = true;
  s.role[0].succeeded = 9;
  return s;
}
static void encoding() {
  auto s = sample();
  char body[TelemetryBodyLimit + 1];
  auto size = encodeTelemetry(s, "esp32-aabbccddeeff", "Aspen one,=\\", {}, body, sizeof(body));
  assert(size && size == strlen(body));
  assert(strstr(body, "name=Aspen\\ one\\,\\=\\\\ schema=1i\n"));
  assert(strstr(body, "uptime_seconds=123.456"));
  assert(strstr(body, "rx_packets_total=17i"));
  assert(strstr(body, "tx_succeeded_total=11i"));
  assert(strstr(body, ",role=repeater ready=1i"));
  assert(strstr(body, "tx_succeeded_total=9i"));
  assert(strstr(body, "frequency_hz=912525000i,bandwidth_hz=250000i"));
  assert(!strstr(body, "battery") && !strstr(body, "temperature"));
  assert(!strstr(body, "wifi_rssi") && !strstr(body, "psram_"));
  assert(!strstr(body, "PRIVATE") && !strstr(body, "public_key"));
  assert(!strstr(body, "meshcore_lua"));
  const std::string output(body);
  size_t lines = 0;
  for (auto c : output) if (c == '\n') ++lines;
  assert(lines == 4 + RadioDashboard::ROLE_CAPACITY);
  char small[20];
  assert(!encodeTelemetry(s, "device", "Aspen", {}, small, sizeof(small)) && !small[0]);
  assert(!encodeTelemetry(s, "bad\nid", "Aspen", {}, body, sizeof(body)) && !body[0]);
  assert(!encodeTelemetry(s, "device", "bad\rname", {}, body, sizeof(body)));
  assert(!encodeTelemetry(s, "", "Aspen", {}, body, sizeof(body)));
  assert(!encodeTelemetry(s, "device", "Aspen", {}, body, 0));
  s.batteryMv = 4100;
  s.temperatureC = -10.5;
  s.radio.wifi_connected = true;
  s.radio.wifi_rssi = -61;
  s.psramTotal = 8388608;
  s.psramFree = 8123456;
  s.lua.available = true;
  assert(encodeTelemetry(s, "device", "Aspen", {}, body, sizeof(body)));
  assert(strstr(body, "battery_volts=4.100"));
  assert(strstr(body, "mcu_temperature_celsius=-10.500"));
  assert(strstr(body, "wifi_rssi_dbm=-61i"));
  assert(strstr(body, "psram_free_bytes=8123456i"));
  assert(!strstr(body, "last_vm_peak_bytes"));
  s.temperatureC = INFINITY;
  s.lua.vmAvailable = true;
  s.lua.peakBytes = UINT32_MAX;
  s.lua.elapsedUs = UINT64_MAX;
  s.lua.loadUs = 300000;
  s.lua.initUs = 40000;
  s.lua.invokeUs = 12000;
  s.lua.cleanupUs = 1000;
  s.totals.rx_packets = INT64_MAX;
  for (auto &role : s.role) {
    role.available = true;
    role.generation = role.creditMs = role.rfMs = role.succeeded = role.failed = UINT32_MAX;
    role.queued = UINT8_MAX;
  }
  assert(encodeTelemetry(s, "esp32-aabbccddeeff", "1234567890123456789012345678901", {}, body, sizeof(body)));
  assert(!strstr(body, "temperature") && strstr(body, "last_vm_peak_bytes=4294967295i"));
  assert(strstr(body, "last_vm_load_seconds=0.300000,last_vm_init_seconds=0.040000,"));
  assert(strstr(body, "last_vm_invoke_seconds=0.012000,last_vm_cleanup_seconds=0.001000,"));
  printf("PASS line protocol, sensor absence, escaping, whitelist, full-role body %zu/%zu bytes\n",
         strlen(body), TelemetryBodyLimit);
  for (auto &peer : s.repeaters) {
    peer.configured = peer.available = peer.fresh = true;
    strcpy(peer.alias, "abcdefghijklmnop");
    peer.ageSeconds = peer.attempts = peer.failures = UINT32_MAX;
    auto &v = peer.stats;
    v.batteryMv = v.queued = v.errors = v.directDuplicates = v.floodDuplicates = UINT16_MAX;
    v.noise = v.rssi = v.snrQuarterDb = INT16_MIN;
    v.received = v.sent = v.txSeconds = v.uptimeSeconds = v.sentFlood = v.sentDirect =
        v.receivedFlood = v.receivedDirect = v.rxSeconds = v.receiveErrors = UINT32_MAX;
  }
  assert(encodeTelemetry(s, "esp32-aabbccddeeff", "1234567890123456789012345678901", {}, body, sizeof(body)));
  assert(strstr(body, "meshcore_repeater,device=esp32-aabbccddeeff,peer=abcdefghijklmnop"));
  assert(strstr(body, "battery_volts=65.535") && strstr(body, "error_flags=65535i"));
  assert(!strstr(body, "battery_percent") && !strstr(body, "public_key"));
  printf("PASS worst-case local and three remote samples %zu/%zu bytes\n", strlen(body), TelemetryBodyLimit);
  s = sample();
  auto &peer = s.repeaters[0];
  peer.configured = peer.available = true; peer.fresh = false;
  strcpy(peer.alias, "offline"); peer.ageSeconds = 700;
  peer.error = BotRepeaterError::Timeout; peer.stats.batteryMv = 3811;
  assert(encodeTelemetry(s, "device", "Aspen", {}, body, sizeof(body)));
  assert(strstr(body, "available=1i,fresh=0i,error_code=7i"));
  assert(strstr(body, "sample_age_seconds=700i") && !strstr(body, "battery_volts"));
}
static void settings() {
  identity_test::durable.clear();
  TelemetryConfig config;
  assert(loadTelemetryConfig(config) && !config.enabled && config.intervalSeconds == 60);
  config.enabled = true; config.intervalSeconds = 30;
  assert(saveTelemetryConfig(config));
  TelemetryConfig reloaded;
  assert(loadTelemetryConfig(reloaded) && reloaded.enabled && reloaded.intervalSeconds == 30);
  config.enabled = false;
  assert(saveTelemetryConfig(config));
  assert(loadTelemetryConfig(reloaded) && !reloaded.enabled);
  config.intervalSeconds = 29; assert(!saveTelemetryConfig(config));
  config.intervalSeconds = 86401; assert(!saveTelemetryConfig(config));
  config.intervalSeconds = 60;
  identity_test::failCommit = true; assert(!saveTelemetryConfig(config));
  identity_test::failCommit = false;
  identity_test::failRead = true; assert(!saveTelemetryConfig(config));
  assert(!loadTelemetryConfig(reloaded) && !reloaded.enabled);
  identity_test::failRead = false;
  identity_test::durable[{"mc-onchip", "telemetry"}][4] = 2;
  assert(!loadTelemetryConfig(reloaded) && !reloaded.enabled);
  assert(identity_test::handles.empty());
  puts("PASS persisted opt-in, disable, bounds, corruption, commit/readback failure, NVS cleanup");
}
static void publisher() {
  Sink sink;
  TelemetryPublisher publisher(sink);
  assert(!publisher.due(100000));
  TelemetryConfig config; config.enabled = true;
  publisher.configure(config, 0);
  assert(!publisher.due(59999) && publisher.due(60000));
  publisher.drop(60000, TelemetryError::Wifi);
  assert(publisher.status().dropped == 1 && publisher.status().attempts == 0 &&
         publisher.status().lastDropMs == 60000);
  assert(publisher.due(120000));
  sink.accept = false; publisher.publish(120000, "a x=1i\n", 7);
  assert(publisher.status().error == TelemetryError::Busy && publisher.status().dropped == 2);
  sink.accept = true; publisher.publish(180000, "a x=2i\n", 7);
  assert(publisher.status().pending && publisher.status().lastAttemptMs == 180000);
  publisher.publish(180001, "a x=3i\n", 7);
  assert(sink.submits == 1 && sink.body == "a x=2i\n");
  sink.result = {true, 204, TelemetryError::None}; sink.done = true;
  publisher.poll(180100);
  assert(!publisher.status().pending && publisher.status().successes == 1 &&
         publisher.status().lastSuccessMs == 180100);
  uint64_t now = publisher.status().nextMs;
  for (unsigned i = 0; i < 9; ++i) {
    publisher.publish(now, "a x=4i\n", 7);
    sink.result = {false, 503, TelemetryError::Http}; sink.done = true;
    publisher.poll(now + 1);
    assert(!publisher.status().suspended && publisher.status().backoffSeconds <= 3600);
    assert(!publisher.due(publisher.status().nextMs - 1));
    now = publisher.status().nextMs;
  }
  assert(publisher.status().backoffSeconds == 3600 && publisher.status().failures == 9);
  publisher.publish(now, "a x=5i\n", 7);
  sink.result = {false, 401, TelemetryError::Http}; sink.done = true; publisher.poll(now + 1);
  assert(publisher.status().suspended && !publisher.due(now + 100000000));
  publisher.configure(config, now);
  now = publisher.status().nextMs;
  publisher.publish(now, "a x=6i\n", 7);
  config.enabled = false; publisher.configure(config, now + 1);
  assert(sink.cancels == 1 && publisher.status().pending);
  sink.result = {true, 204, TelemetryError::None}; sink.done = true;
  publisher.poll(now + 2);
  assert(!publisher.status().pending && publisher.status().successes == 2 &&
         publisher.status().error == TelemetryError::Disabled);
  config.enabled = true; publisher.configure(config, uint64_t(UINT32_MAX) - 100);
  now = publisher.status().nextMs;
  assert(now > UINT32_MAX);
  publisher.publish(now, "a x=7i\n", 7);
  publisher.poll(now + 35000);
  assert(publisher.status().error == TelemetryError::Timeout && publisher.status().pending);
  assert(sink.cancels == 2 && !publisher.due(now + 10000000));
  sink.result = {false, 0, TelemetryError::Cancelled}; sink.done = true;
  publisher.poll(now + 35001);
  assert(!publisher.status().pending);
  now = publisher.status().nextMs;
  publisher.publish(now, "a x=8i\n", 7);
  sink.result = {false, 0, TelemetryError::Transport}; sink.done = true;
  publisher.poll(now + 1);
  assert(publisher.status().error == TelemetryError::Transport);
  now = publisher.status().nextMs;
  publisher.publish(now, "a x=9i\n", 7);
  sink.result = {false, 0, TelemetryError::Heap}; sink.done = true;
  sink.result.tls.point = BotHttpsFailurePoint::ConnectedReserve;
  sink.result.tls.sdk = -32512;
  sink.result.tls.before = 111948;
  sink.result.tls.ca = 24000;
  sink.result.tls.connected = 22000;
  sink.result.tls.failure = 18096;
  sink.result.tls.after = 111176;
  publisher.poll(now + 1);
  assert(publisher.status().error == TelemetryError::Heap && publisher.status().backoffSeconds);
  char reply[146];
  publisher.command("status", reply, sizeof(reply), now + 1);
  assert(strstr(reply, "error=heap http=0"));
  publisher.command("tls", reply, sizeof(reply), now + 1);
  assert(strstr(reply, "detail=connected-reserve sdk=-32512") && strstr(reply, "peer=0/0"));
  publisher.command("tls-heap", reply, sizeof(reply), now + 1);
  assert(strstr(reply, "before=111948 ca=24000 connected=22000 failure=18096 after=111176"));
  const auto diagnosticAttempt = publisher.status().tlsAttempt;
  now = publisher.status().nextMs;
  publisher.publish(now, "a x=9i\n", 7);
  assert(publisher.status().tlsAttempt == diagnosticAttempt && publisher.status().tls.sdk == -32512);
  sink.result = {true, 0, TelemetryError::None}; sink.done = true;
  publisher.poll(now + 1);
  assert(publisher.status().successes == 2);
  assert(publisher.status().tlsAttempt == publisher.status().attempts && publisher.status().tls.sdk == 0);
  now = publisher.status().nextMs;
  publisher.publish(now, "a x=10i\n", 8);
  sink.result = {false, 0, TelemetryError::Heap};
  auto &tls = sink.result.tls;
  tls.point = BotHttpsFailurePoint::ConnectedReserve;
  tls.sdk = INT32_MIN;
  tls.cipher = UINT16_MAX; tls.peerCount = UINT8_MAX; tls.peerBytes = UINT32_MAX;
  tls.caCached = true;
  tls.before = tls.ca = tls.connected = tls.failure = tls.after = UINT32_MAX;
  tls.largestBefore = tls.largestFailure = tls.largestAfter = UINT32_MAX;
  tls.globalMinBefore = tls.globalMinAfter = UINT32_MAX;
  sink.done = true; publisher.poll(now + 1);
  for (const char *command : {"tls", "tls-heap", "tls-blocks"}) {
    char full[256]{};
    publisher.command(command, full, sizeof(full), now + 1);
    publisher.command(command, reply, sizeof(reply), now + 1);
    assert(!strcmp(full, reply) && strlen(reply) < 146);
  }
  puts("PASS admission, no replay, backpressure, backoff, HTTP/TLS failure, cancellation, millis wrap");
}
static void counters() {
  RadioDashboard dashboard;
  RadioDashboard::Totals totals;
  assert(!dashboard.totals(totals));
  dashboard.received(100, nullptr, 0, -80, 7, 45);
  dashboard.transmitted(120, nullptr, 0, 1, 1, 1, queued_tx::ACCEPTED, 0, 0, 0, 45);
  dashboard.transmitted(170, nullptr, 0, 1, 1, 1, queued_tx::SUCCEEDED, 0, 5, 45, 45);
  assert(dashboard.publish(200, {}));
  assert(dashboard.totals(totals));
  assert(totals.rx_packets == 1 && totals.tx_accepted == 1 && totals.tx_succeeded == 1);
  assert(totals.rx_estimated_ms == 45 && totals.tx_rf_ms == 45);
  puts("PASS in-process physical dashboard totals without events or role sums");
}
static void administration() {
  identity_test::durable.clear();
  Sink sink;
  TelemetryPublisher publisher(sink);
  assert(publisher.begin(0));
  char reply[146];
  publisher.command("", reply, sizeof(reply), 0);
  assert(strstr(reply, "on=0") && strstr(reply, "interval=60"));
  publisher.command("interval 90", reply, sizeof(reply), 0);
  assert(strstr(reply, "Saved telemetry on=0 interval=90"));
  publisher.command("interval 99999999999999", reply, sizeof(reply), 0);
  assert(strstr(reply, "Error:") && publisher.config().intervalSeconds == 90);
  publisher.command("interval -30", reply, sizeof(reply), 0);
  assert(strstr(reply, "Error:"));
  publisher.command("on", reply, sizeof(reply), 0);
  assert(strstr(reply, "Saved telemetry on=1") && publisher.config().enabled);
  TelemetryPublisher rebooted(sink);
  assert(rebooted.begin(0) && rebooted.config().enabled && rebooted.config().intervalSeconds == 90);
  publisher.publish(90000, "a x=1i\n", 7);
  publisher.command("off", reply, sizeof(reply), 90001);
  assert(!publisher.config().enabled && sink.cancels == 1);
  assert(rebooted.begin(0) && !rebooted.config().enabled);
  identity_test::failWrite = true;
  publisher.command("on", reply, sizeof(reply), 90002);
  assert(strstr(reply, "Error:") && !publisher.config().enabled &&
         publisher.status().error == TelemetryError::Storage);
  identity_test::failWrite = false;
  for (const char *command : {"status", "times", "counts", "help"}) {
    publisher.command(command, reply, sizeof(reply), 90003);
    assert(strlen(reply) < sizeof(reply) - 1 && !strstr(reply, "Bearer ") && !strstr(reply, "password"));
  }
  puts("PASS native administration persisted on/off/interval, restart, cancellation, failure-safe state");
}
int main(int argc, char **argv) {
  if (argc == 2 && !strcmp(argv[1], "--fixture")) {
    char body[TelemetryBodyLimit + 1];
    const auto size = encodeTelemetry(sample(), "host-protocol-fixture", "telemetry-test", {}, body, sizeof(body));
    assert(size);
    fwrite(body, 1, size, stdout);
    return 0;
  }
  encoding(); settings(); publisher(); counters(); administration();
}
