// SPDX-License-Identifier: Apache-2.0
#pragma once
// HTTPS client for the shared-room service's /v1/info, /v1/node/redeem and
// /v1/aliases/{alias}/control calls (aspen-node.v1). Portable: the TLS stream is
// injected, so native tests script the server.
#include <stddef.h>
#include <stdint.h>

namespace onchip {
enum class CloudTlsFailure : uint8_t { None, Clock, Verify, Memory, Network };
struct CloudTlsTarget {
  const char *address, *host, *ca;
  bool systemRoots;
};
// One TLS stream. `read`/`write` return 0 when they would block and a negative
// number when the connection failed or closed. All calls run on the network task.
class CloudTransport {
public:
  virtual ~CloudTransport() = default;
  virtual bool open(const CloudTlsTarget &target, CloudTlsFailure &failure) = 0;
  virtual int write(const uint8_t *data, size_t size) = 0;
  virtual int read(uint8_t *data, size_t size) = 0;
  virtual void close() = 0;
  virtual uint32_t now() const = 0;
  virtual void idle() = 0;
};

namespace cloudnode {
constexpr char Protocol[] = "aspen-node.v1";
constexpr size_t RedeemLimit = 2048, ControlLimit = 1024, ResponseLimit = 1536;
// Headers plus the largest body; the caller owns the buffer.
constexpr size_t ExchangeBuffer = 2560;

// Appends to a fixed buffer and remembers an overflow.
class Writer {
  char *out_;
  size_t capacity_, size_ = 0;
  bool good_ = true;
public:
  Writer(char *out, size_t capacity) : out_(out), capacity_(capacity) { if (capacity) out[0] = 0; }
  void raw(const char *text, size_t length);
  void text(const char *text);
  void quoted(const char *text);            // JSON string with escapes
  void hex(const uint8_t *bytes, size_t size);
  void number(uint32_t value);
  bool good() const { return good_; }
  size_t size() const { return size_; }
};

enum class Net : uint8_t { Ok, Clock, Verify, Memory, Dns, Network, Malformed };
struct Reply {
  uint16_t status = 0;
  uint32_t retryAfter = 0;      // seconds, from Retry-After
  const char *body = nullptr;   // NUL-terminated, inside the exchange buffer
  size_t size = 0;
};
// One request on a fresh connection. `body` may be null for GET. Returns Ok only when a
// complete HTTP response arrived; the status is then in `reply`. Redirects are not followed.
Net exchange(CloudTransport &transport, const CloudTlsTarget &target, const char *method,
             const char *path, const char *bearer, const char *body, size_t bodySize,
             char *buffer, size_t bufferSize, Reply &reply, uint32_t timeoutMs);

// Code hash for the signed message: lowercase hex of SHA-256("aspen-node.v1.code\n" + code).
void codeHash(const char canonical[16], char out[65]);
// Lowercase hex of SHA-256(text), first `bytes` bytes.
void sha256Hex(const char *text, size_t length, size_t bytes, char *out);
// Builds the signed text (2.2) and signs it with the management identity.
// `subject` is "-" (join), the first 16 hex of SHA-256(name) (create) or the room public key (convert).
bool signRedeem(const char *host, const char *op, const char *codeOrOpen, const char *subject,
                uint32_t ts, char nodeHex[65], char sigHex[129]);
} // namespace cloudnode
} // namespace onchip
