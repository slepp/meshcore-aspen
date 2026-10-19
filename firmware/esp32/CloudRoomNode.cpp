// SPDX-License-Identifier: Apache-2.0
#include "CloudRoomLifecycle.h"
#include "CloudRoomNode.h"
#if ONCHIP_CLOUD_ROOM_LINK
#include "RoleIdentity.h"
#include <Utils.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

namespace onchip {
namespace cloudnode {
namespace {
constexpr char Alphabet[] = "0123456789abcdef";
void wipe(void *data, size_t size) {
  auto *bytes = static_cast<volatile uint8_t *>(data);
  while (size--) *bytes++ = 0;
}
bool wait(CloudTransport &transport, uint32_t start, uint32_t timeoutMs) {
  return uint32_t(transport.now() - start) < timeoutMs;
}
int hexDigit(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }
}
void Writer::raw(const char *text, size_t length) {
  if (!good_ || length >= capacity_ - size_) { good_ = false; return; }
  memcpy(out_ + size_, text, length);
  size_ += length; out_[size_] = 0;
}
void Writer::text(const char *value) { raw(value, strlen(value)); }
void Writer::quoted(const char *value) {
  raw("\"", 1);
  for (; *value; ++value) {
    if (*value == '"' || *value == '\\') raw("\\", 1);
    raw(value, 1);
  }
  raw("\"", 1);
}
void Writer::hex(const uint8_t *bytes, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    const char pair[2] = {Alphabet[bytes[i] >> 4], Alphabet[bytes[i] & 15]};
    raw(pair, 2);
  }
}
void Writer::number(uint32_t value) {
  char digits[12];
  snprintf(digits, sizeof(digits), "%lu", static_cast<unsigned long>(value));
  text(digits);
}
void sha256Hex(const char *text, size_t length, size_t bytes, char *out) {
  uint8_t hash[32];
  mesh::Utils::sha256(hash, 32, reinterpret_cast<const uint8_t *>(text), length);
  for (size_t i = 0; i < bytes; ++i) { out[i * 2] = Alphabet[hash[i] >> 4]; out[i * 2 + 1] = Alphabet[hash[i] & 15]; }
  out[bytes * 2] = 0;
}
void codeHash(const char canonical[16], char out[65]) {
  char text[19 + 16 + 1];
  memcpy(text, "aspen-node.v1.code\n", 19);
  memcpy(text + 19, canonical, 16);
  sha256Hex(text, 35, 32, out);
  wipe(text, sizeof(text));
}
bool signRedeem(const char *host, const char *op, const char *codeOrOpen, const char *subject,
                uint32_t ts, char nodeHex[65], char sigHex[129]) {
  char hash[65];
  if (!strcmp(codeOrOpen, "OPEN")) strcpy(hash, "open");
  else { codeHash(codeOrOpen, hash); hash[32] = 0; }
  char message[400];
  Writer w(message, sizeof(message));
  w.text(Protocol); w.text("\nhttps://"); w.text(host); w.text("\n"); w.text(op); w.text("\n");
  w.text(hash); w.text("\n");
  uint8_t sig[64], pub[32];
  // The node id sits in the message, so the public key is needed first.
  if (!identityPublicKeyIfPresent("management", pub)) return false;
  w.hex(pub, 32); w.text("\n"); w.number(ts); w.text("\n"); w.text(subject);
  if (!w.good() || !signManagement(reinterpret_cast<const uint8_t *>(message), w.size(), sig, pub)) {
    wipe(message, sizeof(message));
    return false;
  }
  wipe(message, sizeof(message));
  for (unsigned i = 0; i < 32; ++i) { nodeHex[i * 2] = Alphabet[pub[i] >> 4]; nodeHex[i * 2 + 1] = Alphabet[pub[i] & 15]; }
  nodeHex[64] = 0;
  for (unsigned i = 0; i < 64; ++i) { sigHex[i * 2] = Alphabet[sig[i] >> 4]; sigHex[i * 2 + 1] = Alphabet[sig[i] & 15]; }
  sigHex[128] = 0;
  return true;
}
Net exchange(CloudTransport &transport, const CloudTlsTarget &target, const char *method,
             const char *path, const char *bearer, const char *body, size_t bodySize,
             char *buffer, size_t bufferSize, Reply &reply, uint32_t timeoutMs) {
  reply = {};
  const uint32_t start = transport.now();   // the whole exchange, handshake included, shares the limit
  CloudTlsFailure failure = CloudTlsFailure::None;
  if (!transport.open(target, failure)) {
    transport.close();
    switch (failure) {
    case CloudTlsFailure::Clock: return Net::Clock;
    case CloudTlsFailure::Verify: return Net::Verify;
    case CloudTlsFailure::Memory: return Net::Memory;
    default: return Net::Network;
    }
  }
  struct Closer { CloudTransport &t; ~Closer() { t.close(); } } closer{transport};
  // Request head: the bearer token is written straight into the exchange buffer, which
  // is wiped before this function returns.
  Writer head(buffer, bufferSize);
  head.text(method); head.text(" "); head.text(path); head.text(" HTTP/1.1\r\nHost: "); head.text(target.host);
  head.text("\r\nUser-Agent: AspenNode/1\r\nAccept: application/json\r\nAccept-Encoding: identity\r\nConnection: close\r\n");
  if (bearer) { head.text("Authorization: Bearer "); head.text(bearer); head.text("\r\n"); }
  if (body) { head.text("Content-Type: application/json\r\nContent-Length: "); head.number(uint32_t(bodySize)); head.text("\r\n"); }
  head.text("\r\n");
  const auto send = [&](const uint8_t *data, size_t size) {
    size_t sent = 0;
    while (sent < size) {
      const int n = transport.write(data + sent, size - sent);
      if (n < 0) return false;
      sent += size_t(n);
      if (sent == size) break;
      if (!wait(transport, start, timeoutMs)) return false;
      if (!n) transport.idle();
    }
    return true;
  };
  bool sent = head.good() && send(reinterpret_cast<const uint8_t *>(buffer), head.size());
  wipe(buffer, bufferSize);
  if (sent && body && bodySize) sent = send(reinterpret_cast<const uint8_t *>(body), bodySize);
  if (!sent) return Net::Network;
  // Response: headers, then a Content-Length, chunked or close-delimited body. The whole
  // exchange fits `buffer`; a reply that does not is malformed.
  size_t have = 0, headerEnd = 0;
  size_t contentLength = SIZE_MAX;
  bool chunked = false;
  unsigned status = 0;
  const auto parseHeaders = [&]() {
    if (have < 12 || memcmp(buffer, "HTTP/1.1 ", 9)) return false;
    for (unsigned i = 9; i < 12; ++i) {
      if (buffer[i] < '0' || buffer[i] > '9') return false;
      status = status * 10 + unsigned(buffer[i] - '0');
    }
    const char *line = static_cast<const char *>(memchr(buffer, '\n', headerEnd));
    while (line && line + 1 < buffer + headerEnd - 1) {
      ++line;
      const char *end = static_cast<const char *>(memchr(line, '\r', size_t(buffer + headerEnd - line)));
      if (!end) return false;
      if (!strncasecmp(line, "Content-Length:", 15)) {
        contentLength = 0;
        for (const char *p = line + 15; p < end; ++p) {
          if (*p == ' ') continue;
          if (*p < '0' || *p > '9' || contentLength > ResponseLimit) return false;
          contentLength = contentLength * 10 + size_t(*p - '0');
        }
      } else if (!strncasecmp(line, "Transfer-Encoding:", 18)) {
        for (const char *p = line + 18; p + 7 <= end; ++p) if (!strncasecmp(p, "chunked", 7)) chunked = true;
      } else if (!strncasecmp(line, "Retry-After:", 12)) {
        uint32_t value = 0;
        for (const char *p = line + 12; p < end; ++p) {
          if (*p == ' ') continue;
          if (*p < '0' || *p > '9') { value = 0; break; }
          value = value * 10 + uint32_t(*p - '0');
          if (value > 86400) { value = 86400; break; }
        }
        reply.retryAfter = value;
      }
      line = static_cast<const char *>(memchr(end, '\n', size_t(buffer + headerEnd - end)));
    }
    return contentLength == SIZE_MAX || contentLength <= ResponseLimit;
  };
  // Walks chunked framing without modifying it. Complete leaves the decoded size in `decoded`.
  enum class Walk { Incomplete, Complete, Bad };
  size_t decoded = 0;
  const auto walk = [&](size_t from) {
    size_t in = from;
    decoded = 0;
    for (;;) {
      const char *eol = static_cast<const char *>(memchr(buffer + in, '\n', have - in));
      if (!eol) return Walk::Incomplete;
      size_t size = 0;
      const char *p = buffer + in;
      if (p == eol || hexDigit(*p) < 0) return Walk::Bad;
      for (; p < eol && hexDigit(*p) >= 0; ++p) {
        size = size * 16 + size_t(hexDigit(*p));
        if (size > ResponseLimit) return Walk::Bad;
      }
      in = size_t(eol - buffer) + 1;
      if (!size) return Walk::Complete;
      if (have - in < size + 2) return Walk::Incomplete;
      if (buffer[in + size] != '\r' || buffer[in + size + 1] != '\n') return Walk::Bad;
      decoded += size;
      if (decoded > ResponseLimit) return Walk::Bad;
      in += size + 2;
    }
  };
  bool closed = false;
  for (;;) {
    if (headerEnd) {
      if (chunked) {
        const Walk state = walk(headerEnd);
        if (state == Walk::Bad) return Net::Malformed;
        if (state == Walk::Complete) break;
      } else if (contentLength != SIZE_MAX) {
        if (have - headerEnd >= contentLength) break;
      } else if (closed) break;
    }
    if (closed || have + 1 >= bufferSize || !wait(transport, start, timeoutMs)) return Net::Malformed;
    const int n = transport.read(reinterpret_cast<uint8_t *>(buffer) + have, bufferSize - 1 - have);
    if (n < 0) { closed = true; if (!headerEnd) return Net::Malformed; continue; }
    if (!n) { transport.idle(); continue; }
    have += size_t(n);
    buffer[have] = 0;
    if (!headerEnd) {
      const char *end = strstr(buffer, "\r\n\r\n");
      if (end) {
        headerEnd = size_t(end - buffer) + 4;
        if (!parseHeaders()) return Net::Malformed;
      }
    }
  }
  size_t size;
  if (chunked) {
    // Compact the chunks to the front of the buffer, in order.
    size_t in = headerEnd, out = 0;
    for (;;) {
      const char *eol = static_cast<const char *>(memchr(buffer + in, '\n', have - in));
      size_t chunk = 0;
      for (const char *p = buffer + in; p < eol && hexDigit(*p) >= 0; ++p) chunk = chunk * 16 + size_t(hexDigit(*p));
      in = size_t(eol - buffer) + 1;
      if (!chunk) break;
      memmove(buffer + out, buffer + in, chunk);
      out += chunk; in += chunk + 2;
    }
    size = out;
  } else {
    size = contentLength != SIZE_MAX ? contentLength : have - headerEnd;
    if (size > ResponseLimit) return Net::Malformed;
    memmove(buffer, buffer + headerEnd, size);
  }
  buffer[size] = 0;
  reply.status = uint16_t(status);
  reply.body = buffer;
  reply.size = size;
  return Net::Ok;
}
} // namespace cloudnode
} // namespace onchip
#endif // ONCHIP_CLOUD_ROOM_LINK
