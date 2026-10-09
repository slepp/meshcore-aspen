// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>

namespace onchip {
enum class DiagnosticSubsystem : uint8_t {
  System, Wifi, Clock, Companion, Bot, BotVm, Telemetry, Packet, Backup
};
constexpr size_t DiagnosticMessageCapacity = 384;
#if defined(ARDUINO_ARCH_ESP32) && defined(MESHCORE_MAST_ADMIN) && MESHCORE_MAST_ADMIN
bool beginSyslog();
bool syslogCommand(const char *command, char *reply, size_t capacity);
void sendSyslog(const char *message, DiagnosticSubsystem subsystem, uint32_t utc, uint64_t uptimeMs);
#else
inline bool beginSyslog() { return true; }
inline bool syslogCommand(const char *, char *, size_t) { return false; }
inline void sendSyslog(const char *, DiagnosticSubsystem, uint32_t, uint64_t) {}
#endif
} // namespace onchip
