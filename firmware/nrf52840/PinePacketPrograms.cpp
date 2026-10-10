// SPDX-License-Identifier: Apache-2.0
#if NRFMAST_PRODUCTION_LUA
#include "PinePacketPrograms.h"
#include "PineFilesystem.h"
#include "PineRoom.h"
#include "PinePeerDevice.h"
#include "onchip/PacketPrograms.h"
#include "onchip/PacketLua.h"
#include "onchip/NativePacketContracts.h"
#include "platform/nvs.h"
#include <InternalFileSystem.h>
#include <Utils.h>
#include <atomic>
#include <memory>
#include <new>
using namespace Adafruit_LittleFS_Namespace;

namespace nrfmast {
namespace {
constexpr size_t SourceLimit = 4096;
constexpr uint32_t HeapLimit = 12288;
class Store final : public onchip::PacketProgramStore {
  static void path(uint8_t slot, uint8_t file, char (&text)[64]) {
    snprintf(text, sizeof(text), "/command-bot/packet-%u-%u", slot, file);
  }
public:
  bool journal(uint8_t slot, onchip::PacketProgramRecord &record, bool write) override {
    if (write) {
      if (record.capabilities != 0 || record.fuel > 10000 || record.microseconds > 2000) return false;
      for (const auto &source : record.sources)
        if (source.size > SourceLimit || source.runtime != onchip::PacketProgramRuntime::Lua) return false;
    }
    struct Journal { onchip::PacketProgramRecord record; uint8_t hash[32]; } next{};
    nvs_handle_t handle = 0;
    char key[16]; snprintf(key, sizeof(key), "packet%u", slot);
    auto status = nvs_open("pine-packets", write ? NVS_READWRITE : NVS_READONLY, &handle);
    if (!write && status == ESP_ERR_NVS_NOT_FOUND) { record = {}; return true; }
    if (status != ESP_OK) return false;
    if (write) {
      next.record = record;
      digest(reinterpret_cast<uint8_t *>(&next.record), sizeof(record), next.hash);
      status = nvs_set_blob(handle, key, &next, sizeof(next));
      if (status == ESP_OK) status = nvs_commit(handle);
    } else {
      size_t size = sizeof(next);
      status = nvs_get_blob(handle, key, &next, &size);
      if (status == ESP_ERR_NVS_NOT_FOUND) { record = {}; nvs_close(handle); return true; }
      if (size != sizeof(next)) status = ESP_FAIL;
    }
    nvs_close(handle);
    if (status != ESP_OK) return false;
    uint8_t hash[32]; digest(reinterpret_cast<uint8_t *>(&next.record), sizeof(record), hash);
    if (memcmp(hash, next.hash, 32)) return false;
    if (!write) record = next.record;
    if (record.capabilities != 0 || record.fuel > 10000 ||
        record.microseconds > 2000) return false;
    for (const auto &source : record.sources)
      if (source.size > SourceLimit || source.runtime != onchip::PacketProgramRuntime::Lua) return false;
    if (write) {
      onchip::PacketProgramRecord actual;
      return journal(slot, actual, false) && !memcmp(&actual, &record, sizeof(record));
    }
    return true;
  }
  bool read(uint8_t slot, uint8_t index, size_t offset, uint8_t *bytes, size_t size) override {
    char text[64]; path(slot, index, text);
    auto file = botFilesystem.open(text);
    return file && offset <= file.size() && size <= file.size() - offset &&
           file.seek(offset) && file.read(bytes, size) == size;
  }
  bool truncate(uint8_t slot, uint8_t index) override {
    char text[64]; path(slot, index, text);
    auto file = botFilesystem.open(text, "w"); return file && file.size() == 0;
  }
  bool append(uint8_t slot, uint8_t index, const uint8_t *bytes, size_t size) override {
    char text[64]; path(slot, index, text);
    auto file = botFilesystem.open(text, "r+");
    if (!file || file.size() > SourceLimit || size > SourceLimit - file.size()) return false;
    if (!file.seek(file.size()) || file.write(bytes, size) != size) return false;
    file.flush(); return botFilesystem.ready();
  }
  bool length(uint8_t slot, uint8_t index, size_t &size) override {
    char text[64]; path(slot, index, text);
    auto file = botFilesystem.open(text);
    if (!file) return false;
    size = file.size(); return true;
  }
  void digest(const uint8_t *bytes, size_t size, uint8_t hash[32]) override {
    mesh::Utils::sha256(hash, 32, bytes, size);
  }
  void report(uint8_t slot, const char *error) override { Serial.printf("Packet slot %u: %s\n", slot, error); }
};
class Program final : public onchip::PacketProgram {
  onchip::PacketLua lua_;
public:
  bool load(onchip::PacketProgramRuntime runtime, const uint8_t *source, size_t size,
            char *error, size_t capacity) override {
    if (runtime != onchip::PacketProgramRuntime::Lua || size > SourceLimit) {
      snprintf(error, capacity, "Pine packet source requires Lua, at most 4096 bytes"); return false;
    }
    return lua_.load(reinterpret_cast<const char *>(source), size, error, capacity, HeapLimit, 10000, 20000);
  }
  void status(char *reply, size_t capacity, bool timing) const override {
    const auto &s = lua_.stats();
    if (timing) snprintf(reply, capacity, "load_us=%llu invoke_us=%llu instructions=%lu native=%lu",
        (unsigned long long)s.loadUs, (unsigned long long)s.invokeUs,
        (unsigned long)s.instructions, (unsigned long)s.nativeCalls);
    else snprintf(reply, capacity, "source=%lu heap_limit=%lu live=%lu peak=%lu",
        (unsigned long)s.sourceBytes, (unsigned long)s.heapLimit, (unsigned long)s.liveBytes,
        (unsigned long)s.peakBytes);
  }
  packet_engine::Decision process(const packet_engine::Metadata &m, packet_engine::Call &c) override {
    return lua_.process(m, c);
  }
};
class Loader final : public onchip::PacketProgramLoader {
  Store &store_;
  TaskHandle_t task_ = nullptr;
  std::atomic<unsigned> state_{0};
  Request request_;
  Result result_;
  onchip::PacketProgram *retired_ = nullptr;
  static void entry(void *context) {
    auto &self = *static_cast<Loader *>(context);
    for (;;) {
      if (self.state_.load(std::memory_order_acquire) == 1) {
        self.load();
        self.state_.store(2, std::memory_order_release);
      } else if (self.state_.load(std::memory_order_acquire) == 4) {
        delete self.retired_; self.retired_ = nullptr;
        self.state_.store(0, std::memory_order_release);
      }
      vTaskDelay(1);
    }
  }
  void load() {
    result_ = {};
    const uint32_t started = millis();
    const auto fail = [&](const char *text) { snprintf(result_.error, sizeof(result_.error), "%s", text); };
    size_t size;
    const auto &r = request_;
    if (r.source.size > SourceLimit || !store_.length(r.slot, r.from, size) || size != r.source.size) {
      fail("Pine packet source size invalid"); return;
    }
    if (dbgHeapFree() < int(size + HeapLimit + 8192)) { fail("Pine packet candidate exceeds free RAM above 8KiB reserve"); return; }
    std::unique_ptr<uint8_t[]> source(new (std::nothrow) uint8_t[size]);
    if (!source || !store_.read(r.slot, r.from, 0, source.get(), size)) { fail("Pine packet source read failed"); return; }
    uint8_t hash[32]; store_.digest(source.get(), size, hash);
    if (memcmp(hash, r.source.hash, 32)) { fail("Pine packet source hash differs"); return; }
    result_.program = new (std::nothrow) Program;
    if (!result_.program || !result_.program->load(r.source.runtime, source.get(), size, result_.error, sizeof(result_.error))) return;
    if (r.copy) {
      if (!store_.truncate(r.slot, r.to) || !store_.append(r.slot, r.to, source.get(), size) ||
          !store_.read(r.slot, r.to, 0, source.get(), size)) { fail("Pine packet durable source readback failed"); return; }
      store_.digest(source.get(), size, hash);
      if (memcmp(hash, r.source.hash, 32)) { fail("Pine packet durable source hash differs"); return; }
    }
    if (uint32_t(millis() - started) > 2000) { fail("Pine packet installation exceeded 2 seconds"); return; }
    result_.ok = true;
  }
public:
  explicit Loader(Store &store) : store_(store) {}
  bool submit(const Request &r) override {
    if (busy()) return false;
    if (!task_) {
      if (dbgHeapFree() < 16384 + HeapLimit + SourceLimit + 8192 ||
          xTaskCreate(entry, "pine-packet-load", 16384 / sizeof(StackType_t), this, 1, &task_) != pdPASS) return false;
    }
    request_ = r; state_.store(1, std::memory_order_release); return true;
  }
  bool busy() const override { return state_.load(std::memory_order_acquire) != 0; }
  bool poll(Result &out) override {
    if (state_.load(std::memory_order_acquire) != 2) return false;
    out = result_; result_.program = nullptr; state_.store(3, std::memory_order_release); return true;
  }
  void release(onchip::PacketProgram *program) override {
    retired_ = program;
    state_.store(4, std::memory_order_release);
  }
};
struct Service final : packet_engine::Host {
  SharedRadio &radio;
  RadioPort port;
  Store store;
  packet_engine::Pipeline pipeline{*this};
  Loader loader{store};
  onchip::PacketPrograms programs{pipeline, store, loader};
  struct Queued { packet_engine::Emission packet; uint32_t due = 0, expiry = 0; bool used = false; } queue[2];
  bool sending = false;
  explicit Service(SharedRadio &r) : radio(r), port(r, 4) { radio.program = &port; }
  ~Service() { radio.program = nullptr; }
  uint32_t microsNow() override { return micros(); }
  void fault(const char *name, const packet_engine::Metadata &, packet_engine::Fault error) override {
    Serial.printf("Packet %s: %s\n", name, packet_engine::faultText(error));
  }
  bool admit(const packet_engine::Metadata &, const packet_engine::Emission *packets, uint8_t count) override {
    unsigned free = 0; for (const auto &q : queue) if (!q.used) ++free;
    if (count > free) return false;
    for (unsigned p = 0; p < count; ++p) {
      for (auto &q : queue) if (!q.used) {
        q.packet = packets[p]; q.due = millis() + packets[p].delayMs;
        q.expiry = millis() + (packets[p].expiryMs ? packets[p].expiryMs : 60000);
        q.used = true; break;
      }
    }
    return true;
  }
  void service(bool drainOnly) {
    if (!drainOnly) programs.service();
    else for (auto &q : queue) q.used = false;
    if (sending) {
      if (port.isSendComplete() || port.transmissionFailed()) {
        port.onSendFinished(); sending = false;
      }
      return;
    }
    for (auto &q : queue) if (q.used) {
      if (int32_t(millis() - q.expiry) >= 0) { q.used = false; continue; }
      if (int32_t(millis() - q.due) >= 0 && port.startSendRaw(q.packet.bytes, q.packet.length)) {
        q.used = false; sending = true; break;
      }
    }
  }
};
Service *service;
bool selection(bool enabled) {
  uint8_t byte = enabled;
  InternalFS.remove("/packet-enabled.tmp");
  auto output = InternalFS.open("/packet-enabled.tmp", FILE_O_WRITE);
  if (!output || output.write(&byte, 1) != 1) return false;
  output.flush(); output.close();
  auto input = InternalFS.open("/packet-enabled.tmp");
  uint8_t check = 2;
  if (!input || input.size() != 1 || input.read(&check, 1) != 1 || check != byte) return false;
  input.close();
  return InternalFS.rename("/packet-enabled.tmp", "/packet-enabled");
}
}
bool packetProgramsSelected() {
  if (!InternalFS.exists("/packet-enabled")) return false;
  auto input = InternalFS.open("/packet-enabled");
  uint8_t byte = 0;
  return !input || input.size() != 1 || input.read(&byte, 1) != 1 || byte != 0;
}
bool beginPacketPrograms(SharedRadio &radio) {
  if (!packetProgramsSelected() || !botFilesystem.ready() || dbgHeapFree() < int(sizeof(Service) + 8192)) return false;
  service = new (std::nothrow) Service(radio);
  if (!service) return false;
  if (service->programs.begin()) return true;
  delete service; service = nullptr; return false;
}
void servicePacketPrograms(bool drainOnly) { if (service) service->service(drainOnly); }
bool packetProgramHook(RadioPort &, const packet_engine::Metadata &m, uint8_t *bytes, uint16_t &length,
                       uint16_t capacity, packet_engine::Validator validator) {
  return !service || service->pipeline.process(m, bytes, length, capacity, validator) != packet_engine::Decision::Drop;
}
bool packetProgramCommand(const char *input, char *reply, size_t capacity) {
  if (strncmp(input, "packet ", 7)) return false;
  input += 7;
  if (!strcmp(input, "role on") || !strcmp(input, "role off")) {
    const bool enabled = !strcmp(input, "role on");
    if (enabled && (roomSelected() || peerDeviceSelected()))
      snprintf(reply, capacity, "Error: disable Room/peer role and restart before enabling packet role");
    else snprintf(reply, capacity, "%s", selection(enabled) ? "Packet role saved; restart to apply; Lua source retained" : "Error: packet role save failed");
  } else if (!strcmp(input, "api")) {
    snprintf(reply, capacity, "Packet ABI=1 slots=2 source=4096 chunk=48 runtimes=lua stages=255 caps=0 heap=12288 role=%u live=%u",
             packetProgramsSelected(), service != nullptr);
  } else if (!service) snprintf(reply, capacity, "Error: packet role inactive; packet role on then reboot; Lua pauses");
  else {
    unsigned slot, stages, fuel, us, caps, size;
    char id[17], runtime[8], hash[65];
    if (sscanf(input, "%u budget %u %u %u %u", &slot, &stages, &fuel, &us, &caps) == 5 &&
        (fuel > 10000 || us > 2000 || caps != 0))
      snprintf(reply, capacity, "Error: Pine budget requires fuel<=10000 us<=2000 caps=0");
    else if (sscanf(input, "%u begin %16s %7s %u %64s", &slot, id, runtime, &size, hash) == 5 &&
             (size > SourceLimit || strcmp(runtime, "lua")))
      snprintf(reply, capacity, "Error: Pine packet source requires Lua, at most 4096 bytes");
    else service->programs.command(input, reply, capacity);
  }
  return true;
}
}
#endif
