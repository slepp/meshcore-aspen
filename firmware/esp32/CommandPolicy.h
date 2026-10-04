// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace onchip {
inline bool prefix(const char *value, const char *start) {
  return strncmp(value, start, strlen(start)) == 0;
}

inline bool clearsAdminPassword(const char *command) {
  return strcmp(command, "password ") == 0;
}

// These native commands assume exclusive ownership of the device.
inline bool unsafeCLI(const char *command) {
  const char *blocked[] = {
      "clkreboot",       "poweroff",    "shutdown",
      "start ota",       "tempradio",   "clear stats",
      "set radio",      "set freq",
      "set tx ",         "set int",     "set agc",   "set rxboost",
      "set rxgain",      "set fem",     "set cad",   "set adc",
      "set powersaving", "sensor set",  "set gps",   "set bridge",
      "gps on",          "gps off",     "gps sync",  "powersaving",
      "log start"};
  for (auto item : blocked)
    if (prefix(command, item))
      return true;
  return false;
}

inline bool unsafeCompanion(uint8_t command) {
  switch (command) {
  case 11:
  case 12: // physical radio
  case 23:
  case 24: // private identity
  case 33:
  case 34:
  case 35: // native signing buffer is global to the interface
  case 37: // BLE PIN (no BLE endpoint)
  case 38: // includes shared GPS/gain settings
  case 41: // shared sensor mutation
    return true;
  default:
    return false;
  }
}
} // namespace onchip
