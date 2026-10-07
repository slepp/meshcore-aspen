// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "RadioBridge.h"

namespace cloudroom {
struct OpaqueAlias { const char *id = nullptr, *name = nullptr; uint8_t publicKey[32]{}; };
struct OpaqueBuffers {
  struct Operation { uint16_t size = 0; char bytes[512]{}; } operations[AliasLimit][QueueDepth];
};
// Network-task-only protocol bridge. Keeps bounded in-flight dispatch IDs,
// without a room key, radio codec, membership or history cache.
class OpaqueFrontend {
  RadioBridge &radio_;
  const OpaqueAlias *aliases_;
  unsigned count_;
  OpaqueBuffers &buffers_;
  uint32_t request_ = 0, cookie_ = 0;
  struct State { bool ready = false; uint32_t generation = 0; unsigned head = 0, size = 0; char failedId[37]{}; } states_[AliasLimit];
  struct Flight { uint32_t cookie = 0, generation = 0; uint8_t alias = 0; char id[37]{}; bool terminal = false; Receipt::Outcome outcome = Receipt::Unknown; } flights_[QueueDepth];
  bool enqueue(unsigned alias, const char *frame, size_t size);
  size_t receiptFrame(const char *id, Receipt::Outcome, char *, size_t);
public:
  OpaqueFrontend(RadioBridge &radio, const OpaqueAlias *aliases, unsigned count, OpaqueBuffers &buffers)
    : radio_(radio), aliases_(aliases), count_(count), buffers_(buffers) {}
  void opened(unsigned alias, uint32_t generation);
  void disconnected(unsigned alias);
  void received(const Reception &);
  bool advertise(unsigned alias);
  bool frame(unsigned alias, const char *, size_t);
  void receipt(const Receipt &);
  size_t operation(unsigned alias, char *, size_t capacity);
};
} // namespace cloudroom
