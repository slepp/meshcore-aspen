// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stddef.h>
#include <stdint.h>
using esp_err_t = int;
using nvs_handle_t = unsigned;
constexpr int ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_NVS_NOT_FOUND = 1,
    ESP_ERR_INVALID_STATE = 2, ESP_ERR_INVALID_ARG = 3, ESP_ERR_NVS_INVALID_LENGTH = 4,
    ESP_ERR_NVS_NOT_ENOUGH_SPACE = 5, NVS_READONLY = 0, NVS_READWRITE = 1;
struct nvs_stats_t { size_t used_entries = 0, free_entries = 0, total_entries = 0, namespace_count = 0; };
const char *esp_err_to_name(esp_err_t);
esp_err_t nvs_open(const char *, int, nvs_handle_t *);
void nvs_close(nvs_handle_t);
esp_err_t nvs_get_blob(nvs_handle_t, const char *, void *, size_t *);
esp_err_t nvs_set_blob(nvs_handle_t, const char *, const void *, size_t);
esp_err_t nvs_erase_key(nvs_handle_t, const char *);
esp_err_t nvs_commit(nvs_handle_t);
esp_err_t nvs_get_stats(const char *, nvs_stats_t *);
