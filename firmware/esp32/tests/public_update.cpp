// SPDX-License-Identifier: Apache-2.0
#include "../EspFieldUpdate.h"
#include "../Config.h"
#include <cassert>

namespace {
onchip::PublicProvisioningRecord record{};
bool selected = false;
std::string hex(const uint8_t *bytes, size_t size) {
  std::string result;
  for (size_t i = 0; i < size; ++i) {
    char pair[3];
    snprintf(pair, sizeof(pair), "%02x", bytes[i]);
    result += pair;
  }
  return result;
}
httpd_req_t candidate(uint8_t seedByte) {
  httpd_req_t request;
  request.data.resize(8193, 0x55);
  request.data[0] = 0xe9;
  request.data[12] = 9;
  request.data[13] = 0;
  const char target[] = "meshcore-esp-target-v1:xiao-esp32s3:";
  memcpy(request.data.data() + 4080, target, sizeof(target));
  request.content_len = request.data.size();
  uint8_t digest[32], seed[32]{}, signature[64];
  seed[0] = seedByte;
  unsigned size;
  assert(EVP_Digest(request.data.data(), request.data.size(), digest, &size, EVP_sha256(), nullptr) == 1);
  request.headers["X-Mast-Target"] = "xiao-esp32s3";
  request.headers["X-Mast-SHA256"] = hex(digest, sizeof(digest));
  const auto message = std::string("meshcore-esp-update-v1\nesp32s3\nxiao-esp32s3\n") +
      std::to_string(request.content_len) + "\n" + request.headers["X-Mast-SHA256"] + "\n";
  EVP_PKEY *key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, seed, sizeof(seed));
  EVP_MD_CTX *context = EVP_MD_CTX_new();
  size_t signatureSize = sizeof(signature);
  assert(EVP_DigestSignInit(context, nullptr, nullptr, nullptr, key) == 1);
  assert(EVP_DigestSign(context, signature, &signatureSize,
                       reinterpret_cast<const uint8_t *>(message.data()), message.size()) == 1);
  EVP_MD_CTX_free(context);
  EVP_PKEY_free(key);
  request.headers["X-Mast-Signature"] = hex(signature, sizeof(signature));
  return request;
}
void loadAuthority(uint8_t seedByte) {
  uint8_t seed[32]{}, publicKey[32];
  seed[0] = seedByte;
  EVP_PKEY *key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, seed, sizeof(seed));
  size_t size = sizeof(publicKey);
  assert(EVP_PKEY_get_raw_public_key(key, publicKey, &size) == 1);
  EVP_PKEY_free(key);
  strcpy(record.operatorPublicKey, hex(publicKey, sizeof(publicKey)).c_str());
  selected = true;
}
} // namespace
namespace onchip {
// Exercise the production updater's configuration interface independently of
// the SPIFFS parser, which public_provisioning.cpp exercises against stored bytes.
bool publicProvisioningReady() { return selected; }
const PublicProvisioningRecord &publicProvisioning() { return record; }
}
int main(int argc, char **) {
  const uint8_t authority = argc > 1 ? 2 : 1;
  bootState = ESP_OTA_IMG_PENDING_VERIFY;
  runningImage = candidate(0).data;
  onchip::serviceEspUpdate(true, false, false);
  testMillis += 31000;
  onchip::serviceEspUpdate(true, false, false);
  assert(confirms == 0);
  auto request = candidate(0);
  onchip::uploadEspUpdate(&request);
  assert(begins == 0 && request.status == "409 Conflict");
  loadAuthority(authority);
  onchip::serviceEspUpdate(true, false, false);
  testMillis += 31000;
  onchip::serviceEspUpdate(true, false, false);
  assert(confirms == 1 && bootState == ESP_OTA_IMG_VALID);
  request = candidate(authority == 1 ? 0 : 1);
  onchip::uploadEspUpdate(&request);
  assert(begins == 0 && request.status == "403 Forbidden");
  unsigned flushes = 0;
  static bool flushOk = false;
  static unsigned *flushCount = nullptr; flushCount = &flushes;
  onchip::setEspUpdateFlush([] { ++*flushCount; return flushOk; });
  onDelay = [] { onchip::serviceEspUpdate(true, false, false); };
  request = candidate(authority);
  onchip::uploadEspUpdate(&request);
  assert(begins == 0 && switches == 0 && request.status == "503 Service Unavailable" && flushes == 1);
  if (argc > 2) {
    request = candidate(authority == 1 ? 0 : 1);
    request.headers["X-Mast-Allow-Unsaved-State"] = "1";
    onchip::uploadEspUpdate(&request);
    assert(begins == 0 && request.status == "403 Forbidden");
    request = candidate(authority);
    request.headers["X-Mast-Allow-Unsaved-State"] = "1";
    request.data.clear(); // failed transfer must not retain the exception
    onchip::uploadEspUpdate(&request);
    assert(request.response.find("\"allow_unsaved_state\":false") != std::string::npos);
    const unsigned before = begins;
    request = candidate(authority);
    onchip::uploadEspUpdate(&request);
    assert(begins == before && request.status == "503 Service Unavailable");
    request = candidate(authority);
    request.headers["X-Mast-Allow-Unsaved-State"] = "1";
    onchip::uploadEspUpdate(&request);
    assert(switches == 0 && request.status == "200 OK");
    assert(request.response.find("\"state_flush_failed\":true") != std::string::npos);
    onchip::rebootEspUpdate(&request);
    onchip::serviceEspUpdate(true, false, false); testMillis += 1500;
    onchip::serviceEspUpdate(true, false, false);
    assert(ESP.restarts == 1);
    puts("Signed application recovery survives failed upload, activation and reboot flushes; exception clears after failed upload");
    return 0;
  }
  flushOk = true;
  request = candidate(authority);
  onchip::uploadEspUpdate(&request);
  assert(begins == 1 && switches == 0 && request.response.find("\"state\":\"verified\"") != std::string::npos);
  assert(flushes == 3); // dispatcher flush before flash and before boot selection
  flushOk = false;
  onchip::rebootEspUpdate(&request);
  onchip::serviceEspUpdate(true, false, false); testMillis += 1500;
  onchip::serviceEspUpdate(true, false, false);
  assert(ESP.restarts == 0 && flushes == 4);
  puts("Public OTA uses selected operator authority; dispatch flush failure prevents flash/reboot; unprovisioned boot cannot confirm healthy");
}
