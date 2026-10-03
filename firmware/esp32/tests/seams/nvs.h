#pragma once
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>
using esp_err_t = int;
using nvs_handle_t = unsigned;
constexpr int ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_NVS_NOT_FOUND = 1;
constexpr int ESP_ERR_INVALID_STATE = 2, ESP_ERR_INVALID_ARG = 3;
constexpr int ESP_ERR_NVS_INVALID_LENGTH = 4;
constexpr int ESP_ERR_NVS_NOT_ENOUGH_SPACE = 5;
constexpr int NVS_READONLY = 0, NVS_READWRITE = 1;
struct nvs_stats_t { size_t used_entries = 0, free_entries = 0, total_entries = 0, namespace_count = 0; };
namespace identity_test {
using Key = std::pair<std::string, std::string>;
inline std::map<Key, std::vector<uint8_t>> durable;
struct Handle {
  std::string space;
  std::map<Key, std::vector<uint8_t>> pending;
  std::set<Key> erased;
};
inline std::map<unsigned, Handle> handles;
inline unsigned nextHandle = 0, commits = 0;
inline bool failCommit = false, failRead = false, failWrite = false;
inline bool failErase = false;
inline bool eagerWrites = false;
inline size_t freeEntries = 4096;
// Optional physical budget: worst-case blob chunk split, live replacement
// copies and a retained GC page. This is not a flash fragmentation simulator.
inline size_t entryCapacity = 0, peakEntries = 0;
inline std::set<std::string> namespaces;
inline size_t blobEntries(size_t size) { return (size + 31) / 32 + 2 + (size > 400); }
inline size_t usedEntries() {
  size_t used = namespaces.size();
  for (const auto &entry : durable) used += blobEntries(entry.second.size());
  return used;
}
inline bool reserveEntries(size_t entries) {
  const size_t peak = usedEntries() + entries;
  if (entryCapacity < 126 || peak > entryCapacity - 126) return false;
  if (peak > peakEntries) peakEntries = peak;
  return true;
}
inline bool failStats = false;
inline void (*afterWrite)() = nullptr, (*afterCommit)() = nullptr;
inline void (*afterErase)() = nullptr;
inline void (*readHook)(const char *) = nullptr;
inline void (*step)(const char *) = nullptr;
inline void checkpoint(const char *name) { if (step) step(name); }
class NvsDriver {
public:
  virtual ~NvsDriver() = default;
  virtual int open(const char *name, int mode, nvs_handle_t *handle) = 0;
  virtual void close(nvs_handle_t handle) = 0;
  virtual int getBlob(nvs_handle_t handle, const char *key, void *data,
                      size_t *size) = 0;
  virtual int setBlob(nvs_handle_t handle, const char *key, const void *data,
                      size_t size) = 0;
  virtual int eraseKey(nvs_handle_t handle, const char *key) = 0;
  virtual int commit(nvs_handle_t handle) = 0;
  virtual int getStats(const char *partition, nvs_stats_t *stats) = 0;
};
inline NvsDriver *driver = nullptr;
} // namespace identity_test
inline int nvs_get_stats(const char *partition, nvs_stats_t *stats) {
  if (identity_test::driver)
    return identity_test::driver->getStats(partition, stats);
  if (identity_test::failStats) return ESP_FAIL;
  if (identity_test::entryCapacity) {
    stats->total_entries = identity_test::entryCapacity;
    stats->used_entries = identity_test::usedEntries();
    stats->free_entries = stats->total_entries - stats->used_entries;
    stats->namespace_count = identity_test::namespaces.size();
    return ESP_OK;
  }
  stats->free_entries = identity_test::freeEntries;
  return ESP_OK;
}
inline const char *esp_err_to_name(int) { return "injected storage error"; }
inline int nvs_open(const char *name, int mode, nvs_handle_t *handle) {
  if (identity_test::driver)
    return identity_test::driver->open(name, mode, handle);
  using namespace identity_test;
  bool exists = false;
  for (const auto &entry : durable)
    if (entry.first.first == name)
      exists = true;
  exists = exists || namespaces.count(name);
  if (!exists && mode == NVS_READONLY)
    return ESP_ERR_NVS_NOT_FOUND;
  if (entryCapacity && !exists) {
    if (!reserveEntries(1)) return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
    namespaces.insert(name);
  }
  *handle = ++nextHandle;
  handles.emplace(*handle, Handle{name, {}, {}});
  return ESP_OK;
}
inline void nvs_close(nvs_handle_t handle) {
  if (identity_test::driver) {
    identity_test::driver->close(handle);
    return;
  }
  identity_test::handles.erase(handle);
}
inline int nvs_get_blob(nvs_handle_t handle, const char *key, void *data,
                        size_t *size) {
  if (identity_test::driver)
    return identity_test::driver->getBlob(handle, key, data, size);
  using namespace identity_test;
  checkpoint("nvs.before-read");
  if (readHook) readHook(key);
  if (failRead)
    return ESP_FAIL;
  const auto found = durable.find({handles.at(handle).space, key});
  if (found == durable.end())
    return ESP_ERR_NVS_NOT_FOUND;
  if (!data) {
    *size = found->second.size();
    return ESP_OK;
  }
  if (*size < found->second.size())
    return ESP_ERR_NVS_INVALID_LENGTH;
  *size = found->second.size();
  memcpy(data, found->second.data(), *size);
  checkpoint("nvs.after-read");
  return ESP_OK;
}
inline int nvs_set_blob(nvs_handle_t handle, const char *key, const void *data,
                        size_t size) {
  if (identity_test::driver)
    return identity_test::driver->setBlob(handle, key, data, size);
  using namespace identity_test;
  checkpoint("nvs.before-write");
  if (failWrite)
    return ESP_FAIL;
  if (entryCapacity && !reserveEntries(blobEntries(size)))
    return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
  const auto bytes = static_cast<const uint8_t *>(data);
  auto &value = handles.at(handle);
  value.pending[{value.space, key}] = {bytes, bytes + size};
  value.erased.erase({value.space, key});
  if (eagerWrites || entryCapacity) durable[{value.space, key}] = {bytes, bytes + size};
  if (afterWrite) afterWrite();
  checkpoint("nvs.after-write");
  return ESP_OK;
}
inline int nvs_erase_key(nvs_handle_t handle, const char *key) {
  if (identity_test::driver)
    return identity_test::driver->eraseKey(handle, key);
  using namespace identity_test;
  checkpoint("nvs.before-erase");
  if (failErase) return ESP_FAIL;
  auto &value = handles.at(handle);
  const Key identity{value.space, key};
  if (value.erased.count(identity) || (!durable.count(identity) && !value.pending.count(identity)))
    return ESP_ERR_NVS_NOT_FOUND;
  value.pending.erase(identity);
  value.erased.insert(identity);
  if (eagerWrites || entryCapacity) durable.erase(identity);
  if (afterErase) afterErase();
  checkpoint("nvs.after-erase");
  return ESP_OK;
}
inline int nvs_commit(nvs_handle_t handle) {
  if (identity_test::driver)
    return identity_test::driver->commit(handle);
  using namespace identity_test;
  checkpoint("nvs.before-commit");
  ++commits;
  if (failCommit)
    return ESP_FAIL;
  for (const auto &key : handles.at(handle).erased) durable.erase(key);
  for (const auto &entry : handles.at(handle).pending)
    durable[entry.first] = entry.second;
  if (afterCommit) afterCommit();
  checkpoint("nvs.after-commit");
  return ESP_OK;
}
