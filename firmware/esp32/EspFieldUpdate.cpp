// SPDX-License-Identifier: Apache-2.0
#if defined(ARDUINO_ARCH_ESP32) && defined(MESHCORE_MAST_ADMIN) && MESHCORE_MAST_ADMIN
#include "EspFieldUpdate.h"
#include "EspUpdate.h"
#include "Config.h"
#ifndef MESHCORE_ESP_UPDATE_TEST
#include <Arduino.h>
#include <Identity.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <esp_app_format.h>
#include <esp_image_format.h>
#include <esp_timer.h>
#include <esp_attr.h>
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
std::mutex forceMutex;
esp_timer_handle_t forceTimer = nullptr;
std::atomic<uint32_t> forceTicket{0};
uint32_t nextForceTicket = 0, forceDeadline = 0;
bool forceArmed = false, forceFired = false, forcePriorBusy = false;
std::atomic<bool> busy{false}, rebootRequested{false}, rollbackRequested{false}, healthy{false};
std::atomic<bool> rollbackObserved{false};
std::atomic<bool (*)()> flushRuntime{nullptr};
std::atomic<uint32_t> flushRequested{0}, flushCompleted{0};
std::atomic<bool> flushResult{false}, allowUnsavedState{false}, stateFlushFailed{false};
bool acceptFlush(bool saved) {
  if (!saved) stateFlushFailed = true;
  return saved || allowUnsavedState.load();
}
bool flushForUpdate() {
  if (!flushRuntime.load()) return true;
  const uint32_t ticket = flushRequested.fetch_add(1) + 1;
  const uint32_t started = millis();
  while (flushCompleted.load() != ticket && uint32_t(millis() - started) < 10000) delay(1);
  return acceptFlush(flushCompleted.load() == ticket && flushResult.load());
}
// A verified image is selected for boot only when the node is about to restart
// into it, so a cancelled reboot leaves the running slot selected.
std::atomic<const esp_partition_t *> stagedPartition{nullptr};
bool selectStagedBoot() {
  // A failed selection keeps the staged slot so the operator can retry the reboot.
  const esp_partition_t *partition = stagedPartition.load();
  if (!partition || esp_ota_set_boot_partition(partition) != ESP_OK) return false;
  stagedPartition = nullptr;
  return true;
}
void releaseForceRestart() {
  forceTicket = 0;
  busy = forcePriorBusy;
  forceArmed = false;
}
void forceRestartTimer(void *) {
  bool restart = false;
  {
    std::lock_guard<std::mutex> lock(forceMutex);
    // A previously queued expiry callback must not shorten a newly armed delay.
    if (!forceTicket || forceFired || int32_t(millis() - forceDeadline) < 0) return;
    if (forceArmed) { forceFired = true; restart = true; }
    else releaseForceRestart();
  }
  if (restart) {
    // The operator chose to restart into a verified image whose flush failed.
    selectStagedBoot();
    ESP.restart();
  }
}
uint32_t rebootAt = 0, healthAt = 0, unhealthyAt = 0, rolesAt = 0;
bool automaticRollbackAttempted = false;
std::atomic<const char *> recovery{""};
// The reason for a rollback survives the software reset in RTC memory. A power
// loss clears it; the record is shown once, on the boot that follows.
enum RollbackReason : uint8_t { RollbackNone, RollbackRoles, RollbackProvisioning, RollbackOperator };
struct RollbackRecord { uint32_t magic, check; uint32_t uptimeSeconds; uint8_t reason, wifi; };
constexpr uint32_t RollbackMagic = 0x52424b31;
RTC_NOINIT_ATTR RollbackRecord rollbackRecord;
char lastRollback[112]{};
bool rollbackLoaded = false;
uint32_t rollbackCheck(const RollbackRecord &r) {
  return RollbackMagic ^ (r.uptimeSeconds * 2654435761u) ^ (uint32_t(r.reason) << 8) ^ r.wifi;
}
void recordRollback(RollbackReason reason, bool wifiReady) {
  RollbackRecord next{RollbackMagic, 0, millis() / 1000, reason, uint8_t(wifiReady)};
  next.check = rollbackCheck(next);
  rollbackRecord = next;
}
void loadRollback() {
  if (rollbackLoaded) return;
  rollbackLoaded = true;
  const RollbackRecord record = rollbackRecord;
  rollbackRecord.magic = 0;
  if (record.magic != RollbackMagic || record.check != rollbackCheck(record)) return;
  const char *text = record.reason == RollbackRoles ? "radio, role or HTTP startup was not ready" :
    record.reason == RollbackProvisioning ? "public setup record was not provisioned" :
    record.reason == RollbackOperator ? "operator requested rollback" : nullptr;
  if (!text) return;
  snprintf(lastRollback, sizeof(lastRollback), "%s after %us; wifi %s",
           text, unsigned(record.uptimeSeconds), record.wifi ? "connected" : "not connected");
}
const char imageTarget[] = "meshcore-esp-target-v1:" ONCHIP_UPDATE_TARGET ":";
// Scratch for application hashing and upload chunks. It is allocated for the
// duration of one operation instead of reserving 4 KiB of internal RAM.
constexpr size_t TransferBytes = 4096;
struct TransferBuffer {
  uint8_t *data = static_cast<uint8_t *>(
      heap_caps_malloc(TransferBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  TransferBuffer() = default;
  TransferBuffer(const TransferBuffer &) = delete;
  TransferBuffer &operator=(const TransferBuffer &) = delete;
  ~TransferBuffer() { heap_caps_free(data); }
};
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
  TransferBuffer buffer;
  if (!buffer.data) {
    runningHashError = "Running application SHA256 needs 4 KiB of free internal memory; retry status";
    return;
  }
  mbedtls_sha256_context hash;
  mbedtls_sha256_init(&hash);
  bool ok = !mbedtls_sha256_starts_ret(&hash, 0);
  for (size_t offset = 0; ok && offset < metadata.image_len;) {
    const size_t remaining = metadata.image_len - offset;
    const size_t size = remaining < TransferBytes ? remaining : TransferBytes;
    ok = esp_partition_read(partition, offset, buffer.data, size) == ESP_OK &&
         !mbedtls_sha256_update_ret(&hash, buffer.data, size);
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
  bool activate() override {
    if (!flushForUpdate()) return false;
    stagedPartition = partition;
    return true;
  }
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
  loadRollback();
  const char *names[] = {"idle", "receiving", "failed", "verified"};
  static char bootId[17]{};
  if (!bootId[0]) snprintf(bootId, sizeof(bootId), "%08x%08x", unsigned(esp_random()), unsigned(esp_random()));
  char text[1280];
  snprintf(text, sizeof(text),
    "{\"boot_id\":\"%s\",\"state\":\"%s\",\"received\":%u,\"size\":%u,\"error\":\"%s\","
    "\"target\":\"%s\",\"boot_health\":\"%s\",\"recovery\":\"%s\","
    "\"force_restart_supported\":true,\"force_restart_pending\":%s,"
    "\"unsaved_state_supported\":true,\"allow_unsaved_state\":%s,\"state_flush_failed\":%s,"
    "\"reboot_ready\":%s,\"rollback_supported\":%s,\"rollback_ready\":%s,"
    "\"sha256\":\"%s\",\"running_sha256\":\"%s\",\"running_size\":%u,\"running_hash_error\":\"%s\",\"last_rollback\":\"%s\"}",
    bootId, names[update.state()], unsigned(update.received()), unsigned(update.size()), update.error(),
    ONCHIP_UPDATE_TARGET, healthy ? "healthy" : "pending", recovery.load(),
    forceTicket ? "true" : "false",
    allowUnsavedState ? "true" : "false", stateFlushFailed ? "true" : "false",
    update.state() == EspUpdate::Verified ? "true" : "false",
    rollbackObserved ? "true" : "false",
    !healthy && rollbackObserved && esp_ota_check_rollback_is_possible() ? "true" : "false",
    updateDigest, runningDigest, unsigned(runningSize), runningHashError, lastRollback);
  return response(request, code, text);
}
}
uint32_t reserveEspForceRestart(const char *&error, bool *unavailable) {
  error = nullptr;
  if (unavailable) *unavailable = false;
  std::unique_lock<std::mutex> updateLock(updateMutex, std::try_to_lock);
  if (!updateLock.owns_lock()) {
    error = "Application transfer or activation active; inspect /admin/update"; return 0;
  }
  std::lock_guard<std::mutex> lock(forceMutex);
  if (forceTicket) {
    error = "Restart already pending; inspect status before another request"; return 0;
  }
  if (busy && update.state() != EspUpdate::Verified) {
    error = "Application transfer active; inspect /admin/update"; return 0;
  }
  if (!forceTimer) {
    esp_timer_create_args_t args{};
    args.callback = forceRestartTimer;
    args.name = "mast-force-restart";
    if (esp_timer_create(&args, &forceTimer) != ESP_OK) {
      if (unavailable) *unavailable = true;
      error = "Forced restart timer unavailable"; return 0;
    }
  }
  forcePriorBusy = busy;
  forceArmed = forceFired = false;
  forceDeadline = millis() + 30000;
  if (++nextForceTicket == 0) ++nextForceTicket;
  forceTicket = nextForceTicket;
  busy = true;
  if (esp_timer_start_once(forceTimer, 30000000) != ESP_OK) {
    if (unavailable) *unavailable = true;
    releaseForceRestart(); error = "Forced restart timer could not start"; return 0;
  }
  return forceTicket;
}
bool acknowledgeEspForceRestart(uint32_t ticket, bool transmitted) {
  std::lock_guard<std::mutex> lock(forceMutex);
  if (!ticket || ticket != forceTicket || forceArmed || forceFired) return false;
  esp_timer_stop(forceTimer);
  if (!transmitted || int32_t(millis() - forceDeadline) >= 0) { releaseForceRestart(); return false; }
  forceDeadline = millis() + 1500;
  forceArmed = true;
  if (esp_timer_start_once(forceTimer, 1500000) != ESP_OK) {
    releaseForceRestart();
    recovery = "Forced restart timer failed; restart cancelled";
    return false;
  }
  return true;
}
void setEspUpdateFlush(bool (*flush)()) { flushRuntime = flush; }
bool espUpdateBusy() { return busy.load(); }
esp_err_t statusEspUpdate(httpd_req_t *request) {
  std::lock_guard<std::mutex> lock(updateMutex);
  return statusLocked(request, "200 OK");
}
esp_err_t uploadEspUpdate(httpd_req_t *request) {
  std::unique_lock<std::mutex> lock(updateMutex, std::try_to_lock);
  if (!lock.owns_lock())
    return response(request, "409 Conflict", "{\"error\":\"Application update operation active; inspect status\"}");
  bool expectedBusy = false;
  if (!busy.compare_exchange_strong(expectedBusy, true))
    return response(request, "409 Conflict", "{\"error\":\"Application update already active; inspect status\"}");
  updateDigest[0] = 0;
  stagedPartition = nullptr;
  allowUnsavedState = false; stateFlushFailed = false;
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
  char unsaved[8]{};
  if (httpd_req_get_hdr_value_len(request, "X-Mast-Allow-Unsaved-State")) {
    if (httpd_req_get_hdr_value_str(request, "X-Mast-Allow-Unsaved-State", unsaved, sizeof(unsaved)) != ESP_OK ||
        strcmp(unsaved, "1")) {
      busy = false;
      return response(request, "400 Bad Request", "{\"error\":\"X-Mast-Allow-Unsaved-State must be 1\"}");
    }
  }
  allowUnsavedState = authorized && unsaved[0];
  TransferBuffer buffer;
  if (authorized && !buffer.data) {
    busy = false;
    return response(request, "503 Service Unavailable", "{\"error\":\"Update needs 4 KiB of free internal memory; retry\"}");
  }
  if (authorized) memcpy(updateDigest, digest, sizeof(updateDigest));
  if (authorized && !flushForUpdate()) {
    busy = false;
    return response(request, "503 Service Unavailable", "{\"error\":\"Native state flush failed before update; inspect native role storage and bot contacts\"}");
  }
  if (!update.start(adapter, request->content_len, adapter.partition ? adapter.partition->size : 0, authorized)) {
    allowUnsavedState = false;
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
    const int got = httpd_req_recv(request, reinterpret_cast<char *>(buffer.data),
                                 remaining < TransferBytes ? remaining : TransferBytes);
    if (got == HTTPD_SOCK_ERR_TIMEOUT) { delay(1); continue; }
    if (got <= 0) { update.fail("Application transfer incomplete"); break; }
    lastData = millis();
    if (!update.write(buffer.data, got)) break;
    delay(1);
  }
  if (update.state() == EspUpdate::Receiving) update.finish();
  busy = update.state() == EspUpdate::Verified;
  if (!busy) { allowUnsavedState = false; stagedPartition = nullptr; }
  return statusLocked(request, busy ? "200 OK" : "400 Bad Request");
}
esp_err_t rebootEspUpdate(httpd_req_t *request) {
  std::lock_guard<std::mutex> lock(updateMutex);
  if (forceTicket)
    return response(request, "409 Conflict", "{\"error\":\"Forced restart already pending\"}");
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
  if (forceTicket) return;
  const uint32_t requested = flushRequested.load();
  if (requested != flushCompleted.load()) {
    const auto flush = flushRuntime.load();
    flushResult = !flush || flush(); flushCompleted = requested;
  }
  const bool provisioned = publicProvisioningReady();
  rolesReady = rolesReady && provisioned;
  esp_ota_img_states_t state;
  if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
      state == ESP_OTA_IMG_PENDING_VERIFY) rollbackObserved = true;
  // AP/NTP outages do not cause reset loops. Confirmation waits for a useful
  // network service, unless station joining was deliberately disabled. With
  // ready roles and no WiFi, the image is confirmed after ten minutes so a
  // later brownout does not revert a working radio.
  if (!healthy && !rollbackRequested && rolesReady) {
    if (!rolesAt) rolesAt = millis();
  } else rolesAt = 0;
  if (!healthy && !rollbackRequested && rolesReady && (wifiReady || !wifiEnabled)) {
    if (!healthAt) healthAt = millis();
  } else healthAt = 0;
  if (!healthy && !rollbackRequested && rolesReady &&
      ((healthAt && uint32_t(millis() - healthAt) >= 30000) ||
       (rolesAt && uint32_t(millis() - rolesAt) >= 600000))) {
    if (!rollbackObserved || esp_ota_mark_app_valid_cancel_rollback() == ESP_OK)
      healthy = true;
  }
  if (!healthy && !rolesReady && rollbackObserved && !rollbackRequested) {
    if (!unhealthyAt) unhealthyAt = millis();
    if (!automaticRollbackAttempted && uint32_t(millis() - unhealthyAt) >= 300000 &&
        esp_ota_check_rollback_is_possible()) {
      automaticRollbackAttempted = true;
      recovery = "Radio, role or HTTP startup remained unhealthy; returning to previous application";
      recordRollback(provisioned ? RollbackRoles : RollbackProvisioning, wifiReady);
      if (esp_ota_mark_app_invalid_rollback_and_reboot() != ESP_OK) rollbackRecord.magic = 0;
      recovery = "Automatic application rollback failed; use retained USB recovery";
    }
  } else unhealthyAt = 0;
  if (rebootRequested && !rebootAt) rebootAt = millis();
  if (rebootAt && uint32_t(millis() - rebootAt) >= 1500) {
    if (rollbackRequested) {
      // On success this IDF call does not return.
      recordRollback(RollbackOperator, wifiReady);
      if (esp_ota_mark_app_invalid_rollback_and_reboot() != ESP_OK) rollbackRecord.magic = 0;
      rollbackRequested = false; rebootRequested = false; rebootAt = 0;
      recovery = "Application rollback failed; use retained USB recovery";
    } else if (const auto flush = flushRuntime.load(); !acceptFlush(!flush || flush())) {
      rebootRequested = false; rebootAt = 0;
      recovery = "Native state flush failed; application reboot cancelled; inspect native role storage and bot contacts";
    } else if (update.state() == EspUpdate::Verified && !selectStagedBoot()) {
      rebootRequested = false; rebootAt = 0;
      recovery = "Application boot selection failed; reboot cancelled; request reboot again";
    } else ESP.restart();
  }
}
} // namespace onchip
#endif
