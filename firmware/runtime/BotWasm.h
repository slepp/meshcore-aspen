// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "BotVm.h"

namespace onchip {
// A copied package header may precede the portable module.
bool botWasmBytes(const char *source, size_t size, const uint8_t *&bytes, size_t &length);
class BotWasmSession {
  struct Impl;
  Impl *impl_ = nullptr;
public:
  ~BotWasmSession();
  bool load(const char *, size_t, uint32_t, BotVmStats &, char *, size_t, BotVmLimits);
  const BotManifestView &manifest() const;
  void setGeneration(uint32_t);
  void setEventEpoch(std::atomic<uint32_t> *);
  void cancelEvents();
  bool start(uint32_t, const BotEvent &, char *, size_t);
  bool poll(BotSession::Result &);
  bool nextIo(BotIoRequest &);
  bool hasPendingIo() const;
  bool complete(const BotIoResult &);
  void cancel(uint32_t except = 0);
  void clear();
};
} // namespace onchip
