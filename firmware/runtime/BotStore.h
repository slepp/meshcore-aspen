// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "BotTypes.h"
#include <atomic>

namespace onchip {
class BotStore {
  struct Record;
  struct Journal;
  struct Files;
  Record *record_ = nullptr;
  Journal *journal_ = nullptr;
  Files *files_ = nullptr;
  bool admitJournal(bool publicScope, char *error, size_t capacity);
  bool validateRecords(char *error, size_t capacity, bool logical = true);
  bool readFiles(uint32_t started, char *error, size_t capacity);
  bool reclaim(uint32_t started, char *error, size_t capacity);
  bool publish(uint8_t changed, BotIoResult::Outcome &outcome, char *error, size_t capacity,
                uint32_t started, uint32_t budgetMs,
               uint32_t epoch = 0,
               const std::atomic<uint32_t> *generation = nullptr,
               const BotIoRequest *request = nullptr,
               const std::atomic<bool> *sharedState = nullptr,
               const std::atomic<uint32_t> *sharedGrant = nullptr,
               const std::atomic<uint32_t> *eventEpoch = nullptr);
  bool transact(const uint8_t bot[32], const BotIoRequest &request, BotIoResult &result,
                const std::atomic<uint32_t> &generation, const std::atomic<bool> &sharedState,
                const std::atomic<uint32_t> &sharedGrant, const std::atomic<uint32_t> *eventEpoch,
                uint32_t started);
public:
  struct Snapshot {
    uint8_t magic[4]{}, bot[32]{}, scope = 0, principal[32]{}, count = 0;
    struct Entry { char key[BotKeyLimit + 1]{}, value[257]{}; } entries[BotKeysPerScope];
    uint8_t digest[32]{};
  };
  ~BotStore();
  bool recover(char *error, size_t capacity);
  static bool validSnapshotEnvelope(const Snapshot &, const uint8_t bot[32], const char magic[4],
                                    size_t itemSize, unsigned limit, char *error, size_t capacity);
  static bool validSnapshot(const Snapshot &, const uint8_t bot[32], char *error, size_t capacity);
  bool snapshot(const uint8_t bot[32], Snapshot &, char *error, size_t capacity);
  void restore(const uint8_t bot[32], const Snapshot &, BotIoResult &result,
               uint32_t epoch, const std::atomic<uint32_t> &generation);
  void perform(const uint8_t bot[32], const BotIoRequest &request, BotIoResult &result,
               const std::atomic<uint32_t> &generation, const std::atomic<bool> &sharedState,
               const std::atomic<uint32_t> &sharedGrant, const std::atomic<uint32_t> *eventEpoch = nullptr);
};
}
