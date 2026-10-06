// SPDX-License-Identifier: Apache-2.0
#if MESHCORE_NODE_BACKUP && defined(ARDUINO_ARCH_ESP32)
#include "EspNodeBackup.h"
#include "FirmwareNodeBackup.h"
#include "FirmwareIdentity.h"
#include "RoleStorage.h"
#include <nvs.h>

namespace onchip {
namespace {
struct Blob { uint8_t bytes[4096]{}; };
class BlobReader final : public backup::Reader {
  const uint8_t *bytes_;
  size_t remaining_;
public:
  BlobReader(const uint8_t *bytes, size_t size) : bytes_(bytes), remaining_(size) {}
  size_t read(uint8_t *bytes, size_t size) override {
    size = std::min(size, remaining_);
    memcpy(bytes, bytes_, size); bytes_ += size; remaining_ -= size; return size;
  }
};
bool useful(const nvs_entry_info_t &entry) {
  return strcmp(entry.namespace_name, "mc-backup") &&
         (!strncmp(entry.namespace_name, "mc-", 3) || !strcmp(entry.namespace_name, "mesh-phy"));
}
bool stable(const nvs_entry_info_t &entry) {
  return strcmp(entry.namespace_name, "mc-mast-admin") ||
         (strcmp(entry.key, "replay") && strcmp(entry.key, "replay-extra") && strcmp(entry.key, "owner-replay"));
}
class EspBackup final : public FirmwareNodeBackup {
  bool records(backup::TarWriter *archive, uint8_t digest[32], char *error, size_t capacity) {
    Blob *blob = allocateRoleStorage<Blob>("node backup NVS buffer");
    if (!blob) { snprintf(error, capacity, "Backup NVS buffer allocation failed"); return false; }
    struct Release { Blob *&blob; ~Release() { releaseRoleStorage(blob); } } release{blob};
    SHA256 hash;
    hash.reset();
    unsigned count = 0;
    bool ok = true;
    auto iterator = nvs_entry_find("nvs", nullptr, NVS_TYPE_ANY);
    while (iterator && ok) {
      nvs_entry_info_t entry{};
      nvs_entry_info(iterator, &entry);
      if (useful(entry)) {
        nvs_handle_t handle;
        size_t size = sizeof(blob->bytes);
        auto result = entry.type == NVS_TYPE_BLOB ? nvs_open(entry.namespace_name, NVS_READONLY, &handle) : ESP_ERR_INVALID_STATE;
        if (result == ESP_OK) {
          result = nvs_get_blob(handle, entry.key, blob->bytes, &size);
          nvs_close(handle);
        }
        if (result != ESP_OK || nodeBackup().cancelled()) {
          snprintf(error, capacity, "Backup setting unreadable or changed: %s/%s", entry.namespace_name, entry.key);
          ok = false;
        } else {
          char name[80];
          snprintf(name, sizeof(name), "nvs/%s/%s.blob", entry.namespace_name, entry.key);
          if (stable(entry)) {
            const uint32_t length = size;
            hash.update(name, strlen(name) + 1); hash.update(&length, sizeof(length)); hash.update(blob->bytes, size);
          }
          if (archive) {
            BlobReader reader(blob->bytes, size);
            if (!archive->add(name, size, reader)) {
              snprintf(error, capacity, "Backup setting write failed: %s/%s", entry.namespace_name, entry.key);
              ok = false;
            }
          }
          ++count;
          backup::wipe(blob->bytes, sizeof(blob->bytes));
        }
      }
      if (ok) iterator = nvs_entry_next(iterator);
    }
    if (iterator) nvs_release_iterator(iterator);
    if (!count && ok) { snprintf(error, capacity, "Backup NVS inventory is empty"); ok = false; }
    if (ok) hash.finalize(digest, 32);
    return ok;
  }
  bool files(backup::TarWriter *archive, uint8_t digest[32], char *error, size_t capacity) {
    auto root = SPIFFS.open("/", "r");
    if (!root || !root.isDirectory()) { snprintf(error, capacity, "Backup filesystem inventory unavailable"); return false; }
    SHA256 hash;
    hash.reset();
    for (auto input = root.openNextFile(); input; input = root.openNextFile()) {
      if (input.isDirectory()) continue;
      const char *raw = input.path();
      if (!included(raw)) continue;
      char path[96], name[100];
      if (!raw || strlen(raw) >= sizeof(path)) { snprintf(error, capacity, "Backup filename exceeds 95 bytes"); return false; }
      strcpy(path, raw);
      const int nameSize = snprintf(name, sizeof(name), "files%s", path);
      if (nameSize < 0 || size_t(nameSize) >= sizeof(name)) { snprintf(error, capacity, "Backup archive filename exceeds 99 bytes"); return false; }
      if (archive) {
        input.close();
        if (!file(*archive, path, name, &hash, error, capacity)) return false;
      } else {
        const uint32_t size = input.size();
        hash.update(name, strlen(name) + 1); hash.update(&size, sizeof(size));
        uint8_t bytes[512];
        for (uint32_t offset = 0; offset < size;) {
          const size_t count = std::min(size_t(size - offset), sizeof(bytes));
          if (nodeBackup().cancelled() || input.read(bytes, count) != count) {
            snprintf(error, capacity, "Backup source verification failed: %.65s", path); return false;
          }
          hash.update(bytes, count); offset += count; delay(1);
        }
        backup::wipe(bytes, sizeof(bytes));
      }
    }
    hash.finalize(digest, 32); return true;
  }
public:
  bool available() const override { return backupWorkerReady(); }
  void wake() override { wakeBackupWorker(); }
  bool snapshot(backup::TarWriter &archive, char *error, size_t capacity) override {
    uint8_t beforeNvs[32], afterNvs[32], beforeFiles[32], afterFiles[32];
    if (!manifest(archive, "aspen", MESHCORE_SLP_ASPEN_VERSION) ||
        !records(&archive, beforeNvs, error, capacity) ||
        !files(&archive, beforeFiles, error, capacity) ||
        !files(nullptr, afterFiles, error, capacity) ||
        !records(nullptr, afterNvs, error, capacity)) return false;
    if (memcmp(beforeNvs, afterNvs, 32) || memcmp(beforeFiles, afterFiles, 32)) {
      snprintf(error, capacity, "Settings or files changed during backup; request a new snapshot"); return false;
    }
    return true;
  }
};
EspBackup platform;
}
void beginEspNodeBackup() { nodeBackup().begin(platform); }
} // namespace onchip
#endif
