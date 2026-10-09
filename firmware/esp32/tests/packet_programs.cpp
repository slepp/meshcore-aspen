// SPDX-License-Identifier: Apache-2.0
#include "PacketPrograms.h"
#include "PacketProgramWorker.h"
#include <openssl/sha.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
using namespace onchip;
using namespace packet_engine;
static std::atomic<bool> gate{false}, waiting{false};
static std::atomic<unsigned> alive{0};
struct Program final : PacketProgram {
  uint8_t value = 0;
  Program() { ++alive; }
  ~Program() override { --alive; }
  bool load(PacketProgramRuntime, const uint8_t *source, size_t, char *error, size_t capacity) override {
    value = source[0];
    if (value == 'G') {
      waiting = true;
      while (gate.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
      waiting = false;
    }
    if (value == 'X') { snprintf(error, capacity, "invalid test packet program"); return false; }
    return true;
  }
  Decision process(const Metadata &, Call &call) override {
    call.write(0, &value, 1);
    if (value == 'F') { call.fail(Fault::Execution); return Decision::Failed; }
    return Decision::Continue;
  }
  void status(char *output, size_t capacity, bool) const override { snprintf(output, capacity, "memory-test"); }
};
static PacketProgram *create() { return new Program; }
struct Store final : PacketProgramStore {
  PacketProgramRecord records[2];
  std::vector<uint8_t> files[2][4];
  std::mutex lock;
  bool saveFails = false, saveUncertain = false, copyFails = false, readFails = false;
  unsigned reports = 0;
  bool journal(uint8_t slot, PacketProgramRecord &r, bool write) override {
    if (write) {
      if (saveUncertain) { records[slot] = r; return false; }
      if (saveFails) return false;
      records[slot] = r;
    } else r = records[slot];
    return true;
  }
  bool read(uint8_t slot, uint8_t file, size_t offset, uint8_t *output, size_t size) override {
    std::lock_guard<std::mutex> guard(lock);
    const auto &bytes = files[slot][file];
    if (readFails || offset > bytes.size() || size > bytes.size() - offset) return false;
    memcpy(output, bytes.data() + offset, size); return true;
  }
  bool length(uint8_t slot, uint8_t file, size_t &size) override {
    std::lock_guard<std::mutex> guard(lock); size = files[slot][file].size(); return true;
  }
  bool truncate(uint8_t slot, uint8_t file) override {
    std::lock_guard<std::mutex> guard(lock);
    if (copyFails && file != PacketProgramUpload) return false;
    files[slot][file].clear(); return true;
  }
  bool append(uint8_t slot, uint8_t file, const uint8_t *bytes, size_t size) override {
    std::lock_guard<std::mutex> guard(lock);
    if (copyFails && file != PacketProgramUpload) return false;
    auto &output = files[slot][file]; output.insert(output.end(), bytes, bytes + size); return true;
  }
  void digest(const uint8_t *bytes, size_t size, uint8_t out[32]) override { SHA256(bytes, size, out); }
  void report(uint8_t, const char *) override { ++reports; }
};
struct TestHost final : Host {
  uint32_t microsNow() override { return 0; }
  void fault(const char *, const Metadata &, Fault) override {}
  bool admit(const Metadata &, const Emission *, uint8_t) override { return true; }
};
static std::string hex(const uint8_t *bytes, size_t size) {
  std::string output;
  for (size_t i = 0; i < size; ++i) { char text[3]; snprintf(text, sizeof(text), "%02x", bytes[i]); output += text; }
  return output;
}
struct Harness {
  TestHost host;
  Pipeline pipeline{host};
  Store &store;
  PacketProgramWorker loader{store, create};
  PacketPrograms programs{pipeline, store, loader};
  explicit Harness(Store &s, PacketProgram *(*factory)() = create) : store(s), loader(s, factory) { assert(programs.begin()); }
  std::string command(const std::string &input) {
    char response[163]{}; programs.command(input.c_str(), response, sizeof(response)); return response;
  }
  void settle() {
    unsigned idle = 0;
    for (unsigned i = 0; i < 1000; ++i) {
      programs.service();
      if (!loader.busy()) { if (++idle == 3) return; } else idle = 0;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(false && "packet loader did not settle");
  }
  std::string stage(uint8_t slot, const std::string &source, const char *runtime = "lua") {
    uint8_t digest[32]; store.digest(reinterpret_cast<const uint8_t *>(source.data()), source.size(), digest);
    const auto hash = hex(digest, sizeof(digest)), id = hash.substr(0, 16);
    const std::string prefix = "0" + std::to_string(slot); // Decimal parser accepts leading zero.
    assert(command(prefix + " begin " + id + " " + runtime + " " + std::to_string(source.size()) + " " + hash)
           == "Packet upload ready received=0 size=" + std::to_string(source.size()));
    for (size_t offset = 0; offset < source.size(); offset += 48) {
      const size_t size = source.size() - offset < 48 ? source.size() - offset : 48;
      const auto text = prefix + " chunk " + id + " " + std::to_string(offset / 48) + " " +
          hex(reinterpret_cast<const uint8_t *>(source.data()) + offset, size);
      const auto expected = "Packet chunk saved received=" + std::to_string(offset + size);
      assert(command(text) == expected);
      assert(command(text) == expected);
    }
    return id;
  }
  void install(uint8_t slot, const std::string &source, const char *runtime = "lua") {
    const auto id = stage(slot, source, runtime);
    assert(command(std::to_string(slot) + " commit " + id) == "Packet validation accepted; inspect status then hash");
    settle();
  }
  uint8_t run(uint8_t first = 1) {
    uint8_t bytes[Capacity] = {first, 2}; uint16_t size = 2;
    Metadata m; m.source = 3; m.destination = 4; m.payloadType = 5;
    m.generation = 6; m.job = 7; m.rssi = -101; m.snrQuarterDb = -9; m.identity[31] = 0xa5;
    assert(pipeline.process(m, bytes, size, Capacity) == Decision::Continue);
    return bytes[0];
  }
};
static void install_restart_and_rollback() {
  Store store;
  std::string hashA, hashB;
  {
    Harness h(store);
    assert(h.command("0 hash") == "empty");
    assert(h.command("0 enable on").rfind("Error:", 0) == 0);
    assert(h.command("0 budget 256 100 100 0").rfind("Error:", 0) == 0);
    assert(h.command("0 budget 255 10000 2000 5") == "Packet execution budget saved and applied");
    h.install(0, std::string(100, 'A'));
    assert(!h.pipeline.enabled(0) && h.run() == 1);
    hashA = h.command("0 hash");
    assert(h.command("0 enable on") == "saved program enabled" && h.run() == 'A');
    assert(h.command("0 read 2").rfind("2 41414141", 0) == 0);
    h.install(0, "B");
    hashB = h.command("0 hash");
    assert(hashA != hashB && h.run() == 'B');
    assert(h.command("0 rollback") == "Packet rollback accepted; inspect status then hash"); h.settle();
    assert(h.command("0 hash") == hashA && h.run() == 'A');
    assert(h.command("0 stats").find("caps=5") != std::string::npos);
    h.install(1, "C"); assert(h.command("1 enable on") == "saved program enabled");
    assert(h.run() == 'C');
    assert(h.command("1 enable off") == "saved program disabled");
  }
  assert(alive == 0);
  {
    Harness h(store); h.settle();
    assert(h.run() == 'A' && h.pipeline.enabled(0) && !h.pipeline.enabled(1));
    assert(h.command("0 hash") == hashA);
    assert(h.command("0 remove") == "packet selection removed" && h.run() == 1);
    assert(h.command("0 rollback") == "Packet rollback accepted; inspect status then hash"); h.settle();
    assert(!h.pipeline.enabled(0) && h.command("0 hash") == hashA);
    assert(h.command("0 memory") == "memory-test");
  }
  assert(alive == 0);
}
static void isolated_load_and_failures() {
  Store store; Harness h(store);
  h.install(0, "A"); h.command("0 enable on");
  const auto original = h.command("0 hash");
  gate = true;
  const auto id = h.stage(0, "G");
  h.command("0 commit " + id);
  for (unsigned i = 0; i < 1000 && !waiting; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  assert(waiting && h.run() == 'A');
  assert(h.command("0 enable off").rfind("Error: packet loader busy", 0) == 0);
  assert(h.command("0 hash") == original);
  gate = false; h.settle();
  assert(h.run() == 'G');
  const auto retained = h.command("0 hash");
  h.install(0, "X"); assert(h.run() == 'G' && h.command("0 hash") == retained);
  assert(h.command("0 status").find("invalid test packet program") != std::string::npos);
  assert(h.command("0 cancel").rfind("Error:", 0) == 0);
  const auto otherId = h.stage(1, "Z");
  assert(h.command("0 cancel " + otherId).rfind("Error:", 0) == 0);
  assert(h.command("1 cancel " + otherId) == "Packet upload cancelled");
}
static void fault_and_journal_recovery() {
  Store store;
  {
    Harness h(store); h.install(0, "F"); h.command("0 enable on");
    assert(h.run() == 1 && !h.pipeline.enabled(0));
    h.programs.service();
    assert(!store.records[0].enabled && h.command("0 status").find("fault disabled") != std::string::npos);
    h.command("0 enable on"); assert(h.pipeline.enabled(0));
    h.command("0 enable off");
    store.saveFails = true;
    assert(h.command("0 budget 1 100 100 0").rfind("Error:", 0) == 0);
    assert(h.command("0 status").find("sealed=1") != std::string::npos);
    store.saveFails = false;
    assert(h.command("0 retry") == "saved program waiting for loader"); h.settle();
    assert(!h.pipeline.enabled(0) && h.command("0 stats").find("stages=255") != std::string::npos);
    store.saveUncertain = true;
    assert(h.command("0 budget 1 100 100 0").rfind("Error:", 0) == 0);
    store.saveUncertain = false;
    h.command("0 retry"); h.settle();
    assert(h.command("0 stats").find("stages=1 fuel=100 us=100 caps=0") != std::string::npos);
  }
  assert(alive == 0);
  store.records[0].version = 99;
  {
    Harness h(store);
    assert(h.command("0 status").find("sealed=1") != std::string::npos);
    assert(h.command("0 enable on").rfind("Error:", 0) == 0);
    assert(h.command("0 remove") == "packet selection removed");
    assert(store.records[0].version == 1);
  }
}
static void bad_upload_and_durable_copy() {
  Store store; Harness h(store);
  h.install(0, "A");
  const auto retained = h.command("0 hash");
  auto id = h.stage(0, "B");
  {
    std::lock_guard<std::mutex> lock(store.lock);
    store.files[0][PacketProgramUpload][0] = 'D';
  }
  h.command("0 commit " + id); h.settle();
  assert(h.command("0 hash") == retained && h.command("0 status").find("SHA256 differs") != std::string::npos);
  assert(h.command("0 cancel " + id) == "Packet upload cancelled");
  store.copyFails = true; h.install(0, "B");
  assert(h.command("0 hash") == retained && h.command("0 status").find("durable source write failed") != std::string::npos);
  store.copyFails = false; h.command("0 cancel " + h.stage(1, "C"));
  auto wrong = h.command("0 chunk 0000000000000000 0 42");
  assert(wrong.rfind("Error:", 0) == 0);
  assert(h.command("0 budget 1 42949672960 100 0").rfind("Error:", 0) == 0);
}
static void boot_source_corruption() {
  Store store;
  {
    Harness h(store); h.install(0, "A"); h.command("0 enable on");
  }
  const auto active = store.records[0].active;
  store.files[0][active][0] = 'B';
  {
    Harness h(store); h.settle();
    assert(h.run() == 1 && h.command("0 status").find("sealed=1") != std::string::npos);
    assert(h.command("0 status").find("SHA256 differs") != std::string::npos);
    assert(store.records[0].enabled);
    assert(h.command("0 enable on").rfind("Error:", 0) == 0);
    store.files[0][active][0] = 'A';
    assert(h.command("0 retry") == "saved program waiting for loader"); h.settle();
    assert(h.run() == 'A' && h.pipeline.enabled(0));
  }
}
#if MESHCORE_ONCHIP_BOT
uint64_t onchipBotVmTestClock() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}
static void real_vms(const char *path) {
  std::ifstream input(path, std::ios::binary); assert(input);
  const std::string wasm{std::istreambuf_iterator<char>(input), {}};
  Store store;
  std::string hash;
  {
    Harness h(store, makePacketVmProgram);
    h.install(0, "return function() packet.write(0, 'L'); return packet.CONTINUE end");
    assert(h.command("0 memory").rfind("lua source=", 0) == 0);
    assert(h.command("0 enable on") == "saved program enabled" && h.run() == 'L');
    h.command("0 enable off");
    h.install(1, wasm, "wasm");
    assert(h.command("1 memory").find("linear=65536") != std::string::npos);
    assert(h.command("1 enable on") == "saved program enabled" && h.run(0) == 0x70);
    hash = h.command("1 hash");
    h.install(1, "return 42");
    assert(h.command("1 hash") == hash && h.run(0) == 0x70);
    assert(h.command("1 status").find("Error:") != std::string::npos);
    h.install(0, "return function() packet.write(0, 'F'); error('fault') end");
    h.command("0 enable on");
    // A slot fault rolls back the whole invocation, including later programs.
    assert(h.run(0) == 0 && !h.pipeline.enabled(0));
    h.programs.service();
    assert(!store.records[0].enabled && store.records[1].enabled);
  }
  {
    Harness h(store, makePacketVmProgram); h.settle();
    assert(!h.pipeline.enabled(0) && h.pipeline.enabled(1) && h.run(0) == 0x70);
    assert(h.command("1 hash") == hash);
  }
  puts("Packet Lua/Wasm worker installation, failed replacement, fault persistence and boot reload passed");
}
#endif
int main(int argc, char **argv) {
  install_restart_and_rollback();
  isolated_load_and_failures();
  fault_and_journal_recovery();
  bad_upload_and_durable_copy();
  boot_source_corruption();
  assert(alive == 0);
#if MESHCORE_ONCHIP_BOT
  assert(argc == 2); real_vms(argv[1]);
#else
  (void)argc; (void)argv;
#endif
  puts("Packet program installation, restart, rollback, faults and isolated loading passed");
}
