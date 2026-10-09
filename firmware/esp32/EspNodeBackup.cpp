// SPDX-License-Identifier: Apache-2.0
#if MESHCORE_NODE_BACKUP && defined(ARDUINO_ARCH_ESP32)
#include "EspNodeBackup.h"
#include "FirmwareNodeBackup.h"
#include "FirmwareIdentity.h"
#include "RoleStorage.h"
#include <nvs.h>

namespace onchip {
namespace {
struct Inventory {
  struct Digest { uint8_t bytes[32]; } entries[backup::EntryLimit]{};
  size_t count = 0;
  bool add(SHA256 &hash) {
    if (count == backup::EntryLimit) return false;
    hash.finalize(entries[count++].bytes, 32);
    return true;
  }
  void finish(uint8_t digest[32]) {
    // Storage iterators can change order while the encrypted output grows.
    std::sort(entries, entries + count, [](const Digest &a, const Digest &b) {
      return memcmp(a.bytes, b.bytes, 32) < 0;
    });
    SHA256 hash;
    hash.reset();
    hash.update(entries, count * sizeof(Digest));
    hash.finalize(digest, 32);
  }
};
struct Blob { uint8_t bytes[4096]{}; Inventory inventory; };
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
struct Snapshot {
  struct Entry { char name[100]{}; uint32_t size = 0; uint8_t *bytes = nullptr; bool verified = false; };
  Entry entries[backup::EntryLimit - 1]{};
  size_t count = 0;
  uint32_t rawBytes = 2048; // Manifest header/payload and tar terminator.
  const char *error = nullptr;
  Entry *find(const char *name) {
    for (size_t i = 0; i < count; ++i)
      if (!strcmp(entries[i].name, name)) return &entries[i];
    return nullptr;
  }
  ~Snapshot() {
    for (size_t i = 0; i < count; ++i) {
      if (entries[i].bytes) {
        backup::wipe(entries[i].bytes, entries[i].size);
        heap_caps_free(entries[i].bytes);
      }
    }
  }
  bool add(const char *name, uint32_t size, backup::Reader &reader) {
    if (!backup::TarWriter::validName(name) || count == backup::EntryLimit - 1 ||
        size > backup::RawLimit - 1536) {
      error = "Backup snapshot filename, size or record limit exceeded"; return false;
    }
    const uint32_t padded = (size + 511) / 512 * 512;
    if (512 + padded > backup::RawLimit - rawBytes) {
      error = "Backup snapshot exceeds the 2 MiB archive limit"; return false;
    }
    for (size_t i = 0; i < count; ++i) {
      if (!strcmp(entries[i].name, name)) {
        error = "Backup source inventory repeated a filename; request a new snapshot"; return false;
      }
    }
    auto &entry = entries[count++];
    strcpy(entry.name, name); entry.size = size;
    if (size) {
      entry.bytes = static_cast<uint8_t *>(heap_caps_calloc(1, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
      if (!entry.bytes) {
        entry.size = 0;
        error = "Backup snapshot PSRAM allocation failed"; return false;
      }
      for (uint32_t offset = 0; offset < size;) {
        const size_t chunk = std::min(size_t(size - offset), size_t(512));
        if (reader.read(entry.bytes + offset, chunk) != chunk) {
          error = "Backup source read failed during snapshot capture"; return false;
        }
        offset += chunk;
      }
    }
    rawBytes += 512 + padded;
    return true;
  }
  bool emit(backup::TarWriter &archive, char *message, size_t capacity) {
    for (size_t i = 0; i < count; ++i) {
      const auto &entry = entries[i];
      BlobReader reader(entry.bytes, entry.size);
      if (!archive.add(entry.name, entry.size, reader)) {
        snprintf(message, capacity, "Backup snapshot output failed: %.75s", entry.name); return false;
      }
    }
    return true;
  }
};
class FileReader final : public backup::Reader {
  File &file_;
  SHA256 &hash_;
public:
  FileReader(File &file, SHA256 &hash) : file_(file), hash_(hash) {}
  size_t read(uint8_t *bytes, size_t size) override {
    if (nodeBackup().cancelled()) return 0;
    const size_t count = file_.read(bytes, size);
    hash_.update(bytes, count);
    delay(1);
    return count;
  }
};
bool useful(const nvs_entry_info_t &entry) {
  return strcmp(entry.namespace_name, "mc-backup") &&
         (!strncmp(entry.namespace_name, "mc-", 3) || !strcmp(entry.namespace_name, "mesh-phy"));
}
bool stable(const nvs_entry_info_t &entry) {
  return strcmp(entry.namespace_name, "mc-mast-admin") ||
         (strcmp(entry.key, "replay-extra") && strcmp(entry.key, "owner-replay"));
}
class EspBackup final : public FirmwareNodeBackup {
  struct MemoryBackup {
    uint8_t *bytes = nullptr, digest[32]{};
    uint32_t size = 0;
    void clear() {
      if (bytes) {
        backup::wipe(bytes, size);
        heap_caps_free(bytes);
      }
      bytes = nullptr; size = 0; backup::wipe(digest, sizeof(digest));
    }
  } pending_, saved_;
  bool transient_ = false;
  bool records(Snapshot *archive, uint8_t digest[32], char *error, size_t capacity) {
    Blob *blob = allocateRoleStorage<Blob>("node backup NVS buffer");
    if (!blob) { snprintf(error, capacity, "Backup NVS buffer allocation failed"); return false; }
    struct Release { Blob *&blob; ~Release() { releaseRoleStorage(blob); } } release{blob};
    SHA256 hash;
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
            hash.reset();
            hash.update(name, strlen(name) + 1); hash.update(&length, sizeof(length)); hash.update(blob->bytes, size);
            if (!blob->inventory.add(hash)) {
              snprintf(error, capacity, "Backup NVS inventory exceeds 512 records");
              ok = false;
            }
          }
          if (ok && archive) {
            BlobReader reader(blob->bytes, size);
            if (!archive->add(name, size, reader)) {
              snprintf(error, capacity, "%s", archive->error);
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
    if (ok) blob->inventory.finish(digest);
    return ok;
  }
  bool files(Snapshot *archive, uint8_t digest[32], char *error, size_t capacity,
             Snapshot *captured = nullptr) {
    Inventory *inventory = allocateRoleStorage<Inventory>("node backup file inventory");
    if (!inventory) { snprintf(error, capacity, "Backup file inventory allocation failed"); return false; }
    struct Release { Inventory *&inventory; ~Release() { releaseRoleStorage(inventory); } } release{inventory};
    auto root = SPIFFS.open("/", "r");
    if (!root || !root.isDirectory()) { snprintf(error, capacity, "Backup filesystem inventory unavailable"); return false; }
    SHA256 hash;
    for (auto input = root.openNextFile(); input; input = root.openNextFile()) {
      if (input.isDirectory()) continue;
      const char *raw = input.path();
      if (!included(raw)) continue;
      char path[96], name[100];
      if (!raw || strlen(raw) >= sizeof(path)) { snprintf(error, capacity, "Backup filename exceeds 95 bytes"); return false; }
      strcpy(path, raw);
      const int nameSize = snprintf(name, sizeof(name), "files%s", path);
      if (nameSize < 0 || size_t(nameSize) >= sizeof(name)) { snprintf(error, capacity, "Backup archive filename exceeds 99 bytes"); return false; }
      hash.reset();
      if (archive) {
        const uint32_t size = input.size();
        hash.update(name, strlen(name) + 1); hash.update(&size, sizeof(size));
        FileReader reader(input, hash);
        if (!archive->add(name, size, reader)) {
          snprintf(error, capacity, "%s", archive->error); return false;
        }
        if (input.size() != size) {
          snprintf(error, capacity, "Backup source size changed during capture: %.65s", path); return false;
        }
      } else {
        const uint32_t size = input.size();
        auto *entry = captured ? captured->find(name) : nullptr;
        if (captured && (!entry || entry->verified || entry->size != size)) {
          snprintf(error, capacity, "Saved files changed during capture: %.65s", path); return false;
        }
        hash.update(name, strlen(name) + 1); hash.update(&size, sizeof(size));
        uint8_t bytes[512];
        for (uint32_t offset = 0; offset < size;) {
          const size_t count = std::min(size_t(size - offset), sizeof(bytes));
          if (nodeBackup().cancelled() || input.read(bytes, count) != count) {
            backup::wipe(bytes, sizeof(bytes));
            snprintf(error, capacity, "Backup source verification failed: %.65s", path); return false;
          }
          if (entry && memcmp(bytes, entry->bytes + offset, count)) {
            backup::wipe(bytes, sizeof(bytes));
            snprintf(error, capacity, "Saved files changed during capture: %.65s", path); return false;
          }
          hash.update(bytes, count); offset += count; delay(1);
        }
        backup::wipe(bytes, sizeof(bytes));
        if (input.size() != size) {
          snprintf(error, capacity, "Saved files changed during capture: %.65s", path); return false;
        }
        if (entry) entry->verified = true;
      }
      if (!inventory->add(hash)) {
        snprintf(error, capacity, "Backup file inventory exceeds 512 records"); return false;
      }
    }
    if (captured) {
      for (size_t i = 0; i < captured->count; ++i) {
        const auto &entry = captured->entries[i];
        if (!strncmp(entry.name, "files/", 6) && !entry.verified) {
          snprintf(error, capacity, "Saved files changed during capture: %.65s", entry.name + 5); return false;
        }
      }
    }
    inventory->finish(digest); return true;
  }
public:
  const char *lastError() const override {
    return transient_ ? nullptr : FirmwareNodeBackup::lastError();
  }
  bool selectStorage(bool transient, bool load) override {
    if (transient && load && !saved_.bytes) return false;
    transient_ = transient;
    return true;
  }
  bool beginOutput(char *error, size_t capacity) override {
    if (!transient_) return FirmwareNodeBackup::beginOutput(error, capacity);
    pending_.clear();
    pending_.bytes = static_cast<uint8_t *>(heap_caps_calloc(
        1, NodeBackup::FileLimit, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!pending_.bytes) {
      snprintf(error, capacity, "Volatile backup PSRAM allocation failed"); return false;
    }
    return true;
  }
  bool write(const uint8_t *bytes, size_t size) override {
    if (!transient_) return FirmwareNodeBackup::write(bytes, size);
    if (!pending_.bytes || size > NodeBackup::FileLimit - pending_.size) return false;
    memcpy(pending_.bytes + pending_.size, bytes, size);
    pending_.size += size; return true;
  }
  bool publish(const uint8_t expected[32], char *error, size_t capacity) override {
    if (!transient_) return FirmwareNodeBackup::publish(expected, error, capacity);
    SHA256 hash;
    hash.reset(); hash.update(pending_.bytes, pending_.size);
    hash.finalize(pending_.digest, 32);
    if (memcmp(pending_.digest, expected, 32)) {
      snprintf(error, capacity, "Volatile backup checksum readback failed"); return false;
    }
    saved_.clear();
    saved_ = pending_;
    pending_ = {};
    return true;
  }
  bool remove(bool published) override {
    if (!transient_) return FirmwareNodeBackup::remove(published);
    pending_.clear();
    if (published) saved_.clear();
    return true;
  }
  bool size(uint32_t &bytes, uint8_t digest[32]) override {
    if (!transient_) return FirmwareNodeBackup::size(bytes, digest);
    if (!saved_.bytes) return false;
    bytes = saved_.size; memcpy(digest, saved_.digest, 32); return true;
  }
  size_t read(uint32_t offset, uint8_t *bytes, size_t size) override {
    if (!transient_) return FirmwareNodeBackup::read(offset, bytes, size);
    if (!saved_.bytes || offset > saved_.size || size > saved_.size - offset) return 0;
    memcpy(bytes, saved_.bytes + offset, size); return size;
  }
  bool available() const override { return backupWorkerReady(); }
  void wake() override { wakeBackupWorker(); }
  bool snapshot(backup::TarWriter &archive, char *error, size_t capacity) override {
    Snapshot *snapshot = allocateRoleStorage<Snapshot>("node backup snapshot");
    if (!snapshot) { snprintf(error, capacity, "Backup snapshot inventory allocation failed"); return false; }
    struct Release { Snapshot *&snapshot; ~Release() { releaseRoleStorage(snapshot); } } release{snapshot};
    uint8_t beforeNvs[32], afterNvs[32], beforeFiles[32], afterFiles[32];
    // Validate immutable PSRAM copies before slow encrypted filesystem writes.
    if (!records(snapshot, beforeNvs, error, capacity) ||
        !files(snapshot, beforeFiles, error, capacity) ||
        !files(nullptr, afterFiles, error, capacity, snapshot) ||
        !records(nullptr, afterNvs, error, capacity)) return false;
    if (memcmp(beforeNvs, afterNvs, 32)) {
      snprintf(error, capacity, "NVS settings changed during backup; pause settings/data edits and request a new snapshot"); return false;
    }
    if (memcmp(beforeFiles, afterFiles, 32)) {
      snprintf(error, capacity, "Saved files changed during backup; pause settings/data edits and request a new snapshot"); return false;
    }
    if (!manifest(archive, "aspen", MESHCORE_SLP_ASPEN_VERSION)) {
      snprintf(error, capacity, "Backup manifest output failed"); return false;
    }
    if (!snapshot->emit(archive, error, capacity)) {
      const auto *reason = lastError();
      if (reason && *reason) snprintf(error, capacity, "%s", reason);
      return false;
    }
    return true;
  }
};
EspBackup platform;
}
void beginEspNodeBackup() { nodeBackup().begin(platform); }
} // namespace onchip
#endif
