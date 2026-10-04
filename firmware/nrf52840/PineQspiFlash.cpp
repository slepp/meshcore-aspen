#if defined(NRF52_PLATFORM)
#include "PineQspiFlash.h"
#include "NotePartition.h"
#include <Arduino.h>
#include <variant.h>

namespace nrfmast {
namespace {
constexpr uint32_t CHIP_BYTES = 0x200000;
bool inNoteRegion(uint32_t address, size_t bytes) {
  return address >= NoteJournal::BASE && address < NoteJournal::BASE + NoteJournal::BYTES &&
         bytes <= NoteJournal::BASE + NoteJournal::BYTES - address;
}
}

void PineQspiFlash::event(nrfx_qspi_evt_t value, void* context) {
  if (value == NRFX_QSPI_EVENT_DONE) static_cast<PineQspiFlash*>(context)->completed = true;
}

bool PineQspiFlash::wait(nrfx_err_t result, uint32_t timeout) {
  if (result != NRFX_SUCCESS) return false;
  const uint32_t start = millis();
  while (!completed && uint32_t(millis() - start) < timeout) delay(1);
  if (!completed) { nrfx_qspi_uninit(); usable = false; return false; }
  nrfx_err_t busy;
  while ((busy = nrfx_qspi_mem_busy_check()) == NRFX_ERROR_BUSY) {
    if (uint32_t(millis() - start) >= timeout) {
      nrfx_qspi_uninit();
      usable = false;
      return false;
    }
    delay(1);
  }
  return busy == NRFX_SUCCESS;
}

bool PineQspiFlash::custom(uint8_t opcode, uint8_t* output, size_t bytes) {
  nrf_qspi_cinstr_conf_t config = {};
  config.opcode = opcode;
  config.length = static_cast<nrf_qspi_cinstr_len_t>(bytes + 1);
  config.io2_level = config.io3_level = true;
  return nrfx_qspi_cinstr_xfer(&config, nullptr, output) == NRFX_SUCCESS;
}

bool PineQspiFlash::begin() {
  if (!mutex) mutex = xSemaphoreCreateRecursiveMutexStatic(&mutexStorage);
  if (!mutex) return false;
  Lock lock(*this);
  if (initialized) return usable;
  nrfx_qspi_config_t config = {};
  config.pins.sck_pin = g_ADigitalPinMap[PIN_QSPI_SCK];
  config.pins.csn_pin = g_ADigitalPinMap[PIN_QSPI_CS];
  config.pins.io0_pin = g_ADigitalPinMap[PIN_QSPI_IO0];
  config.pins.io1_pin = g_ADigitalPinMap[PIN_QSPI_IO1];
  config.pins.io2_pin = g_ADigitalPinMap[PIN_QSPI_IO2];
  config.pins.io3_pin = g_ADigitalPinMap[PIN_QSPI_IO3];
  config.prot_if.readoc = NRF_QSPI_READOC_FASTREAD;
  config.prot_if.writeoc = NRF_QSPI_WRITEOC_PP;
  config.prot_if.addrmode = NRF_QSPI_ADDRMODE_24BIT;
  config.phy_if.sck_freq = NRF_QSPI_FREQ_32MDIV2;
  config.irq_priority = 7;
  if (nrfx_qspi_init(&config, event, this) != NRFX_SUCCESS) return false;
  initialized = true;
  if (!custom(0xab, nullptr, 0)) return false;
  delay(10);
  uint8_t id[3] = {};
  if (!custom(0x9f, id, sizeof(id))) return false;
  detected = uint32_t(id[0]) << 16 | uint32_t(id[1]) << 8 | id[2];
  usable = detected == 0x856015;  // P25Q16H, the pinned XIAO variant's 2MiB part.
  return usable;
}

bool PineQspiFlash::read(uint32_t address, void* output, size_t bytes) {
  if (!mutex) return false;
  Lock lock(*this);
  if (!usable || !output || !bytes || (address & 3) ||
      (reinterpret_cast<uintptr_t>(output) & 3) || (bytes & 3) ||
      address >= CHIP_BYTES || bytes > CHIP_BYTES - address) return false;
  completed = false;
  return wait(nrfx_qspi_read(output, bytes, address), 1000);
}

bool PineQspiFlash::program(uint32_t address, const void* input, size_t bytes) {
  if (!mutex) return false;
  Lock lock(*this);
  if (!usable || !input || !bytes || (address & 3) ||
      (reinterpret_cast<uintptr_t>(input) & 3) || (bytes & 3) ||
      !writable(address, bytes)) return false;
  const auto* data = static_cast<const uint8_t*>(input);
  while (bytes) {
    size_t count = 256 - (address & 255);
    if (count > bytes) count = bytes;
    completed = false;
    if (!wait(nrfx_qspi_write(data, count, address), 1000)) return false;
    address += count;
    data += count;
    bytes -= count;
  }
  return true;
}

bool PineQspiFlash::erase(uint32_t address) {
  if (!mutex) return false;
  Lock lock(*this);
  if (!usable || (address % NoteJournal::SECTOR) || !writable(address, NoteJournal::SECTOR))
    return false;
  completed = false;
  return wait(nrfx_qspi_erase(NRF_QSPI_ERASE_LEN_4KB, address), 2000);
}

bool PineQspiFlash::writable(uint32_t address, size_t bytes) const {
  return inNoteRegion(address, bytes) ||
      (filesystemWritable && address < NoteJournal::BASE && bytes <= NoteJournal::BASE - address);
}

bool PineQspiFlash::partitionFree(uint32_t address, uint32_t bytes) {
  if (!usable || address % NoteJournal::SECTOR || bytes % NoteJournal::SECTOR ||
      !inNoteRegion(address, bytes)) return false;
  return notePartitionFree(*this, address, bytes, &partition);
}

bool PineQspiFlash::retiredPartition(uint32_t address, uint32_t bytes) {
  if (!usable || detected != 0x856015 || address != NoteJournal::BASE ||
      bytes != NoteJournal::BYTES) return false;
  partition = {};
  partition.checked = partition.retired = true;
  return true;
}
}  // namespace nrfmast
#endif
