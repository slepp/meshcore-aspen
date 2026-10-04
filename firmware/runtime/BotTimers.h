// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "BotTypes.h"
#include "BotStore.h"
#include <atomic>

namespace onchip {
constexpr uint32_t BotTimerMaximumSeconds = 86400, BotTimerOverdueSeconds = 60;
constexpr unsigned BotTimerSlots = 8, BotTimersPerScope = 2;
constexpr unsigned BotTimerReservedSlots = 2;
constexpr uint32_t BotTimerClockSuspendMs = 300000;
constexpr uint32_t BotTimerWaitLifetimeMs = (BotTimerMaximumSeconds + BotTimerOverdueSeconds) * 1000 + BotTimerClockSuspendMs;
bool botTimerClaimCurrent(uint32_t due, char *error, size_t capacity);
class BotTimers {
  struct Record;
  struct Restore;
  struct Storage;
  Storage *storage_ = nullptr;
  Restore *restore_ = nullptr;
  static bool validRecord(const Record &);
  bool read(char *error, size_t capacity);
public:
  ~BotTimers();
  static bool validSnapshot(const BotStore::Snapshot &, const uint8_t bot[32], char *error, size_t capacity);
  bool snapshot(const uint8_t bot[32], BotStore::Snapshot &, char *error, size_t capacity);
  void restore(const uint8_t bot[32], const BotStore::Snapshot &, BotIoResult &,
               uint32_t epoch, const std::atomic<uint32_t> &generation);
  void perform(const uint8_t bot[32], const BotIoRequest &request, BotIoResult &result,
               const std::atomic<uint32_t> &generation, const std::atomic<bool> &sharedState,
               const std::atomic<uint32_t> &sharedGrant, const std::atomic<uint32_t> *eventEpoch = nullptr);
};
}
