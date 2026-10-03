// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>

using esp_err_t = int;
using nvs_handle_t = unsigned;

constexpr int ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_NVS_NOT_FOUND = 1;
constexpr int ESP_ERR_INVALID_STATE = 2, ESP_ERR_INVALID_ARG = 3;
constexpr int ESP_ERR_NVS_INVALID_LENGTH = 4, ESP_ERR_NVS_NOT_ENOUGH_SPACE = 5;
constexpr int NVS_READONLY = 0, NVS_READWRITE = 1;

struct nvs_stats_t {
  size_t used_entries = 0, free_entries = 0, total_entries = 0, namespace_count = 0;
};

// Initialize before any NVS operation. The directory must already exist, be
// owned by this process's effective user, and have mode 0700. Every component
// of the absolute path is opened without following symlinks. One cooperating
// process may hold the directory lock at a time. Never creates the directory.
// The single 0600 snapshot is CRC-checked and rejected if corrupt; failed
// publication blocks further operations until shutdown and explicit re-init.
// Values are bounded to 32768 bytes, names/keys to 15 ASCII characters.
// nvs_get_stats reports the native 630 raw entries; writes retain a separate
// 126-entry compaction reserve, matching the five-page on-chip partition.
esp_err_t native_nvs_init(const char *absolute_private_directory);
void native_nvs_shutdown();

// Before starting CommandBot, expose the Go HELLO identity through volatile NVS.
// Binds one 64-byte scalar-prefix key per lifetime; never generates or saves it.
esp_err_t native_nvs_bind_bot_identity(const uint8_t *expanded, size_t size);

esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle);
void nvs_close(nvs_handle_t handle);
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *data, size_t *size);
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *data, size_t size);
esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key);
esp_err_t nvs_commit(nvs_handle_t handle);
esp_err_t nvs_get_stats(const char *partition, nvs_stats_t *stats);
const char *esp_err_to_name(esp_err_t error);
