// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "BotJournal.h"
#include <SPIFFS.h>

namespace onchip {
constexpr size_t BotScheduleAuthorityEntries = 5;
constexpr uint32_t BotScheduleRecoveryBudgetMs = 10000;
struct BotSchedulePublication {
  BotIoResult::Outcome outcome = BotIoResult::Rejected;
  char error[128]{};
};
static_assert(sizeof(BotSchedulePublication) <= 136, "Keep scheduler publication stack bounded");

// Serialized by the existing storage worker, just like the KV file groups.
// A bank is never overwritten before rereading its committed NVS authority.
template<class Record, unsigned Slots> class BotScheduleFiles {
  struct Authority {
    uint8_t magic[4]{}, bank = 0, reclaim = 0, reserved[2]{};
    uint8_t hash[32]{}, digest[32]{};
  };
  static_assert(sizeof(Authority) == 72 &&
                BotScheduleAuthorityEntries == (sizeof(Authority) + 31) / 32 + 2,
                "Recheck scheduler authority NVS budget");
  const char *space_, *family_, *magic_, *bootstrap_;
  Authority authority_{};
  Record check_[Slots]{};
  bool present_ = false, initialized_ = false, namespacePresent_ = false, verified_ = false;
  bool fail(char *error, size_t capacity, const char *message) {
    snprintf(error, capacity, "%s %s", family_, message); return false;
  }
  static bool checked(const void *data, size_t size) {
    const auto *bytes = static_cast<const uint8_t *>(data);
    uint8_t digest[32];
    mesh::Utils::sha256(digest, 32, bytes, size - 32);
    return !memcmp(digest, bytes + size - 32, 32);
  }
  void path(unsigned bank, char name[48]) const {
    snprintf(name, 48, "/command-bot/%s-%c.bin", family_, bank ? 'b' : 'a');
  }
  bool admit(char *error, size_t capacity) {
    const size_t required = 126 + BotNvsMutationEntries + BotScheduleAuthorityEntries +
        (initialized_ ? 0 : BotKvBootstrapEntries) + !namespacePresent_;
    nvs_stats_t stats{};
    if (nvs_get_stats(nullptr, &stats) != ESP_OK)
      return fail(error, capacity, "NVS headroom unavailable; not published");
    if (stats.free_entries < required) {
      snprintf(error, capacity, "%s NVS headroom: %u free, %u required; not published",
               family_, unsigned(stats.free_entries), unsigned(required));
      return false;
    }
    return true;
  }
  template<class Key> bool reclaim(uint32_t started, Key key, char *error, size_t capacity) {
    for (unsigned slot = 0; slot < Slots; ++slot) {
      if (uint32_t(millis() - started) >= BotScheduleRecoveryBudgetMs)
        return fail(error, capacity, "legacy reclaim deadline; retry recovery");
      nvs_handle_t handle;
      if (nvs_open(space_, NVS_READWRITE, &handle) != ESP_OK)
        return fail(error, capacity, "legacy reclaim open failed; storage blocked");
      char name[16]; key(slot, name);
      auto status = nvs_erase_key(handle, name);
      if (status == ESP_ERR_NVS_NOT_FOUND) { nvs_close(handle); continue; }
      if (status == ESP_OK) status = nvs_commit(handle);
      nvs_close(handle);
      if (status != ESP_OK || nvs_open(space_, NVS_READONLY, &handle) != ESP_OK)
        return fail(error, capacity, "legacy reclaim failed; outcome unknown");
      size_t size = 0; status = nvs_get_blob(handle, name, nullptr, &size); nvs_close(handle);
      if (status != ESP_ERR_NVS_NOT_FOUND)
        return fail(error, capacity, "legacy reclaim readback failed; outcome unknown");
    }
    auto next = authority_; next.reclaim = 0;
    nvs_handle_t handle;
    if (nvs_open(space_, NVS_READWRITE, &handle) != ESP_OK)
      return fail(error, capacity, "migration completion unavailable");
    if (!commitBotJournal(handle, space_, "files", &next, sizeof(next), started,
                          [] { return true; }, error, capacity, BotScheduleRecoveryBudgetMs)) return false;
    authority_ = next;
    return true;
  }
public:
  Record records[Slots]{};
  void invalidate() { verified_ = false; }
  BotScheduleFiles(const char *space, const char *family, const char *magic, const char *bootstrap)
      : space_(space), family_(family), magic_(magic), bootstrap_(bootstrap) {}

  template<class Valid, class Key>
  bool recover(Valid valid, Key key, char *error, size_t capacity) {
    const uint32_t started = millis();
    const auto previous = authority_;
    const bool cached = verified_;
    verified_ = present_ = initialized_ = false; authority_ = {};
    nvs_handle_t handle;
    auto status = nvs_open(space_, NVS_READONLY, &handle);
    namespacePresent_ = status == ESP_OK;
    if (!namespacePresent_ && status != ESP_ERR_NVS_NOT_FOUND)
      return fail(error, capacity, "authority open failed; storage blocked");
    size_t size = sizeof(authority_);
    if (namespacePresent_) status = nvs_get_blob(handle, "files", &authority_, &size);
    if (status == ESP_OK && size == sizeof(authority_)) {
      nvs_close(handle);
      if (memcmp(authority_.magic, magic_, 4) || authority_.bank > 1 || authority_.reclaim > 1 ||
          authority_.reserved[0] || authority_.reserved[1] || !checked(&authority_, sizeof(authority_)))
        return fail(error, capacity, "authority corrupt; storage blocked");
      present_ = initialized_ = true;
      if (uint32_t(millis() - started) >= BotScheduleRecoveryBudgetMs)
        return fail(error, capacity, "authority read deadline; retry recovery");
      if (!cached || memcmp(&previous, &authority_, sizeof(previous))) {
        char name[48]; path(authority_.bank, name);
        auto file = SPIFFS.open(name, "r");
        const bool complete = file && file.size() == sizeof(records) &&
            file.read(reinterpret_cast<uint8_t *>(records), sizeof(records)) == sizeof(records) &&
            file.size() == sizeof(records);
        file.close();
        uint8_t hash[32]; mesh::Utils::sha256(hash, 32, reinterpret_cast<const uint8_t *>(records), sizeof(records));
        if (!complete || memcmp(hash, authority_.hash, 32) || !valid(records))
          return fail(error, capacity, "file corrupt/unavailable; storage blocked");
      }
      if (authority_.reclaim && !reclaim(started, key, error, capacity)) return false;
      if (uint32_t(millis() - started) >= BotScheduleRecoveryBudgetMs)
        return fail(error, capacity, "recovery deadline; retry recovery");
      verified_ = true;
      return true;
    }
    bool bootstrap = status == ESP_OK && size == 48 &&
        !memcmp(&authority_, bootstrap_, 4) && checked(&authority_, size);
    if (bootstrap) {
      const auto *bytes = reinterpret_cast<const uint8_t *>(&authority_);
      for (unsigned i = 4; i < 16; ++i) bootstrap = bootstrap && !bytes[i];
    }
    if (namespacePresent_ && status != ESP_ERR_NVS_NOT_FOUND && !bootstrap) {
      nvs_close(handle); return fail(error, capacity, "initialization corrupt; storage blocked");
    }
    initialized_ = bootstrap; authority_ = {};
    if (!initialized_) {
      for (unsigned bank = 0; bank < 2; ++bank) {
        char name[48]; path(bank, name);
        if (SPIFFS.exists(name)) {
          if (namespacePresent_) nvs_close(handle);
          return fail(error, capacity, "files lack authority; storage blocked");
        }
      }
    }
    bool legacy = false;
    for (unsigned slot = 0; slot < Slots; ++slot) {
      records[slot] = {};
      if (!namespacePresent_) continue;
      if (uint32_t(millis() - started) >= BotScheduleRecoveryBudgetMs) {
        nvs_close(handle); return fail(error, capacity, "legacy read deadline; retry recovery");
      }
      char name[16]; key(slot, name); size = sizeof(Record);
      status = nvs_get_blob(handle, name, &records[slot], &size);
      if (status == ESP_ERR_NVS_NOT_FOUND) continue;
      if (status != ESP_OK || size != sizeof(Record)) {
        nvs_close(handle); return fail(error, capacity, "legacy record unavailable; storage blocked");
      }
      legacy = true;
    }
    if (namespacePresent_) nvs_close(handle);
    if (uint32_t(millis() - started) >= BotScheduleRecoveryBudgetMs)
      return fail(error, capacity, "legacy read deadline; retry recovery");
    if (!valid(records)) return fail(error, capacity, "legacy records corrupt/duplicate; storage blocked");
    if (!legacy && !initialized_) return true;
    memcpy(authority_.magic, magic_, 4); authority_.reclaim = 1;
    BotSchedulePublication result;
    if (!publish(result, started, [] { return true; }, BotScheduleRecoveryBudgetMs)) {
      snprintf(error, capacity, "%s", result.error); return false;
    }
    if (!reclaim(started, key, error, capacity)) return false;
    verified_ = true;
    return true;
  }

  template<class Result, class Current>
  bool publish(Result &result, uint32_t started, Current current, uint32_t budgetMs = 2000) {
    verified_ = false;
    if (!admit(result.error, sizeof(result.error))) return false;
    if (!initialized_) {
      uint8_t marker[48]{}; memcpy(marker, bootstrap_, 4);
      nvs_handle_t handle;
      if (nvs_open(space_, NVS_READWRITE, &handle) != ESP_OK)
        return fail(result.error, sizeof(result.error), "initialization open failed; not published");
      namespacePresent_ = true; result.outcome = BotIoResult::Unknown;
      if (!commitBotJournal(handle, space_, "files", marker, sizeof(marker), started, current,
                            result.error, sizeof(result.error), budgetMs)) return false;
      initialized_ = true; result.outcome = BotIoResult::Rejected;
    }
    if (!current() || uint32_t(millis() - started) >= budgetMs)
      return fail(result.error, sizeof(result.error), "cancelled/deadline before file publication");
    if (present_) {
      char activeName[48]; path(authority_.bank, activeName);
      auto active = SPIFFS.open(activeName, "r");
      const bool complete = active && active.size() == sizeof(check_) &&
          active.read(reinterpret_cast<uint8_t *>(check_), sizeof(check_)) == sizeof(check_) &&
          active.size() == sizeof(check_);
      active.close();
      uint8_t hash[32];
      mesh::Utils::sha256(hash, 32, reinterpret_cast<const uint8_t *>(check_), sizeof(check_));
      if (!complete || memcmp(hash, authority_.hash, 32))
        return fail(result.error, sizeof(result.error), "active file corrupt/unavailable; not published");
      if (!current() || uint32_t(millis() - started) >= budgetMs)
        return fail(result.error, sizeof(result.error), "cancelled/deadline after active file verification");
    }
    auto next = authority_; memcpy(next.magic, magic_, 4);
    next.bank = present_ ? 1 - authority_.bank : 0;
    char name[48]; path(next.bank, name);
    auto file = SPIFFS.open(name, "w");
    if (!file) return fail(result.error, sizeof(result.error), "inactive file open failed; not published");
    // Record digests remain the deployed BTM1/BRM1 format used by BTD1/BRD1.
    for (auto &record : records) {
      const auto *bytes = reinterpret_cast<const uint8_t *>(&record);
      bool used = false;
      for (unsigned i = 0; i < 4; ++i) used = used || bytes[i];
      if (used) mesh::Utils::sha256(record.digest, 32, bytes, sizeof(Record) - 32);
    }
    const auto *bytes = reinterpret_cast<const uint8_t *>(records);
    mesh::Utils::sha256(next.hash, 32, bytes, sizeof(records));
    const bool written = file.write(bytes, sizeof(records)) == sizeof(records);
    file.flush(); file.close();
    auto check = SPIFFS.open(name, "r");
    const bool verified = check && check.size() == sizeof(records) &&
        check.read(reinterpret_cast<uint8_t *>(check_), sizeof(check_)) == sizeof(check_) &&
        check.size() == sizeof(records) && !memcmp(records, check_, sizeof(records));
    check.close();
    if (!written || !verified)
      return fail(result.error, sizeof(result.error), "inactive file write/readback failed; not published");
    if (!current() || uint32_t(millis() - started) >= budgetMs)
      return fail(result.error, sizeof(result.error), "cancelled/deadline before authority publication");
    nvs_handle_t handle;
    if (nvs_open(space_, NVS_READWRITE, &handle) != ESP_OK)
      return fail(result.error, sizeof(result.error), "authority open failed; not published");
    result.outcome = BotIoResult::Unknown;
    if (!commitBotJournal(handle, space_, "files", &next, sizeof(next), started, current,
                          result.error, sizeof(result.error), budgetMs)) return false;
    authority_ = next; present_ = true; result.outcome = BotIoResult::Committed;
    return true;
  }
};
}
