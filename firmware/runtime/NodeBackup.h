// SPDX-License-Identifier: Apache-2.0
#pragma once
#if MESHCORE_NODE_BACKUP
#include "NodeBackupArchive.h"
#include <atomic>

namespace onchip {
class NodeBackupPlatform : public backup::Sink {
public:
  virtual bool available() const = 0;
  virtual void wake() = 0;
  virtual bool beginOutput(char *error, size_t size) = 0;
  virtual bool publish(const uint8_t digest[32], char *error, size_t size) = 0;
  virtual bool remove(bool published) = 0;
  virtual bool size(uint32_t &bytes, uint8_t digest[32]) = 0;
  virtual size_t read(uint32_t offset, uint8_t *bytes, size_t size) = 0;
  virtual bool snapshot(backup::TarWriter &archive, char *error, size_t size) = 0;
  virtual bool seal(uint8_t ephemeral[32], uint8_t nonce[16], const uint8_t recipient[32],
                    uint8_t shared[32]) = 0;
  virtual uint32_t now() const = 0;
};

class NodeBackup {
public:
  static constexpr uint32_t RfPacingMs = 5000;
  static constexpr uint32_t FileLimit = backup::RawLimit + (backup::RawLimit + 127) / 128 +
                                       backup::HeaderBytes + backup::MacBytes;
  void begin(NodeBackupPlatform &platform) { platform_ = &platform; }
  void command(const char *text, char *reply, size_t size, bool radio);
  void work();
  bool beginRead(const char *id);
  size_t read(uint32_t offset, uint8_t *bytes, size_t size);
  void endRead();
  uint32_t bytes() const { return bytes_; }
  bool cancelled() const { return cancel_.load(); }
  bool active() const;
private:
  enum State { Empty, Reserving, PendingExport, PendingLoad, Working, Ready, Reading, Failed };
  std::atomic<State> state_{Empty};
  std::atomic<bool> cancel_{false};
  NodeBackupPlatform *platform_ = nullptr;
  uint8_t recipient_[32]{};
  char id_[17]{}, hash_[65]{}, error_[120]{};
  uint32_t bytes_ = 0, lastRfRead_ = 0;
  bool haveRfRead_ = false;
  bool describe();
  void fail(const char *reason);
};
NodeBackup &nodeBackup();
} // namespace onchip
#endif
