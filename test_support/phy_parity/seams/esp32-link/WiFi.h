#pragma once
#include <Stream.h>
#include <string>
#include <vector>

constexpr int WIFI_STA = 1, WL_CONNECTED = 3;
constexpr int WIFI_POWER_8_5dBm = 34, WIFI_POWER_5dBm = 20;
namespace link_test {
inline std::vector<std::string> calls;
inline int failure = 0;
inline int power = -1;
inline int connects = 0;
}
struct WiFiClass {
  std::string hostname;
  void persistent(bool enabled) {
    link_test::calls.push_back(enabled ? "persistent" : "volatile");
  }
  bool setHostname(const char* name) {
    link_test::calls.push_back("hostname");
    hostname = name;
    return link_test::failure != 1;
  }
  const char* getHostname() const {
    if (link_test::failure == 2) return "wrong-name";
    if (link_test::failure == 3) return nullptr;
    return hostname.c_str();
  }
  bool mode(int mode) {
    link_test::calls.push_back(mode == WIFI_STA ? "station" : "invalid-mode");
    return link_test::failure != 4;
  }
  void setSleep(bool enabled) { link_test::calls.push_back(enabled ? "sleep" : "awake"); }
  void setAutoReconnect(bool enabled) { link_test::calls.push_back(enabled ? "reconnect" : "manual"); }
  void begin(const char*, const char*) { link_test::calls.push_back("associate"); }
  int status() const { return WL_CONNECTED; }
};
inline WiFiClass WiFi;
struct WiFiClient : Stream {
  bool active = false;
  bool connected() const { return active; }
  void stop() { active = false; }
  bool connect(const char*, uint16_t, int timeout) {
    if (timeout != 1000) std::abort();
    ++link_test::connects;
    return active = true;
  }
  void setNoDelay(bool) {}
  void setTimeout(unsigned) {}
  size_t write(const uint8_t*, size_t n) override { return n; }
  size_t write(uint8_t) override { return 1; }
};
