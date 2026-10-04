// SPDX-License-Identifier: Apache-2.0
#if NRFMAST_PRODUCTION_LUA
#include "platform/nvs.h"
#include "PineFilesystem.h"
#include <cstring>
#include <cstdio>

namespace {
struct Handle {
  char space[17]{}, key[17]{};
  bool writable = false, pending = false, erase = false;
};
Handle handles[8];
StaticSemaphore_t mutexStorage;
SemaphoreHandle_t mutex;
struct Guard {
  Guard() { taskENTER_CRITICAL(); if (!mutex) mutex = xSemaphoreCreateMutexStatic(&mutexStorage); taskEXIT_CRITICAL(); xSemaphoreTake(mutex, portMAX_DELAY); }
  ~Guard() { xSemaphoreGive(mutex); }
};
bool name(const char *value) {
  if (!value || !*value || strlen(value) > 16) return false;
  for (const char *p = value; *p; ++p)
    if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-')) return false;
  return true;
}
Handle *get(nvs_handle_t handle) { return handle && handle <= 8 && handles[handle - 1].space[0] ? &handles[handle - 1] : nullptr; }
void path(const Handle &h, const char *key, char (&out)[96], bool staged = false) {
  snprintf(out, sizeof(out), "/metadata/%s/%s%s", h.space, key, staged ? ".stage" : "");
}
}
const char *esp_err_to_name(esp_err_t value) {
  return value == ESP_OK ? "ok" : value == ESP_ERR_NVS_NOT_FOUND ? "record missing" :
      value == ESP_ERR_NVS_NOT_ENOUGH_SPACE ? "metadata storage full" : "metadata filesystem operation failed";
}
esp_err_t nvs_open(const char *space, int mode, nvs_handle_t *out) {
  if (!out || !name(space) || (mode != NVS_READONLY && mode != NVS_READWRITE)) return ESP_ERR_INVALID_ARG;
  *out = 0; Guard guard;
  if (!nrfmast::botFilesystem.ready()) return ESP_FAIL;
  char directory[96]; snprintf(directory, sizeof(directory), "/metadata/%s", space);
  if (!nrfmast::botFilesystem.exists(directory)) {
    if (!nrfmast::botFilesystem.ready()) return ESP_FAIL;
    if (mode == NVS_READONLY) return ESP_ERR_NVS_NOT_FOUND;
    if (!nrfmast::botFilesystem.mkdir(directory)) return ESP_FAIL;
  }
  for (const auto &h : handles)
    if (h.space[0] && h.writable && mode == NVS_READWRITE && !strcmp(h.space, space))
      return ESP_ERR_INVALID_STATE;
  for (unsigned i = 0; i < 8; ++i) if (!handles[i].space[0]) {
    handles[i] = {}; strcpy(handles[i].space, space); handles[i].writable = mode == NVS_READWRITE;
    *out = i + 1; return ESP_OK;
  }
  return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
}
void nvs_close(nvs_handle_t value) {
  Guard guard;
  if (auto *h = get(value)) {
    if (h->pending) { char staged[96]; path(*h, h->key, staged, true); nrfmast::botFilesystem.remove(staged); }
    *h = {};
  }
}
esp_err_t nvs_get_blob(nvs_handle_t value, const char *key, void *out, size_t *size) {
  if (!name(key) || !size) return ESP_ERR_INVALID_ARG;
  Guard guard; auto *h = get(value); if (!h) return ESP_ERR_INVALID_ARG;
  if (!nrfmast::botFilesystem.ready()) return ESP_FAIL;
  char file[96]; path(*h, key, file);
  auto input = nrfmast::botFilesystem.open(file);
  if (!input) {
    const bool exists = nrfmast::botFilesystem.exists(file);
    return !nrfmast::botFilesystem.ready() || exists ? ESP_FAIL : ESP_ERR_NVS_NOT_FOUND;
  }
  const size_t actual = input.size(), capacity = *size; *size = actual;
  if (!nrfmast::botFilesystem.ready()) return ESP_FAIL;
  if (!out) return ESP_OK;
  if (capacity < actual) return ESP_ERR_NVS_INVALID_LENGTH;
  return input.read(static_cast<uint8_t *>(out), actual) == actual ? ESP_OK : ESP_FAIL;
}
esp_err_t nvs_set_blob(nvs_handle_t value, const char *key, const void *in, size_t size) {
  if (!name(key) || !in || !size || size > 4096) return ESP_ERR_INVALID_ARG;
  Guard guard; auto *h = get(value);
  if (!h || !h->writable || (h->pending && strcmp(h->key, key))) return ESP_ERR_INVALID_STATE;
  char file[96]; path(*h, key, file, true);
  auto output = nrfmast::botFilesystem.open(file, "w");
  if (!output || output.write(static_cast<const uint8_t *>(in), size) != size) return ESP_FAIL;
  output.flush(); output.close();
  auto check = nrfmast::botFilesystem.open(file);
  if (!check || check.size() != size) return ESP_FAIL;
  uint8_t bytes[128];
  for (size_t offset = 0; offset < size;) {
    const size_t count = size - offset > sizeof(bytes) ? sizeof(bytes) : size - offset;
    if (check.read(bytes, count) != count || memcmp(bytes, static_cast<const uint8_t *>(in) + offset, count)) return ESP_FAIL;
    offset += count;
  }
  strcpy(h->key, key); h->pending = true; h->erase = false; return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t value, const char *key) {
  if (!name(key)) return ESP_ERR_INVALID_ARG;
  Guard guard; auto *h = get(value);
  if (!h || !h->writable || h->pending) return ESP_ERR_INVALID_STATE;
  if (!nrfmast::botFilesystem.ready()) return ESP_FAIL;
  char file[96]; path(*h, key, file);
  if (!nrfmast::botFilesystem.exists(file))
    return nrfmast::botFilesystem.ready() ? ESP_ERR_NVS_NOT_FOUND : ESP_FAIL;
  strcpy(h->key, key); h->pending = true; h->erase = true; return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t value) {
  Guard guard; auto *h = get(value);
  if (!h || !h->writable) return ESP_ERR_INVALID_ARG;
  if (!h->pending) return ESP_OK;
  char file[96], staged[96]; path(*h, h->key, file); path(*h, h->key, staged, true);
  const bool ok = h->erase ? nrfmast::botFilesystem.remove(file) : nrfmast::botFilesystem.rename(staged, file);
  if (ok) { h->pending = false; h->key[0] = 0; }
  return ok ? ESP_OK : ESP_FAIL;
}
esp_err_t nvs_get_stats(const char *, nvs_stats_t *stats) {
  if (!stats) return ESP_ERR_INVALID_ARG;
  uint32_t used, total;
  if (!nrfmast::botFilesystem.capacity(used, total)) return ESP_FAIL;
  *stats = {};
  // These reservations include filesystem metadata and retained replacement banks.
  constexpr uint32_t reserve = 16 * nrfmast::NoteJournal::SECTOR;
  stats->total_entries = total / 128; stats->used_entries = (used + reserve + 127) / 128;
  stats->free_entries = stats->used_entries < stats->total_entries ? stats->total_entries - stats->used_entries : 0;
  return ESP_OK;
}
#endif
