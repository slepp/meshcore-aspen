#pragma once
#include <cstddef>
#include <cstdint>
void esp_fill_random(void *, size_t);

using esp_err_t = int;
#ifndef MESHCORE_TEST_ESP_ERRORS_DEFINED
#define MESHCORE_TEST_ESP_ERRORS_DEFINED
constexpr int ESP_OK = 0, ESP_FAIL = -1;
#endif
enum esp_mac_type_t { ESP_MAC_WIFI_STA };
// Native tests use the lab radio's factory MAC, 20:6e:f1:88:07:b0.
inline esp_err_t esp_read_mac(uint8_t *mac, esp_mac_type_t) {
  const uint8_t value[6] = {0x20, 0x6e, 0xf1, 0x88, 0x07, 0xb0};
  for (unsigned i = 0; i < 6; ++i) mac[i] = value[i];
  return ESP_OK;
}
