// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stddef.h>
#include <stdio.h>

namespace onchip {
#if defined(ARDUINO_ARCH_ESP32) && defined(MESHCORE_MAST_ADMIN) && MESHCORE_MAST_ADMIN
bool beginSyslog();
bool syslogCommand(const char *command, char *reply, size_t capacity);
void sendSyslog(const char *message);
#else
inline bool beginSyslog() { return true; }
inline bool syslogCommand(const char *, char *, size_t) { return false; }
inline void sendSyslog(const char *) {}
#endif
} // namespace onchip
