#include "RadioDashboard.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

struct DashboardStreamTest {
  struct Output {
    std::string current[3];
    std::vector<std::string> complete[3];
    std::vector<int> dropped;
    int blocked = -1;
    unsigned pings = 0;
    static bool send(void *context, int fd, uint8_t opcode, const uint8_t *data,
                     size_t length, bool final) {
      auto &output = *static_cast<Output *>(context);
      assert(length <= 1024);
      if (fd == output.blocked)
        return false;
      if (opcode == 9) {
        ++output.pings;
        return true;
      }
      assert(opcode == (output.current[fd].empty() ? 1 : 0));
      output.current[fd].append(reinterpret_cast<const char *>(data), length);
      if (final) {
        output.complete[fd].push_back(output.current[fd]);
        output.current[fd].clear();
      }
      return true;
    }
    static void drop(void *context, int fd) {
      static_cast<Output *>(context)->dropped.push_back(fd);
    }
  };

  static void run() {
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    const int small_buffer = 1024;
    assert(setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF, &small_buffer,
                      sizeof(small_buffer)) == 0);
    char input[4]{};
    assert(RadioDashboard::sendAvailable(sockets[0], "test", 4, 0) == 4);
    assert(recv(sockets[1], input, 4, MSG_DONTWAIT) == 4);
    assert(memcmp(input, "test", 4) == 0);
    std::string oversized(65536, 'x');
    assert(RadioDashboard::sendAvailable(sockets[0], oversized.data(),
                                         oversized.size(), 0) == -1);
    // The kernel accepted only part of the buffer; the SDK must see failure,
    // not the positive short count it would mistake for a complete frame.
    assert(RadioDashboard::sendAvailable(sockets[0], "full", 4, 0) == -1);
    const int partial =
        recv(sockets[1], &oversized[0], oversized.size(), MSG_DONTWAIT);
    assert(partial > 0 && partial < static_cast<int>(oversized.size()));
    close(sockets[0]);
    close(sockets[1]);

    RadioDashboard unready;
    auto *waiting = unready.subscribe(0, 100);
    Output missing;
    unready.pumpLive(3099, "radio", Output::send, Output::drop, &missing);
    assert(!waiting->closing);
    unready.pumpLive(3100, "radio", Output::send, Output::drop, &missing);
    assert(waiting->closing && missing.dropped == std::vector<int>{0});

    RadioDashboard dashboard;
    RadioDashboard::RadioStatus status;
    status.dma_free_heap = 66000;
    status.dma_largest_heap = 55284;
    status.dma_minimum_heap = 8904;
    queued_tx::putFloat(status.profile + 11, 1);
    const uint8_t packet[16]{};
    for (unsigned i = 0; i < RadioDashboard::HISTORY; ++i)
      dashboard.received(100, packet, sizeof(packet), -70, 4, 40);
    dashboard.publish(100, status);
    auto *first = dashboard.subscribe(0, 100);
    auto *slow = dashboard.subscribe(1, 100);
    assert(first && slow && !dashboard.subscribe(2, 100));
    Output output;
    output.blocked = 1;
    dashboard.pumpLive(100, "radio", Output::send, Output::drop, &output);
    assert(first->sending && slow->closing &&
           output.dropped == std::vector<int>{1});
    // Closing slots cannot be reused until the SDK frees the old session
    // context.
    assert(!dashboard.subscribe(2, 100));
    *slow = {};
    auto *second = dashboard.subscribe(2, 100);
    assert(second == slow);
    dashboard.publish(200, status);
    dashboard.publish(250, status);
    for (uint64_t now = 250; now < 2000; now += 25)
      dashboard.pumpLive(now, "radio", Output::send, Output::drop, &output);
    assert(output.complete[0].size() == 2 && output.complete[2].size() == 2);
    assert(output.complete[0][0].find("\"publication\":1,") !=
           std::string::npos);
    assert(output.complete[0][1].find("\"publication\":3,") !=
           std::string::npos);
    assert(output.complete[0] == output.complete[2]);
    assert(output.complete[0][0].find("\"dma_free_bytes\":66000,\"dma_largest_bytes\":55284,\"dma_minimum_bytes\":8904") !=
           std::string::npos);
    assert(!first->sending && !second->sending);

    // A pending fragment times out without accumulating newer snapshots.
    dashboard.publish(2100, status);
    dashboard.pumpLive(2100, "radio", Output::send, Output::drop, &output);
    assert(first->sending);
    dashboard.publish(4100, status);
    dashboard.pumpLive(4100, "radio", Output::send, Output::drop, &output);
    assert(first->closing && second->closing && output.dropped.size() == 3);
    *first = {};
    *second = {};
    first = dashboard.subscribe(0, 4200);
    output.current[0].clear();
    dashboard.publish(9200, status);
    dashboard.pumpLive(9200, "radio", Output::send, Output::drop, &output);
    assert(output.pings == 1 && first->pong_deadline_ms == 14200);
    // A responsive browser's pong resets only its deadline.
    first->pong_deadline_ms = 0;
    for (uint64_t now = 9225; now < 10000; now += 25)
      dashboard.pumpLive(now, "radio", Output::send, Output::drop, &output);
    dashboard.publish(14200, status);
    dashboard.pumpLive(14200, "radio", Output::send, Output::drop, &output);
    assert(output.pings == 2 && first->pong_deadline_ms == 19200);
    for (uint64_t now = 14225; now < 15000; now += 25)
      dashboard.pumpLive(now, "radio", Output::send, Output::drop, &output);
    dashboard.publish(19200, status);
    dashboard.pumpLive(19200, "radio", Output::send, Output::drop, &output);
    assert(first->closing);
    *first = {};
    first = dashboard.subscribe(0, 19300);
    dashboard.pumpLive(22201, "radio", Output::send, Output::drop, &output);
    assert(first->closing); // stale RF publication, despite a live HTTP task
  }
};

static RadioDashboard::RadioStatus profile() {
  RadioDashboard::RadioStatus status;
  queued_tx::put32(status.profile, 912525000);
  queued_tx::put32(status.profile + 4, 250000);
  status.profile[8] = 7;
  status.profile[9] = 5;
  status.profile[10] = 2;
  queued_tx::putFloat(status.profile + 11, 99);
  status.generation = 1;
  status.committed = true;
  status.credit_ms = 36000;
  status.wifi_connected = true;
  status.wifi_rssi = -51;
  return status;
}

static void timing_and_signal_are_separate() {
  RadioDashboard dashboard;
  auto status = profile();
  assert(dashboard.publish(100, status));
  const uint8_t packet[] = {0xc0, 0xdb, '<', '"', '\\', 0xff};
  dashboard.transmitted(100, packet, sizeof(packet), 0, 7, 9,
                        queued_tx::ACCEPTED, 0, 0, 0, 0);
  dashboard.transmitted(2700, packet, sizeof(packet), 0, 7, 9,
                        queued_tx::SUCCEEDED, 0, 2000, 600, 500);
  dashboard.received(2750, packet, sizeof(packet), -87.5f, -2.25f, 40);
  dashboard.transmitted(3200, packet, sizeof(packet), 1, 8, 10,
                        queued_tx::UNKNOWN, queued_tx::RF_TIMEOUT, 30000, 300,
                        200);
  assert(dashboard.publish(3250, status));
  RadioDashboard::Snapshot snapshot;
  assert(dashboard.snapshot(snapshot));
  assert(snapshot.tx_accepted == 1 && snapshot.tx_succeeded == 1 &&
         snapshot.tx_unknown == 1);
  assert(snapshot.tx_rf_ms == 900 && snapshot.rx_estimated_ms == 40 &&
         snapshot.rx_packets == 1);
  assert(snapshot.event_count == 3 && snapshot.wifi_uptime_ms == 3150);
  assert(snapshot.events[0].queue_ms == 2000 &&
         snapshot.events[0].rf_ms == 600);
  assert(!snapshot.events[0].has_signal && snapshot.events[1].has_signal);
  assert(snapshot.events[1].rssi == -87.5f && snapshot.events[1].snr == -2.25f);
  assert(snapshot.events[1].rf_ms == 0 && snapshot.events[1].rx);
  assert(snapshot.traffic[2].tx_rf_ms == 700 &&
         snapshot.traffic[3].tx_rf_ms == 200);
  assert(snapshot.traffic[2].rx_estimated_ms == 40);
  status.wifi_connected = false;
  dashboard.publish(3300, status);
  dashboard.snapshot(snapshot);
  assert(snapshot.wifi_uptime_ms == 0);
  status.wifi_connected = true;
  dashboard.publish(3400, status);
  dashboard.publish(3500, status);
  dashboard.snapshot(snapshot);
  assert(snapshot.wifi_uptime_ms == 100);
}

static RadioDashboard::Snapshot ring_wrap_and_clock_rollover() {
  RadioDashboard dashboard;
  const auto status = profile();
  dashboard.publish(0xfffffff0, status);
  const uint8_t packet[255] = {'<', '/', 's', 'c', 'r',  'i',
                               'p', 't', '>', '"', '\\', 0xff};
  for (uint32_t i = 1; i <= 40; ++i)
    dashboard.received(0xfffffff0 + i * 1000, packet, sizeof(packet), -70,
                       4.25f, 40);
  dashboard.transmitted(0xfffffff0 + 40100u, packet, sizeof(packet), 0, 1, 1,
                        queued_tx::SUCCEEDED, 0, 7000, 75, 70);
  dashboard.transmitted(0xfffffff0 + 40250u, packet, sizeof(packet), 0, 1, 2,
                        queued_tx::UNKNOWN, queued_tx::RF_TIMEOUT, 8000, 150,
                        100);
  dashboard.publish(0xfffffff0 + 40250u, status);
  RadioDashboard::Snapshot snapshot;
  dashboard.snapshot(snapshot);
  assert(snapshot.uptime_ms == uint64_t{0xfffffff0} + 40250);
  assert(snapshot.event_count == 32 && snapshot.events_overwritten == 10 &&
         snapshot.events_total == 42);
  assert(snapshot.events[(snapshot.event_next + 31) % 32].sequence == 42);
  for (const auto &event : snapshot.events)
    assert(event.preview_length == 16 && event.length == 255);
  return snapshot;
}

static void old_occupancy_is_bounded() {
  RadioDashboard dashboard;
  auto status = profile();
  const uint8_t byte = 1;
  dashboard.transmitted(200000, &byte, 1, 0, 1, 1, queued_tx::UNKNOWN, 8,
                        300000, 120000, 80000);
  dashboard.received(200001, &byte, 1, std::numeric_limits<float>::quiet_NaN(),
                     1, 50);
  dashboard.publish(200001, status);
  RadioDashboard::Snapshot snapshot;
  dashboard.snapshot(snapshot);
  uint32_t observed = 0;
  for (const auto &bucket : snapshot.traffic)
    observed += bucket.tx_rf_ms;
  assert(observed == 59000 && snapshot.tx_rf_ms == 120000);
  assert(!snapshot.events[1].has_signal);
}

static void published_copies_are_coherent() {
  RadioDashboard dashboard;
  std::atomic<bool> finished{false};
  std::thread reader([&] {
    RadioDashboard::Snapshot snapshot;
    while (!finished.load(std::memory_order_acquire)) {
      if (dashboard.snapshot(snapshot)) {
        assert(snapshot.radio.generation == snapshot.tx_succeeded);
        assert(snapshot.tx_rf_ms == snapshot.tx_succeeded * 10);
        assert(snapshot.events_total == snapshot.tx_succeeded);
        assert(snapshot.events[(snapshot.event_next + 31) % 32].sequence ==
               snapshot.tx_succeeded);
      }
      std::this_thread::yield();
    }
  });
  auto status = profile();
  const uint8_t packet = 1;
  for (uint32_t i = 1; i <= 10000; ++i) {
    dashboard.transmitted(i * 100, &packet, 1, 0, 1, i, queued_tx::SUCCEEDED, 0,
                          90000, 10, 10);
    status.generation = i;
    dashboard.publish(i * 100, status);
  }
  finished.store(true, std::memory_order_release);
  reader.join();
  assert(dashboard.publish(1000000, status));
  RadioDashboard::Snapshot snapshot;
  assert(dashboard.snapshot(snapshot) && snapshot.tx_succeeded == 10000);
}

int main(int argc, char **argv) {
  assert(argc == 2);
  timing_and_signal_are_separate();
  old_occupancy_is_bounded();
  published_copies_are_coherent();
  DashboardStreamTest::run();
  auto snapshot = ring_wrap_and_clock_rollover();
  for (size_t i = 0; i < KISS_MAX_TCP_CLIENTS; ++i) {
    auto &client = snapshot.radio.client[i];
    client.connected = true;
    client.negotiated = true;
    client.generation = UINT32_MAX;
    client.factor = std::numeric_limits<float>::max();
    client.credit_ms = UINT32_MAX;
    client.rf_ms = UINT32_MAX;
  }
  snapshot.radio.clients = KISS_MAX_TCP_CLIENTS;
  snapshot.radio.dma_free_heap = UINT32_MAX;
  snapshot.radio.dma_largest_heap = 123456;
  snapshot.radio.dma_minimum_heap = 0;
  char json[RadioDashboard::JSON_CAPACITY];
  const size_t length = RadioDashboard::formatJSON(
      snapshot, "Radio \"A\"\n<script>", json, sizeof(json));
  assert(length > 0 && length < sizeof(json));
  std::ofstream output(argv[1]);
  output.write(json, length);
  output.close();
  assert(output);
  char too_small[8] = "old";
  assert(RadioDashboard::formatJSON(snapshot, "radio", too_small,
                                    sizeof(too_small)) == 0);
  assert(too_small[0] == '\0');
  std::printf("Dashboard timing, bounds, rollover, concurrent snapshots and "
              "JSON passed (%zu-byte snapshot, %zu-byte JSON)\n",
              sizeof(snapshot), length);
}
