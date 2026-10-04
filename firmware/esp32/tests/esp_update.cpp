// SPDX-License-Identifier: Apache-2.0
#include "../EspFieldUpdate.h"
#include "../EspUpdate.h"
#include <cassert>
#include <iostream>
extern "C" bool verifyRollbackLater();
std::string hex(const uint8_t *p, size_t n) {
  std::string result;
  for (size_t i = 0; i < n; ++i) { char b[3]; snprintf(b, sizeof(b), "%02x", p[i]); result += b; }
  return result;
}
httpd_req_t candidate(uint8_t chip = 9, bool includeTarget = true) {
  httpd_req_t r;
  r.data.resize(8193, 0x55); r.data[0] = 0xe9; r.data[12] = chip; r.data[13] = 0;
  const char marker[] = "meshcore-esp-target-v1:xiao-esp32s3:";
  // Exercise target-marker detection across receive-buffer boundaries.
  if (includeTarget) memcpy(r.data.data() + 4080, marker, sizeof(marker));
  r.content_len = r.data.size();
  uint8_t hash[32], seed[32]{}, signature[64];
  mbedtls_sha256_context h; mbedtls_sha256_init(&h); mbedtls_sha256_starts_ret(&h, 0);
  mbedtls_sha256_update_ret(&h, r.data.data(), r.data.size()); mbedtls_sha256_finish_ret(&h, hash); mbedtls_sha256_free(&h);
  r.headers["X-Mast-Target"] = "xiao-esp32s3"; r.headers["X-Mast-SHA256"] = hex(hash, 32);
  auto message = std::string("meshcore-esp-update-v1\nesp32s3\nxiao-esp32s3\n") +
    std::to_string(r.content_len) + "\n" + hex(hash, 32) + "\n";
  auto *key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, seed, 32);
  auto *ctx = EVP_MD_CTX_new(); size_t n = 64;
  assert(EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, key) == 1);
  assert(EVP_DigestSign(ctx, signature, &n, reinterpret_cast<const uint8_t *>(message.data()), message.size()) == 1);
  EVP_MD_CTX_free(ctx); EVP_PKEY_free(key);
  r.headers["X-Mast-Signature"] = hex(signature, 64);
  return r;
}
void rejected(httpd_req_t r, bool preflight = false) {
  const unsigned before = begins;
  assert(onchip::uploadEspUpdate(&r) == ESP_OK);
  assert(r.response.find("\"state\":\"failed\"") != std::string::npos);
  assert(!onchip::espUpdateBusy() && switches == 0 && ESP.restarts == 0);
  if (preflight) assert(begins == before);
  httpd_req_t reboot; onchip::rebootEspUpdate(&reboot);
  assert(reboot.status == "409 Conflict");
}
void duringWrite() {
  auto second = candidate(); onchip::uploadEspUpdate(&second);
  assert(second.status == "409 Conflict");
}
int main(int argc, char **argv) {
  const bool unsupported = argc > 1 && !strcmp(argv[1], "unsupported-bootloader");
  const bool recover = argc > 1 && !strcmp(argv[1], "recovery");
  const bool rollbackError = argc > 1 &&
    (!strcmp(argv[1], "rollback-error") || !strcmp(argv[1], "automatic-rollback-error"));
  const bool automaticRecovery = argc > 1 &&
    (!strcmp(argv[1], "automatic-recovery") || !strcmp(argv[1], "automatic-rollback-error"));
  failRollback = rollbackError;
  const bool runningHash = argc > 1 && !strcmp(argv[1], "running-hash");
  const bool runningHashError = argc > 1 && !strcmp(argv[1], "running-hash-error");
  const auto runningCandidate = candidate();
  runningImage = runningCandidate.data;
  if (runningHash || runningHashError) {
    failRead = runningHashError;
    httpd_req_t status;
    if (runningHashError) {
      noRunningPartition = true;
      onchip::statusEspUpdate(&status);
      assert(status.response.find("\"running_sha256\":\"\"") != std::string::npos);
      assert(status.response.find("\"running_size\":0") != std::string::npos);
      assert(status.response.find("Running application partition unavailable") != std::string::npos);
      noRunningPartition = false;
    }
    onchip::statusEspUpdate(&status);
    if (runningHash) {
      assert(status.response.find("\"running_sha256\":\"" + runningCandidate.headers.at("X-Mast-SHA256") + "\"") != std::string::npos);
      assert(status.response.find("\"sha256\":\"\"") != std::string::npos);
      assert(status.response.find("\"running_size\":8193") != std::string::npos);
    } else {
      assert(status.response.find("\"running_sha256\":\"\"") != std::string::npos);
      assert(status.response.find("\"running_size\":0") != std::string::npos);
      assert(status.response.find("Running application SHA256 read failed") != std::string::npos);
      failRead = false; onchip::statusEspUpdate(&status);
      assert(status.response.find("\"running_sha256\":\"" + runningCandidate.headers.at("X-Mast-SHA256") + "\"") != std::string::npos);
    }
    std::cout << "ESP post-reboot raw running-image digest readback passed\n"; return 0;
  }
  runningImage[400] ^= 1;
  assert(verifyRollbackLater()); assert(confirms == 0);
  auto r = candidate(); r.headers.erase("X-Mast-Signature"); rejected(r, true);
  r = candidate(); r.headers["X-Mast-Target"] = "another-board"; rejected(r, true);
  r = candidate(); r.headers["X-Mast-Signature"][0] ^= 1; rejected(r, true);
  r = candidate(); inactive.size = 4096; rejected(r, true); inactive.size = 0x330000;
  r = candidate(); r.stop = 5000; rejected(r); assert(aborts);
  r = candidate(); r.data[200] ^= 1; rejected(r);
  rejected(candidate(0));
  rejected(candidate(9, false));
  failImage = true; rejected(candidate()); failImage = false;
  failWrite = true; rejected(candidate()); failWrite = false;
  failSwitch = true; rejected(candidate()); failSwitch = false;
  failLength = true; rejected(candidate()); failLength = false;
  if (recover || automaticRecovery || rollbackError) {
    bootState = ESP_OTA_IMG_PENDING_VERIFY;
    r = candidate(); const auto before = begins; onchip::uploadEspUpdate(&r);
    assert(r.status == "409 Conflict" && begins == before && switches == 0);
    onchip::espUpdateRecovery("Startup failed");
    onchip::serviceEspUpdate(false, false, true);
    if (automaticRecovery) {
      testMillis += 299999; onchip::serviceEspUpdate(false, false, true);
      assert(ESP.restarts == 0 && confirms == 0);
      testMillis += 1; onchip::serviceEspUpdate(false, false, true);
      assert(rollbacks == 1 && ESP.restarts == unsigned(!rollbackError) && confirms == 0 && switches == 0);
      onchip::serviceEspUpdate(false, false, true);
      assert(rollbacks == 1);
      if (rollbackError) {
        httpd_req_t status; onchip::statusEspUpdate(&status);
        assert(status.response.find("Automatic application rollback failed; use retained USB recovery") != std::string::npos);
      }
      std::cout << "ESP bounded unhealthy-role rollback passed\n"; return 0;
    }
    httpd_req_t rollback; rollback.headers["X-Mast-Rollback"] = "previous";
    onchip::rebootEspUpdate(&rollback); assert(rollback.status == "200 OK");
    onchip::serviceEspUpdate(false, false, true); testMillis += 1500;
    onchip::serviceEspUpdate(false, false, true);
    assert(rollbacks == 1 && ESP.restarts == unsigned(!rollbackError) && confirms == 0 && switches == 0);
    if (rollbackError) {
      onchip::serviceEspUpdate(false, false, true);
      assert(rollbacks == 1 && ESP.restarts == 0);
      httpd_req_t status; onchip::statusEspUpdate(&status);
      assert(status.response.find("Application rollback failed; use retained USB recovery") != std::string::npos);
    }
    std::cout << "ESP explicit pending-image recovery rollback passed\n"; return 0;
  }
  onWrite = duringWrite;
  r = candidate(); assert(onchip::uploadEspUpdate(&r) == ESP_OK);
  onWrite = nullptr;
  assert(r.response.find("\"state\":\"verified\"") != std::string::npos);
  assert(switches == 1 && ESP.restarts == 0 && onchip::espUpdateBusy());
  auto second = candidate(); onchip::uploadEspUpdate(&second); assert(second.status == "409 Conflict");
  httpd_req_t status; onchip::statusEspUpdate(&status); assert(status.response.find("\"received\":8193") != std::string::npos);
  assert(status.response.find("\"sha256\":\"" + r.headers.at("X-Mast-SHA256") + "\"") != std::string::npos);
  assert(status.response.find("\"running_sha256\":\"" + r.headers.at("X-Mast-SHA256") + "\"") == std::string::npos);
  if (!unsupported) bootState = ESP_OTA_IMG_PENDING_VERIFY;
  onchip::serviceEspUpdate(false, true, true); assert(confirms == 0);
  onchip::serviceEspUpdate(true, false, true); testMillis += 360000;
  onchip::serviceEspUpdate(true, false, true); assert(confirms == 0 && ESP.restarts == 0);
  onchip::serviceEspUpdate(true, true, true); testMillis += 30000;
  // A selected-source fault breaks the continuous health window even with HTTP up.
  onchip::serviceEspUpdate(false, true, true); assert(confirms == 0);
  testMillis += 30000;
  onchip::serviceEspUpdate(false, true, true); assert(confirms == 0);
  onchip::serviceEspUpdate(true, true, true); testMillis += 29999;
  onchip::serviceEspUpdate(true, true, true); assert(confirms == 0);
  testMillis += 1;
  onchip::serviceEspUpdate(true, true, true); assert(confirms == unsigned(!unsupported));
  onchip::statusEspUpdate(&status);
  assert(status.response.find(unsupported ? "\"rollback_supported\":false" : "\"rollback_supported\":true") != std::string::npos);
  httpd_req_t reboot; reboot.failSend = true;
  assert(onchip::rebootEspUpdate(&reboot) == -1);
  onchip::serviceEspUpdate(true, true, true); testMillis += 2000;
  onchip::serviceEspUpdate(true, true, true); assert(ESP.restarts == 0);
  reboot.failSend = false; onchip::rebootEspUpdate(&reboot);
  onchip::serviceEspUpdate(true, true, true); assert(ESP.restarts == 0);
  testMillis += 1500; onchip::serviceEspUpdate(true, true, true); assert(ESP.restarts == 1);
  std::cout << "ESP authenticated manifest/stream/boot-health tests passed\n";
}
