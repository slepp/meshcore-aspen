#pragma once
#include <stddef.h>
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

// Aspen (MESHCORE_ONCHIP) names itself KISS_HOSTNAME followed by the last
// three bytes of the factory WiFi MAC, such as aspen-8807b0, so several radios
// can share a LAN and each keeps its name across firmware updates and erases.
// KISS_HOSTNAME_FIXED=1 uses KISS_HOSTNAME unchanged. Radio-only modem builds
// keep their fixed name by default because hosts address them by it.
#ifndef KISS_HOSTNAME_FIXED
#ifdef MESHCORE_ONCHIP
#define KISS_HOSTNAME_FIXED 0
#else
#define KISS_HOSTNAME_FIXED 1
#endif
#endif
#if !KISS_HOSTNAME_FIXED && (defined(ARDUINO_ARCH_ESP32) || defined(MESHCORE_MAST_WEB_TEST))
#define RADIO_NETWORK_HOSTNAME_MAC 1
#include <esp_system.h>
#endif

namespace radio_network {
constexpr char hostnamePrefix[] = KISS_HOSTNAME;
constexpr bool hostnameFixed = KISS_HOSTNAME_FIXED;
constexpr size_t HostnameCapacity = 32;  // Arduino's WiFi buffer, terminator included.
constexpr size_t HostnameSuffixLength = 7; // "-" and six hex digits.
bool startStation();

// Writes PREFIX-XXXXXX from the last three MAC bytes, or PREFIX alone when
// fixed. Returns false, leaving an empty string, if the name does not fit.
inline bool formatHostname(const char* prefix, const uint8_t mac[6], bool fixed,
                           char* out, size_t size) {
  static const char hex[] = "0123456789abcdef";
  size_t n = 0;
  while (prefix[n]) ++n;
  const size_t length = n + (fixed ? 0 : HostnameSuffixLength);
  if (!size) return false;
  if (length >= size || length >= HostnameCapacity) {
    out[0] = 0;
    return false;
  }
  for (size_t i = 0; i < n; ++i) out[i] = prefix[i];
  if (!fixed) {
    out[n++] = '-';
    for (unsigned i = 3; i < 6; ++i) {
      out[n++] = hex[mac[i] >> 4];
      out[n++] = hex[mac[i] & 15];
    }
  }
  out[n] = 0;
  return true;
}

// The device's mDNS, DHCP and syslog name, computed once on first use. A
// board whose MAC cannot be read, and host builds, use the prefix alone.
inline const char* hostname() {
  struct Name {
    char text[HostnameCapacity]{};
    Name() {
      uint8_t mac[6]{};
#ifdef RADIO_NETWORK_HOSTNAME_MAC
      const bool fixed = esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK;
#else
      const bool fixed = true;
#endif
      formatHostname(hostnamePrefix, mac, fixed, text, sizeof(text));
    }
  };
  static const Name name;
  return name.text;
}

// SDK association and DHCP are separate states. All deadlines use millis(),
// including across its wrap; event callbacks only supply a loss generation.
class WifiRecovery {
public:
  static constexpr uint32_t RetryMs = 30000, DhcpMs = 120000, MaxRetryMs = 120000;
  struct Actions {
    bool down = false, up = false, retry = false, restart = false;
  };

  void begin(uint32_t now, bool enabled) {
    enabled_ = enabled;
    next_ = now + RetryMs;
    attempts_ = 0;
    dhcp_waited_ = false;
  }
  Actions update(uint32_t now, bool associated, bool hasIP, uint32_t ip,
                 uint32_t loss) {
    Actions result;
    const bool usable = enabled_ && associated && hasIP && ip;
    const bool changed = loss != loss_ || (ready_ && ip != ip_);
    result.down = ready_ && (!usable || changed);
    result.up = usable && (!ready_ || changed);
    if (usable || ready_) {
      next_ = now + RetryMs;
      attempts_ = 0;
      dhcp_waited_ = false;
    }
    // Grant DHCP one interval per attempt, never a fresh deadline for every
    // association flap. An overdue dispatch must not postpone recovery.
    if (!usable && associated && !dhcp_waited_) {
      if (int32_t(now - next_) < 0) next_ = now + DhcpMs;
      dhcp_waited_ = true;
    }
    ready_ = usable;
    ip_ = ip;
    loss_ = loss;
    if (enabled_ && !usable && int32_t(now - next_) >= 0) {
      result.retry = true;
      if (attempts_ < 3) ++attempts_;
      result.restart = attempts_ >= 3;
      next_ = now + retryDelay();
      dhcp_waited_ = false;
    }
    return result;
  }
  bool ready() const { return ready_; }
  bool enabled() const { return enabled_; }
  unsigned attempts() const { return attempts_; }
  uint32_t retryIn(uint32_t now) const {
    return !enabled_ || ready_ || int32_t(now - next_) >= 0 ? 0 : next_ - now;
  }
private:
  uint32_t retryDelay() const {
    return attempts_ <= 1 ? RetryMs : attempts_ == 2 ? RetryMs * 2 : MaxRetryMs;
  }
  uint32_t next_ = 0, ip_ = 0, loss_ = 0;
  unsigned attempts_ = 0;
  bool enabled_ = false, dhcp_waited_ = false, ready_ = false;
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

constexpr size_t prefixLength(const char* p) { return *p ? 1 + prefixLength(p + 1) : 0; }

// Arduino's WiFi hostname buffer is 32 bytes, including the terminator.
static_assert(validHostname(hostnamePrefix),
              "KISS_HOSTNAME must be a 1..31 character DNS label: letters, digits, interior hyphens; no .local suffix");
static_assert(hostnameFixed ||
                  prefixLength(hostnamePrefix) + HostnameSuffixLength < HostnameCapacity,
              "KISS_HOSTNAME must be at most 24 characters to leave room for -XXXXXX; "
              "set KISS_HOSTNAME_FIXED=1 to use a longer fixed name");
static_assert(KISS_TCP_PORT > 0 && KISS_TCP_PORT <= 65535,
              "KISS TCP port must be 1..65535");
static_assert(KISS_HTTP_PORT > 0 && KISS_HTTP_PORT <= 65535,
              "Dashboard HTTP port must be 1..65535");
} // namespace radio_network
