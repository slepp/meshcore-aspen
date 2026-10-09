// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "PacketPrograms.h"

namespace onchip {
class PacketProgramWorker final : public PacketProgramLoader {
  struct Impl;
  Impl *impl_ = nullptr;
  PacketProgramStore &store_;
  using Factory = PacketProgram *(*)();
  Factory factory_;
public:
  PacketProgramWorker(PacketProgramStore &store, Factory factory) : store_(store), factory_(factory) {}
  ~PacketProgramWorker() override;
  bool submit(const Request &) override;
  bool poll(Result &) override;
  bool busy() const override;
  void release(PacketProgram *) override;
};
} // namespace onchip
