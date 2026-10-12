// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>
#if defined(MESHCORE_MAST_ADMIN) && MESHCORE_MAST_ADMIN && (defined(ARDUINO_ARCH_ESP32) || defined(MESHCORE_MAST_WEB_TEST))
namespace onchip {
bool espUpdateBusy();
uint32_t reserveEspForceRestart(const char *&error, bool *unavailable = nullptr);
bool acknowledgeEspForceRestart(uint32_t ticket, bool transmitted);
}
#endif
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
void setEspUpdateFlush(bool (*flush)());
void serviceEspUpdate(bool rolesReady, bool wifiReady, bool wifiEnabled);
void espUpdateRecovery(const char *reason);
}
#endif
