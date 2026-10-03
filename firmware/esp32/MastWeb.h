// SPDX-License-Identifier: Apache-2.0
#pragma once
#if defined(MESHCORE_MAST_ADMIN) && MESHCORE_MAST_ADMIN
#include "MastAdmin.h"
#if defined(ARDUINO_ARCH_ESP32) || defined(MESHCORE_MAST_WEB_TEST)
#include <esp_http_server.h>
#endif
namespace onchip {
void serviceMastWeb(MastAdmin &admin);
void serviceMastWebRecovery();
#if defined(ARDUINO_ARCH_ESP32) || defined(MESHCORE_MAST_WEB_TEST)
esp_err_t registerMastWeb(httpd_handle_t server);
#endif
}
#endif
