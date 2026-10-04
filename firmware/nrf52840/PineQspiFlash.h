#pragma once
#if defined(NRF52_PLATFORM)
#include "NoteJournal.h"
#include "NotePartition.h"
#include <nrfx_qspi.h>
#include <rtos.h>

namespace nrfmast {
class PineQspiFlash : public NoteFlash {
  bool initialized = false, usable = false;
  volatile bool completed = false;
  uint32_t detected = 0;
  NotePartitionStatus partition;
  bool filesystemWritable = false;
  StaticSemaphore_t mutexStorage{};
  SemaphoreHandle_t mutex = nullptr;
  struct Lock {
    PineQspiFlash& flash;
    explicit Lock(PineQspiFlash& f) : flash(f) { xSemaphoreTakeRecursive(flash.mutex, portMAX_DELAY); }
    ~Lock() { xSemaphoreGiveRecursive(flash.mutex); }
  };
  bool writable(uint32_t address, size_t bytes) const;
  static void event(nrfx_qspi_evt_t event, void* context);
  bool wait(nrfx_err_t result, uint32_t timeout);
  bool custom(uint8_t opcode, uint8_t* output, size_t bytes);

public:
  void authorizeFilesystem(bool authorized) { filesystemWritable = authorized; }
  bool begin() override;
  bool partitionFree(uint32_t address, uint32_t bytes) override;
  bool retiredPartition(uint32_t address, uint32_t bytes) override;
  bool read(uint32_t address, void* output, size_t bytes) override;
  bool program(uint32_t address, const void* input, size_t bytes) override;
  bool erase(uint32_t address) override;
  uint32_t jedecId() const { return detected; }
  const NotePartitionStatus& partitionStatus() const { return partition; }
};
}  // namespace nrfmast
#endif
