// SPDX-License-Identifier: Apache-2.0
#include "PacketLua.h"
#include "support/PacketServiceHost.h"
#include <cassert>
#include <chrono>
#include <cstring>
#include <cstdio>
uint64_t onchipBotVmTestClock() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}
int main() {
  using namespace packet_engine;
  onchip::PacketLua lua;
  char error[128]{};
  const char *source = "local n=0; return function() n=n+1; packet.write(1,'Z'); return packet.CONTINUE end";
  if (!lua.load(source, strlen(source), error, sizeof(error), 12288, 10000, 20000)) {
    fprintf(stderr, "Pine packet Lua load: %s\n", error); return 1;
  }
  assert(lua.stats().heapLimit == 12288 && lua.stats().peakBytes <= 12288);
  PacketServiceHost host;
  Pipeline pipeline(host);
  assert(pipeline.attach(lua, {"pine", 255, 10000, 2000, 0}) == Registration::Attached);
  uint8_t bytes[Capacity] = {1, 2};
  uint16_t length = 2;
  Metadata metadata;
  metadata.stage = Stage::Receive;
  assert(pipeline.process(metadata, bytes, length, Capacity) == Decision::Continue);
  assert(bytes[1] == 'Z' && pipeline.enabled(0));
  source = "return function() packet.write(1,'X'); while true do end end";
  assert(lua.load(source, strlen(source), error, sizeof(error), 12288, 10000, 20000));
  bytes[1] = 'Z';
  assert(pipeline.process(metadata, bytes, length, Capacity) == Decision::Continue);
  assert(bytes[1] == 'Z' && !pipeline.enabled(0) && !host.errors.empty());
  source = "local x='x'; while true do x=x..x end";
  assert(!lua.load(source, strlen(source), error, sizeof(error), 12288, 10000, 20000));
  assert(error[0]);
  puts("Pine packet Lua: 12KiB load/invoke, 2ms/fuel fault rollback and bounded initialization");
}
