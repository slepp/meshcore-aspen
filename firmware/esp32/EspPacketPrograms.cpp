// SPDX-License-Identifier: Apache-2.0
#if defined(ARDUINO_ARCH_ESP32) && MESHCORE_ONCHIP_BOT
#include "EspPacketPrograms.h"
#include "NativePacketHost.h"
#include "PacketProgramWorker.h"
#include "Runtime.h"
#include "MastSource.h"
#include <SPIFFS.h>
#include <Utils.h>
#include <cstdio>
#include <new>

namespace onchip {
namespace {
class Store final : public PacketProgramStore {
  static void path(uint8_t slot, uint8_t file, char (&text)[24]) {
    if (file == PacketProgramUpload) snprintf(text, sizeof(text), "/packet-%u.stage", slot);
    else snprintf(text, sizeof(text), "/packet-%u-%u.bin", slot, file);
  }
public:
  bool journal(uint8_t slot, PacketProgramRecord &record, bool write) override {
    struct Journal {
      PacketProgramRecord record;
      uint8_t hash[32]{};
    } next;
    char key[16]; snprintf(key, sizeof(key), "packet%u", slot);
    bool present = false;
    if (!write) {
      if (!mastRecord(key, &next, sizeof(next), false, present)) return false;
      if (!present) { record = {}; return true; }
      uint8_t hash[32]; digest(reinterpret_cast<const uint8_t *>(&next.record), sizeof(next.record), hash);
      if (memcmp(hash, next.hash, sizeof(hash))) return false;
      record = next.record; return true;
    }
    next.record = record;
    digest(reinterpret_cast<const uint8_t *>(&next.record), sizeof(next.record), next.hash);
    if (!mastRecord(key, &next, sizeof(next), true, present)) return false;
    Journal actual;
    return mastRecord(key, &actual, sizeof(actual), false, present) && present &&
           !memcmp(&actual, &next, sizeof(actual));
  }
  bool read(uint8_t slot, uint8_t index, size_t offset, uint8_t *bytes, size_t size) override {
    char text[24]; path(slot, index, text);
    auto file = SPIFFS.open(text, "r");
    if (!file || offset > file.size() || size > file.size() - offset ||
        !file.seek(offset)) return false;
    return file.read(bytes, size) == size;
  }
  bool length(uint8_t slot, uint8_t index, size_t &size) override {
    char text[24]; path(slot, index, text);
    auto file = SPIFFS.open(text, "r");
    if (!file) return false;
    size = file.size(); return true;
  }
  bool truncate(uint8_t slot, uint8_t index) override {
    char text[24]; path(slot, index, text);
    auto file = SPIFFS.open(text, "w");
    if (!file) return false;
    file.flush(); return file.size() == 0;
  }
  bool append(uint8_t slot, uint8_t index, const uint8_t *bytes, size_t size) override {
    char text[24]; path(slot, index, text);
    auto file = SPIFFS.open(text, "a");
    if (!file || file.size() > PacketProgramSourceLimit || size > PacketProgramSourceLimit - file.size()) return false;
    const bool ok = file.write(bytes, size) == size;
    file.flush(); return ok;
  }
  void digest(const uint8_t *bytes, size_t size, uint8_t output[32]) override {
    mesh::Utils::sha256(output, 32, bytes, size);
  }
  void report(uint8_t slot, const char *error) override {
    Serial.printf("Packet slot %u: %s\n", slot, error);
    char message[96];
    snprintf(message, sizeof(message), "Packet slot %u controller error; inspect packet %u status", slot, slot);
    diagnosticEvent(message);
  }
};
uint32_t clock() { return micros(); }
void fault(const char *engine, const packet_engine::Metadata &m, packet_engine::Fault f) {
  char message[160];
  snprintf(message, sizeof(message), "%s stage=%u: %s", engine, unsigned(m.stage), packet_engine::faultText(f));
  Serial.println(message); diagnosticEvent(message);
}
struct Service {
  Store store;
  NativePacketHost host;
  PacketProgramWorker loader;
  PacketPrograms programs;
  explicit Service(WifiKissMultiplexer &mux)
      : host(mux, clock, fault, packetSystemSnapshot, packetComposeOwned),
        loader(store, makePacketVmProgram), programs(host.pipeline(), store, loader) {}
  ~Service() { host.stop(); }
};
Service *service;
}
bool beginPacketPrograms(WifiKissMultiplexer &mux) {
  if (service) return false;
  service = new (std::nothrow) Service(mux);
  if (!service) { Serial.println("Packet controller allocation failed"); return false; }
  if (!service->programs.begin() || !service->host.begin()) {
    delete service; service = nullptr; return false;
  }
  return true;
}
void servicePacketPrograms() {
  if (service) { service->programs.service(); service->host.service(); }
}
void packetProgramCommand(const char *input, char *reply, size_t capacity) {
  if (!service) { snprintf(reply, capacity, "Error: packet controller unavailable"); return; }
  if (!strcmp(input, "api")) {
    snprintf(reply, capacity, "Packet ABI=1 slots=2 source=16384 chunk=48 runtimes=%s stages=255 caps=7",
#if ONCHIP_BOT_WASM
             "lua,wasm");
#else
             "lua");
#endif
  } else if (!strcmp(input, "phy")) {
    const auto &s = service->host.status();
    snprintf(reply, capacity, "phy=%s accepted=%u applied=%u faults=%u last=%s",
             NativePacketHost::phyStateText(s.phy), s.phyAccepted, s.phyApplied, s.faults,
             packet_engine::faultText(s.lastFault));
  } else if (!strcmp(input, "phy cancel")) {
    snprintf(reply, capacity, "%s", service->host.cancelPhy() ?
             "Pending packet PHY request cancelled" : "Error: no packet PHY request is pending");
  } else service->programs.command(input, reply, capacity);
}
void stopPacketPrograms() { delete service; service = nullptr; }
}
#endif
