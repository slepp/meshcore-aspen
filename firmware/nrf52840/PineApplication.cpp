// SPDX-License-Identifier: Apache-2.0
#include "PineApplication.h"
#include <SHA256.h>
#include <Utils.h>
#include <cstdio>
#include <cstring>
#ifndef PINE_APPLICATION_START
#define PINE_APPLICATION_START 0x27000
#endif

extern "C" {
extern uint8_t __flash_arduino_start, __flash_arduino_end;
extern uint8_t __etext, __data_start__, __data_end__;
}

namespace nrfmast {
bool applicationCommand(const char* command, char* reply) {
  if (strcmp(command, "get application")) return false;
  const auto start = reinterpret_cast<uintptr_t>(&__flash_arduino_start);
  const auto bytes = applicationBytes(start, reinterpret_cast<uintptr_t>(&__etext),
                                     reinterpret_cast<uintptr_t>(&__data_start__),
                                     reinterpret_cast<uintptr_t>(&__data_end__),
                                     reinterpret_cast<uintptr_t>(&__flash_arduino_end), PINE_APPLICATION_START);
  if (!bytes) {
    strcpy(reply, "Error: application linker range outside Pine's retained flash layout");
    return true;
  }
  static char digest[65]{};
  if (!digest[0]) {
    SHA256 hash;
    uint8_t binary[32];
    hash.reset();
    hash.update(reinterpret_cast<const uint8_t*>(start), bytes);
    hash.finalize(binary, sizeof(binary));
    mesh::Utils::toHex(digest, binary, sizeof(binary));
  }
  snprintf(reply, 160, "application start=0x%lx bytes=%lu sha256=%s",
           static_cast<unsigned long>(start), static_cast<unsigned long>(bytes), digest);
  return true;
}
}
