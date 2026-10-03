#pragma once
#include <stdint.h>

#ifndef KISS_TCP_PORT
#define KISS_TCP_PORT 8001
#endif
#ifndef KISS_HTTP_PORT
#define KISS_HTTP_PORT 80
#endif

#ifndef KISS_HOSTNAME
#if defined(WIFI_HOSTNAME)
#define KISS_HOSTNAME WIFI_HOSTNAME
#elif defined(HOSTNAME)
#define KISS_HOSTNAME HOSTNAME
#else
#define KISS_HOSTNAME "meshcore-radio"
#endif
#endif

namespace radio_network {
constexpr char hostname[] = KISS_HOSTNAME;
bool startStation();

// SDK association and DHCP are separate states. All deadlines use millis(),
// including across its wrap; event callbacks only supply a loss generation.
class WifiRecovery {
public:
  static constexpr uint32_t RetryMs = 30000, DhcpMs = 120000;
  struct Actions {
    bool down = false, up = false, retry = false;
  };

  void begin(uint32_t now, bool enabled) {
    enabled_ = enabled;
    next_ = now + RetryMs;
  }
  Actions update(uint32_t now, bool associated, bool hasIP, uint32_t ip,
                 uint32_t loss) {
    Actions result;
    const bool usable = associated && hasIP && ip;
    const bool changed = loss != loss_ || (ready_ && ip != ip_);
    result.down = ready_ && (!usable || changed);
    result.up = usable && (!ready_ || changed);
    if (associated && (!associated_ || (ready_ && !usable)))
      next_ = now + DhcpMs;
    else if (!associated && associated_)
      next_ = now + RetryMs;
    associated_ = associated;
    ready_ = usable;
    ip_ = ip;
    loss_ = loss;
    if (enabled_ && !usable && int32_t(now - next_) >= 0) {
      result.retry = true;
      next_ = now + RetryMs;
    }
    return result;
  }
  bool ready() const { return ready_; }
private:
  uint32_t next_ = 0, ip_ = 0, loss_ = 0;
  bool enabled_ = false, associated_ = false, ready_ = false;
};

inline WifiRecovery& wifiRecovery() {
  static WifiRecovery recovery;
  return recovery;
}

constexpr bool hostnameCharacter(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '-';
}

constexpr bool hostnameTail(const char* p, unsigned remaining) {
  return !*p || (remaining && hostnameCharacter(*p) && (p[1] || *p != '-') &&
                hostnameTail(p + 1, remaining - 1));
}

constexpr bool validHostname(const char* name) {
  return *name && *name != '-' && hostnameTail(name, 31);
}

// Arduino's WiFi hostname buffer is 32 bytes, including the terminator.
static_assert(validHostname(hostname),
              "KISS_HOSTNAME must be a 1..31 character DNS label: letters, digits, interior hyphens; no .local suffix");
static_assert(KISS_TCP_PORT > 0 && KISS_TCP_PORT <= 65535,
              "KISS TCP port must be 1..65535");
static_assert(KISS_HTTP_PORT > 0 && KISS_HTTP_PORT <= 65535,
              "Dashboard HTTP port must be 1..65535");
} // namespace radio_network
