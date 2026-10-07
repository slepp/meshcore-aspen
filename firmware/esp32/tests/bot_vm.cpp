// SPDX-License-Identifier: Apache-2.0
#include "BotVm.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <fstream>
#include <iterator>
#include <chrono>
extern "C" {
#include <lua.h>
}
using namespace onchip;

static uint64_t fakeTime = 0, fakeStep = 0;
uint64_t onchipBotVmTestClock() {
  if (fakeStep) return fakeTime += fakeStep;
  return std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}
static BotEvent event(const char *command) {
  BotEvent value{};
  char error[128]{};
  assert(parseBotCommand(command, strlen(command), value, error, sizeof(error)));
  value.path.known = true;
  value.path.count = 2;
  value.path.bytes[0] = 0xa1; value.path.bytes[1] = 0xb2;
  value.signal = true; value.rssi = -90; value.snr = 5;
  value.authenticated = true; value.sender[0] = 1;
  return value;
}
static std::string program(const std::string &body) {
  return "function custom() " + body + " end command('custom','','Test command')";
}
static BotAction run(const std::string &source, const BotEvent &input, bool success = true,
                     BotVmLimits limits = {}, const BotManifest *installed = nullptr) {
  BotAction action;
  BotVmStats stats;
  char error[128]{};
  const bool ok = BotVm::invoke(source.data(), source.size(), input, action, stats,
                               error, sizeof(error), limits, installed);
  if (ok != success) fprintf(stderr, "unexpected VM result: %s\n", error);
  assert(ok == success);
  if (!ok) {
    assert(action.kind == BotAction::None);
    assert(error[0]);
  }
  assert(stats.peakBytes <= limits.heapBytes);
  return action;
}
static BotManifest validate(const std::string &source, bool success = true) {
  BotManifest manifest;
  BotVmStats stats;
  char error[128]{};
  const bool ok = BotVm::validate(source.data(), source.size(), stats, error, sizeof(error), {}, &manifest);
  if (ok != success) fprintf(stderr, "unexpected validation: %s\n", error);
  assert(ok == success);
  if (!ok) assert(error[0] && !manifest.count);
  return manifest;
}
static void commandOverrides() {
  BotSession vm;
  BotVmStats stats;
  char error[128]{};
  uint32_t id = 0;
  const auto load = [&](const std::string &source, bool ok = true) {
    const bool loaded = vm.load(source.data(), source.size(), 17, stats, error, sizeof(error));
    if (loaded != ok) fprintf(stderr, "override load: %s\n", error);
    assert(loaded == ok && stats.peakBytes <= BotVmLimits{}.heapBytes);
  };
  const auto invoke = [&](const BotEvent &input, bool ok = true) {
    BotSession::Result result;
    assert(vm.start(++id, input, error, sizeof(error)) && vm.poll(result));
    if (result.ok != ok) fprintf(stderr, "override invocation: %s\n", result.error);
    assert(result.ok == ok && result.stats.peakBytes <= BotVmLimits{}.heapBytes);
    return result;
  };
  const std::string wrapper =
      "function patched() return 'Regional '..call_original('ping') end "
      "override_command('ping','patched')";
  const auto metadata = validate(wrapper);
  assert(metadata.count == 1 && metadata.find("ping") &&
         !strcmp(metadata.find("ping")->function, "patched") &&
         !strcmp(metadata.find("ping")->help, "Reply Pong"));
  assert(!metadata.isBundled());
  assert(!strcmp(run(wrapper, event("!ping")).text, "Regional Pong"));
  assert(!strcmp(run(program("return call_original('calc','(2+3)*4')"), event("!custom")).text, "= 20"));
  load(wrapper);
  assert(!strcmp(invoke(event("!ping")).action.text, "Regional Pong"));
  invoke(event("!ping extra"), false);
  assert(!strcmp(invoke(event("!path")).action.text, "1:a1b2"));
  assert(strstr(invoke(event("!help ping")).action.text, "Reply Pong"));
  assert(strstr(invoke(event("!plugins")).action.text, "custom"));
  load("function patched(seconds) return tostring(seconds or 5)..' '..call_original('ping','') end "
       "override_command('mt','patched')");
  assert(!strcmp(invoke(event("!mt")).action.text, "5 Pong"));
  assert(!strcmp(invoke(event("!mt 30")).action.text, "30 Pong"));
  auto invalidSeconds = event("!mt 30"); strcpy(invalidSeconds.arguments, "31");
  invoke(invalidSeconds, false);
  for (const char *bad : {
       "function patched() end override_command('missing','patched')",
       "function patched() end override_command('ping','ping')",
       "function patched() end override_command('ping','patched',{})",
       "function patched() end override_command('ping\\0x','patched')",
       "function patched() end override_command('ping','patched') override_command('ping','patched')",
       "override_command('ping','absent')",
       "function patched() end command('ping','','','patched')",
       "function patched() end override_command('ping','patched') command('ping','','','patched')",
       "call_original('ping') function patched() end"}) {
    load(bad, false);
  }
  for (const char *body : {
       "return call_original('missing')", "return call_original('ping',{})",
       "return call_original('ping',ctx)", "return call_original('ping','','extra')",
       "return call_original('ping','extra')", "return call_original('calc','')",
       "return call_original('ping','\\0')",
       "override_command('ping','custom') return 'bad'"}) {
    load(program(body)); invoke(event("!custom"), false);
  }
  std::string full = "function patched() return 'ok' end ";
  for (const char *name : {"ping", "path", "test", "mt", "trace", "calc", "convert", "roll"})
    full += "override_command('" + std::string(name) + "','patched') ";
  load(full);
  assert(vm.manifest().count == BotCommandLimit);
  load(full + "override_command('choose','patched')", false);
  load("function observed() call_original('ping') end events.on('message','observed')");
  BotEvent message; message.kind = BotEvent::Message;
  invoke(message, false);
  load("function patched(text) return 'owner replacement' end override_command('admin','patched') "
       "function bridge() return call_original('admin','bot status') end");
  auto owner = event("!admin bot status");
  invoke(owner, false);
  owner.owner = true;
  assert(!strcmp(invoke(owner).action.text, "owner replacement"));
  for (unsigned condition = 0; condition < 3; ++condition) {
    auto denied = owner;
    if (condition == 0) denied.owner = false;
    if (condition == 1) denied.local = true;
    if (condition == 2) { denied.authenticated = false; denied.channelVerified = true; strcpy(denied.channel, "#lab"); }
    invoke(denied, false);
    strcpy(denied.name, "bridge"); denied.arguments[0] = 0;
    invoke(denied, false);
  }
  BotIoRequest request;
  BotIoResult completion;
  BotSession::Result result;
  auto bridge = owner; strcpy(bridge.name, "bridge"); bridge.arguments[0] = 0;
  assert(vm.start(++id, bridge, error, sizeof(error)) && vm.nextIo(request) &&
         request.kind == BotIoRequest::Admin && !strcmp(request.value, "bot status"));
  vm.cancel(); assert(vm.poll(result) && !result.ok);
  load("function patched(key) sleep(1) return call_original('recall')..' patched' end "
       "override_command('recall','patched') "
       "function alias(prefix) return call_original('list-memories') end "
       "command('alias','prefix?:string:32','Original alias') "
       "function other() return call_original('recall','\"two words\"') end");
  auto alice = event("!recall camp"), bob = alice;
  alice.sender[31] = 1; bob.sender[31] = 2;
  assert(vm.start(++id, alice, error, sizeof(error)) && vm.nextIo(request) && request.kind == BotIoRequest::Sleep);
  completion.token = request.token; completion.ok = true;
  assert(vm.complete(completion) && vm.nextIo(request) && request.kind == BotIoRequest::Get &&
         !strcmp(request.key, "camp") && !memcmp(request.principal, alice.sender, 32));
  const auto aliceToken = request.token;
  assert(vm.start(++id, bob, error, sizeof(error)) && vm.nextIo(request) && request.kind == BotIoRequest::Sleep);
  completion = {}; completion.token = request.token; completion.ok = true;
  assert(vm.complete(completion) && vm.nextIo(request) && request.kind == BotIoRequest::Get &&
         !memcmp(request.principal, bob.sender, 32) && !(request.token == aliceToken));
  completion = {}; completion.token = request.token; completion.ok = completion.found = true;
  strcpy(completion.value, "Bob");
  assert(vm.complete(completion) && vm.poll(result) && result.ok && !strcmp(result.action.text, "Bob patched"));
  completion.token = aliceToken; strcpy(completion.value, "Alice");
  assert(vm.complete(completion) && vm.poll(result) && result.ok && !strcmp(result.action.text, "Alice patched"));
  assert(vm.start(++id, event("!other"), error, sizeof(error)) && vm.nextIo(request) &&
         request.kind == BotIoRequest::Get && !strcmp(request.key, "two words"));
  vm.cancel(); assert(vm.poll(result) && !result.ok);
  assert(vm.start(++id, event("!alias ca"), error, sizeof(error)) && vm.nextIo(request) &&
         request.kind == BotIoRequest::List && !strcmp(request.key, "ca"));
  vm.cancel(); assert(vm.poll(result) && !result.ok);
  auto reflection = alice; reflection.local = true;
  invoke(reflection, false);
  load("function patched(key,text) call_original('remember'); return missing.x end "
       "override_command('remember','patched')");
  assert(vm.start(++id, event("!remember camp tea"), error, sizeof(error)) && vm.nextIo(request) &&
         request.kind == BotIoRequest::Put && !strcmp(request.key, "camp") && !strcmp(request.value, "tea"));
  completion = {}; completion.token = request.token; completion.ok = true; completion.outcome = BotIoResult::Committed;
  assert(vm.complete(completion) && vm.poll(result) && !result.ok && result.action.kind == BotAction::None);
  load("function patched(key) return call_original('recall') end override_command('recall','patched')");
  assert(vm.start(++id, alice, error, sizeof(error)) && vm.nextIo(request));
  completion = {}; completion.token = request.token;
  vm.cancel(); assert(vm.poll(result) && !result.ok && strstr(result.error, "may be unknown"));
  assert(!vm.complete(completion));
  load("function patched() while true do end end override_command('ping','patched')");
  invoke(event("!ping"), false);
  assert(!strcmp(invoke(event("!path")).action.text, "1:a1b2"));
  load(wrapper);
  BotSession candidate;
  const char *replacement = "function patched() return 'Replacement' end override_command('ping','patched')";
  assert(candidate.load(replacement, strlen(replacement), 18, stats, error, sizeof(error)));
  vm.swap(candidate);
  assert(!strcmp(invoke(event("!ping")).action.text, "Replacement"));
  vm.swap(candidate);
  assert(!strcmp(invoke(event("!ping")).action.text, "Regional Pong"));
  load(BotDefaultSource);
  assert(vm.manifest().isBundled());
  assert(!strcmp(invoke(event("!ping")).action.text, "Pong"));
  load("function patched(place) return 'weather replacement' end override_command('weather','patched') "
       "function bridge() return call_original('weather','Ottawa') end "
       "function shared() return call_original('board','list') end "
       "function personal() return call_original('remind','5m tea') end");
  invoke(event("!weather Ottawa"), false);
  invoke(event("!bridge"), false);
  invoke(event("!shared"), false);
  invoke(event("!personal"), false);
  auto channel = event("!shared");
  channel.authenticated = false; channel.channelVerified = channel.targeted = true;
  channel.sharedState = true; channel.sharedGrant = 19;
  strcpy(channel.channel, "#lab"); channel.channelId[31] = 4;
  assert(vm.start(++id, channel, error, sizeof(error)) && vm.nextIo(request) &&
         request.kind == BotIoRequest::List && request.scope == BotIoRequest::Channel &&
         request.grant == 19 && !memcmp(request.principal, channel.channelId, 32));
  vm.cancel(); assert(vm.poll(result) && !result.ok);
  auto personal = event("!personal"); personal.reminderAccess = true; personal.reminderGrant = 23;
  assert(vm.start(++id, personal, error, sizeof(error)) && vm.nextIo(request) &&
         request.kind == BotIoRequest::ReminderSet && request.grant == 23 &&
         !memcmp(request.principal, personal.sender, 32));
  vm.cancel(); assert(vm.poll(result) && !result.ok);
  auto weather = event("!weather Ottawa"); weather.homeAccess = true; weather.homeGrant = 7;
#if ONCHIP_BOT_COMPACT_PROFILE
  invoke(weather, false);
  strcpy(weather.name, "bridge"); weather.arguments[0] = 0;
  invoke(weather, false);
#else
  assert(!strcmp(invoke(weather).action.text, "weather replacement"));
  strcpy(weather.name, "bridge"); weather.arguments[0] = 0;
  assert(vm.start(++id, weather, error, sizeof(error)) && vm.nextIo(request) &&
         request.kind == BotIoRequest::Rpc && request.grant == 7);
  vm.cancel(); assert(vm.poll(result) && !result.ok);
#endif
  puts("PASS builtin overrides: explicit metadata, true originals/aliases/arguments, implicit caller policy, cooperative jobs, admitted effects, cancellation, replacement/rollback and platform bounds");
}
static void registry() {
  const std::string declaration = "function hello(name) reply('Hello '..name) end";
  const auto inferred = validate(declaration);
  assert(inferred.count == 1 && !strcmp(inferred.commands[0].name, "hello") &&
         !strcmp(inferred.commands[0].schema, "name:string:159"));
  assert(!strcmp(run(declaration, event("!hello slepp")).text, "Hello slepp"));
  assert(!strcmp(run(declaration, event("!hello 'two names'")).text, "Hello two names"));
  run(declaration, event("!hello"), false);
  run(declaration, event("!hello one two"), false);
  assert(!strcmp(run(declaration, event("!ping")).text, "Pong"));
  assert(strstr(run(declaration, event("!help hello"), true, {}, &inferred).text, "name:string:159"));
  const std::string helperSource =
      "local function greeting(name) return 'Hello '..name end "
      "function hello(name) reply(greeting(name)) end "
      "function bye() reply('Bye') end";
  const auto helpers = validate(helperSource);
  assert(helpers.count == 2 && !strcmp(helpers.commands[0].name, "bye") &&
         !strcmp(helpers.commands[1].name, "hello") && !helpers.find("greeting"));
  assert(!strcmp(run(helperSource, event("!hello slepp")).text, "Hello slepp"));
  assert(!strcmp(run(helperSource, event("!bye")).text, "Bye"));
  const std::string privateHelper =
      "function _greeting(name,a,b,c,d,...) return 'Hello '..name end "
      "function hello(name) reply(_greeting(name)) end";
  const auto privateManifest = validate(privateHelper);
  assert(privateManifest.count == 1 && privateManifest.find("hello") &&
         !privateManifest.find("_greeting"));
  assert(!strcmp(run(privateHelper, event("!hello slepp")).text, "Hello slepp"));
  run(privateHelper, event("!_greeting slepp"), false);
  const std::string aliases =
      "function list_memories() reply('fixture memory') end "
      "command('read-memories','','Read fixture','list_memories') "
      "command('memories','','Read fixture alias','list_memories')";
  const auto aliased = validate(aliases);
  assert(aliased.count == 2 && aliased.find("read-memories") &&
         !aliased.find("list_memories"));
  assert(!strcmp(aliased.find("read-memories")->function, "list_memories"));
  assert(!strcmp(run(aliases, event("!read-memories")).text, "fixture memory"));
  assert(!strcmp(run(aliases, event("!memories")).text, "fixture memory"));
  assert(strstr(run(aliases, event("!help read-memories"), true, {}, &aliased).text, "Read fixture"));
  run(aliases, event("!read-memories extra"), false);
  validate(aliases + " command('read-memories','','Duplicate','list_memories')", false);
  validate("function list_memories() end command('read-memories','','Missing export')", false);
  validate("function list_memories() end command('-bad','','','list_memories')", false);
  validate("function list_memories() end command('bad.name','','','list_memories')", false);
  validate("function list_memories() end command('ping','','','list_memories')", false);
  const std::string cooperation =
      "memories={} "
      "function memorize(note) memories[1]=note end "
      "function list_memories() return memories[1] end "
      "function testall() memorize('get groceries') reply(list_memories()) end";
  assert(validate(cooperation).count == 3);
  assert(!strcmp(run(cooperation, event("!testall")).text, "get groceries"));
  const auto four = validate("function four(a,b,c,d) reply(a..b..c..d) end");
  assert(!strcmp(four.commands[0].schema, "a:string:159,b:string:159,c:string:159,d:string:159"));
  validate("function five(a,b,c,d,e) end", false);
  validate("function variadic(...) end", false);
  validate("function upper(Name) end", false);
  validate("function ping() reply('replacement') end", false);
  validate("function reply(text) end function hello(name) end", false);
  validate("function hello(name) end _ENV[1]=function() end", false);
  validate("function hello(name) end _ENV['bad\\0name']=function() end", false);
  validate("function hello(name) end for i=1,65 do _ENV[i]=i end", false);
  validate("function hello() end function other() end command('hello','','','other')", false);
  validate("function long(aaaaaaaaaaaaaaaaaaaaaaaa,bbbbbbbbbbbbbbbbbbbbbbbb,"
           "cccccccccccccccccccccccc,dddddddddddddddddddddddd) end", false);
  run("function custom() reply('declared') end command('custom','','Explicit')",
      event("!custom"));
  const std::string hello =
      "function hello(name) reply('Hello '..name) end "
      "command('hello','name:string:32','Greet a person') "
      "function twice(n,enabled) if enabled then reply(tostring(n*2)) else reply('off') end end "
      "command('double','n:int:-8:8,enabled:bool','Bounded arithmetic','twice')";
  const auto manifest = validate(hello);
  assert(manifest.count == 2 && manifest.find("hello") && manifest.find("double"));
  assert(!strcmp(manifest.find("double")->function, "twice"));
  assert(!strcmp(run(hello, event("!hello slepp")).text, "Hello slepp"));
  assert(!strcmp(run(hello, event("!hello \"two names\"")).text, "Hello two names"));
  assert(!strcmp(run(hello, event("!hello two\\ names")).text, "Hello two names"));
  assert(!strcmp(run(hello, event("!double -8 true")).text, "-16"));
  assert(!strcmp(run(hello, event("!double 8 false")).text, "off"));
  for (const char *command : {"!hello", "!hello a b", "!hello ''", "!hello 'bad", "!hello 'a'b",
                              "!hello abc\\", "!double 9 true", "!double 1 1", "!double 1.0 true",
                              "!double 999999999999999999999 true", "!unknown"}) {
    run(hello, event(command), false);
  }
  run(hello, event(("!hello " + std::string(33, 'x')).c_str()), false);
  // Native diagnostics do not execute an installed script, even if it is broken.
  assert(!strcmp(run("while true do end", event("!ping")).text, "Pong"));
  assert(!strcmp(run(hello, event("!ping")).text, "Pong"));
  assert(strstr(run(hello, event("!help"), true, {}, &manifest).text, "!hello"));
  assert(strstr(run(hello, event("!help hello"), true, {}, &manifest).text, "name:string:32"));
  assert(strstr(run(hello, event("!help unknown"), true, {}, &manifest).text, "unknown"));
  const std::string optional =
      "function words(s,n) if n then reply(s..tostring(n)) else reply(s) end end "
      "command('words','s:string:8,n?:int:1:3','Optional argument') "
      "function rest(s) reply(s) end command('rest','s:text:159','Remaining text')";
  assert(!strcmp(run(optional, event("!words hi")).text, "hi"));
  assert(!strcmp(run(optional, event("!words hi 3")).text, "hi3"));
  assert(!strcmp(run(optional, event("!rest a complete sentence")).text, "a complete sentence"));
  for (const char *badSchema : {"name", "n:int:3:1", "n:int:1", "n:float", "n:string:0",
                                "n:string:160", "n?:bool,m:bool", "n:text:8,m:bool", "n:bool,",
                                "a:bool,b:bool,c:bool,d:bool,e:bool"}) {
    validate("function custom() end command('custom','" + std::string(badSchema) + "','Bad schema')", false);
  }
  for (const char *bad : {"return function(e) return {kind='reply',text='legacy'} end",
                          "local function custom() end",
                          "command('custom','','Missing export')",
                          "function ping() reply('replacement') end command('ping','','Forbidden')",
                          "function custom() end command('custom','','a') command('custom','','b')",
                          "function Custom() end command('Custom','','Uppercase')",
                          "function custom() end command('custom\\0other','','NUL')",
                          "command('custom','','Native helper','reply')",
                          "reply('Initialization side effect')"}) validate(bad, false);
  std::string full;
  for (unsigned i = 0; i < BotCommandLimit; ++i) {
    auto name = "c" + std::to_string(i);
    full += "function " + name + "() reply('ok') end command('" + name + "','','')";
  }
  assert(validate(full).count == BotCommandLimit);
  validate(full + "function overflow() end command('overflow','','')", false);
}
static void manifestLifetimes() {
  const char *source =
      "module('words',function() return {suffix='-ok'} end) "
      "function delayed(name) sleep(1) return name..require('words').suffix end "
      "command('later','name:string:8','Wait with metadata','delayed')";
  BotManifest detached;
  {
    auto original = validate(source);
    strcpy(original.events[0], "on_start"); original.eventMask = 1;
    BotManifestView view = original;
    detached = view;
    assert(detached.commands == detached.storage && detached.commands != original.commands);
    original.clear();
  }
  assert(detached.find("later") && detached.moduleCount == 1 && detached.eventMask == 1 &&
         !strcmp(detached.modules[0], "words") && !strcmp(detached.events[0], "on_start"));
  BotManifest copied(detached);
  assert(copied.commands == copied.storage && copied.commands != detached.commands);
  BotManifestView alias = copied;
  copied = alias;
  copied = static_cast<const BotManifestView &>(copied);
  detached.clear();
  assert(copied.find("later") && copied.moduleCount == 1 && copied.eventMask == 1);

  BotSession vm;
  BotVmStats stats;
  char error[128]{};
  assert(vm.load(source, strlen(source), 1, stats, error, sizeof(error)));
  const auto &borrowed = vm.manifest();
  const auto *entries = borrowed.commands;
  const auto *command = borrowed.find("later");
  assert(command && !strcmp(command->function, "delayed") &&
         !strcmp(command->schema, "name:string:8") && borrowed.moduleCount == 1);
  BotManifest installed = borrowed;
  assert(vm.start(1, event("!later Alice"), error, sizeof(error)));
  BotIoRequest custom, native;
  assert(vm.nextIo(custom) && custom.kind == BotIoRequest::Sleep);
  assert(vm.start(2, event("!recall memo"), error, sizeof(error)));
  assert(vm.nextIo(native) && native.kind == BotIoRequest::Get);
  if (BotJobLimit == 2) assert(!vm.start(3, event("!ping"), error, sizeof(error)));
  assert(borrowed.commands == entries && borrowed.find("later") == command);
  vm.cancel(1);
  BotSession::Result result;
  assert(vm.poll(result) && !result.ok && result.job == 2);
  BotIoResult done;
  done.token = native.token; done.ok = true;
  assert(!vm.complete(done));
  done.token = custom.token;
  assert(vm.complete(done) && vm.poll(result) && result.ok && result.job == 1 &&
         !strcmp(result.action.text, "Alice-ok"));
  assert(borrowed.commands == entries && !strcmp(command->help, "Wait with metadata"));
  assert(vm.start(4, event("!help later"), error, sizeof(error)));
  assert(vm.poll(result) && result.ok && strstr(result.action.text, "name:string:8"));

  assert(vm.start(5, event("!later Before"), error, sizeof(error)) && vm.nextIo(custom));
  const char *fresh = "function fresh() return 'new' end command('fresh','','New command')";
  const BotCommand *replacementEntries = nullptr;
  {
    BotSession candidate;
    assert(candidate.load(fresh, strlen(fresh), 2, stats, error, sizeof(error)));
    replacementEntries = candidate.manifest().commands;
    vm.swap(candidate);
    assert(vm.manifest().commands == replacementEntries && candidate.manifest().commands == entries);
    assert(candidate.manifest().find("later") == command);
    candidate.cancel();
    assert(candidate.poll(result) && !result.ok && result.job == 5);
    done.token = custom.token;
    assert(!candidate.complete(done) && !vm.complete(done));
  }
  assert(vm.manifest().commands == replacementEntries && vm.manifest().find("fresh") &&
         installed.find("later") && !strcmp(installed.find("later")->help, "Wait with metadata"));
  assert(vm.start(6, event("!help fresh"), error, sizeof(error)));
  assert(vm.poll(result) && result.ok && strstr(result.action.text, "New command"));
  assert(vm.start(7, event("!recall memo"), error, sizeof(error)) && vm.nextIo(native));
  assert(vm.load(source, strlen(source), 3, stats, error, sizeof(error)));
  done.token = native.token;
  assert(!vm.complete(done) && vm.manifest().find("later") && !vm.manifest().find("fresh"));
  assert(vm.start(8, event("!later Back"), error, sizeof(error)) && vm.nextIo(custom));
  done.token = custom.token;
  assert(vm.complete(done) && vm.poll(result) && result.ok && !strcmp(result.action.text, "Back-ok"));
  for (const char *bad : {"function broken(",
                         "module('bad',function() while true do end end) function bad() end",
                         "local t={} for i=1,10000 do t[i]={i,i,i,i} end function bad() end"}) {
    assert(!vm.load(bad, strlen(bad), 4, stats, error, sizeof(error)) && error[0]);
    assert(vm.manifest().commands == nullptr && vm.manifest().count == 0);
    assert(!vm.complete(done) && !vm.start(9, event("!ping"), error, sizeof(error)));
    assert(installed.find("later") && copied.find("later"));
  }
  assert(vm.load(BotDefaultSource, strlen(BotDefaultSource), 5, stats, error, sizeof(error)));
  BotManifest nativeCopy = vm.manifest();
  assert(nativeCopy.find("ping") && nativeCopy.commands == nativeCopy.storage);
  vm.clear();
  assert(nativeCopy.find("ping") && !strcmp(nativeCopy.find("ping")->function, "ping"));
  puts("PASS manifest lifetimes: owned copies/aliases, custom/native suspended jobs, cancellation, swap/destruction, rollback and failed init");
}
static void initializationBudgets() {
  static_assert(BotVmLimits{}.loadWallUs == 330000 &&
                BotVmLimits{}.initWallUs == 50000 && BotVmLimits{}.wallUs == 20000);
  static_assert(BotVmLimits{}.instructions == 10000 && BotVmLimits{}.parserSteps == 16384);
  static_assert(BotVmLimits{}.heapBytes == (ONCHIP_BOT_COMPACT_PROFILE ? 48 : 96) * 1024);
  BotSession vm;
  BotVmStats stats;
  BotVmLimits limits;
  char error[128]{};
  const std::string source =
      "local n=0 for i=1,500 do n=n+i end "
      "function busy() local n=0 for i=1,4000 do n=n+i end return tostring(n) end "
      "function delayed() sleep(1000) return 'awake' end";
  fakeTime = 1000; fakeStep = 20;
  const bool loaded = vm.load(source.data(), source.size(), 1, stats, error, sizeof(error));
  if (!loaded) fprintf(stderr, "initialization headroom: %s\n", error);
  assert(loaded && stats.initUs > 20000 && stats.initUs < 50000);
  assert(stats.loadUs < limits.loadWallUs && stats.peakBytes <= limits.heapBytes);
  BotSession::Result result;
  assert(vm.start(1, event("!busy"), error, sizeof(error)));
  assert(vm.poll(result) && !result.ok && strstr(result.error, "invocation"));
  assert(result.stats.invokeUs >= limits.wallUs && result.stats.instructions < limits.instructions);
  assert(result.action.kind == BotAction::None);
  assert(vm.start(2, event("!delayed"), error, sizeof(error)));
  BotIoRequest request;
  assert(vm.nextIo(request) && request.kind == BotIoRequest::Sleep);
  fakeTime += 1000000;
  BotIoResult completion; completion.token = request.token; completion.ok = true;
  assert(vm.complete(completion) && vm.poll(result) && result.ok);
  assert(!strcmp(result.action.text, "awake") && result.stats.invokeUs < limits.wallUs);
  assert(result.stats.loadUs == 0 && result.stats.initUs == 0);
  const std::string excessive = "local n=0 for i=1,1500 do n=n+i end function custom() return n end";
  assert(!vm.load(excessive.data(), excessive.size(), 2, stats, error, sizeof(error)));
  assert(strstr(error, "Init deadline ") && strstr(error, "/50000us peak="));
  assert(stats.initUs >= limits.initWallUs && stats.instructions < limits.instructions);
  fakeStep = 0;
  puts("PASS separate budgets: 20..50ms Init accepted, >50ms Init and >20ms invocation rejected, suspended I/O excluded");
}
static void retained() {
  BotSession vm;
  BotVmStats stats;
  char error[128]{};
  const char *source =
      "count=0 "
      "function nested() timer.sleep(1000) return ping() end "
      "function hello(name) local sender=ctx.sender.public_key count=count+1 "
      "local text=nested() "
      "if ctx.sender.public_key~=sender then return 'WRONG PRINCIPAL' "
      "else return name..text..tostring(count) end end "
      "function memorize(text) kv.put('memory',text) return 'committed' end "
      "command('memorize','text:text:100','Remember') "
      "function list_memories() return kv.get('memory') or 'empty' end "
      "command('read-memories','','Recall','list_memories') "
      "function loop() while true do end end";
  assert(vm.load(source, strlen(source), 1, stats, error, sizeof(error)));
  auto a = event("!hello A"), b = event("!hello B");
  b.sender[0] = 2;
  assert(vm.start(1, a, error, sizeof(error)));
  BotIoRequest first, second;
  assert(vm.nextIo(first) && first.kind == BotIoRequest::Sleep);
  assert(vm.start(2, b, error, sizeof(error)) && vm.nextIo(second));
  BotSession::Result result;
  assert(!vm.poll(result));
  BotIoResult completion{};
  completion.token = second.token; completion.ok = true;
  assert(vm.complete(completion) && vm.poll(result) && result.ok && result.job == 2);
  assert(!strcmp(result.action.text, "BPong2"));
  completion.token = first.token;
  assert(vm.complete(completion) && vm.poll(result) && result.ok && result.job == 1);
  assert(!strcmp(result.action.text, "APong2"));
  assert(!vm.complete(completion));
  assert(vm.start(3, event("!memorize get groceries"), error, sizeof(error)) && vm.nextIo(first));
  assert(first.kind == BotIoRequest::Put && !strcmp(first.key, "memory") &&
         !strcmp(first.value, "get groceries") && first.principal[0] == 1);
  completion = {}; completion.token = first.token; completion.ok = true;
  assert(vm.complete(completion) && vm.poll(result) && result.ok);
  assert(vm.start(4, event("!read-memories"), error, sizeof(error)) && vm.nextIo(second));
  assert(second.scope == first.scope && !memcmp(second.principal, first.principal, 32) &&
         !strcmp(second.key, first.key));
  completion = {}; completion.token = second.token; completion.ok = true; completion.found = true;
  strcpy(completion.value, "get groceries");
  assert(vm.complete(completion) && vm.poll(result) && result.ok &&
         !strcmp(result.action.text, "get groceries"));
  assert(vm.start(5, event("!loop"), error, sizeof(error)) && vm.poll(result) && !result.ok);
  assert(vm.start(6, event("!ping"), error, sizeof(error)) && vm.poll(result) && result.ok);
  assert(vm.start(7, a, error, sizeof(error)) && vm.nextIo(first));
  vm.cancel();
  completion.token = first.token;
  assert(!vm.complete(completion) && vm.poll(result) && !result.ok);
  BotSession candidate;
  for (const char *bad : {"reply('stage') function custom() end",
                          "sleep(1) function custom() end",
                          "kv.put('k','v') function custom() end",
                          "timer.set('name',1) function custom() end",
                          "timer.wait('name') function custom() end",
                          "ping=function() end",
                          "ctx={} function custom() end",
                          "function custom() end command('custom','','','missing')"}) {
    assert(!candidate.load(bad, strlen(bad), 2, stats, error, sizeof(error)));
  }
  const char *replacement = "function fresh() return 'fresh' end";
  assert(candidate.load(replacement, strlen(replacement), 2, stats, error, sizeof(error)));
  vm.swap(candidate);
  assert(!vm.complete(completion));
  assert(vm.start(8, event("!fresh"), error, sizeof(error)) && vm.poll(result) && result.ok);
  assert(vm.start(9, event("!help"), error, sizeof(error)) && vm.poll(result) && result.ok &&
         strstr(result.action.text, "!fresh"));
  const char *waiting = "function wait() local sender=ctx.sender.public_key sleep(1000) return sender end";
  fakeTime = 1000; fakeStep = 1;
  assert(vm.load(waiting, strlen(waiting), 3, stats, error, sizeof(error)));
  for (unsigned i = 0; i < BotJobLimit; ++i)
    assert(vm.start(20 + i, event("!wait"), error, sizeof(error)));
  assert(!vm.start(30, event("!wait"), error, sizeof(error)));
  assert(vm.nextIo(first));
  fakeTime += 10000000;
  completion = {}; completion.token = first.token; completion.ok = true;
  assert(vm.complete(completion) && vm.poll(result) && result.ok &&
         result.stats.invokeUs < 20000);
  vm.cancel();
  for (unsigned i = 1; i < BotJobLimit; ++i) assert(vm.poll(result) && !result.ok);
  fakeStep = 0;
  for (const char *body : {"kv.get=nil", "timer.sleep=nil", "mesh.send=nil", "reply=function() end",
                           "ctx={}", "ping=function() end", "while true do sleep(1) end"}) {
    const auto bad = program(body);
    assert(vm.load(bad.data(), bad.size(), 4, stats, error, sizeof(error)));
    assert(vm.start(31, event("!custom"), error, sizeof(error)));
    for (unsigned i = 0; i < BotIoLimit; ++i)
      if (vm.nextIo(first)) {
        completion = {}; completion.token = first.token; completion.ok = true;
        assert(vm.complete(completion));
      }
    assert(vm.poll(result) && !result.ok);
  }
  const char *drafts =
      "function create() saved=mesh.compose{kind='dm',text='request'} sleep(1) return 'created' end "
      "function other() mesh.send(saved) return 'bad' end";
  assert(vm.load(drafts, strlen(drafts), 5, stats, error, sizeof(error)));
  assert(vm.start(40, event("!create"), error, sizeof(error)) && vm.nextIo(first));
  assert(vm.start(41, event("!other"), error, sizeof(error)) && vm.poll(result) && !result.ok);
  assert(strstr(result.error, "different invocation"));
  completion = {}; completion.token = first.token; completion.ok = true;
  assert(vm.complete(completion) && vm.poll(result) && result.ok);
  for (const char *body : {"mesh.compose{to='other',text='x'}", "mesh.compose{kind='raw',text='x'}",
                           "mesh.compose{kind='dm\\0raw',text='x'}", "mesh.send({text='x'})",
                           "mesh.wait{kind='text',timeout_ms=30001}", "mesh.wait{kind='raw'}",
                           "mesh.wait{kind='text\\0ack'}"}) {
    const auto bad = program(body);
    assert(vm.load(bad.data(), bad.size(), 6, stats, error, sizeof(error)));
    assert(vm.start(42, event("!custom"), error, sizeof(error)) && vm.poll(result) && !result.ok);
  }
  const char *literal = "function hello(name) reply('Hello '..name) end";
  assert(vm.load(literal, strlen(literal), 7, stats, error, sizeof(error)));
  assert(vm.start(50, event("!hello slepp"), error, sizeof(error)) && vm.nextIo(first));
  assert(first.kind == BotIoRequest::Send && first.reply && !strcmp(first.value, "Hello slepp"));
  assert(!vm.poll(result));
  completion = {}; completion.token = first.token; completion.ok = true;
  completion.queued = completion.transmitted = true;
  assert(vm.complete(completion) && vm.poll(result) && result.ok && result.action.kind == BotAction::None);
  assert(!vm.complete(completion));
  assert(vm.manifest().count == 1 && vm.manifest().find("hello"));
  assert(vm.start(51, event("!ping"), error, sizeof(error)) && vm.poll(result) &&
         result.ok && !strcmp(result.action.text, "Pong"));
  const char *cooperative =
      "function _greeting(name) sleep(1) return 'Hello '..name end "
      "function hello(name) return _greeting(name) end "
      "function greet(name) return hello(name) end";
  assert(vm.load(cooperative, strlen(cooperative), 8, stats, error, sizeof(error)));
  assert(vm.manifest().count == 2 && !vm.manifest().find("_greeting"));
  assert(vm.start(52, event("!greet slepp"), error, sizeof(error)) && vm.nextIo(first));
  completion = {}; completion.token = first.token; completion.ok = true;
  assert(first.kind == BotIoRequest::Sleep && vm.complete(completion) &&
         vm.poll(result) && result.ok && !strcmp(result.action.text, "Hello slepp"));
  puts("retained VM: shared globals, nested timer yields, caller context, KV requests and source fences passed");
}
static void scopedStorage() {
  BotSession vm;
  BotVmStats stats;
  char error[128]{};
  const char *source =
      "function read(scope) return kv.get('memory',scope) or 'empty' end "
      "function arm() return timer.set('wake',5,'channel').state end "
      "function invalid() timer.set('wake',0) return 'bad' end "
      "function readonly_timer() local r=timer.get('wake') r.state='bad' return 'bad' end";
  assert(vm.load(source, strlen(source), 1, stats, error, sizeof(error)));
  auto channel = event("!read channel");
  channel.authenticated = false; channel.sharedState = channel.channelVerified = channel.targeted = true;
  channel.sharedGrant = 9; channel.channelId[0] = 7; channel.channelId[31] = 8;
  memset(channel.sender, 0, 32);
  strcpy(channel.channel, "#fixture"); strcpy(channel.nickname, "owner");
  BotIoRequest request;
  BotSession::Result result;
  const auto denied = [&](BotEvent input) {
    assert(vm.start(1, input, error, sizeof(error)) && vm.poll(result) && !result.ok);
    assert(!vm.nextIo(request));
  };
  auto bad = channel; bad.channelVerified = false; denied(bad);
  bad = channel; bad.sharedState = false; denied(bad);
  bad = channel; strcpy(bad.arguments, "caller"); denied(bad);
  bad = channel; strcpy(bad.arguments, "conversation"); denied(bad);
  bad = channel; strcpy(bad.arguments, "bot"); denied(bad);
  denied(event("!read channel"));
  denied(event("!read bot"));
  assert(vm.start(1, channel, error, sizeof(error)) && vm.nextIo(request));
  assert(request.scope == BotIoRequest::Channel &&
         !memcmp(request.principal, channel.channelId, sizeof(request.principal)) && request.grant == 9);
  BotIoResult done; done.token = request.token; done.ok = done.found = true;
  strcpy(done.value, "shared");
  assert(vm.complete(done) && vm.poll(result) && result.ok && !strcmp(result.action.text, "shared"));
  strcpy(channel.name, "arm"); channel.arguments[0] = 0;
  assert(vm.start(2, channel, error, sizeof(error)) && vm.nextIo(request));
  assert(request.kind == BotIoRequest::TimerSet && request.delaySeconds == 5 &&
         request.scope == BotIoRequest::Channel && request.principal[0] == 7);
  done = {}; done.token = request.token; done.ok = true; done.timerState = BotTimerState::Pending;
  assert(vm.complete(done) && vm.poll(result) && result.ok && !strcmp(result.action.text, "pending"));
  denied(event("!invalid"));
  assert(vm.start(3, event("!readonly_timer"), error, sizeof(error)) && vm.nextIo(request));
  done.token = request.token;
  assert(vm.complete(done) && vm.poll(result) && !result.ok && strstr(result.error, "read-only"));
  puts("PASS Lua channel authority/scope denial, key-derived principals, typed durable timer yield and immutable result");
}
static void namedThreadScopes() {
  BotSession vm; BotVmStats stats; char error[128]{};
  const char *source =
      "function read(scope,name) return kv.get('key',{scope=scope,thread=name}) or 'empty' end "
      "function write(scope,name) kv.put('key','v',{scope=scope,thread=name}) return 'ok' end "
      "function compare() return kv.cas('key',false,'v',{scope='caller',thread='notes'}) end "
      "function batch() kv.transaction({{key='a',value='v'},{key='b',value='v'}},"
      "{scope='caller',thread='notes'}) return 'ok' end "
      "function list() return tostring(kv.list('',{scope='caller',thread='notes'}).count) end "
      "function arm() return timer.set('wake',5,{scope='caller',thread='notes'}).state end "
      "function invalid() return kv.get('key',{scope='caller',thread='notes',principal='fake'}) end";
  assert(vm.load(source, strlen(source), 1, stats, error, sizeof(error)));
  BotIoRequest request; BotSession::Result result;
  const auto start = [&](const BotEvent &input) {
    assert(vm.start(1, input, error, sizeof(error)));
  };
  const auto finish = [&] {
    BotIoResult done; done.ok = true; done.token = request.token;
    done.outcome = BotIoResult::Committed; done.timerState = BotTimerState::Pending;
    assert(vm.complete(done) && vm.poll(result));
    if (!result.ok) fprintf(stderr, "thread completion failed: %s\n", result.error);
    assert(result.ok);
  };
  auto dm = event("!read conversation notes"); dm.sender[31] = 9;
  start(dm); assert(vm.nextIo(request));
  assert(request.scope == BotIoRequest::ConversationThread && !strcmp(request.key, "notes/key") &&
         !memcmp(request.principal, dm.sender, 32));
  finish();
  start(event("!batch")); assert(vm.nextIo(request));
  assert(request.scope == BotIoRequest::CallerThread && request.mutations == 2 &&
         !strcmp(request.key, "notes/") && !strcmp(request.mutation[0].key, "notes/a") &&
         !strcmp(request.mutation[1].key, "notes/b"));
  finish();
  start(event("!list")); assert(vm.nextIo(request) && !strcmp(request.key, "notes/")); finish();
  start(event("!arm")); assert(vm.nextIo(request) && request.kind == BotIoRequest::TimerSet &&
                            !strcmp(request.key, "notes/wake")); finish();
  auto channel = event("!read channel notes");
  channel.authenticated = false; channel.sharedState = channel.channelVerified = channel.targeted = true;
  channel.sharedGrant = 9; channel.channelId[0] = 7; channel.channelId[31] = 8;
  memset(channel.sender, 0, 32); strcpy(channel.channel, "#fixture");
  start(channel); assert(vm.nextIo(request));
  assert(request.scope == BotIoRequest::ChannelThread && request.grant == 9 &&
         !memcmp(request.principal, channel.channelId, 32)); finish();
  const auto deny = [&](const BotEvent &input) {
    start(input); assert(vm.poll(result) && !result.ok && !vm.nextIo(request));
  };
  auto readOnly = event("!write caller notes");
  strcpy(readOnly.threadRules[0].name, "notes"); readOnly.threadRules[0].access = 16;
  deny(readOnly);
  strcpy(readOnly.name, "read"); strcpy(readOnly.arguments, "caller notes");
  start(readOnly); assert(vm.nextIo(request)); finish();
  auto compare = event("!compare"); compare.policyFlags = 32; deny(compare);
  deny(event("!read caller bad/name"));
  start(event("!read caller abcdefghijklmnopqrstuvwx"));
  assert(vm.nextIo(request) && strlen(request.key) == 28); finish();
  deny(event("!read caller abcdefghijklmnopqrstuvwxy"));
  deny(event("!invalid"));
  auto notTargeted = channel; strcpy(notTargeted.name, "write"); notTargeted.targeted = false;
  deny(notTargeted);
  auto noShared = channel; noShared.sharedState = false; deny(noShared);
  auto other = event("!write caller other");
  strcpy(other.threadRules[0].name, "notes"); other.threadRules[0].access = 0;
  start(other); assert(vm.nextIo(request) && !strcmp(request.key, "other/key")); finish();
  puts("PASS Lua named threads: same native full-origin scopes, bounded encoded keys, CAS/transactions/list/timers, per-thread RW upper bounds, target/shared denials and invalid descriptors");
}
static void packetContinuations() {
  BotSession vm;
  BotVmStats stats;
  char error[128]{};
  BotIoRequest request;
  BotSession::Result result;
  const char *source =
      "function own() saved=mesh.wait{kind='text',exact='hello',from=ctx.sender.public_key,path_width=3,route='flood'} "
      "sleep(10) local t=mesh.forward(saved) return saved.text end "
      "function steal() mesh.forward(saved) end "
      "function peek() return saved.text end "
      "function change() local p=mesh.wait{kind='text'} p.text='fake' end "
      "function once() local p=mesh.compose{text='one'} mesh.send(p) mesh.send(p) end "
      "function split() mesh.send(mesh.compose{kind='trace',route='2:aabb'}) "
      "local p=mesh.wait{kind='trace'} if p.authenticated or not p.correlated or p.snr[1]~=-1 then return 'bad' end "
      "return p.text end "
      "function nested() local a=ping() return a..'; '..mesh.multitrace('1:42',2) end "
      "function group() mesh.send(mesh.compose{kind='channel',text='group'}) return 'ok' end";
  assert(vm.load(source, strlen(source), 10, stats, error, sizeof(error)));
  auto input = event("!own"); input.forwardAccess = true; input.forwardGrant = 7;
  assert(vm.start(1, input, error, sizeof(error)) && vm.nextIo(request));
  assert(request.kind == BotIoRequest::Wait && request.exact && !strcmp(request.key, "hello") &&
         request.pathWidth == 3 && request.routeType == 1);
  BotIoResult done{}; done.token = request.token; done.ok = true;
  done.packet.id = 9; done.packet.authenticated = done.packet.forwardable = true;
  strcpy(done.value, "hello");
  assert(vm.complete(done) && vm.nextIo(request) && request.kind == BotIoRequest::Sleep);
  for (auto name : {"!steal", "!peek"}) {
    auto thief = event(name); thief.sender[0] = 2; thief.forwardAccess = true;
    assert(vm.start(2, thief, error, sizeof(error)) && vm.poll(result) && !result.ok);
    assert(!vm.nextIo(request));
  }
  done = {}; done.token = request.token; done.ok = true;
  assert(vm.complete(done) && vm.nextIo(request) && request.kind == BotIoRequest::Forward &&
         request.packetId == 9 && request.grant == 7);
  done.token = request.token;
  assert(vm.complete(done) && vm.poll(result) && result.ok && !strcmp(result.action.text, "hello"));
  assert(vm.start(3, event("!change"), error, sizeof(error)) && vm.nextIo(request));
  done = {}; done.token = request.token; done.ok = true; done.packet.id = 1;
  assert(vm.complete(done) && vm.poll(result) && !result.ok && strstr(result.error, "read-only"));
  assert(vm.start(4, event("!once"), error, sizeof(error)) && vm.nextIo(request));
  done = {}; done.token = request.token; done.ok = true;
  assert(vm.complete(done) && vm.poll(result) && !result.ok && strstr(result.error, "already submitted"));
  assert(vm.start(5, event("!split"), error, sizeof(error)) && vm.nextIo(request) &&
         request.kind == BotIoRequest::Trace && request.traceSendOnly &&
         request.routeWidth == 2 && request.routeSize == 2);
  done = {}; done.token = request.token; done.ok = done.queued = done.transmitted = true;
  assert(vm.complete(done) && vm.nextIo(request) && request.waitKind == BotIoRequest::TraceWait);
  done.token = request.token; done.found = true; done.trace.width = 2; done.trace.count = 1; done.trace.snr[0] = -4;
  strcpy(done.value, "trace result");
  assert(vm.complete(done) && vm.poll(result) && result.ok && !strcmp(result.action.text, "trace result"));
  assert(vm.start(6, event("!nested"), error, sizeof(error)) && vm.nextIo(request));
  for (unsigned i = 0; i < 2; ++i) {
    assert(request.kind == BotIoRequest::Trace && !request.traceSendOnly && request.route[0] == 0x42);
    done = {}; done.token = request.token; done.ok = true; strcpy(done.value, "one");
    assert(vm.complete(done));
    if (!i) assert(vm.nextIo(request));
  }
  assert(vm.poll(result) && result.ok && !strcmp(result.action.text, "Pong; 1: one | 2: one"));
  auto channel = event("!group"); channel.authenticated = false; channel.channelVerified = channel.targeted = true;
  strcpy(channel.channel, "#fixture"); memset(channel.sender, 0, 32);
  assert(vm.start(7, channel, error, sizeof(error)) && vm.nextIo(request) &&
         request.kind == BotIoRequest::Send && request.packetKind == BotIoRequest::ChannelPacket);
  done = {}; done.token = request.token; done.ok = true;
  assert(vm.complete(done) && vm.poll(result) && result.ok);
  for (const char *body : {"mesh.forward({text='fake'})", "mesh.compose{kind='channel',text='leak'}",
       "mesh.compose{text='x',flags=0}", "mesh.compose{kind='trace',route='3:aabbcc'}",
       "mesh.compose{kind='trace',route='1:42',text='x'}", "mesh.wait{kind='text',from='other'}",
       "mesh.wait{kind='text',path_width=4}", "mesh.wait{kind='text',prefix='x',exact='x'}",
       "mesh.wait{kind='trace',prefix='x'}", "mesh.wait{kind='text',capacity=99}",
       "mesh.multitrace('1:42',4)"}) {
    auto bad = program(body);
    assert(vm.load(bad.data(), bad.size(), 11, stats, error, sizeof(error)));
    assert(vm.start(8, event("!custom"), error, sizeof(error)) && vm.poll(result) && !result.ok);
    assert(!vm.nextIo(request));
  }
  const auto bounded = program("for i=1,3 do mesh.multitrace('1:42',3) end return 'bad'");
  assert(vm.load(bounded.data(), bounded.size(), 12, stats, error, sizeof(error)));
  assert(vm.start(9, event("!custom"), error, sizeof(error)));
  for (unsigned i = 0; i < BotIoLimit; ++i) {
    assert(vm.nextIo(request));
    done = {}; done.token = request.token; done.ok = true; strcpy(done.value, "ok");
    assert(vm.complete(done));
  }
  assert(vm.poll(result) && !result.ok && strstr(result.error, "I/O limit"));
  const auto summary = program("return mesh.multitrace('1:42',3)");
  assert(vm.load(summary.data(), summary.size(), 13, stats, error, sizeof(error)));
  assert(vm.start(10, event("!custom"), error, sizeof(error)));
  for (unsigned i = 0; i < 3; ++i) {
    assert(vm.nextIo(request)); done = {}; done.token = request.token; done.ok = true;
    memset(done.value, 'x', 140); assert(vm.complete(done));
  }
  assert(vm.poll(result) && result.ok && strlen(result.action.text) <= BotReplyLimit &&
         strstr(result.action.text, "; truncated"));
  assert(vm.start(11, event("!custom"), error, sizeof(error)) && vm.nextIo(request));
  done = {}; done.token = request.token; strcpy(done.error, "Native TX deadline; outcome unknown");
  assert(vm.complete(done) && vm.poll(result) && !result.ok && strstr(result.error, "trace 1"));
  assert(vm.start(12, event("!custom"), error, sizeof(error)) && vm.nextIo(request));
  vm.cancel(); done.token = request.token; done.ok = true;
  assert(!vm.complete(done) && vm.poll(result) && !result.ok);
  puts("PASS Lua packet ownership, denied grants/fields, selected channel, split TRACE, bounded nested multitrace and cancellation");
}
static void personalReminderApi() {
  BotSession vm;
  BotVmStats stats;
  char error[128]{};
  const char *source = "function custom() return reminder.after(60,'coffee').state end";
  assert(vm.load(source, strlen(source), 1, stats, error, sizeof(error)));
  BotIoRequest request;
  BotSession::Result result;
  for (const char *command : {"!help remind", "!help reminders", "!help cancel"}) {
    const auto input = event(command);
    assert(!input.reminderAccess);
    const auto help = run(BotDefaultSource, input);
    const bool denied = !strcmp(command, "!help remind");
    assert((strstr(help.text, "unavailable here") != nullptr) == denied);
    assert(vm.start(8, input, error, sizeof(error)) && vm.poll(result) && result.ok &&
           !strcmp(result.action.text, help.text));
    assert(!vm.nextIo(request));
  }
  const auto help = run(BotDefaultSource, event("!help"));
  assert(strstr(help.text, " !cancel ") && !strstr(help.text, " !remind ") &&
         !strstr(help.text, " !remind;"));
  auto input = event("!remind 2h check the oven");
  input.reminderAccess = true; input.reminderGrant = 9;
  assert(vm.start(1, input, error, sizeof(error)) && vm.nextIo(request));
  assert(request.kind == BotIoRequest::ReminderSet && request.delaySeconds == 7200 &&
         request.principal[0] == input.sender[0] && request.grant == 9 &&
         !strcmp(request.value, "check the oven"));
  BotIoResult done; done.token = request.token; done.ok = true;
  done.reminderState = BotReminderState::Pending; done.revision = 7; done.deadlineUtc = 1800000000;
  assert(vm.complete(done) && vm.poll(result) && result.ok &&
         !strcmp(result.action.text, "#7 pending; due UTC 1800000000"));
  assert(vm.start(2, event("!reminders"), error, sizeof(error)) && vm.nextIo(request) &&
         request.kind == BotIoRequest::ReminderList);
  done = {}; done.token = request.token; done.ok = true; strcpy(done.value, "7 pending @1800000000");
  assert(vm.complete(done) && vm.poll(result) && result.ok && !strcmp(result.action.text, done.value));
  assert(vm.start(3, event("!cancel 4294967295"), error, sizeof(error)) && vm.nextIo(request) &&
         request.kind == BotIoRequest::ReminderCancel && request.revision == UINT32_MAX);
  done = {}; done.token = request.token; done.ok = true; done.revision = UINT32_MAX;
  done.reminderState = BotReminderState::Unknown;
  assert(vm.complete(done) && vm.poll(result) && result.ok && strstr(result.action.text, "unknown"));
  for (const char *text : {"!remind 0 tea", "!remind 25h tea", "!remind -2s tea", "!remind 1h2m tea",
                           "!cancel 0", "!cancel 4294967296", "!cancel abc"}) {
    input = event(text); input.reminderAccess = true;
    assert(vm.start(4, input, error, sizeof(error)) && vm.poll(result) && !result.ok);
    assert(!vm.nextIo(request));
  }
  input = event("!remind 1s no grant");
  assert(vm.start(5, input, error, sizeof(error)) && vm.poll(result) && !result.ok && strstr(result.error, "grant"));
  for (const char *text : {"!remind 1s leak", "!reminders", "!cancel 7"}) {
    input = event(text); input.authenticated = false; input.channelVerified = true;
    input.reminderAccess = true; strcpy(input.channel, "#fixture"); memset(input.sender, 0, 32);
    assert(vm.start(6, input, error, sizeof(error)) && vm.poll(result) && !result.ok &&
           strstr(result.error, "private DM"));
  }
  for (const char *bad : {"function cancel() return 'hijack' end",
                          "function remind() return 'hijack' end",
                          "function custom() reminder={} return 'bad' end"}) {
    if (!strncmp(bad, "function custom", 15)) {
      assert(vm.load(bad, strlen(bad), 2, stats, error, sizeof(error)));
      assert(vm.start(7, event("!custom"), error, sizeof(error)) && vm.poll(result) && !result.ok);
    } else assert(!vm.load(bad, strlen(bad), 2, stats, error, sizeof(error)));
  }
  puts("PASS built-in Lua personal reminder commands/API, duration/ID bounds, typed state, private authority and protected command names");
}
static void personalNotesApi() {
  BotSession vm;
  BotVmStats stats;
  char error[128]{};
  const char *source = "function custom() return 'ready' end";
  assert(vm.load(source, strlen(source), 1, stats, error, sizeof(error)));
  BotIoRequest request;
  BotSession::Result result;
  auto invoke = [&](const char *command) {
    assert(vm.start(1, event(command), error, sizeof(error)) && vm.nextIo(request));
  };
  invoke("!remember travel get groceries tomorrow");
  assert(request.kind == BotIoRequest::Put && !strcmp(request.key, "travel") &&
         !strcmp(request.value, "get groceries tomorrow") &&
         request.scope == BotIoRequest::Caller && request.principal[0] == 1);
  BotIoResult done; done.token = request.token; done.ok = true;
  assert(vm.complete(done) && vm.poll(result) && result.ok &&
         !strcmp(result.action.text, "Note travel created; committed"));
  invoke("!remember travel overwritten");
  done.token = request.token; done.found = true;
  assert(vm.complete(done) && vm.poll(result) && result.ok &&
         !strcmp(result.action.text, "Note travel replaced; committed"));
  invoke("!recall travel");
  done.token = request.token; strcpy(done.value, "overwritten");
  assert(request.kind == BotIoRequest::Get && vm.complete(done) && vm.poll(result) &&
         result.ok && !strcmp(result.action.text, "overwritten"));
  for (bool found : {true, false}) {
    invoke("!forget travel"); done = {}; done.token = request.token; done.ok = true; done.found = found;
    assert(request.kind == BotIoRequest::Delete && vm.complete(done) && vm.poll(result) && result.ok);
    assert(!strcmp(result.action.text, found ? "Note travel deleted; committed" : "No note: travel"));
  }
  invoke("!recall missing");
  done = {}; done.token = request.token; done.ok = true;
  assert(vm.complete(done) && vm.poll(result) && result.ok && !strcmp(result.action.text, "No note: missing"));
  invoke("!recall long");
  done = {}; done.token = request.token; done.ok = done.found = true;
  memset(done.value, 'x', BotValueLimit);
  assert(vm.complete(done) && vm.poll(result) && result.ok);
  if (BotValueLimit > BotReplyLimit) assert(strstr(result.action.text, "exceeds RF capacity"));
  else assert(strlen(result.action.text) == BotValueLimit);
  for (const char *value : {"", "line\nbreak", "\xc3\xa9"}) {
    invoke("!recall unsafe"); done = {}; done.token = request.token; done.ok = done.found = true;
    strcpy(done.value, value);
    assert(vm.complete(done) && vm.poll(result) && result.ok &&
           strstr(result.action.text, "empty, non-ASCII or exceeds RF capacity"));
  }
  invoke("!recall exact"); done = {}; done.token = request.token; done.ok = done.found = true;
  memset(done.value, 'x', BotReplyLimit);
  assert(vm.complete(done) && vm.poll(result) && result.ok && strlen(result.action.text) == BotReplyLimit);
  for (const char *command : {"!notes a", "!list-memories a"}) {
    invoke(command);
    assert(request.kind == BotIoRequest::List && !strcmp(request.key, "a") &&
           request.scope == BotIoRequest::Caller && request.principal[0] == 1);
    done = {}; done.token = request.token; done.ok = true; done.keys.count = 2;
    strcpy(done.keys.keys[0], "alpha"); strcpy(done.keys.keys[1], "apple");
    assert(vm.complete(done) && vm.poll(result) && result.ok &&
           !strcmp(result.action.text, "Notes (2): [alpha] [apple]"));
  }
  invoke("!notes"); done = {}; done.token = request.token; done.ok = true;
  assert(vm.complete(done) && vm.poll(result) && result.ok && !strcmp(result.action.text, "No notes"));
  invoke("!notes"); done = {}; done.token = request.token; done.ok = true; done.keys.count = BotKeysPerScope;
  for (unsigned i = 0; i < BotKeysPerScope; ++i) {
    memset(done.keys.keys[i], 'a' + i, BotKeyLimit);
    done.keys.keys[i][BotKeyLimit] = 0;
  }
  assert(vm.complete(done) && vm.poll(result) && result.ok);
  assert(strlen(result.action.text) <= BotReplyLimit &&
         strstr(result.action.text, "truncated; narrow prefix") &&
         strstr(result.action.text, "Notes (8)"));
  for (unsigned extra : {0u, 1u}) {
    invoke("!notes"); done = {}; done.token = request.token; done.ok = true; done.keys.count = 8;
    // 10-byte label + 128 key bytes + 24 delimiter bytes exactly fills one RF reply.
    for (unsigned i = 0; i < 8; ++i)
      memset(done.keys.keys[i], 'a' + i, i < 4 ? 30 : i == 4 ? 2 + extra : 2);
    assert(vm.complete(done) && vm.poll(result) && result.ok);
    if (!extra) assert(strlen(result.action.text) == BotReplyLimit && !strstr(result.action.text, "truncated"));
    else assert(strlen(result.action.text) <= BotReplyLimit && strstr(result.action.text, "truncated; narrow prefix"));
  }
  invoke("!notes"); done = {}; done.token = request.token; done.ok = true; done.keys.count = 1;
  strcpy(done.keys.keys[0], "non\nascii");
  assert(vm.complete(done) && vm.poll(result) && result.ok &&
         !strcmp(result.action.text, "Error: non-ASCII note keys; ask owner to inspect stored data"));
  for (const char *command : {"!remember", "!remember key", "!remember \"\" value",
                              "!recall", "!forget", "!notes x y", "!list-memories x y"}) {
    assert(vm.start(2, event(command), error, sizeof(error)) && vm.poll(result) && !result.ok);
    assert(!vm.nextIo(request));
  }
  const auto tooLongKey = "!remember " + std::string(BotKeyLimit + 1, 'k') + " value";
  const auto tooLongValue = "!remember k " + std::string(121, 'v');
  for (const auto &command : {tooLongKey, tooLongValue}) {
    assert(vm.start(2, event(command.c_str()), error, sizeof(error)) && vm.poll(result) && !result.ok);
    assert(!vm.nextIo(request));
  }
  for (const char *command : {"!remember k leak", "!recall k", "!forget k", "!notes", "!list-memories"}) {
    for (bool channel : {false, true}) {
      auto input = event(command);
      if (channel) {
        input.authenticated = false; input.channelVerified = input.sharedState = true;
        strcpy(input.channel, "#example1"); strcpy(input.nickname, "owner"); memset(input.sender, 0, 32);
      } else input.local = true;
      assert(vm.start(2, input, error, sizeof(error)) && vm.poll(result) && !result.ok &&
             strstr(result.error, "authenticated private DM"));
      assert(!vm.nextIo(request));
    }
  }
  invoke("!notes");
  done = {}; done.token = request.token; done.ok = true; done.keys.count = BotKeysPerScope + 1;
  assert(!vm.complete(done));
  done.keys.count = 1; memset(done.keys.keys[0], 'x', sizeof(done.keys.keys[0]));
  assert(!vm.complete(done));
  done.keys.count = 2; strcpy(done.keys.keys[0], "duplicate"); strcpy(done.keys.keys[1], "duplicate");
  assert(!vm.complete(done));
  done = {}; done.token = request.token; done.ok = true;
  vm.cancel();
  assert(!vm.complete(done) && vm.poll(result) && !result.ok);
  for (const char *name : {"remember", "recall", "forget", "notes", "list-memories"}) {
    const auto bad = "function custom() return 'bad' end command('" + std::string(name) + "','','','custom')";
    assert(!vm.load(bad.data(), bad.size(), 2, stats, error, sizeof(error)));
  }
  for (const char *body : {"kv.list(1)", "kv.list('x\\0y')", "kv.list('', 'unknown')",
                           "kv.list('', 'bot')", "kv.list('', 'channel')", "kv.list('',nil,1)",
                           "kv.list('123456789012345678901234567890123')",
                           "kv.list=nil", "notes=function() end"}) {
    const auto bad = program(body);
    assert(vm.load(bad.data(), bad.size(), 2, stats, error, sizeof(error)));
    assert(vm.start(3, event("!custom"), error, sizeof(error)) && vm.poll(result) && !result.ok);
    assert(!vm.nextIo(request));
  }
  const char *enumerate =
      "function enumerate(scope) local r=kv.list('',scope) "
      "if r.truncated then return 'BAD' end return r.keys[1] or 'empty' end "
      "function mutate() local r=kv.list() r.keys[1]='bad' end";
  assert(vm.load(enumerate, strlen(enumerate), 3, stats, error, sizeof(error)));
  auto input = event("!enumerate channel"); input.authenticated = false;
  memset(input.sender, 0, 32); strcpy(input.channel, "#example1"); input.channelId[31] = 5;
  input.channelVerified = input.sharedState = true; input.sharedGrant = 7;
  assert(vm.start(4, input, error, sizeof(error)) && vm.nextIo(request) &&
         request.kind == BotIoRequest::List && request.scope == BotIoRequest::Channel &&
         request.principal[31] == 5 && request.grant == 7);
  done = {}; done.token = request.token; done.ok = true; done.keys.count = 1; strcpy(done.keys.keys[0], "board");
  assert(vm.complete(done) && vm.poll(result) && result.ok && !strcmp(result.action.text, "board"));
  assert(vm.start(5, event("!mutate"), error, sizeof(error)) && vm.nextIo(request));
  done.token = request.token;
  assert(vm.complete(done) && vm.poll(result) && !result.ok && strstr(result.error, "read-only"));
  assert(vm.start(6, event("!enumerate caller"), error, sizeof(error)) && vm.nextIo(request));
  done = {}; done.token = request.token; strcpy(done.error, "KV read cancelled/deadline");
  assert(vm.complete(done) && vm.poll(result) && !result.ok && strstr(result.error, "cancelled/deadline"));
  puts("PASS bundled notes and kv.list Lua: exact outcomes, shared alias, bounded RF summary, immutable keys, input/private-scope guards and completion/source fences");
}
static void utilityCommands() {
  for (bool channel : {false, true}) {
    auto request = [&](const char *command) {
      auto e = event(command);
      if (channel) {
        e.authenticated = false; e.channelVerified = true;
        memset(e.sender, 0, 32); strcpy(e.channel, "#example1"); strcpy(e.nickname, "untrusted owner");
      }
      return e;
    };
    assert(!strcmp(run(BotDefaultSource, request("!calc (2+3)*4")).text, "= 20"));
    assert(!strcmp(run(BotDefaultSource, request("!convert 32 F C")).text, "32 F = 0 C"));
    assert(strstr(run(BotDefaultSource, request("!roll")).text, "Roll 1d6: ["));
    assert(strstr(run(BotDefaultSource, request("!roll 12d1000-3")).text, "Roll 12d1000-3: ["));
    const auto choice = run(BotDefaultSource, request("!choose red | green | blue"));
    assert(strstr(choice.text, "Choice ") && strlen(choice.text) <= BotReplyLimit);
    for (const char *bad : {"!calc", "!calc 1/0", "!calc 1e18*2", "!calc 1e-18/2",
                            "!calc return 7", "!calc ((1)", "!convert 1 m kg", "!convert 1 MB b extra",
                            "!roll 0d6", "!roll 2d1", "!choose a||b"}) run(BotDefaultSource, request(bad), false);
  }
  auto e = event("!calc 2+3");
  e.authenticated = false; run(BotDefaultSource, e, false);
  strcpy(e.channel, "#example1"); run(BotDefaultSource, e, false);
  e.channelVerified = true; e.local = true; run(BotDefaultSource, e, false);
  e = event("!choose one|two"); e.replyLimit = 3; run(BotDefaultSource, e, false);
  assert(strstr(run(BotDefaultSource, event("!help")).text, "!help 2"));
  assert(strstr(run(BotDefaultSource, event("!help convert")).text, "Case-sensitive"));

  BotSession vm;
  BotVmStats stats;
  char error[128]{};
  const char *source =
      "function combined() local v=calc('2+2') for i=1,6 do v=calc('2+2') end "
      "timer.sleep(5) return v..'; '..convert('1','h','min') end "
      "function limit() for i=1,9 do calc('1') end end";
  assert(vm.load(source, strlen(source), 1, stats, error, sizeof(error)));
  BotIoRequest a, b;
  assert(vm.start(1, event("!combined"), error, sizeof(error)) && vm.nextIo(a));
  auto other = event("!combined"); other.sender[31] = 2;
  assert(vm.start(2, other, error, sizeof(error)) && vm.nextIo(b));
  BotSession::Result result;
  for (const auto &io : {b, a}) {
    BotIoResult done; done.token = io.token; done.ok = true;
    assert(vm.complete(done) && vm.poll(result) && result.ok &&
           !strcmp(result.action.text, "= 4; 1 h = 60 min"));
  }
  assert(vm.start(3, event("!limit"), error, sizeof(error)) && vm.poll(result) &&
         !result.ok && strstr(result.error, "8 calls"));
  assert(vm.start(4, event("!calc 1"), error, sizeof(error)) && vm.poll(result) && result.ok);
  for (const char *body : {"utility.calc('1\\0+2')", "utility.calc(123)", "utility.convert('1','m')",
                           "utility.roll(false)", "utility.roll('d6','extra')", "utility.choose('a\\n|b')",
                           "utility.calc=function() end", "utility={}", "calc=function() end"}) {
    const auto bad = program(body);
    assert(vm.load(bad.data(), bad.size(), 2, stats, error, sizeof(error)));
    assert(vm.start(5, event("!custom"), error, sizeof(error)) && vm.poll(result) && !result.ok);
  }
  for (const char *name : {"calc", "convert", "roll", "choose"}) {
    const auto bad = "function custom() return 'bad' end command('" + std::string(name) + "','','','custom')";
    assert(!vm.load(bad.data(), bad.size(), 3, stats, error, sizeof(error)));
    const auto overwrite = "function " + std::string(name) + "() return 'bad' end";
    assert(!vm.load(overwrite.data(), overwrite.size(), 3, stats, error, sizeof(error)));
  }
  const char *init = "utility.roll() function custom() end";
  assert(!vm.load(init, strlen(init), 3, stats, error, sizeof(error)));
  auto full = std::string("function custom() return 'maximum source' end --");
  full.resize(BotSourceLimit, 'x');
  assert(vm.load(full.data(), full.size(), 4, stats, error, sizeof(error)) &&
         vm.start(6, event("!custom"), error, sizeof(error)) && vm.poll(result) && result.ok);
  full += 'x'; assert(!vm.load(full.data(), full.size(), 5, stats, error, sizeof(error)));
  puts("PASS protected utility Lua: DM/verified-channel authority, nested overlapping coroutines, helper/command immutability, 8-call budget, full 4KiB custom source and truthful failures");
}
static void channelBoard() {
  BotSession vm;
  BotVmStats stats;
  char error[128]{};
  assert(vm.load(BotDefaultSource, strlen(BotDefaultSource), 1, stats, error, sizeof(error)));
  const auto channel = [&](const char *command) {
    auto e = event(command);
    e.authenticated = false; e.channelVerified = e.sharedState = e.targeted = true; e.sharedGrant = 7;
    memset(e.sender, 0, 32); e.channelId[0] = 9; e.channelId[31] = 11;
    strcpy(e.channel, "#example1"); strcpy(e.nickname, "not an identity");
    return e;
  };
  BotIoRequest request;
  unsigned job = 0;
  const auto start = [&](const char *command) {
    assert(vm.start(++job, channel(command), error, sizeof(error)) && vm.nextIo(request));
    assert(request.scope == BotIoRequest::Channel && request.grant == 7 &&
           request.principal[0] == 9 && request.principal[31] == 11);
  };
  const auto finish = [&](BotIoResult done) {
    done.token = request.token;
    BotSession::Result result;
    assert(vm.complete(done) && vm.poll(result) && result.ok);
    return std::string(result.action.text);
  };
  BotIoResult done; done.ok = true;
  start("!board put tea bring mugs");
  assert(request.kind == BotIoRequest::Put && !strcmp(request.key, "board:tea") &&
         !strcmp(request.value, "bring mugs"));
  assert(finish(done) == "Board tea created; committed");
  start("!board put tea refill"); done.found = true;
  assert(finish(done) == "Board tea replaced; committed");
  start("!board get tea"); strcpy(done.value, "refill");
  assert(request.kind == BotIoRequest::Get && finish(done) == "refill");
  for (bool found : {true, false}) {
    start("!board delete tea"); done = {}; done.ok = true; done.found = found;
    assert(request.kind == BotIoRequest::Delete &&
           finish(done) == (found ? "Board tea deleted; committed" : "No board entry: tea"));
  }
  start("!board list te"); done = {}; done.ok = true; done.keys.count = 2;
  strcpy(done.keys.keys[0], "board:tea"); strcpy(done.keys.keys[1], "board:test");
  assert(request.kind == BotIoRequest::List && !strcmp(request.key, "board:te") &&
         finish(done) == "Board (2): [tea] [test]");
  start("!board list tea"); done.keys.count = 1;
  assert(finish(done) == "Board (1): [tea]");
  start("!board list"); done = {}; done.ok = true;
  assert(finish(done) == "No board entries");
  start("!board list"); done.keys.count = 1; strcpy(done.keys.keys[0], "board:");
  assert(finish(done) == "Error: empty board key; ask owner to inspect stored data");
  start("!board list");
  done.keys.count = 8;
  for (unsigned i = 0; i < 8; ++i)
    strcpy(done.keys.keys[i], ("board:" + std::string(26, 'a' + i)).c_str());
  auto text = finish(done);
  assert(text.size() <= BotReplyLimit && text.find("truncated; narrow prefix") != std::string::npos &&
         text.find("board:") == std::string::npos);
  for (unsigned extra : {0u, 1u}) {
    start("!board list"); done = {}; done.ok = true; done.keys.count = 8;
    for (unsigned i = 0; i < 8; ++i)
      strcpy(done.keys.keys[i], ("board:" + std::string(i < 7 ? 16 : 16 + extra, 'a' + i)).c_str());
    text = finish(done);
    if (!extra) assert(text.size() == BotReplyLimit && text.find("truncated") == std::string::npos);
    else assert(text.size() <= BotReplyLimit && text.find("truncated") != std::string::npos);
  }
  start("!board get long"); done = {}; done.ok = done.found = true;
  memset(done.value, 'v', BotValueLimit);
  const auto longest = finish(done);
  if (BotValueLimit > BotReplyLimit)
    assert(longest.find("exceeds RF capacity") != std::string::npos);
  else assert(longest.size() <= BotReplyLimit && longest.find(std::string(BotValueLimit, 'v')) != std::string::npos);
  for (const char *command : {"!board put k", "!board get", "!board delete", "!board list k extra",
                              "!board get k extra", "!board delete k extra", "!board other"}) {
    BotSession::Result result;
    assert(vm.start(++job, channel(command), error, sizeof(error)) && vm.poll(result) &&
           result.ok && strstr(result.action.text, "Error:") && !vm.nextIo(request));
  }
  for (const auto &command : {"!board put " + std::string(27, 'k') + " v",
                              "!board put k " + std::string(121, 'v')}) {
    BotSession::Result result;
    assert(vm.start(++job, channel(command.c_str()), error, sizeof(error)) &&
           vm.poll(result) && !result.ok && !vm.nextIo(request));
  }
  for (unsigned mode = 0; mode < 5; ++mode) {
    auto e = channel("!board put k denied");
    if (mode == 0) e = event("!board put k denied");
    if (mode == 1) e.channelVerified = false;
    if (mode == 2) e.authenticated = true;
    if (mode == 3) e.local = true;
    if (mode == 4) e.sharedState = false;
    BotSession::Result result;
    assert(vm.start(++job, e, error, sizeof(error)) && vm.poll(result) && !vm.nextIo(request));
    assert(!result.ok && strstr(result.error, "not granted"));
  }
  start("!board list"); done = {}; done.token = request.token; done.ok = true; done.keys.count = 1;
  strcpy(done.keys.keys[0], "notes:private");
  assert(!vm.complete(done));
  vm.cancel(); BotSession::Result result;
  assert(!vm.complete(done) && vm.poll(result) && !result.ok);
  const char *mutate = "function custom() local r=kv.list('board:','channel') r.suffixes[1]='bad' end";
  assert(vm.load(mutate, strlen(mutate), 2, stats, error, sizeof(error)));
  start("!custom"); done = {}; done.ok = true; done.keys.count = 1; strcpy(done.keys.keys[0], "board:tea");
  done.token = request.token;
  assert(vm.complete(done) && vm.poll(result) && !result.ok && strstr(result.error, "read-only"));
  const char *bad = "function board() return 'override' end";
  assert(!vm.load(bad, strlen(bad), 3, stats, error, sizeof(error)));
  puts("PASS bundled board Lua: yielding shared prefix, relative immutable keys, exact RF bounds, explicit outcomes, private/grant/input denial and stale completion guards");
}
static void diagnosticCommands() {
  auto e = event("!status");
  e.node.available = e.node.hasIdentity = e.node.enabled = e.node.ready = true;
  e.node.rolesKnown = e.node.wifiKnown = true;
  e.node.selectedRoles = 15; e.node.readyRoles = 5;
  e.node.uptimeMs = uint64_t(UINT32_MAX) + 2001;
  e.node.publicKey[0] = 0xab;
  strcpy(e.node.nativeRevision, "d92964352441"); strcpy(e.node.build, "fixture compilation");
  e.air.available = true; e.air.transmitting = true;
  e.air.creditMs = e.air.aggregateCreditMs = e.air.rfMs = e.air.aggregateRfMs = UINT32_MAX;
  e.air.successes = e.air.failures = e.air.aggregateSuccesses = e.air.aggregateFailures = UINT32_MAX;
  e.air.generation = e.air.configurationGeneration = UINT32_MAX;
  e.air.reservedMs = e.air.remainingMs = e.air.limitMs = UINT32_MAX;
  e.air.queued = UINT16_MAX; e.air.aggregateQueued = UINT8_MAX;
  const auto command = [&](const char *text) {
    auto request = e; char error[128]{};
    assert(parseBotCommand(text, strlen(text), request, error, sizeof(error)));
    const auto reply = run(BotDefaultSource, request);
    assert(strlen(reply.text) <= e.replyLimit);
    for (const char *p = reply.text; *p; ++p) assert(*p >= 32 && *p <= 126);
    return std::string(reply.text);
  };
  for (bool channel : {false, true}) {
    e.authenticated = !channel; e.channelVerified = channel;
    strcpy(e.channel, channel ? "#example1" : "");
    e.replyLimit = channel ? BotReplyLimit - 33 : BotReplyLimit;
    assert(command("!about").find("key=ab00") != std::string::npos);
    assert(command("!version").find("MeshCore rev d92964352441; Lua 5.5.1; built fixture compilation") == 0);
    assert(command("!uptime") == "Uptime snapshot since boot: 49d 17h 2m 49s");
    auto status = command("!status");
    assert(status.find("WiFi=disconnected") != std::string::npos &&
           status.find("battery=unavailable") != std::string::npos &&
           status.find("roles selected/ready=15/5") != std::string::npos);
    for (const char *page : {"!air", "!air 1", "!air 2", "!air 3", "!air 4"}) {
      const auto result = command(page);
      assert(result.find("Error:") == std::string::npos && result.find("4294967295") != std::string::npos);
    }
    assert(command("!help air").find("page?:int:1:4") != std::string::npos);
    assert(command("!help service").find("unavailable here") != std::string::npos);
    assert(command("!help").find("!service") == std::string::npos);
  }
  e.authenticated = true; e.channelVerified = false; e.channel[0] = 0;
  e.node.enabled = e.node.ready = false; e.node.selectedRoles = e.node.readyRoles = 0;
  assert(command("!status").find("bot=off") != std::string::npos);
  e.node.enabled = true; e.node.fault = true; e.node.wifiConnected = true;
  assert(command("!status").find("bot=not-ready fault=yes WiFi=connected") != std::string::npos);
  e.local = true;
  assert(command("!signal").find("local reflection; RF unmeasured") != std::string::npos);
  assert(command("!signal").find("RSSI") == std::string::npos);
  e.local = false;
  for (uint8_t width : {1, 2, 3}) {
    e.path.width = width; e.path.count = 1;
    memset(e.path.bytes, 0xab, width);
    assert(command("!signal").find(std::string("path=") +
           (width == 1 ? "ab" : width == 2 ? "abab" : "ababab")) != std::string::npos);
  }
  e.path.width = 1; e.path.count = 63;
  assert(command("!signal").find("path omitted; use !path") != std::string::npos);
  e.path.known = false; e.signal = false;
  assert(command("!signal") == "Packet RF measurement unavailable; path=unknown (direct)");
  e.node.available = e.air.available = false;
  for (const char *c : {"!about", "!version", "!uptime", "!status", "!air"})
    assert(command(c).find("unavailable") != std::string::npos);
  e.authenticated = false;
  run(BotDefaultSource, e, false);
  for (const char *bad : {"!air 0", "!air 5", "!air text", "!about extra", "!status extra"})
    run(BotDefaultSource, event(bad), false);
  for (const char *name : {"about", "version", "uptime", "status", "signal", "air"})
    validate("function " + std::string(name) + "() return 'override' end", false);
  run(program("ctx.node.ready=false"), event("!custom"), false);
  run(program("ctx.air.credit_ms=0"), event("!custom"), false);
  run(program("node.report=function() return 'fake' end"), event("!custom"), false);
  validate("function node() return 'override' end", false);
  validate("node.report('status') function custom() end", false);
  for (const char *args : {"'status',1", "'air',0", "'air',5", "'air','1'",
                           "'air',1.5", "'signal\\0other'", "'unknown'", "'air',1,2"})
    run(program("return node.report(" + std::string(args) + ")"), event("!custom"), false);
  BotSession vm;
  BotVmStats stats;
  char error[128]{};
  for (const char *source : {"node={} function custom() end",
                            "function status() return 'override' end",
                            "node.report=function() end function custom() end"})
    assert(!vm.load(source, strlen(source), 1, stats, error, sizeof(error)));
  const char *source =
      "function custom() return node.report('signal') end "
      "function change() node={} return 'bad' end";
  assert(vm.load(source, strlen(source), 2, stats, error, sizeof(error)));
  assert(vm.manifest().count == 2 && vm.manifest().find("custom"));
  BotSession::Result result;
  assert(vm.start(1, event("!custom"), error, sizeof(error)) && vm.poll(result) &&
         result.ok && strstr(result.action.text, "RSSI=-90dBm SNR=5dB"));
  assert(vm.start(2, event("!change"), error, sizeof(error)) && vm.poll(result) &&
         !result.ok && strstr(result.error, "reserved"));
  assert(vm.start(3, event("!signal"), error, sizeof(error)) && vm.poll(result) &&
         result.ok && strstr(result.action.text, "RSSI=-90dBm SNR=5dB"));
  auto bad = event("!version"); memset(bad.node.build, 'x', sizeof(bad.node.build));
  run(BotDefaultSource, bad, false);
  puts("PASS diagnostics Lua: real-metadata contract, RF-capacity high counters, explicit missing data, private-free DM/channel help, local signal and full-width paths");
}
static void packageModules() {
  const std::string source =
      "module('base',function() local n=0 return {next=function() n=n+1 return n end} end) "
      "module('greet',function() local b=require('base') return {hello=function(name) "
      "return 'Hello '..name..' '..tostring(b.next()) end} end) "
      "function hello(name) return require('greet').hello(name) end";
  const auto manifest = validate(source);
  assert(manifest.count == 1 && manifest.moduleCount == 2 && !strcmp(manifest.modules[0], "base"));
  assert(!strcmp(run(source, event("!hello first")).text, "Hello first 1"));
  BotSession vm; BotVmStats stats; char error[128]{};
  assert(vm.load(source.data(), source.size(), 1, stats, error, sizeof(error)));
  assert(vm.manifest().moduleCount == 2);
  for (unsigned i = 1; i <= 2; ++i) {
    assert(vm.start(i, event("!hello retained"), error, sizeof(error)));
    BotSession::Result result; assert(vm.poll(result) && result.ok);
    assert(result.action.text == std::string("Hello retained ") + std::to_string(i));
  }
  assert(vm.load(source.data(), source.size(), 2, stats, error, sizeof(error)));
  assert(vm.start(3, event("!hello reboot"), error, sizeof(error)));
  BotSession::Result result; assert(vm.poll(result) && result.ok && !strcmp(result.action.text, "Hello reboot 1"));
  const char *io =
      "module('state',function() return {read=function() return kv.get('module') end} end) "
      "function read() return require('state').read() end";
  assert(vm.load(io, strlen(io), 3, stats, error, sizeof(error)));
  assert(vm.start(4, event("!read"), error, sizeof(error)));
  BotIoRequest request; assert(vm.nextIo(request) && request.kind == BotIoRequest::Get && request.principal[0] == 1);
  BotIoResult done; done.token = request.token; done.ok = done.found = true; strcpy(done.value, "module yield");
  assert(vm.complete(done) && vm.poll(result) && result.ok && !strcmp(result.action.text, "module yield"));
  for (const char *bad : {
      "module('a',function() return require('b') end) module('b',function() return require('a') end)",
      "module('a',function() return require('missing') end)",
      "module('a',function() while true do end end)",
      "module('a',function() reply('staging effect') return {} end)",
      "module('a',function() return false end)",
      "module('a',function() return {} end) module('a',function() return {} end)",
      "module('../file',function() return {} end)",
      "module('node',function() return {} end)",
      "module('binary','\\27Lua')",
      "module('a',function() module('b',function() return {} end) return {} end)",
      "require='replaced'"}) {
    const auto complete = std::string(bad) + " function hello() return 'never' end";
    validate(complete, false);
    assert(!vm.load(complete.data(), complete.size(), 4, stats, error, sizeof(error)));
  }
  std::string excessive;
  for (unsigned i = 0; i < 9; ++i)
    excessive += "module('m" + std::to_string(i) + "',function() return {} end) ";
  validate(excessive + "function hello() return 'no' end", false);
  const std::string late = "function late() module('late',function() return {} end) end";
  assert(vm.load(late.data(), late.size(), 5, stats, error, sizeof(error)));
  assert(vm.start(1, event("!late"), error, sizeof(error)) && vm.poll(result) && !result.ok);
  puts("PASS package modules: declared native resolution/cache, shared environment, nested dependencies/yields, reload, cycles, binary/path/native denial and eager bounded side-effect-free initialization");
}
static void namedSourceCapacity() {
  std::string files[BotSourcePartLimit];
  const auto envelope = [&]() {
    std::string source = "--@meshcore-sources/1\n";
    for (unsigned i = 0; i < BotSourcePartLimit; ++i)
      source += "--@source file" + std::to_string(i) + " " + std::to_string(files[i].size()) +
                "\n" + files[i] + "\n";
    return source;
  };
  for (unsigned i = 0; i < BotSourcePartLimit; ++i) {
    const auto number = std::to_string(i), name = "file" + number;
    files[i] = "function " + name + "() return settings.value..':" + number +
               "' end command('" + name + "','','File " + number + "')\n";
  }
  files[0] = "settings={value='configured'}\n" + files[0];
  const auto last = files[BotSourcePartLimit - 1];
  std::string source;
  for (size_t padding = 0; padding < BotSourceLimit; ++padding) {
    files[BotSourcePartLimit - 1] = last + "--" + std::string(padding, 'x') + "\n";
    source = envelope();
    if (source.size() >= BotSourceLimit) break;
  }
  assert(source.size() == BotSourceLimit);
  BotSourcePart parts[BotSourcePartLimit]{};
  unsigned count = 0; char error[128]{};
  assert(splitBotSources(source.data(), source.size(), parts, count, error, sizeof(error)) &&
         count == BotSourcePartLimit);
  BotSession vm; BotVmStats stats; BotVmLimits limits;
  const bool loaded = vm.load(source.data(), source.size(), 1, stats, error, sizeof(error), limits);
  if (!loaded) fprintf(stderr, "File-set capacity failed: %s (peak=%zu limit=%zu)\n",
                       error, stats.peakBytes, limits.heapBytes);
  assert(loaded && vm.manifest().count == BotSourcePartLimit && stats.peakBytes <= limits.heapBytes);
  const auto peak = stats.peakBytes;
  for (unsigned i = 0; i < BotSourcePartLimit; ++i) {
    BotSession::Result result;
    const auto command = "!file" + std::to_string(i);
    assert(vm.start(i + 1, event(command.c_str()), error, sizeof(error)) && vm.poll(result) && result.ok);
    assert(result.action.text == std::string("configured:") + std::to_string(i));
  }
  auto excessive = source + " ";
  assert(!splitBotSources(excessive.data(), excessive.size(), parts, count, error, sizeof(error)));
  files[BotSourcePartLimit - 1] = last;
  excessive = envelope() + "--@source extra 3\n--x\n";
  assert(!splitBotSources(excessive.data(), excessive.size(), parts, count, error, sizeof(error)) &&
         strstr(error, "eight-source"));
  printf("PASS named source capacity: files=%u envelope=%zu heap_limit=%zu load_peak=%zu jobs=%u; shared config/eight handlers, 4097-byte and ninth-file rejection\n",
         BotSourcePartLimit, BotSourceLimit, limits.heapBytes, peak, BotJobLimit);
}
static void namedSourcesAndRepeaterVm() {
  const auto set = [](const std::string &a, const std::string &b) {
    return std::string("--@meshcore-sources/1\n--@source main ") + std::to_string(a.size()) +
        "\n" + a + "\n--@source monitor " + std::to_string(b.size()) + "\n" + b + "\n";
  };
  const std::string main =
      "function shared_value() return 'shared' end function custom() return shared_value() end "
      "command('custom','','Shared procedure')";
  const std::string monitor =
      "function fleet_poll() local peer=repeater.next() if peer then "
      "local r=repeater.status(peer) if r.ok and r.battery_volts~=3.811 then error('voltage') end "
      "if not r.ok and r.code~='timeout' then error('failure') end end end "
      "events.every(15,'fleet_poll')";
  BotSession vm; BotVmStats stats; char error[128]{};
  auto source = set(main, monitor);
  assert(vm.load(source.data(), source.size(), 17, stats, error, sizeof(error)));
  assert(vm.manifest().scheduleSeconds == 15 && vm.subscriptions() == 16);
  BotSession::Result result;
  assert(vm.start(1, event("!custom"), error, sizeof(error)) && vm.poll(result) &&
         result.ok && !strcmp(result.action.text, "shared"));
  std::atomic<uint32_t> epoch{7}; vm.setEventEpoch(&epoch);
  BotEvent scheduled; scheduled.kind = BotEvent::Scheduled; scheduled.eventEpoch = 7;
  BotIoRequest request;
  for (bool success : {true, false}) {
    assert(vm.start(2, scheduled, error, sizeof(error)) && vm.nextIo(request) &&
           request.kind == BotIoRequest::RepeaterNext && request.eventEpoch == 7);
    BotIoResult done; done.token = request.token; done.ok = done.found = true;
    strcpy(done.value, "pilot");
    assert(vm.complete(done) && vm.nextIo(request) &&
           request.kind == BotIoRequest::RepeaterStatus && !strcmp(request.key, "pilot"));
    done = {}; done.token = request.token; done.ok = success;
    done.repeater.available = done.repeater.fresh = success; done.repeater.stats.batteryMv = 3811;
    done.repeater.error = success ? BotRepeaterError::None : BotRepeaterError::Timeout;
    strcpy(done.error, success ? "" : "Repeater response timed out");
    assert(vm.complete(done) && vm.poll(result) && result.ok && result.action.kind == BotAction::None);
  }
  for (const std::string &other : {
         std::string("function shared_value() return 'collision' end command('other','','Collision','shared_value')"),
         std::string("function other() end command('custom','','Duplicate','other')"),
         std::string("function other() end events.every(30,'other') events.every(15,'other')")}) {
    source = set(main, other);
    assert(!vm.load(source.data(), source.size(), 18, stats, error, sizeof(error)));
    assert(strstr(error, "collision") || strstr(error, "Duplicate") || strstr(error, "duplicate") ||
           strstr(error, "one declaration"));
  }
  source = set(BotDefaultSource,
      "function fleet_poll() local p=repeater.next() if p then repeater.status(p) end end events.every(15,'fleet_poll')");
  assert(source.size() <= BotSourceLimit);
  assert(vm.load(source.data(), source.size(), 19, stats, error, sizeof(error)));
  assert(vm.start(3, event("!ping"), error, sizeof(error)) && vm.poll(result) &&
         result.ok && !strcmp(result.action.text, "Pong"));
  source = std::string("--@meshcore-sources/1\n--@builtin main\n--@source monitor ") +
      std::to_string(monitor.size()) + "\n" + monitor + "\n";
  assert(vm.load(source.data(), source.size(), 20, stats, error, sizeof(error)));
  assert(vm.manifest().scheduleSeconds == 15 && vm.subscriptions() == 16);
  assert(vm.start(4, event("!ping"), error, sizeof(error)) && vm.poll(result) &&
         result.ok && !strcmp(result.action.text, "Pong"));
  source = "--@meshcore-sources/1\n--@builtin main\n";
  assert(vm.load(source.data(), source.size(), 21, stats, error, sizeof(error)));
  assert(!vm.subscriptions() && !vm.manifest().scheduleSeconds);
  assert(vm.start(5, event("!ping"), error, sizeof(error)) && vm.poll(result) &&
         result.ok && !strcmp(result.action.text, "Pong"));
  for (const char *bad : {"events.every(0,'work')", "events.every(86401,'work')",
                         "events.every(15,'missing')", "events.every(15,'ping')",
                         "events.on('scheduled','work')"}) {
    const std::string text = std::string("function work() end ") + bad;
    assert(!vm.load(text.data(), text.size(), 20, stats, error, sizeof(error)));
  }
  source = "function stop() events.off('scheduled') return 'stopped' end "
           "function poll() end events.every(15,'poll')";
  assert(vm.load(source.data(), source.size(), 22, stats, error, sizeof(error)));
  assert(vm.start(6, event("!stop"), error, sizeof(error)) && vm.poll(result) &&
         result.ok && !strcmp(result.action.text, "stopped") && !vm.subscriptions());
  BotSourcePart parts[BotSourcePartLimit]{}; unsigned count = 0;
  for (const char *bad : {"--@meshcore-sources/2\n", "--@meshcore-sources/1\n",
                         "--@meshcore-sources/1\n--@source a 9\nshort\n",
                         "--@meshcore-sources/1\n--@source a 1\nx\n--@source a 1\ny\n"}) {
    assert(!splitBotSources(bad, strlen(bad), parts, count, error, sizeof(error)));
  }
  uint8_t bytes[56]{};
  bytes[0] = 0xe3; bytes[1] = 0x0e; bytes[4] = 0x9c; bytes[5] = 0xff;
  bytes[42] = 0xf9; bytes[43] = 0xff; bytes[52] = 42;
  BotRepeaterStats decoded;
  assert(decodeBotRepeaterStats(bytes, sizeof(bytes), decoded) && decoded.batteryMv == 3811 &&
         decoded.noise == -100 && decoded.snrQuarterDb == -7 && decoded.receiveErrors == 42);
  assert(!decodeBotRepeaterStats(bytes, 48, decoded) && !decoded.batteryMv);
  puts("PASS named Lua sources: shared procedures, collision rejection, preserved bundled commands, recurring declarations, typed repeater success/failure and native status decoding");
}
static void subscriptionVm() {
  BotSession vm; BotVmStats stats; char error[128]{};
  const char *source =
      "function start() kv.put('seen',ctx.kind,'bot') end events.on('startup','start') "
      "function message() local c=ctx sleep(1) "
      "kv.put('seen',c.message,c.channel.present and 'channel' or 'bot') end events.on('message','message') "
      "function off() events.off('message') return 'off' end";
  assert(vm.load(source, strlen(source), 1, stats, error, sizeof(error)));
  assert(vm.manifest().count == 1 && vm.subscriptions() == 5);
  std::atomic<uint32_t> epoch{1}; vm.setEventEpoch(&epoch);
  BotEvent e; e.kind = BotEvent::Startup; e.eventEpoch = 1; e.sharedState = true; e.sharedGrant = 4;
  assert(vm.start(1, e, error, sizeof(error)));
  BotIoRequest request; assert(vm.nextIo(request) && request.kind == BotIoRequest::Put &&
      request.scope == BotIoRequest::Bot && request.eventEpoch == 1 && !strcmp(request.value, "startup"));
  BotIoResult done; done.token = request.token; done.ok = true;
  assert(vm.complete(done));
  BotSession::Result result; assert(vm.poll(result) && result.ok && result.action.kind == BotAction::None);
  e.kind = BotEvent::Message; e.channelVerified = true; e.channelId[31] = 9;
  strcpy(e.channel, "#example1"); strcpy(e.nickname, "owner"); strcpy(e.message, "channel content");
  assert(vm.start(2, e, error, sizeof(error)) && vm.nextIo(request) && request.kind == BotIoRequest::Sleep);
  strcpy(e.message, "mutated input");
  done.token = request.token; assert(vm.complete(done));
  assert(vm.nextIo(request) && request.scope == BotIoRequest::Channel && request.principal[31] == 9 &&
         !strcmp(request.value, "channel content"));
  ++epoch; done.token = request.token; assert(vm.complete(done));
  assert(vm.poll(result) && !result.ok && strstr(result.error, "epoch"));
  assert(vm.start(3, event("!off"), error, sizeof(error)) && vm.poll(result) && result.ok);
  assert(vm.subscriptions() == 1 && epoch == 3);
  for (const char *body : {"kv.get('private')", "reply('no route')", "mesh.send(mesh.compose('text','no'))",
                           "reminder.after(1,'no')", "rpc.call('home','health',{})", "ctx.kind='changed'",
                           "while true do end"}) {
    const auto bad = std::string("function _event() ") + body + " end events.on('message','_event')";
    assert(vm.load(bad.data(), bad.size(), 2, stats, error, sizeof(error)));
    auto dm = event("!unused"); dm.kind = BotEvent::Message; dm.sharedState = dm.homeAccess = dm.reminderAccess = true;
    assert(vm.start(1, dm, error, sizeof(error)) && vm.poll(result) && !result.ok);
    assert(!vm.nextIo(request));
  }
  for (const char *bad : {"events.on('unknown','handler')", "events.on('startup','missing')",
                          "events.on('startup','ping')",
                          "events.on('startup','handler') events.on('startup','handler')"}) {
    const auto text = std::string("function handler() end ") + bad;
    assert(!vm.load(text.data(), text.size(), 3, stats, error, sizeof(error)));
  }
  puts("PASS event VM: declared handlers, no staging invocation, immutable copied ctx, bounded yield, bot/channel scopes, private/RF/RPC denial, revoke/off fences and runaway budgets");
}
static void commandDiscoveryAndMeshGrants() {
  BotSession vm; BotVmStats stats; char error[128]{};
  const char *source =
      "module('shared',function() return {} end) "
      "function open() return 'open' end command('open','','Public command','open','public','!open') "
      "function restricted() return 'owner' end command('restricted','','Owner only','restricted','owner') "
      "function listening() local p=mesh.wait{kind='channel',prefix='answer:'} "
      "if p.authenticated or p.from or p.nickname~='owner' then return 'BAD AUTH' end return p.text end";
  assert(vm.load(source, strlen(source), 1, stats, error, sizeof(error)));
  BotSession::Result result;
  unsigned job = 0;
  const auto invoke = [&](const char *command, bool owner = false) {
    auto e = event(command); e.owner = owner; e.node.sourceGeneration = 42;
    assert(vm.start(++job, e, error, sizeof(error)) && vm.poll(result));
    return result;
  };
  assert(!invoke("!restricted").ok);
  assert(invoke("!restricted", true).ok && !strcmp(result.action.text, "owner"));
  const auto first = invoke("!help");
  assert(first.ok);
  unsigned page = 0, pages = 0;
  assert(sscanf(first.action.text, "Help %u/%u:", &page, &pages) == 2 && page == 1 && pages > 1);
  std::string discovery;
  for (unsigned i = 1; i <= pages; ++i) {
    const auto r = invoke(("!help " + std::to_string(i)).c_str());
    assert(r.ok && strlen(r.action.text) <= BotReplyLimit);
    discovery += r.action.text;
  }
  assert(discovery.find("!neighbors") != std::string::npos && discovery.find("!plugins") != std::string::npos &&
         discovery.find("!open") != std::string::npos && discovery.find("!restricted") == std::string::npos &&
         discovery.find("!admin") == std::string::npos);
  assert(!invoke("!help 40").ok && !invoke("!help 0").ok);
  assert(strstr(invoke("!help restricted").action.text, "unavailable here"));
  assert(strstr(invoke("!help open").action.text, "e.g. !open"));
  assert(strstr(invoke("!plugins").action.text, "g42 custom 3 commands, 1 modules"));
  assert(strstr(invoke("!plugins 2").action.text, "shared"));
  BotIoRequest io;
  assert(vm.start(++job, event("!neighbors"), error, sizeof(error)) && vm.nextIo(io) &&
         io.kind == BotIoRequest::Inspect && io.revision == 1);
  vm.cancel(); BotIoResult done; done.ok = true; done.token = io.token;
  assert(!vm.complete(done) && vm.poll(result) && !result.ok);
  assert(!invoke("!admin bot status").ok);
  auto owner = event("!admin bot status"); owner.owner = true;
  assert(vm.start(++job, owner, error, sizeof(error)) && vm.nextIo(io) && io.kind == BotIoRequest::Admin);
  done = {}; done.token = io.token; done.ok = true; strcpy(done.value, "bot ready");
  assert(vm.complete(done) && vm.poll(result) && result.ok && !strcmp(result.action.text, "bot ready"));
  auto channel = event("!listening");
  channel.authenticated = false; memset(channel.sender, 0, 32);
  channel.channelVerified = channel.targeted = true; strcpy(channel.channel, "#example1");
  assert(vm.start(++job, channel, error, sizeof(error)) && vm.poll(result) && !result.ok);
  channel.channelWait = true; channel.meshGrant = 7;
  assert(vm.start(++job, channel, error, sizeof(error)) && vm.nextIo(io) &&
         io.waitKind == BotIoRequest::ChannelWait && io.grant == 7);
  done = {}; done.token = io.token; done.ok = true;
  done.packet.channel = true; strcpy(done.packet.nickname, "owner"); strcpy(done.value, "answer:yes");
  assert(vm.complete(done) && vm.poll(result) && result.ok && !strcmp(result.action.text, "answer:yes"));
  const std::string key = "02" + std::string(62, '0');
  const auto send = "function destination() return mesh.send(mesh.compose{to='" + key + "',text='question'}).transmitted end";
  assert(vm.load(send.data(), send.size(), 2, stats, error, sizeof(error)));
  auto e = event("!destination");
  assert(vm.start(1, e, error, sizeof(error)) && vm.poll(result) && !result.ok && !vm.nextIo(io));
  e.destinations[0][0] = 2; e.meshGrant = 9;
  assert(vm.start(2, e, error, sizeof(error)) && vm.nextIo(io) &&
         io.kind == BotIoRequest::Send && io.principal[0] == 2 && io.grant == 9);
  vm.cancel(); done = {}; done.token = io.token; done.ok = true;
  assert(!vm.complete(done) && vm.poll(result) && !result.ok);
  validate("function custom() end command('custom','','',nil,'ungranted-root')", false);
  puts("PASS command discovery/mesh: real pagination, permission parity, examples/modules, native inspection/admin yields, granted DM, unauthenticated channel wait and cancellation");
}
static void boundedJson() {
  const auto check = [](const std::string &body, const char *expected) {
    assert(!strcmp(run(program("return " + body), event("!custom")).text, expected));
  };
  check("(function() local t={name='olé',values={1,true,json.null},blank=json.array()} "
        "local x=json.decode(json.encode(t)) "
        "if x.name=='olé' and x.values[3]==json.null and json.encode(x.blank)=='[]' "
        "then return 'ok' else return 'bad' end end)()", "ok");
  check("(function() local t=json.decode('{\"arr\":[true,null,\"\\\\uD83D\\\\uDE00\"],\"empty\":{}}') "
        "if t.arr[3]=='😀' and json.encode(t.empty)=='{}' then return 'ok' else return 'bad' end end)()",
        "ok");
  check("json.encode(json.decode('{\"n\":-1.25e2,\"text\":\"a\\\\u0000b\"}').text)",
        "\"a\\u0000b\"");
  check("json.encode(json.decode('[]'))", "[]");
  check("json.encode(json.decode('null'))", "null");
  check("(function() local t=json.decode(json.encode({zero=0,last=9223372036854775807})) "
        "if t.last==9223372036854775807 and t.zero==0 then return 'ok' end end)()", "ok");
  for (const char *body : {
           "json.encode(0/0)", "json.encode(1/0)", "json.encode(function() end)",
           "json.encode({[1]='a',label='b'})",
           "local a={} a[1]=a json.encode(a)",
           "local a={} for i=1,65 do a[i]=i end json.encode(a)",
           "local a={} local t=a for i=1,9 do t.x={} t=t.x end json.encode(a)",
           "local x='' for i=1,1025 do x=x..'x' end json.encode(x)",
           "json.encode(' bad\\255')",
           "local x='' for i=1,1024 do x=x..'x' end json.encode({key=x})",
           "json.decode('{\"duplicate\":1,\"duplicate\":2}')",
           "json.decode('\"\\\\uD800\"')", "json.decode('\"\\\\uDEAD\"')",
           "json.decode('\"\\\\uD800\\\\u0041\"')",
           "json.decode('\"\\xC0\\xAF\"')",
           "json.decode('1e999')", "json.decode('01')",
           "json.decode('[1,]')", "json.decode('true false')",
           "local x='' for i=1,2049 do x=x..' ' end json.decode(x)",
           "local x='' for i=1,9 do x=x..'[' end x=x..'0' for i=1,9 do x=x..']' end json.decode(x)",
           "local x='[' for i=1,64 do x=x..'0,' end json.decode(x..'0]')",
           "json.null.extra=1", "json.encode=nil", "json={}"
       }) run(program(body), event("!custom"), false);
  validate("json={} function custom() end", false);
  puts("PASS JSON VM: roundtrip, null, array, UTF-8, escaping, depth/element/byte/cycle and invalid-input bounds");
}
#if !ONCHIP_BOT_COMPACT_PROFILE
static void networkVm() {
  BotSession vm;
  BotVmStats stats;
  char error[128]{};
  const char *source =
      "function get() local r=http.get('weather') "
      "if not r.ok then return r.error.code..':'..tostring(r.http_status) end "
      "return r.body.place end "
      "function post() local r=http.post('metrics',{values={1,true,json.null}}) "
      "return r.body.accepted and 'accepted' or 'rejected' end "
      "function custom() local r=rpc.call('metrics','readings',{device='node',n=12}) "
      "if not r.ok then return r.error.code end return tostring(r.result.count) end "
      "function nullable() local r=http.get('empty') "
      "if not r.ok then return r.error.code end "
      "if r.body==json.null then return 'null' end return 'other' end "
      "function legacy() local r=rpc.call('home','weather',{place='Paris'}) "
      "return r.result.location end "
      "function invalid() return http.get('missing') end";
  assert(vm.load(source, strlen(source), 1, stats, error, sizeof(error)));
  auto granted = event("!get");
  granted.homeAccess = true; granted.homeGrant = 17;
  BotIoRequest request;
  BotIoResult completion;
  BotSession::Result result;
  assert(vm.start(1, granted, error, sizeof(error)) && vm.nextIo(request));
  assert(request.kind == BotIoRequest::HttpGet && !strcmp(request.endpoint, "weather") &&
         !request.json[0] && request.grant == 17 && request.principal[0] == 1);
  assert(!vm.poll(result));
  completion.token = request.token; completion.ok = true; completion.httpStatus = 200;
  strcpy(completion.json, "{\"place\":\"Paris\"}");
  assert(vm.complete(completion) && vm.poll(result) && result.ok &&
         !strcmp(result.action.text, "Paris"));
  strcpy(granted.name, "post");
  assert(vm.start(2, granted, error, sizeof(error)) && vm.nextIo(request));
  assert(request.kind == BotIoRequest::HttpPost && !strcmp(request.endpoint, "metrics") &&
         !strcmp(request.json, "{\"values\":[1,true,null]}"));
  completion = {}; completion.token = request.token; completion.ok = true;
  completion.httpStatus = 201; strcpy(completion.json, "{\"accepted\":true}");
  assert(vm.complete(completion) && vm.poll(result) && result.ok &&
         !strcmp(result.action.text, "accepted"));
  strcpy(granted.name, "custom");
  assert(vm.start(3, granted, error, sizeof(error)) && vm.nextIo(request));
  assert(request.kind == BotIoRequest::Rpc && !strcmp(request.endpoint, "metrics") &&
         !strcmp(request.key, "readings") &&
         (!strcmp(request.json, "{\"device\":\"node\",\"n\":12}") ||
          !strcmp(request.json, "{\"n\":12,\"device\":\"node\"}")));
  completion = {}; completion.token = request.token; completion.ok = true;
  completion.httpStatus = 200; strcpy(completion.json, "{\"count\":3}");
  assert(vm.complete(completion) && vm.poll(result) && result.ok &&
         !strcmp(result.action.text, "3"));
  assert(vm.start(31, granted, error, sizeof(error)) && vm.nextIo(request));
  completion = {}; completion.token = request.token; completion.ok = true;
  completion.httpStatus = 200; strcpy(completion.json, "\"unterminated");
  assert(vm.complete(completion) && vm.poll(result) && result.ok &&
         !strcmp(result.action.text, "invalid_response"));
  strcpy(granted.name, "nullable");
  assert(vm.start(32, granted, error, sizeof(error)) && vm.nextIo(request));
  completion = {}; completion.token = request.token; completion.ok = true;
  completion.httpStatus = 200; strcpy(completion.json, "null");
  assert(vm.complete(completion) && vm.poll(result) && result.ok &&
         !strcmp(result.action.text, "null"));
  strcpy(granted.name, "legacy");
  assert(vm.start(4, granted, error, sizeof(error)) && vm.nextIo(request));
  assert(request.kind == BotIoRequest::Rpc && !strcmp(request.endpoint, "home") &&
         !strcmp(request.key, "weather") && !strcmp(request.value, "Paris") &&
         !strcmp(request.json, "{\"place\":\"Paris\"}"));
  completion = {}; completion.token = request.token; completion.ok = true;
  completion.httpStatus = 200; strcpy(completion.value, "Paris");
  assert(vm.complete(completion) && vm.poll(result) && result.ok &&
         !strcmp(result.action.text, "Paris"));
  strcpy(granted.name, "get");
  assert(vm.start(5, granted, error, sizeof(error)) && vm.nextIo(request));
  completion = {}; completion.token = request.token; completion.ok = true;
  completion.httpStatus = 200; strcpy(completion.json, "{\"place\":");
  assert(vm.complete(completion) && vm.poll(result) && result.ok &&
         !strcmp(result.action.text, "invalid_response:200"));
  assert(vm.start(6, granted, error, sizeof(error)) && vm.nextIo(request));
  completion = {}; completion.token = request.token; completion.ok = false;
  completion.httpStatus = 403; strcpy(completion.rpcCode, "denied");
  assert(vm.complete(completion) && vm.poll(result) && result.ok &&
         !strcmp(result.action.text, "denied:403"));
  completion = {}; completion.token = request.token; completion.ok = true;
  memset(completion.json, 'x', sizeof(completion.json));
  assert(vm.start(7, granted, error, sizeof(error)) && vm.nextIo(request));
  completion.token = request.token;
  assert(!vm.complete(completion));
  vm.cancel(); assert(vm.poll(result) && !result.ok);
  for (const char *body : {
           "http.get('https://example.com/')", "http.get('foo/bar')",
           "http.get('Bad')", "http.get('a\\0b')",
           "http.post('metrics',nil)", "http.post('metrics',{x=function() end})",
           "rpc.call('home','health',nil)", "rpc.call('metrics','readings',{1,2})",
           "rpc.call('metrics','readings',json.array())", "rpc.call('home','bad/op',{})",
           "rpc.call('home','echo',{text=1})",
           "rpc.call('home','health',{other=true})",
           "rpc.call('home','echo',{text='ok',other=true})",
           "http.get('weather','GET')", "rpc.call('home','health',{},'extra')"
       }) {
    const auto bad = program(body);
    strcpy(granted.name, "custom");
    assert(vm.load(bad.c_str(), bad.size(), 2, stats, error, sizeof(error)));
    assert(vm.start(8, granted, error, sizeof(error)) && vm.poll(result) && !result.ok);
    assert(!vm.nextIo(request));
  }
  const char *networkSource = "function custom() return http.get('weather') end";
  assert(vm.load(networkSource, strlen(networkSource), 3, stats, error, sizeof(error)));
  for (unsigned n = 0; n < 4; ++n) {
    auto denied = granted;
    switch (n) {
    case 0: denied.homeAccess = false; break;
    case 1: denied.authenticated = false; break;
    case 2: strcpy(denied.channel, "#general"); break;
    case 3: denied.local = true; break;
    }
    assert(vm.start(10 + n, denied, error, sizeof(error)) &&
           vm.poll(result) && !result.ok && !vm.nextIo(request));
  }
  for (const char *helper : {"http", "json", "rpc"}) {
    validate("function custom() end " + std::string(helper) + "={}", false);
  }
  std::ifstream example("../runtime/examples/network_api.lua");
  assert(example);
  const std::string sourceExample((std::istreambuf_iterator<char>(example)),
                                  std::istreambuf_iterator<char>());
  const auto manifest = validate(sourceExample);
  assert(manifest.find("net_health") && manifest.find("net_status") &&
         manifest.find("net_echo") && manifest.find("net_sum"));
  puts("PASS network VM: named HTTP/RPC yield, home compatibility, structured responses, grants and invalid arguments");
}
#endif
#ifndef ONCHIP_BOT_VM_LIBRARY_TEST
int main(int argc, char **argv) {
  if (argc == 2 && !strcmp(argv[1], "--source-capacity-test")) {
    namedSourceCapacity();
    return 0;
  }
  namedSourceCapacity();
  namedSourcesAndRepeaterVm();
  commandOverrides();
  commandDiscoveryAndMeshGrants();
  boundedJson();
#if !ONCHIP_BOT_COMPACT_PROFILE
  networkVm();
#endif
  subscriptionVm();
  packageModules();
  diagnosticCommands();
  channelBoard();
  utilityCommands();
  personalNotesApi();
  personalReminderApi();
  packetContinuations();
  scopedStorage();
  namedThreadScopes();
  retained();
  manifestLifetimes();
  initializationBudgets();
  {
    BotSession vm;
    BotVmStats stats;
    char error[128]{};
    const char *source =
        "function identify() local auth=ctx.sender.authenticated local view=ctx.sender sleep(1) "
        "for i=1,100 do if ctx.sender~=view then return 'REBUILT VIEW' end end "
        "if auth~=ctx.sender.authenticated then return 'WRONG AUTH' end "
        "if ctx.channel.present then "
        "if ctx.sender.public_key then return 'WRONG KEY' end return ctx.channel.name end "
        "return ctx.sender.public_key end";
    assert(vm.load(source, strlen(source), 1, stats, error, sizeof(error)));
    auto dm = event("!identify"), channel = dm;
    channel.authenticated = false;
    memset(channel.sender, 0, sizeof(channel.sender));
    strcpy(channel.channel, "#example1"); strcpy(channel.nickname, "owner");
    BotIoRequest a, b;
    assert(vm.start(1, dm, error, sizeof(error)) && vm.nextIo(a));
    assert(vm.start(2, channel, error, sizeof(error)) && vm.nextIo(b));
    BotIoResult done{};
    done.ok = true; done.token = b.token;
    BotSession::Result result;
    assert(vm.complete(done) && vm.poll(result) && result.ok && result.job == 2);
    assert(!strcmp(result.action.text, "#example1"));
    done.token = a.token;
    assert(vm.complete(done) && vm.poll(result) && result.ok && result.job == 1);
    assert(strlen(result.action.text) == 64 && !strncmp(result.action.text, "01", 2));
  }
  static_assert(BotReplyLimit == 162 && BotPathLimit == MAX_PATH_SIZE,
                "Bot capacities must match the pinned native message/path limits");
  registry();
  std::ifstream file("tests/bot_large.lua");
  assert(file);
  const std::string large((std::istreambuf_iterator<char>(file)), {});
  assert(large.size() >= 3500 && large.size() <= BotSourceLimit);
  assert(validate(large).count == BotCommandLimit);
  assert(!strcmp(run(large, event("!station 32")).text,
                 "Fixture East 32 [AD07ah] elevation=432"));
  assert(!strcmp(run(large, event("!hello slepp")).text, "Hello slepp"));
  assert(!strcmp(run(large, event("!difference 1 32")).text, "Fixture elevation gap: 331"));
  assert(!strcmp(run(large + std::string(BotSourceLimit - large.size(), ' '),
                     event("!product -100 100")).text, "-10000"));
  {
    const auto full = large + std::string(BotSourceLimit - large.size(), ' ');
    BotVmStats phases;
    BotAction action;
    BotVmLimits limits;
    char error[128]{};
    fakeStep = 5;
    assert(BotVm::invoke(full.data(), full.size(), event("!hello slepp"), action,
                         phases, error, sizeof(error)));
    assert(phases.loadUs > 20000 && phases.loadUs < limits.loadWallUs &&
           phases.initUs < limits.initWallUs && phases.invokeUs < limits.wallUs &&
           phases.elapsedUs == phases.loadUs + phases.initUs + phases.invokeUs + phases.cleanupUs);
    limits.loadWallUs = 20000;
    assert(!BotVm::invoke(full.data(), full.size(), event("!hello slepp"), action,
                          phases, error, sizeof(error), limits));
    assert(strstr(error, "during load") && action.kind == BotAction::None);
    limits = {}; limits.initWallUs = 1;
    const auto small = program("reply('ok')");
    assert(!BotVm::invoke(small.data(), small.size(), event("!custom"), action,
                          phases, error, sizeof(error), limits));
    assert(strstr(error, "initialization") && action.kind == BotAction::None);
    limits = {};
    const auto busy = program("local n=0 for i=1,4000 do n=n+i end reply(tostring(n))");
    assert(!BotVm::invoke(busy.data(), busy.size(), event("!custom"), action,
                          phases, error, sizeof(error), limits));
    assert(strstr(error, "invocation") && phases.instructions < limits.instructions &&
           action.kind == BotAction::None);
    fakeStep = 0;
  }
  {
    BotSession retained;
    BotVmStats phases;
    BotVmLimits limits;
    char error[128]{};
    fakeTime = 1000; fakeStep = 20;
    limits.loadWallUs = 100000;
    assert(!retained.load(BotDefaultSource, strlen(BotDefaultSource), 1, phases, error, sizeof(error), limits));
    assert(strstr(error, "Load deadline ") && strstr(error, "/100000us peak="));
    assert(phases.loadUs >= limits.loadWallUs && phases.peakBytes < limits.heapBytes);
    limits = {};
    assert(limits.loadWallUs == 330000 && limits.initWallUs == 50000 &&
           limits.wallUs == 20000 && limits.heapBytes == 96 * 1024 &&
           limits.instructions == 10000 && limits.parserSteps == 16384);
    const auto full = large + std::string(BotSourceLimit - large.size(), ' ');
    for (const std::string &source : {std::string(BotDefaultSource), full}) {
      fakeTime = 1000;
      const bool loaded = retained.load(source.data(), source.size(), 1, phases, error, sizeof(error), limits);
      if (!loaded) fprintf(stderr, "slow full-suite load (%zu editable bytes): %s; load=%llu init=%llu parser=%u\n",
                           source.size(), error, static_cast<unsigned long long>(phases.loadUs),
                           static_cast<unsigned long long>(phases.initUs), phases.parserSteps);
      assert(loaded);
      assert(phases.loadUs > 100000 && phases.loadUs < limits.loadWallUs &&
             phases.initUs < limits.initWallUs && phases.peakBytes < limits.heapBytes &&
             phases.parserSteps <= limits.parserSteps);
      printf("PASS slow retained load: editable=%u load=%llu init=%llu us peak=%uB parser=%u\n",
             unsigned(source.size()), static_cast<unsigned long long>(phases.loadUs),
             static_cast<unsigned long long>(phases.initUs), unsigned(phases.peakBytes), phases.parserSteps);
      BotSession::Result result;
      assert(retained.start(1, event("!ping"), error, sizeof(error)));
      assert(retained.poll(result) && result.ok && !strcmp(result.action.text, "Pong"));
      assert(retained.start(2, event("!calc 2+3"), error, sizeof(error)));
      assert(retained.poll(result) && result.ok && !strcmp(result.action.text, "= 5"));
      assert(retained.start(3, event("!remember task groceries"), error, sizeof(error)));
      BotIoRequest request;
      assert(retained.nextIo(request) && request.kind == BotIoRequest::Put &&
             !strcmp(request.key, "task") && !strcmp(request.value, "groceries"));
      BotIoResult saved; saved.token = request.token; saved.ok = true;
      assert(retained.complete(saved));
      assert(retained.poll(result) && result.ok && !strcmp(result.action.text, "Note task created; committed"));
      if (source.size() == BotSourceLimit) {
        assert(retained.start(4, event("!hello slepp"), error, sizeof(error)));
        assert(retained.nextIo(request) && request.kind == BotIoRequest::Send &&
               request.reply && !strcmp(request.value, "Hello slepp"));
        saved = {}; saved.token = request.token; saved.ok = saved.queued = saved.transmitted = true;
        assert(retained.complete(saved) && retained.poll(result) && result.ok &&
               result.action.kind == BotAction::None);
      }
    }
    fakeTime = 1000; fakeStep = 1; limits.initWallUs = 1;
    assert(!retained.load(BotDefaultSource, strlen(BotDefaultSource), 1, phases, error, sizeof(error), limits));
    assert(strstr(error, "Init deadline ") && strstr(error, "/1us peak="));
    fakeStep = 0;
    limits = {}; limits.heapBytes = 16000;
    assert(!retained.load(BotDefaultSource, strlen(BotDefaultSource), 1, phases, error, sizeof(error), limits));
    assert(strstr(error, "Lua heap limit 16000B peak="));
    assert(phases.peakBytes <= limits.heapBytes);
    limits = {};
    fakeTime = 1000; fakeStep = 40;
    assert(retained.load(BotDefaultSource, strlen(BotDefaultSource), 1, phases, error, sizeof(error), limits));
    assert(phases.loadUs >= limits.loadWallUs && phases.loadUs < BotBundledLoadWallUs &&
           phases.initUs < limits.initWallUs && phases.peakBytes < limits.heapBytes);
    assert(BotVm::validate(BotDefaultSource, strlen(BotDefaultSource), phases, error, sizeof(error), limits));
    assert(phases.loadUs >= limits.loadWallUs && phases.loadUs < BotBundledLoadWallUs);
    const std::string modifiedBundle = std::string(BotDefaultSource) + " ";
    assert(!retained.load(modifiedBundle.data(), modifiedBundle.size(), 1, phases, error, sizeof(error), limits));
    assert(strstr(error, "Load deadline ") && strstr(error, "/330000us peak="));
    assert(phases.loadUs >= limits.loadWallUs);
    fakeTime = 1000; fakeStep = 130;
    assert(!retained.load(BotDefaultSource, strlen(BotDefaultSource), 1, phases, error, sizeof(error), limits));
    assert(strstr(error, "Load deadline ") && strstr(error, "/1000000us peak="));
    assert(phases.loadUs >= BotBundledLoadWallUs);
    fakeStep = 0;
    puts("PASS exact bundled recovery load headroom: bounded 1s, edited source stays 330ms, explicit limits and Init/active/heap unchanged");
    puts("PASS retained bundled source: bounded full-suite/4KiB loading, separate Init/active limits, unchanged heap bounds and truthful failures");
  }
  assert(!strcmp(run(BotDefaultSource, event("!ping")).text, "Pong"));
  assert(!strcmp(run(program(
      "local i=9223372036854775807 "
      "if i+1~=-9223372036854775807-1 or 9007199254740992.0+1.0~=9007199254740992.0 "
      "then reply('numeric mismatch') else reply(tostring(i)) end"), event("!custom")).text,
      "9223372036854775807"));
  assert(!strcmp(run(BotDefaultSource, event("!path")).text, "1:a1b2"));
  assert(!strcmp(run(BotDefaultSource, event("!test")).text,
                 "Connected; path 1:a1b2; RSSI=-90 dBm SNR=5 dB"));
  auto unmeasured = event("!test");
  unmeasured.signal = false;
  assert(!strstr(run(BotDefaultSource, unmeasured).text, "local;"));
  unmeasured.local = true;
  assert(strstr(run(BotDefaultSource, unmeasured).text, "local; RSSI/SNR unavailable"));
  auto mt = event("!mt 1"); mt.observationCount = 8; mt.truncated = true;
  for (auto &path : mt.observations) path.known = true;
  assert(strstr(run(BotDefaultSource, mt).text, "8 unique paths"));
  assert(strstr(run(BotDefaultSource, mt).text, "truncated"));
  auto routes = event("!mt 5"); routes.observationCount = 2;
  for (unsigned i = 0; i < 2; ++i) {
    auto &path = routes.observations[i];
    path.known = true; path.width = 3; path.count = 2;
    for (unsigned j = 0; j < path.size(); ++j) path.bytes[j] = uint8_t(0xa1 + 16 * i + j);
  }
  assert(!strcmp(run(BotDefaultSource, routes).text,
                 "2 unique paths in 5000 ms; a1a2a3,a4a5a6 | b1b2b3,b4b5b6"));
  routes.observations[0].width = 1; routes.observations[0].count = 6;
  routes.observations[1].width = 2; routes.observations[1].count = 3;
  assert(!strcmp(run(BotDefaultSource, routes).text,
                 "2 unique paths in 5000 ms; a1,a2,a3,a4,a5,a6 | b1b2,b3b4,b5b6"));
  for (uint8_t width : {1, 2, 3}) {
    for (auto &path : routes.observations) {
      path.width = width; path.count = uint8_t(std::min<size_t>(63, BotPathLimit / width));
      memset(path.bytes + 6, 0xcd, sizeof(path.bytes) - 6);
    }
    for (uint16_t limit : {80, 116, int(BotReplyLimit)}) {
      routes.replyLimit = limit;
      const std::string text = run(BotDefaultSource, routes).text;
      const auto start = text.find("; ") + 2, split = text.find(" | "), end = text.rfind("; truncated");
      assert(text.size() <= limit && split != std::string::npos && end != std::string::npos);
      for (const auto &part : {text.substr(start, split - start), text.substr(split + 3, end - split - 3)}) {
        assert(part.substr(part.size() - 3) == "...");
        const auto hashes = part.substr(0, part.size() - 3);
        size_t offset = 0;
        do {
          const auto comma = hashes.find(',', offset);
          const auto end = comma == std::string::npos ? hashes.size() : comma;
          assert(end - offset == 2 * width);
          offset = end + 1;
        } while (offset < hashes.size());
      }
    }
  }
  routes.replyLimit = BotReplyLimit; routes.observationCount = 1;
  routes.observations[0].count = 0;
  assert(strstr(run(BotDefaultSource, routes).text, "no repeaters"));
  routes.observationCount = 0; routes.path.known = false;
  assert(strstr(run(BotDefaultSource, routes).text, "direct/local path unknown"));
  routes.observationCount = BotObservationLimit; routes.replyLimit = 116;
  for (unsigned i = 0; i < BotObservationLimit; ++i) {
    auto &path = routes.observations[i];
    path.known = true; path.width = 3; path.count = 21;
    memset(path.bytes, int(0xa0 + i), sizeof(path.bytes));
  }
  const std::string crowded = run(BotDefaultSource, routes).text;
  assert(crowded.size() <= routes.replyLimit && crowded.find("8 unique paths") == 0 &&
         crowded.find("a0a0a0") != std::string::npos &&
         crowded.find(" | a1a1a1") != std::string::npos &&
         crowded.find("; truncated") != std::string::npos);
  puts("PASS multitrace collector output: two actual width-three paths, whole-segment truncation at reply bounds and direct/no-hop distinction");
  assert(strstr(run(BotDefaultSource, event("!trace")).text, "Error: TRACE"));
  assert(run(BotDefaultSource, event("!trace 2:0102aabb")).kind == BotAction::Trace);
  const std::string wideTrace = "!trace 8:" + std::string(144, 'a');
  assert(event(wideTrace.c_str()).routeSize == 72);
  assert(run(BotDefaultSource, event(wideTrace.c_str())).kind == BotAction::Trace);
  run(BotDefaultSource, event("!other hello"), false);
  auto wide = event("!path");
  wide.path.width = 3; wide.path.count = 21;
  assert(strlen(run(BotDefaultSource, wide).text) == 128);
  strcpy(wide.name, "test");
  assert(strstr(run(BotDefaultSource, wide).text, "path too long"));
  auto direct = event("!path"); direct.path.known = false;
  assert(strstr(run(BotDefaultSource, direct).text, "no complete heard path"));
  auto invalid = mt; invalid.observationCount = 9;
  run(BotDefaultSource, invalid, false);
  invalid = event("!mt"); invalid.windowMs = 30001;
  run(BotDefaultSource, invalid, false);
  for (const char *body : {
           "while true do end", "ctx.name='oops'", "ctx.sender.public_key='oops'",
           "ctx.limits.reply_bytes=10000", "io.open('secret')", "require('os')",
           "load('return 1')", "os.time()", "ctx.observation.paths[1]='oops'",
           "request_trace()", "reply('\\0')", "reply('first') reply('second')",
           "command('later','','no')", "local t={} for i=1,10000 do t[i]={i,i,i,i} end"}) {
    run(program(body), event("!custom"), false);
  }
  run("while true do end", event("!custom"), false);
  assert(!strcmp(run(program("if ctx.name or ctx.arguments then reply('legacy') else reply('safe') end"),
                     event("!custom")).text, "safe"));
  for (const size_t length : {BotReplyLimit, BotReplyLimit + 1}) {
    run(program("reply('" + std::string(length, 'a') + "')"), event("!custom"), length == BotReplyLimit);
  }
  const auto maximumCommand = "!x " + std::string(BotArgumentsLimit, 'a');
  assert(maximumCommand.size() == BotTextLimit);
  assert(strlen(event(maximumCommand.c_str()).arguments) == BotArgumentsLimit);
  assert(!strcmp(run(program("reply(tostring(ctx.limits.reply_bytes))"),
                     event("!custom")).text, "162"));
  BotVmLimits limits;
  BotVmStats measured;
  BotAction measuredAction;
  char measuredError[128]{};
  const auto ping = event("!ping");
  assert(BotVm::invoke(BotDefaultSource, strlen(BotDefaultSource), ping,
                       measuredAction, measured, measuredError, sizeof(measuredError)));
  assert(measured.elapsedUs == measured.loadUs + measured.initUs + measured.invokeUs + measured.cleanupUs);
  printf("%s host: integer=%zu bytes, number=%zu bytes; "
         "ping peak=%zu bytes, instructions=%u, parser=%u, elapsed=%llu us\n",
         LUA_RELEASE, sizeof(lua_Integer), sizeof(lua_Number), measured.peakBytes,
         measured.instructions, measured.parserSteps, static_cast<unsigned long long>(measured.elapsedUs));
  limits.instructions = measured.instructions;
  assert(run(BotDefaultSource, ping, true, limits).kind == BotAction::Reply);
  --limits.instructions; run(BotDefaultSource, ping, false, limits);
  limits = {}; limits.parserSteps = measured.parserSteps;
  assert(run(BotDefaultSource, ping, true, limits).kind == BotAction::Reply);
  --limits.parserSteps; run(BotDefaultSource, ping, false, limits);
  limits = {}; limits.instructions = 1; run(BotDefaultSource, ping, false, limits);
  limits = {}; limits.parserSteps = 10; run(BotDefaultSource, ping, false, limits);
  limits = {}; limits.heapBytes = 1; run(BotDefaultSource, ping, false, limits);
  limits = {}; limits.wallUs = 0; run(BotDefaultSource, ping, false, limits);
  limits = {}; limits.loadWallUs = 0; run(BotDefaultSource, ping, false, limits);
  run("\x1bLua", event("!custom"), false);
  run(std::string(BotSourceLimit + 1, ' '), event("!custom"), false);
  const auto nested = "return " + std::string(100, '(') +
                      "function() end" + std::string(100, ')');
  assert(!BotVm::invoke(nested.c_str(), nested.size(), event("!custom"), measuredAction,
                       measured, measuredError, sizeof(measuredError)));
  assert(strstr(measuredError, "C stack overflow") && measuredAction.kind == BotAction::None);
  std::string exactSource = program("reply('ok')");
  exactSource.resize(BotSourceLimit, ' ');
  assert(!strcmp(run(exactSource, event("!custom")).text, "ok"));
  char error[128];
  BotEvent tooLong;
  assert(!parseBotCommand((maximumCommand + "a").c_str(), BotTextLimit + 1,
                          tooLong, error, sizeof(error)));
  const auto tooManyTraceHops = "!trace 1:" + std::string(128, 'a');
  assert(!parseBotCommand(tooManyTraceHops.c_str(), tooManyTraceHops.size(),
                          tooLong, error, sizeof(error)));
  const char *traceWidthThree = "!trace 3:00";
  assert(!parseBotCommand(traceWidthThree, strlen(traceWidthThree),
                          tooLong, error, sizeof(error)));
  assert(strstr(error, "width 1,2,4,8"));
  for (const char *command : {"!", "!bad.name", "!mt 0", "!mt 31",
                              "!trace 2:00", "!trace 1:zz", "!ping\n"}) {
    BotEvent value;
    assert(!parseBotCommand(command, strlen(command), value, error, sizeof(error)));
  }
  puts("bot VM: native registry, typed arguments, protected diagnostics, read-only ctx and budgets passed");
}
#endif
