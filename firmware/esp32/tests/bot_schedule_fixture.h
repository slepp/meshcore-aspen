// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "BotReminders.h"
#include <Utils.h>
#include <nvs.h>
#include <cassert>

namespace schedule_fixture {
constexpr uint32_t Epoch = 1767225600;
struct Timer {
  uint8_t magic[4]{}, bot[32]{}, principal[32]{};
  char name[onchip::BotKeyLimit + 1]{};
  uint8_t scope = 0, state = 0, reserved = 0;
  uint32_t due = 0, revision = 0;
  uint8_t digest[32]{};
};
struct Reminder {
  uint8_t magic[4]{}, bot[32]{}, principal[32]{};
  uint32_t id = 0, revision = 0, due = 0, sourceGeneration = 0;
  uint8_t state = 0, reserved[3]{};
  char text[onchip::BotReminderTextLimit + 4]{};
  uint8_t digest[32]{};
};
static_assert(sizeof(Timer) == 144 && sizeof(Reminder) == 244, "Deployed scheduler format");
template<class T> void seal(T &record) {
  mesh::Utils::sha256(record.digest, 32, reinterpret_cast<const uint8_t *>(&record), offsetof(T, digest));
}
inline Timer timer(unsigned slot) {
  Timer record;
  memcpy(record.magic, "BTM\1", 4);
  record.bot[0] = 1; record.bot[31] = slot % 2;
  record.principal[0] = 7; record.principal[31] = slot;
  record.scope = slot >= onchip::BotTimerSlots ? unsigned(onchip::BotIoRequest::Bot) : slot % 4;
  if (record.scope == onchip::BotIoRequest::Bot) {
    memset(record.principal, 0, 32); record.bot[30] = slot;
  }
  record.state = 1 + slot % 4;
  record.due = Epoch; record.revision = slot + 1;
  snprintf(record.name, sizeof(record.name), "timer%u", slot);
  seal(record); return record;
}
inline Reminder reminder(unsigned slot) {
  Reminder record;
  memcpy(record.magic, "BRM\1", 4);
  record.bot[0] = 1; record.bot[31] = slot % 2;
  record.principal[0] = 7; record.principal[31] = slot;
  record.id = record.revision = slot + 1;
  record.due = Epoch; record.sourceGeneration = slot + 10;
  record.state = 1 + slot % 5;
  snprintf(record.text, sizeof(record.text), "reminder%u", slot);
  seal(record); return record;
}
template<class T> void seed(const char *space, const char *key, const T &record) {
  nvs_handle_t handle;
  assert(nvs_open(space, NVS_READWRITE, &handle) == ESP_OK);
  assert(nvs_set_blob(handle, key, &record, sizeof(record)) == ESP_OK && nvs_commit(handle) == ESP_OK);
  nvs_close(handle);
}
inline void seedLegacy() {
  for (unsigned slot = 0; slot < onchip::BotTimerSlots + onchip::BotTimerReservedSlots; ++slot) {
    char key[16]; snprintf(key, sizeof(key), "%c%02u", slot < onchip::BotTimerSlots ? 't' : 'r',
                           slot < onchip::BotTimerSlots ? slot : slot - onchip::BotTimerSlots);
    seed("mc-bot-timer", key, timer(slot));
  }
  for (unsigned slot = 0; slot < onchip::BotReminderSlots; ++slot) {
    char key[16]; snprintf(key, sizeof(key), "r%02u", slot);
    seed("mc-bot-remind", key, reminder(slot));
  }
}
inline void verifyLegacy(onchip::BotTimers &timers, onchip::BotReminders &reminders) {
  char error[128];
  for (unsigned slot = 0; slot < onchip::BotTimerSlots + onchip::BotTimerReservedSlots; ++slot) {
    const auto expected = timer(slot);
    onchip::BotStore::Snapshot data; data.scope = expected.scope;
    memcpy(data.principal, expected.principal, 32);
    assert(timers.snapshot(expected.bot, data, error, sizeof(error)) && data.count == 1);
    assert(!memcmp(data.entries, &expected, sizeof(expected)));
  }
  for (unsigned slot = 0; slot < onchip::BotReminderSlots; ++slot) {
    const auto expected = reminder(slot);
    onchip::BotStore::Snapshot data; memcpy(data.principal, expected.principal, 32);
    assert(reminders.snapshot(expected.bot, data, error, sizeof(error)) && data.count == 1);
    assert(!memcmp(data.entries, &expected, sizeof(expected)));
  }
}
}
