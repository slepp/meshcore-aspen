// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <Arduino.h>
#include <cassert>
#include <cstring>
#include <esp_heap_caps.h>

// The SDK boundary deliberately omits date verification, like the pinned S3
// library. Tests below execute production SecureTransport, not OpenSSL's TLS.
#define MBEDTLS_SSL_KEEP_PEER_CERTIFICATE
#define MBEDTLS_SSL_RENEGOTIATION
constexpr int MBEDTLS_SSL_RENEGOTIATION_DISABLED = 0;
struct mbedtls_ssl_config { bool renegotiation = true; };
struct mbedtls_ssl_context {};
struct sslclient_context { mbedtls_ssl_config ssl_conf; mbedtls_ssl_context ssl_ctx; };
inline const char *mbedtls_ssl_get_ciphersuite(const mbedtls_ssl_context *) {
  return "TLS-ECDHE-RSA-WITH-AES-128-GCM-SHA256";
}
inline int mbedtls_ssl_get_ciphersuite_id(const char *) { return 0xc02f; }
inline void mbedtls_ssl_conf_renegotiation(mbedtls_ssl_config *config, int enabled) {
  config->renegotiation = enabled != MBEDTLS_SSL_RENEGOTIATION_DISABLED;
}
struct mbedtls_x509_time { int year, mon, day, hour, min, sec; };
struct mbedtls_x509_crt {
  struct { unsigned char *p; size_t len; } raw{};
  mbedtls_x509_time valid_from{}, valid_to{};
  mbedtls_x509_crt *next = nullptr;
};
namespace device_tls_test {
inline unsigned char encoded = 1;
inline mbedtls_x509_crt leaf, intermediate, root;
inline mbedtls_x509_crt *peer = &leaf;
inline unsigned connections = 0, writes = 0, stops = 0, parses = 0, frees = 0;
inline unsigned clients = 0;
inline size_t parseBytes = 0, connectionBytes[3]{};
inline void *parseAllocation = nullptr;
inline int parseResult = 0, connectError = -1;
inline bool connectResult = true;
inline void (*afterConnect)() = nullptr;
inline void (*onRead)() = nullptr;
inline mbedtls_x509_crt certificate() {
  return {{&encoded, 1}, {2025, 1, 1, 0, 0, 0}, {2027, 1, 1, 0, 0, 0}, nullptr};
}
inline void reset() {
  assert(!clients && !parseAllocation);
  leaf = intermediate = root = certificate();
  leaf.next = &intermediate;
  peer = &leaf;
  connections = writes = stops = parses = frees = 0;
  parseResult = 0; connectResult = true; connectError = -1;
  afterConnect = onRead = nullptr;
  parseBytes = 0;
  for (auto &bytes : connectionBytes) bytes = 0;
}
}
inline void mbedtls_x509_crt_init(mbedtls_x509_crt *out) { *out = {}; }
inline int mbedtls_x509_crt_parse(mbedtls_x509_crt *out, const unsigned char *, size_t) {
  assert(!device_tls_test::clients && "Extra CA date parse must not overlap a TLS client/context");
  ++device_tls_test::parses;
  if (device_tls_test::parseBytes) {
    device_tls_test::parseAllocation = heap_caps_malloc(device_tls_test::parseBytes,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    assert(device_tls_test::parseAllocation);
  }
  *out = device_tls_test::root;
  return device_tls_test::parseResult;
}
inline void mbedtls_x509_crt_free(mbedtls_x509_crt *out) {
  ++device_tls_test::frees;
  if (device_tls_test::parseAllocation) {
    heap_caps_free(device_tls_test::parseAllocation);
    device_tls_test::parseAllocation = nullptr;
  }
  *out = {};
}
class IPAddress {
public:
  bool fromString(const char *address) { return !strcmp(address, "192.0.2.1"); }
};
constexpr int WL_CONNECTED = 3;
struct TestWiFi { int status() const { return WL_CONNECTED; } };
inline TestWiFi WiFi;
class WiFiClientSecure {
protected:
  sslclient_context storage_;
  sslclient_context *sslclient = &storage_;
private:
  void *allocations_[3]{};
  size_t offset_ = 0;
  static constexpr const char *response =
      "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 36\r\n\r\n"
      "{\"ok\":true,\"result\":{\"status\":\"ok\"}}";
public:
  WiFiClientSecure() { ++device_tls_test::clients; }
  ~WiFiClientSecure() { --device_tls_test::clients; }
  void setTimeout(unsigned seconds) { assert(seconds == 1); }
  void setHandshakeTimeout(unsigned seconds) { assert(seconds == 8); }
  void setCACert(const char *) {}
  int connect(IPAddress, uint16_t, const char *host, const char *ca, const char *cert, const char *key) {
    assert(!strcmp(host, "home.example") && ca && !cert && !key);
    ++device_tls_test::connections;
    for (unsigned i = 0; i < 3; ++i) if (device_tls_test::connectionBytes[i]) {
      allocations_[i] = heap_caps_malloc(device_tls_test::connectionBytes[i],
          MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      assert(allocations_[i]);
    }
    if (device_tls_test::afterConnect) device_tls_test::afterConnect();
    return device_tls_test::connectResult;
  }
  const mbedtls_x509_crt *getPeerCertificate() { return device_tls_test::peer; }
  int lastError(char *text, size_t size) {
    snprintf(text, size, "injected SDK error");
    return device_tls_test::connectError;
  }
  size_t write(const uint8_t *, size_t size) {
    assert(!sslclient->ssl_conf.renegotiation);
    device_tls_test::writes += size;
    return size;
  }
  int available() const { return int(strlen(response) - offset_); }
  bool connected() const { return true; }
  int read(uint8_t *data, size_t size) {
    if (device_tls_test::onRead) device_tls_test::onRead();
    size = std::min(size, strlen(response) - offset_);
    memcpy(data, response + offset_, size); offset_ += size;
    return int(size);
  }
  void stop() {
    ++device_tls_test::stops;
    for (auto &allocation : allocations_) if (allocation) {
      heap_caps_free(allocation);
      allocation = nullptr;
    }
  }
};
