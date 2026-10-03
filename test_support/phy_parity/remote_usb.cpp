// SPDX-License-Identifier: Apache-2.0
#define main existing_phy_cases
#include "combined.cpp"
#undef main
#include <MyMesh.h>
#include <helpers/ArduinoSerialInterface.h>

LifecycleTestBoard board;
SensorManager sensors;
VolatileRTCClock rtc_clock;
static SocketStream transport(-1);
ObservedRemoteRadio radio_driver(transport);
static uint32_t entropy = 123456789;
void randomSeed(long value) { entropy = value; }
long random(long low, long high) {
  entropy ^= entropy << 13;
  entropy ^= entropy >> 17;
  entropy ^= entropy << 5;
  return low + entropy % (high - low);
}
mesh::LocalIdentity radio_new_identity() {
  StdRNG rng;
  return mesh::LocalIdentity(&rng);
}

using Bytes = std::vector<uint8_t>;
static std::vector<Bytes> strictFrames() {
  std::vector<Bytes> frames;
  const auto &bytes = Serial.output;
  for (size_t at = 0; at < bytes.size();) {
    if (bytes[at] != '>' || bytes.size() - at < 3)
      fprintf(stderr, "Unframed USB byte 0x%02x at %zu/%zu\n", bytes[at],
              at, bytes.size());
    assert(bytes[at] == '>' && bytes.size() - at >= 3);
    const size_t size = bytes[at + 1] | (size_t(bytes[at + 2]) << 8);
    assert(size && size <= MAX_FRAME_SIZE && size <= bytes.size() - at - 3);
    frames.emplace_back(bytes.begin() + at + 3, bytes.begin() + at + 3 + size);
    at += 3 + size;
  }
  Serial.output.clear();
  return frames;
}

int main() {
  Fixture physical;
  transport.fd = physical.peers[1];
  StdRNG rng;
  SimpleMeshTables tables;
  DataStore store(SPIFFS, rtc_clock);
  // The upstream companion packet pools have process lifetime.
  static MyMesh native(radio_driver, rng, rtc_clock, tables, store);
  store.begin();
  native.begin(false);
  ArduinoSerialInterface usb;
  usb.begin(Serial);
  native.startInterface(usb);
  auto command = [&](const Bytes &request) {
    Serial.input.push_back('<');
    Serial.input.push_back(request.size());
    Serial.input.push_back(request.size() >> 8);
    Serial.input.insert(Serial.input.end(), request.begin(), request.end());
    native.loop();
    auto frames = strictFrames();
    assert(frames.size() == 1);
    return frames[0];
  };
  auto diagnostic = [&](uint8_t index) {
    auto frame = command({56, 0x80, index});
    assert(frame.size() >= 36 && frame[0] == 24 && frame[1] == 0x80 &&
           frame[2] == 1);
    assert(frame.size() == size_t(36 + frame[35]));
    return frame;
  };
  auto first = diagnostic(0);
  assert(first[3] == 0 && first[12] == 0 && first.size() == 36);
  assert(command({56, 0x80, 1})[0] == 1);
  assert(command({56, 0x80, 0, 0})[0] == 1);
  assert(command({22, 3})[0] == 13);
  assert(command({1, 0, 0, 0, 0, 0, 0, 0})[0] == 5);
  for (uint8_t type = 0; type < 3; ++type) {
    auto frame = command({56, type});
    assert(frame[0] == 24 && frame[1] == type);
  }

  uint32_t rfStartedAt = 0;
  auto tick = [&] {
    const bool transmitting = physical.radio.sending;
    physical.step();
    if (!transmitting && physical.radio.sending)
      rfStartedAt = clock_ms;
    native.loop();
    ++clock_ms;
    assert(strictFrames().empty());
  };
  auto ready = [&] {
    for (unsigned i = 0; i < 2000 && !radio_driver.queuedReady(); ++i)
      tick();
    assert(radio_driver.queuedReady());
  };
  auto disconnect = [&] {
    assert(physical.peers[1] >= 0);
    close(physical.peers[1]);
    physical.peers[1] = transport.fd = -1;
    radio_driver.onLinkDisconnected();
    tick();
    assert(!radio_driver.queuedReady());
  };
  auto reconnect = [&] {
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    physical.server.add(WiFiClient(sockets[0]));
    physical.peers[1] = transport.fd = sockets[1];
    physical.step();
    radio_driver.onLinkConnected();
    ready();
  };
  radio_driver.begin();
  ready();
  mesh::QueuedRadioStats original;
  assert(radio_driver.getQueuedRadioStats(original));

  auto advertise = [&] {
    const auto before = physical.radio.transmitted.size();
    const auto results = radio_driver.results.size();
    assert(command({7, 1}) == Bytes{0});
    for (unsigned i = 0; i < 2000 &&
         (physical.radio.transmitted.size() == before ||
          radio_driver.results.size() == results); ++i)
      tick();
    assert(physical.radio.transmitted.size() == before + 1);
    assert(radio_driver.results.size() == results + 1);
    const auto accepted = radio_driver.results.back();
    assert(accepted.state == queued_tx::ACCEPTED && accepted.job != 0);
    return accepted.job;
  };
  auto finish = [&](uint32_t job, uint32_t airtime) {
    const auto sent = radio_driver.getPacketsSent();
    const auto results = radio_driver.results.size();
    const uint32_t expectedRF = clock_ms + airtime - rfStartedAt;
    physical.finish(airtime);
    for (unsigned i = 0; i < 100; ++i)
      tick();
    assert(radio_driver.getPacketsSent() == sent + 1);
    assert(!physical.radio.sending && radio_driver.results.size() == results + 1);
    const auto result = radio_driver.results.back();
    assert(result.job == job && result.state == queued_tx::SUCCEEDED &&
           result.has_rf_ms && result.rf_ms == expectedRF);
  };

  const auto abandoned = advertise();
  const auto sentBeforeDisconnect = radio_driver.getPacketsSent();
  const auto resultCount = radio_driver.results.size();
  disconnect();
  assert(radio_driver.results.size() == resultCount + 1);
  const auto unknown = radio_driver.results.back();
  assert(unknown.job == abandoned && unknown.state == queued_tx::UNKNOWN &&
         unknown.reason == queued_tx::DISCONNECTED && !unknown.has_rf_ms);
  auto down = diagnostic(0);
  assert(down[22] == RemoteKissDiagnostics::Offline);
  assert(queued_tx::get32(down.data() + 27) == original.generation);
  assert(command({22, 3})[0] == 13); // USB remains usable with PHY disconnected.
  const uint32_t abandonedRF = clock_ms + 77 - rfStartedAt;
  physical.finish(77);
  reconnect();
  mesh::QueuedRadioStats replacement;
  assert(radio_driver.getQueuedRadioStats(replacement) &&
         replacement.generation != original.generation &&
         replacement.aggregate_rf_ms == abandonedRF && replacement.source_rf_ms == 0);
  const auto transmissions = physical.radio.transmitted.size();
  for (unsigned i = 0; i < 200; ++i)
    tick();
  assert(physical.radio.transmitted.size() == transmissions &&
         radio_driver.results.size() == resultCount + 1 &&
         radio_driver.getPacketsSent() == sentBeforeDisconnect);
  assert(command({22, 3})[0] == 13);
  assert(command({1, 0, 0, 0, 0, 0, 0, 0})[0] == 5);
  assert(command({56, 1})[0] == 24);
  finish(advertise(), 23);

  Bytes longMessage(90, 'X');
  longMessage.push_back(0);
  const auto beforePaging = diagnostic(0);
  const uint32_t total = queued_tx::get32(beforePaging.data() + 4);
  for (unsigned i = 0; i < 10; ++i)
    RemoteKissDiagnostics::record(RemoteKissDiagnostics::Receive,
        reinterpret_cast<const char *>(longMessage.data()), i);
  assert(strictFrames().empty());
  const auto newest = diagnostic(0);
  assert(newest.size() == 107 && newest[3] == 3 && newest[12] == 8);
  assert(queued_tx::get32(newest.data() + 4) == total + 10);
  assert(queued_tx::get32(newest.data() + 8) == total + 2);
  assert(queued_tx::get32(newest.data() + 23) == 9);
  assert(queued_tx::get32(diagnostic(7).data() + 23) == 2);
  assert(command({56, 0x80, 8})[0] == 1);
  assert(command({22, 3})[0] == 13);
  puts("PASS actual native companion + ArduinoSerialInterface + socket Remote/"
       "KissModem/mux: strict USB frames, paged diagnostics, UNKNOWN without "
       "replay, fresh generation and successful post-reconnect TX");
}
