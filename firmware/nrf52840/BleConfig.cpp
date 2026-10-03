#include "BleConfig.h"

namespace nrfmast {

static constexpr uint8_t MAGIC[8] = {'P', 'N', 'B', 'L', 'E', '0', '1', 0};
static constexpr const char* LIVE = "/pine-ble";
static constexpr const char* STAGE = "/pine-ble.new";

bool BleConfig::begin() {
  pin = 0;
  enabled = false;
  if (!fs.exists(LIVE)) return true;
  auto file = fs.open(LIVE);
  Record record = {};
  const bool ok = file && file.size() == sizeof(record) &&
                  file.read(reinterpret_cast<uint8_t*>(&record), sizeof(record)) == sizeof(record);
  file.close();
  if (!ok || memcmp(record.magic, MAGIC, sizeof(MAGIC)) || record.enabled > 1 ||
      record.reserved[0] || record.reserved[1] || record.reserved[2] ||
      (record.pin != 0 && (record.pin < 100000 || record.pin > 999999)) ||
      (record.enabled && !record.pin)) return false;
  pin = record.pin;
  enabled = record.enabled;
  return true;
}

bool BleConfig::save(uint32_t newPin, bool newEnabled) {
  Record record = {}, saved = {};
  memcpy(record.magic, MAGIC, sizeof(MAGIC));
  record.pin = newPin;
  record.enabled = newEnabled;
  fs.remove(STAGE);
#if defined(NRF52_PLATFORM)
  auto file = fs.open(STAGE, FILE_O_WRITE);
#else
  auto file = fs.open(STAGE, "w");
#endif
  if (!file) return false;
  const bool written = file.write(reinterpret_cast<const uint8_t*>(&record), sizeof(record)) == sizeof(record);
  file.close();
  file = fs.open(STAGE);
  bool ok = written && file && file.size() == sizeof(record) &&
            file.read(reinterpret_cast<uint8_t*>(&saved), sizeof(saved)) == sizeof(saved) &&
            !memcmp(&record, &saved, sizeof(record));
  file.close();
  ok = ok && fs.rename(STAGE, LIVE);
  if (!ok) fs.remove(STAGE);
  if (ok) { pin = newPin; enabled = newEnabled; }
  return ok;
}

}  // namespace nrfmast
