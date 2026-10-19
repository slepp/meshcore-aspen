// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string>
#include <map>
#include <vector>
#include <cstring>
#include <cstdio>
#include <openssl/evp.h>
using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_ERR_INVALID_STATE = -1, HTTPD_SOCK_ERR_TIMEOUT = -2;
struct httpd_req_t {
  size_t content_len = 0, offset = 0, stop = SIZE_MAX;
  std::map<std::string, std::string> headers;
  std::vector<uint8_t> data;
  std::string status, response;
  bool failSend = false;
};
inline int httpd_req_get_hdr_value_str(httpd_req_t *r, const char *name, char *out, size_t size) {
  auto it = r->headers.find(name);
  if (it == r->headers.end() || it->second.size() >= size) return -1;
  strcpy(out, it->second.c_str()); return 0;
}
inline size_t httpd_req_get_hdr_value_len(httpd_req_t *r, const char *name) {
  auto it = r->headers.find(name); return it == r->headers.end() ? 0 : it->second.size();
}
inline int httpd_resp_set_status(httpd_req_t *r, const char *status) { r->status = status; return 0; }
inline int httpd_resp_set_type(httpd_req_t *, const char *) { return 0; }
inline int httpd_resp_set_hdr(httpd_req_t *, const char *, const char *) { return 0; }
inline int httpd_resp_sendstr(httpd_req_t *r, const char *text) { r->response = text; return r->failSend ? -1 : 0; }
inline int httpd_req_recv(httpd_req_t *r, char *out, size_t size) {
  size_t end = r->data.size() < r->stop ? r->data.size() : r->stop;
  if (r->offset >= end) return 0;
  size = size < end - r->offset ? size : end - r->offset;
  memcpy(out, r->data.data() + r->offset, size); r->offset += size; return size;
}
constexpr unsigned MALLOC_CAP_INTERNAL = 1 << 11, MALLOC_CAP_8BIT = 1 << 2;
inline bool failTransferAllocation = false;
inline size_t transferAllocations = 0, transferReleases = 0;
inline void *heap_caps_malloc(size_t size, unsigned capabilities) {
  if (capabilities != (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)) abort();
  if (failTransferAllocation) return nullptr;
  ++transferAllocations;
  return malloc(size);
}
inline void heap_caps_free(void *memory) {
  if (memory) ++transferReleases;
  free(memory);
}
inline uint32_t testMillis = 1;
inline uint32_t millis() { return testMillis; }
inline void (*onDelay)() = nullptr;
inline void delay(unsigned n) { testMillis += n; if (onDelay) onDelay(); }
struct EspMock { unsigned restarts = 0; void restart() { ++restarts; } };
inline EspMock ESP;
namespace mesh {
struct Identity {
  uint8_t key[32];
  explicit Identity(const uint8_t *k) { memcpy(key, k, 32); }
  bool verify(const uint8_t *sig, const uint8_t *msg, size_t size) {
    EVP_PKEY *p = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, key, 32);
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    bool ok = EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, p) == 1 &&
              EVP_DigestVerify(ctx, sig, 64, msg, size) == 1;
    EVP_MD_CTX_free(ctx); EVP_PKEY_free(p); return ok;
  }
};
}
using mbedtls_sha256_context = EVP_MD_CTX *;
inline void mbedtls_sha256_init(mbedtls_sha256_context *c) { *c = EVP_MD_CTX_new(); }
inline void mbedtls_sha256_free(mbedtls_sha256_context *c) { EVP_MD_CTX_free(*c); }
inline int mbedtls_sha256_starts_ret(mbedtls_sha256_context *c, int) { return EVP_DigestInit_ex(*c, EVP_sha256(), nullptr) == 1 ? 0 : -1; }
inline int mbedtls_sha256_update_ret(mbedtls_sha256_context *c, const uint8_t *p, size_t n) { return EVP_DigestUpdate(*c, p, n) == 1 ? 0 : -1; }
inline int mbedtls_sha256_finish_ret(mbedtls_sha256_context *c, uint8_t *p) { unsigned n; return EVP_DigestFinal_ex(*c, p, &n) == 1 ? 0 : -1; }
struct esp_partition_t { uint32_t size, address; };
using esp_ota_handle_t = unsigned;
enum esp_ota_img_states_t { ESP_OTA_IMG_VALID, ESP_OTA_IMG_PENDING_VERIFY };
inline esp_partition_t running{0x330000, 0x10000}, inactive{0x330000, 0x340000};
inline std::vector<uint8_t> written, runningImage;
inline unsigned begins = 0, switches = 0, confirms = 0, aborts = 0;
inline bool failWrite = false, failImage = false, failSwitch = false;
inline bool failLength = false, failRead = false, noRunningPartition = false;
inline void (*onWrite)() = nullptr;
inline esp_ota_img_states_t bootState = ESP_OTA_IMG_VALID;
inline const esp_partition_t *esp_ota_get_running_partition() {
  return noRunningPartition ? nullptr : &running;
}
inline const esp_partition_t *esp_ota_get_next_update_partition(void *) { return &inactive; }
inline int esp_ota_begin(const esp_partition_t *p, size_t n, unsigned *h) {
  if (p != &inactive) return -1;
  ++begins; written.clear(); *h = 1; return n <= p->size ? 0 : -1;
}
inline int esp_ota_write(unsigned, const uint8_t *p, size_t n) {
  if (onWrite) onWrite();
  if (failWrite) return -1;
  written.insert(written.end(), p, p + n); return 0;
}
inline int esp_ota_end(unsigned) {
  return !failImage && written.size() >= 24 && written[0] == 0xe9 && written[12] == 9 && !written[13] ? 0 : -1;
}
inline int esp_ota_set_boot_partition(const esp_partition_t *p) {
  if (failSwitch || p == &running) return -1;
  ++switches; return 0;
}
inline int esp_ota_abort(unsigned) { ++aborts; return 0; }
inline int esp_ota_get_state_partition(const esp_partition_t *, esp_ota_img_states_t *s) { *s = bootState; return 0; }
inline int esp_ota_mark_app_valid_cancel_rollback() { ++confirms; bootState = ESP_OTA_IMG_VALID; return 0; }
inline bool esp_ota_check_rollback_is_possible() { return bootState == ESP_OTA_IMG_PENDING_VERIFY; }
inline unsigned rollbacks = 0;
inline bool failRollback = false;
inline int esp_ota_mark_app_invalid_rollback_and_reboot() {
  ++rollbacks;
  if (failRollback) return -1;
  ESP.restart(); return 0;
}
struct esp_partition_pos_t { uint32_t offset, size; };
struct esp_image_metadata_t { uint32_t image_len = 0; };
constexpr int ESP_IMAGE_VERIFY_SILENT = 1;
inline int esp_image_get_metadata(const esp_partition_pos_t *p, esp_image_metadata_t *m) {
  const auto &image = p->offset == running.address ? runningImage : written;
  m->image_len = image.size() - unsigned(failLength && p->offset != running.address);
  return image.empty() ? -1 : 0;
}
inline int esp_image_verify(int, const esp_partition_pos_t *p, esp_image_metadata_t *m) {
  return esp_image_get_metadata(p, m);
}
inline int esp_partition_read(const esp_partition_t *p, size_t offset, void *out, size_t size) {
  if (failRead || p != &running || offset + size > runningImage.size()) return -1;
  memcpy(out, runningImage.data() + offset, size); return 0;
}

// One-shot ESP timers run without the radio/role dispatcher.
struct EspTimer { void (*callback)(void *) = nullptr; void *arg = nullptr; bool active = false; uint32_t due = 0; };
using esp_timer_handle_t = EspTimer *;
struct esp_timer_create_args_t { void (*callback)(void *) = nullptr; void *arg = nullptr; const char *name = nullptr; };
inline EspTimer testTimer;
inline bool failTimerCreate = false, failTimerStart = false;
inline void (*onTimerCreate)() = nullptr;
inline int esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *out) {
  if (failTimerCreate) return -1;
  if (onTimerCreate) onTimerCreate();
  testTimer.callback = args->callback; testTimer.arg = args->arg; *out = &testTimer; return ESP_OK;
}
inline int esp_timer_start_once(esp_timer_handle_t timer, uint64_t us) {
  if (failTimerStart || timer->active) return -1;
  timer->due = testMillis + us / 1000; timer->active = true; return ESP_OK;
}
inline int esp_timer_stop(esp_timer_handle_t timer) { timer->active = false; return ESP_OK; }
inline void advanceEspTimers(uint32_t ms) {
  testMillis += ms;
  if (testTimer.active && int32_t(testMillis - testTimer.due) >= 0) {
    testTimer.active = false; testTimer.callback(testTimer.arg);
  }
}
inline uint32_t esp_random() { static uint32_t value = 0x12345678; return ++value; }
