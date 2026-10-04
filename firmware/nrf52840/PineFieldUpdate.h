#pragma once
#include <stdint.h>
#include <atomic>

namespace nrfmast {
constexpr bool authenticatedBleBond(bool connected, bool secured, bool bonded,
                                    unsigned securityMode, unsigned securityLevel) {
  return connected && secured && bonded && securityMode == 1 && securityLevel >= 3;
}
class FieldUpdateWindow {
  std::atomic<uint32_t> started_{0};
  std::atomic<bool> armed_{false};
public:
  static constexpr uint32_t DurationMs = 120000;
  bool arm(uint32_t now, bool capability, bool pinConfigured, bool authorized) {
    if (!capability || !pinConfigured || !authorized || armed_.load(std::memory_order_acquire)) return false;
    started_.store(now, std::memory_order_relaxed);
    armed_.store(true, std::memory_order_release); return true;
  }
  bool active(uint32_t now) const {
    return armed_.load(std::memory_order_acquire) &&
           uint32_t(now - started_.load(std::memory_order_relaxed)) < DurationMs;
  }
  bool expired(uint32_t now) const { return armed_.load(std::memory_order_acquire) && !active(now); }
  uint32_t remaining(uint32_t now) const {
    return active(now) ? DurationMs - uint32_t(now - started_.load(std::memory_order_relaxed)) : 0;
  }
  void revoke() { armed_.store(false, std::memory_order_release); }
  bool authorize(uint32_t now, bool authenticated, const uint8_t* data, unsigned length) const {
    return active(now) && authenticated && data && (length == 1 || length == 2) &&
           data[0] == 1 && (length == 1 || data[1] == 4);
  }
};

bool fieldUpdateCommand(uint32_t senderTimestamp, const char* text, char* reply);
void fieldUpdateLoop();
bool fieldUpdatePending();
}
