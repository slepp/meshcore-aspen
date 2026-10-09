// SPDX-License-Identifier: Apache-2.0
#if MESHCORE_NODE_BACKUP
#include "NodeBackup.h"
#include <cstdlib>

namespace onchip {
NodeBackup &nodeBackup() { static NodeBackup service; return service; }
namespace {
bool key(const char *text, uint8_t bytes[32]) {
  if (!text || strlen(text) != 64) return false;
  bool nonzero = false;
  for (unsigned i = 0; i < 32; ++i) {
    const auto digit = [](char c) { return c >= '0' && c <= '9' ? c - '0' :
        c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
    const int high = digit(text[2 * i]), low = digit(text[2 * i + 1]);
    if (high < 0 || low < 0) return false;
    bytes[i] = high * 16 + low; nonzero |= bytes[i] != 0;
  }
  return nonzero;
}
class Output final : public backup::Sink {
  NodeBackupPlatform &platform_;
  NodeBackup &service_;
  uint32_t size_ = 0;
  SHA256 hash_;
public:
  Output(NodeBackupPlatform &platform, NodeBackup &service) : platform_(platform), service_(service) { hash_.reset(); }
  bool write(const uint8_t *bytes, size_t size) override {
    if (service_.cancelled() || size > NodeBackup::FileLimit - size_ ||
        !platform_.write(bytes, size)) return false;
    hash_.update(bytes, size); size_ += size; return true;
  }
  void digest(uint8_t bytes[32]) { hash_.finalize(bytes, 32); }
};
}
bool NodeBackup::active() const {
  const auto state = state_.load();
  return state == Reserving || state == PendingExport || state == PendingLoad || state == Working;
}
void NodeBackup::fail(const char *reason) {
  if (reason != error_) snprintf(error_, sizeof(error_), "%s", reason);
  state_ = Failed;
}
bool NodeBackup::describe() {
  uint8_t expected[32];
  if (!platform_->size(bytes_, expected) || bytes_ < backup::HeaderBytes + backup::MacBytes || bytes_ > FileLimit) {
    snprintf(error_, sizeof(error_), "Saved backup missing or has invalid size"); return false;
  }
  SHA256 hash;
  hash.reset();
  uint8_t buffer[512];
  for (uint32_t offset = 0; offset < bytes_;) {
    const size_t count = std::min(size_t(bytes_ - offset), sizeof(buffer));
    if (cancel_ || platform_->read(offset, buffer, count) != count) {
      snprintf(error_, sizeof(error_), "Saved backup read failed or cancelled"); return false;
    }
    if (!offset && memcmp(buffer, "MCB\1\1\0\0\0", 8)) {
      snprintf(error_, sizeof(error_), "Saved backup header is invalid"); return false;
    }
    hash.update(buffer, count); offset += count;
  }
  uint8_t digest[32];
  hash.finalize(digest, sizeof(digest));
  if (memcmp(digest, expected, sizeof(digest))) {
    snprintf(error_, sizeof(error_), "Saved backup checksum failed"); return false;
  }
  for (unsigned i = 0; i < sizeof(digest); ++i) snprintf(hash_ + 2 * i, 3, "%02x", digest[i]);
  memcpy(id_, hash_, 16); id_[16] = 0;
  haveRfRead_ = false;
  return true;
}
void NodeBackup::work() {
  auto selected = state_.load();
  if ((selected != PendingExport && selected != PendingLoad) ||
      !state_.compare_exchange_strong(selected, Working)) return;
  error_[0] = 0;
  bool ok = true;
  if (selected == PendingExport) {
    uint8_t ephemeral[32]{}, nonce[16]{}, shared[32]{};
    ok = platform_->seal(ephemeral, nonce, recipient_, shared) &&
         platform_->beginOutput(error_, sizeof(error_));
    if (ok) {
      Output output(*platform_, *this);
      backup::SealedSink sealed(output);
      backup::TarWriter archive(sealed);
      ok = sealed.begin(ephemeral, recipient_, nonce, shared) &&
           platform_->snapshot(archive, error_, sizeof(error_)) &&
           archive.finish() && sealed.finish() && !cancel_;
      if (ok) {
        uint8_t digest[32];
        output.digest(digest);
        ok = platform_->publish(digest, error_, sizeof(error_));
      }
    }
    backup::wipe(shared, sizeof(shared));
    backup::wipe(recipient_, sizeof(recipient_));
    if (!ok && !platform_->remove(false)) {
      snprintf(error_, sizeof(error_), "Backup failed; temporary file cleanup failed");
    }
  }
  if (!ok && !error_[0]) {
    const auto *reason = platform_->lastError();
    if (reason) snprintf(error_, sizeof(error_), "%s", reason);
  }
  if (ok && !cancel_) ok = describe();
  if (cancel_) fail("Backup cancelled; previously published file may remain");
  else if (!ok) fail(error_[0] ? error_ : "Backup storage, compression or encryption failed");
  else state_ = Ready;
}
bool NodeBackup::beginRead(const char *id) {
  State expected = Ready;
  if (!id || !state_.compare_exchange_strong(expected, Reading)) return false;
  if (strcmp(id, id_)) { endRead(); return false; }
  return true;
}
size_t NodeBackup::read(uint32_t offset, uint8_t *bytes, size_t size) {
  if (state_ != Reading || !bytes || offset > bytes_ || size > bytes_ - offset) return 0;
  return platform_->read(offset, bytes, size);
}
void NodeBackup::endRead() {
  State expected = Reading;
  state_.compare_exchange_strong(expected, Ready);
}
void NodeBackup::command(const char *text, char *reply, size_t capacity, bool radio) {
  const auto say = [&](const char *message) { snprintf(reply, capacity, "%s", message); };
  if (!text || !*text || !strcmp(text, "help")) {
    say("backup start KEY64; start-ram KEY64; status; load; load-ram; read64 ID16 OFFSET; read ID16 OFFSET; cancel; clear. RAM lost on restart; retain operator seed.");
    return;
  }
  if (!platform_ || !platform_->available()) { say("Error: node backup storage worker unavailable"); return; }
  auto current = state_.load();
  if (!strcmp(text, "load") || !strcmp(text, "load-ram")) {
    if (active() || current == Reading || !state_.compare_exchange_strong(current, Reserving)) {
      say("Error: backup worker or download busy; inspect backup status"); return;
    }
    if (!platform_->selectStorage(!strcmp(text, "load-ram"), true)) {
      state_ = current;
      say("Error: volatile backup unavailable; it is lost on restart"); return;
    }
    cancel_ = false; state_ = PendingLoad; platform_->wake();
    say("PREPARING saved backup; check backup status"); return;
  }
  if (!strcmp(text, "status")) {
    if (current == Empty) {
      if (state_.compare_exchange_strong(current, Reserving)) {
        cancel_ = false; state_ = PendingLoad; platform_->wake();
      }
      say("PREPARING saved backup; check backup status"); return;
    }
    if (current == Ready || current == Reading) {
      snprintf(reply, capacity, "READY %s bytes=%lu sha=%s", id_, static_cast<unsigned long>(bytes_), hash_);
    } else if (current == Failed) snprintf(reply, capacity, "Error: %s", error_);
    else say("PREPARING; check backup status");
    return;
  }
  if (!strncmp(text, "start ", 6) || !strncmp(text, "start-ram ", 10)) {
    const bool transient = !strncmp(text, "start-ram ", 10);
    uint8_t recipient[32];
    if (!key(text + (transient ? 10 : 6), recipient)) { say("Error: backup start requires a lowercase operator KEY64"); return; }
    if (active() || current == Reading || !state_.compare_exchange_strong(current, Reserving)) {
      say("Error: backup worker or download busy; inspect backup status"); return;
    }
    if (!platform_->selectStorage(transient, false)) {
      state_ = current;
      say("Error: volatile backup storage unavailable on this platform"); return;
    }
    memcpy(recipient_, recipient, sizeof(recipient_));
    cancel_ = false; state_ = PendingExport; platform_->wake();
    say(transient ? "PREPARING volatile encrypted backup; download before restart" :
        "PREPARING encrypted node backup; check backup status"); return;
  }
  if (!strcmp(text, "cancel")) {
    if (!active()) { say("Error: no backup preparation active"); return; }
    cancel_ = true; platform_->wake();
    say("Cancellation requested; inspect backup status"); return;
  }
  if (!strcmp(text, "clear")) {
    if (active() || current == Reading || !state_.compare_exchange_strong(current, Reserving)) {
      say("Error: backup worker or download busy; inspect backup status"); return;
    }
    if (!platform_->remove(true)) { fail("Backup file removal failed"); say("Error: backup file removal failed"); return; }
    bytes_ = 0; id_[0] = hash_[0] = 0; state_ = Empty;
    say("Saved backup removed; device settings unchanged"); return;
  }
  const bool base64 = !strncmp(text, "read64 ", 7);
  if (base64 || !strncmp(text, "read ", 5)) {
    char id[17]{}, offsetText[11]{}, extra;
    if (sscanf(text + (base64 ? 7 : 5), "%16s %10s %c", id, offsetText, &extra) != 2 ||
        !*offsetText || strspn(offsetText, "0123456789") != strlen(offsetText)) {
      say("Error: backup read requires ID16 and decimal byte offset"); return;
    }
    const unsigned long long number = strtoull(offsetText, nullptr, 10);
    if (number > UINT32_MAX || number > bytes_) { say("Error: backup byte offset is out of range"); return; }
    if (current != Ready || strcmp(id, id_)) { say("Error: backup ID changed or is not ready; inspect backup status"); return; }
    const uint32_t now = platform_->now();
    if (radio && haveRfRead_ && uint32_t(now - lastRfRead_) < RfPacingMs) {
      snprintf(reply, capacity, "WAIT ms=%lu", static_cast<unsigned long>(RfPacingMs - uint32_t(now - lastRfRead_))); return;
    }
    const uint32_t offset = number;
    capacity = std::min(capacity, size_t(163));
    const int header = snprintf(reply, capacity, "%s %s %lu ", base64 ? "CHUNK64" : "CHUNK",
                                id_, static_cast<unsigned long>(offset));
    if (header < 0 || size_t(header) + (base64 ? 2 : 96) >= capacity) {
      say("Error: backup reply capacity is too small"); return;
    }
    uint8_t bytes[122];
    const size_t limit = base64 ? (capacity - 1 - size_t(header)) * 3 / 4 : 48;
    const size_t count = std::min(size_t(bytes_ - offset), std::min(limit, sizeof(bytes)));
    if (!beginRead(id)) { say("Error: backup download busy"); return; }
    const bool ok = !count || read(offset, bytes, count) == count;
    endRead();
    if (!ok) { say("Error: backup file read failed"); return; }
    if (!count) { say("EOF"); return; }
    if (radio) { lastRfRead_ = now; haveRfRead_ = true; }
    if (base64) {
      static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
      size_t output = size_t(header);
      uint32_t bits = 0;
      unsigned pending = 0;
      for (size_t i = 0; i < count; ++i) {
        bits = (bits << 8) | bytes[i];
        pending += 8;
        while (pending >= 6) {
          pending -= 6;
          reply[output++] = alphabet[(bits >> pending) & 63];
        }
      }
      if (pending) reply[output++] = alphabet[(bits << (6 - pending)) & 63];
      reply[output] = 0;
    } else {
      for (size_t i = 0; i < count; ++i) snprintf(reply + header + 2 * i, 3, "%02x", bytes[i]);
    }
    return;
  }
  say("Error: unknown backup command; use backup help");
}
} // namespace onchip
#endif
