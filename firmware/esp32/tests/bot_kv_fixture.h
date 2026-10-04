// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "BotTypes.h"
#include <Utils.h>
#include <SPIFFS.h>
#include <nvs.h>

namespace kv_fixture {
struct Record {
  uint8_t magic[4]{'B', 'K', 'V', 1}, bot[32]{};
  struct Entry {
    uint8_t used = 0, scope = 0, principal[32]{};
    char key[onchip::BotKeyLimit + 1]{}, value[onchip::BotValueLimit + 1]{};
  } entry;
  uint8_t digest[32]{};
};
struct Redo {
  uint8_t magic[4]{'B', 'T', 'X', 1}, count = 0, slots[8]{}, reserved[3]{};
  Record records[8]{};
  uint8_t digest[32]{};
};
static_assert(sizeof(Record) == 392 && sizeof(Redo) == 3184);
inline void seal(void *data, size_t size) {
  auto *bytes = static_cast<uint8_t *>(data);
  mesh::Utils::sha256(bytes + size - 32, 32, bytes, size - 32);
}
inline Record record(unsigned bot, unsigned scope, unsigned principal, const char *key, const char *value) {
  Record r; r.bot[0] = bot; r.entry.scope = scope; r.entry.principal[0] = principal;
  r.entry.used = key != nullptr;
  if (key) strcpy(r.entry.key, key);
  if (value) strcpy(r.entry.value, value);
  seal(&r, sizeof(r));
  return r;
}
inline void seed(unsigned slot, const Record &record) {
  char key[16]; snprintf(key, sizeof(key), "%c%02u", slot < 32 ? 'v' : 'r', slot < 32 ? slot : slot - 32);
  const auto *bytes = reinterpret_cast<const uint8_t *>(&record);
  identity_test::durable[{"mc-bot-kv", key}] = {bytes, bytes + sizeof(record)};
}
inline void seedRedo(Redo &redo) {
  seal(&redo, sizeof(redo));
  const auto *bytes = reinterpret_cast<const uint8_t *>(&redo);
  identity_test::durable[{"mc-bot-kv", "txn"}] = {bytes, bytes + sizeof(redo)};
}
inline std::vector<uint8_t> &authority() {
  return identity_test::durable.at({"mc-bot-kv", "txn"});
}
inline std::vector<uint8_t> &file(unsigned slot) {
  char path[40];
  const unsigned shard = slot / 8, bank = (authority().at(4) >> shard) & 1;
  snprintf(path, sizeof(path), "/command-bot/kv-%u-%c.bin", shard, bank ? 'b' : 'a');
  return filesystem_test::files.at(path);
}
inline Record read(unsigned slot) {
  Record r;
  memcpy(static_cast<void *>(&r), file(slot).data() + (slot % 8) * sizeof(r), sizeof(r));
  return r;
}
inline void replace(unsigned slot, const Record &r) {
  auto &bytes = file(slot);
  memcpy(bytes.data() + (slot % 8) * sizeof(r), &r, sizeof(r));
  auto &a = authority();
  mesh::Utils::sha256(a.data() + 8 + (slot / 8) * 32, 32, bytes.data(), bytes.size());
  seal(a.data(), a.size());
}
}
