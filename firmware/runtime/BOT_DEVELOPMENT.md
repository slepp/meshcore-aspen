# Write, run locally and install a Lua command

Run your command on your computer before uploading it to a MeshCore radio.
`bot-local` builds and selects the project's native runner directly: the retained
Lua 5.5.1 VM, `BotWorker`, command registry, verified storage, async jobs and
encrypted MeshCore packets all participate. It uses an in-memory radio,
NVS/filesystem and a socket-free HTTPS fixture. It never connects to a radio or
remote service.

The first build fetches pinned Lua/MeshCore and the existing native-test
dependencies. Use the project's normal C/C++/Python build environment.
The network-enabled runner also needs cJSON headers and its runtime library;
the default header location is the ESP32 SDK. On a Linux development machine
using distribution cJSON, pass `BOT_JSON_INCLUDE=/usr/include/cjson` and
`BOT_JSON_LIBS=-lcjson` to the make command after installing its development
package. Replay itself needs no network connection.

From the repository root:

```sh
printf '!hello mesh\n' | make -s -C firmware/esp32 bot-local \
  SOURCE=firmware/runtime/plugins/template.lua
```

After the first build, the final JSONL reply contains `"text":"Hello mesh"`.
The named template is a complete command:

```lua
function hello(name)
  return "Hello " .. name
end
command("hello", "name:string:32", "Greet a name", "hello", "public", "!hello mesh")
```

An ordinary Lua return supplies one reply. Declare arguments rather than
splitting the incoming packet yourself. `string:N` accepts one token (or a
quoted token); `text:N` consumes the remaining text; `int:MIN:MAX` and `bool`
validate typed values; a `?` suffix on an argument name makes it optional.
Optional arguments follow required arguments, and a text argument must be last.
Missing, extra, malformed and out-of-range arguments name the affected command
and direct the caller to `!help NAME`. Inputs and RF replies are printable ASCII.
Keep a reply within `ctx.limits.reply_bytes`; do not truncate private data
silently. Commands have no ordinary cooldown; queue/airtime/concurrency limits
and the separate HTTPS request limits still apply.

## Replace or wrap a builtin without rebuilding firmware

Install one operator-controlled Lua source generation to patch a builtin
command or choose regional behavior. On firmware exposing these helpers,
no application flash, identity change or data erase is needed:

```lua
function regional_ping()
  return call_original("ping") .. " from the regional bot"
end
override_command("ping", "regional_ping")
```

Try the complete [override example](plugins/examples/overrides.lua):

```sh
printf '!ping\n' | make -s -C firmware/esp32 bot-local \
  SOURCE=firmware/runtime/plugins/examples/overrides.lua
```

Expect `Pong from the regional bot`. Package and install that source through
the same operator source workflow as any other command set:

```sh
python3 tools/hardware/admin.py package create \
  firmware/runtime/plugins/examples/overrides.lua .tmp/regional.bot.lua \
  --name regional --version 1.0.0 --capability cmdmeta
make -C firmware/esp32 bot-plugin-install PACKAGE=.tmp/regional.bot.lua \
  MAST_CLI_ARGS='--web http://MAST --password-file /private/mast-password'
```

Use your platform's existing authenticated source transport on Pine or the
native host. Include all custom exports and overrides you want to retain:
installation replaces the entire source generation. Durable installations
restore the override on restart; RAM-only activation ends on restart.
`source rollback` restores the previous generation; `source remove` restores
bundled dispatch. Inspect `source status` after either asynchronous operation.
Neither operation removes the bot identity, notes or other durable scoped data.

`override_command(name, export)` keeps the builtin's argument schema,
help/example and authorization. The export receives the normal parsed,
typed positional arguments. It must be a non-native global Lua function;
it consumes one of the eight command slots. Unknown originals, duplicate
names and missing exports reject installation. Directly redefining `ping`
or using `command("ping",...)` still rejects installation.

`call_original(name[, argument_text])` calls the firmware's actual bundled
handler, bypassing overrides. Omit the text to reuse this invocation's raw
arguments, or supply bounded command argument text using normal quoting:
`call_original("calc", "(2+3)*4")` returns `= 20`.
It returns the handler's first result, and yields when that handler performs
cooperative I/O. It never sends an extra reply by itself.

There is no context parameter: the current native invocation supplies the
caller, bot identity, reply route and grants. Calling another original checks
that original's policy too; a public wrapper cannot use `admin`, private
notes, shared state or home services without their authority. Saved or forged
Lua context tables cannot change that authority. Helpers cannot be called
to register commands during a request, and `call_original` is unavailable
during initialization or event subscriptions. Pine retains its 48 KiB heap,
two-job profile and unsupported HTTPS commands.

An override failure reports the command error; it does not retry the builtin.
If a native operation was already admitted, it may have committed or transmitted
before cancellation, failure or replacement. Inspect its state before repeating
a mutation. Removing an override restores subsequent dispatch, not its effects.
Unchanged builtin names still use bundled dispatch; on ESP32 and the host they
retain the independent diagnostic heap. Explicitly overridden names run in
the active source heap, so keep native management available for recovery.

## Assert replies, including failures

Run the template's success, missing argument, quoted argument and extra argument
checks:

```sh
make -s -C firmware/esp32 bot-local \
  SOURCE=firmware/runtime/plugins/template.lua \
  SCENARIO=firmware/runtime/plugins/examples/hello.scenario.json
```

Expected result: `PASS 4 native replay assertions` on stderr, native JSONL
records on stdout, and exit status zero. Change the expected `"Hello mesh"`
to another string: the same command exits nonzero and shows the mismatching
record.

A scenario is just replay text plus assertions against the existing JSONL
protocol:

```json
{
  "replay": ["!hello mesh", "!hello"],
  "expect": [
    {"type": "reply", "text": "Hello mesh"},
    {"type": "reply", "text_contains": "!hello: name: required argument missing"}
  ]
}
```

Assertions match records in order and compare the fields you specify.
`text_contains` checks a substring; other fields use exact equality.
Every `reply`, `async_reply`, `pending`, `reject` and `error` must be asserted,
as must any record with `ok:false`. Other record types are included when
listed in `expect`; source/configuration chatter otherwise stays on stdout
without increasing assertion count. Unexpected records fail the run.
Exit codes: **0** completed assertions (or an unasserted replay), **1** failed
assertions/native setup or validation, **2** invalid local CLI/scenario, unreadable
scenario/replay, malformed JSONL or the 120-second process timeout.
These are the `bot_local.py` exit codes; GNU make returns a nonzero recipe-failure
status (normally 2) when that helper fails.
An unasserted replay intentionally allows negative command replies;
use a scenario in automation.

For your own text replay, use `REPLAY=path` or pipe lines on stdin. Paths passed
to `bot-local`, and paths inside its replay, are relative to the repository root.
Omit `SOURCE` to exercise the shipped commands. The make target selects the
binary for the current build flags; never select a build with a shell glob.

## Authenticated users, channels, time and async work

Each `@context dm NAME` selects a different local peer with a full native public
key. The runner learns its signed advert, encrypts its request, and decrypts
the actual reply. The JSONL `context` record reports that key.
`@channel #NAME` configures the native bot radio's selected channel; it restarts
the fixture and restores its source from the native deployment journal. Channel requests require
`@context channel NAME NICKNAME`; they use the channel's real key and explicitly
target the bot. Nicknames do **not** create individual or owner authority.

| Replay line | What it changes or checks |
| --- | --- |
| `@context dm alice` | Select/create an authenticated DM caller; default peer is `default`. |
| `@owner alice` | Commit the selected peer's full public key through native MastAdmin `trust`. Does not select that caller or bypass request admission. `@owner none` revokes it. |
| `@channel #lab-one` | Select a key-derived channel, recreate the fixture, restore the journal-selected source. In-flight work is cancelled. |
| `@context channel alice Alice` | Select targeted verified channel traffic; nickname is descriptive. |
| `@grant shared on` | Apply the native default-off channel KV grant. `off` revokes it. |
| `@grant reminders on` | Apply the native personal reminder grant. |
| `@grant home on` | Apply the home HTTPS grant; requires configured endpoints and trusted time. |
| `@grant channel-wait on` | Apply verified channel waits; remains default-off. |
| `@grant events on` | Enable all four native event subscriptions; `off` disables them. |
| `@route 1:aabb` | Receive the selected peer's authenticated direct path-return packet. Native route widths: 1, 2, 3 bytes per hop; hex bytes must contain whole hops. `@route 1:` learns a zero-hop direct route. |
| `@clock 1767225600` | Offer native network-time authority; inspect `accepted`/`trusted`. A rejected sample does not become trusted. |
| `@advance 5000` | Advance simulated monotonic time by milliseconds, then service 80 native steps and collect async replies. |
| `@send !alarm tea 2` | Submit a command without treating no immediate reply as failure. |
| `@pump 80` | Service 1..10,000 native steps (2 simulated milliseconds each); collect async replies. |
| `@message dm bob answer` | Receive an ordinary encrypted DM from a named local peer. |
| `@message channel bob ready` | Receive ordinary verified channel text; can resume `mesh.wait`. |
| `@reflect !owner-test` | Transmit the selected peer's encrypted request through another local modem source, rather than receive it over RF. Collect the native reply, if any. Local reflection cannot authorize an owner command. |
| `@cancel` | Cancel active native jobs, service completion, collect replies. Does not erase durable timers. |
| `@fault storage`, `@fault commit`, `@fault off` | Inject NVS read/commit failures or clear them. `commit` can produce an unknown mutation outcome. |
| `@duplicate` | Redeliver the next exact encrypted command packet; native dedup prevents a second invocation. |
| `@install PATH` | Run native MastSource begin/chunk/commit, verified copy, validation, durable journal commit and activation. A rejected candidate retains the previous durable source; inspect status for partial or uncertain outcomes. |
| `@source status` / `@source hash` | Inspect the native deployment journal and selected source hash. |
| `@source cancel` | Cancel a failed or partial upload before installing different bytes. |
| `@source rollback` / `@source remove` / `@source retry` | Start the native previous-source/bundled-source/live-recovery transaction. Acceptance is not activation: use `@pump 800` and inspect `@source status` before relying on it. |
| `@restart` | Recreate the native fixture with retained NVS/filesystem state; restore custom or bundled source through the real MastSource journal. No reinstall is needed. |
| `@status` | Inspect source generation and reply/duplicate/VM-failure counters. |

Time does not jump a minute between requests. Native initialization and each
service step still advance simulated time. Use explicit clock/advance/route
lines to test trusted-time and fresh-route requirements. A synchronous command
without a reply yet produces `pending`; use `@send` and then collect the
`async_reply` records for deliberately delayed work. Async replies identify a
DM peer or a channel, not a guessed request ID.
The real bot airtime budget still limits a long burst of replies: a missing
reply can also be an admission/airtime denial, not a Lua waiter. Use `!air`
before a large batch, and `@advance 61000` between batches when testing command
behavior rather than budget exhaustion.

Runnable scenarios:

```sh
# Separate full-key private users, unknown commit and duplicate packet
make -s -C firmware/esp32 bot-local \
  SOURCE=firmware/runtime/plugins/examples/lab.lua \
  SCENARIO=firmware/runtime/plugins/examples/private.scenario.json
# New users and rollback-compatible note formats in two private scopes
make -s -C firmware/esp32 bot-local SOURCE=firmware/runtime/plugins/examples/notes-v2.lua \
  SCENARIO=firmware/runtime/plugins/examples/notes.scenario.json
# Native trusted owner, unrelated DM/channel/local-reflection denials, revocation
# and custom-package recovery after restart
make -s -C firmware/esp32 bot-local SOURCE=firmware/runtime/plugins/examples/owner.lua \
  SCENARIO=firmware/runtime/plugins/examples/owner.scenario.json
# Missing grant, nickname changes, different channel secrets and DM isolation
make -s -C firmware/esp32 bot-local \
  SCENARIO=firmware/runtime/plugins/examples/board.scenario.json
# Timer claim/cancel/restart retention and channel-wait grant
make -s -C firmware/esp32 bot-local \
  SOURCE=firmware/runtime/plugins/examples/lab.lua \
  SCENARIO=firmware/runtime/plugins/examples/timers.scenario.json
# Autonomous personal reminder, restart, fresh route and user isolation
make -s -C firmware/esp32 bot-local \
  SCENARIO=firmware/runtime/plugins/examples/reminders.scenario.json
# Configured native HTTP/JSON/RPC path, rejection, uncertain outcome and cancellation
make -s -C firmware/esp32 bot-local \
  SCENARIO=firmware/runtime/plugins/examples/network.scenario.json
# Weather fixture data, explicit-place checks and service echo
make -s -C firmware/esp32 bot-local \
  SCENARIO=firmware/runtime/plugins/examples/weather.scenario.json
# Named GET/POST, custom RPC, malformed replies and argument validation
make -s -C firmware/esp32 bot-local SOURCE=firmware/runtime/examples/network_api.lua \
  SCENARIO=firmware/runtime/plugins/examples/http.scenario.json
make -C firmware/esp32 bot-local-test
```

### Socket-free network outcomes

`@network setup` commits a test-only named `home` endpoint and its allowed RPC
operations through the native configuration API. It does **not** enable home
access. `@network setup-examples` also configures `home_status`, `home_echo`
and `home_rpc` for `examples/network_api.lua`. Then use `@grant home on`.
`@network reply JSON` provides a raw JSON
body in an HTTP 200 response with Content-Length; native HTTPS code performs
request encoding, authorization, response bounds and schema validation.
`@network reject` refuses a connection **before submission**;
`@network unknown` accepts request bytes and loses the response, so remote
outcome is unknown; `@network hold` delays reads until another network directive,
cancellation or deadline. The `network` record's `submissions` counts requests
whose bytes reached the fixture. Invalid JSON/schema is not converted to success.
HTTPS request limits still apply: 2/caller and 4/global per minute.

TLS handshakes and certificate validation are deliberately outside this
socket-free transport; run `bot-native-https-test` for the local TLS tests.
Do not infer RF delivery from a simulated reply or a queue acceptance.

### Owner authority and source recovery

The runner bootstraps real Management/MastAdmin services and binds their
`commandBotService()` to the same native bot that receives the encrypted
requests. `@owner alice` configures native trust; `@context dm alice` then sends
that full-key peer's encrypted request. Native admission supplies `ctx.owner`;
an unrelated DM, channel nickname or local reflection cannot supply it.
`@owner none` revokes trust, and a later `@owner bob` replaces the trusted key.
Trust and private data survive fixture restart.

The owner scenario also runs native `!admin bot shared on` and checks
`!admin bot shared`, exercising policy changes on that same bot. The native
admin transport/operation restrictions remain active: credentials and bulk
source/data are not admitted through Lua `!admin`.

For a packaged owner example:

```sh
python3 tools/hardware/admin.py package create \
  firmware/runtime/plugins/examples/owner.lua .tmp/owner.bot.lua \
  --name owner-lab --version 1.0.0 --capability cmdmeta --capability kv
make -s -C firmware/esp32 bot-local SOURCE=.tmp/owner.bot.lua \
  SCENARIO=firmware/runtime/plugins/examples/owner.scenario.json
```

`--source` and `@install` use the real MastSource upload/deployment transaction;
`--validate` only checks a source without deploying it. Fixture restart
recovers the journal-selected file, verifies it and activates it through the
retained worker. A syntax/validation failure keeps the previous active source;
cancel the incomplete upload before installing different bytes. Source
controls and `@owner` are explicit local operator setup, not RF messages that
claim owner authority.

### Local replay limits

NVS and filesystem state are retained in memory within one runner process.
`@restart` tests native journal recovery across fixture recreation, not an
OS-process restart, physical power loss or disk persistence. Private
KV/timer/reminder banks remain available. A pending timer stays durable, but
its old Lua waiter is gone; a reminder still needs fresh trusted time and a
direct route after restart. The local board rejects physical MCU reboot;
use `@restart` instead of `!admin reboot` or `apply`.
No live service, radio, credentials or RF endpoint is needed.

## APIs used by these commands

These are developer shortcuts; [BOT_RUNTIME.md](BOT_RUNTIME.md) is the canonical
reference for bounds, authorization, asynchronous ownership and wire behavior.
I/O functions yield the calling coroutine; they are not blocking host Lua mocks.
Do not use them at source initialization.
Only the documented sandbox functions are exposed, not every standard Lua
global or library. Use typed command arguments and the supplied APIs.

| API | Return/error semantics |
| --- | --- |
| `command(name, schema, help[, export[, permission[, example]]])` | Register at initialization. Up to eight custom exports; help/example <=64 ASCII bytes. Permission names are `public`, `dm`, `owner`, `channel`, `shared`, `reminder`, `home`. Declaration never grants access. |
| `return TEXT` / `reply(TEXT)` | One bounded printable reply to this invocation's authenticated route. A returned string is simplest; do not also call `reply` for that response. |
| `command_help(name[, page])` | Bounded permission-aware help text; `!help NAME PAGE` follows long help. Native commands cannot be replaced by a custom export. |
| `ctx.sender`, `ctx.channel`, `ctx.grants`, `ctx.packet`, `ctx.radio`, `ctx.limits` | Immutable invocation metadata. A channel has no authenticated individual full key; a local reflection is not RF reception. |
| `kv.get(key[, scope])` | `value, rf_safe`; absent is `nil`, and storage/authorization errors raise. Test `rf_safe` before returning arbitrary stored bytes over RF. |
| `kv.put(key, value[, scope])` | `true, "created"` or `true, "replaced"` after verified publication. Rejected/unknown mutations raise; read state before retrying an unknown outcome. |
| `kv.delete(key[, scope])` | `true, "deleted"` or `true, "absent"`; failures raise. |
| `kv.list(prefix[, scope])` | Immutable `{count, keys, suffixes, rf_safe}`; honor reply capacity and let users narrow the prefix. |
| `kv.transaction(operations[, scope])` | `{ok, status, error}` for up to four distinct keys in one scope. Check `ok`; distinguish conflict/rejected/unknown, retaining rollback-compatible formats. |
| `timer.set(name, seconds[, scope])` | View with `state, deadline_utc, revision, replaced, time_trusted`; 1..86400 seconds; trusted time required. |
| `timer.get/cancel/wait(name[, scope])` | Immutable state view. Wait yields until a durable claim; cancellation/clock/deadline/storage errors raise. Cancel cannot undo a claimed effect. |
| `sleep(ms)` / `timer.sleep(ms)` | Yield for 1..30000 milliseconds; transient and cancelled with the source/job, not restored after restart. Late completion fails. |
| `reminder.after(duration, text)` | View with personal ID/state/deadline; `1s`..`24h`, DM and owner-enabled reminder grant. Durable autonomous sending rechecks time and fresh direct route. |
| `reminder.list()` / `reminder.cancel(id)` | Bounded personal listing / state view. `sent` is confirmed TX, not recipient delivery; `unknown` must not be blindly rescheduled. |
| `utility.calc/convert/roll/choose(...)` | Bounded printable result; invalid syntax, incompatible units or RNG failure raise. Units are case-sensitive; randomness is native, not seeded Lua `math.random`. |
| `rpc.call(service, operation, args)` | `{ok=true, result=...}` or `{ok=false, error={code,message}}`; configured endpoint, grant and trusted time required. `unknown` means submission may have happened. |
| `rpc.text(value, limit)` | Printable bounded display text, 16..reply_bytes; does not make an operation successful. |
| `http.get(endpoint)`, `http.post(endpoint, table)` | Named configured endpoint only, no arbitrary URL/query; `{ok, http_status, body}` with decoded JSON `body`, or `{ok=false, error={code,message}}`. |
| `json.encode(value)`, `json.decode(text)` | Bounded native JSON conversion; malformed/unsupported values raise. |
| `mesh.wait{kind, exact/prefix, from, timeout_ms}` | Yield for matching authenticated DM or verified channel traffic; channel waits require their explicit grant. Timeout/cancel/grant revocation raises. |

Omitted scope chooses this authenticated full-key `"caller"` scope; `"conversation"`
binds caller and bot. Use `"channel"`
only for the verified selected channel with its explicit shared-state grant.
The `"bot"` scope is shared across users and also grant-gated. Source identity
and nicknames never substitute for the native full-key principal.

## Update one installed Lua file

Use the existing authenticated owner connection to manage files in one shared
Lua environment. For example, on a host role's private owner socket:

```sh
python3 -B tools/hardware/admin.py --unix-socket /path/to/owner.sock source-list
python3 -B tools/hardware/admin.py --unix-socket /path/to/owner.sock source-install config config.lua
python3 -B tools/hardware/admin.py --unix-socket /path/to/owner.sock source-install monitor monitor.lua
python3 -B tools/hardware/admin.py --unix-socket /path/to/owner.sock source-export monitor saved-monitor.lua
python3 -B tools/hardware/admin.py --unix-socket /path/to/owner.sock source-remove monitor
```

For an on-device role, use its authenticated web or encrypted Management
connection instead of `--unix-socket`. `source-install` replaces only the
named file, preserving the other file bytes. The installer commits the entire
set against the hash it downloaded; another operator's intervening change
rejects the commit. Validation rejects conflicting definitions before
activation. Successful activation rebuilds the shared Lua environment and
cancels its current Lua jobs; durable data and the separate Wasm runtime remain.

The source-set limit is **eight entries and 4,096 bytes total**, including
names, lengths and separators. A bundled-command reference consumes one entry,
leaving seven custom files; it does not copy the bundled program into the
envelope. Lua configuration tables can live in a named file, but JSON and
arbitrary filesystem imports are not Lua sources. The entry order controls
initialization, so install shared configuration before its consumers.
`source-export` reads one installed file unchanged and creates a new private
output file; it does not activate or replace anything. A symbolic bundled
reference has no installed file to export. `download` reads the entire set.

`source rollback` selects the previous complete source set, not an independent
version of one file. To restore one file while retaining the others, use
`source-install` with the operator's known-good copy. The same source limits
apply on the host, ESP32 and compact Pine profile; Lua allocator limits and job
slots are profile-specific.

If the selected file is missing or its stored size changed, the role refuses
whole-set rollback because it cannot read the active schema contract. Native
Management remains available: inspect `source status`, wait for any bounded live
retries to finish, use `source remove`, wait for bundled handlers to become
active, then install your known-good files.
Removing source does not erase durable data. Do not load an older schema unless
it can read that data.

Developers can check the exact envelope and runtime boundary without radio
traffic:

```sh
make -C firmware/esp32 bot-source-capacity-test ONCHIP_BOT_WASM=0
make -C firmware/esp32 bot-source-capacity-test ONCHIP_BOT_WASM=0 \
  TEST_VARIANT=source-capacity-compact \
  TEST_FLAGS='-DONCHIP_BOT_COMPACT_PROFILE=1 -DONCHIP_BOT_JOB_LIMIT=2'
make -C firmware/esp32 bot-source-set-test ONCHIP_BOT_WASM=0
make -C firmware/esp32 bot-source-set-test ONCHIP_BOT_WASM=1
python3 -m unittest tools.hardware.tests.test_lua_sources tools.hardware.tests.test_admin
```

These host-native checks report their own allocator peak, not a device's
available heap or a firmware-installation result.
The source-set fixture measures Lua allocator and requested workspace peaks,
source-file bytes and serialized journal blobs across staging, activation and
native recovery. Filesystem/NVS overhead and non-Lua host allocations are not
included; these values are not an MCU free-heap measurement. Its restart checks
recreate the native fixture before and after journal selection, not a physical
power cut. Pine's single-session installer currently cancels Lua jobs before
candidate verification, rather than waiting for activation.

## Package metadata and schema rules

The first line records the package ID and version, exact Lua runtime and
command API, required native API capabilities, data schema, and rollback
compatibility:

```text
--@meshcore-bot/1;name=hello;version=1.0.0;runtime=lua-5.5.1;api=named-commands-v1;caps=kv;schema=none;rollback=none
```

Supported capability names include `cmdmeta`, `events`, `https`, `kv`,
`kv.atomic`, `mesh`, `mesh-chan`, `mesh-dest`, `modules`, `reminders`, `timers`,
and `utilities`. `cmdmeta` requires optional command permission/example
metadata; `mesh-dest` requires owner-granted full-key DM destinations; and
`mesh-chan` requires verified channel waits (still gated by the owner's
default-off flag). Stateful/custom channel commands require an explicit
`!@KEY8` or full-key target; a channel key is not an individual identity.
A manifest lists API requirements only; it does not grant them. Native owner
policy, authenticated request context and per-operation
authorization still control storage scopes, mesh sends, configured host calls
and event access. Firmware checks the format, runtime, API, capability names
and schema transition before activating a candidate.

Command declarations may include optional export, permission, and example
metadata while preserving existing declarations:

```lua
command("lookup", "key:string:32", "Look up a value",
        "lookup_value", "dm", "!lookup plan")
```

Supported permission names are `public`, `dm`, `owner`, `channel`,
`shared`, `reminder`, and `home`. Permission metadata describes command
admission; it does not create owner trust, a channel grant, a reminder grant,
or a configured-host grant. Keep the example short and non-sensitive.

Native discovery includes paginated `!help` / `!plugins`, bounded
`!neighbors`, and `!admin` operations restricted to a trusted-owner DM.
`!plugins` describes the active runtime source generation, its manifest,
modules and events; that generation label resets on boot and is not the durable
source-journal revision. It does not imply independent plugin installation or
quarantine.

Use `schema=none` when the source has no application data format. Otherwise use
`name@positive-integer`. If an update changes schema from `notes@1` to
`notes@2`, it must declare `rollback=notes@1`. The new source must retain the
old representation for the previous version while it may be selected for
rollback. The old version then reads data written by the new version. The
source manager refuses schema changes that omit this declaration and refuses
a rollback that is not declared by the active package.
On a first install over bundled source (`schema=none`), `rollback=none`
declares compatibility with that bundled schema and permits rollback to the
bundled generation.

Schema conversion is not an arbitrary firmware migration script. Perform a
compatible conversion lazily within the authenticated data scope using
`kv.transaction`, keep old keys/formats readable until rollback is no longer
needed, and propagate `conflict`, `unknown` and `rejected` outcomes. The
`plugins/examples/notes-v1.lua` and `notes-v2.lua` example retains the v1 key
and atomically updates both representations, so a rollback does not lose a
note. The v2 example also initializes a new caller's empty scope; it refuses
unmarked pre-existing note data rather than overwriting it. Transactions are
bounded to four distinct keys in one existing storage
scope. Destructive or incompatible schema changes are rejected; the loader
does not claim to make them reversible.

Package the tested hello command from the repository root:

```sh
mkdir -p .tmp
python3 tools/hardware/admin.py package create \
  firmware/runtime/plugins/template.lua .tmp/hello.bot.lua \
  --name hello --version 1.0.0 --capability cmdmeta
python3 tools/hardware/admin.py package inspect .tmp/hello.bot.lua
make -C firmware/esp32 bot-package-validate PACKAGE="$(pwd)/.tmp/hello.bot.lua"
make -s -C firmware/esp32 bot-local SOURCE=.tmp/hello.bot.lua \
  SCENARIO=firmware/runtime/plugins/examples/hello.scenario.json
```

The last command compiles and initializes the package in the actual embedded
runtime. Package creation fails if metadata plus Lua source exceeds 4,096
bytes. No archive, decompressor or per-handler VM is introduced.

## Test-only native replay

`bot-host-runner` runs the native bot VM, supervisor, parser and MeshCore
packet path with a simulated radio and in-memory NVS/SPIFFS. To run a bot on
a real shared modem, use the [Go native host](../../README.md#host-status).
The host submits transmit requests to the modem's shared airtime/CAD scheduler
and correlates terminal receipts; queue acceptance alone is not RF success.

The reusable harness is
[`firmware/esp32/tests/support/BotNativeHarness.h`](../esp32/tests/support/BotNativeHarness.h)
in namespace `bot_native_test`;
`make -C firmware/esp32 bot-native-harness-test` builds it
with the same native sources and runs the native and package replay suites.
`TxFrame` reports the native `(sourceSlot, sourceGeneration, sourceJob)` token,
priority, eligibility delay and expiry at simulated radio start. Test
completion must use `completeSimulatedTxForTest(token, airtimeMs)` and yields a
terminal `TxOutcome` with `simulated=true`, without transmitting over RF.
Storage can be replaced through `BotNativeHarness::Options` with
`StorageDriver` implementations of the checked NVS and filesystem seams.
Persistent adapters must provide durable NVS commit and atomic filesystem
rename.

`bot-local` always emits JSONL. For direct C++ integration, build the
`bot-host-runner` target and pass its **exact** `BOT_HOST_RUNNER` path (the
target uses that path internally), with `--format jsonl`, `--source FILE` or
`--bundled`, and optionally `--replay FILE`. Use `--validate FILE` for
compile/initialization-only validation. From the repository root, the stable
operator entry points are:

```sh
make -C firmware/esp32 bot-local SOURCE=PACKAGE.lua REPLAY=FILE
make -C firmware/esp32 bot-package-validate PACKAGE="$(pwd)/PACKAGE.lua"
```

JSONL v1 emits one object per line with `format:"meshcore-bot-replay-v1"` and
typed `validated`, `source`, `reply`, `install`, `restart`, `clock`, `fault`,
`advance`, `status`, `reject`, or `error` records. Context/scenario extensions
add `context`, `owner`, `reflection`, `source_control`, `channel`, `grant`,
`network`, `route`, `message`, `send`, `pump`, `cancel`, `pending` and
`async_reply`. Validation returns one
`validated` record on stdout; replay starts with a `source` record and then
records command results and directive outcomes. `validated` contains runtime,
API, supervisor, command, source byte count and SHA256; `source` and successful
`install` records contain generation, source slot, byte count and SHA256;
`reply` contains the command, response text and duplicate flag; `restart`
identifies `bundled` or `journal` recovery and includes native deployment
status. `owner` reports the configured full key and commit outcome;
`context.owner` reports current native trust for that request context, not
permanent authority. `source_control.ok` reports acceptance of the control
request, not completion of an asynchronous deployment.
A bundled `source` record identifies
`BotDefaultSource`'s bytes/hash; supplemental compiled utility/network/board/
diagnostic handlers are not included in that text hash.
`clock`, `fault`, `advance` and `status` contain
their respective epoch/trust, fault/enabled, millisecond, and runtime counter
fields. Failed installs and rejected lines contain an `ok:false` or stable
`code` plus message/line. `@duplicate` has no standalone record; the next
`reply` has `duplicate:true`. CLI/setup failures return a versioned `error`
record on stderr and a nonzero exit status; replay command exceptions are
versioned `error` records on stdout. Native syntax errors produce a `reject`
with `code:"invalid_native_command"` and the parser message; explicit TRACE
width 3 is rejected, never inferred or downgraded. Native diagnostic output is
suppressed in JSONL mode. Text replay is the only input protocol:
one `!command ...` or supported `@directive` per line from a file or stdin;
stdin does not accept binary role packets or radio state. Non-ASCII output
bytes are escaped individually as `\u00hh`, without UTF-8 normalization.
Native admission denials without an RF notice emit `reject` with
`code:"native_admission"` and the native admission snapshot. `status` additionally
reports `rejected`, `airtime_limited`, `busy` and current `jobs`.

Oversized or overflowing `@advance` values are rejected within the remaining
32-bit native clock-age range. Neither clock directive uses a physical clock
or a network.

## Source-generation lifecycle

A package is one complete custom source generation, not an independently
installed plugin. Its `name` is descriptive metadata; there is one active
manifest with up to eight command exports and one shared Lua environment.
Install and update replace that complete generation, rollback restores the
previous generation, and remove restores the bundled source. Per-plugin
isolation, removal, and crash-loop quarantine are not provided.

The metadata header consumes bytes inside the existing 4,096-byte source
envelope and adds no package/schema NVS record. Source activation continues to
use the existing deployment journal and source slots.

## Optional signatures

Create a detached signature with a caller-selected Ed25519 key:

```sh
python3 tools/hardware/admin.py package keygen .tmp/plugin-signing.key .tmp/plugin-signing.pub
python3 tools/hardware/admin.py package sign .tmp/hello.bot.lua \
  --private-key-file .tmp/plugin-signing.key
python3 tools/hardware/admin.py package verify .tmp/hello.bot.lua \
  --signature .tmp/hello.bot.lua.sig --public-key-file .tmp/plugin-signing.pub
```

The private key is created with mode `0600`; signature verification is optional
and uses the public key selected by the operator. It does not replace mast
password or trusted-companion authorization, and the device does not pin a
package-signing identity.

## Install, update, inspect and recover

The mast CLI uses the existing authenticated native RF transfer through a KISS
gateway or the authenticated mast web endpoint. `MAST_CLI_ARGS` contains only
connection options and file paths; credentials remain in private files.

```sh
make -C firmware/esp32 bot-plugin-install PACKAGE=../../.tmp/hello.bot.lua \
  MAST_CLI_ARGS='--gateway MODEM_HOST --target FULL_MAST_PUBLIC_KEY --seed-file ../../.tmp/companion.seed --password-file ../../.tmp/mast-password'
make -C firmware/esp32 bot-plugin-status MAST_CLI_ARGS='--web http://mast.local --password-file ../../.tmp/mast-password'
make -C firmware/esp32 bot-plugin-diagnose MAST_CLI_ARGS='--web http://mast.local --password-file ../../.tmp/mast-password'
make -C firmware/esp32 bot-plugin-reboot MAST_CLI_ARGS='--web http://mast.local --password-file ../../.tmp/mast-password'
make -C firmware/esp32 bot-plugin-update PACKAGE=../../.tmp/hello-1.1.0.bot.lua MAST_CLI_ARGS='...'
make -C firmware/esp32 bot-plugin-rollback MAST_CLI_ARGS='...'
make -C firmware/esp32 bot-plugin-remove MAST_CLI_ARGS='...'
```

`package-install` and `update` inspect local metadata and optional signatures,
query the device runtime/API/capabilities and current schema, then use the
existing resumable numbered source transfer. The device compiles and validates
the candidate before atomically changing its durable active generation. The
previous source remains available; `rollback` checks the active schema contract
before switching. `remove` restores the bundled command source and retains
application data. Reboot loads the durably selected source without erasing the
node identity or application storage. `diagnose` reports source hash/generation,
package metadata, runtime API, bot state and counters; it does not export
private user data or credentials.

If the active source was uploaded without package metadata, its application
schema is unknown. Restore bundled handlers with `bot-plugin-remove` before
installing a metadata-bearing package; the source manager retains stored data
and refuses to guess that an unknown schema is compatible.

When the owner has enabled `bot home on` and committed the fixed named HTTPS
GET endpoint `package`, an authenticated owner can fetch a package by its
expected full source hash. The endpoint returns raw Lua source as
`text/plain` or `application/octet-stream` with an explicit Content-Length from
1 through 4,096 bytes. Redirects and chunked/encoded responses are rejected.
The existing native HTTPS worker streams the body into the source staging
file while checking its SHA256; after completion the source manager performs
the normal readback, package metadata, schema and Lua validation before
durable activation. Network completion never activates a package. The CLI
accepts no URL, host, path or credentials from the caller and adds no network
client:

```sh
make -C firmware/esp32 bot-plugin-fetch PACKAGE_SHA256=FULL_LOWERCASE_SHA256 \
  MAST_CLI_ARGS='--web http://mast.local --password-file ../../.tmp/mast-password'
```

The expected SHA256 covers the decoded plain Lua source, not the JSON wrapper.
The device verifies that hash, then checks the package metadata, runtime, API,
capabilities, schema compatibility and Lua validation before activation.
Network I/O uses the shared native HTTPS worker; a failed, cancelled or
incompatible fetch leaves the active source unchanged. If status is pending or
unknown, inspect `bot-plugin-status` before retrying.

Optional signed package install verifies the detached signature on the host
before transfer:

```sh
make -C firmware/esp32 bot-plugin-install PACKAGE=.tmp/hello.bot.lua \
  SIGNATURE=.tmp/hello.bot.lua.sig VERIFY_KEY=.tmp/plugin-signing.pub MAST_CLI_ARGS='...'
```

For configured HTTP/JSON/RPC, production native-host TLS and expected-hash fetch
setup, see [network API](NETWORK_API.md). The focused, hardware-free validation
command is `make -C firmware/esp32 bot-network-check`.

Use the complete local integration set with `make -C firmware/esp32
bot-release-check`. It covers package parsing/signatures, the embedded-runtime
host runner, source lifecycle, data limits, and configured HTTPS worker tests.
