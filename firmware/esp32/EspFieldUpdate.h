// SPDX-License-Identifier: Apache-2.0
#pragma once
#if defined(ARDUINO_ARCH_ESP32) && defined(MESHCORE_MAST_ADMIN) && MESHCORE_MAST_ADMIN
#ifdef MESHCORE_ESP_UPDATE_TEST
#include "tests/esp_update_platform.h"
#else
#include <esp_http_server.h>
#endif
namespace onchip {
esp_err_t uploadEspUpdate(httpd_req_t *request);
esp_err_t statusEspUpdate(httpd_req_t *request);
esp_err_t rebootEspUpdate(httpd_req_t *request);
bool espUpdateBusy();
void serviceEspUpdate(bool rolesReady, bool wifiReady, bool wifiEnabled);
void espUpdateRecovery(const char *reason);
}
#endif
