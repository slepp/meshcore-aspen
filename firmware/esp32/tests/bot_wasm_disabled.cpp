// SPDX-License-Identifier: Apache-2.0
#include "BotVm.h"
#include <cassert>
#include <cstring>
#include <cstdio>
using namespace onchip;
int main() {
  static_assert(!ONCHIP_BOT_WASM, "Disabled-build test must exclude Wasm");
  const char *sources[] = {"\0asm\1\0\0\0",
    "--@meshcore-bot/1;name=fixture;version=1.0.0;runtime=wamr-2.4.1;api=meshcore-v1;caps=none;schema=none;rollback=none\n\0asm\1\0\0\0"};
  const size_t sizes[] = {8, 127};
  for (unsigned i = 0; i < 2; ++i) {
    BotSession session; BotVmStats stats; char error[128]{};
    const size_t size = i ? strlen(sources[i]) + 8 : sizes[i];
    assert(!session.load(sources[i], size, 1, stats, error, sizeof(error)));
    assert(!strcmp(error, "Wasm runtime unavailable in this build"));
    assert(!BotVm::validate(sources[i], size, stats, error, sizeof(error)));
    assert(!strcmp(error, "Wasm runtime unavailable in this build"));
  }
  BotSession lua; BotVmStats stats; char error[128]{};
  const char *source = "function hello() reply('Lua retained') end";
  assert(lua.load(source, strlen(source), 1, stats, error, sizeof(error)) && !lua.isWasm());
  BotEvent event; assert(parseBotCommand("!hello", 6, event, error, sizeof(error)));
  assert(lua.start(1, event, error, sizeof(error)));
  BotIoRequest request; assert(lua.nextIo(request));
  assert(request.kind == BotIoRequest::Send && request.reply && !strcmp(request.value, "Lua retained"));
  BotIoResult sent{}; sent.token = request.token; sent.ok = sent.queued = sent.transmitted = true;
  assert(lua.complete(sent));
  BotSession::Result result; assert(lua.poll(result) && result.ok);
  puts("PASS disabled Wasm explicitly rejected; existing Lua runs without WAMR");
}
