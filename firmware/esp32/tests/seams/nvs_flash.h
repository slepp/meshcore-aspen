#pragma once
#include <cassert>
inline int nvs_flash_erase() {
  assert(false && "Global NVS erase is forbidden");
  return -1;
}
