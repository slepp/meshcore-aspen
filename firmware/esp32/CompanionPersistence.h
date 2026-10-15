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
enum class Status { Valid, Invalid, Unreadable };
inline Status contactsStatus(fs::FS &fs, const char *path, unsigned limit) {
  auto file = fs.open(path, "r");
  if (!file) return Status::Unreadable;
  if (file.size() > limit * ContactBytes + 32) return Status::Invalid;
  const size_t size = file.size();
  // Older applications may rewrite legacy records or clear all contacts after rollback.
  const bool legacy = size % ContactBytes == 0;
  if (!legacy && size % ContactBytes != 32) return Status::Invalid;
  SHA256 hash; uint8_t buffer[128], expected[32], actual[32];
  size_t remaining = legacy ? size : size - 32;
  while (remaining) {
    const size_t count = std::min(remaining, sizeof(buffer));
    if (file.read(buffer, count) != count) return Status::Unreadable;
    hash.update(buffer, count); remaining -= count;
  }
  if (legacy) return strcmp(path, "/contacts3-writing") ? Status::Valid : Status::Invalid;
  if (file.read(expected, 32) != 32) return Status::Unreadable;
  hash.finalize(actual, 32);
  return !memcmp(actual, expected, 32) ? Status::Valid : Status::Invalid;
}
inline bool contactsValid(fs::FS &fs, const char *path, unsigned limit) {
  return contactsStatus(fs, path, limit) == Status::Valid;
}
inline Status blobStatus(fs::FS &fs, const char *path, uint8_t *bytes, uint8_t &size) {
  size = 0;
  auto file = fs.open(path, "r");
  if (!file) return Status::Unreadable;
  if (file.size() < 100 || file.size() > 255) return Status::Invalid;
  const uint8_t length = file.size();
  if (file.read(bytes, length) != length) return Status::Unreadable;
  mesh::Packet packet{};
  if (!packet.readFrom(bytes, length) || packet.getRawLength() != length ||
      packet.getPayloadVer() != PAYLOAD_VER_1 || packet.getPayloadType() != PAYLOAD_TYPE_ADVERT ||
      packet.payload_len < 100 || packet.payload_len > 100 + MAX_ADVERT_DATA_SIZE) return Status::Invalid;
  const size_t dataSize = packet.payload_len - 100;
  uint8_t message[36 + MAX_ADVERT_DATA_SIZE]{};
  memcpy(message, packet.payload, 36); memcpy(message + 36, packet.payload + 100, dataSize);
  mesh::Identity identity(packet.payload);
  AdvertDataParser advert(packet.payload + 100, dataSize);
  if (!identity.verify(packet.payload + 36, message, 36 + dataSize) ||
      !advert.isValid() || !advert.hasName()) return Status::Invalid;
  size = length;
  return Status::Valid;
}
inline uint8_t blobRead(fs::FS &fs, const char *path, uint8_t *bytes) {
  uint8_t size;
  blobStatus(fs, path, bytes, size);
  return size;
}
class CheckedStream final : public Stream {
  File &file_;
public:
  SHA256 hash;
  size_t written = 0;
  bool failed = false;
  int firstRead = -1, lastRead = -1;
  explicit CheckedStream(File &file) : file_(file) {}
  size_t write(uint8_t byte) override { return write(&byte, 1); }
  size_t write(const uint8_t *bytes, size_t size) override {
    const size_t count = file_.write(bytes, size);
    hash.update(bytes, size); written += size;
    failed = failed || count != size;
    return count;
  }
  int read() override {
    const bool pending = file_.available() > 0;
    const int value = file_.read();
    failed = failed || (pending && value < 0);
    if (value >= 0 && value != ' ' && value != '\r' && value != '\n' && value != '\t') {
      if (firstRead < 0) firstRead = value;
      lastRead = value;
    }
    return value;
  }
  int available() override { return file_.available(); }
  int peek() override { return -1; }
  void flush() override { file_.flush(); }
};
inline bool matches(fs::FS &fs, const char *path, size_t size, const uint8_t expected[32]) {
  auto file = fs.open(path, "r");
  if (!file || file.size() != size) return false;
  SHA256 hash; uint8_t bytes[128], digest[32];
  while (size) {
    const size_t count = std::min(size, sizeof(bytes));
    if (file.read(bytes, count) != count) return false;
    hash.update(bytes, count); size -= count;
  }
  hash.finalize(digest, sizeof(digest));
  return !memcmp(digest, expected, sizeof(digest));
}
template<class Prefs> Status prefsStatus(fs::FS &fs, const char *path) {
  auto file = fs.open(path, "r");
  if (!file) return Status::Unreadable;
  if (file.size() < 2) return Status::Invalid;
  Prefs check;
  CheckedStream stream(file);
  const bool valid = check.loadSerial(stream);
  if (stream.failed) return Status::Unreadable;
  return valid && stream.firstRead == '{' && stream.lastRead == '}' ? Status::Valid : Status::Invalid;
}
inline Status channelsStatus(fs::FS &fs, const char *path, unsigned limit) {
  auto file = fs.open(path, "r");
  if (!file) return Status::Unreadable;
  constexpr size_t ChannelBytes = 68;
  if (file.size() % ChannelBytes || file.size() > limit * ChannelBytes) return Status::Invalid;
  uint8_t bytes[ChannelBytes];
  for (size_t remaining = file.size(); remaining; remaining -= sizeof(bytes))
    if (file.read(bytes, sizeof(bytes)) != sizeof(bytes)) return Status::Unreadable;
  return Status::Valid;
}
inline void previousPath(const char *path, char (&previous)[72]) {
  // The scoped ESP32 primary is already 30 characters; SPIFFS permits 31 plus NUL.
  if (!strncmp(path, "/bl/", 4)) snprintf(previous, sizeof(previous), "/bp/%s", path + 4);
  else snprintf(previous, sizeof(previous), "%s-prev", path);
}
template<class Inspect> bool publish(fs::FS &fs, const char *stage, const char *path, Inspect inspect) {
  if (inspect(stage) != Status::Valid) return false;
  char previous[72]; previousPath(path, previous);
  if (fs.exists(path)) {
    const Status status = inspect(path);
    if (status == Status::Unreadable) return false;
    if (status == Status::Valid) {
      if (fs.exists(previous) && !fs.remove(previous)) return false;
      if (!fs.rename(path, previous)) return false;
    } else if (!fs.remove(path)) return false;
  }
  // The prior valid file survives a reset or failure before this rename.
  return fs.rename(stage, path) && inspect(path) == Status::Valid;
}
template<class Prefs> bool savePrefs(fs::FS &fs, Prefs &prefs) {
  constexpr const char *stage = "/prefs-writing";
  auto file = fs.open(stage, "w");
  if (!file) return false;
  CheckedStream stream(file);
  const bool saved = prefs.saveSerial(stream);
  file.flush(); file.close();
  uint8_t digest[32]; stream.hash.finalize(digest, sizeof(digest));
  return saved && !stream.failed && matches(fs, stage, stream.written, digest) &&
      publish(fs, stage, "/prefs.json", [&](const char *path) { return prefsStatus<Prefs>(fs, path); });
}
inline uint8_t blobGet(fs::FS &fs, const char *path, uint8_t *bytes) {
  uint8_t size = blobRead(fs, path, bytes);
  if (size) return size;
  char previous[72]; previousPath(path, previous);
  return blobRead(fs, previous, bytes);
}
inline bool blobPut(fs::FS &fs, const char *path, const uint8_t *bytes, uint8_t size) {
  constexpr const char *stage = "/bl-writing";
  auto file = fs.open(stage, "w");
  bool ok = file && file.write(bytes, size) == size;
  if (file) { file.flush(); file.close(); }
  uint8_t check[255];
  ok = ok && blobRead(fs, stage, check) == size && !memcmp(bytes, check, size);
  if (ok) ok = publish(fs, stage, path, [&](const char *name) {
    uint8_t length;
    return blobStatus(fs, name, check, length);
  });
  if (!ok) fs.remove(stage);
  else {
    char previous[72]; previousPath(path, previous);
    if (fs.exists(previous) && !fs.remove(previous)) return false;
  }
  return ok;
}
inline void blobDelete(fs::FS &fs, const char *path) {
  fs.remove(path);
  char previous[72]; previousPath(path, previous); fs.remove(previous);
}
} // namespace companion_persistence
} // namespace onchip
