// SPDX-License-Identifier: Apache-2.0
#include "BotHttps.h"
#include "BotHttpsMetrics.h"
#include "BotVm.h"
#include <esp_heap_caps.h>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <fstream>
#include <iterator>
#include <string>
#include <algorithm>
using namespace onchip;
void delay(unsigned long) {}
uint64_t onchipBotVmTestClock() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}
static const char *health = "{\"ok\":true,\"result\":{\"status\":\"ok\"}}";
static BotHttpsConfig configuration() {
  return {"192.0.2.1", "home.example", "-----BEGIN CERTIFICATE-----\nfixture\n-----END CERTIFICATE-----",
          "0123456789abcdef0123456789abcdef", 443, 7};
}
struct Transport : BotHttpsTransport {
  bool validate(char *, size_t) override { return true; }
  std::string response, sent;
  size_t offset = 0, opens = 0, closes = 0;
  uint32_t clock = 100;
  bool rejected = false, stalled = false;
  std::function<void()> opening, reading;
  bool open(const BotHttpsConfig &config, char *error, size_t capacity) override {
    ++opens;
    assert(!strcmp(config.host, "home.example"));
    if (opening) opening();
    if (rejected) snprintf(error, capacity, "fixture certificate rejected");
    return !rejected;
  }
  int write(const uint8_t *data, size_t size) override {
    size = std::min(size, size_t(7)); sent.append(reinterpret_cast<const char *>(data), size); return int(size);
  }
  int read(uint8_t *data, size_t size) override {
    if (reading) reading();
    if (stalled) return 0;
    if (offset == response.size()) return -1;
    size = std::min({size, size_t(3), response.size() - offset});
    memcpy(data, response.data() + offset, size); offset += size; return int(size);
  }
  void close() override { ++closes; }
  uint32_t now() const override { return clock; }
  void idle() override { clock += 100; }
  void body(const std::string &body, unsigned status = 200) {
    offset = 0;
    response = "HTTP/1.1 " + std::to_string(status) + " Fixture\r\nContent-Type: application/json; charset=utf-8\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
  }
};
static BotIoRequest request() {
  BotIoRequest value;
  value.kind = BotIoRequest::Rpc; value.token = {1, 1, 1};
  value.deadline = 15100; value.grant = 1; value.principal[0] = 1;
  strcpy(value.key, "health"); return value;
}
static BotIoResult perform(Transport &transport, const BotIoRequest &request,
                           BotHttpsConfig config = configuration()) {
  std::atomic<uint32_t> generation{1}, grant{1};
  std::atomic<bool> enabled{true}, stopping{false};
  BotHttps provider(config, transport);
  BotIoResult result;
  provider.perform(request, result, generation, grant, enabled, stopping);
  return result;
}
static void protocol() {
  Transport transport; transport.body(health);
  auto query = request();
  auto result = perform(transport, query);
  assert(result.ok && result.httpStatus == 200 && !strcmp(result.value, "ok"));
  assert(transport.sent.find("POST /v1/rpc HTTP/1.1\r\nHost: home.example:443\r\n") == 0);
  assert(transport.sent.find("\"request_id\":\"1-1-1\",\"args\":{}") != std::string::npos);
  assert(transport.opens == 1 && transport.closes == 1);
  assert(psram_test::allocations.empty());
  for (const std::string &bad : std::initializer_list<std::string>{
      "HTTP/1.1 302 Redirect\r\nLocation: https://other.example\r\n\r\n",
      "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 3\r\nContent-Length: 3\r\n\r\nfoo",
      "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 2049\r\n\r\n",
      "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nTransfer-Encoding: chunked\r\n\r\n",
      "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 1\r\n\r\nx",
      "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 1\r\nContent-Encoding: gzip\r\n\r\nx",
      "HTTP/1.1 200 OK\n\n", std::string(2050, 'x'),
      std::string("HTTP/1.1 200 OK\r\nX: ") + std::string("a\0b", 3) + "\r\n\r\n"}) {
    Transport peer; peer.response = bad;
    result = perform(peer, query);
    assert(!result.ok && result.rpcCode[0] && result.error[0] && peer.closes == 1);
  }
  Transport stalled; stalled.stalled = true;
  result = perform(stalled, query);
  assert(!result.ok && !strcmp(result.rpcCode, "timeout") && stalled.clock == query.deadline);
  struct Expiring : Transport {
    mutable unsigned completedChecks = 0;
    uint32_t now() const override {
      // Expire after the pre-decode check of the fully received body.
      if (offset && offset == response.size() && ++completedChecks >= 2) return 15100;
      return Transport::now();
    }
  } expiring;
  expiring.body(health);
  result = perform(expiring, query);
  assert(!result.ok && !strcmp(result.rpcCode, "timeout"));
  Transport untrusted; untrusted.rejected = true;
  result = perform(untrusted, query);
  assert(!result.ok && untrusted.sent.empty());
  for (const char *address : {"", "home.example", "127.1", "256.0.0.1", "0.0.0.0", "224.0.0.1"}) {
    auto config = configuration(); config.address = address;
    Transport peer; result = perform(peer, query, config);
    assert(!result.ok && peer.opens == 0);
  }
  for (const char *host : {"https://home.example", "home.example\r\nX:bad", "user@home.example"}) {
    auto config = configuration(); config.host = host;
    assert(!config.valid());
  }
  auto config = configuration(); config.operations = BotHttpsConfig::Health;
  auto echo = query; strcpy(echo.key, "echo"); strcpy(echo.value, "a\"b\\c");
  Transport denied; assert(!perform(denied, echo, config).ok && denied.opens == 0);
  Transport escaped; escaped.body("{\"ok\":true,\"result\":{\"text\":\"a\\\"b\\\\c\"}}");
  assert(perform(escaped, echo).ok);
  assert(escaped.sent.find("\"text\":\"a\\\"b\\\\c\"") != std::string::npos);
  psram_test::failAfter = 0;
  Transport exhausted; assert(!perform(exhausted, query).ok && exhausted.opens == 0);
  psram_test::failAfter = -1;
}
static void responses() {
  auto query = request();
  const auto decode = [&](const std::string &body, unsigned status = 200) {
    BotIoResult result; result.httpStatus = status;
    BotHttps::decode(query, body.data(), body.size(), result);
    return result;
  };
  for (const std::string &bad : std::initializer_list<std::string>{
      "{\"ok\":true,\"ok\":false,\"result\":{\"status\":\"ok\"}}",
      "{\"ok\":true,\"result\":{\"status\":\"ok\",\"status\":\"ok\"}}",
      "{\"ok\":true,\"result\":{\"status\":\"ok\\u0000wrong\"}}",
      "{\"ok\":true,\"result\":{\"status\":\"ok\"},\"extra\":0}",
      "{\"ok\":true,\"result\":{\"status\":\"ok\"}} trailing",
      "{\"ok\":true,\"result\":{\"status\":[[[[[]]]]]}}",
      "{\"ok\":false,\"error\":{\"code\":\"bad\",\"message\":\"failed\"}}",
      "{\"ok\":1,\"result\":{\"status\":\"ok\"}}", std::string(2049, ' ')}) {
    auto result = decode(bad);
    assert(!result.ok && !strcmp(result.rpcCode, "invalid_response"));
  }
  auto error = decode("{\"ok\":false,\"error\":{\"code\":\"weather_disabled\",\"message\":\"disabled\"}}", 503);
  assert(!error.ok && !strcmp(error.rpcCode, "weather_disabled"));
  strcpy(query.key, "weather");
  std::string weather = "{\"ok\":true,\"result\":{\"source\":\"open-meteo\",\"location\":\"Park\","
      "\"country\":\"Canada\",\"temperature_c\":-1.25,\"weather_code\":3,"
      "\"observed_at\":\"2026-09-28T12:00:00Z\",\"source_age_seconds\":120}}";
  auto result = decode(weather);
  assert(result.ok && result.temperatureC == -1.25 && result.weatherCode == 3 &&
         result.sourceAgeSeconds == 120 && !strcmp(result.value, "Park"));
  auto invalid = weather; invalid.replace(invalid.find("2026-09-28"), 10, "2026-02-30");
  assert(!decode(invalid).ok);
  invalid = weather; invalid.replace(invalid.find("-1.25"), 5, "1e999");
  assert(!decode(invalid).ok);
  for (const char *number : {"01", "1.", "+1", "1e", "-.2"}) {
    invalid = weather; invalid.replace(invalid.find("-1.25"), 5, number);
    assert(!decode(invalid).ok);
  }
  uint32_t random = 1234567;
  for (unsigned run = 0; run < 512; ++run) {
    std::string bytes(run % 257, ' ');
    for (char &byte : bytes) {
      random ^= random << 13; random ^= random >> 17; random ^= random << 5;
      byte = char(random & 255);
    }
    assert(!decode(bytes).ok);
  }
}
static void fences() {
  for (unsigned phase = 0; phase < 3; ++phase) {
    Transport peer; peer.body(health);
    BotHttps provider(configuration(), peer);
    std::atomic<uint32_t> generation{1}, grant{1};
    std::atomic<bool> enabled{true}, stopping{false};
    BotIoResult result;
    if (phase == 0) enabled = false;
    if (phase == 1) peer.opening = [&]() { generation = 2; };
    if (phase == 2) peer.reading = [&]() { grant = 2; };
    provider.perform(request(), result, generation, grant, enabled, stopping);
    assert(!result.ok && result.rpcCode[0]);
    if (phase < 2) assert(peer.sent.empty());
  }
  Transport peer;
  BotHttps provider(configuration(), peer);
  std::atomic<uint32_t> generation{1}, grant{1};
  std::atomic<bool> enabled{true}, stopping{false};
  BotIoResult result;
  auto query = request();
  for (unsigned i = 0; i < 3; ++i) {
    peer.body(health);
    provider.perform(query, result, generation, grant, enabled, stopping);
    assert(result.ok == (i < 2));
  }
  assert(!strcmp(result.rpcCode, "rate_limited") && peer.opens == 2);
}
static void coroutines() {
  BotSession vm;
  BotVmStats stats;
  char error[128]{};
  const char *source =
      "function fetch(text) local who=ctx.sender.public_key "
      "local r=rpc.call('home','echo',{text=text}) "
      "if who~=ctx.sender.public_key then return 'WRONG' end "
      "if r.ok then return r.result.text else return r.error.code end end";
  assert(vm.load(source, strlen(source), 1, stats, error, sizeof(error)));
  const auto event = [&](const char *command, uint8_t caller) {
    BotEvent event;
    assert(parseBotCommand(command, strlen(command), event, error, sizeof(error)));
    event.authenticated = event.homeAccess = true; event.homeGrant = 1; event.sender[0] = caller;
    return event;
  };
  auto a = event("!fetch first", 1), b = event("!fetch second", 2);
  assert(vm.start(1, a, error, sizeof(error)));
  BotIoRequest one, two;
  assert(vm.nextIo(one) && one.kind == BotIoRequest::Rpc && one.principal[0] == 1);
  assert(vm.start(2, b, error, sizeof(error)) && vm.nextIo(two) && two.principal[0] == 2);
  BotIoResult done; done.token = two.token; done.ok = true; strcpy(done.value, "second");
  BotSession::Result result;
  assert(vm.complete(done) && vm.poll(result) && result.ok && !strcmp(result.action.text, "second"));
  done = {}; done.token = one.token; strcpy(done.rpcCode, "offline"); strcpy(done.error, "No WiFi");
  assert(vm.complete(done) && vm.poll(result) && result.ok && !strcmp(result.action.text, "offline"));
  assert(!vm.complete(done));
  auto denied = a; denied.authenticated = false;
  assert(vm.start(3, denied, error, sizeof(error)) && vm.poll(result) && !result.ok && !vm.nextIo(one));
  denied = a; denied.homeAccess = false;
  assert(vm.start(4, denied, error, sizeof(error)) && vm.poll(result) && !result.ok && !vm.nextIo(one));
  assert(vm.start(5, a, error, sizeof(error)) && vm.nextIo(one));
  vm.cancel();
  done.token = one.token;
  assert(!vm.complete(done) && vm.poll(result) && !result.ok);
  for (const char *bad : {"rpc={} function x() end", "rpc.call('home','health',{}) function x() end"}) {
    BotSession candidate;
    assert(!candidate.load(bad, strlen(bad), 2, stats, error, sizeof(error)));
  }
}
static void hardwareFixture() {
  std::ifstream file("tests/https_probe.lua");
  assert(file);
  const std::string source(std::istreambuf_iterator<char>(file), {});
  BotSession vm;
  BotVmStats stats;
  char error[128];
  assert(vm.load(source.data(), source.size(), 1, stats, error, sizeof(error)));
  const char *commands[] = {"!health nonce", "!echo nonce", "!probe_weather Park"};
  const char *expected[] = {"Home nonce ok", "Echo nonce", "-1.25 C; code 3; age 12s"};
  for (unsigned i = 0; i < 3; ++i) {
    BotEvent event;
    assert(parseBotCommand(commands[i], strlen(commands[i]), event, error, sizeof(error)));
    event.authenticated = event.homeAccess = true; event.homeGrant = 1; event.sender[0] = 1;
    assert(vm.start(i + 1, event, error, sizeof(error)));
    BotIoRequest request;
    assert(vm.nextIo(request) && request.kind == BotIoRequest::Rpc);
    BotIoResult done; done.token = request.token; done.ok = true; done.httpStatus = 200;
    strcpy(done.value, i == 0 ? "ok" : "nonce");
    done.temperatureC = -1.25; done.weatherCode = 3; done.sourceAgeSeconds = 12;
    BotSession::Result result;
    assert(vm.complete(done) && vm.poll(result) && result.ok && !strcmp(result.action.text, expected[i]));
  }
}
static void bundledCommands() {
  BotSession vm;
  BotVmStats stats;
  char error[128]{};
  assert(vm.load(BotDefaultSource, strlen(BotDefaultSource), 1, stats, error, sizeof(error)));
  unsigned job = 0;
  const auto event = [&](const char *command, uint8_t sender = 1) {
    BotEvent e;
    assert(parseBotCommand(command, strlen(command), e, error, sizeof(error)));
    e.authenticated = e.homeAccess = true; e.homeGrant = 1; e.sender[0] = sender;
    return e;
  };
  const auto start = [&](const char *command) {
    assert(vm.start(++job, event(command), error, sizeof(error)));
    BotIoRequest io; assert(vm.nextIo(io) && io.kind == BotIoRequest::Rpc);
    io.deadline = 15100;
    return io;
  };
  const auto finish = [&](const BotIoResult &done) {
    BotSession::Result result;
    assert(vm.complete(done) && vm.poll(result) && result.ok && result.job == done.token.job);
    const std::string reply = result.action.text;
    assert(!reply.empty() && reply.size() <= BotReplyLimit);
    for (unsigned char c : reply) assert(c >= 32 && c <= 126);
    assert(!vm.complete(done));
    return reply;
  };
  const std::string weather =
      "{\"ok\":true,\"result\":{\"source\":\"open-meteo\",\"location\":\"Fixture City\","
      "\"country\":\"Testland\",\"temperature_c\":-1.25,\"weather_code\":3,"
      "\"observed_at\":\"2026-09-28T12:00:00Z\",\"source_age_seconds\":120}}";
  for (const char *command : {"!weather Fixture City", "!service weather Fixture City"}) {
    auto io = start(command);
    assert(!strcmp(io.key, "weather") && !strcmp(io.value, "Fixture City") && io.principal[0] == 1);
    Transport t; t.body(weather);
    assert(finish(perform(t, io)) == "Weather Fixture City: -1.25 C; code 3; age 120s");
  }
  auto io = start("!service health");
  Transport t; t.body(health);
  assert(!strcmp(io.key, "health") && !io.value[0] &&
         finish(perform(t, io)) == "Home health: ok");
  io = start("!service echo quoted \"hello\"");
  Transport echo; echo.body("{\"ok\":true,\"result\":{\"text\":\"quoted \\\"hello\\\"\"}}");
  assert(finish(perform(echo, io)) == "Home echo: quoted \"hello\"");
  const auto longEcho = std::string(120, '\\');
  io = start(("!service echo " + longEcho).c_str());
  Transport escapedEcho;
  escapedEcho.body("{\"ok\":true,\"result\":{\"text\":\"" + std::string(240, '\\') + "\"}}");
  const auto echoReply = finish(perform(escapedEcho, io));
  assert(echoReply.find("Home echo: ") == 0 && echoReply.find("...[truncated]") != std::string::npos &&
         echoReply.size() == 131);
  auto unicode = weather;
  unicode.replace(unicode.find("Fixture City"), 12, "Qu\\u00e9bec");
  io = start("!weather Quebec"); Transport utf8; utf8.body(unicode);
  assert(finish(perform(utf8, io)) == "Weather Qu\\xC3\\xA9bec: -1.25 C; code 3; age 120s");
  auto longName = weather;
  std::string escaped;
  for (unsigned i = 0; i < 40; ++i) escaped += "\\u00e9";
  longName.replace(longName.find("Fixture City"), 12, escaped);
  io = start("!weather Long"); Transport longLocation; longLocation.body(longName);
  const auto truncated = finish(perform(longLocation, io));
  assert(truncated.find("...[truncated]") != std::string::npos &&
         truncated.find("-1.25 C; code 3; age 120s") != std::string::npos);
  for (const char *code : {"weather_disabled", "provider_unavailable", "provider_timeout",
                           "provider_stale", "place_not_found", "unauthorized"}) {
    io = start("!weather City"); Transport peer;
    peer.body("{\"ok\":false,\"error\":{\"code\":\"" + std::string(code) + "\",\"message\":\"fixture error\"}}", 503);
    assert(finish(perform(peer, io)) == "Home weather error: " + std::string(code) + "; fixture error");
  }
  for (const std::string &bad : {std::string("{"), std::string(2049, 'x'),
                                std::string("{\"ok\":true,\"result\":{}}")}) {
    io = start("!weather City"); Transport peer; peer.body(bad);
    assert(finish(perform(peer, io)).find("invalid_response") != std::string::npos);
  }
  auto stale = weather; stale.replace(stale.find(":120}"), 5, ":7201}");
  io = start("!weather City"); Transport stalePeer; stalePeer.body(stale);
  assert(finish(perform(stalePeer, io)).find("invalid_response") != std::string::npos);
  io = start("!weather City"); Transport offline; offline.rejected = true;
  assert(finish(perform(offline, io)).find("transport_error") != std::string::npos);
  io = start("!weather City"); Transport timeout; timeout.stalled = true;
  assert(finish(perform(timeout, io)).find("timeout") != std::string::npos);
  io = start("!weather City"); Transport allowlist;
  auto config = configuration(); config.operations = BotHttpsConfig::Health;
  assert(finish(perform(allowlist, io, config)).find("permission_denied") != std::string::npos &&
         allowlist.opens == 0);
  for (const char *command : {"!weather", "!service weather", "!service health unwanted",
                              "!service echo", "!service other", "!service weather "}) {
    BotSession::Result result;
    assert(vm.start(++job, event(command), error, sizeof(error)) &&
           vm.poll(result) && result.ok && !strncmp(result.action.text, "Error:", 6) && !vm.nextIo(io));
  }
  for (unsigned mode = 0; mode < 4; ++mode) {
    auto e = event("!weather City");
    if (mode == 0) e.homeAccess = false;
    if (mode == 1) e.local = true;
    if (mode >= 2) { strcpy(e.channel, "#example1"); e.channelVerified = true; }
    if (mode == 3) { e.authenticated = false; strcpy(e.nickname, "owner"); }
    BotSession::Result result;
    assert(vm.start(++job, e, error, sizeof(error)) && vm.poll(result) && !result.ok && !vm.nextIo(io));
  }
  for (const auto &command : {"!weather " + std::string(81, 'p'),
                              "!service echo " + std::string(121, 'x')}) {
    BotSession::Result result;
    assert(vm.start(++job, event(command.c_str()), error, sizeof(error)) &&
           vm.poll(result) && !result.ok && !vm.nextIo(io));
  }
  io = start("!service health");
  BotIoResult malformed; malformed.token = io.token; malformed.ok = true; strcpy(malformed.value, "ok");
  memset(malformed.rpcCode, 'x', sizeof(malformed.rpcCode)); assert(!vm.complete(malformed));
  malformed.rpcCode[0] = 0; malformed.temperatureC = std::nan(""); assert(!vm.complete(malformed));
  malformed.temperatureC = 0;
  assert(finish(malformed) == "Home health: ok");
  const auto a = start("!weather City");
  assert(vm.start(++job, event("!service echo second", 2), error, sizeof(error)));
  BotIoRequest b; assert(vm.nextIo(b) && b.principal[0] == 2);
  b.deadline = 15100;
  Transport second; second.body("{\"ok\":true,\"result\":{\"text\":\"second\"}}");
  assert(finish(perform(second, b)) == "Home echo: second");
  vm.cancel(); BotSession::Result cancelled; assert(vm.poll(cancelled) && !cancelled.ok);
  Transport late; late.body(weather); const auto old = perform(late, a);
  assert(!vm.complete(old));
  assert(vm.load(BotDefaultSource, strlen(BotDefaultSource), 2, stats, error, sizeof(error)));
  assert(vm.start(a.token.job, event("!service health", 2), error, sizeof(error)) && vm.nextIo(io));
  assert(!vm.complete(old) && !vm.poll(cancelled));
  vm.cancel(); assert(vm.poll(cancelled) && !cancelled.ok);
  for (const char *name : {"weather", "service"}) {
    const auto source = "function " + std::string(name) + "() return 'override' end";
    assert(!vm.load(source.data(), source.size(), 3, stats, error, sizeof(error)));
  }
  puts("PASS bundled weather/service: native parsed results, escaped bounded locations, explicit errors, default/allowlist/privacy denial, overlapping principals and cancelled/recycled generation fences");
}
int main() {
  BotHttpsMeasurement metrics;
  metrics.record({UINT32_MAX - 5, 100, 80, 70, 60, 40, 30, 12, 0});
  metrics.record({4, 75, 50, 65, 45, 20, 25, 13, 1});
  metrics.record({14, 98, 79, 65, 59, 39, 25, 12, 0});
  assert(metrics.samples == 3 && metrics.maxGapMs == 10 && metrics.peakSockets == 13);
  assert(metrics.before.internal == 100 && metrics.minimum.internal == 75 && metrics.after.internal == 98);
  assert(metrics.minimum.dma == 45 && metrics.minimum.dmaLargest == 20 && metrics.socketErrors == 1);
  protocol(); responses(); fences(); coroutines(); hardwareFixture(); bundledCommands();
  assert(psram_test::allocations.empty());
  puts("PASS bounded HTTPS HTTP/JSON, errors, quotas, cancellation and coroutine attribution; no target TLS claim");
}
