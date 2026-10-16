#pragma once
#include <cassert>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>
struct StaticQueue_t {
  std::mutex mutex;
  size_t capacity = 0, itemSize = 0;
  std::deque<std::vector<uint8_t>> items;
};
using QueueHandle_t = StaticQueue_t*;
constexpr int pdTRUE = 1, pdFALSE = 0;
inline QueueHandle_t xQueueCreateStatic(size_t count, size_t size, uint8_t*, StaticQueue_t* queue) {
  queue->capacity = count; queue->itemSize = size; return queue;
}
inline int xQueueReset(QueueHandle_t queue) {
  std::lock_guard<std::mutex> lock(queue->mutex); queue->items.clear(); return pdTRUE;
}
inline size_t uxQueueSpacesAvailable(QueueHandle_t queue) {
  std::lock_guard<std::mutex> lock(queue->mutex); return queue->capacity - queue->items.size();
}
inline int xQueueSend(QueueHandle_t queue, const void* bytes, unsigned wait) {
  assert(!wait);
  std::lock_guard<std::mutex> lock(queue->mutex);
  if (queue->items.size() == queue->capacity) return pdFALSE;
  const auto* start = static_cast<const uint8_t*>(bytes);
  queue->items.emplace_back(start, start + queue->itemSize); return pdTRUE;
}
inline int xQueueReceive(QueueHandle_t queue, void* bytes, unsigned wait) {
  assert(!wait);
  std::lock_guard<std::mutex> lock(queue->mutex);
  if (queue->items.empty()) return pdFALSE;
  memcpy(bytes, queue->items.front().data(), queue->itemSize);
  queue->items.pop_front(); return pdTRUE;
}
