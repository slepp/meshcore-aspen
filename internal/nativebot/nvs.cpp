// SPDX-License-Identifier: Apache-2.0
#include "nvs.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <cstdio>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {
using Bytes = std::vector<uint8_t>;
using Key = std::pair<std::string, std::string>;
using Records = std::map<Key, Bytes>;
constexpr char Snapshot[] = "nvs.snapshot";
constexpr uint8_t Magic[8] = {'M', 'C', 'N', 'V', 'S', '\r', '\n', 1};
constexpr size_t MaxEntries = 630, GcEntries = 126;
constexpr size_t MaxBlob = 32768, MaxSnapshot = 1024 * 1024;
constexpr size_t MaxNamespaces = 64, MaxRecords = 1024;
constexpr size_t Header = sizeof(Magic) + 4 * sizeof(uint32_t);

struct Handle {
  std::string space;
  bool writable;
  Records pending;
  std::set<Key> erased;
};
std::mutex mutex;
int directory = -1;
bool poisoned = false;
std::set<std::string> spaces;
Records committed;
Bytes host_identity;
std::map<nvs_handle_t, Handle> handles;
nvs_handle_t next_handle = 0;
uint64_t next_temp = 0;

bool valid_name(const char *name) {
  if (!name) return false;
  size_t length = 0;
  for (; length <= 15 && name[length]; ++length) {
    const unsigned char c = name[length];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '-'))
      return false;
  }
  return length > 0 && length <= 15;
}
bool valid_name(const std::string &name) {
  return valid_name(name.c_str()) && name.size() == std::strlen(name.c_str());
}
size_t blob_entries(size_t size) { return (size + 31) / 32 + 2 + (size > 400); }
size_t used_entries(const std::set<std::string> &names, const Records &records) {
  size_t used = names.size();
  for (const auto &record : records) used += blob_entries(record.second.size());
  return used;
}
void append32(Bytes &out, uint32_t value) {
  for (int i = 0; i < 4; ++i) out.push_back(uint8_t(value >> (8 * i)));
}
uint32_t read32(const Bytes &in, size_t &at) {
  uint32_t value = 0;
  for (int i = 0; i < 4; ++i) value |= uint32_t(in[at++]) << (8 * i);
  return value;
}
uint32_t crc32(const uint8_t *data, size_t size) {
  uint32_t crc = ~uint32_t(0);
  for (size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (int j = 0; j < 8; ++j)
      crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}
bool encode(const std::set<std::string> &names, const Records &records, Bytes &out) {
  if (names.size() > MaxNamespaces || records.size() > MaxRecords ||
      used_entries(names, records) > MaxEntries - GcEntries) return false;
  out.assign(Magic, Magic + sizeof(Magic));
  append32(out, 1);
  append32(out, uint32_t(names.size()));
  append32(out, uint32_t(records.size()));
  append32(out, 0);  // Payload length, filled in below.
  for (const auto &name : names) {
    if (!valid_name(name)) return false;
    out.push_back(uint8_t(name.size()));
    out.insert(out.end(), name.begin(), name.end());
  }
  for (const auto &record : records) {
    const auto &space = record.first.first;
    const auto &key = record.first.second;
    if (!valid_name(space) || !valid_name(key) || !names.count(space) ||
        record.second.size() > MaxBlob) return false;
    out.push_back(uint8_t(space.size()));
    out.insert(out.end(), space.begin(), space.end());
    out.push_back(uint8_t(key.size()));
    out.insert(out.end(), key.begin(), key.end());
    append32(out, uint32_t(record.second.size()));
    out.insert(out.end(), record.second.begin(), record.second.end());
    if (out.size() > MaxSnapshot - 4) return false;
  }
  const uint32_t payload = uint32_t(out.size() - Header);
  for (int i = 0; i < 4; ++i) out[Header - 4 + i] = uint8_t(payload >> (8 * i));
  append32(out, crc32(out.data(), out.size()));
  return out.size() <= MaxSnapshot;
}
bool decode(const Bytes &in, std::set<std::string> &names, Records &records) {
  if (in.size() < Header + 4 || in.size() > MaxSnapshot ||
      std::memcmp(in.data(), Magic, sizeof(Magic))) return false;
  size_t at = sizeof(Magic);
  const auto version = read32(in, at), name_count = read32(in, at);
  const auto record_count = read32(in, at), payload = read32(in, at);
  if (version != 1 || name_count > MaxNamespaces || record_count > MaxRecords ||
      payload != in.size() - Header - 4) return false;
  const size_t end = in.size() - 4;
  size_t checksum = end;
  if (read32(in, checksum) != crc32(in.data(), end)) return false;
  auto read_name = [&](std::string &name) {
    if (at >= end || in[at] == 0 || in[at] > 15 || in[at] > end - at - 1) return false;
    const size_t length = in[at++];
    name.assign(reinterpret_cast<const char *>(in.data() + at), length);
    at += length;
    return valid_name(name);
  };
  std::string previous;
  for (uint32_t i = 0; i < name_count; ++i) {
    std::string name;
    if (!read_name(name) || (i && name <= previous)) return false;
    names.insert(name);
    previous = name;
  }
  Key last;
  for (uint32_t i = 0; i < record_count; ++i) {
    std::string space, key;
    if (!read_name(space) || !read_name(key) || !names.count(space) ||
        end - at < 4) return false;
    const uint32_t length = read32(in, at);
    if (length > MaxBlob || length > end - at) return false;
    Key identity{space, key};
    if (i && identity <= last) return false;
    records.emplace(identity, Bytes(in.begin() + at, in.begin() + at + length));
    at += length;
    last = identity;
  }
  return at == end && used_entries(names, records) <= MaxEntries - GcEntries;
}
bool write_all(int fd, const Bytes &data) {
  size_t at = 0;
  while (at < data.size()) {
    const ssize_t count = write(fd, data.data() + at, data.size() - at);
    if (count > 0) at += size_t(count);
    else if (count < 0 && errno == EINTR) continue;
    else return false;
  }
  return true;
}
bool load(int fd, std::set<std::string> &names, Records &records) {
  int file = openat(fd, Snapshot, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (file < 0) return errno == ENOENT;
  struct stat stat{};
  bool ok = fstat(file, &stat) == 0 && S_ISREG(stat.st_mode) &&
      (stat.st_mode & 07777) == 0600 && stat.st_uid == geteuid() &&
      stat.st_nlink == 1 && stat.st_size > 0 && stat.st_size <= off_t(MaxSnapshot);
  Bytes content;
  if (ok) {
    content.resize(size_t(stat.st_size));
    size_t at = 0;
    while (at < content.size()) {
      const ssize_t count = read(file, content.data() + at, content.size() - at);
      if (count > 0) at += size_t(count);
      else if (count < 0 && errno == EINTR) continue;
      else { ok = false; break; }
    }
    uint8_t extra;
    if (ok && read(file, &extra, 1) != 0) ok = false;
  }
  if (close(file) != 0) ok = false;
  return ok && decode(content, names, records);
}
bool recover_temps(int fd) {
  const int copy = dup(fd);
  if (copy < 0) return false;
  DIR *listing = fdopendir(copy);
  if (!listing) { close(copy); return false; }
  std::vector<std::string> stale;
  bool ok = true;
  errno = 0;
  while (auto *entry = readdir(listing)) {
    const std::string name(entry->d_name);
    if (name.compare(0, std::strlen(Snapshot) + 5, "nvs.snapshot.tmp.") != 0) {
      errno = 0;
      continue;
    }
    struct stat st{};
    if (fstatat(fd, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) ||
        !S_ISREG(st.st_mode) || (st.st_mode & 07777) != 0600 ||
        st.st_uid != geteuid() || st.st_nlink != 1 ||
        st.st_size > off_t(MaxSnapshot)) { ok = false; break; }
    stale.push_back(name);
    errno = 0;
  }
  const int listing_error = errno;
  if (closedir(listing) || listing_error) ok = false;
  if (ok && !stale.empty()) {
    for (const auto &name : stale)
      if (unlinkat(fd, name.c_str(), 0)) { ok = false; break; }
    if (ok) ok = fsync(fd) == 0;
    if (ok) std::fprintf(stderr, "Host bot recovered %zu uncommitted NVS files\n", stale.size());
  }
  return ok;
}
int private_directory(const char *path) {
  if (!path || path[0] != '/') return -1;
  int fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) return -1;
  std::string path_copy(path);
  size_t start = 1;
  while (start < path_copy.size()) {
    const size_t slash = path_copy.find('/', start);
    const std::string component = path_copy.substr(start, slash - start);
    if (component.empty() || component == "." || component == "..") {
      close(fd); return -1;
    }
    int child = openat(fd, component.c_str(),
                       O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    close(fd);
    if (child < 0) return -1;
    fd = child;
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
  struct stat stat{};
  if (path_copy == "/" || path_copy.back() == '/' || fstat(fd, &stat) != 0 ||
      !S_ISDIR(stat.st_mode) || (stat.st_mode & 07777) != 0700 ||
      stat.st_uid != geteuid() || flock(fd, LOCK_EX | LOCK_NB) != 0) {
    close(fd); return -1;
  }
  return fd;
}
bool publish(const Bytes &snapshot) {
  std::string temp;
  int fd = -1;
  for (int i = 0; i < 128; ++i) {
    temp = "nvs.snapshot.tmp." + std::to_string(getpid()) + "." +
           std::to_string(++next_temp);
    fd = openat(directory, temp.c_str(),
                O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd >= 0 || errno != EEXIST) break;
  }
  if (fd < 0) return false;
  bool ok = fchmod(fd, 0600) == 0 && write_all(fd, snapshot) && fsync(fd) == 0;
  if (close(fd) != 0) ok = false;
  if (ok) ok = renameat(directory, temp.c_str(), directory, Snapshot) == 0;
  if (ok) ok = fsync(directory) == 0;
  if (!ok) unlinkat(directory, temp.c_str(), 0);
  return ok;
}
bool ready() { return directory >= 0 && !poisoned; }
} // namespace

esp_err_t native_nvs_init(const char *path) {
  std::lock_guard<std::mutex> lock(mutex);
  if (directory >= 0 || !path || path[0] != '/') return ESP_ERR_INVALID_ARG;
  int fd = private_directory(path);
  if (fd < 0) return ESP_ERR_INVALID_STATE;
  std::set<std::string> names;
  Records records;
  if (!load(fd, names, records) || !recover_temps(fd)) {
    close(fd);
    return ESP_ERR_INVALID_STATE;
  }
  directory = fd;
  spaces = std::move(names);
  committed = std::move(records);
  poisoned = false;
  return ESP_OK;
}
void native_nvs_shutdown() {
  std::lock_guard<std::mutex> lock(mutex);
  handles.clear();
  committed.clear();
  std::fill(host_identity.begin(), host_identity.end(), 0);
  host_identity.clear();
  spaces.clear();
  if (directory >= 0) close(directory);
  directory = -1;
  poisoned = false;
}
esp_err_t native_nvs_bind_bot_identity(const uint8_t *expanded, size_t size) {
  if (!expanded || size != 64 || (expanded[0] & 7) || (expanded[31] & 0xc0) != 0x40)
    return ESP_ERR_INVALID_ARG;
  std::lock_guard<std::mutex> lock(mutex);
  if (!ready()) return ESP_ERR_INVALID_STATE;
  if (!host_identity.empty())
    return host_identity == Bytes(expanded, expanded + size) ? ESP_OK : ESP_ERR_INVALID_STATE;
  // HELLO is the only key authority. Older native snapshots may contain a
  // mirror; discard it in memory and omit it from subsequent snapshot commits.
  committed.erase({"mc-onchip", "command-bot"});
  spaces.insert("mc-onchip");
  host_identity.assign(expanded, expanded + size);
  return ESP_OK;
}
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle) {
  std::lock_guard<std::mutex> lock(mutex);
  if (!name || !handle || !valid_name(name) ||
      (mode != NVS_READONLY && mode != NVS_READWRITE)) return ESP_ERR_INVALID_ARG;
  if (!ready()) return ESP_ERR_INVALID_STATE;
  if (mode == NVS_READONLY && !spaces.count(name)) return ESP_ERR_NVS_NOT_FOUND;
  if (!spaces.count(name) && spaces.size() >= MaxNamespaces)
    return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
  for (uint64_t attempt = 0; attempt < UINT32_MAX; ++attempt) {
    if (++next_handle == 0) ++next_handle;
    if (!handles.count(next_handle)) {
      handles.emplace(next_handle, Handle{name, mode == NVS_READWRITE, {}, {}});
      *handle = next_handle;
      return ESP_OK;
    }
  }
  return ESP_ERR_INVALID_STATE;
}
void nvs_close(nvs_handle_t handle) {
  std::lock_guard<std::mutex> lock(mutex);
  handles.erase(handle);
}
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *data, size_t *size) {
  std::lock_guard<std::mutex> lock(mutex);
  if (!valid_name(key) || !size) return ESP_ERR_INVALID_ARG;
  if (!ready()) return ESP_ERR_INVALID_STATE;
  auto h = handles.find(handle);
  if (h == handles.end()) return ESP_ERR_INVALID_ARG;
  if (h->second.space == "mc-onchip" && !std::strcmp(key, "command-bot") && !host_identity.empty()) {
    if (!data) { *size = host_identity.size(); return ESP_OK; }
    if (*size < host_identity.size()) { *size = host_identity.size(); return ESP_ERR_NVS_INVALID_LENGTH; }
    *size = host_identity.size();
    std::memcpy(data, host_identity.data(), *size);
    return ESP_OK;
  }
  auto found = committed.find({h->second.space, key});
  if (found == committed.end()) return ESP_ERR_NVS_NOT_FOUND;
  if (!data) { *size = found->second.size(); return ESP_OK; }
  if (*size < found->second.size()) {
    *size = found->second.size();
    return ESP_ERR_NVS_INVALID_LENGTH;
  }
  *size = found->second.size();
  if (*size) std::memcpy(data, found->second.data(), *size);
  return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *data, size_t size) {
  std::lock_guard<std::mutex> lock(mutex);
  if (!valid_name(key) || (size && !data)) return ESP_ERR_INVALID_ARG;
  if (!ready()) return ESP_ERR_INVALID_STATE;
  auto h = handles.find(handle);
  if (h == handles.end() || !h->second.writable) return ESP_ERR_INVALID_ARG;
  if (h->second.space == "mc-onchip" && !std::strcmp(key, "command-bot") && !host_identity.empty())
    return ESP_ERR_INVALID_STATE;
  if (size > MaxBlob) return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
  const Key identity{h->second.space, key};
  size_t peak = used_entries(spaces, committed) + !spaces.count(h->second.space);
  for (const auto &pending : h->second.pending)
    if (pending.first != identity) peak += blob_entries(pending.second.size());
  peak += blob_entries(size);
  if (peak > MaxEntries - GcEntries ||
      (h->second.pending.size() >= MaxRecords && !h->second.pending.count(identity)))
    return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
  Bytes value(size);
  if (size) std::memcpy(value.data(), data, size);
  h->second.pending[identity] = std::move(value);
  h->second.erased.erase(identity);
  return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key) {
  std::lock_guard<std::mutex> lock(mutex);
  if (!valid_name(key)) return ESP_ERR_INVALID_ARG;
  if (!ready()) return ESP_ERR_INVALID_STATE;
  auto h = handles.find(handle);
  if (h == handles.end() || !h->second.writable) return ESP_ERR_INVALID_ARG;
  if (h->second.space == "mc-onchip" && !std::strcmp(key, "command-bot") && !host_identity.empty())
    return ESP_ERR_INVALID_STATE;
  const Key identity{h->second.space, key};
  if (h->second.erased.count(identity) ||
      (!committed.count(identity) && !h->second.pending.count(identity)))
    return ESP_ERR_NVS_NOT_FOUND;
  h->second.pending.erase(identity);
  h->second.erased.insert(identity);
  return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle) {
  std::lock_guard<std::mutex> lock(mutex);
  if (!ready()) return ESP_ERR_INVALID_STATE;
  auto h = handles.find(handle);
  if (h == handles.end() || !h->second.writable) return ESP_ERR_INVALID_ARG;
  if (h->second.pending.empty() && h->second.erased.empty() && spaces.count(h->second.space)) return ESP_OK;
  auto names = spaces;
  auto records = committed;
  names.insert(h->second.space);
  for (const auto &key : h->second.erased) records.erase(key);
  for (const auto &record : h->second.pending) records[record.first] = record.second;
  Bytes snapshot;
  if (!encode(names, records, snapshot)) return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
  if (!publish(snapshot)) {
    poisoned = true;
    return ESP_FAIL;
  }
  spaces.swap(names);
  committed.swap(records);
  h->second.pending.clear();
  h->second.erased.clear();
  return ESP_OK;
}
esp_err_t nvs_get_stats(const char *partition, nvs_stats_t *stats) {
  std::lock_guard<std::mutex> lock(mutex);
  if (!stats || (partition && std::strcmp(partition, "nvs"))) return ESP_ERR_INVALID_ARG;
  if (!ready()) return ESP_ERR_INVALID_STATE;
  stats->total_entries = MaxEntries;
  stats->used_entries = used_entries(spaces, committed);
  stats->free_entries = stats->total_entries - stats->used_entries;
  stats->namespace_count = spaces.size();
  return ESP_OK;
}
const char *esp_err_to_name(esp_err_t error) {
  switch (error) {
  case ESP_OK: return "ESP_OK";
  case ESP_FAIL: return "ESP_FAIL";
  case ESP_ERR_NVS_NOT_FOUND: return "ESP_ERR_NVS_NOT_FOUND";
  case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
  case ESP_ERR_INVALID_ARG: return "ESP_ERR_INVALID_ARG";
  case ESP_ERR_NVS_INVALID_LENGTH: return "ESP_ERR_NVS_INVALID_LENGTH";
  case ESP_ERR_NVS_NOT_ENOUGH_SPACE: return "ESP_ERR_NVS_NOT_ENOUGH_SPACE";
  default: return "ESP_ERR_UNKNOWN";
  }
}
