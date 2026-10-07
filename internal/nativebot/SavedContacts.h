// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <helpers/AdvertDataHelpers.h>
#include <Identity.h>
#include <Packet.h>
#include <cJSON.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace onchip {

// Public signed adverts are read from the existing Base authorities; no keys,
// routes, identity mirrors or peer records are written by this adapter.
class SavedContacts {
  struct Peer {
    std::array<uint8_t, 32> key{};
    std::array<uint8_t, 255> advert{};
    uint8_t size = 0;
    uint32_t timestamp = 0;
  };
  class File {
    int fd_;
  public:
    explicit File(int fd) : fd_(fd) {}
    ~File() { if (fd_ >= 0) close(fd_); }
    File(const File &) = delete;
    File &operator=(const File &) = delete;
    int get() const { return fd_; }
  };
  std::string root_;
  std::array<Peer, 16> peers_{};
  size_t count_ = 0;
  bool overflow_ = false;
  static constexpr size_t MaximumDocument = 16 * 1024 * 1024;

  static bool privateDirectory(int fd) {
    struct stat st{};
    return fd >= 0 && !fstat(fd, &st) && S_ISDIR(st.st_mode) &&
        st.st_uid == geteuid() && (st.st_mode & 07777) == 0700;
  }
  static cJSON *field(const cJSON *object, const char *name) {
    cJSON *found = nullptr;
    for (auto *item = object ? object->child : nullptr; item; item = item->next)
      if (item->string && !strcmp(item->string, name)) {
        if (found) return nullptr;
        found = item;
      }
    return found;
  }
  static bool one(const cJSON *value) {
    return cJSON_IsNumber(value) && value->valuedouble == 1;
  }
  static int base64Digit(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    return c == '+' ? 62 : c == '/' ? 63 : -1;
  }
  static size_t blob(const cJSON *value, uint8_t *out, size_t capacity) {
    if (cJSON_IsArray(value)) {
      size_t size = 0;
      for (auto *item = value->child; item; item = item->next) {
        if (size == capacity || !cJSON_IsNumber(item) || !std::isfinite(item->valuedouble) ||
            item->valuedouble < 0 || item->valuedouble > 255 ||
            item->valuedouble != std::floor(item->valuedouble)) return 0;
        out[size++] = static_cast<uint8_t>(item->valuedouble);
      }
      return size;
    }
    if (!cJSON_IsString(value) || !value->valuestring) return 0;
    const auto *text = reinterpret_cast<const unsigned char *>(value->valuestring);
    const size_t length = strlen(value->valuestring);
    if (!length || length % 4 || length > ((capacity + 2) / 3) * 4) return 0;
    size_t size = 0;
    for (size_t i = 0; i < length; i += 4) {
      const int a = base64Digit(text[i]), b = base64Digit(text[i + 1]);
      const int c = text[i + 2] == '=' ? 0 : base64Digit(text[i + 2]);
      const int d = text[i + 3] == '=' ? 0 : base64Digit(text[i + 3]);
      const unsigned padding = text[i + 2] == '=' ? 2 : text[i + 3] == '=' ? 1 : 0;
      if (a < 0 || b < 0 || c < 0 || d < 0 ||
          (padding && i + 4 != length) ||
          (padding == 2 && (text[i + 3] != '=' || (b & 15))) ||
          (padding == 1 && (c & 3)) || size + 3 - padding > capacity) return 0;
      const unsigned bits = (unsigned(a) << 18) | (unsigned(b) << 12) |
                            (unsigned(c) << 6) | unsigned(d);
      out[size++] = bits >> 16;
      if (padding < 2) out[size++] = bits >> 8;
      if (!padding) out[size++] = bits;
    }
    return size;
  }
  static void scrub(cJSON *value) {
    for (auto *item = value; item; item = item->next) {
      if (item->valuestring) explicit_bzero(item->valuestring, strlen(item->valuestring));
      item->valueint = 0;
      item->valuedouble = 0;
      if (item->child) scrub(item->child);
    }
  }
  static bool signedAdvert(Peer &peer) {
    mesh::Packet packet{};
    if (!peer.size || !packet.readFrom(peer.advert.data(), peer.size) ||
        packet.getPayloadVer() != PAYLOAD_VER_1 || packet.getPayloadType() != PAYLOAD_TYPE_ADVERT ||
        !mesh::Packet::isValidPathLen(packet.path_len) || packet.payload_len < 100 ||
        packet.payload_len > 100 + MAX_ADVERT_DATA_SIZE ||
        memcmp(packet.payload, peer.key.data(), peer.key.size())) return false;
    const size_t dataSize = packet.payload_len - 100;
    uint8_t message[36 + MAX_ADVERT_DATA_SIZE]{};
    memcpy(message, packet.payload, 36);
    memcpy(message + 36, packet.payload + 100, dataSize);
    mesh::Identity identity(peer.key.data());
    AdvertDataParser advert(packet.payload + 100, dataSize);
    if (!identity.verify(packet.payload + 36, message, 36 + dataSize) ||
        !advert.isValid() || !advert.hasName()) return false;
    peer.timestamp = uint32_t(packet.payload[32]) | (uint32_t(packet.payload[33]) << 8) |
                     (uint32_t(packet.payload[34]) << 16) | (uint32_t(packet.payload[35]) << 24);
    return true;
  }
  void include(const Peer &peer) {
    for (size_t i = 0; i < count_; ++i) {
      if (peers_[i].key == peer.key) {
        if (peer.timestamp > peers_[i].timestamp) peers_[i] = peer;
        return;
      }
    }
    if (count_ == peers_.size()) { overflow_ = true; return; }
    peers_[count_++] = peer;
  }
  bool document(int directory, const char *name, uint8_t hash, bool &absent) {
    File file(openat(directory, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    absent = file.get() < 0 && errno == ENOENT;
    if (absent) return true;
    struct stat st{};
    if (file.get() < 0 || fstat(file.get(), &st) || !S_ISREG(st.st_mode) ||
        st.st_uid != geteuid() || (st.st_mode & 07777) != 0600 || st.st_nlink != 1 ||
        st.st_size <= 0 || st.st_size > off_t(MaximumDocument)) return false;
    std::vector<char> data(size_t(st.st_size) + 1);
    size_t used = 0;
    while (used < size_t(st.st_size)) {
      const ssize_t count = read(file.get(), data.data() + used, size_t(st.st_size) - used);
      if (count > 0) { used += size_t(count); continue; }
      if (count < 0 && errno == EINTR) continue;
      explicit_bzero(data.data(), data.size());
      return false;
    }
    const bool terminated = !memchr(data.data(), 0, used);
    cJSON *parsed = terminated ? cJSON_ParseWithOpts(data.data(), nullptr, true) : nullptr;
    explicit_bzero(data.data(), data.size());
    if (!parsed) return false;
    cJSON *state = parsed;
    bool valid = cJSON_IsObject(parsed);
    if (!strcmp(name, "identity-state.json")) {
      state = field(parsed, "state");
      const auto *type = field(parsed, "document");
      valid = valid && one(field(parsed, "version")) && cJSON_IsString(type) &&
              !strcmp(type->valuestring, "companion.json") && cJSON_IsObject(state);
    }
    const auto *contacts = field(state, "Contacts");
    uint8_t authorityKey[32]{};
    valid = valid && one(field(state, "Version")) &&
        blob(field(state, "PublicKey"), authorityKey, sizeof(authorityKey)) == sizeof(authorityKey) &&
        (cJSON_IsArray(contacts) || cJSON_IsNull(contacts)) &&
        cJSON_GetArraySize(contacts) <= 350;
    if (valid) for (auto *item = contacts->child; item; item = item->next) {
      Peer peer;
      if (!cJSON_IsObject(item) ||
          blob(field(item, "PublicKey"), peer.key.data(), peer.key.size()) != peer.key.size() ||
          peer.key[0] != hash) continue;
      peer.size = static_cast<uint8_t>(blob(field(item, "Advert"), peer.advert.data(), peer.advert.size()));
      if (signedAdvert(peer)) include(peer);
    }
    scrub(parsed);
    cJSON_Delete(parsed);
    return valid;
  }
  bool authority(int common, const char *name, uint8_t hash) {
    File directory(openat(common, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (directory.get() < 0 && errno == ENOENT) return true;
    if (!privateDirectory(directory.get())) {
      std::fprintf(stderr, "Host bot: saved contacts unavailable: %s directory must be owned mode0700 without symlinks\n", name);
      return false;
    }
    bool absent = false;
    if (!document(directory.get(), "identity-state.json", hash, absent)) {
      std::fprintf(stderr, "Host bot: saved contacts unavailable: %s/identity-state.json invalid or not owned mode0600\n", name);
      return false;
    }
    // The envelope is the active authority; never fall back to its stale document.
    if (!absent || document(directory.get(), "companion.json", hash, absent)) return true;
    std::fprintf(stderr, "Host bot: saved contacts unavailable: %s/companion.json invalid or not owned mode0600\n", name);
    return false;
  }
  void refresh(uint8_t hash) {
    count_ = 0;
    overflow_ = false;
    File directory(open(root_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (!privateDirectory(directory.get())) {
      std::fputs("Host bot: saved contacts unavailable: shared state directory must be owned mode0700 without symlinks\n", stderr);
      return;
    }
    if (!authority(directory.get(), "base", hash) ||
        !authority(directory.get(), "secondary", hash)) {
      count_ = 0;
      return;
    }
    if (overflow_) {
      count_ = 0;
      std::fprintf(stderr, "Host bot: saved contacts unavailable: more than16 signed full keys match peer hash %02x\n", hash);
      return;
    }
    std::sort(peers_.begin(), peers_.begin() + count_,
              [](const Peer &a, const Peer &b) { return a.key < b.key; });
  }
public:
  void configure(const char *nativeRoot) {
    root_ = std::filesystem::path(nativeRoot).parent_path().parent_path().string();
  }
  bool lookup(const uint8_t *hash, unsigned &cursor, uint8_t *key,
              uint8_t *advert, uint8_t &size) {
    if (root_.empty() || !hash || !key || !advert) return false;
    if (!cursor) refresh(hash[0]);
    if (cursor >= count_) return false;
    const auto &peer = peers_[cursor++];
    memcpy(key, peer.key.data(), peer.key.size());
    memcpy(advert, peer.advert.data(), peer.size);
    size = peer.size;
    return true;
  }
};

} // namespace onchip
