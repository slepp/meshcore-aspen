# Lua and portable Wasm on one bot

The native host bot and ESP32-S3R8/Wio SX1262 build can keep a Lua command
package and a Wasm package active together. Wasm commands use the same native
identity, command permissions, full-key KV scopes, I/O queues and radio
source as Lua. Installing or removing a Wasm package does not erase Lua
source or either runtime's durable data.

The device target is ESP32-S3 with PSRAM; builds without PSRAM cannot load
a Wasm session and report allocation failure rather than taking internal
radio memory. The supplied native worker target is Linux x86-64. This
integration does not add an nRF52840 interpreter or enable Wasm on Pine.

On ESP32, the interpreter runs on the existing FreeRTOS command worker.
Its native execution identity is the current task handle, so guest initialization
and calls do not require a pthread-created worker. Guest threads remain disabled;
the Linux host keeps WAMR's POSIX thread adapter.
ESP32 runtime mutexes still use the SDK's pthread mutex implementation, which
supports FreeRTOS callers. Thread creation, condition waits and shared guest
memory are not enabled. The SDK task handle and WAMR execution identity are
32-bit values on ESP32-S3; WAMR compares this opaque identity and formats it
through `uintptr_t` when logging.

The command worker's minimum-free-stack diagnostic measures its native task
stack. It is separate from the 8KiB Wasm operand-stack limit. When the SDK
does not provide a native stack boundary, WAMR receives `NULL`; do not treat
worker high-water readings as a WAMR native-stack guard.

Build examples and run the host checks from the repository root:

```sh
make -C firmware/esp32 bot-wasm-test bot-wasm-integration-test bot-wasm-worker-test
make -C firmware/esp32 bot-wasm-build-test
```

The second command builds an ESP32 candidate with native management;
it does not flash a radio.
Use `bot-firmware` with `ENV=Xiao_S3_WIO_onchip_beta` and an operator-owned
`CONFIG` for authenticated management and persistent deployment. Ordinary
`firmware` preparation does not prepare either interpreter.
## Compile Wasm in or out

Use the existing `ONCHIP_BOT_WASM` flag for both host and firmware builds.
Make defaults to `1`; C++ without a prepared firmware configuration defaults
to `0`. This is a compile-time selection, not an owner setting:

```sh
make -C firmware/esp32 bot-native-worker bot-host-runner ONCHIP_BOT_WASM=1
make -C firmware/esp32 bot-wasm-disabled-test bot-vm-test ONCHIP_BOT_WASM=0
```

Host artifacts have separate `-wasm1` and `-wasm0` build directories.
Disabled artifacts do not build or link WAMR or `BotWasm.cpp`, contain no
WAMR runtime symbols, allocate no Wasm sessions or interpreter pool, and
advertise only Lua through `source api runtimes`. Raw modules, packaged
modules and `source wasm ...` requests report `Wasm runtime unavailable in
this build`; they are never parsed or executed as Lua. Lua, JSON, HTTP,
configured RPC, native management and the shared radio keep their existing
behavior.
Disabling Wasm does not erase its durable journal or packages. Re-enabling
the interpreter loads that runtime's last selection; flashing filesystem
images is a separate destructive operation.

Build either ESP32 candidate without flashing:

```sh
# Supply the normal build environment or operator-owned profile separately.
make -C firmware/esp32 bot-firmware ONCHIP_BOT_WASM=1 \
  BUILD="$PWD/.tmp/onchip-wasm-enabled" \
  CONFIG="$PWD/firmware/esp32/platformio.ini.example" ENV=Xiao_S3_WIO_onchip_https
make -C firmware/esp32 bot-firmware ONCHIP_BOT_WASM=0 \
  BUILD="$PWD/.tmp/onchip-wasm-disabled" \
  CONFIG="$PWD/firmware/esp32/platformio.ini.example" ENV=Xiao_S3_WIO_onchip_https
```

The example profile uses `${sysenv.ONCHIP_BOT_WASM}`, exported by Make.
It also requires a valid `MESHCORE_HOSTNAME` DNS label. For build-only
fixtures, prefix the commands with `MESHCORE_HOSTNAME=wasm-fixture`; configure
the other operator-owned environment values before building an image to
install.
In a private profile, use that same flag or set a literal `0`/`1` matching
the Make argument. Preparation records the selection in `WasmConfig.h`;
contradictory compiler flags fail with an instruction to prepare again.
Disabled preparation does not fetch/configure WAMR, and removes its
generated library, source and SDK from that build tree. Reusing a build tree
after changing the flag therefore cannot retain the interpreter dependency.
When running PlatformIO directly in a prepared tree, export the same
`ONCHIP_BOT_WASM` value first. Use `bot-prepare` again when changing it.

For an installed node, build with the same provisioning mode and private
configuration, selecting the flag explicitly. Keep credentials out of command
arguments and logs. Upload only the application; do not provision its filesystem
again. See [ESP32 updates](../esp32/README.md#first-flash-and-updates).

## Build and install a package

The SDK uses Clang 21.1.8 with `wasm-ld` 21 and Rust 1.96.0 with the
`wasm32-unknown-unknown` target. No WASI sysroot, C library, Cargo dependencies,
JIT or target-specific Wasm compiler is needed. Install these development
tools on the build host, not on the radio. The C and Rust examples contain
arithmetic, private persistent notes and asynchronous configured-home RPC.

Select the LLVM 21.1.8 tools on `PATH`, then prepare the pinned Rust target:

```sh
rustup toolchain install 1.96.0 --profile minimal
rustup target add --toolchain 1.96.0 wasm32-unknown-unknown
clang --version
wasm-ld --version
```

```sh
make -C firmware/esp32 bot-wasm-examples
python3 tools/hardware/admin.py package create \
  .tmp/wasm-examples/c-notes.wasm .tmp/wasm-notes.package \
  --name wasm-notes --version 1.0.0 --capability cmdmeta --capability kv
python3 tools/hardware/admin.py package inspect .tmp/wasm-notes.package
```

`wasm/build_examples.py` contains the complete compiler commands and writes
the executed commands to `.tmp/wasm-examples/commands.json`. Both toolchains
use a 4 KiB guest C/Rust stack, one initial/maximum 64 KiB memory, no entry
point and stripped debug/custom sections. Use the same resulting module
on host and device; do not rebuild it for Xtensa.

Install with the existing authenticated CLI connection:

```sh
python3 tools/hardware/admin.py --unix-socket /absolute/bot/native/admin.sock \
  package-install .tmp/wasm-notes.package
python3 tools/hardware/admin.py --unix-socket /absolute/bot/native/admin.sock \
  --runtime wasm status
python3 tools/hardware/admin.py --unix-socket /absolute/bot/native/admin.sock \
  --runtime wasm rollback
```

The RF `--gateway`, `--target`, `--seed-file` and optional password options
and authenticated `--web` connection work through the same management
backend. Package installation detects its runtime automatically and checks
runtime/API/capabilities before transferring chunks. Lifecycle operations
default to Lua; use `--runtime wasm` for Wasm download, status, rollback and
remove. Detached Ed25519 package signatures use the same signing/verification
commands as Lua. Device authentication remains mandatory; detached signature
verification remains a CLI option.

For an owner-configured HTTPS package endpoint, inspect the selected
runtime's fetch contract and then request the exact package digest:

```sh
python3 tools/hardware/admin.py --unix-socket /absolute/bot/native/admin.sock \
  --runtime wasm command "source api fetch"
python3 tools/hardware/admin.py --unix-socket /absolute/bot/native/admin.sock \
  --runtime wasm package-fetch package PACKAGE_SHA256
```

The fixed `package` GET alias, trust bundle and credentials stay in native
owner configuration. Enable the HTTPS policy with `home on` on a host role
or `bot home on` on an on-device role after configuring the endpoint and
synchronizing time. These commands save and apply the grant immediately;
they do not require `apply` or a reboot.

Wasm fetch streams binary bytes to its own staging file;
the complete digest, package runtime/API/schema and actual interpreter
validation must succeed before activation. `application/wasm` and
`application/octet-stream` responses are accepted. Failed or cancelled
requests retain the active selections. A submitted GET with a lost
completion is reported as unknown, not successful or automatically retried.
Explicitly canceling an in-flight fetch discards staging and retains the
active source. When its GET was already submitted, status distinguishes
that known local cancellation from the unknown remote GET outcome. Failed
staging cleanup still reports an error; cancellation never confirms remote
receipt or changes the active generation.

On the authenticated `/admin` page, refresh program capabilities, select
**Wasm**, choose a `.wasm` or packaged binary file, then **Upload / resume**
and **Verify and install**. Durable Wasm readback downloads a binary file
after digest/generation verification. Neither upload nor download decodes
Wasm into the Lua editor; unsaved Lua edits remain in the tab. The disabled
build does not enable the Wasm installer. Installation replaces only the
selected runtime's whole command set; it is not an arbitrary package registry.

An ordinary authenticated bot DM can invoke `!wnote text`, `!wnote`,
`!wadd 40 2`, or the Rust examples' `!rnote` and `!radd`. Expected sum: `42`.
RPC examples `!wecho text` / `!recho text` require the operator's configured
home-service grant and use its existing asynchronous provider. Neither
module receives URLs, service credentials, private identity keys or a shell.

Native management commands:

```text
source api runtimes
source wasm api
source wasm api package
source wasm metadata
source wasm status
source wasm rollback
source wasm remove
```

Lua keeps its existing `source ...` commands and deployment journal. Wasm
uses a separate `wasm-source` journal and three separate SPIFFS package
slots. SHA256 checks, numbered durable chunks, staging, validation, readback,
activation and rollback use the existing installer. Shared staging is
serialized between runtimes. Updating a runtime cancels outstanding jobs
in both runtimes to fence the shared native I/O generation, but retains
the other runtime's VM and durable data. Conflicting command names are
rejected. Removal does not erase KV, timers or reminders.

## Runtime and module profile

WAMR **2.4.1**, revision
`b124f70345d712bead5c0c2393acb2dc583511de`, is fetched with archive SHA256
`c197d6d811c23b3df0446e91abffba1091caf282e026156d19ace8ba2fc83f3e`.
Firmware MeshCore remains `companion-v1.17.1`,
`d92964352441e53b93e8667b802e04f6e072b39e`; Lua remains 5.5.1.

Host and device execute the classic interpreter, with fast interpreter,
AOT, JIT, WASI, built-in libc, guest threads, bulk memory, reference types,
SIMD, GC, memory64, multiple memories and module linking disabled. WAMR's
numeric/control instructions, multi-value internal functions, sign extension
and non-trapping float-to-int conversion are available. SDK exports/imports
use only `i32` signatures. Tables and imported globals/memories are rejected;
the module must define one unshared memory with initial/maximum one page.
Start sections and implicit initialization exports are rejected: only
metered `mc_init` initializes a package.

| Bound | Value |
| --- | --- |
| Package, metadata plus module | 4096 bytes |
| Command registry | 8 Wasm commands alongside 8 custom Lua commands |
| Native job capacity | 4 total; one reserved for native diagnostics |
| Linear memory | at most 65536 bytes; growth past one page returns `-1` |
| Interpreter operand/call stack | 8192 bytes per execution environment |
| Instruction budget | 10000 across start and all async resumptions |
| Guest execution wall budget | 20000 µs accumulated across resumptions |
| Initialization wall budget | 50000 µs for `mc_init`, separate from command execution |
| Loader wall budget | 330000 µs, checked after bounded load |
| Imported native calls | 64 across start and resumptions |
| Yielded native operations | 8 across start and resumptions |
| WAMR shared runtime pool | fixed 1048576 bytes, allocated lazily in PSRAM |

The interpreter dispatch checks counted fuel on actual instructions.
A small pinned patch adds a deadline/event-epoch check at dispatch and
counts instructions across resumptions. Infinite loops therefore trap
inside the interpreter; the radio task or an outer watchdog need not kill
the worker. Native imports only validate/copy bounded values and submit
existing asynchronous operations.

ESP32 linear memory, interpreter allocations and session storage require
PSRAM without internal-heap fallback. The host interpreter test measures
52,176 bytes for one session with the current networking result/request
structures; target sizes differ with pointer width. The existing
VM worker's 16 KiB task stack and storage/network workers retain their
internal-memory tiers. Jobs and copied requests/completions stay in retained
session storage and reset in place; the ESP32 build's Wasm result-poll function
uses a 32-byte stack frame. `Command VM` diagnostics report instruction count
and CPU time; serial `Wasm pool=... peak=... session=... linear=... stack=...`
reports the shared allocator's measured high water and session size.
Pool high water is global, not a per-module heap measurement. Host linear
memory is separately mapped; on ESP32 it is separately allocated in PSRAM.
The shared pool remains allocated after the last Wasm package is removed,
until the bot process or device restarts.

An unloaded Wasm session returns an empty manifest view without allocating a
command table in internal RAM. On ESP32, keep the native TLS admission budget:
two SDK record buffers within 68 KiB total work and a separate 32 KiB radio
reserve. A rejected admission returns `heap_unavailable` before connecting;
free PSRAM does not substitute for this internal-RAM requirement.

Firmware build reports show static RAM and flash, not free internal RAM or
PSRAM while Wi-Fi, TLS and radio workloads run. Measure those separately on
the installed candidate. The disabled build has no retained Wasm pool;
enabling Wasm does not preallocate its 1 MiB pool until a module is loaded.
The `Xiao_S3_WIO_onchip_https` build measured 138,068 bytes static RAM and
1,914,485 bytes flash with Wasm enabled, versus 137,892 bytes static RAM and
1,821,133 bytes flash with Wasm disabled, using the `wasm-fixture` hostname.

## ABI v1

Imports belong exclusively to module **`meshcore_v1`**. SDK declarations:
[C](wasm/sdk/meshcore.h), [Rust](wasm/sdk/meshcore.rs).

| Import | Signature (all arguments/results `i32`) |
| --- | --- |
| `command` | `(id,name,len,schema,len,help,len,permission) -> status` |
| `subscribe` | `(event_kind,handler_id) -> status` |
| `read` | `(opaque_handle,field,guest_output,capacity) -> copied_length` |
| `reply` | `(job_handle,guest_text,length) -> status` |
| `io` | `(job_handle,kind,scope,key,key_length,value,value_length,delay) -> operation_handle` |

Addresses designate guest linear-memory offsets, never firmware pointers.
Ranges are checked before copying; strings are bounded and NUL-free,
replies printable ASCII within the packet limit. Public sender/channel
keys are copied as all 32 bytes. Integer fields use little-endian `i32`,
uptime uses little-endian `u64`; schemas convert command integers to signed
32-bit values. Lua retains its existing signed 64-bit arithmetic. Missing
optional arguments copy zero bytes. Opaque handles are local to the active
invocation, never reused in a session; wrong/stale handles trap.

Required exports:

```text
mc_init() -> i32                        returns 1 (ABI version)
mc_start(job_handle, handler_id) -> i32  returns 0=done or 1=pending
mc_resume(job_handle, operation_handle, ok) -> i32
```

Registration is initialization-only. A pending return requires exactly one
submitted I/O. Guest stack frames do not survive a pending return: save
continuation state in linear memory and return. The host later calls
`mc_resume` with the matching opaque operation; `read` copies its bounded
value, error, outcome and typed timer fields. Module globals are shared by
its concurrent jobs, so keep per-job continuation state when required.
Completion tokens use the existing source/job/operation fence. Update,
rollback, cancellation or event grant revocation prevents a stale
completion from resuming its guest.

I/O kind, scope and permission numbers are pinned in the C/Rust SDK;
existing IDs are unchanged. Utility is appended as kind `25`, after the
native network/package variants. Kinds `0..3`, `8..12` and `17` retain their
original calling conventions. Event callbacks may sleep and use scoped KV
(including atomic operations) or timers, but cannot send radio traffic,
set personal reminders or obtain private network authority.

| Operation | `key` / `value` / `delay` arguments |
| --- | --- |
| KV get/put/delete/list, timers | Key/prefix and optional value; timer-set delay is seconds |
| CAS / transaction | Empty key; value is 1..4 copied `mc_mutation` descriptors; CAS requires one comparison |
| Personal reminders | Empty key; set takes text and 1..86400 seconds; cancel takes an ID in delay; list has no text |
| Send / wait / TRACE | Key is one `mc_mesh_options`; value is send text or wait prefix/exact filter; delay is 1..30000 ms, or zero for 5000 ms |
| Forward | Empty key/value; delay is an owned received-packet handle, never a native packet ID |
| Advert / inspection / admin | Advert has no text; inspection key is `neighbors`, delay selects page 1..16 (zero selects 1); admin value is the owner command |
| Utility | Key is `calc`, `convert`, `roll` or `choose`; value is text, or one `mc_convert` descriptor for conversion |
| HTTP GET / POST | Key is a configured endpoint name; POST value is bounded JSON, GET has no payload |
| Home RPC | Key is `health`, `echo` or `weather`; value is its bounded text argument |
| Named RPC | Key is a `mc_rpc` descriptor (24 bytes), value is empty; descriptor copies configured service, operation and JSON object arguments |

For named service `home`, the arguments are exactly `{}` for `health`,
`{"text":"…"}` for `echo`, or `{"place":"…"}` for `weather`. Echo text is
1..256 decoded bytes; weather place is 1..80 decoded bytes. The binding
decodes JSON string escapes and Unicode into the copied native argument,
and rejects duplicate/extra fields, wrong types, invalid UTF-8, NUL and
out-of-bound text before dispatch. Other configured service mappings retain
their JSON arguments. These calls still require private authenticated DM
and the native home grant.

Use `mc_resume`'s `ok` argument to distinguish successful RPC completion.
Successful home calls expose `MC_VALUE` and the copied weather fields;
their `MC_JSON` and `MC_RPC_CODE` may be empty. On failure, read
`MC_RPC_CODE` and `MC_ERROR`; do not require an invented `"ok"` code on
success.

Descriptors contain guest offsets/lengths, not borrowed native pointers.
Atomic flags distinguish comparison, expected presence and deletion;
distinct-key, single-scope transactions use the existing journal/recovery
fence. KV request/publication deadlines remain two seconds, including
recovery; the separate recovery limit remains ten seconds. A sparse store
reuses an exactly identical, already-validated unused record's digest and
constructs its sealed empty record once per recovery. Missing authority
beside existing files still blocks access; no absent-file result is cached.
An interrupted first write's verified initialization marker resumes empty
file-bank creation under the recovery deadline, discarding unpublished
contents. A cold request can still reach its two-second deadline while
recovery completes; reconcile its outcome before a new write, and never
replay an unknown publication.
Send destinations are the authenticated caller or an owner-granted
full key. Channel sends require verified channel authority and explicit
targeting; channel waits require the native owner grant. TRACE accepts
explicit 1/2/4/8-byte routes. Forwarding requires a received DM owned by
the same invocation and the owner-configured pair grant.

`read` exposes copied outcome (`0=rejected`, `1=committed`, `2=conflict`,
`3=unknown`), timer/reminder state, HTTP status/JSON/submission state,
radio queued/transmitted/acknowledged flags and owned packet/trace records.
Admission or a queued transmission is not confirmed RF transmission.
Copied authority, argument type tags, node/air/signal/path snapshots and
public keys contain no credentials. Wire records have fixed SDK layouts:
`mc_node` is 160 bytes, `mc_packet` is 180 bytes; descriptor members are
little-endian `u32`, signal values are `f32`, temperature is `f64`.
Read output capacity is bounded to 2048 bytes, including complete network
JSON; replies retain the native RF bound and printable-ASCII requirement.

Command permissions and storage decisions use the same native functions
as Lua. Native grants/configuration, source generations and event epochs
are checked at admission and completion. A revoked grant after submission
does not erase an uncertain/committed outcome. Both runtimes receive shared
event kinds, with alternating first dispatch and distinct native-operation
token namespaces; one runtime's callback does not suppress the other.

## Developer validation

`bot-wasm-test` executes C/Rust modules and deliberately faulty modules.
It checks arithmetic, copied KV/RPC requests, stale completion cancellation,
unknown imports, wrong handles, out-of-bounds imports and memory access,
runaway command/initialization code, recursive operand-stack exhaustion, refused memory growth,
native-call/native-operation limits and real runtime-pool exhaustion.
After exhaustion, a previously loaded arithmetic module still executes.
`bot-https-worker-test` with `ONCHIP_BOT_WASM=1` also builds the C/Rust
fixtures and sends their legacy and named home health/echo/weather requests
through the actual native provider using a controlled transport. It checks
that malformed home arguments never open a connection. No external service
is contacted.
`bot-wasm-integration-test` uses actual native encrypted packet dispatch:
Wasm notes survive runtime replacement, Lua keeps replying, and native
`!ping` works after a runaway Wasm trap. Its host runner installs raw Wasm
and binary packages through the Wasm `MastSource` begin/chunk/commit lifecycle,
while Lua uses its own journal. Restart reconstructs both retained programs
and preserves their durable data. Owner-command checks use the bound native
owner key and deny other callers before and after restart. Run
`bot-wasm-integration-test bot-local-test` together to check both runtime
lifecycles and the Lua owner's native administration/network scenarios.
Both runtimes share one custom-command namespace. Before changing a durable
source selection, the deployment service checks the candidate against the
other runtime's registered commands and reserves that namespace through
commit and live activation. A conflict reports the command and runtime,
retains both programs, and leaves the active/previous selection, generation,
hash and durable data unchanged. Rollback and restoring bundled handlers use
the same check; removing Wasm publishes an empty command set. Native builtin
names remain reserved. Enable the bot before committing Wasm or updating Lua
alongside retained Wasm so the live namespace can be checked; an unavailable
command VM refuses those activations without changing the deployment. Lua
can still be saved while the bot is disabled when the Wasm journal is empty.
A rejected upload remains staged for
explicit retry or `source cancel` / `source wasm cancel`.
If either runtime has a saved source that has not activated, apply or remove
that source before installing into the other runtime.
Use `source retry` for Lua recovery and `source wasm retry` for Wasm recovery.
Lua retry is refused while Wasm is copying, publishing or awaiting recovery,
before changing staging or command admission. A refused live staging/removal
attempt keeps the saved selection and tries again at most three times, with
1-, 2- and 3-second delays. After those retries, status names the selected
runtime's recovery command; a failed Wasm publication keeps its namespace
reservation until Wasm recovery or removal succeeds.

The integration suite rejects conflicts in both directions, then restarts
and verifies the original commands and custom state. It also checks both
rollback directions, bundled/remove behavior and reserved builtin names.
Recovery schedules pause actual file writes before publication and during a
reserved live copy, then check that a refused Lua retry changes neither
journal, staging bytes nor admission. They also exhaust live-copy retries,
hold a native cold-staging result, and run a 30-second native packet collector
to check bounded staging/removal refusal and recovery without restarting.
The native replay directives `@source-now` and `@staging` respectively skip
the normal post-command pump and report staging byte count/SHA256 and command
readiness.
`@fault source-copy-pause`, `source-live-pause`, `source-copy-wait`,
`source-live-read` and `source-result-held` control these test schedules;
`@fault off` releases them. These controls exist only in the host test runner.
`bot-wasm-worker-test` runs the
actual host worker executable through durable management upload, independent
update/rollback/remove and restart. Existing Lua VM/worker tests remain
available unchanged.
The worker suite also checks both callbacks on shared event kinds, their
revocation, real atomic/utility providers and Lua/native recovery after a
Wasm trap. Native reservation tests race source staging and two namespace
reservations, and reject tokens retained across a worker restart. The same
executable exercises real localhost TLS binary fetch,
wrong hashes, cancellation and restart. Browser-script tests compare exact
NUL/high-bit bytes across upload/download and preserve the Lua editor.
The host lifecycle test also damages Wasm package metadata while retaining
a valid native filesystem envelope: Lua and native management still start,
and `source wasm remove` recovers that runtime's journal.

`make -C firmware/esp32 beta-test ONCHIP_BOT_WASM=1` also executes an
owner-only Lua `node.admin` command and the C `wguard` state machine's
`MC_ADMIN` requests through the real native administration backend.
An authenticated Management owner can query bot status, but neither
source can change Management/Relay/Room passwords, import a private
identity, upload bulk source/data, set Wi-Fi credentials or stage HTTPS
CA/token secrets. The test compares retained credentials/settings after
each denial, checks native role logins, and retains the encrypted
Management RF password-setter/persistence tests. With flag `0`, the same
suite exercises Lua without building or loading the Wasm fixture.
`bot-native-clock-test`, `bot-native-worker-test` and `bot-native-host-test`
retain trusted host UTC, authenticated private Unix grant controls and
restart/no-replay checks in both build variants.

The build/test commands above do not change radio or service configuration.

Each runtime has one active package/VM. A package can register eight commands;
Lua can declare its existing internal source modules. Wasm module linking is
unavailable and `modules` is rejected as a required capability.

After boot, wait for synchronized SNTP time before TLS or timestamp-sensitive
RF checks; the build seed is not trusted UTC. Advertise the companion so a
newly booted bot can learn its authenticated reply route.
Cancellation discards local staging/jobs, but a submitted GET or POST can still
have a remote effect. Inspect status/readback before repeating it.
Exactly matching firmware-owned bundled Lua bytes have a default one-second
recovery-load allowance; other Lua sources and Wasm retain the 330 ms load
budget. Explicit nondefault loader limits remain authoritative.
WAMR does not guard the native task stack; measure task headroom and internal
heap/largest block alongside the separate PSRAM pool and linear memory.
See [resource accounting](../esp32/resource-budget.md) for pinned build measurements.

### Field validation procedure

Keep the operator profile, shared PHY, identities, authentication and
credentials unchanged. Use an actual radio-specific app-only image, not a
compile-only image with fixture credentials. The sealed in-memory operator
header is included by the existing `Xiao_S3_WIO_onchip_https_probe` profile;
the plain HTTPS profile does not consume `ONCHIP_OPERATOR_HEADER`.
Do not provision/erase SPIFFS: it destroys retained role/program files and
durable bot data. Do not erase NVS identities either.
Use `.tmp/wasm-examples/commands.json` to identify the exact compiler inputs.
Prepare and inspect packages without a connection:

```sh
make -C firmware/esp32 bot-wasm-examples
python3 tools/hardware/admin.py package create \
  .tmp/wasm-examples/c-notes.wasm .tmp/wasm-notes.package \
  --name wasm-notes --version 1.0.0 --capability cmdmeta --capability kv
python3 tools/hardware/admin.py package inspect .tmp/wasm-notes.package
```

Within the assigned radio window, use the existing configured
`MAST_CLI_ARGS` connection/authentication flags with these commands. Those
flags must select the configured Management identity and private key/password
files, never inline secrets. For example, with those flags supplied through
Make's existing `MAST_CLI_ARGS` variable:

```sh
make -C firmware/esp32 mast-cli ARGS="$MAST_CLI_ARGS --runtime wasm package-install .tmp/wasm-notes.package"
make -C firmware/esp32 mast-cli ARGS="$MAST_CLI_ARGS --runtime wasm status"
make -C firmware/esp32 mast-cli ARGS="$MAST_CLI_ARGS --runtime wasm download .tmp/wasm-radio.package"
cmp .tmp/wasm-notes.package .tmp/wasm-radio.package
make -C firmware/esp32 mast-cli ARGS="$MAST_CLI_ARGS --runtime wasm command 'source hash'"
make -C firmware/esp32 mast-cli ARGS="$MAST_CLI_ARGS --runtime wasm rollback"
make -C firmware/esp32 mast-cli ARGS="$MAST_CLI_ARGS --runtime wasm command 'source retry'"
make -C firmware/esp32 mast-cli ARGS="$MAST_CLI_ARGS --runtime wasm remove"
```

`rollback` requires a previous retained selection. `remove` stops that
runtime and selects no program; it retains its journal and durable KV.
An install with an uncertain response requires `status`/`hash` readback,
not blind replay. After live recovery fails, choose `source wasm retry` for
Wasm or `source retry` for Lua. The other runtime's retry is refused while a
publication/recovery lease is held. A reboot interrupts pending jobs; record
their queued/confirmed/unknown outcomes before requesting it.

1. Package `c-arithmetic.wasm`, `c-notes.wasm` and `c-rpc.wasm` using the
   `package create` command above with distinct names. Install one at a
   time through authenticated RF `package-install`; use fresh arithmetic
   arguments, a unique `!wnote` marker, `!wnote` and configured `!wecho text`. Confirm each
   reply arrives over RF on the companion, alongside an unchanged Lua
   command and native `!ping`.
2. Interrupt a numbered Wasm upload, repeat the same upload ID/digest,
   compare `--runtime wasm download` bytes with the original package,
   install, reboot and read the stored note. Update/rollback/remove Wasm
   while retaining the Lua source hash and note data; repeat for Lua.
3. Install `fault-0.wasm` and invoke `!wfault` for runaway interruption;
   verify Lua and native management respond afterward. Run the remaining
   `fault-1..8.wasm` fixtures for handles, ranges, recursion, growth,
   native-call and accumulated-I/O/instruction bounds. `fault-9.wasm`
   must be rejected at initialization without changing the active hashes.
4. Exercise runtime-selected authenticated web binary upload/download and
   configured TLS fetch, including offline/cancel/unknown results. Confirm
   no automatic request replay. Compare native grant-off behavior and
   radio queued/uncertain/confirmed outcomes against the host fixtures.
5. Record serial phase times, `Wasm pool` metrics, worker stack high water
   and free internal/PSRAM before loading, after loading and with mixed
   Wi-Fi/TLS/RF roles running. Pool/session figures from the host are not
   physical measurements. Also run the disabled image: discovery must
   advertise Lua only, Lua data must survive, and Wasm requests must be
   explicitly rejected without interpreter allocations.
