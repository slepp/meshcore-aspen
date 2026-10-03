#pragma once
#include "FreeRTOS.h"
#include <cstring>
#include <deque>
#include <vector>
struct TestQueue {
  unsigned capacity;
  size_t itemSize;
  std::deque<std::vector<uint8_t>> data;
};
using QueueHandle_t = TestQueue *;
inline QueueHandle_t xQueueCreate(unsigned capacity, size_t size) {
  return new TestQueue{capacity, size, {}};
}
inline void vQueueDelete(QueueHandle_t queue) { delete queue; }
inline int xQueueSend(QueueHandle_t queue, const void *value, TickType_t) {
  if (queue->data.size() == queue->capacity)
    return pdFALSE;
  const auto bytes = static_cast<const uint8_t *>(value);
  queue->data.emplace_back(bytes, bytes + queue->itemSize);
  return pdTRUE;
}
inline int xQueueOverwrite(QueueHandle_t queue, const void *value) {
  queue->data.clear();
  return xQueueSend(queue, value, 0);
}
inline int xQueueReceive(QueueHandle_t queue, void *value, TickType_t) {
  if (queue->data.empty())
    return pdFALSE;
  memcpy(value, queue->data.front().data(), queue->itemSize);
  queue->data.pop_front();
  return pdTRUE;
}
