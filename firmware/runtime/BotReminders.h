// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "BotTimers.h"

namespace onchip {
constexpr unsigned BotReminderSlots = 8, BotRemindersPerCaller = 2;
constexpr size_t BotReminderTextLimit = 120;
struct BotReminderDispatch {
  uint8_t principal[32]{};
  uint32_t id = 0, revision = 0, due = 0, sourceGeneration = 0;
  uint32_t generation = 0, grant = 0;
  char text[BotReminderTextLimit + 1]{};
  BotReminderState outcome = BotReminderState::Unknown;
  bool approved = false;
};
class BotReminders {
  struct Record;
  struct Storage;
  Storage *storage_ = nullptr;
  Record *records_ = nullptr;
  unsigned nextSlot_ = 0;
  bool read(char *error, size_t capacity);
  bool transition(unsigned slot, BotReminderState state,
                  const std::atomic<bool> &stopping, char *error, size_t capacity,
                  BotIoResult *outcome = nullptr);
  static bool validRecord(const Record &);
public:
  ~BotReminders();
  static bool validSnapshot(const BotStore::Snapshot &, const uint8_t bot[32], char *error, size_t capacity);
  bool snapshot(const uint8_t bot[32], BotStore::Snapshot &, char *error, size_t capacity);
  void restore(const uint8_t bot[32], const BotStore::Snapshot &, BotIoResult &,
               uint32_t epoch, const std::atomic<uint32_t> &generation);
  void perform(const uint8_t bot[32], const BotIoRequest &, BotIoResult &,
               const std::atomic<uint32_t> &generation, const std::atomic<bool> &enabled,
               const std::atomic<uint32_t> &grant, const std::atomic<bool> &stopping,
               uint32_t protectedId);
  bool next(const uint8_t bot[32], BotReminderDispatch &, const std::atomic<bool> &stopping,
            char *error, size_t capacity);
  bool claim(const uint8_t bot[32], BotReminderDispatch &, const std::atomic<bool> &enabled,
             const std::atomic<uint32_t> &grant, const std::atomic<bool> &stopping,
             char *error, size_t capacity);
  bool complete(const uint8_t bot[32], const BotReminderDispatch &,
                const std::atomic<bool> &stopping, char *error, size_t capacity);
};
}
