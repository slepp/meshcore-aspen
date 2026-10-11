// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <FS.h>
#include <SHA256.h>
#include <Utils.h>
#include <Packet.h>
#include <Identity.h>
#include <helpers/AdvertDataHelpers.h>
#include <cstring>
#include <algorithm>

namespace onchip {
// Latest signed advert per contact, owned by the companion's dispatch loop.
template<unsigned Limit> class CompanionAdvertCache {
  struct Entry { uint8_t key[32]{}, bytes[255]{}, size = 0; } entries_[Limit]{};
public:
  bool put(const uint8_t *key, int keySize, const uint8_t *bytes, int size) {
    if (keySize != 32 || size < 1 || size > 255) return false;
    Entry *slot = nullptr;
    for (auto &entry : entries_) {
      if (entry.size && !memcmp(entry.key, key, 32)) { slot = &entry; break; }
      if (!entry.size && !slot) slot = &entry;
    }
    if (!slot) return false;
    memcpy(slot->key, key, 32); memcpy(slot->bytes, bytes, size); slot->size = size;
    return true;
  }
  template<class Store> int get(Store &store, const uint8_t *key, int keySize, uint8_t *bytes) const {
    if (keySize == 32) for (const auto &entry : entries_)
      if (entry.size && !memcmp(entry.key, key, 32)) {
        memcpy(bytes, entry.bytes, entry.size); return entry.size;
      }
    return store.getBlobByKey(key, keySize, bytes);
  }
  void forget(const uint8_t *key) {
    for (auto &entry : entries_) if (entry.size && !memcmp(entry.key, key, 32)) entry.size = 0;
  }
  template<class Store> bool flush(Store &store) {
    for (auto &entry : entries_) if (entry.size) {
      if (!store.putBlobByKey(entry.key, 32, entry.bytes, entry.size)) return false;
      entry.size = 0;
    }
    return true;
  }
};
namespace companion_persistence {
constexpr size_t ContactBytes = 152;
inline bool contactsValid(fs::FS &fs, const char *path, unsigned limit) {
  auto file = fs.open(path, "r");
  if (!file || file.size() > limit * ContactBytes + 32) return false;
  const size_t size = file.size();
  // Older applications may rewrite legacy records or clear all contacts after rollback.
  if (size % ContactBytes == 0) return strcmp(path, "/contacts3-writing") != 0;
  if (size % ContactBytes != 32) return false;
  SHA256 hash; uint8_t buffer[128], expected[32], actual[32];
  size_t remaining = size - 32;
  while (remaining) {
    const size_t count = std::min(remaining, sizeof(buffer));
    if (file.read(buffer, count) != count) return false;
    hash.update(buffer, count); remaining -= count;
  }
  if (file.read(expected, 32) != 32) return false;
  hash.finalize(actual, 32); return !memcmp(actual, expected, 32);
}
inline uint8_t blobRead(fs::FS &fs, const char *path, uint8_t *bytes) {
  auto file = fs.open(path, "r");
  if (!file || file.size() < 100 || file.size() > 255) return 0;
  const uint8_t size = file.size();
  if (file.read(bytes, size) != size) return 0;
  mesh::Packet packet{};
  if (!packet.readFrom(bytes, size) || packet.getRawLength() != size ||
      packet.getPayloadVer() != PAYLOAD_VER_1 || packet.getPayloadType() != PAYLOAD_TYPE_ADVERT ||
      packet.payload_len < 100 || packet.payload_len > 100 + MAX_ADVERT_DATA_SIZE) return 0;
  const size_t dataSize = packet.payload_len - 100;
  uint8_t message[36 + MAX_ADVERT_DATA_SIZE]{};
  memcpy(message, packet.payload, 36); memcpy(message + 36, packet.payload + 100, dataSize);
  mesh::Identity identity(packet.payload);
  AdvertDataParser advert(packet.payload + 100, dataSize);
  return identity.verify(packet.payload + 36, message, 36 + dataSize) &&
      advert.isValid() && advert.hasName() ? size : 0;
}
template<class Valid> bool publish(fs::FS &fs, const char *stage, const char *path, Valid valid) {
  if (!valid(stage)) return false;
  char previous[72]; snprintf(previous, sizeof(previous), "%s-prev", path);
  if (fs.exists(path)) {
    if (valid(path)) {
      if (fs.exists(previous) && !fs.remove(previous)) return false;
      if (!fs.rename(path, previous)) return false;
    } else if (!fs.remove(path)) return false;
  }
  // The prior valid file survives a reset or failure before this rename.
  return fs.rename(stage, path) && valid(path);
}
inline uint8_t blobGet(fs::FS &fs, const char *path, uint8_t *bytes) {
  uint8_t size = blobRead(fs, path, bytes);
  if (size) return size;
  char previous[72]; snprintf(previous, sizeof(previous), "%s-prev", path);
  return blobRead(fs, previous, bytes);
}
inline bool blobPut(fs::FS &fs, const char *path, const uint8_t *bytes, uint8_t size) {
  constexpr const char *stage = "/bl-writing";
  auto file = fs.open(stage, "w");
  bool ok = file && file.write(bytes, size) == size;
  if (file) { file.flush(); file.close(); }
  uint8_t check[255];
  ok = ok && blobRead(fs, stage, check) == size && !memcmp(bytes, check, size);
  if (ok) ok = publish(fs, stage, path, [&](const char *name) { return blobRead(fs, name, check) != 0; });
  if (!ok) fs.remove(stage);
  else {
    char previous[72]; snprintf(previous, sizeof(previous), "%s-prev", path);
    if (fs.exists(previous) && !fs.remove(previous)) return false;
  }
  return ok;
}
inline void blobDelete(fs::FS &fs, const char *path) {
  fs.remove(path);
  char previous[72]; snprintf(previous, sizeof(previous), "%s-prev", path); fs.remove(previous);
}
} // namespace companion_persistence
} // namespace onchip
