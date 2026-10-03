// SPDX-License-Identifier: Apache-2.0
#if defined(ARDUINO_ARCH_ESP32) && defined(MESHCORE_MAST_ADMIN) && MESHCORE_MAST_ADMIN
#include "EspFieldUpdate.h"
#include "EspUpdate.h"
#include "Config.h"
#ifndef MESHCORE_ESP_UPDATE_TEST
#include <Arduino.h>
#include <Identity.h>
#include <esp_ota_ops.h>
#include <esp_app_format.h>
#include <esp_image_format.h>
#include <mbedtls/sha256.h>
#endif
#include <atomic>
#include <mutex>

// Arduino 2.0.17's weak C hook skips initArduino's pre-setup confirmation.
extern "C" bool verifyRollbackLater() { return true; }

namespace onchip {
namespace {
EspUpdate update;
std::mutex updateMutex;
std::atomic<bool> busy{false}, rebootRequested{false}, rollbackRequested{false}, healthy{false};
std::atomic<bool> rollbackObserved{false};
uint32_t rebootAt = 0, healthAt = 0, unhealthyAt = 0;
bool automaticRollbackAttempted = false;
std::atomic<const char *> recovery{""};
const char imageTarget[] = "meshcore-esp-target-v1:" ONCHIP_UPDATE_TARGET ":";
uint8_t transferBuffer[4096];
char updateDigest[65]{}, runningDigest[65]{};
uint32_t runningSize = 0;
const char *runningHashError = "";
void encodeDigest(const uint8_t digest[32], char text[65]) {
  for (unsigned i = 0; i < 32; ++i) snprintf(text + 2 * i, 3, "%02x", digest[i]);
}
void cacheRunningDigest() {
  if (runningDigest[0]) return;
  const auto *partition = esp_ota_get_running_partition();
  if (!partition) {
    runningHashError = "Running application partition unavailable; check partition layout";
    return;
  }
  esp_image_metadata_t metadata{};
  const esp_partition_pos_t position{partition->address, partition->size};
  if (esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &position, &metadata) != ESP_OK ||
      !metadata.image_len || metadata.image_len > partition->size) {
    runningHashError = "Running application image verification failed";
    return;
  }
  mbedtls_sha256_context hash;
  mbedtls_sha256_init(&hash);
  bool ok = !mbedtls_sha256_starts_ret(&hash, 0);
  for (size_t offset = 0; ok && offset < metadata.image_len;) {
    const size_t remaining = metadata.image_len - offset;
    const size_t size = remaining < sizeof(transferBuffer) ? remaining : sizeof(transferBuffer);
    ok = esp_partition_read(partition, offset, transferBuffer, size) == ESP_OK &&
         !mbedtls_sha256_update_ret(&hash, transferBuffer, size);
    offset += size;
    delay(1);
  }
  uint8_t digest[32];
  ok = ok && !mbedtls_sha256_finish_ret(&hash, digest);
  mbedtls_sha256_free(&hash);
  if (ok) {
    encodeDigest(digest, runningDigest);
    runningSize = metadata.image_len;
    runningHashError = "";
  } else runningHashError = "Running application SHA256 read failed; retry status";
}
bool unhex(const char *text, uint8_t *out, size_t size) {
  if (strlen(text) != size * 2) return false;
  for (size_t i = 0; i < size * 2; ++i) {
    const char c = text[i];
    const int n = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
    if (n < 0) return false;
    if (!(i & 1)) out[i / 2] = n << 4;
    else out[i / 2] |= n;
  }
  return true;
}
struct Adapter final : EspUpdate::Backend {
  const esp_partition_t *partition = nullptr;
  esp_ota_handle_t handle = 0;
  bool open = false, hashOpen = false;
  bool targetFound = false;
  size_t targetMatched = 0;
  size_t expectedSize = 0;
  uint8_t targetPrefixes[sizeof(imageTarget)]{};
  uint8_t expected[32]{};
  mbedtls_sha256_context hash;
  bool begin(size_t size) override {
    expectedSize = size;
    for (size_t i = 1, matched = 0; i < sizeof(imageTarget) - 1; ++i) {
      while (matched && imageTarget[i] != imageTarget[matched]) matched = targetPrefixes[matched - 1];
      if (imageTarget[i] == imageTarget[matched]) ++matched;
      targetPrefixes[i] = matched;
    }
    mbedtls_sha256_init(&hash); hashOpen = true;
    if (mbedtls_sha256_starts_ret(&hash, 0)) return false;
    open = esp_ota_begin(partition, size, &handle) == ESP_OK;
    return open;
  }
  bool write(const uint8_t *data, size_t size) override {
    for (size_t i = 0; i < size; ++i) {
      while (targetMatched && data[i] != uint8_t(imageTarget[targetMatched]))
        targetMatched = targetPrefixes[targetMatched - 1];
      if (data[i] == uint8_t(imageTarget[targetMatched])) ++targetMatched;
      if (targetMatched == sizeof(imageTarget) - 1) {
        targetFound = true; targetMatched = targetPrefixes[targetMatched - 1];
      }
    }
    return !mbedtls_sha256_update_ret(&hash, data, size) &&
           esp_ota_write(handle, data, size) == ESP_OK;
  }
  bool verify() override {
    uint8_t actual[32];
    if (!targetFound || mbedtls_sha256_finish_ret(&hash, actual) || memcmp(actual, expected, 32)) return false;
    // esp_ota_end consumes the handle even on failure and validates chip/revision,
    // segment bounds, checksum and the image's appended SHA256 when present.
    open = false;
    if (esp_ota_end(handle) != ESP_OK) return false;
    esp_image_metadata_t metadata{};
    const esp_partition_pos_t position{partition->address, partition->size};
    // Match the complete raw file, including its appended hash. Extra trailing
    // bytes would make upload and post-reboot running digests disagree.
    return esp_image_get_metadata(&position, &metadata) == ESP_OK &&
           metadata.image_len == expectedSize;
  }
  bool activate() override { return esp_ota_set_boot_partition(partition) == ESP_OK; }
  void abort() override {
    if (open) esp_ota_abort(handle);
    open = false;
  }
  ~Adapter() {
    abort();
    if (hashOpen) mbedtls_sha256_free(&hash);
  }
};
esp_err_t response(httpd_req_t *request, const char *status, const char *text) {
  httpd_resp_set_status(request, status);
  httpd_resp_set_type(request, "application/json");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  httpd_resp_set_hdr(request, "Connection", "close");
  return httpd_resp_sendstr(request, text);
}
esp_err_t statusLocked(httpd_req_t *request, const char *code) {
  cacheRunningDigest();
  const char *names[] = {"idle", "receiving", "failed", "verified"};
  char text[1024];
  snprintf(text, sizeof(text),
    "{\"state\":\"%s\",\"received\":%u,\"size\":%u,\"error\":\"%s\","
    "\"target\":\"%s\",\"boot_health\":\"%s\",\"recovery\":\"%s\","
    "\"reboot_ready\":%s,\"rollback_supported\":%s,\"rollback_ready\":%s,"
    "\"sha256\":\"%s\",\"running_sha256\":\"%s\",\"running_size\":%u,\"running_hash_error\":\"%s\"}",
    names[update.state()], unsigned(update.received()), unsigned(update.size()), update.error(),
    ONCHIP_UPDATE_TARGET, healthy ? "healthy" : "pending", recovery.load(),
    update.state() == EspUpdate::Verified ? "true" : "false",
    rollbackObserved ? "true" : "false",
    !healthy && rollbackObserved && esp_ota_check_rollback_is_possible() ? "true" : "false",
    updateDigest, runningDigest, unsigned(runningSize), runningHashError);
  return response(request, code, text);
}
}
bool espUpdateBusy() { return busy.load(); }
esp_err_t statusEspUpdate(httpd_req_t *request) {
  std::lock_guard<std::mutex> lock(updateMutex);
  return statusLocked(request, "200 OK");
}
esp_err_t uploadEspUpdate(httpd_req_t *request) {
  bool expectedBusy = false;
  if (!busy.compare_exchange_strong(expectedBusy, true))
    return response(request, "409 Conflict", "{\"error\":\"Application update already active; inspect status\"}");
  std::lock_guard<std::mutex> lock(updateMutex);
  updateDigest[0] = 0;
  Adapter adapter;
  adapter.partition = esp_ota_get_next_update_partition(nullptr);
  if (!adapter.partition || adapter.partition == esp_ota_get_running_partition()) {
    update.fail("Inactive OTA application partition unavailable");
    busy = false; return statusLocked(request, "503 Service Unavailable");
  }
  esp_ota_img_states_t bootState;
  if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &bootState) == ESP_OK &&
      bootState == ESP_OTA_IMG_PENDING_VERIFY) {
    rollbackObserved = true;
    update.fail("Running application is unconfirmed; wait for boot health or request available rollback");
    busy = false; return statusLocked(request, "409 Conflict");
  }
  char target[64], digest[65], signature[129], manifest[256];
  uint8_t key[32], sig[64];
  bool authorized = adapter.partition &&
    adapter.partition != esp_ota_get_running_partition() &&
    httpd_req_get_hdr_value_str(request, "X-Mast-Target", target, sizeof(target)) == ESP_OK &&
    !strcmp(target, ONCHIP_UPDATE_TARGET) &&
    httpd_req_get_hdr_value_str(request, "X-Mast-SHA256", digest, sizeof(digest)) == ESP_OK &&
    unhex(digest, adapter.expected, 32) &&
    httpd_req_get_hdr_value_str(request, "X-Mast-Signature", signature, sizeof(signature)) == ESP_OK &&
    unhex(signature, sig, 64) && publicProvisioningReady() &&
    unhex(operatorPublicKey(), key, 32);
  const int length = snprintf(manifest, sizeof(manifest), "meshcore-esp-update-v1\nesp32s3\n%s\n%u\n%s\n",
                              ONCHIP_UPDATE_TARGET, unsigned(request->content_len), authorized ? digest : "");
  authorized = authorized && length > 0 && size_t(length) < sizeof(manifest) &&
    mesh::Identity(key).verify(sig, reinterpret_cast<uint8_t *>(manifest), length);
  if (authorized) memcpy(updateDigest, digest, sizeof(updateDigest));
  if (!update.start(adapter, request->content_len, adapter.partition ? adapter.partition->size : 0, authorized)) {
    busy = false;
    return statusLocked(request, authorized ? "400 Bad Request" : "403 Forbidden");
  }
  // The HTTP worker has an 8 KiB stack shared with Ed25519 verification.
  const uint32_t started = millis();
  uint32_t lastData = started;
  while (update.received() < update.size()) {
    if (uint32_t(millis() - started) > 180000 || uint32_t(millis() - lastData) > 10000) {
      update.fail("Application transfer timed out"); break;
    }
    const size_t remaining = update.size() - update.received();
    const int got = httpd_req_recv(request, reinterpret_cast<char *>(transferBuffer),
                                 remaining < sizeof(transferBuffer) ? remaining : sizeof(transferBuffer));
    if (got == HTTPD_SOCK_ERR_TIMEOUT) { delay(1); continue; }
    if (got <= 0) { update.fail("Application transfer incomplete"); break; }
    lastData = millis();
    if (!update.write(transferBuffer, got)) break;
    delay(1);
  }
  if (update.state() == EspUpdate::Receiving) update.finish();
  busy = update.state() == EspUpdate::Verified;
  return statusLocked(request, busy ? "200 OK" : "400 Bad Request");
}
esp_err_t rebootEspUpdate(httpd_req_t *request) {
  std::lock_guard<std::mutex> lock(updateMutex);
  char rollback[16];
  if (httpd_req_get_hdr_value_len(request, "X-Mast-Rollback")) {
    if (httpd_req_get_hdr_value_str(request, "X-Mast-Rollback", rollback, sizeof(rollback)) != ESP_OK ||
        strcmp(rollback, "previous"))
      return response(request, "400 Bad Request", "{\"error\":\"X-Mast-Rollback must be previous\"}");
    if (healthy || !rollbackObserved || !esp_ota_check_rollback_is_possible())
      return response(request, "409 Conflict", "{\"error\":\"No unconfirmed application with a valid rollback slot\"}");
    const auto result = statusLocked(request, "200 OK");
    if (result == ESP_OK) { rollbackRequested = true; rebootRequested = true; }
    return result;
  }
  if (update.state() != EspUpdate::Verified)
    return response(request, "409 Conflict", "{\"error\":\"No verified application awaiting reboot\"}");
  const esp_err_t result = statusLocked(request, "200 OK");
  if (result == ESP_OK) rebootRequested = true;
  return result;
}
void espUpdateRecovery(const char *reason) { recovery = reason; }
void serviceEspUpdate(bool rolesReady, bool wifiReady, bool wifiEnabled) {
  rolesReady = rolesReady && publicProvisioningReady();
  esp_ota_img_states_t state;
  if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
      state == ESP_OTA_IMG_PENDING_VERIFY) rollbackObserved = true;
  // AP/NTP outages do not cause reset loops. Confirmation waits for a useful
  // network service, unless station joining was deliberately disabled.
  if (!healthy && !rollbackRequested && rolesReady && (wifiReady || !wifiEnabled)) {
    if (!healthAt) healthAt = millis();
    if (uint32_t(millis() - healthAt) >= 30000) {
      if (!rollbackObserved || esp_ota_mark_app_valid_cancel_rollback() == ESP_OK)
        healthy = true;
    }
  } else if (!healthy) healthAt = 0;
  if (!healthy && !rolesReady && rollbackObserved && !rollbackRequested) {
    if (!unhealthyAt) unhealthyAt = millis();
    if (!automaticRollbackAttempted && uint32_t(millis() - unhealthyAt) >= 300000 &&
        esp_ota_check_rollback_is_possible()) {
      automaticRollbackAttempted = true;
      recovery = "Radio, role or HTTP startup remained unhealthy; returning to previous application";
      esp_ota_mark_app_invalid_rollback_and_reboot();
      recovery = "Automatic application rollback failed; use retained USB recovery";
    }
  } else unhealthyAt = 0;
  if (rebootRequested && !rebootAt) rebootAt = millis();
  if (rebootAt && uint32_t(millis() - rebootAt) >= 1500) {
    if (rollbackRequested) {
      // On success this IDF call does not return.
      esp_ota_mark_app_invalid_rollback_and_reboot();
      rollbackRequested = false; rebootRequested = false; rebootAt = 0;
      recovery = "Application rollback failed; use retained USB recovery";
    } else ESP.restart();
  }
}
} // namespace onchip
#endif
