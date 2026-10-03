#include "RadioDashboard.h"
#include <cassert>
#include <cstdio>
#include <string>

constexpr unsigned MALLOC_CAP_8BIT = 4, MALLOC_CAP_DMA = 8, MALLOC_CAP_INTERNAL = 2048;
static uint32_t dma_free = 12345, dma_largest = 6789, dma_minimum = 64;
static unsigned heap_reads;
size_t heap_caps_get_free_size(unsigned caps) {
  assert(caps == 2060);
  ++heap_reads;
  return dma_free;
}
size_t heap_caps_get_largest_free_block(unsigned caps) {
  assert(caps == 2060);
  ++heap_reads;
  return dma_largest;
}
size_t heap_caps_get_minimum_free_size(unsigned caps) {
  assert(caps == 2060);
  ++heap_reads;
  return dma_minimum;
}
struct {
  uint32_t getFreeHeap() { return 66000; }
  uint32_t getMinFreeHeap() { return 8904; }
} ESP;
struct { int RSSI() { return -51; } } WiFi;
struct { uint32_t getPacketsRecvErrors() { return 7; } } radio_driver;
struct { void dashboardStatus(RadioDashboard::RadioStatus&) {} } kiss_stream;
static bool wifi_was_connected = true;
static RadioDashboard dashboard;
RadioDashboard& getDashboard() { return dashboard; }
unsigned long millis() { return 1234; }

// Compile the actual loop-owned sampler, not an equivalent test implementation.
#include "dashboard-sample.inc"

int main() {
  publishDashboard();
  RadioDashboard::Snapshot snapshot;
  assert(dashboard.snapshot(snapshot) && heap_reads == 3);
  assert(snapshot.radio.free_heap == 66000 && snapshot.radio.minimum_heap == 8904);
  assert(snapshot.radio.dma_free_heap == 12345 && snapshot.radio.dma_largest_heap == 6789 &&
         snapshot.radio.dma_minimum_heap == 64);
  dma_free = dma_largest = dma_minimum = 0;
  char json[RadioDashboard::JSON_CAPACITY];
  assert(dashboard.snapshot(snapshot));
  assert(RadioDashboard::formatJSON(snapshot, "radio", json, sizeof(json)));
  assert(std::string(json).find("\"dma_free_bytes\":12345,\"dma_largest_bytes\":6789,\"dma_minimum_bytes\":64") !=
         std::string::npos);
  assert(heap_reads == 3); // Readers retain the sampled values; they never query the heap.
  publishDashboard();
  assert(dashboard.snapshot(snapshot) && heap_reads == 6);
  assert(snapshot.radio.dma_free_heap == 0 && snapshot.radio.dma_largest_heap == 0 &&
         snapshot.radio.dma_minimum_heap == 0);
  puts("Production dashboard sampler: exact DMA capabilities, bytes, zero and read-only published values passed");
}
