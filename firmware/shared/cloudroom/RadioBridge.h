// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <atomic>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "CloudRoomWire.h"

namespace cloudroom {
constexpr unsigned QueueDepth = 8;
constexpr unsigned AliasLimit = 3;
struct Reception {
  uint16_t size = 0;
  int16_t rssi = 0, snrQuarter = 0;
  uint8_t bytes[RadioLimit]{};
};
struct Transmission {
  uint32_t cookie = 0, generation = 0, delayMs = 0;
  uint8_t alias = 0, priority = 0;
  uint16_t size = 0;
  uint8_t bytes[RadioLimit]{};
};
struct Receipt {
  enum Outcome : uint8_t { Sent, Failed, Unknown } outcome = Unknown;
  uint32_t cookie = 0, generation = 0;
  uint8_t alias = 0;
};
// Payload arrays may live in PSRAM. These atomics/control objects stay in
// internal memory. Exactly one producer and one consumer per queue.
template <typename T, unsigned Capacity> class Spsc {
  T *slots_ = nullptr;
  std::atomic<uint32_t> head_{0}, tail_{0};
public:
  void bind(T (&slots)[Capacity]) { slots_ = slots; }
  bool push(const T &value) {
    const auto head = head_.load(std::memory_order_relaxed);
    if (!slots_ || uint32_t(head-tail_.load(std::memory_order_acquire)) == Capacity) return false;
    slots_[head % Capacity] = value;
    head_.store(head+1,std::memory_order_release);
    return true;
  }
  bool pop(T &value) {
    const auto tail = tail_.load(std::memory_order_relaxed);
    if (!slots_ || tail == head_.load(std::memory_order_acquire)) return false;
    value = slots_[tail % Capacity];
    tail_.store(tail+1,std::memory_order_release);
    return true;
  }
  bool full() const { return !slots_ || uint32_t(head_.load(std::memory_order_acquire)-tail_.load(std::memory_order_acquire)) == Capacity; }
};
struct RadioBuffers {
  Reception rx[QueueDepth];
  Transmission tx[QueueDepth];
  Receipt results[QueueDepth];
};
class RadioBridge {
  std::atomic<uint32_t> generations_[AliasLimit]{};
public:
  Spsc<Reception,QueueDepth> rx;
  Spsc<Transmission,QueueDepth> tx;
  Spsc<Receipt,QueueDepth> results;
  void bind(RadioBuffers &buffers) { rx.bind(buffers.rx); tx.bind(buffers.tx); results.bind(buffers.results); }
  // Network task increments on each disconnected/replaced connection. Already
  // submitted RF stays uncertain; stale work that was never submitted is failed.
  uint32_t disconnect(unsigned alias) { return alias<AliasLimit ? generations_[alias].fetch_add(1,std::memory_order_acq_rel)+1 : 0; }
  uint32_t generation(unsigned alias) const { return alias<AliasLimit ? generations_[alias].load(std::memory_order_acquire) : 0; }
  bool current(const Transmission &job) const { return job.alias<AliasLimit && job.generation==generation(job.alias); }
  // Dispatch-task producer. Local reflections never enter the cloud RX stream.
  bool received(const uint8_t *bytes,size_t size,int16_t rssi,int16_t snrQuarter,bool local) {
    if (local || !bytes || !size || size>RadioLimit) return false;
    Reception packet; packet.size=uint16_t(size); packet.rssi=rssi; packet.snrQuarter=snrQuarter;
    memcpy(packet.bytes,bytes,size);
    return rx.push(packet);
  }
  // Network-task producer, called only after authoritative RF dispatch permission.
  bool submit(Transmission job) {
    if (!job.cookie || !job.size || job.size>RadioLimit || !current(job)) return false;
    return tx.push(job);
  }
};
} // namespace cloudroom
