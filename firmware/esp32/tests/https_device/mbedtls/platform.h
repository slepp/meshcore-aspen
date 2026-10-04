// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <esp_heap_caps.h>

namespace device_tls_memory {
inline void *(*allocate)(size_t, size_t) = nullptr;
inline void (*release)(void *) = nullptr;
inline unsigned setups = 0;
}
inline int mbedtls_platform_set_calloc_free(void *(*allocate)(size_t, size_t), void (*release)(void *)) {
  ++device_tls_memory::setups;
  device_tls_memory::allocate = allocate;
  device_tls_memory::release = release;
  return 0;
}
inline void *mbedtls_calloc(size_t count, size_t size) {
  return device_tls_memory::allocate ? device_tls_memory::allocate(count, size) :
         heap_caps_malloc(count * size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}
inline void mbedtls_free(void *memory) {
  if (device_tls_memory::release) device_tls_memory::release(memory);
  else heap_caps_free(memory);
}
