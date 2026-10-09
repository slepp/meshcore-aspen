// SPDX-License-Identifier: Apache-2.0
#include "TelemetryEndpoint.h"
#if ONCHIP_BOT_HTTPS
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <strings.h>

namespace onchip {
void performTelemetryPost(BotHttpsTransport &transport, const TelemetryEndpoint &endpoint,
                          const char *body, size_t size, uint32_t deadline,
                          const std::atomic<bool> &cancelled, const std::atomic<bool> &stopping,
                          TelemetryCompletion &result) {
  result = {};
  transport.resetDiagnostics();
  struct Close {
    BotHttpsTransport &transport;
    TelemetryCompletion &result;
    ~Close() { transport.close(); result.tls = transport.diagnostics(); }
  } close{transport, result};
  char error[128]{};
  const auto alive = [&]() {
    if (cancelled || stopping) { result.error = TelemetryError::Cancelled; return false; }
    if (int32_t(transport.now() - deadline) >= 0) { result.error = TelemetryError::Timeout; return false; }
    if (!transport.validate(error, sizeof(error))) {
      result.error = transport.failure() == BotHttpsTransport::Failure::Heap ?
                     TelemetryError::Heap : TelemetryError::Transport;
      return false;
    }
    return true;
  };
  if (!alive()) return;
  if (!endpoint.valid() || !size || size > TelemetryBodyLimit ||
      uint32_t(deadline - transport.now()) > BotHttpsDeadlineMs) {
    result.error = TelemetryError::Endpoint; return;
  }
  if (!transport.open(endpoint.https(), error, sizeof(error))) {
    result.error = transport.failure() == BotHttpsTransport::Failure::Heap ?
                   TelemetryError::Heap : TelemetryError::Transport;
    return;
  }
  if (!alive()) return;
  char header[1024];
  const int length = snprintf(header, sizeof(header),
      "POST %s HTTP/1.1\r\nHost: %s:%u\r\n%s%s%s"
      "Content-Type: text/plain\r\nAccept-Encoding: identity\r\nConnection: close\r\nContent-Length: %u\r\n\r\n",
      endpoint.path, endpoint.host, endpoint.port,
      endpoint.token[0] ? "Authorization: Bearer " : "", endpoint.token,
      endpoint.token[0] ? "\r\n" : "", unsigned(size));
  if (length <= 0 || size_t(length) >= sizeof(header)) { result.error = TelemetryError::Encoding; return; }
  const auto write = [&](const char *data, size_t bytes) {
    size_t offset = 0;
    while (offset < bytes && alive()) {
      // A sample fits one TLS record. Avoid twelve small TCP_NODELAY records
      // competing with the radio's internal heap while their ACKs are pending.
      const size_t remaining = bytes - offset;
      const int count = transport.write(reinterpret_cast<const uint8_t *>(data + offset), remaining);
      if (count < 0 || size_t(count) > remaining) return false;
      if (!count) { transport.idle(); continue; }
      offset += size_t(count);
    }
    return offset == bytes;
  };
  if (!write(header, size_t(length)) || !write(body, size)) return;
  char line[512];
  size_t total = 0;
  const auto readLine = [&]() {
    size_t n = 0;
    while (n + 1 < sizeof(line) && ++total <= 2048 && alive()) {
      const int count = transport.read(reinterpret_cast<uint8_t *>(line + n), 1);
      if (count < 0 || count > 1) return false;
      if (!count) { --total; transport.idle(); continue; }
      if (line[n++] == '\n') {
        if (n < 2 || line[n - 2] != '\r') return false;
        line[n - 2] = 0;
        for (size_t i = 0; i + 2 < n; ++i)
          if (uint8_t(line[i]) < 32 && line[i] != '\t') return false;
        return true;
      }
    }
    return false;
  };
  if (!readLine() || strlen(line) < 12 ||
      (strncmp(line, "HTTP/1.1 ", 9) && strncmp(line, "HTTP/1.0 ", 9)) ||
      line[9] < '2' || line[9] > '5' || line[10] < '0' || line[10] > '9' ||
      line[11] < '0' || line[11] > '9' || (line[12] && line[12] != ' ')) return;
  const uint16_t status = (line[9] - '0') * 100 + (line[10] - '0') * 10 + line[11] - '0';
  bool hasLength = false;
  while (readLine()) {
    if (!line[0]) {
      if (!alive()) return;
      result.httpStatus = status;
      result.ok = status >= 200 && status < 300;
      result.error = result.ok ? TelemetryError::None : TelemetryError::Http;
      return;
    }
    char *colon = strchr(line, ':');
    if (!colon || colon == line) return;
    *colon++ = 0;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(line); *p; ++p)
      if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') || strchr("!#$%&'*+-.^_`|~", *p))) return;
    if (!strcasecmp(line, "content-length")) {
      if (hasLength) return;
      hasLength = true;
      while (*colon == ' ' || *colon == '\t') ++colon;
      if (!*colon) return;
      for (; *colon; ++colon) if (*colon < '0' || *colon > '9') return;
    }
  }
}
} // namespace onchip
#endif
