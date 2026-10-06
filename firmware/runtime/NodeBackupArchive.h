// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <AES.h>
#include <SHA256.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdint.h>

namespace onchip::backup {
constexpr uint32_t RawLimit = 2 * 1024 * 1024;
constexpr uint32_t EntryLimit = 512;
constexpr size_t HeaderBytes = 88, MacBytes = 32;
constexpr char EncryptionDomain[] = "meshcore-backup-v1 encryption";
constexpr char AuthenticationDomain[] = "meshcore-backup-v1 authentication";

inline void wipe(void *memory, size_t size) {
  auto *bytes = static_cast<volatile uint8_t *>(memory);
  while (size--) *bytes++ = 0;
}

class Sink {
public:
  virtual ~Sink() = default;
  virtual bool write(const uint8_t *bytes, size_t size) = 0;
};
class Reader {
public:
  virtual ~Reader() = default;
  virtual size_t read(uint8_t *bytes, size_t size) = 0;
};

class SealedSink final : public Sink {
  Sink &output_;
  AES128 aes_;
  SHA256 mac_;
  uint8_t macKey_[32]{}, counter_[16]{}, stream_[16]{};
  unsigned streamOffset_ = 16;
  bool active_ = false, started_ = false;
public:
  explicit SealedSink(Sink &output) : output_(output) {}
  ~SealedSink() {
    aes_.clear(); mac_.clear();
    wipe(macKey_, sizeof(macKey_)); wipe(counter_, sizeof(counter_)); wipe(stream_, sizeof(stream_));
  }
  bool begin(const uint8_t ephemeral[32], const uint8_t recipient[32],
             const uint8_t nonce[16], const uint8_t shared[32]) {
    if (started_ || !ephemeral || !recipient || !nonce || !shared) return false;
    started_ = true;
    SHA256 hash;
    uint8_t key[32], header[HeaderBytes]{'M','C','B',1, 1,0,0,0};
    hash.reset(); hash.update(EncryptionDomain, sizeof(EncryptionDomain) - 1);
    hash.update(shared, 32); hash.finalize(key, sizeof(key));
    const bool keyed = aes_.setKey(key, 16);
    wipe(key, sizeof(key));
    hash.reset(); hash.update(AuthenticationDomain, sizeof(AuthenticationDomain) - 1);
    hash.update(shared, 32); hash.finalize(macKey_, sizeof(macKey_));
    memcpy(header + 8, ephemeral, 32); memcpy(header + 40, recipient, 32);
    memcpy(header + 72, nonce, 16); memcpy(counter_, nonce, 16);
    mac_.resetHMAC(macKey_, sizeof(macKey_)); mac_.update(header, sizeof(header));
    active_ = keyed && output_.write(header, sizeof(header));
    return active_;
  }
  bool write(const uint8_t *bytes, size_t size) override {
    if (!active_ || (!bytes && size)) return false;
    uint8_t ciphertext[128];
    while (size) {
      const size_t count = std::min(size, sizeof(ciphertext));
      for (size_t i = 0; i < count; ++i) {
        if (streamOffset_ == sizeof(stream_)) {
          aes_.encryptBlock(stream_, counter_);
          for (int j = 15; j >= 0 && ++counter_[j] == 0; --j) {}
          streamOffset_ = 0;
        }
        ciphertext[i] = bytes[i] ^ stream_[streamOffset_++];
      }
      mac_.update(ciphertext, count);
      if (!output_.write(ciphertext, count)) { active_ = false; return false; }
      bytes += count; size -= count;
    }
    return true;
  }
  bool finish() {
    if (!active_) return false;
    uint8_t tag[MacBytes];
    mac_.finalizeHMAC(macKey_, sizeof(macKey_), tag, sizeof(tag));
    active_ = false;
    return output_.write(tag, sizeof(tag));
  }
};

class TarWriter {
  Sink &output_;
  uint32_t rawBytes_ = 0, entries_ = 0;
  bool failed_ = false, finished_ = false;
  bool emit(const uint8_t *bytes, size_t size) {
    if (failed_ || finished_ || size > RawLimit - rawBytes_) return false;
    rawBytes_ += size;
    while (size) {
      const size_t count = std::min(size, size_t(128));
      uint8_t packet[129];
      size_t offset = 0;
      while (offset < count) {
        size_t run = 1;
        while (offset + run < count && bytes[offset + run] == bytes[offset]) ++run;
        size_t packetSize;
        if (run >= 3) {
          packet[0] = 0x80 | uint8_t(run - 1); packet[1] = bytes[offset];
          packetSize = 2; offset += run;
        } else {
          const size_t first = offset;
          offset += run;
          while (offset < count) {
            run = 1;
            while (offset + run < count && bytes[offset + run] == bytes[offset]) ++run;
            if (run >= 3) break;
            offset += run;
          }
          packet[0] = uint8_t(offset - first - 1);
          memcpy(packet + 1, bytes + first, offset - first);
          packetSize = offset - first + 1;
        }
        if (!output_.write(packet, packetSize)) { failed_ = true; return false; }
      }
      bytes += count; size -= count;
    }
    return true;
  }
public:
  explicit TarWriter(Sink &output) : output_(output) {}
  static bool validName(const char *name) {
    if (!name || !*name || *name == '/' || strlen(name) > 99) return false;
    const char *segment = name;
    for (const char *p = name;; ++p) {
      if (!*p || *p == '/') {
        const size_t size = p - segment;
        if (!size || (size == 1 && *segment == '.') ||
            (size == 2 && segment[0] == '.' && segment[1] == '.')) return false;
        if (!*p) return true;
        segment = p + 1;
      } else if (*p < 32 || *p > 126 || *p == '\\') return false;
    }
  }
  bool add(const char *name, uint32_t size, Reader &input) {
    if (failed_ || finished_ || !validName(name) || entries_ >= EntryLimit ||
        size > RawLimit - 1536) return false;
    const uint32_t padded = (size + 511) / 512 * 512;
    if (rawBytes_ > RawLimit - 1536 - padded) return false;
    uint8_t block[512]{};
    memcpy(block, name, strlen(name));
    snprintf(reinterpret_cast<char *>(block + 100), 8, "%07o", 0600);
    snprintf(reinterpret_cast<char *>(block + 108), 8, "%07o", 0);
    snprintf(reinterpret_cast<char *>(block + 116), 8, "%07o", 0);
    snprintf(reinterpret_cast<char *>(block + 124), 12, "%011lo", static_cast<unsigned long>(size));
    snprintf(reinterpret_cast<char *>(block + 136), 12, "%011o", 0);
    memset(block + 148, ' ', 8); block[156] = '0';
    memcpy(block + 257, "ustar", 5); memcpy(block + 263, "00", 2);
    unsigned sum = 0;
    for (auto byte : block) sum += byte;
    snprintf(reinterpret_cast<char *>(block + 148), 7, "%06o", sum); block[155] = ' ';
    if (!emit(block, sizeof(block))) return false;
    for (uint32_t offset = 0; offset < size;) {
      const size_t count = std::min(size_t(size - offset), sizeof(block));
      if (input.read(block, count) != count) { failed_ = true; return false; }
      if (!emit(block, count)) return false;
      offset += count;
    }
    wipe(block, sizeof(block));
    if (size % sizeof(block) && !emit(block, sizeof(block) - size % sizeof(block))) return false;
    ++entries_;
    return true;
  }
  bool finish() {
    uint8_t zeros[512]{};
    if (!emit(zeros, sizeof(zeros)) || !emit(zeros, sizeof(zeros))) return false;
    finished_ = true;
    return true;
  }
  uint32_t rawBytes() const { return rawBytes_; }
  uint32_t entries() const { return entries_; }
};
} // namespace onchip::backup
