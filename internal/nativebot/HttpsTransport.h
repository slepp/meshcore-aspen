// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "BotHttps.h"
#include <openssl/ssl.h>

namespace onchip {
class NativeBotHttpsTransport final : public BotHttpsTransport {
public:
  NativeBotHttpsTransport();
  ~NativeBotHttpsTransport() override { close(); }
  bool open(const BotHttpsConfig &, char *, size_t) override;
  bool validate(char *, size_t) override;
  int write(const uint8_t *, size_t) override;
  int read(uint8_t *, size_t) override;
  void close() override;
  uint32_t now() const override;
  void idle() override;
  unsigned transmitted = 0;
  uint64_t requestNonce() const override { return nonce_; }
private:
  uint64_t nonce_ = 0;
  SSL_CTX *context_ = nullptr;
  SSL *ssl_ = nullptr;
  int socket_ = -1;
};
bool nativeBotHttpsClockTrusted();
}
