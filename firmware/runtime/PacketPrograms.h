// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "PacketPipeline.h"
#include <stddef.h>

namespace onchip {
constexpr size_t PacketProgramSourceLimit = 16384;
constexpr uint8_t PacketProgramEmpty = 3, PacketProgramUpload = 3;
enum class PacketProgramRuntime : uint8_t { Lua, Wasm };
struct PacketProgramSource {
  uint32_t size = 0;
  uint8_t hash[32]{};
  PacketProgramRuntime runtime = PacketProgramRuntime::Lua;
  uint8_t reserved[3]{};
};
struct PacketProgramRecord {
  uint32_t version = 1;
  uint32_t stages = 255, fuel = 10000, microseconds = 2000, capabilities = 0;
  uint8_t active = PacketProgramEmpty, previous = PacketProgramEmpty, enabled = 0, reserved = 0;
  PacketProgramSource sources[3];
};
static_assert(sizeof(PacketProgramRecord) == 144, "Packet program journal layout changed");
class PacketProgramStore {
public:
  virtual ~PacketProgramStore() = default;
  virtual bool journal(uint8_t slot, PacketProgramRecord &, bool write) = 0;
  virtual bool read(uint8_t slot, uint8_t file, size_t offset,
                    uint8_t *output, size_t size) = 0;
  virtual bool truncate(uint8_t slot, uint8_t file) = 0;
  virtual bool append(uint8_t slot, uint8_t file, const uint8_t *, size_t) = 0;
  virtual bool length(uint8_t slot, uint8_t file, size_t &) = 0;
  virtual void digest(const uint8_t *, size_t, uint8_t output[32]) = 0;
  virtual void report(uint8_t slot, const char *) = 0;
};
class PacketProgram : public packet_engine::Engine {
public:
  virtual bool load(PacketProgramRuntime, const uint8_t *, size_t, char *, size_t) = 0;
  virtual void status(char *, size_t, bool timing = false) const = 0;
};
PacketProgram *makePacketVmProgram();
class PacketProgramLoader {
public:
  struct Request {
    uint8_t slot = 0, from = 0, to = 0;
    bool copy = false;
    PacketProgramSource source;
  };
  struct Result {
    bool ok = false;
    PacketProgram *program = nullptr;
    char error[128]{};
  };
  virtual ~PacketProgramLoader() = default;
  virtual bool submit(const Request &) = 0;
  virtual bool poll(Result &) = 0;
  virtual bool busy() const = 0;
  // Return candidate/retired ownership to the loader for off-task destruction.
  virtual void release(PacketProgram *) = 0;
};
class PacketPrograms {
  struct Slot final : packet_engine::Engine {
    PacketProgramRecord saved;
    PacketProgram *program = nullptr;
    bool sealed = false, bootPending = false;
    char outcome[128] = "empty";
    struct Upload {
      bool active = false;
      char id[17]{};
      PacketProgramSource source;
      uint32_t received = 0;
    } upload;
    packet_engine::Decision process(const packet_engine::Metadata &m, packet_engine::Call &c) override {
      return program ? program->process(m, c) : packet_engine::Decision::Continue;
    }
  } slots_[packet_engine::EngineLimit];
  packet_engine::Pipeline &pipeline_;
  PacketProgramStore &store_;
  PacketProgramLoader &loader_;
  bool attached_ = false, pending_ = false, bootJob_ = false;
  uint8_t pendingSlot_ = 0;
  PacketProgramRecord candidate_;
  static const char *name(uint8_t);
  static bool valid(const PacketProgramRecord &);
  packet_engine::Budget budget(uint8_t, const PacketProgramRecord &) const;
  void fail(uint8_t, const char *, bool seal = false);
  bool start(uint8_t, const PacketProgramRecord &, uint8_t from, bool copy, bool boot);
  bool save(uint8_t, const PacketProgramRecord &);
public:
  PacketPrograms(packet_engine::Pipeline &p, PacketProgramStore &s, PacketProgramLoader &l)
      : pipeline_(p), store_(s), loader_(l) {}
  PacketPrograms(const PacketPrograms &) = delete;
  PacketPrograms &operator=(const PacketPrograms &) = delete;
  ~PacketPrograms();
  bool begin();
  void service();
  void command(const char *, char *, size_t);
};
} // namespace onchip
