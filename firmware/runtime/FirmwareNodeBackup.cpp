// SPDX-License-Identifier: Apache-2.0
#if MESHCORE_NODE_BACKUP && (defined(ARDUINO_ARCH_ESP32) || defined(NRF52_PLATFORM))
#include "FirmwareNodeBackup.h"
#include "RoleIdentity.h"
#include <Arduino.h>
#include <nvs.h>

namespace onchip {
namespace {
constexpr char Space[] = "mc-backup";
class MemoryReader final : public backup::Reader {
  const uint8_t *bytes_;
  size_t remaining_;
public:
  MemoryReader(const void *bytes, size_t size) : bytes_(static_cast<const uint8_t *>(bytes)), remaining_(size) {}
  size_t read(uint8_t *bytes, size_t size) override {
    size = std::min(size, remaining_);
    memcpy(bytes, bytes_, size); bytes_ += size; remaining_ -= size; return size;
  }
};
class FileReader final : public backup::Reader {
  File &file_;
  SHA256 *hash_;
public:
  FileReader(File &file, SHA256 *hash) : file_(file), hash_(hash) {}
  size_t read(uint8_t *bytes, size_t size) override {
    if (nodeBackup().cancelled()) return 0;
    const size_t count = file_.read(bytes, size);
    if (hash_ && count) hash_->update(bytes, count);
    delay(1); return count;
  }
};
}
const char *FirmwareNodeBackup::path(uint8_t slot) {
  return slot ? "/metadata/node-backup-b.mcb" : "/metadata/node-backup-a.mcb";
}
bool FirmwareNodeBackup::pointer(Record &record, bool &present) {
  present = false;
  nvs_handle_t handle;
  auto result = nvs_open(Space, NVS_READONLY, &handle);
  if (result == ESP_ERR_NVS_NOT_FOUND) return true;
  if (result != ESP_OK) return false;
  size_t size = sizeof(record);
  result = nvs_get_blob(handle, "active", &record, &size);
  nvs_close(handle);
  if (result == ESP_ERR_NVS_NOT_FOUND) return true;
  if (result != ESP_OK || size != sizeof(record) || memcmp(record.magic, "NBP\1", 4) ||
      record.slot > 1 || record.reserved[0] || record.reserved[1] || record.reserved[2] ||
      record.bytes < backup::HeaderBytes + backup::MacBytes || record.bytes > NodeBackup::FileLimit) return false;
  present = true; return true;
}
bool FirmwareNodeBackup::beginOutput(char *error, size_t capacity) {
  bool present;
  if (!pointer(record_, present)) {
    snprintf(error, capacity, "Backup pointer unreadable; existing backup retained"); return false;
  }
  selected_ = present ? 1 - record_.slot : 0;
  if (SPIFFS.exists(path(selected_)) && !SPIFFS.remove(path(selected_))) {
    snprintf(error, capacity, "Inactive backup file removal failed"); return false;
  }
  const uint32_t total = SPIFFS.totalBytes(), used = SPIFFS.usedBytes();
  if (!total || used > total || total - used <= 8192) {
    snprintf(error, capacity, "Backup needs filesystem headroom above the 8192-byte reserve"); return false;
  }
  budget_ = total - used - 8192;
  output_ = SPIFFS.open(path(selected_), "w");
  written_ = 0;
  if (!output_) snprintf(error, capacity, "Backup output file could not be opened");
  return bool(output_);
}
bool FirmwareNodeBackup::write(const uint8_t *bytes, size_t size) {
  if (!output_ || size > budget_ - written_ || output_.write(bytes, size) != size) return false;
  written_ += size; return true;
}
bool FirmwareNodeBackup::publish(const uint8_t expected[32], char *error, size_t capacity) {
  output_.flush(); output_.close();
  auto input = SPIFFS.open(path(selected_), "r");
  if (!input || input.size() != written_) {
    snprintf(error, capacity, "Backup output size readback failed; previous backup retained"); return false;
  }
  SHA256 hash;
  hash.reset();
  uint8_t bytes[512], digest[32];
  for (uint32_t offset = 0; offset < written_;) {
    const size_t count = std::min(size_t(written_ - offset), sizeof(bytes));
    if (nodeBackup().cancelled() || input.read(bytes, count) != count) {
      snprintf(error, capacity, "Backup output readback failed or cancelled"); return false;
    }
    hash.update(bytes, count); offset += count; delay(1);
  }
  input.close();
  hash.finalize(digest, sizeof(digest));
  if (memcmp(digest, expected, sizeof(digest))) {
    snprintf(error, capacity, "Backup output checksum readback failed; previous backup retained"); return false;
  }
  Record next;
  next.slot = selected_; next.bytes = written_; memcpy(next.digest, digest, sizeof(digest));
  nvs_handle_t handle;
  auto result = nvs_open(Space, NVS_READWRITE, &handle);
  if (result == ESP_OK) {
    result = nvs_set_blob(handle, "active", &next, sizeof(next));
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
  }
  Record actual;
  bool present;
  if (result != ESP_OK || !pointer(actual, present) || !present || memcmp(&actual, &next, sizeof(next))) {
    snprintf(error, capacity, "Backup publication outcome unknown; use backup load before retry"); return false;
  }
  record_ = actual; return true;
}
bool FirmwareNodeBackup::remove(bool published) {
  output_.close();
  bool present;
  Record active;
  if (!pointer(active, present)) return false;
  if (!published) {
    if (present && active.slot == selected_) return true;
    return !SPIFFS.exists(path(selected_)) || SPIFFS.remove(path(selected_));
  }
  nvs_handle_t handle;
  auto result = nvs_open(Space, NVS_READWRITE, &handle);
  if (result != ESP_OK) return false;
  result = nvs_erase_key(handle, "active");
  if (result == ESP_ERR_NVS_NOT_FOUND) result = ESP_OK;
  if (result == ESP_OK) result = nvs_commit(handle);
  nvs_close(handle);
  if (result != ESP_OK) return false;
  for (uint8_t slot = 0; slot < 2; ++slot)
    if (SPIFFS.exists(path(slot)) && !SPIFFS.remove(path(slot))) return false;
  return true;
}
bool FirmwareNodeBackup::size(uint32_t &bytes, uint8_t digest[32]) {
  bool present;
  if (!pointer(record_, present) || !present) return false;
  auto input = SPIFFS.open(path(record_.slot), "r");
  if (!input || input.size() != record_.bytes) return false;
  bytes = record_.bytes; memcpy(digest, record_.digest, 32); return true;
}
size_t FirmwareNodeBackup::read(uint32_t offset, uint8_t *bytes, size_t size) {
  auto input = SPIFFS.open(path(record_.slot), "r");
  if (!input || offset > record_.bytes || size > record_.bytes - offset || !input.seek(offset)) return 0;
  return input.read(bytes, size);
}
bool FirmwareNodeBackup::seal(uint8_t ephemeral[32], uint8_t nonce[16], const uint8_t recipient[32],
                              uint8_t shared[32]) {
  HardwareRNG rng;
  mesh::LocalIdentity identity(&rng);
  identity.calcSharedSecret(shared, recipient);
  memcpy(ephemeral, identity.pub_key, 32); rng.random(nonce, 16);
  backup::wipe(&identity, sizeof(identity));
  bool nonzero = false;
  for (unsigned i = 0; i < 32; ++i) nonzero |= shared[i] != 0;
  return nonzero;
}
uint32_t FirmwareNodeBackup::now() const { return millis(); }
bool FirmwareNodeBackup::manifest(backup::TarWriter &archive, const char *product, const char *version) {
  char text[256];
  const int size = snprintf(text, sizeof(text),
      "{\"schema_version\":1,\"format\":\"meshcore-node-backup\",\"product\":\"%s\",\"firmware\":\"%s\","
      "\"contents\":\"configuration,identities,source,data\"}\n", product, version);
  if (size < 0 || size_t(size) >= sizeof(text)) return false;
  MemoryReader reader(text, size);
  return archive.add("manifest.json", size, reader);
}
bool FirmwareNodeBackup::included(const char *path) {
  if (!path || path[0] != '/' || !strncmp(path, "/metadata/node-backup-", 22) ||
      !strncmp(path, "/metadata/mc-backup/", 20)) return false;
  const size_t length = strlen(path);
  return !(length >= 6 && !strcmp(path + length - 6, ".stage")) &&
         !(length >= 4 && !strcmp(path + length - 4, ".tmp"));
}
bool FirmwareNodeBackup::file(backup::TarWriter &archive, const char *path, const char *name,
                              SHA256 *digest, char *error, size_t capacity) {
  auto input = SPIFFS.open(path, "r");
  if (!input || input.isDirectory()) {
    snprintf(error, capacity, "Backup source file unavailable: %.70s", path); return false;
  }
  const size_t size = input.size();
  if (digest) {
    digest->update(name, strlen(name) + 1);
    const uint32_t length = size;
    digest->update(&length, sizeof(length));
  }
  FileReader reader(input, digest);
  if (size > backup::RawLimit || !archive.add(name, size, reader) || input.size() != size) {
    snprintf(error, capacity, "Backup source read failed or changed: %.65s", path); return false;
  }
  return true;
}
} // namespace onchip
#endif
