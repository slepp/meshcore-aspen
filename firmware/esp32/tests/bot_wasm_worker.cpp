// SPDX-License-Identifier: Apache-2.0
#include "BotWorker.h"
#include "Clock.h"
#include <SPIFFS.h>
#include <nvs.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>
using namespace onchip;
unsigned long millis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}
void delay(unsigned long ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
static BotWorker::Result poll(BotWorker &worker, const char *operation = "invoke",
                              std::string *reply = nullptr) {
  for (unsigned i = 0; i < 5000; ++i) {
    BotIoRequest request;
    while (worker.pollRadio(request)) {
      assert(request.kind == BotIoRequest::Send && request.reply);
      if (reply) *reply = request.value;
      BotIoResult sent{};
      sent.token = request.token; sent.ok = sent.queued = sent.transmitted = true;
      assert(worker.completeRadio(sent));
    }
    BotWorker::Result result;
    if (worker.poll(result)) return result;
    delay(1);
  }
  fprintf(stderr, "Wasm worker timed out waiting for %s\n", operation);
  assert(false && "Wasm worker result timeout"); return {};
}
static BotEvent event(const char *command) {
  BotEvent e; char error[128]{};
  assert(parseBotCommand(command, strlen(command), e, error, sizeof(error)));
  e.authenticated = e.sharedState = true; e.sender[0] = 1;
  return e;
}
int main(int argc, char **argv) {
  setbuf(stdout, nullptr);
  assert(argc == 3);
  const std::string directory = std::string(argv[1]) + "/wasm-worker-" + std::to_string(getpid());
  assert(mkdir(directory.c_str(), 0700) == 0);
  assert(mkdir((directory + "/nvs").c_str(), 0700) == 0);
  assert(mkdir((directory + "/spiffs").c_str(), 0700) == 0);
  assert(native_nvs_init((directory + "/nvs").c_str()) == ESP_OK);
  assert(native_spiffs_init((directory + "/spiffs").c_str()));
  beginClocks(); beginNetworkClock(true); receiveNetworkTime(1767225600); loopClocks();
  uint8_t identity[32]{33};
  BotWorker worker; assert(worker.begin(identity));
  const auto stage = [&](const std::string &source) {
    assert(worker.stage(source.data(), source.size()) && poll(worker, "stage").ok);
    assert(worker.activate() && poll(worker, "activate").ok);
  };
  const auto module = [&](const char *name) {
    std::ifstream file(std::string(argv[2]) + "/" + name + ".wasm", std::ios::binary);
    assert(file); return std::string(std::istreambuf_iterator<char>(file), {});
  };
  stage("function lstart() sleep(20) kv.put('lua-events','seen','bot') end "
        "function lseen() reply(kv.get('lua-events','bot') or 'missing') end "
        "events.on('startup','lstart') events.on('node_status','lstart')");
  stage(module("c-dispatch"));
  assert(worker.setSharedState(true)); assert(worker.setEventAccess(9));
  BotEvent subscription; subscription.kind = BotEvent::Startup; subscription.sharedState = true;
  assert(worker.invoke(subscription, 10));
  auto result = poll(worker, "startup event"); assert(result.ok && result.operation == BotWorker::Operation::Event);
  const auto check = [&](const char *command, const char *text) {
    assert(worker.invoke(event(command), 11));
    std::string emitted;
    auto result = poll(worker, command, &emitted);
    if (!result.ok) fprintf(stderr, "%s: %s\n", command, result.error);
    const char *received = result.action.kind == BotAction::Reply ? result.action.text : emitted.c_str();
    if (strcmp(received, text)) fprintf(stderr, "%s: expected '%s', received '%s'\n", command, text, received);
    assert(result.ok && !strcmp(received, text));
  };
  check("!lseen", "seen"); check("!wseen", "seen");
  subscription.kind = BotEvent::NodeStatus;
  assert(worker.invoke(subscription, 12));
  assert(poll(worker, "node-status event").ok);
  check("!lseen", "seen"); check("!wseen", "seen");
  assert(worker.invoke(subscription, 13)); assert(worker.setEventAccess(0));
  assert(!poll(worker, "revoked event").ok && !worker.eventMask());
  check("!lseen", "seen"); check("!wseen", "seen");
  puts("PASS both runtimes receive shared event kinds; unique I/O token namespaces and revocation fences");
  stage(module("contract-3"));
  check("!wcontract", "= 42"); check("!lseen", "seen");
  stage(module("contract-7"));
  check("!wcontract", "utilities complete"); check("!lseen", "seen");
  stage(module("contract-0"));
  check("!wcontract", "conflict");
  stage(module("fault-0"));
  assert(worker.invoke(event("!wfault"), 20));
  assert(!poll(worker).ok);
  check("!lseen", "seen"); check("!ping", "Pong");
  {
    BotWorker validator; assert(validator.begin());
    const auto candidate = module("c-notes");
    assert(validator.stage(candidate.data(), candidate.size()) && poll(validator).ok);
    char error[128]{};
    const uint32_t generation = worker.generation();
    const uint32_t publication = worker.reserveSourcePublication(&validator, true, error, sizeof(error));
    assert(publication && !error[0] && worker.sourcePublicationCurrent(publication));
    assert(worker.generation() == generation);
    assert(!worker.stage("function other() return 'other' end", 34) &&
           !worker.activate() && !worker.removeWasm());
    uint8_t digest[32]{};
    assert(!worker.stageFile(candidate.size(), digest));
    assert(!worker.stage(candidate.data(), candidate.size(), publication + 1));
    check("!lseen", "seen"); check("!ping", "Pong");
    assert(worker.stage(candidate.data(), candidate.size(), publication) && poll(worker).ok);
    assert(worker.generation() == generation && worker.sourcePublicationCurrent(publication));
    assert(worker.activate(publication) && poll(worker).ok);
    assert(worker.generation() != generation && !worker.sourcePublicationCurrent(publication));
    worker.releaseSourcePublication(publication + 1);
    assert(!worker.removeWasm());
    worker.releaseSourcePublication(publication);
    check("!wnote durable", "Note saved"); check("!wnote", "durable");
    const char *conflict = "function wnote() return 'hijacked' end";
    assert(validator.stage(conflict, strlen(conflict)) && poll(validator).ok);
    const uint32_t retained = worker.generation();
    assert(!worker.reserveSourcePublication(&validator, false, error, sizeof(error)));
    assert(strstr(error, "!wnote") && strstr(error, "Wasm runtime"));
    assert(worker.generation() == retained);
    check("!wnote", "durable"); check("!lseen", "seen");
    BotWorker changed;
    assert(changed.begin(identity));
    const char *other = "function wnote() return 'new Lua namespace' end";
    assert(validator.stage(candidate.data(), candidate.size()) && poll(validator).ok);
    assert(changed.stage(other, strlen(other)) && poll(changed).ok);
    assert(changed.activate() && poll(changed).ok);
    const uint32_t changedGeneration = changed.generation();
    assert(!changed.reserveSourcePublication(&validator, true, error, sizeof(error)));
    assert(strstr(error, "!wnote") && strstr(error, "Lua runtime") &&
           changed.generation() == changedGeneration);
    const auto arithmetic = module("c-arithmetic");
    assert(validator.stage(arithmetic.data(), arithmetic.size()) && poll(validator).ok);
    const uint32_t stale = changed.reserveSourcePublication(&validator, true, error, sizeof(error));
    assert(stale);
    changed.stop();
    assert(changed.begin(identity) && !changed.sourcePublicationCurrent(stale));
    const uint32_t fresh = changed.reserveSourcePublication(&validator, true, error, sizeof(error));
    assert(fresh && fresh != stale);
    changed.releaseSourcePublication(stale);
    assert(!changed.stage(other, strlen(other)));
    changed.releaseSourcePublication(fresh);
    const auto concurrently = [](auto first, auto second) {
      std::atomic<unsigned> ready{0};
      const auto start = [&] {
        ready.fetch_add(1);
        while (ready.load() != 2) std::this_thread::yield();
      };
      std::thread left([&] { start(); first(); });
      std::thread right([&] { start(); second(); });
      left.join(); right.join();
    };
    for (unsigned i = 0; i < 24; ++i) {
      uint32_t left = 0, right = 0;
      char leftError[128]{}, rightError[128]{};
      concurrently(
          [&] { left = changed.reserveSourcePublication(&validator, true, leftError, sizeof(leftError)); },
          [&] { right = changed.reserveSourcePublication(&validator, true, rightError, sizeof(rightError)); });
      assert(bool(left) != bool(right));
      changed.releaseSourcePublication(left ? left : right);
      bool staged = false;
      left = 0;
      concurrently(
          [&] { left = changed.reserveSourcePublication(&validator, true, leftError, sizeof(leftError)); },
          [&] { staged = changed.stage(other, strlen(other)); });
      assert(bool(left) != staged);
      if (left) changed.releaseSourcePublication(left);
      else assert(poll(changed).ok);
    }
    changed.stop();
    validator.stop();
    puts("PASS publication reservation fences source mutations/generation, retains running jobs and rejects live namespace conflicts before publication");
  }
  assert(worker.removeWasm() && poll(worker).ok);
  check("!lseen", "seen");
  stage(module("c-dispatch"));
  stage("function patched_ping() return call_original('ping')..' regional' end "
        "override_command('ping','patched_ping') "
        "function marker() return 'marker' end command('marker','','Override marker')");
  check("!ping", "Pong regional");
  check("!marker", "marker");
  check("!help marker", "!marker : Override marker");
  check("!wseen", "seen");
  worker.stop(); native_spiffs_shutdown(); native_nvs_shutdown();
  std::filesystem::remove_all(directory);
  puts("PASS native Wasm utility/atomic outcomes, runaway recovery and independent Lua lifecycle");
}
