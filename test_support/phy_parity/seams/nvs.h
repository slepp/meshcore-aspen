#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>
#include <map>
#include <string>

using esp_err_t = int;
using nvs_handle_t = unsigned;
constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_FAIL = -1;
constexpr esp_err_t ESP_ERR_NVS_NOT_FOUND = 1;
constexpr esp_err_t ESP_ERR_NVS_INVALID_LENGTH = 2;
constexpr int NVS_READONLY = 0;
constexpr int NVS_READWRITE = 1;

namespace nvs_test {
struct Store {
  bool exists = false;
  bool fail_open = false;
  bool fail_read = false;
  bool fail_write = false;
  bool fail_commit = false;
  bool commit_despite_failure = false;
  unsigned writes = 0;
  unsigned commits = 0;
  std::vector<uint8_t> durable;
  std::vector<uint8_t> pending;
  std::map<std::string, std::vector<uint8_t>> auxiliary, pendingAuxiliary;
};
inline Store store;
inline void reset() { store = {}; }
}

inline const char* esp_err_to_name(esp_err_t) { return "injected NVS error"; }
inline esp_err_t nvs_open(const char*, int mode, nvs_handle_t* handle) {
  auto& s = nvs_test::store;
  if (s.fail_open) return ESP_FAIL;
  if (!s.exists && mode == NVS_READONLY) return ESP_ERR_NVS_NOT_FOUND;
  s.exists = true;
  *handle = 1;
  return ESP_OK;
}
inline void nvs_close(nvs_handle_t) {
  nvs_test::store.pending.clear();
  nvs_test::store.pendingAuxiliary.clear();
}
inline esp_err_t nvs_get_blob(nvs_handle_t, const char* key, void* data, size_t* length) {
  auto& s = nvs_test::store;
  if (s.fail_read) return ESP_FAIL;
  const std::vector<uint8_t> *value = &s.durable;
  if (strcmp(key, "profile")) {
    const auto found = s.auxiliary.find(key);
    if (found == s.auxiliary.end()) return ESP_ERR_NVS_NOT_FOUND;
    value = &found->second;
  }
  if (value->empty()) return ESP_ERR_NVS_NOT_FOUND;
  if (*length < value->size()) return ESP_ERR_NVS_INVALID_LENGTH;
  *length = value->size();
  memcpy(data, value->data(), *length);
  return ESP_OK;
}
inline esp_err_t nvs_set_blob(nvs_handle_t, const char* key, const void* data, size_t length) {
  auto& s = nvs_test::store;
  ++s.writes;
  if (s.fail_write) return ESP_FAIL;
  const auto* bytes = static_cast<const uint8_t*>(data);
  if (!strcmp(key, "profile")) s.pending.assign(bytes, bytes + length);
  else s.pendingAuxiliary[key].assign(bytes, bytes + length);
  return ESP_OK;
}
inline esp_err_t nvs_commit(nvs_handle_t) {
  auto& s = nvs_test::store;
  ++s.commits;
  if (s.fail_commit && !s.commit_despite_failure) return ESP_FAIL;
  if (!s.pending.empty()) s.durable = s.pending;
  for (const auto &record : s.pendingAuxiliary) s.auxiliary[record.first] = record.second;
  s.pending.clear();
  s.pendingAuxiliary.clear();
  return s.fail_commit ? ESP_FAIL : ESP_OK;
}
