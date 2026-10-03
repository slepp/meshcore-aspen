#pragma once
#include <WiFi.h>
using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0;
inline const char* esp_err_to_name(esp_err_t) { return "ESP_FAIL"; }
inline esp_err_t esp_wifi_set_max_tx_power(int power) {
  link_test::calls.push_back("power");
  link_test::power = power;
  return link_test::failure == 5 ? -1 : ESP_OK;
}
