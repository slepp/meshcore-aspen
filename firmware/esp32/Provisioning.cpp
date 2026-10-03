// SPDX-License-Identifier: Apache-2.0
#if defined(MESHCORE_PUBLIC_PROVISIONING) && MESHCORE_PUBLIC_PROVISIONING
#include "Provisioning.h"
#include <Arduino.h>
#include <SPIFFS.h>
#include <Utils.h>
#include <KissModem.h>
#include <string.h>
#if defined(MESHCORE_ONCHIP_BOT) && MESHCORE_ONCHIP_BOT
#include "BotSettings.h"
#include <nvs.h>
#endif

namespace onchip {
namespace {
PublicProvisioningRecord selected{};
bool ready = false;
void wipe(void *memory, size_t size) {
  auto *bytes = static_cast<volatile uint8_t *>(memory);
  while (size--) *bytes++ = 0;
}
bool zeros(const uint8_t *bytes, size_t size) {
  for (size_t i = 0; i < size; ++i)
    if (bytes[i]) return false;
  return true;
}
bool text(const char *value, size_t capacity, bool required) {
  const char *end = static_cast<const char *>(memchr(value, 0, capacity));
  if (!end || (required && end == value)) return false;
  for (const char *p = value; p != end; ++p)
    if (uint8_t(*p) < 32 || uint8_t(*p) > 126) return false;
  return zeros(reinterpret_cast<const uint8_t *>(end), capacity - (end - value));
}
bool publicKey(const char (&value)[65], bool required) {
  if (!text(value, sizeof(value), required)) return false;
  if (!value[0]) return !required;
  if (strlen(value) != 64) return false;
  bool nonzero = false, nonff = false;
  for (unsigned i = 0; i < 64; ++i) {
    const char c = value[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    nonzero |= c != '0';
    nonff |= c != 'f';
  }
  return nonzero && nonff;
}
bool failure(const char *condition) {
  ready = false;
  wipe(&selected, sizeof(selected));
  Serial.printf("Public setup %s; RF and administration disabled; restore private setup over USB\n",
                condition);
  return false;
}
} // namespace
uint32_t provisioningUint32(const uint8_t bytes[4]) {
  return uint32_t(bytes[0]) | uint32_t(bytes[1]) << 8 |
         uint32_t(bytes[2]) << 16 | uint32_t(bytes[3]) << 24;
}
bool validatePublicProvisioning(const PublicProvisioningRecord &record) {
  uint8_t digest[32];
  mesh::Utils::sha256(digest, sizeof(digest),
                      reinterpret_cast<const uint8_t *>(&record),
                      offsetof(PublicProvisioningRecord, digest));
  const bool hashValid = !memcmp(digest, record.digest, sizeof(digest));
  wipe(digest, sizeof(digest));
  const uint32_t frequency = provisioningUint32(record.frequencyHz);
  const uint32_t bandwidth = provisioningUint32(record.bandwidthHz);
  const bool validBandwidth = bandwidth == 7800 || bandwidth == 10400 ||
      bandwidth == 15600 || bandwidth == 20800 || bandwidth == 31250 ||
      bandwidth == 41700 || bandwidth == 62500 || bandwidth == 125000 ||
      bandwidth == 250000 || bandwidth == 500000;
  const size_t wifiLength = strnlen(record.wifiPassword, sizeof(record.wifiPassword));
  bool wifiValid = !wifiLength || (wifiLength >= 8 && wifiLength <= 63);
  if (wifiLength == 64) {
    wifiValid = true;
    for (unsigned i = 0; i < 64; ++i) {
      const char c = record.wifiPassword[i];
      wifiValid &= (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                   (c >= 'A' && c <= 'F');
    }
  }
  return hashValid && !memcmp(record.magic, "MCP\1", 4) &&
      !(record.roles & ~15u) && record.pathWidth >= 1 && record.pathWidth <= 3 &&
      zeros(record.reserved, sizeof(record.reserved)) && !record.radioReserved &&
      zeros(record.tailReserved, sizeof(record.tailReserved)) &&
      frequency >= 150000000 && frequency <= 960000000 && validBandwidth &&
      record.sf >= 5 && record.sf <= 12 && record.cr >= 5 && record.cr <= 8 &&
      record.txDbm <= 22 &&
      text(record.adminPassword, sizeof(record.adminPassword), true) &&
      text(record.roomPassword, sizeof(record.roomPassword), false) &&
      text(record.mastPassword, sizeof(record.mastPassword), true) &&
      publicKey(record.operatorPublicKey, true) &&
      publicKey(record.trustedCompanionPublicKey, false) &&
      text(record.wifiSsid, sizeof(record.wifiSsid), false) &&
      text(record.wifiPassword, sizeof(record.wifiPassword), false) && wifiValid &&
      record.wifiEnabled <= 1 && (!record.wifiEnabled || record.wifiSsid[0]);
}
bool beginPublicProvisioning() {
  ready = false;
  wipe(&selected, sizeof(selected));
  if (!SPIFFS.begin(false)) return failure("filesystem unavailable (no automatic format)");
  auto layout = SPIFFS.open("/onchip-layout", "r");
  constexpr char expected[] = "meshcore-onchip-fs-v1\n";
  char actual[sizeof(expected) - 1]{};
  if (!layout || layout.size() != sizeof(actual) ||
      layout.read(reinterpret_cast<uint8_t *>(actual), sizeof(actual)) != sizeof(actual) ||
      memcmp(actual, expected, sizeof(actual)))
    return failure("filesystem layout invalid");
  layout.close();
  auto file = SPIFFS.open("/public-setup.bin", "r");
  if (!file) return failure("record missing");
  if (file.size() != sizeof(selected) ||
      file.read(reinterpret_cast<uint8_t *>(&selected), sizeof(selected)) != sizeof(selected) ||
      file.size() != sizeof(selected) || !validatePublicProvisioning(selected)) {
    file.close();
    return failure("record invalid or truncated");
  }
  file.close();
  ready = true;
  Serial.println("Public setup loaded; saved role preferences and identities retained");
  return true;
}
bool publicProvisioningReady() { return ready; }
bool loadPublicInitialRadio(RadioConfig &radio) {
  if (!ready) {
    Serial.println("Public setup unavailable; shared radio configuration refused");
    return false;
  }
  radio = {provisioningUint32(selected.frequencyHz),
           provisioningUint32(selected.bandwidthHz),
           selected.sf, selected.cr, selected.txDbm};
  return true;
}
bool initializePublicRuntimePreferences() {
  if (!ready) return failure("record not loaded");
#if defined(MESHCORE_ONCHIP_BOT) && MESHCORE_ONCHIP_BOT
  nvs_handle_t handle;
  esp_err_t result = nvs_open("mc-onchip", NVS_READONLY, &handle);
  if (result == ESP_OK) {
    size_t size = 0;
    result = nvs_get_blob(handle, "bot-radio", nullptr, &size);
    nvs_close(handle);
  }
  if (result != ESP_OK && result != ESP_ERR_NVS_NOT_FOUND)
    return failure("bot radio preferences read failed");
  BotRadioPolicy policy;
  if (result == ESP_OK) {
    if (!loadBotRadioPolicy(policy)) return failure("saved bot radio preferences invalid");
  } else {
    policy.pathWidth = selected.pathWidth;
    if (!saveBotRadioPolicy(policy)) return failure("initial bot radio preferences commit failed");
  }
#endif
  return true;
}
const PublicProvisioningRecord &publicProvisioning() { return selected; }
} // namespace onchip
#endif
