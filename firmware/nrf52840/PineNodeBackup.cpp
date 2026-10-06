// SPDX-License-Identifier: Apache-2.0
#if MESHCORE_NODE_BACKUP && NRFMAST_PRODUCTION_LUA
#include "PineNodeBackup.h"
#include "PineFilesystem.h"
#include "FirmwareIdentity.h"
#include "onchip/FirmwareNodeBackup.h"
#include "onchip/CommandBot.h"
#include <InternalFileSystem.h>

namespace nrfmast {
namespace {
class InternalReader final : public onchip::backup::Reader {
  Adafruit_LittleFS_Namespace::File &file_;
  SHA256 &hash_;
public:
  InternalReader(Adafruit_LittleFS_Namespace::File &file, SHA256 &hash) : file_(file), hash_(hash) {}
  size_t read(uint8_t *bytes, size_t size) override {
    if (onchip::nodeBackup().cancelled()) return 0;
    const int count = file_.read(bytes, size);
    if (count > 0) hash_.update(bytes, count);
    delay(1); return count > 0 ? count : 0;
  }
};
class PineBackup final : public onchip::FirmwareNodeBackup {
  struct Inventory {
    PineBackup &owner;
    onchip::backup::TarWriter *archive;
    SHA256 hash;
    char *error;
    size_t capacity;
    unsigned count = 0;
    Inventory(PineBackup &owner, onchip::backup::TarWriter *archive, char *error, size_t capacity)
        : owner(owner), archive(archive), error(error), capacity(capacity) { hash.reset(); }
  };
  static bool record(const char *path, void *context) {
    auto &inventory = *static_cast<Inventory *>(context);
    if (!included(path)) return true;
    char name[100];
    const int size = snprintf(name, sizeof(name), "files%s", path);
    if (size < 0 || size_t(size) >= sizeof(name) || ++inventory.count > 480) {
      snprintf(inventory.error, inventory.capacity, "Backup file count or filename limit exceeded"); return false;
    }
    if (!strcmp(path, "/metadata/mc-mast/pine-owner")) {
      auto owner = botFilesystem.open(path, "r");
      uint8_t identity[36];
      const uint32_t length = 72;
      if (!owner || owner.size() != length || owner.read(identity, sizeof(identity)) != sizeof(identity)) {
        snprintf(inventory.error, inventory.capacity, "Backup Pine owner record unreadable"); return false;
      }
      inventory.hash.update(name, strlen(name) + 1); inventory.hash.update(&length, sizeof(length));
      inventory.hash.update(identity, sizeof(identity));
      owner.close();
      return !inventory.archive || inventory.owner.file(*inventory.archive, path, name, nullptr, inventory.error, inventory.capacity);
    }
    if (inventory.archive)
      return inventory.owner.file(*inventory.archive, path, name, &inventory.hash, inventory.error, inventory.capacity);
    auto input = botFilesystem.open(path, "r");
    if (!input) { snprintf(inventory.error, inventory.capacity, "Backup file unreadable: %.65s", path); return false; }
    const uint32_t length = input.size();
    inventory.hash.update(name, strlen(name) + 1); inventory.hash.update(&length, sizeof(length));
    uint8_t bytes[256];
    for (uint32_t offset = 0; offset < length;) {
      const size_t count = std::min(size_t(length - offset), sizeof(bytes));
      if (onchip::nodeBackup().cancelled() || input.read(bytes, count) != count) {
        snprintf(inventory.error, inventory.capacity, "Backup file changed or unreadable: %.60s", path); return false;
      }
      inventory.hash.update(bytes, count); offset += count; delay(1);
    }
    onchip::backup::wipe(bytes, sizeof(bytes));
    return true;
  }
  bool inventory(onchip::backup::TarWriter *archive, uint8_t digest[32], char *error, size_t capacity) {
    Inventory files(*this, archive, error, capacity);
    if (!botFilesystem.visit("/command-bot", record, &files) ||
        !botFilesystem.visit("/metadata", record, &files)) {
      if (!error[0]) snprintf(error, capacity, "Backup QSPI inventory failed");
      return false;
    }
    auto root = InternalFS.open("/");
    if (!root || !root.isDirectory()) {
      snprintf(error, capacity, "Backup internal filesystem inventory failed"); return false;
    }
    for (auto input = root.openNextFile(); input; input = root.openNextFile()) {
      if (input.isDirectory()) continue;
      const char *base = input.name();
      if (!base || !*base || strchr(base, '/')) {
        snprintf(error, capacity, "Backup internal filename is invalid"); return false;
      }
      if (!strcmp(base, "_nrfstage.id") || !strcmp(base, "pine-lua-v1")) continue;
      char name[100];
      const int size = snprintf(name, sizeof(name), "config/internal/%s", base);
      if (size < 0 || size_t(size) >= sizeof(name) || ++files.count > 480) {
        snprintf(error, capacity, "Backup internal filename or file count limit exceeded"); return false;
      }
      const uint32_t length = input.size();
      files.hash.update(name, strlen(name) + 1); files.hash.update(&length, sizeof(length));
      InternalReader reader(input, files.hash);
      if (archive) {
        if (!archive->add(name, length, reader)) {
          snprintf(error, capacity, "Backup internal file read failed: %.55s", base); return false;
        }
      } else {
        uint8_t bytes[256];
        for (uint32_t offset = 0; offset < length;) {
          const size_t count = std::min(size_t(length - offset), sizeof(bytes));
          if (reader.read(bytes, count) != count) {
            snprintf(error, capacity, "Backup internal file changed: %.55s", base); return false;
          }
          offset += count;
        }
        onchip::backup::wipe(bytes, sizeof(bytes));
      }
      input.close();
    }
    files.hash.finalize(digest, 32); return true;
  }
public:
  bool available() const override {
    return botFilesystem.ready();
  }
  void wake() override {}
  bool snapshot(onchip::backup::TarWriter &archive, char *error, size_t capacity) override {
    uint8_t before[32], after[32];
    if (!manifest(archive, "pine", MESHCORE_SLP_PINE_VERSION) ||
        !inventory(&archive, before, error, capacity) ||
        !inventory(nullptr, after, error, capacity)) return false;
    if (memcmp(before, after, 32)) {
      snprintf(error, capacity, "Pine settings or files changed during backup; request a new snapshot"); return false;
    }
    return true;
  }
};
PineBackup platform;
}
void beginNodeBackup() { onchip::nodeBackup().begin(platform); }
}
#endif
