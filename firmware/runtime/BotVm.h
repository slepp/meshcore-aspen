// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "BotTypes.h"
#include "BotRegistry.h"
#include <atomic>

namespace onchip {
class BotWasmSession;
// Worker-only source compilation and native dispatch of registered exports.
class BotVm {
public:
  static bool validate(const char *source, size_t size, BotVmStats &stats,
                       char *error, size_t errorSize, BotVmLimits limits = {},
                       BotManifest *manifest = nullptr);
  static bool invoke(const char *source, size_t size, const BotEvent &event,
                     BotAction &action, BotVmStats &stats, char *error,
                     size_t errorSize, BotVmLimits limits = {},
                     const BotManifest *installed = nullptr);
};

// All methods run on the VM worker. Providers exchange copied, fenced I/O only.
class BotSession {
  struct Impl;
  Impl *impl_ = nullptr;
#if ONCHIP_BOT_WASM
  BotWasmSession *wasm_ = nullptr;
#endif
public:
  static const size_t StorageBytes;
  static const size_t InitializationStorageBytes;
  struct Result {
    uint32_t job = 0;
    bool ok = false;
    BotAction action{};
    BotVmStats stats{};
    char error[128]{};
  };
  BotSession() = default;
  ~BotSession();
  BotSession(const BotSession &) = delete;
  BotSession &operator=(const BotSession &) = delete;
  bool load(const char *source, size_t size, uint32_t generation,
            BotVmStats &stats, char *error, size_t errorSize,
            BotVmLimits limits = {});
  const BotManifestView &manifest() const;
  void setHelp(const BotManifest &installed);
  void setGeneration(uint32_t generation);
  uint8_t subscriptions() const;
  void setEventEpoch(std::atomic<uint32_t> *epoch);
  void cancelEvents();
  bool start(uint32_t job, const BotEvent &event, char *error, size_t errorSize);
  bool poll(Result &result);
  bool nextIo(BotIoRequest &request);
  bool hasPendingIo() const;
  bool complete(const BotIoResult &result);
  void cancel(uint32_t except = 0);
  void clear();
  void swap(BotSession &other);
  bool isWasm() const;
};
} // namespace onchip
