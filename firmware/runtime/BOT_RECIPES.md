# Build useful Lua and Wasm commands

Start with a command you can run locally, then install it through the node's
authenticated Management connection. The [web tutorials](https://ve6slp.ca/projects/meshcore/extend/tutorials/)
show sample bot and Management conversations. These examples do not require a
radio until you choose to install them.

Lua runs on Aspen, Pine and the native Linux worker used by Birch and Willow.
Wasm requires the ESP32-S3 with PSRAM or a Wasm-enabled native Linux worker;
Pine does not run Wasm. Read `source api runtimes` on your node rather than
assuming a feature is present in an older image.

## Add a greeting, without replacing other files

Save this as `hello.lua`:

```lua
function hello(name)
  return "Hello " .. name
end
command("hello", "name:string:32", "Greet a name",
        "hello", "public", "!hello mesh")
```

Run the supplied identical template and its success/error scenarios from the
repository root:

```sh
make -s -C firmware/esp32 bot-local \
  SOURCE=firmware/runtime/plugins/template.lua \
  SCENARIO=firmware/runtime/plugins/examples/hello.scenario.json
```

Expected: `PASS 4 native replay assertions`. The runner uses native encrypted
packets and an in-memory radio; it never transmits. The
[development guide](BOT_DEVELOPMENT.md) lists build dependencies, schemas and
replay directives.

On Aspen, check `source status` through authenticated Management before
installing. Export your existing files to private storage. Prefer a named source
operation for a single command file:

```sh
python3 tools/hardware/admin.py \
  --web http://NODE --password-file /private/node-password \
  source-install greeting hello.lua
python3 tools/hardware/admin.py \
  --web http://NODE --password-file /private/node-password \
  status
```

For the same named installation over LoRa, follow the
[RF upload walkthrough](../../tools/hardware/RF_ADMIN.md#add-one-lua-file-through-a-kiss-gateway).
It includes companion-console login, a separate KISS upload connection and
durable chunk/activation replies. A bot DM is not a bulk upload endpoint.

`NODE` and the password-file path are placeholders. This updates only
`greeting`, preserving other named source files. The complete Lua set must still
fit its aggregate source and command limits; duplicate exports reject it.
Whole-set `package-install` or `source replace` replaces the selected runtime's
entire set instead. Native hosts can use their owner socket; Pine can use its
authenticated RF source transport. See
[named source operations](BOT_DEVELOPMENT.md#update-one-installed-lua-file).

Wait for status to show the new selection active before testing. In a private
DM to the bot, send `!hello mesh` and expect `Hello mesh`. Send `!hello` to check
the missing-argument error and `!help hello` for the generated help.

Installation does not flash firmware or change role identities. A durable
selection returns after restart. Source replacement cancels pending work in
that runtime, not durable notes; restoring old code does not undo writes or
packets it already sent. Read status after a failed or uncertain operation before
deciding to retry. Keep native Management available for recovery.

## Keep private notes and support code rollback

The complete [notes-v2.lua](plugins/examples/notes-v2.lua) example gives each
authenticated DM caller their own note. A channel nickname is not a private
identity. The handlers are declared with `"dm"` permission; native admission
checks that context.

It migrates version-1 data with one scoped transaction, rejects unfamiliar
schemas, and writes both the old and new note fields. This lets version-1 code
continue reading updated data after a code rollback. It does not silently
discard an unrecognised record.

The write pattern is:

```lua
local result = kv.transaction({
  {key="example:schema", expect="2", value="2"},
  {key="example:note-v1", value=text},
  {key="example:note-v2", value=text},
})
if not result.ok then
  return "Note write " .. result.status .. "; read before retrying"
end
return "Saved"
```

Use the whole example, including its migration function, not this fragment
alone. A `conflict` changes nothing. An `unknown` outcome requires reading the
affected keys before another explicit mutation; it is not a successful write.
Check `rf_safe` from `kv.get` before returning arbitrary stored bytes. This
example's values enter through bounded printable command arguments.

```sh
make -s -C firmware/esp32 bot-local \
  SOURCE=firmware/runtime/plugins/examples/notes-v2.lua \
  SCENARIO=firmware/runtime/plugins/examples/notes.scenario.json
```

Expected: eight assertions, including two private callers and code-version
changes. Alice can save `lunch`; Bob initially reads `No note`. Restoring the
earlier code keeps each caller's stored value. See
[scoped storage and transactions](BOT_RUNTIME.md#scoped-durable-state).

## Wait for a timer without blocking the bot

The [lab.lua](plugins/examples/lab.lua) example declares
`!alarm NAME SECONDS`. It sets a durable timer, waits cooperatively and returns
its state:

```lua
function alarm(name, seconds)
  timer.set(name, seconds)
  local state = timer.wait(name)
  return "Timer " .. name .. " " .. state.state
end
command("alarm", "name:string:32,seconds:int:1:60",
        "Wait on a durable timer", "alarm", "dm", "!alarm tea 2")
```

Timers need trusted time. Waiting releases the worker for other jobs; it does
not give the node unlimited concurrent jobs or airtime. Restart retains the
timer but loses the old waiting coroutine. Use the example's `!timer-state tea`
after reconnecting. For a message that should be delivered after restart, use
the reminder API with its owner grant, fresh trusted time and direct route.

The local `@clock`, `@send`, `@advance` and `@restart` directives let you test
these cases without waiting in real time. See
[async replay](BOT_DEVELOPMENT.md#authenticated-users-channels-time-and-async-work)
and [timers/reminders](BOT_RUNTIME.md#sleep-deadlines-and-reminders).

## Answer an addressed channel command

A public command declaration does not enable reception on every channel.
The operator must configure membership, execution/reply policy and any storage
grant. Read all memberships and choose an unused slot; do not overwrite an
existing channel just to run a tutorial.

For a new addressed-only membership on Aspen:

```text
bot off
apply
```

Reconnect to native Management and verify `bot status` says disabled. Then,
using the unused slot `2` in this example:

```text
bot membership 2 #lab
bot access 2 default 12
bot aliases aspen,aspen-bot,a
bot membership 2
bot access 2 list 0
bot aliases
bot on
apply
```

Joining resets the membership default to 63 and clears overrides. Disabling
and restarting first prevents a temporarily permissive bot on the new channel.
The final restart enables the saved selection. Alias changes replace the list;
retain any existing aliases you want. These commands are node-wide Management
operations, not ordinary posts to `#lab`.

In `#lab`, `!@aspen-bot hello mesh` returns `Hello mesh`.
The companion mention `!@[Aspen-Bot] hello mesh` uses the same saved alias.
With default mask 12, unaddressed commands stay silent.
Channel keys and nicknames do not grant owner authority or private DM storage.
For an already configured addressed-only membership, no restart or rejoin is
needed to try the command.

See [channel targeting and policy](BOT_RUNTIME.md) and
[command endpoints](../../COMMANDS.md).

## Compile C or Rust to Wasm

Use the retained SDK rather than WASI or a shell. Follow the
[toolchain setup](WASM_RUNTIME.md#build-and-install-a-package), then:

```sh
make -C firmware/esp32 bot-wasm-examples
python3 tools/hardware/admin.py package create \
  .tmp/wasm-examples/c-notes.wasm .tmp/wasm-notes.package \
  --name wasm-notes --version 1.0.0 --capability cmdmeta --capability kv
python3 tools/hardware/admin.py package inspect .tmp/wasm-notes.package
```

The [C notes example](wasm/examples/notes.c) declares `!wnote [TEXT]`. `mc_start`
submits `MC_PUT` or `MC_GET` in caller scope and returns `MC_PENDING`.
`mc_resume` reads the native operation outcome and sends the reply; storage
failure follows the error branch, not the success branch. Guest code never
receives private identity keys or service credentials.

Check the node's runtimes, then install:

```sh
python3 tools/hardware/admin.py \
  --web http://NODE --password-file /private/node-password \
  command 'source api runtimes'
python3 tools/hardware/admin.py \
  --web http://NODE --password-file /private/node-password \
  package-install .tmp/wasm-notes.package
python3 tools/hardware/admin.py \
  --web http://NODE --password-file /private/node-password \
  --runtime wasm status
```

Package installation detects Wasm from the package. Lifecycle operations default
to Lua; `--runtime wasm` selects Wasm for status, download, rollback and removal.
Installing Wasm replaces only the Wasm command set and cancels its pending work;
Lua keeps running. An authenticated bot DM `!wnote lunch` replies `Note saved`;
`!wnote` then replies `lunch`.

The supplied C/Rust arithmetic and configured-RPC examples are under
[`wasm/examples`](wasm/examples). RPC needs an operator-configured endpoint,
trusted time and the home grant; a guest cannot supply an arbitrary URL.
Read the [SDK and runtime limits](WASM_RUNTIME.md) before adding memory or I/O.
