// SPDX-License-Identifier: Apache-2.0
#include "BotNetworkConfig.h"
#include <SPIFFS.h>
#include <esp_heap_caps.h>
#include <SHA256.h>
#include <cassert>
#include <cstring>
#include <string>
using namespace onchip;
void delay(unsigned long) {}

static const char *ca = "-----BEGIN CERTIFICATE-----\nfixture\n-----END CERTIFICATE-----";
static const std::string fixtureToken(32, 't');
static const char *token = fixtureToken.c_str();
static bool admin(const std::string &command) {
  char reply[160]{};
  const bool ok = botHttpsAdmin(command.c_str(), reply, sizeof(reply));
  assert(!strstr(reply, token) && !strstr(reply, "fixture"));
  return ok;
}
static std::string hex(const char *text) {
  const char digits[] = "0123456789abcdef";
  std::string result;
  for (const unsigned char *p = reinterpret_cast<const unsigned char *>(text); *p; ++p) {
    result.push_back(digits[*p >> 4]); result.push_back(digits[*p & 15]);
  }
  return result;
}
struct Peer : BotHttpsTransport {
  std::string sent, response;
  size_t offset = 0;
  int opens = 0, closes = 0;
  uint32_t clock = 100;
  bool revoke = false;
  size_t revokeAt = 0;
  const char *revokeCommand = "drop service";
  bool connected = true;
  bool online() const override { return connected; }
  bool validate(char *, size_t) override { return true; }
  bool open(const BotHttpsConfig &config, char *, size_t) override {
    ++opens;
    assert(!strcmp(config.address, "192.0.2.1") &&
           !strcmp(config.host, "home.example") && !strcmp(config.token, token));
    return true;
  }
  int write(const uint8_t *bytes, size_t size) override {
    sent.append(reinterpret_cast<const char *>(bytes), size);
    return int(size);
  }
  int read(uint8_t *bytes, size_t size) override {
    if (revoke && offset >= revokeAt) {
      revoke = false;
      assert(admin(revokeCommand) && admin("commit"));
    }
    if (offset == response.size()) return -1;
    size = std::min(size, response.size() - offset);
    memcpy(bytes, response.data() + offset, size);
    offset += size;
    return int(size);
  }
  void close() override { ++closes; }
  uint32_t now() const override { return clock; }
  void idle() override { ++clock; }
  void body(const std::string &json, unsigned status = 200) {
    response = "HTTP/1.1 " + std::to_string(status) +
        " OK\r\nContent-Type: application/json\r\nContent-Length: " +
        std::to_string(json.size()) + "\r\n\r\n" + json;
    offset = 0;
  }
};
static BotIoRequest query(BotIoRequest::Kind kind) {
  BotIoRequest request;
  request.kind = kind; request.token = {1, 1, 1};
  request.deadline = 15100;
  request.grant = 1; request.principal[0] = 42;
  request.networkEpoch = botNetworkEpoch();
  if (kind == BotIoRequest::Rpc) {
    strcpy(request.endpoint, "service");
    strcpy(request.key, "lookup");
    strcpy(request.json, "{\"id\":1}");
  } else strcpy(request.key, "service");
  return request;
}
static BotIoResult perform(Peer &peer, const BotIoRequest &request,
                           const BotHttpsFetchRequest *fetch = nullptr,
                           BotHttpsFetchResult *fetched = nullptr) {
  BotNetworkRoute route;
  assert(botNetworkResolve(request, route));
  std::atomic<uint32_t> generation{1}, grant{1};
  std::atomic<bool> enabled{true}, stopping{false};
  BotHttps provider(route.https(), peer);
  BotIoResult result;
  provider.perform(request, result, generation, grant, enabled, stopping, &route, fetch, fetched);
  return result;
}
struct PackageSink : BotHttpsBodySink {
  std::string bytes;
  bool committed = false, aborted = false, failWrite = false;
  unsigned begins = 0;
  uint32_t announced = 0;
  bool begin(uint32_t length) override { ++begins; announced = length; return true; }
  bool write(const uint8_t *data, size_t size) override {
    if (failWrite) return false;
    bytes.append(reinterpret_cast<const char *>(data), size); return true;
  }
  bool commit(uint32_t size, const uint8_t[32]) override {
    committed = size == bytes.size(); return committed;
  }
  void abort(const char *) override { aborted = true; bytes.clear(); }
};
int main() {
  assert(!botNetworkConfigured());
  assert(admin("endpoint service 192.0.2.1 home.example 443 /v1/data both"));
  assert(!admin("ca service xyz"));
  const auto certificate = hex(ca);
  for (size_t offset = 0; offset < certificate.size(); offset += 80)
    assert(admin("ca service " + certificate.substr(offset, 80)));
  assert(admin("token service " + hex(token)));
  assert(admin("rpc service lookup /v1/lookup"));
  assert(admin("endpoint telemetry 192.0.2.1 home.example 443 /insert/0/influx/write post"));
  for (size_t offset = 0; offset < certificate.size(); offset += 80)
    assert(admin("ca telemetry " + certificate.substr(offset, 80)));
  assert(admin("token telemetry " + hex(token)));
  assert(admin("endpoint package 192.0.2.1 home.example 443 /v1/package get"));
  for (size_t offset = 0; offset < certificate.size(); offset += 80)
    assert(admin("ca package " + certificate.substr(offset, 80)));
  assert(admin("token package " + hex(token)));
  assert(!admin("endpoint bad 192.0.2.1 home.example 443 /v1/../secrets get"));
  assert(!admin("commit extra"));
  assert(admin("commit"));
  assert(botNetworkConfigured());
  assert(admin("endpoint home 192.0.2.1 home.example 443 /v1/rpc post"));
  assert(!admin("endpoint home 192.0.2.1 home.example 443 /wrong post"));
  assert(!admin("commit"));
  for (size_t offset = 0; offset < certificate.size(); offset += 80)
    assert(admin("ca home " + certificate.substr(offset, 80)));
  assert(admin("token home " + hex(token)));
  assert(admin("ops home 1"));
  assert(admin("commit") && botNetworkHomeConfigured());
  BotIoRequest bundled;
  bundled.kind = BotIoRequest::Rpc; bundled.token = {1, 1, 1};
  bundled.grant = 1; bundled.deadline = 15100; bundled.principal[0] = 42;
  bundled.networkEpoch = botNetworkEpoch();
  strcpy(bundled.endpoint, "home"); strcpy(bundled.key, "health");
  Peer health;
  health.body("{\"ok\":true,\"result\":{\"status\":\"ok\"}}");
  auto result = perform(health, bundled);
  assert(result.ok && !strcmp(result.value, "ok"));
  bundled.endpoint[0] = 0;
  Peer legacyHealth;
  legacyHealth.body("{\"ok\":true,\"result\":{\"status\":\"ok\"}}");
  result = perform(legacyHealth, bundled);
  assert(result.ok && !strcmp(result.value, "ok"));
  auto unnamed = query(BotIoRequest::Rpc);
  unnamed.endpoint[0] = 0;
  BotNetworkRoute unnamedRoute;
  assert(!botNetworkResolve(unnamed, unnamedRoute));
  auto malformed = bundled;
  memset(malformed.key, 'a', sizeof(malformed.key));
  assert(!botNetworkResolve(malformed, unnamedRoute));
  malformed = bundled;
  memset(malformed.endpoint, 'a', sizeof(malformed.endpoint));
  assert(!botNetworkResolve(malformed, unnamedRoute));
  strcpy(bundled.key, "echo"); strcpy(bundled.value, "denied");
  Peer deniedOperation;
  result = perform(deniedOperation, bundled);
  assert(!result.ok && !strcmp(result.rpcCode, "permission_denied") &&
         !deniedOperation.opens);
  assert(admin("endpoint service 192.0.2.1 home.example 443 /v1/new get"));
  assert(!admin("commit"));
  assert(admin("discard"));
  assert(admin("rpc service second /v1/second"));
  filesystem_test::writeLimit = 0;
  assert(!admin("commit"));
  filesystem_test::writeLimit = std::numeric_limits<size_t>::max();
  assert(admin("discard"));
  assert(admin("rpc service second /v1/second"));
  filesystem_test::failReadAfterRename = true;
  const auto oldEpoch = botNetworkEpoch();
  assert(!admin("commit") && !botNetworkConfigured() &&
         botNetworkEpoch() != oldEpoch);
  filesystem_test::readLimit = std::numeric_limits<size_t>::max();
  assert(botNetworkReload() && botNetworkConfigured());
  assert(botNetworkReload());
  assert(botNetworkConfigured());
  Peer get;
  get.body("{\"items\":[1,true,null]}");
  result = perform(get, query(BotIoRequest::HttpGet));
  assert(result.ok && !strcmp(result.json, "{\"items\":[1,true,null]}"));
  assert(get.sent.find("GET /v1/data HTTP/1.1\r\n") == 0);
  assert(get.sent.find("Authorization: Bearer " + std::string(token) + "\r\n") != std::string::npos);
  assert(get.sent.find("\r\n\r\n") == get.sent.size() - 4);
  assert(get.opens == 1 && get.closes == 1);
  std::string source(4095, 'a');
  source.push_back('\n');
  SHA256 digest;
  uint8_t expected[32]{};
  digest.update(source.data(), source.size());
  digest.finalize(expected, sizeof(expected));
  auto packageRequest = query(BotIoRequest::PackageGet);
  strcpy(packageRequest.key, "package");
  PackageSink sink;
  BotHttpsFetchRequest fetch;
  fetch.id = 7; memcpy(fetch.expectedSha256, expected, sizeof(expected));
  fetch.sink = &sink;
  BotHttpsFetchResult fetched;
  Peer package;
  const auto raw = [&](const std::string &body) {
    return "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
  };
  package.response = raw(source);
  result = perform(package, packageRequest, &fetch, &fetched);
  assert(result.ok && result.httpStatus == 200 && result.networkSubmitted &&
         sink.committed && !sink.aborted && sink.begins == 1 &&
         sink.announced == BotSourceLimit && sink.bytes == source &&
         fetched.bytes == BotSourceLimit && !memcmp(fetched.sha256, expected, 32));
  assert(package.sent.find("GET /v1/package HTTP/1.1\r\n") == 0);
  assert(package.sent.find("Accept: application/octet-stream\r\n") != std::string::npos);
  assert(package.sent.find("\r\n\r\n") == package.sent.size() - 4);
  PackageSink mismatched;
  fetch.sink = &mismatched; fetch.expectedSha256[0] ^= 1;
  Peer wrongHash; wrongHash.response = raw(source);
  result = perform(wrongHash, packageRequest, &fetch, &fetched);
  assert(!result.ok && !strcmp(result.rpcCode, "hash_mismatch") &&
         mismatched.aborted && !mismatched.committed && mismatched.bytes.empty());
  fetch.expectedSha256[0] ^= 1;
  PackageSink tooLarge;
  fetch.sink = &tooLarge;
  Peer oversized; oversized.response = raw(source + "x");
  result = perform(oversized, packageRequest, &fetch, &fetched);
  assert(!result.ok && !strcmp(result.rpcCode, "invalid_response") &&
         !tooLarge.begins && !tooLarge.committed);
  PackageSink writeFailure;
  fetch.sink = &writeFailure; writeFailure.failWrite = true;
  Peer failedWrite; failedWrite.response = raw(source);
  result = perform(failedWrite, packageRequest, &fetch, &fetched);
  assert(!result.ok && !strcmp(result.rpcCode, "storage_error") &&
         writeFailure.aborted && !writeFailure.committed);
  const std::string leakedSource(127, 'x');
  const std::string leakedBody = leakedSource + token + "\n";
  SHA256 leakedHash;
  leakedHash.update(leakedBody.data(), leakedBody.size());
  leakedHash.finalize(fetch.expectedSha256, sizeof(fetch.expectedSha256));
  PackageSink leakedPackage;
  fetch.sink = &leakedPackage;
  Peer sourceWithCredential; sourceWithCredential.response = raw(leakedBody);
  result = perform(sourceWithCredential, packageRequest, &fetch, &fetched);
  assert(!result.ok && !strcmp(result.rpcCode, "invalid_response") &&
         !result.json[0] && leakedPackage.aborted &&
         !leakedPackage.committed && leakedPackage.bytes.empty());
  memcpy(fetch.expectedSha256, expected, sizeof(expected));
  PackageSink revokedSink;
  fetch.sink = &revokedSink;
  Peer revokedPackage; revokedPackage.response = raw(source);
  revokedPackage.revoke = true;
  revokedPackage.revokeCommand = "drop package";
  revokedPackage.revokeAt = revokedPackage.response.size() - source.size() + 128;
  result = perform(revokedPackage, packageRequest, &fetch, &fetched);
  assert(!result.ok && !strcmp(result.rpcCode, "unknown") &&
         result.networkSubmitted && revokedSink.aborted &&
         !revokedSink.committed && revokedSink.bytes.empty());

  Peer offline;
  offline.connected = false;
  result = perform(offline, query(BotIoRequest::HttpGet));
  assert(!result.ok && !strcmp(result.rpcCode, "offline") &&
         !result.networkSubmitted && !offline.opens);
  Peer leaked;
  leaked.body("{\"credential\":\"" + std::string(token) + "\"}");
  result = perform(leaked, query(BotIoRequest::HttpGet));
  assert(!result.ok && !strcmp(result.rpcCode, "invalid_response") && !result.json[0]);
  Peer post;
  post.body("42");
  auto request = query(BotIoRequest::HttpPost);
  strcpy(request.json, "{\"value\":true}");
  result = perform(post, request);
  assert(result.ok && !strcmp(result.json, "42"));
  assert(post.sent.find("POST /v1/data HTTP/1.1\r\n") == 0);
  assert(post.sent.find("\r\n\r\n{\"value\":true}") != std::string::npos);
  Peer rpc;
  rpc.body("{\"ok\":true,\"result\":[{\"value\":1},false]}");
  result = perform(rpc, query(BotIoRequest::Rpc));
  assert(result.ok && !strcmp(result.json, "[{\"value\":1},false]"));
  assert(rpc.sent.find("POST /v1/lookup HTTP/1.1\r\n") == 0);
  assert(rpc.sent.find("\"operation\":\"lookup\"") != std::string::npos);
  assert(rpc.sent.find("\"args\":{\"id\":1}") != std::string::npos);
  Peer rpcError;
  rpcError.body("{\"ok\":false,\"error\":{\"code\":\"not_found\",\"message\":\"unknown item\"}}", 404);
  result = perform(rpcError, query(BotIoRequest::Rpc));
  assert(!result.ok && !strcmp(result.rpcCode, "not_found") &&
         !strcmp(result.error, "Configured RPC rejected") && !result.json[0]);
  Peer rejected;
  rejected.body("{\"message\":\"must not expose\"}", 401);
  result = perform(rejected, query(BotIoRequest::HttpGet));
  assert(!result.ok && !strcmp(result.rpcCode, "http_error") && !result.json[0]);
  Peer revoked;
  revoked.body("{\"ok\":true,\"result\":1}");
  revoked.revoke = true;
  result = perform(revoked, query(BotIoRequest::Rpc));
  assert(!result.ok && !strcmp(result.rpcCode, "unknown") &&
         result.networkSubmitted && !result.json[0]);
  BotNetworkRoute absent;
  assert(!botNetworkResolve(query(BotIoRequest::HttpGet), absent));
  assert(botNetworkReload() && botNetworkConfigured() && botNetworkHomeConfigured());
  assert(psram_test::allocations.size() == 1);
  puts("PASS bounded named HTTPS, RPC, SPIFFS reload and revocation");
}
