// SPDX-License-Identifier: Apache-2.0
#include "BotWorker.h"
#include "BotNetworkConfig.h"
#include "BotSettings.h"
#include <nvs.h>
#include <esp_heap_caps.h>
#include <SHA256.h>
#include <cassert>
#include <chrono>
#include <cstring>
#include <thread>
#include <functional>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
using namespace onchip;
static std::atomic<uint32_t> wallAdvance{0};
unsigned long millis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count() + wallAdvance.load();
}
void delay(unsigned long ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
struct Transport : BotHttpsTransport {
  bool validate(char *, size_t) override { return true; }
  std::atomic<unsigned> opens{0}, writes{0}, closes{0};
  std::atomic<bool> blocked{true};
  std::atomic<bool> holdIdle{false};
  std::atomic<uint32_t> clockAdvance{0};
  std::function<void()> onClose;
  std::string response =
      "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 36\r\n\r\n"
      "{\"ok\":true,\"result\":{\"status\":\"ok\"}}";
  size_t offset = 0;
  bool telemetry = false;
  std::vector<std::string> hosts;
  bool open(const BotHttpsConfig &config, char *, size_t) override {
    telemetry = !strcmp(config.host, "vm.test");
    hosts.emplace_back(config.host);
    offset = 0; ++opens; return true;
  }
  int write(const uint8_t *, size_t size) override { ++writes; return int(size); }
  int read(uint8_t *data, size_t size) override {
    if (blocked) return 0;
    static const std::string native = "HTTP/1.1 204 No Content\r\n\r\n";
    const auto &reply = telemetry ? native : response;
    size = std::min(size, reply.size() - offset);
    if (!size) return -1;
    memcpy(data, reply.data() + offset, size); offset += size;
    return int(size);
  }
  void close() override {
    ++closes;
    auto callback = std::move(onClose);
    if (callback) callback();
  }
  uint32_t now() const override { return millis() + clockAdvance; }
  void idle() override {
    while (holdIdle.load()) delay(1);
    delay(1);
  }
};
static void wait(const std::function<bool()> &ready) {
  const auto start = millis();
  while (!ready() && millis() - start < 3000) delay(1);
  assert(ready());
}
static BotWorker::Result poll(BotWorker &worker) {
  BotWorker::Result result;
  bool done = false;
  wait([&]() { return done || (done = worker.poll(result)); });
  return result;
}
static BotEvent event(const char *command, uint8_t sender = 1) {
  BotEvent event;
  char error[128];
  assert(parseBotCommand(command, strlen(command), event, error, sizeof(error)));
  event.authenticated = event.homeAccess = true;
  event.sender[0] = sender;
  return event;
}
static void dataRestoreWorkspace() {
  BotIoResult reset;
  memset(static_cast<void *>(&reset), 0x41, sizeof(reset));
  resetBotIoResult(reset);
  assert(!reset.ok && !reset.networkSubmitted && !reset.httpStatus);
  assert(reset.outcome == BotIoResult::Rejected && reset.packet.path.width == 1);
  assert(reset.timerState == BotTimerState::Missing && reset.reminderState == BotReminderState::Missing);
  for (char byte : reset.json) assert(!byte);
  const size_t persistent = psram_test::allocations.size();
  uint8_t identity[32]{31};
  BotWorker worker;
  assert(worker.begin(identity, nullptr, nullptr, true));
  const auto command = [&](const std::string &text) {
    char reply[192]{};
    worker.dataCommand(text.c_str(), reply, sizeof(reply));
    return std::string(reply);
  };
  const auto status = [&](const char *prefix) {
    std::string reply;
    wait([&] {
      reply = command("status");
      return !reply.compare(0, strlen(prefix), prefix);
    });
    return reply;
  };
  assert(command("export bot " + std::string(64, '0')).find("PENDING") == 0);
  const auto exported = status("EXPORTED ");
  const std::string hash = exported.substr(9, 64), id = hash.substr(0, 16);
  std::vector<std::string> chunks;
  for (unsigned i = 0; i < 52; ++i) {
    const auto reply = command("read " + id + " " + std::to_string(i));
    if (reply == "EOF") break;
    assert(reply.find("DATA ") == 0);
    chunks.push_back(reply.substr(5));
  }
  const auto stage = [&] {
    assert(command("begin " + id + " " + hash).find("UPLOADING") == 0);
    for (unsigned i = 0; i < chunks.size(); ++i)
      assert(command("chunk " + id + " " + std::to_string(i) + " " + chunks[i]).find("RECEIVED") == 0);
    assert(command("stage " + id).find("PENDING") == 0);
    status("STAGED");
  };
  stage();
  psram_test::failAfter = 0;
  assert(command("restore " + id).find("PENDING") == 0);
  assert(status("REJECTED").find("data restore result storage unavailable") != std::string::npos);
  psram_test::failAfter = -1;
  stage();
  assert(command("restore " + id).find("PENDING") == 0);
  status("COMMITTED");
  const unsigned allocated = psram_test::attempts;
  stage();
  assert(command("restore " + id).find("PENDING") == 0);
  status("COMMITTED");
  assert(psram_test::attempts == allocated);
  worker.stop();
  assert(psram_test::allocations.size() == persistent);
  puts("PASS restore result: denied PSRAM leaves data unmodified, successful retry reuses one workspace, stop releases it");
}
static void radioRejection() {
  uint8_t identity[32]{32};
  BotWorker worker;
  assert(worker.begin(identity));
  const char *source = "function attempt() reply('probe') end";
  assert(worker.stage(source, strlen(source)) && poll(worker).ok);
  assert(worker.activate() && poll(worker).ok);
  assert(worker.invoke(event("!attempt"), 32));
  BotIoRequest request;
  bool acquired = false;
  wait([&] { return acquired || (acquired = worker.pollRadio(request)); });
  auto stale = request.token;
  ++stale.operation;
  assert(!worker.rejectRadio(stale, "Wrong operation"));
  assert(worker.rejectRadio(request.token, "Radio request cancelled before admission"));
  assert(!worker.rejectRadio(request.token, "Duplicate completion"));
  const auto result = poll(worker);
  assert(result.job == 32 && !result.ok &&
         strstr(result.error, "Radio request cancelled before admission"));
  worker.stop();
  puts("PASS radio rejection: exact token ownership, one completion and retained reason without dispatch-stack result");
}
static void bundledWorker() {
  Transport transport;
  BotHttpsConfig config{"192.0.2.1", "home.example",
      "-----BEGIN CERTIFICATE-----\nfixture\n-----END CERTIFICATE-----",
      "0123456789abcdef0123456789abcdef", 443, 7};
  const std::string body =
      "{\"ok\":true,\"result\":{\"source\":\"open-meteo\",\"location\":\"Fixture City\","
      "\"country\":\"Testland\",\"temperature_c\":12.5,\"weather_code\":3,"
      "\"observed_at\":\"2026-09-28T12:00:00Z\",\"source_age_seconds\":900}}";
  transport.response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
      std::to_string(body.size()) + "\r\n\r\n" + body;
  uint8_t identity[32]{9};
  BotWorker worker;
  assert(worker.begin(identity, &transport, &config));
  assert(worker.stage(BotDefaultSource, strlen(BotDefaultSource)) && poll(worker).ok);
  assert(worker.activate() && poll(worker).ok);
  worker.setHomeAccess(true);
  assert(worker.setReminderAccess(true));
  assert(worker.invoke(event("!weather Fixture City"), 10));
  wait([&] { return transport.writes.load() >= 2; });
  const auto local = [&](const char *command, const char *expected) {
    assert(worker.invoke(event(command, 2), 11));
    const auto result = poll(worker);
    assert(result.ok && result.job == 11 && !strcmp(result.action.text, expected));
  };
  local("!ping", "Pong");
  local("!calc 6*7", "= 42");
  local("!remember location private note", "Note location created; committed");
  local("!recall location", "private note");
  local("!notes", "Notes (1): [location]");
  local("!reminders", "No personal reminders");
  transport.blocked = false;
  auto result = poll(worker);
  assert(result.ok && result.job == 10 &&
         !strcmp(result.action.text, "Weather Fixture City: 12.5 C; code 3; age 900s"));
  wait([&] { return transport.closes.load() == 1; });
  transport.blocked = true;
  assert(worker.invoke(event("!service weather Fixture City", 2), 12));
  wait([&] { return transport.writes.load() >= 4; });
  worker.setHomeAccess(false); worker.setHomeAccess(true);
  result = poll(worker);
  assert(result.ok && result.job == 12 && strstr(result.action.text, "unknown"));
  wait([&] { return transport.closes.load() == 2; });
  assert(worker.invoke(event("!weather Fixture City", 3), 13));
  wait([&] { return transport.writes.load() >= 6; });
  const char *replacement = "function hello() return 'replacement' end";
  assert(worker.stage(replacement, strlen(replacement)));
  assert(poll(worker).operation == BotWorker::Operation::Stage);
  assert(worker.activate());
  bool activated = false, cancelled = false;
  for (unsigned i = 0; i < 2; ++i) {
    result = poll(worker);
    if (result.operation == BotWorker::Operation::Activate) activated = result.ok;
    else cancelled = result.job == 13 && !result.ok;
  }
  assert(activated && cancelled);
  wait([&] { return transport.closes.load() == 3; });
  local("!hello", "replacement");
  local("!recall location", "private note");
  assert(worker.invoke(event("!service weather Fixture City", 4), 14));
  wait([&] { return transport.writes.load() >= 8; });
  transport.clockAdvance = BotHttpsDeadlineMs + 1;
  result = poll(worker);
  assert(result.ok && result.job == 14 && strstr(result.action.text, "timeout"));
  wait([&] { return transport.closes.load() == 4; });
  worker.stop();
  puts("PASS bundled RPC worker: pending weather alongside local ping/utilities/notes/reminders, returned source data, grant revoke, source replacement and timeout");
}
static void serviceTxHandoff() {
  Transport transport;
  BotHttpsConfig config{"192.0.2.1", "home.example",
      "-----BEGIN CERTIFICATE-----\nfixture\n-----END CERTIFICATE-----",
      "0123456789abcdef0123456789abcdef", 443, 7};
  uint8_t identity[32]{10};
  BotWorker worker;
  assert(worker.begin(identity, &transport, &config));
  const char *source =
      "function deliver() local tx=reply(service('health')) "
      "if not tx.queued or not tx.transmitted or tx.acknowledged then return 'BAD TX' end end";
  assert(worker.stage(source, strlen(source)) && poll(worker).ok);
  assert(worker.activate() && poll(worker).ok);
  worker.setHomeAccess(true);
  BotIoResult previous;
  for (unsigned i = 0; i < 2; ++i) {
    transport.blocked = true;
    assert(worker.invoke(event("!deliver", i + 1), 20 + i));
    wait([&] { return transport.writes.load() >= 2 * (i + 1); });
    BotWorker::Result result;
    BotIoRequest radio;
    assert(!worker.poll(result) && !worker.pollRadio(radio));
    transport.blocked = false;
    bool acquired = false;
    wait([&] { return acquired || (acquired = worker.pollRadio(radio)); });
    assert(radio.kind == BotIoRequest::Send && radio.reply && radio.token.job == 20 + i &&
           !strcmp(radio.value, "Home health: ok"));
    assert(!worker.poll(result)); // Native queue ownership is not terminal TX.
    if (i) assert(!worker.completeRadio(previous));
    BotIoResult tx; tx.token = radio.token; tx.radioJob = 123 + i; tx.queued = true;
    tx.ok = tx.transmitted = i == 0;
    if (i) strcpy(tx.error, "Radio TX failed; outcome unknown");
    assert(worker.completeRadio(tx));
    result = poll(worker);
    assert(result.job == 20 + i && result.ok == (i == 0) && result.action.kind == BotAction::None);
    if (i) assert(strstr(result.error, "Radio TX failed"));
    previous = tx;
  }
  worker.stop();
  puts("PASS service TX handoff: RPC success precedes owned radio queue/terminal TX, no ACK/delivery claim, failed/late completion isolation");
}
static void nativeTelemetryWorker() {
  const auto command = [](const std::string &text) {
    char reply[163];
    telemetryEndpointCommand(text.c_str(), reply, sizeof(reply));
    assert(strncmp(reply, "Error:", 6));
  };
  command("address 192.0.2.2"); command("host vm.test"); command("path /write");
  const char *ca = "-----BEGIN CERTIFICATE-----\nfixture\n-----END CERTIFICATE-----";
  std::string hex;
  for (const char *p = ca; *p; ++p) {
    char pair[3]; snprintf(pair, sizeof(pair), "%02x", uint8_t(*p)); hex += pair;
  }
  command("ca " + hex); command("commit");
  const size_t persistent = psram_test::allocations.size();
  const char *body = "meshcore_device,device=host-test uptime_seconds=1\n";
  Transport transport;
  BotHttpsConfig home{"192.0.2.1", "home.example", ca,
                      "0123456789abcdef0123456789abcdef", 443, 7};
  uint8_t identity[32]{19};
  BotWorker worker;
  assert(worker.begin(identity, &transport, &home));
  const char *source = "function query() local r=rpc.call('home','health',{}) "
                       "if r.ok then return 'ok' end return r.error.code end";
  assert(worker.stage(source, strlen(source)) && poll(worker).ok);
  assert(worker.activate() && poll(worker).ok);
  worker.setHomeAccess(true);
  assert(worker.invoke(event("!query", 1), 101));
  wait([&] { return transport.opens == 1; });
  assert(worker.jobsInUse() == 1);
  assert(worker.invoke(event("!query", 2), 102));
  assert(worker.submitTelemetry(body, strlen(body)));
  assert(!worker.submitTelemetry(body, strlen(body)));
  transport.blocked = false;
  bool done = false; TelemetryCompletion result;
  wait([&] { return done || (done = worker.pollTelemetry(result)); });
  assert(result.ok && result.httpStatus == 204);
  auto first = poll(worker), second = poll(worker);
  assert(first.ok && second.ok && first.job != second.job);
  assert(worker.jobsInUse() == 0);
  assert(transport.hosts.size() == 3 && transport.hosts[0] == "home.example" &&
         transport.hosts[1] == "vm.test" && transport.hosts[2] == "home.example");
  assert(transport.closes == 3);
  worker.setHomeAccess(false);
  transport.blocked = true;
  assert(worker.submitTelemetry(body, strlen(body)));
  wait([&] { return transport.opens == 4; });
  assert(worker.invoke(event("!ping", 3), 103));
  auto ping = poll(worker);
  assert(ping.ok && ping.job == 103 && !strcmp(ping.action.text, "Pong"));
  worker.cancelTelemetry();
  done = false;
  wait([&] { return done || (done = worker.pollTelemetry(result)); });
  assert(!result.ok && result.error == TelemetryError::Cancelled && transport.closes == 4);
  assert(worker.submitTelemetry(body, strlen(body)));
  wait([&] { return transport.opens == 5; });
  worker.stop();
  assert(transport.closes == 5 && psram_test::allocations.size() == persistent);
  Transport native;
  native.blocked = false;
  assert(worker.begin(nullptr, &native, nullptr, true) && worker.ensureNativeHttps());
  assert(worker.submitTelemetry(body, strlen(body)));
  done = false;
  wait([&] { return done || (done = worker.pollTelemetry(result)); });
  assert(result.ok && native.opens == 1 && native.closes == 1);
  worker.stop();
  assert(psram_test::allocations.size() == persistent);
  puts("PASS native-only POST: one shared transport, fair RPC/telemetry/RPC, local ping, separate grant, bounded slot, cancellation, no bot identity, cleanup");
}
static void runtimeHomeWorker() {
  const auto stage = [](const std::string &text) {
    char reply[160]{};
    assert(botHttpsAdmin(text.c_str(), reply, sizeof(reply)));
  };
  const auto hex = [](const std::string &bytes) {
    std::string result;
    const char digits[] = "0123456789abcdef";
    for (unsigned char byte : bytes) {
      result.push_back(digits[byte >> 4]); result.push_back(digits[byte & 15]);
    }
    return result;
  };
  stage("endpoint home 192.0.2.1 home.example 443 /v1/rpc post");
  const auto pem = hex("-----BEGIN CERTIFICATE-----\nfixture\n-----END CERTIFICATE-----");
  for (size_t offset = 0; offset < pem.size(); offset += 80)
    stage("ca home " + pem.substr(offset, 80));
  stage("token home " + hex("0123456789abcdef0123456789abcdef"));
  stage("ops home 1");
  stage("endpoint package 192.0.2.1 home.example 443 /v1/example/status get");
  for (size_t offset = 0; offset < pem.size(); offset += 80)
    stage("ca package " + pem.substr(offset, 80));
  stage("token package " + hex("0123456789abcdef0123456789abcdef"));
  stage("commit");
  assert(!botHttpsConfig().valid() && botNetworkHomeConfigured());
  Transport transport;
  transport.blocked = false;
  uint8_t identity[32]{11};
  BotWorker worker;
  assert(worker.begin(identity, &transport));
  assert(worker.stage(BotDefaultSource, strlen(BotDefaultSource)) && poll(worker).ok);
  assert(worker.activate() && poll(worker).ok);
  worker.setHomeAccess(true);
  assert(worker.invoke(event("!service health"), 32));
  auto result = poll(worker);
  assert(result.ok && result.job == 32 && !strcmp(result.action.text, "Home health: ok"));
  assert(!worker.requestOwnerFetch("https://unapproved/") &&
         worker.requestOwnerFetch("package"));
  BotIoResult fetched;
  bool fetchedDone = false;
  wait([&] { return fetchedDone || (fetchedDone = worker.pollOwnerFetch(fetched)); });
  assert(fetched.ok && !strcmp(fetched.json, "{\"ok\":true,\"result\":{\"status\":\"ok\"}}"));
  transport.blocked = true;
  assert(worker.requestOwnerFetch("package"));
  wait([&] { return transport.writes.load() >= 4; });
  worker.cancelOwnerFetch();
  assert(worker.pollOwnerFetch(fetched) && !fetched.ok &&
         !strcmp(fetched.rpcCode, "cancelled") && !fetched.json[0]);
  wait([&] { return transport.closes.load() >= 3; });
  assert(!worker.pollOwnerFetch(fetched));
  assert(worker.invoke(event("!service health"), 33));
  wait([&] { return transport.writes.load() >= 6; });
  assert(worker.requestOwnerFetch("package"));
  assert(transport.opens.load() == 4 && !worker.pollOwnerFetch(fetched));
  stage("endpoint package 192.0.2.1 home.example 443 /v1/example/updated get");
  for (size_t offset = 0; offset < pem.size(); offset += 80)
    stage("ca package " + pem.substr(offset, 80));
  stage("token package " + hex("0123456789abcdef0123456789abcdef"));
  stage("commit");
  result = poll(worker);
  assert(result.ok && result.job == 33 &&
         strstr(result.action.text, "unknown"));
  fetchedDone = false;
  wait([&] { return fetchedDone || (fetchedDone = worker.pollOwnerFetch(fetched)); });
  assert(!fetched.ok && !strcmp(fetched.rpcCode, "permission_denied") &&
         !fetched.json[0] && transport.opens.load() == 4);
  worker.stop();
  Transport malformed;
  malformed.blocked = false;
  malformed.response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
      "Content-Length: 1\r\n\r\n!";
  malformed.onClose = [&] {
    stage("ops home 3");
    stage("commit");
  };
  BotWorker next;
  assert(next.begin(identity, &malformed));
  assert(next.stage(BotDefaultSource, strlen(BotDefaultSource)) && poll(next).ok);
  assert(next.activate() && poll(next).ok);
  next.setHomeAccess(true);
  assert(next.invoke(event("!service health"), 34));
  result = poll(next);
  assert(result.ok && result.job == 34 && strstr(result.action.text, "unknown"));
  assert(malformed.opens.load() == 1 && malformed.closes.load() >= 1);
  next.stop();
  puts("PASS single HTTPS socket: active RPC + queued owner GET + live edit; submitted/malformed completion unknown, unsent fetch denied, local cancellations immediate");
}
#if defined(ONCHIP_BOT_OWNER_FETCH_TEST)
struct PackageSink : BotHttpsBodySink {
  std::string bytes;
  bool committed = false, aborted = false;
  unsigned begins = 0;
  bool begin(uint32_t size) override { ++begins; return size == BotSourceLimit; }
  bool write(const uint8_t *data, size_t size) override {
    bytes.append(reinterpret_cast<const char *>(data), size); return true;
  }
  bool commit(uint32_t size, const uint8_t[32]) override {
    committed = size == bytes.size(); return committed;
  }
  void abort(const char *) override { aborted = true; bytes.clear(); }
};
static void packageWorker() {
  const std::string source(BotSourceLimit, 'x');
  SHA256 hash;
  uint8_t digest[32]{};
  hash.update(source.data(), source.size());
  hash.finalize(digest, sizeof(digest));
  const auto request = [&](uint32_t id, PackageSink &sink) {
    BotHttpsFetchRequest fetch;
    fetch.id = id; fetch.sink = &sink;
    memcpy(fetch.expectedSha256, digest, sizeof(digest));
    return fetch;
  };
  uint8_t identity[32]{14};
  Transport held;
  BotWorker worker;
  assert(worker.begin(identity, &held));
  assert(worker.stage(BotDefaultSource, strlen(BotDefaultSource)) && poll(worker).ok);
  assert(worker.activate() && poll(worker).ok);
  worker.setHomeAccess(true);
  assert(worker.invoke(event("!service health"), 40));
  wait([&] { return held.writes.load() >= 2; });
  PackageSink denied;
  const auto pending = request(41, denied);
  assert(worker.fetchPackage(pending) && !worker.fetchPackage(pending));
  BotHttpsFetchResult fetched;
  assert(worker.pollPackageFetch(fetched) &&
         fetched.id == 41 && fetched.state == BotHttpsFetchResult::Queued);
  char reply[160]{};
  assert(botHttpsAdmin("ops home 1", reply, sizeof(reply)));
  assert(botHttpsAdmin("commit", reply, sizeof(reply)));
  const auto rpc = poll(worker);
  assert(rpc.ok && rpc.job == 40 && strstr(rpc.action.text, "unknown"));
  bool complete = false;
  wait([&] {
    if (complete) return true;
    if (!worker.pollPackageFetch(fetched)) return false;
    complete = fetched.state != BotHttpsFetchResult::Queued &&
               fetched.state != BotHttpsFetchResult::Running;
    return complete;
  });
  assert(fetched.id == 41 && fetched.state == BotHttpsFetchResult::Failed &&
         !strcmp(fetched.rpcCode, "permission_denied") && !fetched.networkSubmitted &&
         denied.aborted && !denied.committed && !denied.begins && held.opens == 1);
  assert(!worker.pollPackageFetch(fetched));
  worker.stop();

  Transport transport;
  transport.blocked = false;
  transport.response = "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
      "Content-Length: 4096\r\n\r\n" + source;
  BotWorker fresh;
  assert(fresh.begin(identity, &transport));
  fresh.setHomeAccess(true);
  PackageSink sink;
  const auto actual = request(42, sink);
  assert(fresh.fetchPackage(actual));
  complete = false;
  wait([&] {
    if (complete) return true;
    if (!fresh.pollPackageFetch(fetched)) return false;
    complete = fetched.state != BotHttpsFetchResult::Queued &&
               fetched.state != BotHttpsFetchResult::Running;
    return complete;
  });
  assert(fetched.id == 42 && fetched.state == BotHttpsFetchResult::Complete &&
         fetched.bytes == BotSourceLimit && fetched.httpStatus == 200 &&
         fetched.networkSubmitted && !memcmp(fetched.sha256, digest, 32) &&
         sink.committed && !sink.aborted && sink.bytes == source &&
         transport.opens == 1 && transport.closes >= 1);
  transport.onClose = [&] {
    assert(botHttpsAdmin("ops home 3", reply, sizeof(reply)));
    assert(botHttpsAdmin("commit", reply, sizeof(reply)));
  };
  PackageSink late;
  assert(fresh.fetchPackage(request(43, late)));
  complete = false;
  wait([&] {
    if (complete) return true;
    if (!fresh.pollPackageFetch(fetched)) return false;
    complete = fetched.state != BotHttpsFetchResult::Queued &&
               fetched.state != BotHttpsFetchResult::Running;
    return complete;
  });
  assert(fetched.id == 43 && fetched.state == BotHttpsFetchResult::Unknown &&
         !strcmp(fetched.rpcCode, "unknown") && fetched.networkSubmitted &&
         late.committed && late.aborted && late.bytes.empty() &&
         transport.opens == 2);
  fresh.stop();

  Transport cancelledTransport;
  BotWorker interrupted;
  assert(interrupted.begin(identity, &cancelledTransport));
  assert(interrupted.stage(BotDefaultSource, strlen(BotDefaultSource)) &&
         poll(interrupted).ok);
  assert(interrupted.activate() && poll(interrupted).ok);
  interrupted.setHomeAccess(true);
  PackageSink cancelled;
  assert(interrupted.fetchPackage(request(44, cancelled)));
  wait([&] { return cancelledTransport.writes.load() >= 1; });
  assert(interrupted.invoke(event("!ping", 2), 45));
  const auto local = poll(interrupted);
  assert(local.ok && local.job == 45 && !strcmp(local.action.text, "Pong"));
  interrupted.cancelPackageFetch();
  complete = false;
  wait([&] {
    if (complete) return true;
    if (!interrupted.pollPackageFetch(fetched)) return false;
    complete = fetched.state != BotHttpsFetchResult::Queued &&
               fetched.state != BotHttpsFetchResult::Running;
    return complete;
  });
  assert(fetched.id == 44 && fetched.state == BotHttpsFetchResult::Unknown &&
         !strcmp(fetched.rpcCode, "cancelled") && !cancelled.committed &&
         cancelled.aborted && cancelledTransport.opens == 1);
  assert(!interrupted.pollPackageFetch(fetched));
  interrupted.stop();
  puts("PASS 4096-byte package GET on owner slot: shared-socket RPC/edit, verified stream, post-commit unknown cleanup, cancel/late-result isolation and local progress");
}
#endif
static void queuedDeadlines() {
  uint8_t identity[32]{12};
  for (unsigned active = 0; active < 2; ++active) {
    Transport transport;
    transport.holdIdle = true;
    BotWorker worker;
    assert(worker.begin(identity, &transport));
    assert(worker.stage(BotDefaultSource, strlen(BotDefaultSource)) && poll(worker).ok);
    assert(worker.activate() && poll(worker).ok);
    worker.setHomeAccess(true);
    if (active == 0) {
      assert(worker.requestOwnerFetch("package"));
      wait([&] { return transport.writes.load() >= 1; });
      assert(worker.invoke(event("!service health"), 35));
      assert(worker.invoke(event("!ping", 2), 36));
      const auto local = poll(worker);
      assert(local.ok && local.job == 36 && !strcmp(local.action.text, "Pong"));
    } else {
      assert(worker.invoke(event("!service health"), 37));
      wait([&] { return transport.writes.load() >= 2; });
      assert(worker.requestOwnerFetch("package"));
    }
    wallAdvance = BotHttpsDeadlineMs + 1;
    if (active == 0) {
      const auto queued = poll(worker);
      assert(queued.ok && queued.job == 35 && strstr(queued.action.text, "timeout"));
    } else {
      BotIoResult queued;
      bool complete = false;
      wait([&] { return complete || (complete = worker.pollOwnerFetch(queued)); });
      assert(!queued.ok && !queued.networkSubmitted &&
             !strcmp(queued.rpcCode, "timeout"));
    }
    assert(transport.opens.load() == 1);
    transport.holdIdle = false;
    if (active == 0) {
      BotIoResult fetched;
      bool complete = false;
      wait([&] { return complete || (complete = worker.pollOwnerFetch(fetched)); });
      assert(!fetched.ok && !strcmp(fetched.rpcCode, "timeout"));
    } else {
      const auto running = poll(worker);
      assert(running.ok && running.job == 37 && strstr(running.action.text, "timeout"));
    }
    worker.stop();
    wallAdvance = 0;
  }
  puts("PASS pending Lua RPC and owner GET deadlines expire without waiting for active shared HTTPS socket");
}
#if ONCHIP_BOT_WASM
static void wasmConfiguredHome(const char *directory) {
  char reply[128]{};
  assert(botHttpsAdmin("ops home 7", reply, sizeof(reply)));
  assert(botHttpsAdmin("commit", reply, sizeof(reply)));
  Transport transport;
  transport.blocked = false;
  const std::string json = "{\"ok\":true,\"result\":{\"text\":\"hello\"}}";
  transport.response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
      std::to_string(json.size()) + "\r\n\r\n" + json;
  uint8_t identity[32]{44};
  BotWorker worker; assert(worker.begin(identity, &transport));
  worker.setHomeAccess(true);
  unsigned sender = 41;
  for (const char *name : {"c-rpc", "rust-rpc", "contract-5"}) {
    std::ifstream file(std::string(directory) + "/" + name + ".wasm", std::ios::binary);
    assert(file);
    const std::string module(std::istreambuf_iterator<char>(file), {});
    assert(worker.stage(module.data(), module.size()) && poll(worker).ok);
    assert(worker.activate() && poll(worker).ok);
    const bool named = !strcmp(name, "contract-5");
    const unsigned id = sender++;
    assert(worker.invoke(event(named ? "!wcontract" : name[0] == 'c' ? "!wecho hello" : "!recho hello",
                               id), id));
    const auto result = poll(worker);
    if (!result.ok || strcmp(result.action.text, named ? "ok" : "hello"))
      fprintf(stderr, "%s configured home: %s / %s\n", name, result.error, result.action.text);
    assert(result.ok && !strcmp(result.action.text, named ? "ok" : "hello"));
  }
  assert(transport.opens.load() == 3);
  worker.stop();
  Transport arguments; arguments.blocked = false;
  BotWorker named; assert(named.begin(identity, &arguments));
  named.setHomeAccess(true);
  std::ifstream file(std::string(directory) + "/c-rpc-args.wasm", std::ios::binary);
  assert(file);
  const std::string module(std::istreambuf_iterator<char>(file), {});
  assert(named.stage(module.data(), module.size()) && poll(named).ok);
  assert(named.activate() && poll(named).ok);
  struct Case { const char *command, *body; };
  const Case cases[] = {
    {"!warg_health {}", "{\"ok\":true,\"result\":{\"status\":\"ok\"}}"},
    {"!warg_echo {\"text\":\"hello\"}", "{\"ok\":true,\"result\":{\"text\":\"hello\"}}"},
    {"!warg_weather {\"place\":\"Paris\"}",
      "{\"ok\":true,\"result\":{\"source\":\"open-meteo\",\"location\":\"Paris\","
      "\"country\":\"FR\",\"temperature_c\":12.5,\"weather_code\":3,"
      "\"observed_at\":\"2025-01-02T03:04:05Z\",\"source_age_seconds\":60}}"},
  };
  for (const auto &test : cases) {
    arguments.response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
        std::to_string(strlen(test.body)) + "\r\n\r\n" + test.body;
    const unsigned id = sender++;
    assert(named.invoke(event(test.command, id), id));
    const auto result = poll(named);
    assert(result.ok && !strcmp(result.action.text, "ok"));
  }
  assert(arguments.opens.load() == 3);
  assert(named.invoke(event("!warg_echo {\"text\":\"a\",\"extra\":1}", sender), sender));
  const auto rejected = poll(named);
  assert(!rejected.ok && strstr(rejected.error, "Home RPC") && arguments.opens.load() == 3);
  named.stop();
  puts("PASS actual C/Rust legacy and descriptor Wasm health/echo/weather through configured native home; invalid arguments never open HTTPS");
}
#endif
int main(int argc, char **argv) {
  bool enabled = true;
  assert(loadBotHomeAccess(enabled) && !enabled);
  const auto journal = identity_test::durable;
  assert(saveBotHomeAccess(true) && loadBotHomeAccess(enabled) && enabled);
  identity_test::failCommit = true;
  assert(!saveBotHomeAccess(false));
  identity_test::failCommit = false;
  assert(loadBotHomeAccess(enabled) && enabled);
  assert(saveBotHomeAccess(false));
  identity_test::durable = journal;

  {
    Transport transport;
    BotHttpsConfig config{"192.0.2.1", "home.example",
        "-----BEGIN CERTIFICATE-----\nfixture\n-----END CERTIFICATE-----",
        "0123456789abcdef0123456789abcdef", 443, 7};
    uint8_t identity[32] = {1};
    BotWorker worker;
    assert(worker.begin(identity, &transport, &config));
    worker.setHomeAccess(true);
    const char *source =
        "function query() local key=ctx.sender.public_key local r=rpc.call('home','health',{}) "
        "if ctx.sender.public_key~=key then return 'WRONG' end "
        "if r.ok then return r.result.status else return r.error.code end end "
        "function later() sleep(40) return query() end";
    assert(worker.stage(source, strlen(source)) && poll(worker).ok);
    assert(worker.activate() && poll(worker).ok);
    assert(worker.invoke(event("!query"), 1));
    wait([&]() { return transport.writes.load() >= 2; });
    assert(worker.invoke(event("!ping", 2), 2));
    auto result = poll(worker);
    assert(result.ok && result.job == 2 && !strcmp(result.action.text, "Pong"));
    worker.setHomeAccess(false);
    worker.setHomeAccess(true);
    result = poll(worker);
    assert(result.ok && result.job == 1 && !strcmp(result.action.text, "unknown"));
    wait([&]() { return transport.closes.load() == 1; });
    // Revocation before a delayed helper must not borrow a newly granted epoch.
    assert(worker.invoke(event("!later", 3), 3));
    delay(10);
    worker.setHomeAccess(false);
    worker.setHomeAccess(true);
    result = poll(worker);
    assert(result.ok && result.job == 3 && !strcmp(result.action.text, "unavailable"));
    assert(transport.opens == 1);
    transport.blocked = false;
    assert(worker.invoke(event("!query", 2), 4));
    result = poll(worker);
    assert(result.ok && result.job == 4 && !strcmp(result.action.text, "ok"));
    wait([&]() { return transport.closes.load() == 2; });
    transport.blocked = true;
    assert(worker.invoke(event("!query", 4), 5));
    wait([&]() { return transport.opens.load() == 3; });
    assert(worker.stage(source, strlen(source)));
    assert(poll(worker).operation == BotWorker::Operation::Stage);
    assert(worker.activate());
    bool activated = false, cancelled = false;
    for (unsigned i = 0; i < 2; ++i) {
      result = poll(worker);
      if (result.operation == BotWorker::Operation::Activate) activated = result.ok;
      else cancelled = result.job == 5 && !result.ok;
    }
    bundledWorker();
    serviceTxHandoff();
    assert(activated && cancelled);
    wait([&]() { return transport.closes.load() == 3; });
    worker.stop();
  }
  assert(psram_test::allocations.empty());
  nativeTelemetryWorker();
  runtimeHomeWorker();
  dataRestoreWorkspace();
  radioRejection();
#if defined(ONCHIP_BOT_OWNER_FETCH_TEST)
  packageWorker();
#endif
  queuedDeadlines();
#if ONCHIP_BOT_WASM
  if (argc > 1) wasmConfiguredHome(argv[1]);
#endif
  puts("PASS separate HTTPS worker, concurrent diagnostic, live grant epoch, source cancellation and policy persistence");
}
