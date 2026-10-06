// SPDX-License-Identifier: Apache-2.0
#pragma once
#if MESHCORE_NODE_BACKUP
#include "NodeBackup.h"
#include <SPIFFS.h>

namespace onchip {
class FirmwareNodeBackup : public NodeBackupPlatform {
public:
  bool beginOutput(char *error, size_t size) override;
  bool publish(const uint8_t digest[32], char *error, size_t size) override;
  bool remove(bool published) override;
  bool size(uint32_t &bytes, uint8_t digest[32]) override;
  size_t read(uint32_t offset, uint8_t *bytes, size_t size) override;
  bool write(const uint8_t *bytes, size_t size) override;
  bool seal(uint8_t ephemeral[32], uint8_t nonce[16], const uint8_t recipient[32],
            uint8_t shared[32]) override;
  uint32_t now() const override;
protected:
  bool manifest(backup::TarWriter &archive, const char *product, const char *version);
  bool file(backup::TarWriter &archive, const char *path, const char *name,
            SHA256 *digest, char *error, size_t size);
  static bool included(const char *path);
private:
  struct Record {
    uint8_t magic[4]{'N','B','P',1}, slot = 0, reserved[3]{};
    uint32_t bytes = 0;
    uint8_t digest[32]{};
  } record_;
  static_assert(sizeof(Record) == 44, "Backup pointer layout changed");
  File output_;
  uint8_t selected_ = 0;
  uint32_t written_ = 0, budget_ = 0;
  static const char *path(uint8_t slot);
  bool pointer(Record &record, bool &present);
};
} // namespace onchip
#endif
