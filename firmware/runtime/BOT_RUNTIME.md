# Lua bot usage and runtime reference

Send commands to an independently addressed bot on a standalone ESP32, Pine
nRF52840, or a Go host running `native_lua`. They use native Lua **5.5.1** and the same bounded
command/storage/mesh API. The bot has its own identity, not the Base's or the
KISS endpoint's identity, and shares the modem's one physical scheduler.
Pine's [production setup and explicit capacities](../nrf52840/PRODUCTION-LUA.md)
cover its RF administration, storage migration and lack of WiFi/HTTPS. Limits
below use the default profile unless that platform guide specifies otherwise.

Start a bot using [ESP32 image selection](../esp32/README.md#choose-an-image) or
[host native Lua setup](../../HOST_GUIDE.md#host-status).
Use beta/HTTPS on ESP32 for durable source installation and rollback.
The core-only `Xiao_S3_WIO_onchip_bot` has RAM-only custom activation and returns
to bundled handlers after restart; its bot key/selection still persist.
The dashboard labels this role **command-bot** and the modem service **KISS**.

Wasm can be compiled alongside Lua on ESP32-S3 and the native host.
See [portable Wasm packages and build flags](WASM_RUNTIME.md).

## Try the bot

Have the sending companion radio advertise so the bot learns its contact,
then send the bot a private DM:

```text
!ping
!help
!help remember
!remember shopping get tea
!recall shopping
!forget shopping
```

Expect `Pong`, permission-filtered command help, a committed note, then its
text. Deletion is deliberate; omit the last command if you want to keep it.
Notes bind to the full authenticated user key and bot key, not display names
or command-function names. `!remember` replaces an existing note at the same
key; `!forget` deletes it. `!plugins` describes the current source generation;
it does not mean separate independently installed plugins.
On an owner-selected channel, use `!@BOTKEY8 COMMAND` (or the full bot key)
for custom/stateful commands. A channel nickname is not a private user identity.

Ordinary command cooldown is **zero**. A busy worker, full queue, exhausted
airtime or storage admission can still refuse work. Configured network calls
have their own two-per-caller/four-global-per-minute admission, unrelated to
command cooldown. `!status`, `!signal` and `!air` inspect readiness, actual
RF metadata and airtime; a TX counter does not show recipient delivery.

## Opt-in adaptive airtime admission

An operator can moderate bot work when the **local shared modem** is busy.
On a device with authenticated mast administration, send `bot adaptive on`,
then reboot through the existing owner workflow. On a native Go host, use its
private owner socket with `adaptive on`, then restart the host service.
`bot adaptive` (host: `adaptive`) reads saved/live selection, measurement
availability, congestion, local load and the current allowance. Disable with
`bot adaptive off` (host: `adaptive off`) and restart. Missing settings default
to off; the existing static bot policy remains unchanged when disabled.
This setting does not change identities, channels, permissions or radio tuning.
An unreadable adaptive setting keeps static admission running and reports
`policy-fault=1` in `bot adaptive` (host: `adaptive`). The fault remains visible
until an owner saves `off` or `on` with successful readback. Use `off`, inspect
the saved selection, then restart; only this setting is overwritten, not keys
or notes. A failed repair retains the warning.
The non-Lua nRF native bot accepts the same `bot adaptive [on|off]` command over local
USB or the repeater's authenticated administration. Its separate InternalFS
flag also applies after reboot; missing settings default to off. The nRF
native bot uses a 3,600 ms/min reference allowance while congested and retains
its one-reply-awaiting-ACK limit. Its admission denial leaves notes unchanged.
Its load sample includes driver-active TX intervals even when completion is
uncertain; confirmed RF totals remain separate.

With the setting enabled:

- The bot samples aggregate physical TX duration and estimates RX airtime from
  actual received packet lengths and the modem's current PHY airtime lookup.
  Local reflection adds no RX airtime. One bot adapter counts each reception
  once, rather than adding repeater, room and companion copies. RX estimates
  include adapter-buffer drops; host transport losses before the worker are
  not visible. Undecodable energy and other nodes' queues are not measured.
- One-second samples use a quarter-weight EWMA. Congestion enters at 600
  permille observed local occupancy or four queued physical jobs. It leaves
  only after three samples at or below 300 permille with at most one queued
  job. Overlapping RX/TX estimates are capped at 1000 permille; this is a
  local admission signal, **not network-wide channel utilization**.
- During congestion the bot allowance is 25–50% of its configured
  `airtime-ms/min`. A bounded leaky airtime ledger divides this allowance
  equally among recently demanding authenticated DM callers and verified
  channels. All nicknames on one channel share its principal. A caller may
  borrow one packet quantum from unused aggregate allowance when its share
  is smaller than a packet; it cannot borrow a second. Eight principal slots
  expire after 60 seconds without demand and without pending TX. Extra
  low-load callers share one overflow accounting bucket; new principals are
  refused if those slots are full during congestion.
  Waiting callers without airtime debt do not consume refill: the full
  aggregate allowance drains debt, redistributing unused shares.
- New work requires headroom for the PHY-estimated smallest encrypted reply.
  Congestion admits at most one or two active work slots, at most one per
  competing caller. Actual bot replies, mesh sends, traces, script adverts
  and reminders also require estimated airtime admission. Native ACKs,
  discovery replies, startup/owner adverts and other roles keep their existing
  rules; their actual TX still contributes to the local aggregate measurement.
  All traffic retains the physical queue and airtime ceilings.
- The four packet reservations settle only on final TX results. Known
  pre-RF rejection/failure releases the reservation; measured RF replaces the
  estimate. Unknown or lost completion conservatively charges the estimate.
  Cancellation does not pretend an admitted packet was unsent. Ledger debt
  decays and unused caller slots expire; no retry job or sleep is scheduled.
  Restart clears volatile measurements/debt, not the saved opt-in setting.

`Not run: adaptive ...` means the command did not enter the VM. An outgoing
operation can instead fail with `adaptive radio congestion` or
`adaptive caller airtime share`; do not treat it as queued or transmitted.
An ESP/host script may have committed local state before its outgoing operation
is refused; inspect that state before repeating a mutation.
Inspect `bot adaptive`, reduce offered bot work, and try one explicit command
after congestion clears. RF notices use the existing one-per-minute notice
budget and may be suppressed when airtime is exhausted. Missing/stale host
radio snapshots refuse adaptive work with `adaptive radio measurements
unavailable`; inspect the host–modem connection rather than retrying repeatedly.
There is no new ordinary command cooldown.

For a short authorized field check, first record `bot adaptive`/`!air` and
exchange two low-load `!ping` commands. Enable and restart, then offer a small
bounded burst from two authenticated callers while another role is sending
normal traffic. Inspect local load, `denied` and `pending`; observe admission
reducing before the physical queue grows unbounded. Stop the burst, wait for
quiet samples, and send one `!ping` from each caller. Confirm `congested=0`,
`pending=0` and both replies. Distinguish terminal TX from DM ACK/delivery.
Do not manufacture RF interference or change radio profiles for this check.

Developer checks:

```sh
make -C firmware/esp32 bot-adaptive-test ONCHIP_BOT_WASM=0
make -C firmware/esp32 bot-adaptive-native-test ONCHIP_BOT_WASM=0
make -C firmware/esp32 bot-adaptive-admin-test ONCHIP_BOT_WASM=1
make -C firmware/esp32 local-test ONCHIP_BOT_WASM=0 -o python-test
make -C firmware/nrf52840 adaptive-arm-check native-test
```

The first two commands check bounded controller decisions and encrypted native
command admission/recovery. `bot-adaptive-admin-test` checks encrypted owner
selection and corrupt-record repair without running the broader admin suite.
`local-test -o python-test` checks the physical
arbiter and reflection counters without the separate Lua/Wasm package suite.
The nRF native target also checks owner persistence, denial before note
mutation, and final bot-packet accounting while companion messages compete.
Build the public images with `make -C firmware/esp32 beta-build
ONCHIP_BOT_WASM=0` and `make -C firmware/nrf52840 build ENV=nrfmast_fleet`;
neither command flashes hardware.

The controller has no worker thread, dynamically growing caller registry,
network transport or alternate scheduler. Its adapter needs
`receivedAirtimeMs()` and the existing fresh queued-radio snapshot; measurement
and ledger arithmetic use bounded unsigned counter differences across rollover.
The nRF shared-radio adapter exposes cumulative PHY-estimated RX,
confirmed RF TX, and radio-active duration (including unconfirmed timeout
intervals), sampled at the physical driver boundary before role fanout.
`make -C firmware/nrf52840 adaptive-arm-check` compiles the controller for
Cortex-M4; the nRF native tests exercise the same load/fairness/recovery logic
using those driver signals. The small native nRF bot uses the same controller
before command side effects and settles its reservation by the actual native
packet's final callback, not an unrelated companion message or an ACK. A failed
native send whose RF start cannot be distinguished conservatively charges the
estimate. It has no separate TX scheduler.

## Find a task or contract

- **Author/test a command:** [local package/replay workflow](BOT_DEVELOPMENT.md).
- **Look up a function:** [API by capability](#lua-api-by-capability),
  [context and authorization](#context-and-authorization),
  [execution/lifecycle limits](#execution-and-lifecycle).
- **Manage source/grants:** [administrator workflow](../esp32/MAST_ADMIN.md#administrator-workflow),
  [source installation/recovery](../esp32/MAST_ADMIN.md#source-and-help-installation).
- **Understand storage:** [scopes](#scope-grants), [KV and timers](#working-retained-state-and-timer-apis),
  [atomic operations](#atomic-kv-and-owner-data-snapshots),
  [private reminders](#reboot-autonomous-personal-dm-reminders).
- **Use the radio:** [mesh continuations](#native-mesh-continuations),
  [pair forwarding](#explicit-pair-dm-forwarding),
  [native discovery](#native-app-ping-and-path-discovery).
- **Use the network:** [approved HTTPS/JSON/RPC configuration](NETWORK_API.md).
- **Extend native integration:** [management hooks](#hooks-for-the-shared-authenticated-rfweb-backend),
  [mast wire contract](../esp32/MAST_BETA_PROTOCOL_V1.md),
  [host worker contract](../../internal/nativebot/PROTOCOL.md).

The tables below describe available functions and their limits. Enable the
required build features, configuration and owner grants on the installed node.

## Lua API by capability

Square brackets indicate optional arguments. Native read-only results cannot
be modified. Table options reject unsupported fields. Bounded text is in bytes.
Unless noted otherwise, authorization/type/limit/provider errors raise a Lua
command error; do not treat an exception or an unknown outcome as success.

### Commands, modules and events

| API | Result / use | Authorization and limits |
| --- | --- | --- |
| `function hello(name) ... end` | Automatically exports `!hello`; inferred arguments are required bounded string tokens | Eight custom commands; local and `_` helpers are not auto-exported |
| `command(name, schema, help[, export[, permission[, example]]])` | Declares typed arguments, alias/help/permission metadata; no operation result | Initialization only; four arguments, 96-byte schema; declarations restrict admission, never grant authority |
| `module(name, loader)` | Declares a package-local loader returning a table | Initialization only; eight modules within the same 4096-byte source |
| `require(name)` | Cached module table, shared within the source generation | Declared modules only; no filesystem/library search; cycles or loader I/O reject staging |
| `events.on(kind, function_name)` | Registers one named handler for each supported kind | Initialization; `startup`, `connectivity`, `message`, `node_status`; owner event-mask grant |
| `events.off(kind)` | `true` if previously subscribed, otherwise `false`; unsubscribes and fences pending events | Invocation only; until reload; does not change saved owner grant or replay events |

See [declaration/schema syntax](#restricted-handler-api),
[metadata and rollback schemas](#compact-package-metadata-and-compatible-schema-changes),
[modules](#declared-package-local-modules) and [events](#granted-event-subscriptions).

### Scoped durable state

All operations yield to storage. The optional scope is `caller` by default,
or `conversation`, `bot`, `channel`; see [authorization](#context-and-authorization).

| API | Successful result | Limits / failure behavior |
| --- | --- | --- |
| `kv.get(key[, scope])` | `value_or_nil, rf_safe` | Key 1..32 B; absent is `nil`, not an error |
| `kv.put(key, value[, scope])` | `true, "created"` or `true, "replaced"` | Value 1..256 B; commit and readback complete before success |
| `kv.delete(key[, scope])` | `true, "deleted"` or `true, "absent"` | Unknown commit is an error; do not retry blindly |
| `kv.list([prefix[, scope]])` | `{count, keys, suffixes, truncated=false, rf_safe}` | Sorted complete scope snapshot; ≤8 keys; pass `nil` prefix to select scope |
| `kv.cas(key, expected, value[, scope])` | `{ok, status, error}` | `false` means expected absence / requested deletion; statuses `committed`, `conflict`, `unknown`, `rejected` |
| `kv.transaction(operations[, scope])` | Same atomic-result shape as CAS | 1..4 distinct `{key, value[, expect]}` entries; one scope/principal; no partial publication |

Strings cannot contain NUL. Eight keys per scope/principal; caller,
conversation and channel share 32 public slots, with eight bot-global reserve
slots. A `conflict` writes nothing. For `unknown`, read back affected keys
before making a new explicit decision. See [atomic semantics](#atomic-kv-and-owner-data-snapshots)
and [file storage/upgrade](#kv-storage-and-upgrade), including downgrade warnings.

### Sleep, deadlines and reminders

| API | Result | Requirements / lifecycle |
| --- | --- | --- |
| `sleep(ms)` / `timer.sleep(ms)` | `true` after resumption | 1..30000 ms; transient coroutine sleep, lost at restart |
| `timer.set(name, seconds[, scope])` | `{state, deadline_utc, revision, replaced, time_trusted}` | Name 1..32 B, delay 1..86400 s; trusted time; two pending names per scope/principal |
| `timer.get(name[, scope])` | Same timer view; absent has `state="missing"` | Available without trusted time; reports `time_trusted=false` |
| `timer.cancel(name[, scope])` | Same timer view | Cannot undo a consumed claim or RF effect |
| `timer.wait(name[, scope])` | Timer view after a durable one-consumer claim | Yields; does not restore a Lua stack or send automatically after reboot |
| `reminder.after(duration, text)` | `{id, state="pending", deadline_utc}` | Authenticated private DM; owner reminder grant, trusted time; 1 s..24 h, text 1..120 B |
| `reminder.list()` | Bounded text summary or `No personal reminders` | Own caller records only; usable with scheduling grant off |
| `reminder.cancel(id)` | `{id, state, deadline_utc}` | Cancels pending work, not an unknown/sent claim |

Named timers and message-bearing personal reminders are separate services.
Reminders have eight total slots, at most two pending per caller. `sent` means
radio TX completed, **not ACK or recipient delivery**. Reboot requires a fresh
authenticated route; consumed/unknown claims are not replayed.
See [deadline semantics](#named-persistent-deadlines)
and [reminder route/time rules](#reboot-autonomous-personal-dm-reminders).

### Replies and native mesh operations

| API | Result | Authorization / bounds |
| --- | --- | --- |
| `return "text"` | Native final reply submission | One final reply by default; 1..162 printable ASCII B, possibly narrower channel limit |
| `reply(text)` | TX result `{queued, transmitted, acknowledged, job}` after completion | One explicit reply per invocation; yielding TX, not delivery |
| `mesh.compose{kind="dm", text=...[, to=...]}` | Opaque invocation-bound draft | Caller by default or up to four owner-granted full-key destinations |
| `mesh.compose{kind="channel", text=...[, to=...]}` | Opaque draft | Explicitly targeted, verified selected channel only |
| `mesh.compose{kind="trace"[, route="WIDTH:HEX"]}` | Opaque direct TRACE draft | Valid inferred/explicit native route; no raw bytes |
| `mesh.send(draft)` | Same terminal TX-result shape as `reply` | Single submission; no automatic retry; granted peers need fresh direct routes |
| `mesh.wait{kind=..., timeout_ms=...[, filters...]}` | ACK/TRACE result or immutable received text/channel handle | 1..30000 ms; `ack`, `text`, `channel`, `trace`; channel wait separately granted |
| `mesh.forward(packet)` | Terminal TX result | One separately granted sender/destination pair; latest eligible follow-up DM, re-encrypted as the bot |
| `request_trace([width_hex])`, `mesh.trace([width_hex])`, `trace([width_hex])` | Bounded native TRACE-result text | Direct route; explicit widths 1/2/4/8, not ordinary width 3 |
| `mesh.multitrace([width_hex[, count]])` | Bounded numbered text summary | Count 1..3, default 2; sequential yielding operations share invocation budget |
| `mesh.advert()`, `advert()` | Bounded completion text | Actual advert TX completion; 15-minute public advert gate and shared airtime |

Text filters are `prefix` **or** `exact` (1..32 B), `from`, `path_width` and
`route`. `from` is the caller or an owner-granted peer already sent to in this
invocation; a channel wait cannot select an authenticated individual.
Four copied messages and 16 dedup entries bound each invocation.
ACK/TRACE correlation is not cryptographic sender authentication.
See [full options/results](#native-mesh-continuations) and
[forwarding eligibility](#explicit-pair-dm-forwarding).

### Approved HTTP, JSON and RPC

| API | Result | Boundaries |
| --- | --- | --- |
| `http.get(alias)` | `{ok, http_status, submitted, body[, error]}` | Exact owner-configured endpoint; authenticated private DM and network grant |
| `http.post(alias, body)` | Same HTTP result | JSON request ≤1024 B; same context/grant |
| `rpc.call(service, operation, args)` | `{ok, http_status, submitted, result[, error]}` | Named approved operation; no caller-supplied URL/header/credential |
| `rpc.text(text, limit)` | RF-safe escaped printable text, with explicit truncation suffix | Text 1..256 B; limit 16..`ctx.limits.reply_bytes`; does not make a whole JSON result RF-safe |
| `json.encode(value)` | JSON string | Rejects cycles/nonfinite numbers/invalid UTF-8 and oversized values |
| `json.decode(text)` | Decoded value/table | Each string value/object key ≤1024 UTF-8 B; strict parse, including duplicate-key rejection |
| `json.array([table])`, `json.null` | Array marker/table and JSON-null sentinel | Preserves empty arrays and JSON null |

Network failures return `error.code` and `error.message`; argument/authorization
errors can raise. `submitted=true` means a request may have reached the service,
not confirmed effect. There is no automatic retry of uncertain requests.
The [network guide](NETWORK_API.md#http-json-and-named-rpc) is the canonical
current configuration/result/transport-limit reference.

ESP32 HTTPS requires fresh accepted SNTP; native Linux HTTPS requires the
kernel synchronization flag as well as a valid wall-clock date. Check native
`home` status through the protected owner socket when network admission fails.
Four endpoint slots are shared by bot calls and package fetch; named RPC can
reuse `home` instead of allocating a second RPC endpoint.

### Utilities and diagnostics

| API | Result | Limits / authority |
| --- | --- | --- |
| `utility.calc(expression)` | RF-safe result text | ≤120 B expression; bounded arithmetic parser |
| `utility.convert(value, from_unit, to_unit)` | RF-safe result text | Value ≤32 B, units ≤4 B; explicit supported unit families |
| `utility.roll([notation])` | RF-safe result text | Default `1d6`; bounded dice notation |
| `utility.choose(options)` | RF-safe choice text | Pipe-separated nonempty options, ≤148 B |
| `node.report(name[, page])` | Read-only diagnostic text | `about`, `version`, `uptime`, `status`, `signal`, `air`, `mt`; only air has pages 1..4 |
| `node.plugins([page])` | Manifest/module summary text | Current source generation, not per-package installation |
| `node.neighbors([page])` | Yielded bounded observation text | Pages 1..16; cached signed contacts/routes, not live adjacency |
| `node.admin(command)` | Yielded bounded backend result text | Authenticated trusted-owner private DM only; limited bot admin operations |
| `command_help([name[, page]])` | Permission-filtered bounded help text | Name or numeric page text; invalid pages raise |
| `tostring(number)` | Native numeric text | Signed integers or finite numbers with magnitude ≤1e9; not Lua's general-purpose `tostring` |
| `ping()`, `test()`, `path()`, `mt()`, `help(...)` | Composable bundled diagnostic text | Current invocation context; nested calls share quotas and do not independently reply |

Utilities require authenticated DM or verified selected-channel context and
allow eight calls per invocation. Unsupported arguments are errors.
See [utility syntax/units](#bundled-utility-commands),
[diagnostics](#read-only-diagnostics) and
[discovery/owner commands](#discovery-targeting-and-owner-commands).

## Context and authorization

`ctx` is a read-only coroutine-local native view, not a mutable global caller.
Cooperating functions/modules retain their invoking job's principal, route,
grants and limits across nested calls and yields. Initialization has no caller
or native I/O authority. See the [field reference](#restricted-handler-api)
for `sender`, `packet`, `radio`, `node`, `air`, `observation` and `limits`;
channel requests additionally provide verified `ctx.channel` and immutable
`ctx.grants`. Check availability/measured flags before interpreting telemetry.

| Context / scope | What it can authorize |
| --- | --- |
| Authenticated private DM | Caller and conversation storage, caller reply/send/waits; reminders/network only with their owner grants |
| `caller` (default) | Full bot key + authenticated sender key; shared across cooperating functions, private from other callers |
| `conversation` | Separate bot/caller DM scope; not arbitrary script-selected threads |
| `bot` | Bot-global durable KV/timers, only with explicit shared-state grant |
| Verified selected channel | Channel scope by full channel-key digest, only with shared-state grant; all members can read/write it |
| Owner-granted destination | Additional full-key DM destination with a fresh authenticated direct route; not an owner identity or arbitrary routing grant |
| Event handler | Granted bot-global or verified channel storage/timers plus sleep; no private caller state, RF replies/sends, reminders or network calls |
| Local reflection / nickname | No authenticated private-user or owner authority; RF signal is not fabricated |

Package capability declarations describe requirements; they grant nothing.
`bot shared`, `bot reminders`, `bot home`, `bot events`, destinations,
channel waits and forwarding are separate owner controls. Revocation fences
pending work immediately; failed persistence may leave the old saved grant
for reboot. Confirm saved/live readback. Regrant never revives an old job.

## Execution and lifecycle

There is **one active cooperative Lua source generation**, not mutually hostile
plugins. Functions/modules share globals and cached tables intentionally.
Installation or rollback replaces the whole custom set; quarantine affects
custom handlers, not one independent package. Native diagnostics and mast
recovery remain separate. Source is text-only, at most **4096 B including its
metadata**, with eight custom commands and eight declared modules.

Four job slots include one reserved native diagnostic slot. Each invocation
has at most eight native I/O operations, a 10,000-instruction bound and 20 ms
cumulative active execution budget across resumes. The source generation has a 96 KiB allocator budget
and a 16,384-step parser bound; installed-source loading is limited to 330 ms
and initialization to 50 ms. Exactly matching firmware-owned bundled Lua
bytes have a one-second recovery-load allowance when using the default loader
limits. Explicit nondefault loader limits remain authoritative; Wasm retains
its 330 ms load budget.
Suspended storage/radio/network/timer time is separate
from active execution. Native providers enforce their own deadlines and token
fences; they do not block radio dispatch or create unlimited Lua tasks.
There is no `io`, `os`, `debug`, arbitrary socket/filesystem/native module
loading or unrestricted dynamic code-loading API.

Source replacement, bot stop and restart cancel coroutines/waits and discard
late completions. They retain scoped durable data and consumed claims, not Lua
heap objects. A mutation already admitted may have committed, and a packet
already queued/on air cannot be undone. Treat an unknown result as uncertain;
inspect state rather than automatically repeating an effect.

Named deadlines do not restart a Lua invocation after reboot. Personal reminder
records can dispatch without a surviving invocation, but only through their
native time/route/grant gates. Scoped scheduler backup restoration is
**no-rearm**, never automatic replay. Source rollback requires compatible data
schemas and does not rewind durable data.

Per-function hostile sandboxes, independent multipackage deployment/quarantine,
recurring/channel reminders and arbitrary user/thread grants are **not
available APIs**. Production nRF Lua uses the
[Pine profile's smaller capacities](../nrf52840/PRODUCTION-LUA.md).
Wasm uses a [separate versioned ABI](WASM_RUNTIME.md#abi-v1) alongside Lua.

## Build and selection

Run from the repository root:

```sh
make -C firmware/esp32 bot-test
make -C firmware/esp32 bot-runtime-test
make -C firmware/esp32 bot-build-test
```

The first target runs the Lua and native packet tests. The second runs the
actual combined runtime with repeater, room, companion, management, command
bot and a KISS client. The third compiles bot-enabled and baseline firmware
using the tracked public example profile in `.tmp/onchip-bot-firmware`; it
does not upload anything or read a private operator profile. All generated
Lua/build/test artifacts are under repository `.tmp/onchip-*`.

For an operator build, use `bot-firmware` with explicit `BUILD`, `CONFIG` and
`ENV=Xiao_S3_WIO_onchip_bot`. `bot-prepare` adds checked Lua and WAMR sources to the
disposable tree; ordinary `firmware` preparation alone does not install them.
The example bot environment opts in with `MESHCORE_ONCHIP_BOT=1`,
`KISS_LOCAL_SOURCES=5` and `ONCHIP_COMMAND_BOT_DEFAULT_ENABLED=1`.
Without that environment, the baseline image does not contain an active bot.

`loadBotEnabled()` / `saveBotEnabled(bool)` read/write the checked, single-key
`mc-onchip/command-select` record. Saving changes the **next boot**, not the
running selection. Missing storage uses the build default (otherwise off);
corruption or I/O failure refuses bot startup. These are trusted native
backend hooks, not network management routes. Existing v1/v2 role-profile
journals, masks, signed RF status and receipt formats are unchanged. A bot
startup/allocation fault leaves existing services and KISS usable.

## Messages and bundled handlers

The enabled bot sends a signed native flood chat advert after its bundled
source validates, so ordinary repeaters can relay its discovery. Another
advert can be requested through `mesh.advert()` or `advert()`, at most once per 15 minutes,
subject to the existing bot airtime budget; this is not a periodic timer.
A native relay regression connects peer and bot only through the relay,
verifies signed discovery and encrypted `Pong` replies over 1/2/3-byte-hash
routes, and preserves the advert rate limit. A peer must first send its own
valid signed MeshCore advert; there are 16 bounded contact slots. Plain native
text DMs addressed to the bot are decrypted using the pinned MeshCore ECDH/
MAC implementation. Invalid or unrecognized packet types never enter a Lua
handler. No new anonymous/admin protocol is used.

### Native app Ping and path discovery

To allow a companion radio to discover a route to the **command-bot** contact,
enable base telemetry explicitly through authenticated mast management:

```text
bot discovery
bot discovery on
bot discovery
```

Expect `saved=1 live=1`. `bot discovery off` stops admitting these requests
immediately and persists across restart; an already queued reply may transmit.
A missing setting defaults to **off**; upgrading
does not grant access. A storage failure disables live access and reports an
uncertain saved outcome; inspect the setting before restarting. The bot must
have received the requesting radio's signed advert and be running. The
16-contact cache and learned routes are RAM-only and must be rebuilt after
restart.

Enabling this setting permits **all signed-advert contacts** to request base
telemetry using their authenticated native encrypted datagrams. It does not
make those contacts owners, grant Lua capabilities, change management trust,
or modify any companion/repeater/room ACL. The on-device reply includes board
voltage and MCU temperature when available. Location and environment sensors
are never queried, even if requested. The host native bot uses the same
permission and routing handler but returns the request tag and zero padding
without fabricated sensor readings. Native replies are limited to one per
second across contacts and share the saved bot airtime budget and packet pool.
Denied, malformed, unknown and locally reflected requests receive no reply.
The app may show a timeout; native telemetry has no permission-error response.
Expected permission, unsupported-request, readiness and capacity denials only
increment the bot's rejected counter; they do not replace its fault status or
produce repeated logs. Malformed packets still use the normal fault diagnostics.

The companion's path-discovery command and ordinary telemetry requests are
distinct from sending the text command `!ping`. This handler supports native
telemetry type **3**; it does not implement repeater status type **1**, login,
or TRACE reflection on the bot's chat identity. A particular app's “Ping”
button can select a different operation; use the contact's role and the
actual request type when diagnosing it.

Other role permissions remain independent:

| Contact | Native request behavior |
| --- | --- |
| Command bot, on-device or host | Type 3 requires `discovery on`; text `!ping` is unchanged |
| Companion / Base | Its native telemetry base/location/environment modes and contact flags apply; base denied means path discovery is denied too |
| Management | Type 1 status and type 3 telemetry require its existing authenticated login/session |
| Repeater / Relay and Room | Their existing login and role-local ACLs control requests; the bot setting grants nothing |

For example, a Base configured with all three telemetry modes `0` continues
to deny requests. Do not change those modes merely to make an app indicator
green. Host bot control uses `discovery [status|on|off]` over its private owner
socket; see [the host process contract](../../internal/nativebot/PROTOCOL.md).

#### Native wire and routing

The reference is local upstream `companion-v1.17.1`,
`d92964352441e53b93e8667b802e04f6e072b39e`:
`examples/companion_radio/MyMesh.cpp` (`CMD_SEND_PATH_DISCOVERY_REQ`,
`onContactRequest`, `onContactPathRecv`) and
`src/helpers/BaseChatMesh.cpp` (`sendRequest`, `onPeerDataRecv`,
`onContactPathRecv`). Companion command **52**, reserved byte `0`, and the
32-byte destination key sends a **flood** `PAYLOAD_TYPE_REQ`. Its encrypted
plaintext is:

```text
tag:u32le | type:03 | inverse-permissions:fe | reserved:00 00 00 | random:4 bytes
```

Native AES zero-pads those 13 bytes to 16. Ordinary telemetry can use inverse
mask `00`; excluding BASE explicitly still denies the response. The bounded
bot handler accepts one decrypted block, requires the three reserved bytes
and trailing padding bytes 13..15 to be zero, and ignores the four random
bytes. Short native binary telemetry forms have indistinguishable zero
padding and follow the same mask rule. Other request types or extensions
receive no response.

A flood request receives upstream `createPathReturn`: the request's encoded
1/2/3-byte-hash path, `PAYLOAD_TYPE_RESPONSE`, and the echoed tag plus Cayenne
LPP base readings. The response itself floods using `ReplyRouting` scope
selection and the request's path width. Direct requests receive a native
RESPONSE on the learned return route, or flood when no return route is known.
No route is invented from an incoming direct packet.

For **ordinary** telemetry, BaseChatMesh learns the returned path and sends
its reciprocal PATH; the bot learns that authenticated route for subsequent
direct replies. Upstream's **special command-52 discovery callback instead
reports both paths to the app and deliberately does not learn a route or send
a reciprocal PATH**. The app decides whether to apply the result. A discovery
response alone therefore does not establish a bot return route. The host's
zero-padded empty telemetry retains more than four response bytes so that
callback also accepts replies at exact cipher-block/path boundaries.

The native harness uses real BaseChatMesh requests, native relay routing,
signature/MAC verification and cipher padding. Run `make -C firmware/esp32
bot-discovery-test` for direct/flood, scoped/unscoped 1/2/3-byte routes, reciprocal
learning, permission denial, malformed/replayed requests, maximum paths and
rate/airtime limits. `bot-discovery-board-test` exercises on-device board
readings and the authenticated management setting; `beta-test` includes
these cases in the full integration suite. `bot-native-worker-test` checks the
same persisted control across host process restarts. These tests do not flash
firmware or change a radio.

### Discovery, targeting and owner commands

`!help [PAGE]` lists all commands available to this invocation, alphabetically,
with real numbered continuation pages. `!help NAME [PAGE]` pages through the
complete schema, description and declared example; unavailable permissions are
reported rather than suggested. `!plugins [PAGE]` reports the active source
generation, bundled/custom command count, declared modules and subscription
mask. This is the worker's runtime/source-fence generation, which resets at
boot; owner `source status` reports the durable journal revision. Modules have
continuation pages. This is one cooperating Lua namespace,
not a separate VM or private storage namespace per command.

`!neighbors [1..16]` pages through actual cached signed adverts: display name,
public-key prefix, advert observation age, received path width/hops, cached
return-route width/hops and last-hop RF measurement when available. These are
observations, not a live adjacency scan or a claim that the cached route still
works. `node.neighbors(page)` yields a bounded dispatcher-produced report;
the VM never reads the live contact cache. `node.plugins(page)` returns the
copied active manifest metadata.

Use `!@BOTKEY8 COMMAND arguments` to select one bot on a configured channel.
`BOTKEY8` is the first eight hex digits of its public key; a full 64-hex key is
also accepted. Targeting is routing selection, not owner authentication.
Custom commands, board operations, TRACE and state-changing channel work
require a target; DMs already select a bot. Other bots silently ignore a target
that does not match. Incoming ordinary path widths 1/2/3 remain interoperable;
configuration mode 2 represents **three-byte** hashes.

Untargeted native read queries (`ping`, `help`, `plugins`, `neighbors`, `about`,
`version`, `uptime`, `status`, `signal`, `path`, `air`, `test`, `mt`, `calc`,
`convert`, `roll`, `choose`) wait a random **250..1250 ms** before invocation,
in addition to an `mt` collection window. Replies include `[q:XXXXXXXX]`,
derived from the same native channel request digest. Hearing that marker
cancels a matching not-yet-run read query. This is bounded, best-effort,
**unauthenticated channel suppression**: a channel member can suppress a
reply, and simultaneous replies remain possible. Explicit targets bypass it.
It never elects a writer or deduplicates state changes across bots. Pending
queries reserve ordinary worker capacity and retain normal airtime limits.

`!admin bot help` works only in an authenticated private DM from a full key
already trusted by native mast administration. It reuses that backend and its
durable timestamp replay fence; a nickname, channel key or Lua declaration
cannot create owner authority. Useful commands include `!admin bot status`,
`bot stats`, `bot log` (current fault/state, not a historical log), `bot limits`,
`bot admission`, `bot mesh`, `bot contention`, `bot cancel`, `source status`,
`roles MASK` and `reboot`. Prefix each with `!admin`. Reboot/apply effects remain
gated on actual acceptance-reply TX, not the inbound DM ACK or queue admission.
Cancellation stops other active commands/events and delayed collectors; an
already admitted native effect can have committed, and autonomous reminders
are unchanged. Credentials and bulk source/data transfers stay on the existing
management CLI/web transport. The management identity remains available when
the optional command bot is disabled or cannot initialize.

Owner `bot name TEXT` saves/applies a 1..31-byte printable name without `:`,
used in channel replies, subsequent signed adverts and `ctx.node.name`.
It never changes the bot key, roles, PHY or stored user records.
`bot mesh` reports applied policy; `bot limits`, `bot contention`, `bot policy`
and `bot admission` expose the fixed worker/rate bounds and saved radio policy.
Names and the mesh grants below share one checked 166-byte `bot-mesh` NVS
record (eight blob entries); existing records are unchanged.
The common mast `role name bot TEXT` is the same persistent policy.
`role key bot [pending|rotate]` adds deliberate native identity rotation:
generated keys are staged, public-only readback never activates them, and
restart applies them. Bot-data ownership remains the original full identity;
no automatic migration, erasure or reminder replay occurs. See
[runtime role configuration](../esp32/MAST_ADMIN.md#runtime-role-names-and-keys) for
all-role controls, active-role requirements and key-rotation consequences.

### Optional hashtag command channel

The same authenticated native mast backend supports `bot channel #example1`,
`bot channel off`, `bot path 1|2|3`, `bot airtime 360..3600`, and `bot policy`.
These save a separate checked `mc-onchip/bot-radio` record, applied on reboot.
The installed 40-byte version-1 hashtag record remains readable; a subsequent
save writes the 57-byte version-2 record, adding an explicit-key flag and
16-byte key. Both versions occupy four NVS blob entries; existing
role/identity/replay journals are unchanged. Default policy has no
channel, one-byte flood paths and 360 ms estimated TX per minute. A narrowband
field profile can explicitly select three-byte paths and 1200 ms/minute
(2% bot airtime), rather than silently losing full-length replies/adverts.
The aggregate radio arbiter still has final admission authority.

By default a selected hashtag uses the stock derived 128-bit key. Alternatively,
owner `role channel bot 0 NAMEHEX KEY32` provisions one explicit native
128-bit PSK and display name; `KEY32` is 32 hex characters. This requires the
native management CLI/web, returns metadata/fingerprint rather than the
secret, and explicitly requires reboot. `bot channel #name` clears the
explicit PSK and selects hashtag derivation; either channel-off control
disables reception after reboot. Neither path changes the bot identity.
The native encrypted group-text format includes the `name: message` prefix.
Only the one configured channel is listened to; other Public traffic does
not invoke handlers.
Built-ins and installed named handlers use the same VM. Replies are native
group messages to that channel, with a reduced context-specific text bound.
Channel replies originate with the saved bot path width, even if the caller
used another width; `!path` still reports the actual received path.
One explicit reply or aggregate result remains the default. Group messages
have no DM ACK or authenticated individual principal: `ctx.channel.name`,
`ctx.channel.present` and `ctx.sender.nickname` describe the request, while
`ctx.sender.authenticated` is false and `ctx.sender.public_key` is absent.
Channel events cannot read private/bot KV, compose DMs or use caller packet
waits, even if the nickname is `owner` or shared KV was enabled for authenticated
DMs. Native administration remains entirely outside the command channel.
Explicitly granted channel KV binds to the verified native channel identity.
Separately granted channel follow-up waits retain channel membership only,
never authenticated individual authority.

Deduplication and observation operate on channel, timestamp and sender/text,
not a nickname-derived principal. Ordinary channel and DM commands have no
per-channel, per-sender or global count cooldown; nickname rotation grants no
authority. An admitted duplicate is still deduplicated, not executed again.
The four-job pool, bounded request history, native radio queue and saved bot
airtime budget remain in force. Rejected valid commands receive `Not run:
<gate>; retry in <N>s`
when radio capacity allows. Notices share one global 60-second throttle and
the existing packet pool and total airtime budget. One separate notice credit
tracks that airtime without stealing an ordinary ACK/reply credit bucket;
it cannot admit a second notice or bypass the total RF budget.
Notices neither acknowledge command admission
nor consume a command admission, and repeated rejection can therefore remain
silent. Busy is a suggested retry interval, not a completion promise.
Resource admission is an operational counter, not a persistent role fault.
Authenticated `bot admission` reports the last gate, remaining wait, active
jobs and airtime/busy counters. Legacy sender/channel/global fields remain
zero for stable telemetry consumers; `bot stats` reports replies, rejects, VM
failures and notice suppression. Existing saved radio/mesh policy records
have no command-cooldown field: upgrading the native runtime takes effect
without resetting role identities, channels, destinations or airtime settings.

Native and sanitizer coverage includes three-byte paths,
wrong-channel/bad-ciphertext rejection, persistence/failure, channel/DM
coroutine interleaving, bare `reply()` completion and denied private authority.
Physical field acceptance is recorded separately; these are not live-RF claims.

| Command | Result |
| --- | --- |
| `!ping` | `Pong` |
| `!test` | Native message connection, complete received flood path when available, actual RSSI/SNR; local reflection explicitly has no measured RF signal |
| `!path` | Ordinary path as `width:hex`, preserving 1-, 2- and 3-byte hash widths, e.g. `2:a1a2b1b2`; a consumed direct route is reported as unknown |
| `!mt [seconds]` | Collects copies of this sender/timestamp/text request for 1..30 seconds (default 5), then reports the actual observed `width:hex` paths (up to eight). Long replies retain whole hash segments and explicitly mark truncation; ninth paths, missing paths and RX queue overflow also mark the result truncated |
| `!trace [width:hex]` | Sends a native **direct** TRACE and awaits its correlated return for up to five seconds; reports native quarter-dB SNR samples or an explicit failure/timeout. Explicit native route widths are 1/2/4/8 bytes; width 3 is rejected. Inferred routes use only ordinary 1/2-byte paths |
| `!about` | Persistent command-bot public key and role identity |
| `!version` | Pinned upstream MeshCore revision, linked Lua version and compilation date/time; no firmware image hash |
| `!uptime` | Boot uptime snapshot, extended across the 32-bit millisecond clock rollover |
| `!status` | Bot readiness/fault flag, selected/ready role masks, WiFi connection state and explicit unavailable battery measurement |
| `!signal` | This request's measured RSSI/SNR and full-width path; local reflection and missing measurements are identified |
| `!air [1..4]` | Bounded pages of actual scheduler credit, queues, RF airtime and TX outcomes; not delivery confirmation or duty-cycle history |

### Read-only diagnostics

The six new diagnostics above use copied native metadata at request dispatch,
not Lua-maintained counters or the last packet seen by the radio. Authenticated
DMs and the configured, verified command channel use the same admission,
deduplication, rate and airtime limits as other commands. No owner grant is
needed and no private configuration, credentials, fault text or remote keys
are included. `!about` exposes only the bot's own public key. These additions
do not change the installed field image.

`!status` role masks use repeater=1, room=2, companion=4 and observer=8;
the command-bot readiness is shown separately. WiFi is reported only where
native WiFi state is available. Battery is always `unavailable`: no voltage,
charge percentage or other sensor reading is inferred.

`!air` page 1 reports bot/aggregate scheduler credit and the bot's rolling
60-second airtime reservation and remaining allowance. Page 2 reports
bot/aggregate queue sizes, aggregate transmitting state and accumulated RF
milliseconds. Page 3 reports bot/aggregate TX success/failure totals. Page 4
reports scheduler/configuration generations and the bot's minute allowance.
Reservations include failed attempts and may include the request's native ACK;
the snapshot precedes its diagnostic reply. TX success is a local radio
outcome, not remote reception. Counters are unsigned 32-bit totals that
wrap/reset; neither historic duty cycle nor a persistent airtime history is
available. Separate requests/pages can observe different snapshots.

Each response fits the native DM or reduced channel limit and printable ASCII
policy. `!signal` directs long paths to `!path` rather than truncating a hash.
Missing identity/build/scheduler metadata produces an explicit unavailable
response. `!help` omits private or grant-dependent commands unavailable in the
current context; `!help NAME` explains that restriction. Long discovery and
command details have numbered continuation pages within the same RF limit.

**Migration:** `about`, `version`, `uptime`, `status`, `signal` and `air` are
newly reserved. Rename/reinstall conflicting custom functions or command
aliases before upgrading. Eight custom exports, the 4,096-byte editable
source bound, existing VM budgets and saved state formats are unchanged.
The authenticated mast command `source api diagnostics` reports these
capabilities; it does not enable a new management transport.

### Paths, replies and admission

Without an explicit TRACE route, a nonempty received **RF flood** path of
width 1 or 2 supplies a reversed candidate. Width-three paths, local
reflections, unknown/consumed direct routes and empty paths cannot supply
that candidate. No-route TRACE returns an error; it never falls back to a
flood. TRACE emission alone is not a claim that the route worked. The retained
runtime checks the returned tag/auth token, exact route/width and hop count,
then reports up to 12 SNR samples with explicit output truncation. TRACE
correlation identifies the matching response; hop hashes remain routing data.
Explicit routes are also bounded by the native 162-byte command text limit.

Ordinary MeshCore path hashes and native TRACE widths are different encodings:
ordinary `path_hash_mode` values 0/1/2 mean 1/2/3-byte hashes, while TRACE
low-flag values 0/1/2/3 mean 1/2/4/8-byte route elements. Explicit bot TRACE
routes accept those native widths and reject width 3. Inferred bot TRACE
supports only ordinary one- and two-byte flood paths. An ordinary three-byte
flood path remains visible unchanged in `!path`, but cannot be reinterpreted or
coerced to a four-byte TRACE route.
The mast `bot path` and `bot role-path` arguments are ordinary byte counts
1..3, not `path_hash_mode` values, and do not configure TRACE. `source api
paths` reports these encodings and supported widths.

Multi-test counts complete paths, not hashes of paths, and excludes local
forwarding reflections from its RF observation count. It runs the handler
once after the window, not continuously during collection. For example,
`!mt 5` can return `2 unique paths in 5000 ms; 3:a1a2a3b1b2b3 | 3:c1c2c3d1d2d3`.
Each entry preserves the received path's 1/2/3-byte hash width and segment
order; hashes are not expanded into full public keys. `3: (no-hop)` means an
observed zero-hop flood, not a consumed direct route. With no complete flood
observation, the reply explicitly reports direct/local path information as
unknown.

When complete paths do not fit the invocation's RF reply limit, space is
shared across observed routes. `...` marks a path shortened at a whole-hash
boundary; `; truncated` marks shortened/omitted paths or incomplete collection.
The reported count is the number of distinct stored observations, not a claim
that a truncated collection contains every route. Two long paths retain
segments from both rather than returning only their count. Full captured
paths remain available to Lua in `ctx.observation.paths`.
A path too long for one `!test` response returns an error directing the sender
to `!path`.
Unknown command names and invalid schema arguments return an explicit error.
`!help [PAGE]` lists native and installed commands; `!help NAME [PAGE]` shows
the complete native argument schema, manifest help and available example.

RSSI/SNR are captured against the bot's native packet at the pre-deferral
receive hook and retained until that packet returns to its four-slot pool.
RF/local classification uses the packet's own native reflection flag.
Later RF packets or local KISS/role reflections therefore cannot overwrite
the metadata used by a deferred `!test`, `!mt` observation or inferred TRACE.
A missing signal snapshot is logged and reported as unavailable, not
replaced with the latest radio sample.

The core deduplicates requests for 120 seconds by sender, timestamp and text,
excluding retry bits/padding; a bounded 64-request cache rejects new requests
if it fills before entries expire rather than risking a duplicate execution.
There is no sender/channel command cooldown or four-command/minute gate.
Estimated bot TX retains a default 360 ms per rolling minute (explicit native
policy may select 360..3600 ms). A bounded per-second credit ledger groups
transmissions without an eight-TX count cap, conservatively retaining each
second's charges until its last TX ages out. The estimate uses the current PHY
and includes the native ACK, actual encrypted reply and route lengths.
Adverts share that budget; the public 15-minute advert interval and host
owner-only one-minute notification cooldown are separate from command
admission. The existing mux remains the final source/aggregate PHY arbiter;
failed attempts retain their reserved credit. Source/resource failures are
logged and visible in the bot's dashboard fault field, never reported as
successful RF delivery. Response counters mean submissions; the shared
dashboard retains actual queued/TX outcomes.

Admission sends the native ACK immediately, before VM execution or an `!mt`
observation window. Flood ACKs use native PATH-return framing; authenticated
PATH exchange, not reversed incoming metadata, supplies subsequent direct
reply routes. Retransmitted authenticated requests are ACKed at most once per
second without invoking the handler again, within the same airtime budget.
The ACK confirms admission, not handler success. Scoped replies use the shared
native region/route helper described in [MAST_ADMIN.md](../esp32/MAST_ADMIN.md).

Bundled initialization and transient live-source failures have three bounded
delayed retries. The mast backend retains previously initialized code during
live-load failure and reports the discrepancy from durable source explicitly;
`source retry` permits operator recovery after exhaustion. Instruction, parser,
source and allocator limits are unchanged; phase-specific wall limits below
replace the old combined 20 ms deadline.

## Restricted handler API

Lua **5.5.1** is fetched from lua.org and checked against SHA-256
`1c4b4068d67061f2a2231ad2b5422e77acea1487ea9890f6320af614f4373dce`.
Host and ESP32 builds use signed 64-bit integers and 64-bit double-precision
numbers. The VM checks the pinned version and numeric sizes at compile time,
and checks the linked Lua ABI inside each protected invocation. Native tests
also exercise integer wraparound and double-precision arithmetic. The 5.5 API
uses its explicit state seed with upstream `luaL_makeseed`; the lexer budget
hook and C-call limit remain enabled. No execution limit was increased for
the upgrade.

Source registers named exported functions. Native code validates the manifest,
looks up the command and parses its arguments before calling Lua. For example,
`!hello slepp` calls `hello("slepp")`:

```lua
function hello(name) reply('Hello '..name) end
```

That literal function is sufficient: native Lua declaration inspection
registers each exported global Lua function and reads its fixed parameter
names from the source-compiled declaration. Inferred parameters are required
`string:159` tokens (the full RF message bound still applies), parsed and
validated before calling the function. No returned table, wrapper, prelude
written by the author, or source-wide dispatch is required. Use `local function`
or a global name beginning with `_` for helpers, which are not automatically
exported. Global helpers remain callable by the other cooperating functions;
they do not consume command slots or require a wire-argument schema. Varargs
and more than four inferred command parameters are rejected; at most 64 global entries may be inspected.
Reserved names are rejected even without explicit registration. Inspection
runs under initialization quotas without exposing Lua's debug library.

`command(name, schema, help[, export[, permission[, example]]])` binds a command to a global Lua
function (the same name unless `export` is supplied). It is optional: use it
for typed/optional arguments, narrower bounds, custom help or command aliases.
Permission defaults to `public`; alternatives are `dm`, `owner`, `channel`,
`shared` (verified channel plus shared-state grant), `reminder` and `home`.
The same predicate filters discovery and external dispatch. Declarations
restrict admission but never grant a capability or make a nickname an owner.
Optional arguments accept `nil`; examples are printable ASCII, at most 64
bytes. Ordinary cooperating Lua calls retain the original invocation and
native-helper grants; these declarations do not add per-function sandboxes.
Explicitly bound exports are not also registered implicitly under another name.
Lua export names are lowercase identifiers, up to 24 bytes. Explicit public
command names may also contain dashes: for example,
`command('read-memories','','Read memories','list_memories')` dispatches
`!read-memories` to the ordinary Lua function `list_memories()`. No automatic
dash/underscore conflation occurs; duplicate command names and inferred/
explicit name collisions reject staging. Each custom set can register eight commands;
explicit builtin overrides count toward those eight slots. Builtin names
remain reserved for ordinary declarations; use `override_command` below.
Manifest help is up to 64 printable ASCII bytes. Duplicate names, missing
exports, invalid schemas and initialization side effects reject installation.
Only initialization can register commands.

Schemas are comma-separated, at most 96 bytes and four positional arguments:
`name:string:N` is one nonempty token; `name:text:N` consumes the remaining
text and must be last; `name:int:MIN:MAX` is a bounded integer;
`name:bool` accepts only `true` or `false`. Strings/text have explicit bounds
of 1..159 bytes. An optional trailing argument uses `name?:TYPE` and becomes
Lua `nil` when omitted. Required arguments cannot follow optional ones.
Token strings support single/double quotes and backslash escaping; malformed
quotes, excess/missing arguments, oversized strings and out-of-range numbers
fail before executing the export. The complete RF command is still at most
162 printable ASCII bytes. Input is never compiled as Lua.

The firmware-supplied read-only `ctx` contains metadata, not command arguments:
- `kind` (`command`, `startup`, `connectivity`, `message`, `node_status`),
  copied `message`, and `targeted`.
- `sender.public_key` (hex, only when authenticated), `sender.authenticated`
  (native contact/MAC), and descriptive unauthenticated `sender.nickname`.
- `channel.present`, `verified`, `name` and `id` (full digest only when verified);
  no channel secret.
- `grants.owner`, `shared`, `home`, `reminders`, `channel_wait` and the immutable
  list of owner-granted full-key `destinations`. These are the invocation's
  captured grants, not a way to create native authority.
- `packet.timestamp`, `route_type`, `path`, `path_known`, `path_width`,
  `path_count`, `trace_available`, and `trace_explicit`.
- `radio.local_reflection`, `measured`, integer `rssi`/`snr`, precise `rssi_dbm`/`snr_db`,
  `frequency_hz`, `bandwidth_hz`, `sf`, and `cr`.
- `node.available`, `public_key` (only with an identity), `enabled`, `ready`,
  `fault` (boolean only), `roles_known`, `selected_roles`, `ready_roles`,
  `wifi_known`, `wifi_connected`, `battery_available` (false), `uptime_ms`,
  `native_revision`, `build`, `name`, `source_generation`, and `lua_version`.
- `air.available`, `transmitting` (aggregate), `queued`, `aggregate_queued`,
  `generation`, `configuration_generation`, `captured_ms`, `credit_ms`,
  `aggregate_credit_ms`, `rf_ms`, `aggregate_rf_ms`, `successes`, `failures`,
  `aggregate_successes`, `aggregate_failures`, `reserved_ms`, `limit_ms`,
  and `remaining_ms`. Check availability before interpreting values;
  `captured_ms` is the wrapping native millisecond capture time.
- `observation.unique`, `capacity`, `window_ms`, `truncated`, and read-only `paths`.
- Read-only `limits.reply_bytes`, `path_bytes`, `trace_bytes`, and `trace_hops`.

Return a reply string or call `reply("...")`. Replies must be 1..162 printable
ASCII bytes. Explicit `reply()` yields until actual TX completion and returns
the same typed result as `mesh.send`; it permits one explicit reply per
invocation. Returning text instead leaves final delivery to the native core.
`request_trace()` now yields for a validated native trace result;
its optional `width:hex` argument uses the same native route validator as
`!trace`. Scripts never supply arbitrary radio bytes. By default one final
reply is submitted. Explicit mesh sends/traces/adverts share the invocation's
eight-operation bound and existing airtime policy; they do not get free
replies or automatic retries. Installing a custom command set replaces the
previous installed generation, including its explicit builtin overrides. Native
`ping`, `test`, `path`, `mt`, `trace`, `help`, `remind`, `reminders`, `cancel`,
`remember`, `recall`, `forget`, `notes`, `list-memories`,
`calc`, `convert`, `roll`, `choose`, `weather`, `service`, `board`,
`about`, `version`, `uptime`, `status`, `signal` and `air`
are reserved and dispatch their bundled registered functions unless the
operator explicitly installs an override. `plugins`, `neighbors` and `admin`
follow the same rule.
Multiple exported handlers are durably mapped by one bounded source/manifest
set in the beta installer; there is no separate per-package upload protocol.
`source rollback` restores that entire previous custom set. The existing v1
deployment journal and native transfer framing are unchanged.

### Explicit builtin overrides and original dispatch

An operator can patch a builtin or choose regional behavior by installing
Lua through the existing source workflow. See the runnable example and
installation commands in [BOT_DEVELOPMENT.md](BOT_DEVELOPMENT.md#replace-or-wrap-a-builtin-without-rebuilding-firmware).
No new firmware flash or identity change is required on an image exposing
these helpers; `source api overrides` reports their availability.

| Lua API | Contract |
| --- | --- |
| `override_command(name, export)` | Source-initialization-only declaration. Select a known builtin and a non-native global Lua export. Clone its schema, permission, help and example; change only its handler. Consumes one custom command slot. |
| `call_original(name[, argument_text])` | Command-invocation-only call to the saved bundled handler, never the installed override. Recheck the named builtin's policy against the current native event. Parse supplied text, or the incoming argument text when omitted, using that builtin's typed schema. Return its first result; cooperative I/O may yield. |

Unknown names, duplicate declarations, missing exports and invalid schemas
reject source installation. Invalid original names/arguments or denied
authority fail the invocation before entering the original handler.
Direct assignment to native/helper names and ordinary `command` declarations
of builtin names remain forbidden. There is no context argument and no
ordinary-caller registration path. `ctx`, native grants, full-key storage
ownership, bot-source ownership, operation quotas and reply routes always
belong to the invoking job, including when calling a different original.
Passing a saved or forged table in place of argument text fails.
The incoming `ctx.message` remains the received message; argument substitution
does not create a new caller or event.

Failure or cancellation does not retry the builtin or undo an admitted
mutation/transmission. Native completion tokens retain the same job and
source-generation fences. Replacement, rollback and removal use the existing
whole-source transactions; durable selection restores on restart, while
RAM-only activation does not. Durable KV and identities remain unchanged.
ESP32/host names without overrides still use their independent diagnostic
heap; explicitly overridden names use the active source heap. Native
management remains available for recovery. Pine uses its existing single
session, 48 KiB heap and two-job profile; home/HTTPS commands remain unavailable,
including through an override or `call_original`.

**Source API migration:** the old `return function(e)`/action-table ABI is
explicitly rejected, not silently interpreted as a command registry. Existing
custom sources must be rewritten to named functions and reinstalled. A legacy
durable source produces a visible live-validation error while the previously
working/bundled diagnostics remain available; identity/role journals are not
reset. An exact copy of bundled source is accepted for restore/readback, but
editing it to override a reserved command is not. The web editor's **New hello
handler** button starts a custom set without copying native diagnostics.

### Compact package metadata and compatible schema changes

The developer CLI wraps ordinary Lua source in a first-line comment held inside
the same 4,096-byte source envelope:

```text
--@meshcore-bot/1;name=example;version=1.0.0;runtime=lua-5.5.1;api=named-commands-v1;caps=kv;schema=none;rollback=none
```

The device checks the metadata format, Lua runtime, command API, known
capability names and data-schema transition before compiling and activating
the candidate. `caps` documents required native API surfaces; it never grants
them. Trusted owner authentication still authorizes deployment, and native
policy checks every persistent, mesh, event and configured-host operation.
Feature builds may advertise `cmdmeta` (optional command permissions/examples),
`mesh-dest` (explicit owner-granted full-key DM destinations), and `mesh-chan`
(verified channel waits). These capabilities describe compiled support only;
the channel-wait owner flag and individual destination grants remain separate.
The metadata is ordinary source text, so existing named Lua source remains
readable and editable. Direct raw-source uploads remain supported for manual
recovery; package lifecycle tooling emits metadata-bearing source.

`schema=none` means the source has no application-specific data format. A
versioned schema is a lowercase name and positive integer, such as
`notes@2`. If a candidate changes from `notes@1` to `notes@2`, it must declare
`rollback=notes@1`; the source manager rejects an undeclared schema change.
Before rolling back, the source manager checks that the active package declared
compatibility with the previous schema. No automatic destructive conversion
is claimed.

Compatible migrations run lazily in each authenticated storage scope using
the existing `kv.transaction` API. A migration publishes a bounded set of
changes atomically, retains the previous representation, and keeps old-version
reads/writes compatible for the rollback window. The shipped
`plugins/examples/notes-v1.lua` and `notes-v2.lua` pair demonstrates this
contract: both versions use the same caller scope, schema markers use compare
and set, and v2 mirrors writes to the v1 key. Conflicts, rejected writes and
unknown commit outcomes stay explicit. A schema change that cannot preserve
the prior representation is incompatible and rejected rather than represented
as an automatic migration.

Use [BOT_DEVELOPMENT.md](BOT_DEVELOPMENT.md) for package creation, optional
signatures, the Make workflow and host replay. `source metadata` and
`source api` provide the current active contract to the authenticated CLI.
Package bytes continue over the existing resumable RF/web source transfer;
there is no archive, decompressor or per-handler VM.

Message and path capacities come from the pinned native headers and
`Mesh::createDatagram` admission rule: 184 payload bytes minus MAC/padding
allowance, timestamp and text flags yields 162 text bytes. Ordinary paths
use the full native `MAX_PATH_SIZE` (64 bytes), retaining 1/2/3-byte hashes;
TRACE has its separate native payload capacity and at most 63 collectable
hops. There is no 64-byte reply or eight-one-byte-path prototype ABI.
The current eight-observation window is a provisional bounded collector
budget, not a path-format restriction; handlers discover its capacity and
actual count. The beta has short physical timing/memory samples in
[MAST_ADMIN.md](../esp32/MAST_ADMIN.md); broader workload measurements are still required
before tuning resource budgets.

Production now retains one shared command environment per active generation.
All owner-installed functions can call each other and the bundled diagnostics,
and share mutable globals/tables. Candidate initialization uses a separate
private state; activation swaps complete validated definitions and cancels old
jobs, not individual functions. Globals reset on replacement/reboot; named
durable KV does not. On ESP32 and the host, a separate retained, native-only
recovery heap dispatches addressed builtin names without an explicit override,
even if custom globals exhaust their heap.
It is not a per-function/plugin sandbox.

The sandbox opens no standard libraries. Native helpers provide command
registration, reply/trace/help, numeric `tostring`, `kv` and timer sleep.
The frozen `utility` table supplies bounded arithmetic, conversion and random
selection helpers; these do not expose a compiler or general math library.
The frozen `node.report(name[, page])` binding formats the six read-only
diagnostics and the `mt` path-collection report from the current invocation,
with the same authority and reply bounds; only `air` accepts a page (1..4,
default 1). It cannot run during source
initialization, refresh a snapshot or mutate native state.
There is no arbitrary filesystem, HTTP/network socket, PHY, mutable clock, external module loader, dynamic
compiler, binary bytecode loader, debug library or script-managed coroutine
API. `ctx` resolves the current Lua thread's native invocation; nested metadata
uses immutable copied userdata views. Native helper/diagnostic bindings are
protected through an empty environment proxy, while ordinary bot globals
remain mutable.

Limits cover editable source (4,096 bytes), allocator live memory (96 KiB, PSRAM-only
on ESP32, per state), executed VM instructions (10,000 per initialization or
invocation), lexer steps (16,384), and C-call depth (40).
Retained startup failures distinguish load/init deadlines from the Lua heap
limit and allocator failure. The existing source/fault response reports the
deadline's elapsed/limit microseconds and peak Lua allocation, or the failed
heap/allocator category and peak/cap bytes; a deadline-induced allocation
refusal is not reported as generic Lua "not enough memory".
Retained compilation/initialization have cumulative **330 ms load and 50 ms
initialization** limits across bundled and installed code. Each invocation has
**20 ms cumulative active wall time**, excluding suspended I/O time, and retains
its instruction count across yields. Runnable loops cannot evade the limits by
yielding. The legacy one-shot test interface retains its separate 20 ms cleanup
limit and combined initialization/invocation instruction counter; production
does not compile/close a state for each request. Retained state teardown frees
bounded native allocations without script finalizers. Native allocator/NVS
calls cannot be preempted mid-call; this is not a hard real-time guarantee.
No phase disables preemption or raises the worker above the radio task.
The aggregate load allowance covers the bundled source plus up to 4,096
editable bytes; it does not relax invocation execution. `bot-vm-test` includes
a 20-us synthetic-clock regression for the bundle plus a maximum-size editable
fixture. Its timings model budget consumption, not physical ESP32 execution.
The source byte count and peak allocations depend on the selected bundle.
Load still has a 330 ms limit, independent of the 20 ms active execution,
96 KiB heap and 16,384 parser-step limits.

`BotVmLimits.initWallUs` selects initialization headroom independently of
`wallUs`, which bounds active invocation and one-shot cleanup. `bot limits`
reports the default load/init/active limits. Lua remains the pinned 5.5.1
bytecode interpreter with a retained state; increasing initialization headroom
does not grant longer-running commands or additional I/O authority.

| Stage | Default bound / accounting |
| --- | --- |
| Download, upload and source-file copy | Transport/storage deadlines, outside the VM phase counters |
| Compile/load installed source | 330 ms aggregate; `loadUs`; exactly matching firmware-owned bundled bytes use the default one-second recovery allowance |
| Initialize declarations, modules and source state | 50 ms aggregate; `initUs` |
| Invoke cached handlers and resume runnable work | 20 ms cumulative per command; `invokeUs`; no recompile |
| Wait for native storage, network, timer or radio I/O | Suspended time is excluded from active execution; native operation deadlines still apply |
| RF transmission | Shared scheduler airtime/admission and TX completion, not VM execution time or recipient delivery |

The existing serial phase report uses microseconds. Optional telemetry exports
the latest completed operation's `last_vm_{load,init,invoke,cleanup}_seconds`;
these are wall intervals, not task CPU measurements or end-to-end reply latency.
The pinned lexer checks the compile budget on every consumed character;
the VM hook checks every instruction. There are no unbounded C library
helpers. Errors, exhausted budgets and late results discard all script actions.
An admitted request can receive a bounded native `Error:` reply instead, under
the same reply/airtime limits; its admission ACK does not promise success.
A VM worker with a 16 KiB FreeRTOS stack handles compilation/execution off radio
callbacks. Four bounded invocation slots retain per-thread context and reply
routes; custom commands may occupy three, leaving one admission slot for native
diagnostics. A separate 6 KiB storage worker performs durable KV I/O. The source
validator, which cannot execute command I/O, does not allocate a storage task.
Data restore lazily reserves one completion workspace in PSRAM and reuses it
until the worker stops; allocation failure rejects the restore before mutation.
KV publication carries only its outcome and error buffer through recovery,
without placing the network JSON result buffer on the storage-task stack.
Core, source buffers and Lua allocations have no internal-RAM fallback.
Atomic mailbox/generation/grant state is explicitly allocated in internal RAM,
not PSRAM, including when the worker belongs to the PSRAM-resident mast backend.
Only copied data, Lua heaps and non-synchronization records use PSRAM.

### Command diagnostic output

On-device VM timing/memory, command-admission and fault lines use the existing
`mesh-diagnostics` worker rather than writing USB from `CommandBot::loop`.
Formatting uses a fixed 160-byte stack buffer; enqueue copies into an eight-entry
FreeRTOS queue with zero wait. The queue also serves companion diagnostics.
Its 160-byte text plus one-byte source tag uses 1,288 bytes of payload storage
(264 bytes more than the previous companion-only queue). A combined image keeps
the same single 4 KiB diagnostic-worker stack; an enabled bot-only setup starts
that worker too. Queue startup failure does not enable a synchronous USB fallback
for the command bot.

When the queue is full, a new log line is discarded and counted. VM statistics
and fault state are retained independently of serial output. Read
`bot diagnostics` through authenticated management for command-log enqueue/drop
counts and `bot log` for current state/fault. The native host keeps its existing
console output when no asynchronous sink is installed.

The pinned XIAO S3 board selects Arduino HWCDC (`ARDUINO_USB_MODE=1`).
Arduino ESP32 2.0.17 defaults to a 256-byte TX ring and a 100 ms transmit timeout.
`HWCDC::write` holds its TX lock while sending and resets its no-progress retry
count when bytes advance; this is not a whole-write deadline. A full buffer or
another serial writer can delay the caller. Without a connected consumer the
SDK can instead discard older buffered output, so an attached unread USB cable
alone does not establish a two-second stall. Do not set a global USB timeout or
open a serial reader to diagnose a live node that resets on port open.

`make -C firmware/esp32 bot-runtime-test` holds the actual diagnostic queue
undrained and checks ten encrypted Pong replies without dispatch-thread serial
writes. The authenticated-web lifecycle test also rejects every diagnostic
enqueue while checking ten Pongs, successful mailbox replies, drop counts and
retained faults. These tests isolate logging backpressure; they do not reproduce
a particular device's HTTP timeout or establish a TLS cause.

### Granted event subscriptions

Declare a named handler with `events.on(kind, function_name)` during source
initialization. Supported kinds are `startup`, `connectivity`, `message` and
`node_status`, one handler each. They share the installed bot environment,
modules and cooperating functions; event-only source sets are supported.
Handlers are not implicitly exposed as public commands. An explicit command
alias may still call the same function.

```lua
function _online()
  kv.put('last_event', ctx.kind, 'bot')
end
events.on('startup', '_online')
events.on('connectivity', '_online')
```

The owner must grant event types through the existing authenticated RF/web
backend: `bot events MASK`, with bits **1 startup, 2 connectivity, 4 message,
8 node_status**; `0` disables all. Missing policy defaults to off. The example
also needs the separate existing `bot shared on` grant to write bot-global KV.
`bot events` reports saved/effectively subscribed masks and queued/dropped/
completed/failed counters. No new authentication scheme is involved.

Startup runs after active source admission, once per activated source generation
per boot; staging/validation never invokes it. Connectivity delivers the current
WiFi availability/connection snapshot and subsequent changes. Node status
delivers current/change snapshots of readiness, fault boolean and selected/ready
roles. The dispatcher captures all snapshots; workers never read live WiFi or
role objects. Source replacement starts new snapshot baselines.

Message handlers receive copied printable text for native decrypted DMs addressed
to this bot and its explicitly selected verified channel, including commands.
`ctx.kind` and `ctx.message` join the existing immutable context. An authenticated
DM carries its actual full sender key. A channel carries the verified channel
identifier and an **unauthenticated nickname**, never an individual sender key.
Local reflections/own transmissions are excluded. Sixteen message hashes are
held for 120 seconds; duplicate copies are suppressed and full dedup capacity
drops new events rather than evicting live entries.

Event metadata is **not private-user authority or a reply route**. Only explicitly
granted bot-global KV/timers (non-channel events) or verified channel KV/timers
(channel messages) are available. Caller/conversation state, RF send/reply/
forward/TRACE/advert/waits, personal reminders and private-user HTTPS RPC reject
explicitly. Sleep and allowed storage/timer operations use the existing yielding
I/O model. Returned strings do not transmit packets; a handler may finish without
a reply. Node snapshots remain read-only.

There is at most one admitted event coroutine/mailbox, globally at most one per
second, no unbounded backlog and no per-event VM/task. Status changes coalesce
to the latest snapshot while busy; messages can be dropped. Events count against
the three custom slots, retaining one native diagnostic slot. Accepted path
collectors reserve capacity alongside worker jobs, so events cannot take their
delayed handoff slots. A failed handoff attempts a bounded busy error through
the ordinary native RF budget; the transport ACK is not application success.
The existing
10,000-instruction, time, heap and eight-I/O budgets apply. A runaway/failing
handler cannot send an error reply over RF; its failure is counted/logged.

`events.off(kind)` disables that source's subscription until source reload.
Unsubscribe and owner grant changes advance a native epoch, fencing queued
callbacks, storage admission and delayed completions. Regrant never revives old
invocations. Revocation disables live access before saving; failed persistence
is explicit and may leave the older saved policy for reboot. Source replacement/
stop cancel old coroutines. Already-published durable writes recover normally;
revocation cannot undo an admitted commit. Volatile events are never replayed
after reboot. `source api events` reports the contract.

### Declared package-local modules

Declare up to eight named module loaders inside the existing 4,096-byte source
envelope. `require(name)` resolves **only** these native registered declarations:

```lua
module('greeting', function()
  return {hello=function(name) return 'Hello '..name end}
end)
function hello(name)
  return require('greeting').hello(name)
end
```

Names are non-native lowercase Lua identifiers, at most 24 bytes. Each loader
must be a source-defined Lua function returning a table. All declarations,
including unused modules, initialize under the existing compile/instruction/
heap/time budgets before a candidate can activate. Duplicate/undeclared names,
dependency cycles, invalid return types and loader failures reject staging.
Loaders cannot declare additional modules or perform native I/O. Initialization
does not have an invocation `ctx`; put yielding work in returned functions.

The returned table is cached once per source generation, shared by cooperating
commands in the same bot environment. Calls through it retain the current
coroutine's immutable caller/scope/grants and yield normally. Modules are not a
new VM or privacy boundary. Native helper names and reserved commands remain
protected. There is no filesystem search, native library loading, binary chunk
loading, package path, runtime declaration or host-side concatenation.

Source export, install, restore, rollback and reboot carry these declarations
as ordinary source bytes in the existing checked source journal. Reload starts
a new module cache; durable KV remains unchanged. The command registry still
allows eight exported custom commands. `source api modules` reports the bounds.

### Atomic KV and owner data snapshots

`kv.cas(key, expected, value[, scope])` compares and commits on the serialized
storage worker. `false` means absent for `expected` and deletion for `value`;
an empty string is a present value. The result is immutable:
`{ok=boolean, status='committed'|'conflict'|'unknown'|'rejected', error=text}`.
A conflict performs no writes. An unknown outcome may have committed: read
the affected keys before making a new decision, rather than retrying blindly.

`kv.transaction(operations[, scope])` accepts one to four distinct keys:

```lua
function transfer()
  local r = kv.transaction({
    {key='left', expect='10', value='9'},
    {key='right', expect='2', value='3'}
  })
  return r.status
end
```

Each operation requires `key` and `value`; optional `expect` compares a string
or `false` for absence. Omitting `expect` is an unconditional mutation.
Unknown fields, holes, duplicate keys, oversized text and scope/grant violations
fail explicitly. Keys are at most 32 bytes, values at most 256 bytes (no NUL),
and all operations use one existing scope/principal. The existing eight-key
quota and public/bot-reserved slot allocation remain in force.

KV payloads live in five double-buffered SPIFFS groups. Every changed group is
written, flushed, closed and read back before a single **200-byte** NVS authority
selects all of them together. That authority is the transaction's publication
point, including an eight-key owner restore. A scalar update rewrites one
3,136-byte group, not the complete store. No per-member NVS replay is needed.
Every access reloads the committed authority, including a same-process retry
after an uncertain commit; it never chooses an inactive file from speculative
RAM. When the exact authority is unchanged, reads and scheduler recovery reuse
the previously verified PSRAM records. KV mutations and restart revalidate all
selected files. Missing or corrupt selected files detected during verification
block access rather than falling back to an older bank.

On upgrade, the storage worker validates all legacy KV slots across every bot
identity and principal, then applies any valid committed redo in PSRAM. An
interrupted older replay can therefore finish even after a source change or
grant revocation. Only after publishing and verifying the file authority does
it reclaim the migrated NVS keys. The authority records unfinished reclamation,
so a restart resumes cleanup without reimporting a partial legacy bank.
Migration does not change ownership, erase a namespace or format SPIFFS.
The storage worker finishes recovery before scheduler writes; failed recovery
also blocks timer/reminder I/O, imports and autonomous transitions. Public
computation and diagnostics remain available.

See [KV storage and upgrade](#kv-storage-and-upgrade) for the file format,
admission budget, downgrade warning and validation commands.

The owner's existing authenticated RF/web administration can export and
explicitly stage/restore one KV scope with `data` commands or `admin.py
data-export` / `data-restore`. See [MAST_ADMIN.md](../esp32/MAST_ADMIN.md#scoped-bot-data).
The checked 2,422-byte version-1 snapshot preserves the full bot identity,
scope and full user/channel principal. Restore atomically replaces that scope's
eight-or-fewer keys, leaving all other scopes and principals alone. Source
rollback/reboot do not revert durable KV. Native credentials, grants, clocks,
identities, timer claims and autonomous reminder journals are not imported or
replayed by this **KV-data** operation; scheduling state stays unchanged.
The same bounded owner staging also supports separate `timers` and `reminders`
families (`BTD`/`BRD`, version 1, 2,422 bytes each). Reminder exports require
caller scope; timers support all four scopes. Backups include durable states,
deadlines, revisions, reminder IDs/text and original source generations.
Scheduler restore requires explicit `no-rearm`: matching live records win,
pending records become cancelled, missing records are imported without delivery
authority, and revisions advance beyond both live and backup high-water values.
Terminal history is never changed back to pending, other records are not
evicted, and a delivery in progress blocks reminder restore. Fresh execution
requires an explicit new schedule. Each family's bounded merge publishes
atomically through its file authority; an uncertain commit requires readback,
not automatic replay on reboot. There is no cross-family transaction. See the
exact policy and complete
per-family backup procedure in [MAST_ADMIN.md](../esp32/MAST_ADMIN.md#scoped-bot-data).
`source api atomic` and `source api data` report these additional contracts
without changing earlier API response shapes.

### Bundled utility commands

`!calc <expression>`, `!convert <value> <from> <to>`, `!roll [dice]` and
`!choose <a>|<b>|...` are protected ordinary Lua functions, registered through
the same schemas, VM and native reply queue as other commands. Installed
functions can call them normally, or use the immutable native
`utility.calc/convert/roll/choose` table with the same string arguments.
They return one printable, bounded reply string; nested calls do not send
their own RF messages. Calls perform small bounded native work on the VM
worker, never on the receive callback. There is no storage, network request,
new task, PHY or random-seed journal.

Authenticated nonlocal private DMs and the explicitly selected, verified
channel are admitted. With the existing owner opt-in selecting `#example1`,
all members may use these stateless utilities without claiming an individual
identity. Channel opt-in remains off by default; the native group-wide
rate/airtime policy and packet dedup still apply regardless of nickname.
These helpers grant no private storage, reminders, forwarding or destination
authority. Unverified channel events and local reflections fail.

**Calculator:** `!calc (2+3)*4` returns `= 20`; `!calc 1/3` returns
`= 0.3333333333`. A native recursive-descent parser accepts decimal literals,
optional decimal exponent, unary `+`/`-`, `+ - * /`, spaces and parentheses.
Multiplication/division bind before addition/subtraction; both levels are
left-associative. No incoming text is compiled or evaluated as Lua.
Functions, variables, powers, modulo, hexadecimal and implicit multiplication
are deliberately unsupported and fail visibly.

Expressions have at most 120 bytes, 64 tokens, 32 arithmetic/unary operations
and eight levels of combined parenthesis/unary nesting. Literals have at most
32 bytes and 15 significant mantissa digits; exponents are at most two digits
with magnitude at most 18. Zero is valid; every other operand/intermediate/
result must have magnitude **1e-18..1e18**. Division by zero, nonfinite numbers,
out-of-range values, underflow and malformed/trailing input are errors.
Calculations use binary64, not exact decimal or arbitrary-precision arithmetic;
roundoff/cancellation can occur. Results are rounded for display to at most
**10 significant digits** using `%g`, with scientific notation when appropriate.
Negative zero is displayed as zero. This is a utility calculator, not a
financial/exact-integer evaluator.

**Conversion:** `!convert 1 mi km` returns `1 mi = 1.609344 km`;
`!convert 32 F C` returns `32 F = 0 C`. The direction is always **FROM to TO**;
the output repeats both units. The value must be one signed decimal scalar,
not an expression. The numeric/precision limits above apply to the input and
result. Units are **case-sensitive**, at most four bytes, with no inferred
aliases or cross-dimension conversions:

| Dimension | Accepted units |
| --- | --- |
| Temperature (absolute, not deltas) | `C`, `F`, `K`; below absolute zero is rejected |
| Distance | `mm`, `cm`, `m`, `km`, `in`, `ft`, `yd`, `mi`, `nmi` |
| Mass | `mg`, `g`, `kg`, `oz`, `lb` (avoirdupois, not fluid ounces) |
| Speed | `m/s`, `km/h`, `mph`, `kn`, `ft/s` |
| Duration | `ms`, `s`, `min`, `h`, `d`, `wk` |
| Data size | `b`, `B`, `kb`, `Mb`, `Gb`, `Tb`, `kB`, `MB`, `GB`, `TB`, `KiB`, `MiB`, `GiB`, `TiB` |

SI prefixes use 1000; `KiB`/`MiB`/`GiB`/`TiB` use powers of 1024.
Lowercase `b` means bits, uppercase `B` bytes (eight bits). One nautical mile
is 1852 m; a knot is one nautical mile per hour; a day is 24 hours and a week
seven days, not calendar arithmetic. Temperature freezing points and absolute
zero use exact reference cases rather than displaying cancellation noise.
Other categories accept signed linear quantities. Display uses the same
10-significant-digit rounding; unsupported spellings/dimensions fail rather
than guessing a unit.

**Dice:** `!roll` defaults to `1d6`; `!roll d20` and `!roll 2d6+3` are valid.
The exact notation is `[N]dS[+/-K]`, lowercase `d`, no internal whitespace,
at most 24 bytes. Count is 1..12, sides 2..1000, modifier magnitude 0..10000.
Output includes the notation, every die, modifier and total, e.g.
`Roll 2d6+3: [2,5] +3 = 10`. There are no exploding/keep-highest/compound dice.

**Choice:** `!choose tea | coffee | water` returns, for example,
`Choice 2/3: coffee`. Input is at most 148 bytes with 2..8 alternatives;
leading/trailing spaces on each are trimmed, and each must contain 1..64
printable bytes. Empty alternatives, newlines and excess counts/lengths are
errors. Every alternative position has equal probability; duplicates count
as separate positions. There is no pipe-escaping syntax.

Dice/choice use rejection sampling from 32-bit native random words, not
biased modulo reduction. At most 32 words may be consumed per helper call;
entropy failure or exhausted rejection budget is explicit, with no fallback
seed or predictable sequence. ESP32 uses the same SDK `esp_fill_random`
source as native identity generation, not time or a reboot-reset Lua PRNG;
entropy quality follows the SDK/platform's boot/RF entropy conditions.
Linux native tests use nonblocking `getrandom`, with deterministic injected
words solely for exact rejection/boundary tests. Target randomness quality
and timing have not been physically measured by this slice.

All helpers together allow **eight calls per invocation**, including nested
functions and calls before/after a yield; the counter is not shared between
callers. Existing cumulative active-time/Lua instruction limits still apply.
The bounded native helpers do not themselves yield and cannot preempt a
single SDK call. Output bounds include the command's RF prefix/labels; dice
and choices fit without hidden truncation. A smaller route/channel reply
capacity fails explicitly. `!help PAGE` includes every permitted native and
installed export in numbered pages, rather than dropping names after one RF
message. Individual command descriptions and examples are paginated too.

**Source/migration:** these four names and `utility` are newly protected.
Rename conflicting installed definitions before upgrading; eight custom
exports remain available. Firmware loads a separate **633-byte** trusted
Lua utility chunk alongside the **3,955-byte** core chunk before
inspecting native declarations. Both share one environment and cumulative
compile/init budgets; only firmware supplies this extra chunk. The editable
source/upload limit remains 4,096 bytes, with full-size source tests.
That utility slice added four entries; the current registry has 30 built-ins,
including the network and board commands below. Source/role/KV/management journals and
signed framing are unchanged. `source api utility` reports the capabilities.

### Bundled personal notes

The ordinary bundled Lua functions now provide `!remember <key> <text>`,
`!recall <key>`, `!forget <key>`, `!notes [prefix]` and
`!list-memories [prefix]`. The last name is a registration alias of `notes`,
not a separate implementation or namespace. All five require a nonlocal,
authenticated **private DM**; even a verified selected channel with the owner
shared-state grant receives an explicit private-DM error before storage I/O.
A nickname cannot establish an individual principal. These commands do not
provide channel access; the separate bundled `!board` command below explicitly
selects the granted channel tier using the same public KV API.

For example, `!remember groceries milk and bread` creates the caller's
`groceries` key, `!recall groceries` reads its text, and either
`!notes gro` or `!list-memories gro` lists the key. `!forget groceries` deletes
it. Installed cooperating functions may call `remember('groceries','milk')`
and `recall('groceries')` normally; the native storage operations yield the
same coroutine and preserve its principal/route. `kv.put('groceries', ...)`
uses exactly the same caller key. Functions/source replacement/reboot do not
partition or reset notes; the full persistent bot and caller identities do.

RF command schemas bound keys/prefixes to 32 bytes and note text to 120 bytes;
the complete request/reply still has a 162-byte limit. Storage retains its
existing eight-key scope quota and 256-byte value bound. Successful writes
say **created/replaced; committed**, deletion says **deleted; committed**,
and missing reads/deletions say **No note**. Storage failures propagate as
errors, including explicit unknown outcomes when a mutation cannot be
verified; no automatic mutation retry occurs. A commit response reports
storage, not proof that the RF reply reached its recipient.

Enumeration lists sorted matching keys, not values. When all keys do not fit
one RF response it preserves whole displayed keys, gives the total matching
count and says **truncated; narrow prefix**. Refine a prefix or recall a known
key; there is no implicit second RF message or cursor. A value created by
other functions may exceed RF capacity, be empty or contain non-ASCII bytes;
`recall` explicitly refuses that representation and directs the author to
`kv.get`. Non-ASCII keys similarly produce an explicit `kv.list` hint rather
than a silent partial list. Raw API reads remain available without rewriting
legacy data.

**Migration:** these five command names and the four corresponding Lua
functions are newly reserved. Rename/reinstall old custom definitions or
aliases before upgrading; conflicting sources fail validation, not silently
override the built-ins. Custom export capacity remains eight. No KV/role/
deployment journal format, key migration or new management transport is
introduced. `source api notes` reports the new commands and `kv-list=1`;
the existing `source api` / `source api storage` response shapes remain stable.

### Bundled shared channel board

`!board put <key> <text>`, `!board get <key>`, `!board list [prefix]` and
`!board delete <key>` are now protected ordinary Lua commands using the real
yielding `kv.*(..., 'channel')` API. They require both the existing explicitly
selected native channel policy and the owner's authenticated **`bot shared on`**
grant. The default-off grant covers bot-global/channel storage generally,
not just boards; there is no new per-member ACL or management protocol.
Private DMs, local reflections and unverified channel requests are explicitly
rejected. A DM cannot select a board by supplying its public hashtag.

**Every holder of the selected channel secret can read, replace or delete
every entry in that board.** Nicknames, including an `owner:` label, establish
no individual authority or ownership. The principal is the full native
key-derived digest of the successfully decrypted selected channel, not its
display name or short on-air hash. Public hashtag keys are derivable from
the hashtag: enabling a `#example1` board does not make its members private
or individually authenticated. Different native keys have different boards;
personal caller-key DM notes remain separate and inaccessible through `board`.

For example, one member can send `!board put picnic bring mugs` and another
can send `!board get picnic`, `!board list pic` or `!board delete picnic`.
Installed cooperating Lua can call `board('put','picnic','bring mugs')` in a
verified channel invocation. All board operations use the stable raw key
**`board:` + public key**, with no source/function/generation component.
Generic channel key `picnic` does not collide with `board:picnic`; private
DM `board:picnic` is in another scope. This prefix is collision avoidance,
**not a security boundary against trusted installed Lua**, which can
deliberately access those raw channel keys. Source replacement and reboot do
not reset them; switching channels and back restores the corresponding board.

Public keys/prefixes are 1..26 bytes (list may omit its prefix), text is
1..120 bytes, and the entire incoming command including the nickname must
still fit the native packet. The `board:` prefix consumes six of the raw
32 key bytes. Boards share the **eight-entry per-channel quota** with all
other channel KV and the **32 public journal slots** with other public scopes;
they cannot allocate the eight bot-global reserve slots or bypass NVS
headroom/write/deadline guards. No separate board bank, extra worker, PHY,
queue or scheduling mechanism is added.

Writes explicitly report **created/replaced; committed** after commit and
independent-handle readback. Deletes report **deleted; committed**, or
**No board entry** when already absent. Failed/uncertain operations remain
visible errors; cancellation after a possible commit is not represented as
rollback or blindly retried. Grant revocation/regrant fences old invocations,
including failures to durably save the revoke. Serial writes are last-commit
wins; a list sees a coherent snapshot, not a multi-operation transaction.
Read-only commands also require the shared-state grant.

Lists contain sorted matching public keys and a matching count, not values.
Responses preserve whole displayed names and report **truncated; narrow
prefix** when necessary; no implicit second RF response or hidden cursor
exists. The channel label reduces the native 162-byte limit; formatting uses
the actual invocation reply budget. Raw values that are empty, non-ASCII or
too large for RF fail explicitly without rewriting storage. Non-ASCII keys
or a raw nameless `board:` entry authored by installed Lua also produce an
explicit maintenance hint rather than a misleading list.

**Migration:** `board` is newly reserved; rename/reinstall conflicting custom
functions or aliases before upgrading. The older raw-storage comparison
fixture is now `shared_board`. Firmware loads an independently bounded
**2,243-byte** board chunk alongside the core/utility/network chunks under
the same cumulative parser, compile/init, heap and invocation budgets.
The registry has 21 built-ins and still eight custom exports; editable source
remains 4,096 bytes. KV/role/source journals and signed status/receipt vectors
are unchanged. `source api board` reports the grant, DM denial, prefix and limits.

### Working retained-state and timer APIs

```lua
function memorize(note)
  kv.put('memory', note)
  return 'Committed'
end
command('memorize', 'note:text:100', 'Remember a note')
function list_memories() return kv.get('memory') or 'No memories' end
command('read-memories', '', 'Recall the note', 'list_memories')
counter = 0
function testall()
  counter = counter + 1
  timer.sleep(1000)
  return ping() .. ' #' .. tostring(counter)
end
```

`!memorize get groceries` and `!read-memories` use the same caller-scoped key;
function names and source hashes never enter the durable namespace. `kv.get`,
`kv.put`, `kv.delete` and `kv.list` yield while the native storage worker operates.
Existing first return values are preserved: `get` returns a string or `nil`,
and `put`/`delete` return true on verified success. `get` additionally returns
an `rf_safe` boolean (nonempty printable ASCII fitting this invocation's
reply limit). `put` returns `"created"` or `"replaced"` as its second value,
only after NVS commit and independent-handle readback; `delete` returns
`"deleted"` after checked commit, or `"absent"` when a durable read proves
the key already absent, without another write. Failures raise a visible
command error, not false/success-shaped fallback data. Ambiguous write/commit
outcomes explicitly say unknown; no automatic mutation retry occurs.

`kv.list([prefix][, scope])` returns an immutable
`{count=N, keys={...}, suffixes={...}, truncated=false, rf_safe=BOOL}` view.
`keys[1..N]` contains all matching keys in bytewise lexicographic order,
never values. The additive immutable `suffixes[1..N]` contains the same keys
with the exact requested prefix removed; an exact match has an empty suffix.
This lets namespaced handlers format relative names without a string library.
Completion validation rejects keys that do not match the requested prefix.
Absent/nil/empty prefix matches everything; other prefixes are case-sensitive,
at most 32 bytes and cannot contain NUL. Pass `nil` as the first argument
to select a scope without a prefix. The list has at most eight 32-byte keys;
no pagination is needed at the native API layer. All 40 records are checked
under the serialized storage worker, so concurrent mutations are seen wholly
before or after this snapshot. Duplicate/corrupt records, over-quota journals,
revoked grants, expired deadlines and stale generations fail instead of
returning a partial success. A complete snapshot can become stale after
resumption; it is not a multi-operation transaction. The `rf_safe` flag
means every key is printable ASCII, not that the complete formatted list
fits a radio packet; Lua supplies the explicit RF truncation described above.

Keys are 1..32 bytes and values at most 256 bytes, without NUL. Each operation
scans at most 40 fixed native slots; each 392-byte, versioned/checksummed record
stores the full bot identity, native scope/principal and symbolic key. There
are at most eight keys per bot/scope/principal, 32 public entries plus eight
**bot-global-only reserve slots**; caller and conversation quotas are independent.
Migration preserves the full records and their original slots, including early
bot-global records in public slots. Ordinary
caller/conversation/channel writes cannot allocate the reserve. Bot-global
access still requires the existing explicit owner shared-state grant; this
does not turn an arbitrary caller key into an owner identity.

Public KV puts/transactions/restores require **197 free NVS entries**.
Bot-global KV, deletes and recovery require **155**, or **159** when first
initializing storage without an existing journal (**160** if its NVS namespace
also needs creating). All 40 payloads
and their transaction publication share one nine-entry NVS authority: adding
more KV keys no longer consumes NVS payload entries. SPIFFS still needs enough
space to write and verify the inactive groups; unavailable storage fails explicitly.
Public timer sets, scheduler imports and personal reminder creation retain
their **308-entry** minimum (bot-global timer restores use the metadata/GC
budget). Timer and reminder payloads, including terminal states, now use
double-buffered SPIFFS files. Each family has one **72-byte / five-entry** NVS
authority, rather than seven or ten entries per record. Nonpublic mutations
and migration require **151** free entries: five authority replacement entries,
126 GC entries and the unchanged 20-entry mutation margin. First initialization
adds four marker entries, and a missing namespace adds one. No reserve
constant is reduced to admit these writes.

These checks use SDK free-entry statistics and retain the **126-entry GC
reserve**. Missing statistics or insufficient headroom denies mutation.
No data is evicted to satisfy admission, and other firmware consumers can
still exhaust the shared partition. `kv.list` reports logical keys in one
scope, not free storage. The physical-budget fixture uses a 176-entry synthetic
metadata baseline and fills all 32 public plus eight reserved KV slots alongside
two reminders and a timer using **195 entries**, leaving **435** free.
Filling every scheduler payload slot consumes the same metadata entries.
The model includes replacement entries and the GC
reserve; it does not simulate flash fragmentation or measure target endurance.

Corrupt records fail explicitly. Normal storage requests have a 2-second
checked mutation deadline; each family's recovery/migration has a separate
10-second checked budget before scheduler mutation.
A commit finishing late is reported as committed-after-deadline, not safe to
retry. Identity/source journals use their existing separate keys.

#### KV storage and upgrade

Before upgrading a node with important Lua data, use the existing owner `data`
export commands for the affected scopes.
The first KV recovery migrates legacy storage automatically. Do not erase NVS
or provision/format SPIFFS during this upgrade. Rekeying the bot leaves the old
identity's records intact and inaccessible to the new identity; it does not
free their slots. Source replacement and restart preserve the same ownership.

**Downgrade:** firmware that only understands NVS `BKV1`/`BTX1` storage cannot
read the new authority. Its recovery fails closed for KV, timer/reminder I/O,
autonomous timer/reminder dispatch and bot-data imports. Older scheduler-only
implementations cannot read the new timer/reminder file authorities either.
Keep a runtime that supports these authorities, or explicitly export/import
each scope using a compatible runtime. Do not
delete the authority key to bypass recovery.

The format is bounded:

* `/command-bot/kv-0-a.bin` through `kv-4-b.bin`: two banks per group,
  eight unchanged 392-byte `BKV1` records per file, exactly 3,136 bytes.
  Files occupy at most 31,360 payload bytes, plus filesystem overhead and GC
  working space. Initial publication writes five groups; subsequent changes
  write only affected groups. A four-operation transaction touches at most
  four groups; a scope restore can touch up to five when retaining early
  bot-global slots in the public bank.
* NVS `mc-bot-kv/txn`: 200 bytes, `BKS` plus version byte `1`, a five-bit bank
  selector, one byte indicating pending legacy reclamation, two zero reserved
  bytes, five SHA-256 file digests, then SHA-256 over the preceding 168 bytes.
  Selector bit `i` chooses bank `b` for group `i` when set, otherwise bank `a`.
  A verified replacement of this one blob publishes the complete new state.
  NVS uses nine entries for it: seven data entries, a chunk and a blob index.
* Before creating the first file, a missing journal receives a checksummed
  48-byte `BKI1` initialization marker: four magic/version bytes, twelve zero
  bytes, then SHA-256 of those sixteen bytes (four NVS entries). Restart with this
  marker retries the intact legacy bank or empty first write. A missing journal
  alongside KV files instead blocks access; it is never treated as empty storage.
  A valid existing `BTX1` journal already provides this initialization boundary.
* The worker uses 22,596 bytes of PSRAM for records, readback, legacy redo and
  scalar scratch. No new task or persistence framework is added. Publication
  does not imply an RF reply reached the caller; uncertain or cancelled commits
  require a read before retrying a state-changing operation.

The 155-entry admission is nine replacement entries + 126 GC entries + 20
mutation-margin entries. A first initialization also budgets four entries for
the marker before allocating the authority, requiring 159 entries. For public
KV, replace the old eight-record reserved
payload allowance (8 × 15 entries) in the 288-entry core reserve with the
nine-entry authority, retaining the rest of the core/scheduler allowance and
the 20-entry margin: **288 − 120 + 9 + 20 = 197**. Existing live entries are
already excluded from SDK free-entry statistics; migration receives no advance
credit for reclaiming them. Timer/reminder constants remain 288 + 20.
Creating a missing KV namespace adds one entry to initialization admission;
the public 197-entry requirement already covers it.

Legacy migration reads `v00..v31`, `r00..r07` and the full 3,184-byte redo or
48-byte cleared marker from `mc-bot-kv`. Every present record is checked before
applying redo; the resulting complete bank must also satisfy duplicate-key
and per-scope limits. Unrelated keys and namespaces are untouched. The first
verified authority has reclamation set. Each migrated key is erased,
committed and checked through a new read handle; a final authority replacement
clears reclamation. Any interruption resumes from the actual committed
authority. Corruption or an unavailable read blocks reads and writes rather
than resetting defaults.

Recovery always reads and validates the complete 200-byte committed authority.
If it is byte-identical to the authority associated with the verified PSRAM
records, repeated reads, exports and scheduler polls do not reread or hash the
3,136-byte files. An authority read error, changed authority, KV mutation or
publication attempt invalidates that cache. Before a KV mutation, all five
selected files are revalidated; the next recovery after publication also
revalidates them. A successful migration can retain its fully verified arena.
Restart always starts without a cache. Read-only access is not continuous
flash scrubbing: corruption occurring on flash while the authority is unchanged
is detected at the next file verification, not by a cached read. Once detected,
it blocks reads and writes; no older bank is selected.

One recovery pass has a 10-second budget covering authority/legacy reads, file
verification or migration publication, and legacy reclamation. Deadline checks
run between storage calls; they cannot interrupt an SDK call already in progress.
A timeout before authority publication leaves the legacy records authoritative.
A timeout after publication resumes from the committed file authority, including
unfinished reclamation. Retry recovery after correcting persistent storage
errors; no reset or erase is required. An ordinary request that triggers a
migration lasting over two seconds can itself time out after migration succeeds.
Read the current value before retrying a mutation; subsequent requests use the
new authority and their normal two-second deadline.

Run the native checks without connecting a device:

```sh
make -C firmware/esp32 bot-kv-files-test bot-storage-test bot-kv-native-test
make -C internal/nativebot test spiffs-test
make -C firmware/esp32 bot-native-worker-test bot-native-host-test
make -C firmware/esp32 bot-build-test
```

The file harness interrupts file open/write/flush/readback, authority
publication and legacy reclamation, with both staged and eager NVS writes.
It also checks short writes, truncation, corrupt records, deadline and grant
fences, and retries after an unknown commit. Cache checks require zero redundant
file reads across repeated polls, revalidation after ambiguous publication, and
mutation/restart detection of latent file corruption. Migration checks cover a
slow successful full bank and timeouts during reads, publication and reclamation.
The storage worker check covers timer admission after slow migration and retry
after a migration timeout. The POSIX worker check uses the production NVS/SPIFFS
adapters for migration, full KV capacity, Lua transactions, source replacement
and restart. These commands do not deploy firmware or establish on-device
power-loss, latency or endurance acceptance.

#### Timer/reminder storage and upgrade

The first access to each scheduler family automatically migrates the deployed
checked `BTM1`/`BRM1` NVS records. Do not erase NVS or provision/format SPIFFS.
This upgrade retains all ten timer slots and eight reminder slots, across every
full bot identity and principal, including the bot-only timer reserve.
It copies deadlines, revisions, reminder IDs/text/source generations and all
pending or terminal states unchanged. A claimed timer, unknown reminder or sent
reminder stays consumed; migration never recreates delivery authority.

* `/command-bot/timers-a.bin` and `timers-b.bin`: ten 144-byte `BTM1` records,
  **1,440 bytes per bank**.
* `/command-bot/reminders-a.bin` and `reminders-b.bin`: eight 244-byte `BRM1`
  records, **1,952 bytes per bank**. Unoccupied slots in both families are all zero.
* NVS `mc-bot-timer/files` and `mc-bot-remind/files`: **72 bytes** each, `BTF1`
  or `BRF1`, one bank selector (`0=a`, `1=b`), a pending-reclamation byte, two
  zero bytes, SHA-256 of the selected file, then SHA-256 over the first 40 bytes.
  Together the double-buffered payloads need **6,784 bytes** of filesystem
  space, plus filesystem overhead and GC working space. Current KV and scheduler
  banks together use at most **38,144 payload bytes**.
* Before the first file write, the same metadata key receives a 48-byte `BTI1`
  or `BRI1` initialization marker: four magic/version bytes, twelve zero bytes,
  then SHA-256 over those sixteen bytes. A restart with this marker retries the
  intact legacy records or empty initial bank. Existing files without authority
  block access, rather than appearing as empty storage.

Every mutation verifies the selected bank, writes/flushes/closes the inactive
bank and checks exact readback before publishing its NVS authority through an
independent handle. `Committed` means that authority was committed and verified.
Migration and reminder transitions keep publication status/errors in a stack
carrier bounded to 136 bytes.
A failed or unverifiable initialization-marker commit also remains `Unknown`:
recovery must confirm the metadata before another write. The marker alone does
not publish the requested KV value, timer or reminder from speculative RAM.
A write, cancellation, deadline or readback failure after publication may be
unknown; inspect the saved state before retrying. A consumed claim is not offered
again even when its success response was lost. Each family restore publishes
one complete no-rearm merge, retaining unrelated scopes and principals.
The `BTD1`/`BRD1` snapshot layout and mandatory `--no-rearm` consent are unchanged.

Migration validates every present legacy record and duplicate name/ID before
publication. Only after the checked file authority is durable does it erase,
commit and independently verify each migrated NVS key (`t00..t07`, `r00..r01`
for timers; `r00..r07` for reminders). Reclamation remains recorded until it
finishes. Interrupted cleanup restarts from the file authority, never from
partly erased legacy records. Unrelated NVS keys and role/configuration metadata
remain untouched.

Each access rereads the committed authority. A byte-identical authority can
reuse the previously verified PSRAM records for reads, exports and polling.
Failed reads, edits and publication attempts invalidate that cache; restart
always verifies the selected bank. Mutation also verifies the active file, so
latent flash corruption blocks publication rather than being overwritten.
Cached reads are not continuous flash scrubbing. Detected corruption, unavailable
files and lost metadata fail closed without selecting an older bank.
The existing storage worker still completes KV recovery before scheduler I/O,
imports or autonomous transitions. Recovery and ownership rules are identical
in firmware and the POSIX native-host backend; rekeying never migrates old data
to the new full key.

Run without connecting or changing a radio:

```sh
make -C firmware/esp32 bot-schedule-files-test bot-schedule-native-test bot-storage-test
make -C firmware/esp32 bot-native-worker-test bot-native-host-test
```

The focused file test interrupts migration/reclamation, claims and no-rearm
publication with staged and eager NVS writes, and checks short reads/writes,
missing/corrupt files, cancellation and dirty-cache retries. The POSIX check uses
the production file/NVS adapters across restart, worker identity replacement and
restore of already consumed effects. Native tests do not measure device stack
peaks, flash endurance or RF delivery.
The public ESP32-S3 beta build uses **1,571,449 bytes of flash** and
**139,752 bytes of linker RAM** with this storage change.

#### Scope grants

The optional last argument selects `"caller"` (default), `"conversation"`,
`"bot"` or `"channel"`: e.g. `kv.put('shared', 'value', 'bot')`. DM conversation scope binds
the verified caller and bot, not an arbitrary script-selected principal.
Bot-global and channel durable access are denied by default; authenticated existing mast
CLI/web commands `bot shared on`, `bot shared off` and `bot shared` persist,
apply and read that grant. This existing grant now covers both explicitly
selected shared tiers, including named timers; channel callers still cannot
access caller, DM conversation or bot-global storage. Revocation invalidates
pending grant epochs before persistence is attempted. A failed durable revoke
is reported, but running access stays disabled; it is not a reboot-persistent
revocation until saved successfully. Regranting cannot authorize an older
invocation. Providers check the epoch before effects and before resumption.
Private caller scopes never change merely because sharing was enabled.
Arbitrary conversation threads and per-principal grant lists remain unimplemented;
ordinary shared Lua globals are not private storage.

Channel scope uses a domain-separated, full 32-byte SHA-256 identity of the
**selected, successfully decrypted native channel key**, not its one-byte wire
hash, display name, `name:` nickname or command text. The immutable
`ctx.channel.verified` and `ctx.channel.id` expose verification and that digest;
neither exposes the channel secret. Group-key verification is not individual
sender authentication: all channel members can read/replace/delete shared
values once the owner enables sharing. Nickname changes cannot create private
namespaces or obtain DM authority. Distinct channel keys and bot identities
remain isolated. Existing 392-byte KV records retain their format and old
caller/conversation/bot scope numbers; the new channel scope is additive.
Older binaries that do not understand that scope fail closed on its records.
Role-profile/source/replay journals and signed RF vectors are unchanged.

For an explicitly shared selected channel, these functions use the same tier
across names, source replacements and reboots:

```lua
local function tier()
  if ctx.channel.present then return 'channel' end
  return 'caller'
end
function board_save(note)
  kv.put('memory', note, tier())
  return 'Committed'
end
command('board-save', 'note:text:100', 'Save a shared board note', 'board_save')
function board_read() return kv.get('memory', tier()) or 'No memories' end
command('board-read', '', 'Recall the board note', 'board_read')
```

The channel path fails visibly without verified native channel authority or the
owner grant; it never silently falls back to a nickname or private scope.

`sleep(ms)` and `timer.sleep(ms)` yield the current invocation for 1..30,000 ms.
They preserve nested Lua call stacks and free the VM/radio workers for other
jobs. Each invocation admits at most eight I/O operations; timer completion
over one second late fails visibly. These sleep timers are transient, not
durable reminders; the distinct named deadline API follows below.
Source generation + invocation + operation tokens fence completions, including
cancelled/recycled slots. Source replacement cannot publish old successful
replies, but a storage mutation already in progress may have committed and is
not blindly retried or erased.

### Named persistent deadlines

The `timer.set(name, seconds[, scope])`, `timer.get(name[, scope])`,
`timer.cancel(name[, scope])` and `timer.wait(name[, scope])` APIs all yield to
the existing native storage worker. They use the same caller/conversation/bot/
channel authority rules as KV, but separate timer file banks and a
`mc-bot-timer/files` authority; scripts
cannot edit timer records through `kv`. No function name or source hash enters
the durable namespace. `timer.sleep` remains the distinct transient millisecond
API; `timer.set` takes **1..86,400 seconds** and a 1..32-byte name.

Successful operations return an immutable view with `state`, `deadline_utc`,
`revision`, `replaced` and `time_trusted`. States are `missing`, `pending`,
`claimed`, `cancelled` and `overdue`. Missing get/cancel returns `missing`, not a
fabricated cancellation. Set atomically replaces the same name and reports
`replaced`; cancel changes only a pending job. Cancelling a claimed job reports
`claimed`: cancellation cannot undo a claim or an RF effect.

Creating or claiming a deadline requires the shared clock's fresh accepted
SNTP authority. Build-day/native-role/system RTC values never suffice,
including after reboot. The deadline uses the conservative latest UTC bound
plus the requested delay, and wait cannot claim before the earliest UTC bound
reaches it. A pending wait more than **60 seconds overdue** is durably marked
`overdue` and fails without a delivery claim. Untrusted/stale time suspends a
pending wait for up to **300,000 ms continuously**, rather than failing on a
transient three-second clock publication stall. Restored trust resumes polling
the same journal revision. Each live wait also has a hard **86,760,000 ms
(24 hours 6 minutes)** resource lifetime. Either bound ending raises an explicit
error without changing the journal; an otherwise pending deadline remains
pending. A new invocation may inspect, cancel or wait again.
Cancellation/source/grant fences still run while time is
untrusted. No claim or overdue decision uses build-day/unsynchronized time.
`get` remains available while
unsynchronized and reports `time_trusted=false`.

There are at most **two pending names per bot/scope/principal**, eight legacy/
public slots and **two bot-global-only reserve slots**. Migration retains the
original public/reserve positions and 144-byte checked record format. Public writers
cannot allocate that reserve; the NVS headroom guard above also applies.
Terminal records can be reused for new names when necessary, so old status may
eventually become `missing`. Journal-wide monotonic revisions prevent an old
waiter from claiming a replacement even after a slot is reused. Revision
exhaustion and corruption fail closed. The same commit plus independent-handle
readback helper serves KV and timers; failed/uncertain/late mutations remain
explicit errors, never automatic write retries.

`wait` suspends its coroutine, polling the native journal at most every 250ms
per live wait without blocking the VM, radio dispatcher or other storage
requests. The existing four invocation slots (three for custom commands),
eight I/O operations per invocation and radio airtime budget still apply.
Replacement/cancellation invalidates a waiting revision. Source replacement
or bot stop cancels the live coroutine but does not delete its saved deadline.
There is no automatic interpreter/coroutine restart after reboot.

Before a successful wait resumes, it **durably claims** the job and verifies
readback; clock eligibility is checked again after that commit and before VM
resumption. Only one waiter can consume the pending revision. A claim is
**not proof of TX, ACK or recipient delivery**. Power loss, cancellation,
lost time trust or an ambiguous commit after the claim may suppress delivery;
the claim is never replayed automatically. This is at-most-once claim
semantics, not exactly-once RF delivery.

```lua
function arm(seconds)
  return timer.set('tea', seconds).state
end
command('arm', 'seconds:int:1:86400', 'Save a deadline')
function await_tea()
  timer.wait('tea')
  return 'Tea reminder'
end
command('await-tea', '', 'Wait for saved deadline', 'await_tea')
function cancel_tea() return timer.cancel('tea').state end
command('cancel-tea', '', 'Cancel saved deadline', 'cancel_tea')
```

`!arm 60` persists the deadline; `!await-tea` waits using its invocation's own
verified principal and copied reply route. After reboot it must be invoked
again with fresh time trust. These generic deadlines do not resurrect Lua
stacks or become automatic RF messages. Use the separate personal DM reminder
API below when a persisted message must dispatch without a surviving invocation.

`source api` reports `kv=2`; `source api storage` reports the four scopes, named
timer operations and `autonomous-reminders=1`; `source api reminders` describes
the separate message-bearing service. Native Make checks cover channel
crypto/identity isolation, cross-function and reboot retention, exact quotas,
clock quality, corrupt/uncertain journals, concurrent diagnostics, one-consumer
claims and source/cancel fences. Simulated native RF confirms one final timer
reply under the existing budget while another caller receives Pong; this is
not a physical deployment or reboot-autonomous reminder claim.

```sh
make -C firmware/esp32 bot-storage-test bot-test beta-test
make -C firmware/esp32 beta-build
```

### Reboot-autonomous personal DM reminders

The bundled Lua command surface now includes:

```text
!remind 20m check the kettle
!reminders
!cancel 17
```

The owner must explicitly enable **`bot reminders on`** through the existing
authenticated mast RF/web backend. `bot reminders` reports saved and live
state; the default is off. `bot reminders off` disables live dispatch and
advances its grant fence before persistence/readback. It **suspends**, not
deletes, still-pending durable jobs. Re-enabling may dispatch those jobs if
their deadline/route remain eligible; it never replays a consumed claim.
An uncertain revoke can leave the previous saved policy for reboot: inspect
saved/applied status rather than assuming a failed write disabled it durably.
List/cancel and their `!help reminders` / `!help cancel` entries remain usable
in authenticated private DMs with the grant off. Only new `!remind` scheduling
requires the grant; its help entry is unavailable until enabled.

Commands are ordinary protected bundled Lua functions using the public,
coroutine-yielding **`reminder.after(duration,text)`**, **`reminder.list()`**
and **`reminder.cancel(id)`** APIs. `after` accepts integer seconds or a bounded
decimal duration with one lowercase `s`, `m`, `h` or `d` suffix, from 1 second
through 24 hours. Text is 1..120 printable ASCII bytes. `after` returns an
immutable `{id, state='pending', deadline_utc}` only after commit/readback;
`cancel` returns immutable `{id,state,deadline_utc}`.
`list` returns one bounded summary of this caller's IDs, states and UTC
deadlines, or `No personal reminders`, with explicit truncation when necessary.
The initial command reply acknowledges **scheduling**, never delivery.

**Compatibility:** `remind`, `reminders` and `cancel` are now protected bundled
names, alongside the six diagnostics. Custom sets still have eight exports;
the bundled registry separately has nine. Existing custom sources using these
three names must rename their exports/command mappings before upgrade. A
conflicting source fails validation visibly; no silent function replacement
or role/identity-journal migration occurs.

Only a native-authenticated, nonlocal private DM's full sender key may create,
list or cancel its jobs. Channel/nickname/local requests are explicitly
unsupported, even with shared state enabled; Lua cannot select a recipient.
There are **eight personal-reminder slots**, at most **two pending jobs per
full caller key**. These are separate from the reserved core timer/KV banks;
there is no special owner-private-reminder priority within the public eight.
Corruption, storage/PSRAM exhaustion and grant failures are visible errors.

The `mc-bot-remind` journal reuses timer time-quality/overdue rules and the
common checked commit plus independent-readback primitive. Each fixed
244-byte versioned/SHA-256-checked record stores the full bot/sender keys,
message, UTC deadline, originating source generation and monotonic durable
ID/revision. Terminal slots may be reused when necessary; old IDs can then be
absent. Creation/transition uncertainty must be checked with `!reminders`,
not blindly retried. Existing timer/role/source/signed receipt formats stay
unchanged.

After boot, the existing storage worker scans pending jobs without restoring
a Lua invocation. A single copied offer/claim/TX mailbox connects it to the
native dispatcher. Dispatch waits for enabled bot/grant, initialized and
unblocked source, shared-radio readiness and fresh accepted SNTP. Untrusted
time simply holds the bounded on-disk jobs; there is no waiting coroutine to
exhaust. With fresh time, a job more than **60 seconds overdue** becomes
`overdue`, without a transmission.

The destination must be rediscovered and have a native-authenticated PATH
received within the last ten minutes, including **after every reboot**.
Routes are not persisted or guessed; a fresh signed advert alone is insufficient.
The recipient/native peer must supply that PATH within the deadline window.
No active route-discovery flood, automatic probe, fallback flood or RF retry
is introduced. Widths 1/2/3, own/loop exclusion, native timestamp/crypto, shared
airtime credits and the existing queued bot source are reused. One route-less
caller does not monopolize scanning: due candidates rotate.

After a restart, wait for the bot to be ready and its clock to synchronize.
Have the recipient's companion advertise, then send the bot a private
`!reminders` DM. The first reply can use a flood path while the companion
exchanges the authenticated return PATH. After the configured command cooldown,
send `!reminders` again and confirm a direct reply. Native companion logs report
this as `direct: true` with `path_encoded: 255`. Refresh the route close enough
to the reminder deadline that its accepted PATH is less than ten minutes old.
Any admitted private command, including `!ping` or `!reminders`, refreshes
contact activity. The authenticated PATH exchange and a direct reply are what
matter; a first flood reply alone is insufficient. List or ping requests do not
schedule another reminder. Do not resend `!remind` to recover a route or replay a
`sent`, `unknown` or `overdue` record.

Only after preflight does storage commit an **`unknown` one-consumer claim**
and verify readback. The dispatcher then rechecks source/grant/time/route before
constructing `Reminder #ID: TEXT` as a native encrypted direct DM. At most one
autonomous TX is outstanding, with a five-second dispatch deadline.
Confirmed queued-radio TX completion is durably recorded as **`sent`**, which
means **TX completed, not ACK or recipient delivery**. Queue rejection,
timeout, lost/late completion, failed readback, revoke, source change or reboot
after claim leave conservative **`unknown`**, never automatic replay.
Cancelling a still-pending job durably records **`cancelled`**; cancelling an
already consumed claim reports its current `unknown`/`sent` state instead of
pretending to undo RF. Already queued/on-air effects cannot be rolled back.
Pending jobs survive source replacement because they are native message data,
not executable handlers; current source/grant generations fence live dispatch.

Native Make coverage includes actual encrypted message construction, reboot
with no surviving invocation, source/time/route gates, concurrent principals,
private cancel/list isolation, quota/reserve thresholds, clock publication loss,
uncertain claim/terminal writes, failed revoke, late/failed TX and no replay.
Public beta/bot-on/bot-off/HTTPS builds pass without `.env`. The scanner reuses
the existing storage task; its eight-record scratch bank is **1,952 PSRAM bytes**,
with bounded mailbox/core/VM growth and no new task, PHY, local source or socket.
The final source tree passes `bot-storage-test`, `bot-test`, `beta-test` and
`bot-https-worker-test` with ASan/UBSan (fatal undefined-behavior checks), plus
the public `beta-build`, `bot-build-test` and `bot-https-build` selectors:

| Reminder milestone profile | Flash bytes | Static internal RAM bytes |
| --- | ---: | ---: |
| Beta | 1,446,581 | 139,592 |
| Bot enabled | 1,386,313 | 139,148 |
| Bot disabled | 1,233,565 | 135,708 |
| HTTPS | 1,583,821 | 137,348 |

These are linker sizes, not runtime peaks. No device heap/stack/NVS
endurance/coexistence result is claimed. All hardware and installed images
remain unchanged.

### Native mesh continuations

```lua
function exchange()
  local tx = mesh.send(mesh.compose{kind='dm', text='question'})
  local ack = mesh.wait{kind='ack', timeout_ms=5000}
  local packet = mesh.wait{kind='text', prefix='answer:', timeout_ms=5000}
  return packet.text
end
```

`mesh.compose{kind='dm', to=ctx.sender.public_key, text=...}` creates an opaque
native draft. `kind` and `to` may be omitted; the current authenticated
DM caller is always the default destination. Owner `bot destination SLOT KEY64`
adds up to four full-key destinations (`SLOT` 1..4); `bot destination SLOT off`
revokes one, and `bot destination SLOT` reads it. Additional sends require a
fresh authenticated cached direct route (at most ten minutes old), preserve
its encoded 1/2/3-byte width, and never fall back to flood. A targeted verified selected-channel invocation can
instead compose `{kind='channel', to=ctx.channel.id, text=...}`; `to` is optional,
but cannot select another channel. A nickname supplies no private DM authority.
The configured native channel key, not its wire hash or display name, owns
channel encryption. `{kind='trace', route='2:aabb'}` creates a direct TRACE
draft; omit `route` only when the invocation has a valid inferred route.
Drafts bind to their invocation/generation, are single-submission, and cannot
be reused by another caller or edited as raw bytes. Native code supplies
timestamp, bot identity, encryption, routing and shared scheduler admission.
Unknown fields, arbitrary bytes/types/ungranted destinations and caller-supplied
flags/keys are errors, not ignored hints. Text must be bounded printable ASCII.

`mesh.send(draft)` yields until the real queued TX terminal result, not just
admission. Its immutable result distinguishes `queued`, `transmitted`,
`acknowledged` and the native radio `job` ID. TX success is not MeshCore ACK
or application delivery. Native TX failure, deadline and unknown outcomes
raise visible errors; no automatic retry occurs. A queued/in-flight operation
may already have taken effect at cancellation, and cannot be claimed undone.

`mesh.wait{kind='ack'|'text'|'channel'|'trace', timeout_ms=N}` yields for 1..30,000 ms.
ACK waits refer to the most recent send in this invocation and never assert
an authenticated individual identity. Text waits accept only later verified
native DMs from this invocation's full caller key by default. Text filters are
`prefix` **or** `exact` (1..32 bytes), `from` (that caller or an owner-granted
peer to which this invocation has already sent),
`path_width` (ordinary widths 1/2/3) and `route='flood'|'direct'` (including
their scoped equivalents). Unsupported fields/types fail explicitly.
Owner `bot channel-wait on` separately enables `kind='channel'` for an invocation
on the selected verified channel. It supports the same text/path filters, but
rejects `from`: the result has `authenticated=false`, a descriptive `nickname`
and no `from` key. It cannot forward a received channel packet as an authenticated
DM. Both mesh grants default off, are copied into immutable `ctx.grants`, and
are rechecked at native admission/completion. Revocation fences pending epochs
even if persistence fails; regrant does not revive them.
Whole-source activation cancels the old generation's invocations and I/O.
The saved owner mesh policy is node-wide and remains configured; fresh
invocations in the new generation take a new context and revalidate that
policy. This is not an independent permission/rollback boundary per export.
Direct RX does not expose the already-consumed route, so a
path-width filter requires known flood-path metadata.

Custom or stateful channel commands require an explicit target (`!@KEY8
COMMAND` or a full public key). Untargeted read-only native channel queries
use bounded jitter and response correlation; this prevents unrelated replies
from being mistaken for a response to the query.

Four copied messages and a non-evicting 16-message canonical dedup history are
retained per invocation from admission, including early responses. Dedup binds
timestamp/text/full authenticated key, not retry bits, padding or path copies.
The first copy supplies metadata; unmatched copies remain available to later
filters. Capacity/dedup saturation drops new copies and reports `truncated`;
no matching retained copy plus overflow fails visibly. Ungranted/uncontacted peers, local
reflections, own ciphertext and incoming `!commands` are excluded.
The immutable native-minted packet userdata carries `kind='text'|'channel'`, `text`,
authenticated `from` or unauthenticated `nickname`, `timestamp`, `authenticated`, `truncated`, `path`, `path_known`,
`path_width`, `route_type`, `measured`, `rssi_dbm`, `snr_db`, and preliminary
`forwardable`. Reading/forwarding a handle from another invocation fails.
No mutable RX buffer, secret or packet-pool object crosses the VM boundary.
Timestamps from different peers/channel nicknames are not compared as if they
shared an authenticated clock. A matching later copied message proves its
native DM sender or channel membership, not application-level request/reply
correlation; use an application-specific text filter when that matters.

`mesh.trace([width_hex])` and ordinary `trace([width_hex])` yield and return
bounded trace-result text, using only a valid explicit/inferred direct route.
Alternatively, send a TRACE draft for the queued/TX result, then
`mesh.wait{kind='trace', timeout_ms=5000}` for its copied response. Exactly one
unconsumed TRACE is allowed per invocation; correlation starts before TX and
retains an early response for at most 30 seconds. Native tag/auth/route/width
and complete hop count must match. The result includes `correlated`, `hops`,
`path_width` and immutable numeric `snr` entries plus bounded summary `text`.
TRACE/ACK correlation is **not cryptographic sender authentication**.
Explicit TRACE routes use native widths 1/2/4/8; width 3 is rejected. Inferred
TRACE uses only ordinary widths 1/2; ordinary width 3 is never reinterpreted as
TRACE width 4.
Late, duplicate, wrong-tag and cancelled-generation responses cannot resume a
new operation. Neither form has a flood fallback.

`mesh.multitrace([width_hex], [count])` sequentially awaits 1..3 native traces
(default 2), sharing the same eight-I/O invocation limit and native airtime
credits. It returns one bounded numbered summary with explicit `; truncated`
when needed; a failed step raises its index and native error, never partial
success disguised as a complete observation. Normal nested Lua calls retain
the original principal and route:

```lua
function multitrace()
  return mesh.multitrace('1:42', 2)
end
function testall()
  local greeting = ping()
  local paths = multitrace()
  sleep(10)
  return greeting..'; '..paths
end
```

`advert()` / `mesh.advert()` yield until actual native advert TX completion;
the existing 15-minute advert gate and airtime budget still apply. Thus
`testall()` can call `ping()`, `trace(...)`, `sleep(...)` and `advert()` in order,
then aggregate results into one final reply. Pending native operations free
the VM worker to serve other invocations. Native ACK/TRACE ingress is guarded
before calling the pinned decoder, and completions are generation/job/operation
fenced; transient RF results do not authorize KV scopes or supply caller identity.

### Explicit pair-DM forwarding

`mesh.forward(packet)` implements one specific authorized operation, **not
transparent packet relay**: re-originate the latest retained follow-up DM to
the owner-configured destination, encrypting afresh as the bot. Text is
`Fwd FULL_SENDER_KEY: ORIGINAL_TEXT`; the 70-byte attribution leaves at most
92 original ASCII bytes. This preserves verifiable bot origin and names the
authenticated original principal without spoofing that sender. There is no
Lua destination argument, raw ciphertext injection or DM-to-channel path.

The same authenticated mast RF/web command backend accepts
`bot forward FROM64:TO64`, where both keys are full, distinct, nonzero public
keys and neither is this bot. The grant is default-off, one pair, persisted
in its own checked-shape/readback NVS record; role-profile/identity/source
journals and signed status/receipt formats are unchanged.
`bot forward`, `bot forward from`, `bot forward to`, and `bot forward off`
provide bounded status, key readback and revocation. This does not introduce a
new RF protocol or depend on the repeater role.

Native admission checks the current pair and epoch again, the packet's
invocation ownership, authenticated sender and age under 30 seconds, and a
destination contact with an authenticated PATH received within ten minutes.
No route discovery or flood fallback is attempted. Ordinary direct routes
retain widths 1/2/3. Own/local reflections, a visible incoming path containing
the bot, a destination path through the bot, and text beginning `Fwd ` are
ineligible. A 16-entry, 120-second native digest ledger claims each attempt
before enqueue, across overlapping invocations; full ledger means denial.
Failed/uncertain transmissions are not automatically retried, nor can a stale
handle forward the text from a newer wait. The returned TX result is not
delivery proof; a subsequent ACK wait is still only a correlator.

Revocation/change disables the live pair and advances its epoch **before**
durable write/readback. Even a failed revoke fences old invocations; regrant
does not revive them. A failed persistence operation may leave the previous
durable policy in place for reboot: confirm saved/live readback rather than
claiming a durable revoke. Cancellation cannot undo already queued/on-air
effects. These bounds are not a general transit-loop detector or an arbitrary
destination/channel grant system; unsupported forwarding remains an error.

### Opt-in configured HTTPS

`rpc.call("home", operation, args)` now yields to a separate native HTTPS
worker. It does not block the radio dispatcher, VM worker or durable-KV worker.
The bundled `home` operations use the `meshcore-bot-service` `/v1/rpc` contract:
`health` with `{}`, `echo` with `{text="..."}`, and `weather` with `{place="..."}`.
The native configuration separately allowlists operations; the Lua registry
cannot grant network access. An authenticated DM plus the saved/live
`bot home on` grant is required. Hashtag/nickname-only requests are denied.
`bot home off` immediately invalidates pending network grant epochs; source
replacement and bot stop fence/cancel old work and reject late completions.
Re-enabling the grant does not lend a new epoch to an older suspended job.
Revocation takes effect in RAM before persistence is attempted; a failed
durable write is reported and must not be treated as a reboot-persistent
revocation, but it cannot leave the running provider enabled.

```lua
function health()
  local r = rpc.call("home", "health", {})
  if not r.ok then return "Home: "..r.error.code end
  return "Home: "..r.result.status
end

function forecast(place)
  local r = rpc.call("home", "weather", {place=place})
  if not r.ok then return "Weather: "..r.error.code end
  return tostring(r.result.temperature_c).." C; age "..
         tostring(r.result.source_age_seconds).."s"
end
```

Results are read-only typed tables: `{ok=true,http_status=200,result=...}` or
`{ok=false,http_status=N,error={code=...,message=...}}`. Transport, policy,
quota and timeout failures are explicit errors, never fake weather.
The weather result includes source, location, country, temperature, weather
code, observation UTC timestamp and age. The native decoder validates those
fields, finite numeric ranges, timestamp shape/calendar date, UTF-8 text,
the 7,200-second service age ceiling and exact object keys/no duplicates.
The service verifies upstream observation freshness and returns the
observation timestamp and age.
Ordinary radio replies retain their existing bounded printable-ASCII contract;
`rpc.text` below safely escapes non-ASCII bytes without silently changing or
dropping the provider's location.

### Bundled weather and service commands

The protected ordinary Lua commands `!weather [place]` and
`!service <name> [arguments]` call the native HTTPS worker.
**A place is required**: `!weather`
without one gives an explicit error because no default is configured.
`!weather New York` and `!service weather New York` call exactly the same
Lua function and native `home/weather` operation.

| Command | Validated arguments and result |
| --- | --- |
| `!weather PLACE` / `!service weather PLACE` | 1..80-byte place, no leading/trailing whitespace. Returns provider location, Celsius temperature, numeric weather code and service-reported observation age in seconds. |
| `!service health` | No arguments. `Home health: ok` reports this service process, **not** weather-provider or radio delivery health. |
| `!service echo TEXT` | 1..120-byte text; the native decoder verifies the service echoed the request. `Home echo: TEXT` is bounded/escaped as needed. |

For example, a successful real response can produce
`Weather Fixture City: 12.5 C; code 3; age 900s`. The name is the provider's
selected location, not a silently substituted caller string. Celsius uses
the existing bounded numeric formatter (up to six significant digits);
weather code is the service's numeric Open-Meteo code, not a guessed
description. Age is the service's measurement at response generation, not
an independently established bot observation time. The service enforces its
two-hour/future-time policy; native decoding also rejects an age outside
0..7200 or malformed/missing/nonfinite fields. No result cache, invented
forecast, retry or fallback location is added.

Both commands require a **nonlocal authenticated private DM**, the saved/live
default-off `bot home on` grant, the configured HTTPS endpoint, fresh TLS time
trust and the separately owner-configured operation mask. Channel group-key
verification or a `name:` nickname cannot supply individual/network authority,
including on opted-in `#example1`. Enabling weather on the host alone does not
grant it in firmware. `service` accepts only literal `health`, `echo` or
`weather`; it cannot name a URL, host or arbitrary RPC operation. Existing
configuration/grant management and transfer framing are unchanged.

Provider/transport errors become bounded `Home OP error: CODE; MESSAGE`
replies, including `weather_disabled`, `provider_stale`, `provider_timeout`,
`provider_unavailable`, `permission_denied`, `invalid_response` and native
`deadline`/transport/clock errors. Grant/admission/schema failures may instead
raise the existing native `Error:` reply. Thus a malformed/oversize response
or offline service cannot look like successful weather. Error codes are
retained even when a long message is explicitly truncated.

The immutable `rpc` table additionally exposes `rpc.text(text, byte_limit)`
for reply formatting, not networking. It accepts 1..256 non-NUL bytes and a
limit from 16 through `ctx.limits.reply_bytes`. Printable ASCII is retained,
backslashes are doubled, and other bytes are escaped as uppercase `\xHH`
(UTF-8 is preserved as its bytes, not transliterated). If needed it truncates
only between complete escape tokens and appends `...[truncated]` **inside**
the byte limit. Weather limits the escaped location to 80 bytes, echo to 120,
and error messages to 80. All required weather numbers remain present;
smaller reply capacities fail explicitly. The final reply is at most 162
printable bytes including labels and truncation markers.

`rpc.call` yields the invocation to the existing network worker; local ping,
utilities, notes and reminders continue while it waits. It uses the original
full-key principal and generation/job/operation token. Grant revoke/regrant,
source replacement, disable and deadline fences apply before native effects
and resumption; an old result cannot resume a recycled caller slot. An HTTPS
success only produces reply text for the existing native radio queue.
It does **not** mean queue admission, terminal RF TX, ACK or delivery.
Nested `reply(service(...))` still waits for native terminal TX separately;
failed/late TX is not retried as another caller.

**Migration and limits:** `weather` and `service` are newly reserved; rename
conflicting custom handlers before upgrade (the older raw-RPC example above
is now `forecast`). Firmware supplies a separate **1,501-byte** trusted Lua
chunk alongside core/utility definitions, under the same cumulative parser,
compile/init, heap and invocation limits. There are 21 built-ins and still
eight custom exports; editable uploads remain at most 4,096 bytes. No journal,
identity, socket/task count, HTTPS timeout or grant is changed.
`source api services` reports the commands, private scope and argument bounds.

**Developer checks:** Make ASan/UBSan
`bot-https-test`, `bot-https-worker-test`, `bot-https-tls-test`, `bot-test`,
`bot-storage-test` and `beta-test` pass. The loopback test runs the real Go
reference service with short-lived generated test TLS material and a
loopback-only weather-provider fixture. It exercises bundled Lua through
verified CA/hostname TLS and the native decoder, with service weather enabled
and disabled, plus unavailable/timeout/malformed/oversize/stale provider
responses. Test TLS material/processes are cleaned up; no operator credentials
or live endpoint is used. This is a controlled fixture, not live public weather
or target RF acceptance. Worker tests cover concurrent local commands, copied
principal ownership, cancellation, late results and separate terminal TX.

Public HTTPS/beta builds pass: respectively **1,599,965 / 137,348** and
**1,462,665 / 139,592 bytes** flash/static internal RAM. These are linker/host
results from an earlier source baseline, not current image sizes or device
heap/stack measurements. Measure socket coexistence and RF replies on the
selected device/profile.

### Native HTTPS configuration and limits

Use [NETWORK_API.md](NETWORK_API.md) for current ESP32 and production native-host
endpoint configuration, HTTP/JSON/RPC results, package fetch and transport limits.
Endpoint addresses, CA/token and approved operations are native owner
configuration, never Lua-supplied URLs or secrets.

An optional compiled `home` fallback is also supported. An operator-owned header selected with
`ONCHIP_BOT_HOME_CONFIG_HEADER` may define:

| Native define | Bound/purpose |
| --- | --- |
| `ONCHIP_BOT_HOME_ADDRESS` | Fixed unicast IPv4 address; no DNS call |
| `ONCHIP_BOT_HOME_HOST` | Up to 253 bytes; TLS SNI/hostname verification and HTTP Host |
| `ONCHIP_BOT_HOME_PORT` | 1..65535, default 443 |
| `ONCHIP_BOT_HOME_CA` | CA PEM, at most 4096 bytes; never insecure TLS |
| `ONCHIP_BOT_HOME_TOKEN` | 32..256 printable non-whitespace ASCII bytes |
| `ONCHIP_BOT_HOME_OPERATIONS` | Bitmask: health=1, echo=2, weather=4; default none |

Keep operator headers out of version control and their contents out of logs. A plain
`bot-https-build` uses no real endpoint/credential and cannot enable the live
grant. Runtime endpoint/CA/credential editing uses the authenticated native
backend below; CA/token staging requires encrypted RF or the protected host
Unix socket, not plaintext HTTP.
Credentials compiled into a firmware image are not protected against a
physical flash read. Scripts cannot choose a URL, header, port, credential,
redirect target or plaintext downgrade. No proxy is used.

The HTTPS-enabled image also accepts **owner-authenticated** native mast
commands under `bot https`; this is the existing administration backend,
not a new service or unauthenticated management endpoint. Certificate and token
chunks require encrypted RF administration, not the plaintext HTTP web console.
`bot https` lists
the verbs. `bot https status` reports only endpoint/mapping counts, pending
staging and epoch; it never returns a CA, token or submitted command. Use:

| Command suffix after `bot https` | Effect |
| --- | --- |
| `endpoint NAME IPV4 HOST PORT PATH get\|post\|both` | Stage a named, fixed-IP HTTPS endpoint, verified TLS hostname, port, fixed path and allowed methods. Replacing an endpoint clears its staged CA/token; load them again before commit. To configure the bundled weather/service commands at runtime, use `home` with `/v1/rpc post`. |
| `ops home MASK` | Approve a runtime `home` operation mask before commit: health=1, echo=2, weather=4 (sum selected bits, 1..7). An uncommitted or zero mask grants nothing. A committed runtime `home` overrides the compiled one; removing it restores an independently configured compiled `home`, if present. |
| `ca NAME HEX` / `token NAME HEX` | Append **one** even-length hex chunk of at most 128 characters (64 bytes) to the named staged endpoint. Encode the PEM as bytes, including its newlines; do not paste PEM/token text or echo hex commands in logs. The complete CA is at most 4096 bytes and the printable non-whitespace token 32..256 bytes. |
| `rpc SERVICE OPERATION PATH` | Stage an exact service/operation-to-path mapping. A service must be a named endpoint or a configured legacy `home`. |
| `drop NAME` / `unmap SERVICE OPERATION` | Remove a staged endpoint (including its mappings) or one staged RPC mapping. |
| `discard` / `commit` | Drop all pending edits or validate and persist them, then revoke the previous network configuration epoch. |

Names are lowercase identifiers; paths are fixed absolute paths with no query,
fragment, traversal or repeated slash. Endpoint IP/hostname/CA/token and
complete mappings are checked on commit. The checked, double-slot
`/command-bot/network-*` records live in SPIFFS and survive reboot; incomplete
staging does not activate or confer network authority. Do not enter secrets on an untrusted terminal, in a shell history file,
through the HTTP web console or a logged proxy, or in source control.
The mast command has a 162-byte text limit (including
`bot https `), so longer CA/token values must be split into successive
96-hex-character-or-smaller chunks (including a maximum-length name). Successful staging replies are generic
and status is redacted; neither is proof of TLS or RF success. No plaintext
HTTP, DNS lookup or arbitrary caller-provided URL is enabled.

After a verified commit, the owner must explicitly use `bot home on` to grant
network use; `bot home off` revokes it. The `bot home` readback shows
saved/applied grant and clock/configuration state without exposing credentials.
Only nonlocal authenticated private DM calls may use that grant; a selected
hashtag, nickname or staged configuration cannot grant it. Enabling also
requires fresh trusted SNTP on ESP32 or kernel-synchronized time on the native
Linux host. On probe-guarded ESP32 builds it also requires a passing
native HTTPS probe. Stored configuration is not a substitute for physical
network/TLS/RF acceptance or a protection against physical flash extraction.

Approved `http.get`, `http.post`, `rpc.call` and bounded `json` are available in
the ESP32 and production native-host Lua runtime. See [configured network API](NETWORK_API.md)
for a runnable setup, numeric limits and streamed package-fetch commands.

Native administration can enqueue one approved JSON GET on the same HTTPS
worker with `commandBotService().requestOwnerFetch(name)` **after authenticating
the owner**. `pollOwnerFetch(result)` returns a single bounded JSON response
(at most 2048 bytes), HTTP status and explicit error code; `cancelOwnerFetch()`
ends the caller's wait and discards any late completion. It never accepts a
URL or adds a connection task. The authenticated `source fetch package SHA256`
command instead streams 1..4096 raw source bytes through the shared owner slot,
checks the expected hash and runs normal metadata/schema/runtime/Lua readback
validation before the existing source journal and activation path.
Missing aliases, HTTP failures, cancellation, hash/schema mismatch or
configuration revocation leave the active source unchanged. Source/package
import remains behind authenticated owner administration; a successful GET
alone does not install or trust remote source.

This deliberately uses a configured address **with an independently verified
TLS hostname**, rather than the SDK's blocking `hostByName()` path (which can
wait about 31 seconds). Address changes require updated native configuration.
The pinned Arduino 2.0.17 S3 SDK has `CONFIG_MBEDTLS_HAVE_TIME_DATE` **disabled**:
its successful CA/signature/hostname handshake does not establish certificate
date validity. The native provider therefore requires a fresh accepted SNTP
sample from the shared clock service, never a build-day, native-role or system
RTC fallback. Clock publication must be at most three seconds old and the
sample age at most twice the configured SNTP interval. The whole-second SNTP
mailbox gives a conservative one-second UTC interval; both ends must fall
inside the certificate validity window. This assumes the configured SNTP
server/network is trusted; ordinary SNTP is not cryptographically authenticated.
The nonblocking clock snapshot gets at most four attempts with 1ms yields
between them. Persistent publication contention, stale publication, absent or
expired SNTP and an out-of-range epoch each fail closed with a distinct reason.

After the SDK's chain/signature/hostname verification, the provider reads the
retained peer certificate chain through `getPeerCertificate()`, validates every
presented leaf/intermediate date, and intersects those dates with the configured
anchors' date window. On a CA change, the bounded native CA bundle is parsed and
freed **before** creating the TLS client; its window and exact PEM bytes are
cached in a single 4,120-byte PSRAM slot. Cache allocation failure uses an
uncached pre-connect parse without an internal-RAM cache fallback. The SDK still
verifies the chain/signature/hostname against the request's CA on every connect;
cached dates are checked against fresh SNTP at every I/O boundary. This
conservatively rejects even an unused
expired anchor or extra expired presented certificate. Each chain is capped at
eight certificates/16 KiB encoded data; malformed dates, missing retained peer
certificates, partial CA parsing and empty/invalid validity intersections fail
closed before any HTTP credential/body write. Peer renegotiation is explicitly
disabled. Fresh SNTP and the established certificate validity window are
rechecked at I/O boundaries and after response decoding, so expiration or lost
time trust cannot produce a successful RPC result. Failures report an explicit
certificate/clock message (`tls_policy` for I/O trust checks); no automatic
retry or insecure fallback occurs. The profile refuses to compile without
SDK peer-certificate retention. Defining a date macro only for application
headers would not repair the prebuilt SDK and is not used.

`bot home on` also rejects an unsynchronized/stale clock; `bot home` includes
`clock=0|1` and does not report the grant applied while time trust is absent.
An already-saved grant after reboot does not bypass per-request time checks.
The provider sets a 1-second
connect/write socket timeout, an 8-second TLS handshake timeout and a 15-second
end-to-end result deadline, including queue time. Generation/grant/deadline
checks run before effects and between I/O calls. SDK calls are not preemptible:
deadline or cancellation can be reported after the current bounded SDK call
returns, never as a successful late result. No automatic HTTP retry occurs;
a partial write/cancellation may have an unknown remote outcome.

Four bounded network mailboxes feed one connection; at most two requests per
authenticated caller and four globally per minute are admitted. The invocation's
existing eight-I/O limit also applies. Echo is narrowed to 256 bytes and place
to 80 bytes. Requests use a random native transport-instance nonce plus generation/job/operation as the service
request ID without exporting the caller's key. Response headers total at most
2048 bytes, each line 511 bytes; JSON bodies at most 2048 bytes, nesting at most
four levels and at most 48 structural components. Lua decoding limits each
string value or object key to 1024 UTF-8 bytes; an oversized string returns
`invalid_response` even if the total body fits. Only explicit Content-Length
JSON is accepted. Redirects, transfer-encoding/chunks, compression, duplicate
length/type headers, malformed/incomplete bodies and unrecognized shapes fail
visibly. cJSON is already supplied by the SDK; native tests use its installed
compatible runtime and the SDK headers, without a package install.

`Xiao_S3_WIO_onchip_https` explicitly reserves `BOT_NET_SOCKETS=1` by reducing
physical KISS clients from four to three. All other HTTP/companion/MQTT/socket
reservations stay unchanged: **16/16 SDK sockets**, not an increased SDK limit.
`Capacity.h` rejects HTTPS with four physical KISS clients or insufficient SDK
capacity. Baseline/field profiles remain four-client/no-network-slot builds.
**The old four-source `radio_session=per_role` Go configuration cannot fit a
three-slot mast.** For that workload, configure `radio_session=required`,
verify MKISS negotiation and count remaining direct/reserved clients against
physical capacity. Four logical ports include controller port 0; extra roles
still need sockets. `auto` can fall back only if its per-role links fit; it
rejects an over-capacity fallback. Use [host capacity/configuration](../../HOST_GUIDE.md#configure-the-go-host)
when selecting roles. Updating source alone does not update a running host
or modem.

TLS runs on an additional 16 KiB internal task stack at priority 1.
The provider lazily allocates a 3585-byte PSRAM workspace, with no internal
fallback; SDK TLS/cJSON and control storage allocate separately. TLS admission
requires the SDK's two simultaneous record-buffer allocations to fit while
retaining the unallocated portion of its 68 KiB TLS budget and 32 KiB for
radio/network work, including allocator overhead. The probes are released
before connect; no additional large contiguous handshake blocks are assumed.
Local memory rejection reports `heap_unavailable` rather than a network error.
See [the TLS memory budget](../esp32/TELEMETRY.md#a-write-fails-without-an-http-response)
for SDK allocation sizes, measured working sets and telemetry status.
Before allocating a client or opening a socket, it allows up to 1000ms for
deferred network cleanup to restore those reserves, checking time trust while
waiting. Unrecovered reserves fail visibly; this is not an HTTP retry and does
not lower either threshold. These are conservative admission thresholds,
**not measured handshake costs or a guarantee of feasibility**. Numeric worker logs expose elapsed time, final
free internal memory, global low-water memory and task stack headroom.
Those do not replace on-target peak/fragmentation/socket measurements.

```sh
make -C firmware/esp32 bot-https-test bot-https-worker-test bot-https-tls-test bot-https-device-test
make -C firmware/esp32 bot-https-build
```

Native checks exercise malformed HTTP/JSON, PSRAM failure, denied capabilities,
quotas, deadlines, live grant revocation/re-enable, source cancellation,
concurrent diagnostics and per-coroutine result attribution. A loopback-only
TLS fixture runs the actual committed Go service and verifies health/echo,
weather-disabled/unauthorized responses and wrong CA/hostname rejection.
The native OpenSSL transport is test-only, not the ESP transport.
The device-path harness separately compiles the **production ESP
`SecureTransport` and clock implementation** against a narrow SDK boundary
that deliberately accepts invalid certificate dates. It rejects expired and
future leaf/intermediate/CA certificates, invalid/missing/cyclic chains,
partial CA parsing, build/native-only time, stale SNTP, loss of trust during
handshake and expiration/trust loss during a response. It checks zero HTTP
credential writes on initial policy failure and disabled peer renegotiation.
It also covers transient/persistent snapshot contention, both exact memory
thresholds and one-byte-below failures, bounded reserve recovery and time-trust
loss during admission.

## Execution and installation boundaries

The [API tables](#lua-api-by-capability) describe available functions.
The [roadmap](../../ROADMAP.md#next-directions) describes potential additions.
Source generation is the unit of installation,
cancellation and quarantine. An uncertain send is never automatically replayed.

Installation uses the authenticated management backend below, independently
of repeater selection. Network and filesystem operations run on native workers
outside Lua execution.

## Hooks for the shared authenticated RF/web backend

The common native management backend reaches the instance through
`commandBotService()` on the dispatch task. It belongs to the separately
addressable `management` service and its durable `mc-onchip/management`
identity, not the optional repeater or the bot's chat identity. Management
starts independently of all selectable roles and remains addressable when
the bot is disabled. Loading/compiling/activating bot source does not require
repeater, room or companion roles to be enabled.

The beta RF adapter reuses native `ANON_REQ` login (trusted full-key
ACL or administrator password), followed by encrypted `TXT_MSG` CLI framing.
`meshcore_py` can send the CLI text unchanged. Native MeshCore has no arbitrary
file-chunk transfer or compression, and `PAYLOAD_TYPE_MULTIPART` is ACK-only;
do not repurpose it for script chunks. With a 184-byte packet payload,
practical CLI text is approximately 162 bytes **before transfer metadata**.
Longer scripts use the bounded numbered-chunk extension in the opt-in beta
management backend, which RF and web share. Existing signed role-selection
and status/receipt behavior is preserved. The underlying bot hooks remain
transport-independent and are not themselves authorization boundaries.

The transport-independent hook sequence is:

1. Authenticate/authorize the operation, bind target/generation/hash, enforce
   upload limits and finish writing **uncompressed** source to SPIFFS
   `BotStagedSourcePath` (`/command-bot/staged.lua`). Flush and close the file;
   keep it immutable until staging completes.
2. Call `stageSourceFile(expectedSize, expectedSha256)`, supplying the exact
   uncompressed byte count and 32-byte SHA-256 from authenticated metadata.
   This enqueues the file read and compile/init validation on the bot worker.
   Call `pollSourceResult(result)` after `loop()` reports completion; its
   operation is `StageFile`, with `sourceReadMs` separate from VM timing.
3. On a successful staging result, call `activateStaged()` and poll its result.
   The active source changes on the worker, between requests. A failed
   admitted staging job invalidates any earlier candidate; it does not replace
   the active source.

The loader only opens that fixed path, read-only, on the existing mounted
SPIFFS. It never mounts/formats storage, writes files, or selects arbitrary
paths. It checks 1..4,096 bytes and the exact file size before reading, uses
at most 256-byte reads into the existing staging buffer, checks EOF and the
digest, and rejects incomplete reads, changed content, growth, and late
results. A separate 100 ms source-read deadline covers opening, reading,
closing and hashing; individual SDK filesystem calls are not preemptible.
Compilation then uses the same independent phase budgets as RAM staging:
330 ms aggregate load followed by at most 50 ms initialization; retained
teardown runs no script finalizers.
Activation uses the verified RAM snapshot, so subsequent file edits/removal
cannot change its code. Filesystem access is never exposed to Lua.

`stageSource(source, length)` remains available for trusted in-memory callers.
Neither entry point accepts a codec parameter or performs decompression.
Codec bytes never request decompression; only bytes that pass normal Lua
text-only validation can proceed.

Calls are dispatch-thread-only and reject busy/invalid requests explicitly.
Do not invoke them directly from an HTTP callback, treat a staging return as
installation success, or authorize script installation merely because an
existing signed role command verified. Durable program storage, generation handling and common RF/web installation
receipts belong to that backend. The beta loads its verified durable slot
through these same hooks after boot; it does not define another RF login
protocol.

## Developer checks

Run the native bot, storage and administration suites:

```sh
UBSAN_OPTIONS=halt_on_error=1 make -C firmware/esp32 bot-test bot-storage-test beta-test \
  TEST_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
```

Tests cover packet handling, role coexistence, source validation and recovery,
per-caller attribution, yielded operations, grants and storage failures.
Compiler flags select separate object directories. The pinned ref10 Ed25519
C objects disable UBSan `shift-base` for signed carry shifts; ASan and the
remaining UBSan checks apply throughout.

For profile sizes and allocation lifetimes, use
[device resource budgets](../esp32/resource-budget.md).
