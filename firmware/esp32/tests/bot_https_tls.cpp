// SPDX-License-Identifier: Apache-2.0
#include "BotHttps.h"
#include "BotNetworkConfig.h"
#include "BotVm.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <openssl/ssl.h>
#include <openssl/pem.h>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
using namespace onchip;
void delay(unsigned long) {}
#if defined(ONCHIP_BOT_NATIVE_HTTPS)
#include "HttpsTransport.h"
unsigned long millis() {
  return uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}
using TLS = NativeBotHttpsTransport;
#else
class TLS : public BotHttpsTransport {
  SSL_CTX *context_ = nullptr;
  SSL *ssl_ = nullptr;
  int socket_ = -1;
public:
  bool validate(char *, size_t) override { return true; }
  unsigned transmitted = 0;
  ~TLS() override { close(); }
  bool open(const BotHttpsConfig &config, char *error, size_t capacity) override {
    const auto fail = [&]() { snprintf(error, capacity, "Loopback TLS verification failed"); return false; };
    context_ = SSL_CTX_new(TLS_client_method());
    if (!context_) return fail();
    SSL_CTX_set_min_proto_version(context_, TLS1_2_VERSION);
    SSL_CTX_set_verify(context_, SSL_VERIFY_PEER, nullptr);
    BIO *bio = BIO_new_mem_buf(config.ca, -1);
    X509 *certificate = bio ? PEM_read_bio_X509(bio, nullptr, nullptr, nullptr) : nullptr;
    BIO_free(bio);
    if (!certificate) return fail();
    const int trusted = X509_STORE_add_cert(SSL_CTX_get_cert_store(context_), certificate);
    X509_free(certificate);
    if (trusted != 1) return fail();
    socket_ = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_ < 0) return fail();
    timeval timeout{2, 0};
    assert(!setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));
    assert(!setsockopt(socket_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)));
    sockaddr_in target{};
    target.sin_family = AF_INET; target.sin_port = htons(config.port);
    if (inet_pton(AF_INET, config.address, &target.sin_addr) != 1 ||
        connect(socket_, reinterpret_cast<sockaddr *>(&target), sizeof(target))) return fail();
    ssl_ = SSL_new(context_);
    if (!ssl_ || SSL_set_fd(ssl_, socket_) != 1 ||
        SSL_set_tlsext_host_name(ssl_, config.host) != 1 ||
        SSL_set1_host(ssl_, config.host) != 1 ||
        SSL_connect(ssl_) != 1 || SSL_get_verify_result(ssl_) != X509_V_OK) return fail();
    return true;
  }
  int write(const uint8_t *bytes, size_t size) override {
    const int n = SSL_write(ssl_, bytes, int(size));
    if (n > 0) transmitted += n;
    return n;
  }
  int read(uint8_t *bytes, size_t size) override {
    const int n = SSL_read(ssl_, bytes, int(size));
    return n <= 0 ? -1 : n;
  }
  void close() override {
    if (ssl_) { SSL_free(ssl_); ssl_ = nullptr; }
    if (context_) { SSL_CTX_free(context_); context_ = nullptr; }
    if (socket_ >= 0) { ::close(socket_); socket_ = -1; }
  }
  uint32_t now() const override {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
  }
  void idle() override { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
};
#endif
int main(int argc, char **argv) {
  assert(argc == 5);
  const bool weather = !strcmp(argv[4], "1");
  const auto read = [](const char *path) {
    std::ifstream file(path); assert(file);
    return std::string(std::istreambuf_iterator<char>(file), {});
  };
  const auto certificate = read(argv[2]), wrongCertificate = read(argv[3]);
  const char *token = std::getenv("MESHCORE_BOT_SERVICE_TOKEN");
  assert(token && strlen(token) == 32);
  BotHttpsConfig config{"127.0.0.1", "localhost", certificate.c_str(),
      token, uint16_t(std::stoi(argv[1])), 7};
  std::atomic<uint32_t> generation{1}, grant{1};
  std::atomic<bool> enabled{true}, stopping{false};
  if (!strcmp(argv[4], "unknown") || !strcmp(argv[4], "cancel")) {
    const bool cancel = !strcmp(argv[4], "cancel");
    TLS transport;
    BotHttps provider(config, transport);
    BotNetworkRoute route;
    strcpy(route.address, config.address); strcpy(route.host, config.host);
    strcpy(route.ca, config.ca); strcpy(route.token, config.token);
    strcpy(route.path, cancel ? "/effect/cancel" : "/effect/unknown");
    route.port = config.port; route.operations = 7; route.post = true;
    BotIoRequest request;
    request.kind = BotIoRequest::HttpPost; request.token = {1, 1, 1};
    request.principal[0] = 1; request.grant = 1;
    request.deadline = transport.now() + BotHttpsDeadlineMs;
    strcpy(request.key, "effects"); strcpy(request.json, "{\"value\":1}");
    std::thread cancellation;
    if (cancel) cancellation = std::thread([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      stopping = true;
    });
    BotIoResult response;
    provider.perform(request, response, generation, grant, enabled, stopping, &route);
    if (cancellation.joinable()) cancellation.join();
    assert(!response.ok && response.networkSubmitted && !response.json[0] &&
           !strcmp(response.rpcCode, cancel ? "cancelled" : "unknown"));
    if (cancel) assert(response.httpStatus == 200);
    puts("PASS real TLS POST: server accepted effect, interrupted/cancelled response remains uncertain and is not replayed");
    return 0;
  }
  const auto query = [&](const BotHttpsConfig &selected, const char *operation, const char *argument) {
    TLS transport;
    BotHttps provider(selected, transport);
    BotIoRequest request;
    request.kind = BotIoRequest::Rpc; request.token = {1, 1, 1}; request.principal[0] = 1;
    request.deadline = transport.now() + BotHttpsDeadlineMs; request.grant = 1;
    strcpy(request.key, operation); strcpy(request.value, argument);
    BotIoResult result;
    provider.perform(request, result, generation, grant, enabled, stopping);
    if (!strcmp(result.rpcCode, "transport_error")) assert(transport.transmitted == 0);
    return result;
  };
  auto result = query(config, "health", "");
  assert(result.ok && !strcmp(result.value, "ok"));
  result = query(config, "echo", "quoted \"hello\" \\ newline-free");
  assert(result.ok && !strcmp(result.value, "quoted \"hello\" \\ newline-free"));
  result = query(config, "weather", "Park");
  if (weather) {
    assert(result.ok && result.temperatureC == 12.5 && result.weatherCode == 3 &&
           !strcmp(result.value, "Fixture City") && result.sourceAgeSeconds >= 900 && result.sourceAgeSeconds <= 961);
  } else assert(!result.ok && result.httpStatus == 503 && !strcmp(result.rpcCode, "weather_disabled"));
  auto invalid = config; invalid.host = "wrong.example";
  assert(!strcmp(query(invalid, "health", "").rpcCode, "transport_error"));
  invalid = config; invalid.ca = wrongCertificate.c_str();
  assert(!strcmp(query(invalid, "health", "").rpcCode, "transport_error"));
  invalid = config; invalid.token = "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz";
  result = query(invalid, "health", "");
  assert(!result.ok && result.httpStatus == 401 && !strcmp(result.rpcCode, "unauthorized"));
  const auto hex = [](const std::string &bytes) {
    std::string encoded;
    const char digits[] = "0123456789abcdef";
    for (unsigned char byte : bytes) {
      encoded.push_back(digits[byte >> 4]);
      encoded.push_back(digits[byte & 15]);
    }
    return encoded;
  };
  const auto admin = [&](const std::string &text) {
    char reply[160]{};
    assert(botHttpsAdmin(text.c_str(), reply, sizeof(reply)));
    assert(!strstr(reply, token) && !strstr(reply, "BEGIN CERTIFICATE"));
  };
  const std::string port = argv[1];
  for (const auto &endpoint : {
      std::pair{"home_status", "/v1/example/status get"},
      std::pair{"home_echo", "/v1/example/echo post"},
      std::pair{"home_rpc", "/v1/rpc post"},
      std::pair{"home", "/v1/rpc post"}}) {
    admin("endpoint " + std::string(endpoint.first) + " 127.0.0.1 localhost " +
          port + " " + endpoint.second);
    const std::string authority = hex(certificate);
    for (size_t at = 0; at < authority.size(); at += 80)
      admin("ca " + std::string(endpoint.first) + " " + authority.substr(at, 80));
    admin("token " + std::string(endpoint.first) + " " + hex(token));
  }
  admin("ops home 7");
  admin("rpc home_rpc sum /v1/rpc");
  admin("rpc home_rpc digest /v1/rpc");
  admin("commit");
  assert(botNetworkReload() && botNetworkConfigured());
  const auto named = [&](BotIoRequest request) {
    if (request.kind != BotIoRequest::Rpc && !request.key[0] && request.endpoint[0]) {
      strcpy(request.key, request.endpoint);
      request.endpoint[0] = 0;
    }
    BotNetworkRoute route;
    assert(botNetworkResolve(request, route));
    TLS transport;
    BotHttps provider(route.https(), transport);
    if (!request.token.job) request.token = {1, 7, 1};
    request.principal[0] = 1;
    request.grant = 1; request.networkEpoch = botNetworkEpoch();
    request.deadline = transport.now() + BotHttpsDeadlineMs;
    BotIoResult response;
    provider.perform(request, response, generation, grant, enabled, stopping, &route);
    return response;
  };
  BotIoRequest example;
  example.kind = BotIoRequest::HttpGet; strcpy(example.key, "home_status");
  result = named(example);
  assert(result.ok && !strcmp(result.json,
      "{\"service\":\"meshcore-bot-service\",\"status\":\"ok\"}"));
  example.kind = BotIoRequest::HttpPost; strcpy(example.key, "home_echo");
  strcpy(example.json, "{\"text\":\"over TLS\"}");
  result = named(example);
  assert(result.ok && !strcmp(result.json, "{\"text\":\"over TLS\"}"));
  example = {}; example.kind = BotIoRequest::Rpc;
  strcpy(example.endpoint, "home_rpc"); strcpy(example.key, "sum");
  strcpy(example.json, "{\"a\":13,\"b\":29}");
  result = named(example);
  assert(result.ok && !strcmp(result.json, "{\"sum\":42}"));
  example.kind = BotIoRequest::HttpGet;
  strcpy(example.key, "home_rpc");
  BotNetworkRoute forbidden;
  assert(!botNetworkResolve(example, forbidden));
  example = {}; example.kind = BotIoRequest::Rpc;
  strcpy(example.endpoint, "home_rpc"); strcpy(example.key, "unmapped");
  assert(!botNetworkResolve(example, forbidden));
  example = {}; example.kind = BotIoRequest::HttpGet;
  strcpy(example.key, "arbitrary_host");
  assert(!botNetworkResolve(example, forbidden));
  {
    std::ifstream source("../runtime/examples/network_api.lua");
    assert(source);
    const std::string plugin((std::istreambuf_iterator<char>(source)), {});
    BotSession vm;
    BotVmStats stats;
    char error[128]{};
    assert(vm.load(plugin.c_str(), plugin.size(), 1, stats, error, sizeof(error)));
    for (const auto &entry : {
        std::pair{"!net_health", "Home: ok"},
        std::pair{"!net_status", "meshcore-bot-service: ok"},
        std::pair{"!net_echo named TLS", "named TLS"},
        std::pair{"!net_sum 13 29", "Sum: 42"},
        std::pair{"!net_digest hello", "70ef65897fbe9afb5dfe8c825327057d1e174e0dfc3d299c340aeb35adcadfe3"}}) {
      BotEvent event;
      assert(parseBotCommand(entry.first, strlen(entry.first), event, error, sizeof(error)));
      event.authenticated = event.homeAccess = true;
      event.homeGrant = 1; event.sender[0] = 1;
      assert(vm.start(8, event, error, sizeof(error)));
      BotIoRequest request;
      assert(vm.nextIo(request));
      BotIoResult response = named(request);
      assert(vm.complete(response));
      BotSession::Result done;
      assert(vm.poll(done) && done.ok && !strcmp(done.action.text, entry.second));
    }
  }
  const auto command = [&](const char *text) {
    BotSession vm;
    BotVmStats stats;
    char error[128];
    assert(vm.load(BotDefaultSource, strlen(BotDefaultSource), 1, stats, error, sizeof(error)));
    BotEvent event;
    assert(parseBotCommand(text, strlen(text), event, error, sizeof(error)));
    event.authenticated = event.homeAccess = true; event.homeGrant = 1; event.sender[0] = 1;
    assert(vm.start(1, event, error, sizeof(error)));
    BotIoRequest request;
    assert(vm.nextIo(request));
    TLS transport;
    BotHttps provider(config, transport);
    request.deadline = transport.now() + BotHttpsDeadlineMs;
    BotIoResult response;
    provider.perform(request, response, generation, grant, enabled, stopping);
    BotSession::Result done;
    assert(vm.complete(response) && vm.poll(done) && done.ok && done.action.kind == BotAction::Reply);
    return std::string(done.action.text);
  };
  assert(command("!service health") == "Home health: ok");
  assert(command("!service echo loopback hello") == "Home echo: loopback hello");
  for (const char *text : {"!weather Fixture City", "!service weather Fixture City"}) {
    const auto reply = command(text);
    if (weather) assert(reply.find("Weather Fixture City: 12.5 C; code 3; age ") == 0 && reply.back() == 's');
    else assert(reply.find("Home weather error: weather_disabled; ") == 0);
  }
  if (weather) for (const auto &entry : {
      std::pair{"Offline", "provider_unavailable"}, {"Timeout", "provider_timeout"},
      {"Malformed", "provider_bad_response"}, {"Oversize", "provider_bad_response"}, {"Stale", "provider_stale"}}) {
    const auto reply = command(("!weather " + std::string(entry.first)).c_str());
    assert(reply.find("Home weather error: " + std::string(entry.second) + "; ") == 0);
  }
  puts("PASS real loopback TLS reference service + named GET/POST/RPC Lua, restricted endpoints, persisted config, legacy weather/echo/health and CA/hostname rejection; not device RF/ESP32 TLS evidence");
}
