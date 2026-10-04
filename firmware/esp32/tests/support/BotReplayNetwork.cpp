// SPDX-License-Identifier: Apache-2.0
#include "BotReplayNetwork.h"
#include "BotHttps.h"
#include "Clock.h"
#include <Arduino.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

namespace {
std::mutex lock;
std::string mode = "reject", body;
std::atomic<unsigned> submissions{0};
class ReplayTransport final : public onchip::BotHttpsTransport {
  std::string response, selected;
  size_t offset = 0;
  bool submitted = false;
public:
  bool open(const onchip::BotHttpsConfig &, char *error, size_t capacity) override {
    std::lock_guard<std::mutex> guard(lock);
    selected = mode; offset = 0; submitted = false;
    if (selected == "reject") {
      snprintf(error, capacity, "Local HTTPS fixture rejected connection before submission");
      return false;
    }
    response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
        std::to_string(body.size()) + "\r\n\r\n" + body;
    return true;
  }
  bool validate(char *, size_t) override { return true; }
  int write(const uint8_t *, size_t size) override {
    if (!submitted) { submitted = true; ++submissions; }
    return int(size);
  }
  int read(uint8_t *data, size_t size) override {
    if (selected == "hold") {
      std::lock_guard<std::mutex> guard(lock);
      if (mode == "hold") return 0;
      selected = mode;
      response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
          std::to_string(body.size()) + "\r\n\r\n" + body;
    }
    if (selected == "unknown" || selected == "reject") return -1;
    size = std::min(size, response.size() - offset);
    if (!size) return -1;
    memcpy(data, response.data() + offset, size); offset += size;
    return int(size);
  }
  void close() override {}
  uint32_t now() const override { return millis(); }
  void idle() override { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
};
}
namespace onchip {
BotHttpsTransport *createNativeBotHttpsTransport() { return new ReplayTransport; }
bool nativeBotHttpsClockTrusted() {
  uint32_t first, last;
  return trustedNetworkTime(first, last);
}
}
namespace bot_native_test {
bool replayNetwork(const std::string &directive, std::string &error) {
  if (directive == "setup" || directive == "setup-examples") {
    const auto configure = [&](const std::string &command) {
      char result[160]{};
      if (!onchip::botHttpsAdmin(command.c_str(), result, sizeof(result))) {
        error = result; return false;
      }
      return true;
    };
    const auto hex = [](const std::string &input) {
      std::string output;
      for (unsigned char c : input) {
        output += "0123456789abcdef"[c >> 4]; output += "0123456789abcdef"[c & 15];
      }
      return output;
    };
    const auto pem = hex("-----BEGIN CERTIFICATE-----\nlocal fixture\n-----END CERTIFICATE-----");
    const auto endpoint = [&](const std::string &name, const std::string &path, const char *method) {
      if (!configure("endpoint " + name + " 192.0.2.1 replay.invalid 443 " + path + " " + method))
        return false;
      for (size_t at = 0; at < pem.size(); at += 80)
        if (!configure("ca " + name + " " + pem.substr(at, 80))) return false;
      return configure("token " + name + " " + hex("local-fixture-token-not-a-credential"));
    };
    if (!endpoint("home", "/v1/rpc", "post") || !configure("ops home 7")) return false;
    if (directive == "setup-examples" &&
        (!endpoint("home_status", "/v1/example/status", "get") ||
         !endpoint("home_echo", "/v1/example/echo", "post") ||
         !endpoint("home_rpc", "/v1/rpc", "post") ||
         !configure("rpc home_rpc sum /v1/rpc") ||
         !configure("rpc home_rpc digest /v1/rpc"))) return false;
    return configure("commit");
  }
  std::lock_guard<std::mutex> guard(lock);
  if (directive.rfind("reply ", 0) == 0) {
    const auto next = directive.substr(6);
    if (next.empty() || next.size() > onchip::BotHttpsBodyLimit) {
      error = "Local HTTPS response requires 1..2048 bytes"; return false;
    }
    body = next; mode = "reply"; return true;
  }
  if (directive == "reject" || directive == "unknown" || directive == "hold") {
    mode = directive; return true;
  }
  error = "Use @network setup|setup-examples|reply JSON|reject|unknown|hold";
  return false;
}
unsigned replayNetworkSubmissions() { return submissions.load(); }
}
