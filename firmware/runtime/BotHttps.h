// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "BotTypes.h"
#include "BotHttpsDiagnostics.h"
#include <atomic>

#ifndef ONCHIP_BOT_HTTPS
#define ONCHIP_BOT_HTTPS 0
#endif
#ifndef ONCHIP_TLS_PSRAM
#define ONCHIP_TLS_PSRAM 1
#endif

namespace onchip {
struct BotNetworkRoute;
constexpr uint32_t BotHttpsDeadlineMs = 15000;
constexpr size_t BotHttpsBodyLimit = 2048;
constexpr size_t BotHttpsRadioReserveBytes = 32 * 1024;
constexpr size_t BotHttpsWorkBudgetBytes = 68 * 1024;
constexpr size_t BotHttpsClientBudgetBytes = 8 * 1024;
struct BotHttpsConfig {
  enum : uint8_t { Health = 1, Echo = 2, Weather = 4 };
  const char *address, *host, *ca, *token;
  uint16_t port;
  uint8_t operations;
  constexpr BotHttpsConfig(const char *address = "", const char *host = "", const char *ca = "",
                          const char *token = "", uint16_t port = 443, uint8_t operations = 0)
      : address(address), host(host), ca(ca), token(token), port(port), operations(operations) {}
  bool valid() const;
  bool validPeer() const;
};
const BotHttpsConfig &botHttpsConfig();
bool botHttpsConfigured();
bool botHttpsClockTrusted();
// Authenticated mast-admin entry; command excludes "bot https ".
// status | discard | commit | endpoint NAME IPV4 HOST PORT PATH get|post|both
// ca|token NAME HEX (up to 128 hex characters per append)
// rpc SERVICE OPERATION PATH | drop NAME | unmap SERVICE OPERATION | retain home
bool botHttpsAdmin(const char *command, char *reply, size_t capacity);

class BotHttpsTransport {
public:
  enum class Failure : uint8_t { Transport, Heap };
  virtual ~BotHttpsTransport() = default;
  virtual bool online() const { return true; }
  virtual uint64_t requestNonce() const { return 0; }
  virtual bool open(const BotHttpsConfig &config, char *error, size_t capacity) = 0;
  virtual bool validate(char *error, size_t capacity) = 0;
  virtual Failure failure() const { return Failure::Transport; }
  virtual void resetDiagnostics() {}
  virtual BotHttpsDiagnostics diagnostics() const { return {}; }
  // Zero on read/write means would block; the worker checks cancellation before retrying.
  virtual int write(const uint8_t *data, size_t size) = 0;
  // Zero means not yet readable; negative means the connection failed/closed.
  virtual int read(uint8_t *data, size_t size) = 0;
  virtual void close() = 0;
  virtual uint32_t now() const = 0;
  virtual void idle() = 0;
};
BotHttpsTransport *createBotHttpsTransport();
bool beginBotHttpsMemory();

class BotHttpsBodySink {
public:
  virtual ~BotHttpsBodySink() = default;
  virtual bool begin(uint32_t contentLength) = 0;
  virtual bool write(const uint8_t *bytes, size_t size) = 0;
  virtual bool commit(uint32_t bytes, const uint8_t sha256[32]) = 0;
  virtual void abort(const char *reason) = 0;
};
struct BotHttpsFetchRequest {
  uint32_t id = 0;
  uint8_t expectedSha256[32]{};
  uint32_t maxBytes = BotSourceLimit;
  BotHttpsBodySink *sink = nullptr;
};
struct BotHttpsFetchResult {
  enum State : uint8_t { Queued, Running, Complete, Failed, Unknown } state = Queued;
  uint32_t id = 0, bytes = 0;
  uint16_t httpStatus = 0;
  uint8_t sha256[32]{};
  bool networkSubmitted = false;
  char rpcCode[40]{}, error[96]{};
};

class BotHttps {
public:
  BotHttps(const BotHttpsConfig &config, BotHttpsTransport &transport)
      : config_(config), transport_(transport) {}
  ~BotHttps();
  void perform(const BotIoRequest &request, BotIoResult &result,
               const std::atomic<uint32_t> &generation, const std::atomic<uint32_t> &grant,
               const std::atomic<bool> &enabled, const std::atomic<bool> &stopping,
               const BotNetworkRoute *route = nullptr,
               const BotHttpsFetchRequest *fetch = nullptr,
               BotHttpsFetchResult *fetchResult = nullptr);
  static bool decode(const BotIoRequest &request, const char *body, size_t size,
                     BotIoResult &result);
private:
  struct Workspace;
  Workspace *workspace_ = nullptr;
  BotHttpsConfig config_;
  BotHttpsTransport &transport_;
  struct Rate {
    bool used = false;
    uint8_t principal[32]{}, count = 0;
    uint32_t at = 0;
  } rates_[4];
  uint32_t window_ = 0;
  uint8_t count_ = 0;
  bool rate(const uint8_t principal[32]);
};
} // namespace onchip
