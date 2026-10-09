// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../seams/nvs.h"
#include <algorithm>

enum nvs_type_t { NVS_TYPE_BLOB, NVS_TYPE_ANY };
struct nvs_entry_info_t {
  char namespace_name[16]{}, key[16]{};
  nvs_type_t type = NVS_TYPE_BLOB;
};
struct BackupIterator {
  std::vector<identity_test::Key> keys;
  size_t offset = 0;
};
using nvs_iterator_t = BackupIterator *;
namespace backup_test {
inline unsigned inventories = 0;
inline bool reverseNvs = false;
inline void (*beforeVerifyNvs)() = nullptr;
}
inline nvs_iterator_t nvs_entry_find(const char *, const char *, nvs_type_t) {
  auto *iterator = new BackupIterator;
  if (backup_test::inventories == 1 && backup_test::beforeVerifyNvs)
    backup_test::beforeVerifyNvs();
  for (const auto &entry : identity_test::durable) iterator->keys.push_back(entry.first);
  if (++backup_test::inventories % 2 == 0 && backup_test::reverseNvs)
    std::reverse(iterator->keys.begin(), iterator->keys.end());
  if (iterator->keys.empty()) { delete iterator; return nullptr; }
  return iterator;
}
inline void nvs_entry_info(nvs_iterator_t iterator, nvs_entry_info_t *entry) {
  const auto &key = iterator->keys.at(iterator->offset);
  strcpy(entry->namespace_name, key.first.c_str());
  strcpy(entry->key, key.second.c_str());
}
inline void nvs_release_iterator(nvs_iterator_t iterator) { delete iterator; }
inline nvs_iterator_t nvs_entry_next(nvs_iterator_t iterator) {
  if (++iterator->offset < iterator->keys.size()) return iterator;
  delete iterator;
  return nullptr;
}
