// SPDX-License-Identifier: Apache-2.0
#include "PacketPrograms.h"
#include "../../firmware/runtime/PacketPrograms.h"
#include "PacketProgramWorker.h"
#include "PacketReports.h"
#include "BotComputeBudget.h"
#include "Clock.h"
#include "MastSource.h"
#include "SPIFFS.h"
#include <Utils.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

namespace onchip {
namespace {
uint32_t clockUs() {
  return uint32_t(std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}
class Store final : public PacketProgramStore {
  static void path(uint8_t slot, uint8_t file, char (&text)[48]) {
    if (file == PacketProgramUpload) snprintf(text, sizeof(text), "/command-bot/packet-%u.stage", slot);
    else snprintf(text, sizeof(text), "/command-bot/packet-%u-%u.bin", slot, file);
  }
public:
  bool journal(uint8_t slot, PacketProgramRecord &record, bool write) override {
    struct Journal { PacketProgramRecord record; uint8_t hash[32]{}; } next;
    char key[16]; snprintf(key, sizeof(key), "packet%u", slot);
    bool present = false;
    if (!write) {
      if (!mastRecord(key, &next, sizeof(next), false, present)) return false;
      if (!present) { record = {}; record.stages = 1; return true; }
      uint8_t hash[32]; digest(reinterpret_cast<const uint8_t *>(&next.record), sizeof(next.record), hash);
      if (memcmp(hash, next.hash, sizeof(hash))) return false;
      record = next.record;
      return record.stages == 1 && !(record.capabilities & ~9u);
    }
    // Explicit remove of a sealed slot starts from the shared empty default.
    if (record.stages == 255 && record.active == PacketProgramEmpty &&
        record.previous == PacketProgramEmpty && !record.enabled && !record.capabilities &&
        !record.sources[0].size && !record.sources[1].size && !record.sources[2].size) record.stages = 1;
    if (record.stages != 1 || (record.capabilities & ~9u)) return false;
    next.record = record;
    digest(reinterpret_cast<const uint8_t *>(&next.record), sizeof(next.record), next.hash);
    if (!mastRecord(key, &next, sizeof(next), true, present)) return false;
    Journal actual;
    return mastRecord(key, &actual, sizeof(actual), false, present) && present &&
           !memcmp(&actual, &next, sizeof(actual));
  }
  bool read(uint8_t slot, uint8_t index, size_t offset, uint8_t *bytes, size_t size) override {
    char text[48]; path(slot, index, text);
    auto file = SPIFFS.open(text, "r");
    return file && offset <= file.size() && size <= file.size() - offset &&
           file.seek(offset) && file.read(bytes, size) == size;
  }
  bool length(uint8_t slot, uint8_t index, size_t &size) override {
    char text[48]; path(slot, index, text);
    auto file = SPIFFS.open(text, "r");
    if (!file) return false;
    size = file.size(); return true;
  }
  bool truncate(uint8_t slot, uint8_t index) override {
    char text[48]; path(slot, index, text);
    auto file = SPIFFS.open(text, "w");
    if (!file) return false;
    file.flush(); file.close();
    auto actual = SPIFFS.open(text, "r");
    return actual && actual.size() == 0;
  }
  bool append(uint8_t slot, uint8_t index, const uint8_t *bytes, size_t size) override {
    char text[48]; path(slot, index, text);
    auto file = SPIFFS.open(text, "r+");
    if (!file || file.size() > PacketProgramSourceLimit || size > PacketProgramSourceLimit - file.size()) return false;
    if (!file.seek(file.size())) return false;
    const bool ok = file.write(bytes, size) == size;
    file.flush(); return ok;
  }
  void digest(const uint8_t *bytes, size_t size, uint8_t output[32]) override {
    mesh::Utils::sha256(output, 32, bytes, size);
  }
  void report(uint8_t slot, const char *error) override {
    std::fprintf(stderr, "Packet slot %u: %s\n", slot, error);
  }
};
class Service final : public packet_engine::Host {
  struct Packet { uint8_t bytes[255]; uint16_t size; int16_t rssi, snr; } queue_[8]{};
  unsigned head_ = 0, count_ = 0;
  RadioConfig profile_;
  uint8_t identity_[32];
  std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
  BotComputeBudget compute_;
public:
  Store store;
  packet_engine::Reports reports;
  packet_engine::Pipeline pipeline;
  PacketProgramWorker loader;
  PacketPrograms programs;
  uint32_t dropped = 0, faults = 0;
  Service(const RadioConfig &profile, const uint8_t *identity)
      : profile_(profile), pipeline(*this), loader(store, makePacketVmProgram), programs(pipeline, store, loader) {
    memcpy(identity_, identity, sizeof(identity_));
  }
  bool begin() { return reports.begin() && programs.begin(); }
  uint32_t microsNow() override { return clockUs(); }
  void beginReports() override { reports.start(); }
  bool report(uint8_t slot, const void *bytes, size_t size) override { return reports.stage(slot, bytes, size); }
  void commitReports() override { reports.commit(); }
  void clearReport(uint8_t slot) override { reports.clear(slot); }
  void fault(const char *engine, const packet_engine::Metadata &, packet_engine::Fault error) override {
    ++faults; std::fprintf(stderr, "Packet %s: %s\n", engine, packet_engine::faultText(error));
  }
  bool admit(const packet_engine::Metadata &, const packet_engine::Emission *, uint8_t) override { return false; }
  bool system(packet_engine::SystemInfo &info) override {
    info = {};
    info.uptimeMs = uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started_).count());
    info.phy = {profile_.freq_hz, profile_.bw_hz, profile_.sf,
                profile_.cr, uint32_t(profile_.tx_power)};
    info.enabledRoles = info.readyRoles = 4; info.flags = packet_engine::RadioReady;
    uint32_t earliest, latest;
    if (trustedNetworkTime(earliest, latest)) {
      info.unixTime = earliest;
      info.flags |= packet_engine::TrustedTime;
    }
    return true;
  }
  void receive(const uint8_t *bytes, size_t size, float rssi, float snr, uint8_t origin) {
    // Unmeasured RF and local reflections must never become zero-valued samples.
    if (origin != 0 || !size || size > 255) return;
    if (count_ == 8) { ++dropped; return; }
    auto &packet = queue_[(head_ + count_) % 8];
    memcpy(packet.bytes, bytes, size); packet.size = uint16_t(size);
    packet.rssi = int16_t(std::lround(rssi)); packet.snr = int16_t(std::lround(snr * 4));
    ++count_;
  }
  void service() {
    programs.service();
    if (!count_ || !compute_.ready(clockUs())) return;
    auto &packet = queue_[head_];
    packet_engine::Metadata metadata;
    metadata.source = 2; metadata.rssi = packet.rssi; metadata.snrQuarterDb = packet.snr;
    metadata.payloadType = (packet.bytes[0] >> 2) & 15;
    memcpy(metadata.identity, identity_, sizeof(identity_));
    const auto started = clockUs();
    // One copied packet per worker turn, at most two existing 20 ms slot budgets.
    // Mutations/drop decisions affect this copy only; bot admission is unchanged.
    pipeline.process(metadata, packet.bytes, packet.size, sizeof(packet.bytes));
    compute_.charge(started, clockUs());
    head_ = (head_ + 1) % 8; --count_;
  }
  uint32_t waitMs() { return count_ ? (compute_.waitUs(clockUs()) + 999) / 1000 : UINT32_MAX; }
};
std::unique_ptr<Service> service;
}
bool beginHostPacketPrograms(const RadioConfig &profile, const uint8_t identity[32]) {
  if (service) return false;
  service.reset(new (std::nothrow) Service(profile, identity));
  if (!service || !service->begin()) { service.reset(); return false; }
  return true;
}
void stopHostPacketPrograms() { service.reset(); }
void hostPacketReceive(const uint8_t *bytes, size_t size, float rssi, float snr, uint8_t origin) {
  if (service) service->receive(bytes, size, rssi, snr, origin);
}
void serviceHostPacketPrograms() { if (service) service->service(); }
uint32_t hostPacketWaitMs() { return service ? service->waitMs() : UINT32_MAX; }
bool hostPacketReport(unsigned slot, mp_report &report) {
  return service && slot < 2 && service->reports.read(uint8_t(slot), report);
}
size_t hostPacketSnapshot(uint8_t *bytes, size_t capacity) {
  constexpr size_t Size = 2 + 2 * sizeof(mp_report);
  if (!service || capacity < Size) return 0;
  memset(bytes, 0, Size); bytes[0] = 1;
  for (unsigned slot = 0; slot < 2; ++slot) {
    mp_report report;
    if (hostPacketReport(slot, report)) {
      bytes[1] |= uint8_t(1u << slot);
      memcpy(bytes + 2 + slot * sizeof(report), &report, sizeof(report));
    }
  }
  return Size;
}
void hostPacketCommand(const char *input, char *reply, size_t capacity) {
  if (!service) { snprintf(reply, capacity, "Error: packet controller unavailable"); return; }
  if (!strcmp(input, "api")) {
    snprintf(reply, capacity, "Packet ABI=1 report=1 slots=2 source=16384 chunk=48 runtimes=%s stages=1 caps=9",
#if ONCHIP_BOT_WASM
             "lua,wasm"
#else
             "lua"
#endif
    );
    return;
  }
  if (!strcmp(input, "phy") || !strcmp(input, "phy cancel")) {
    snprintf(reply, capacity, "Error: Willow packet guests cannot change the shared modem PHY"); return;
  }
  unsigned slot, stages, fuel, us, caps;
  if (sscanf(input, "%u budget %u %u %u %u", &slot, &stages, &fuel, &us, &caps) == 5 &&
      (stages != 1 || (caps & ~9u))) {
    snprintf(reply, capacity, "Error: Willow packet budget requires receive stage 1 and read/report capability bits 1/8"); return;
  }
  service->programs.command(input, reply, capacity);
  if (!strcmp(reply, "packet selection removed")) {
    // Reload the host's Receive-only empty default after sealed-slot recovery.
    char retry[24], outcome[128];
    if (sscanf(input, "%u remove", &slot) == 1) {
      snprintf(retry, sizeof(retry), "%u retry", slot);
      service->programs.command(retry, outcome, sizeof(outcome));
      if (strcmp(outcome, "empty")) snprintf(reply, capacity, "Error: packet selection removed but reload failed: %.100s", outcome);
    }
  }
}
}
