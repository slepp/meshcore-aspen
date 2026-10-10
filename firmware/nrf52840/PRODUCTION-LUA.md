# Production Lua on Pine

Pine runs a native repeater and a separately identified production Lua bot on
one modem. This nRF52840 profile supports Lua and shared-radio roles; it does
not include Wasm, WiFi or HTTPS. Room and observer services are not required.
The [public Pine bundle](https://ve6slp.ca/projects/meshcore/#downloads) is an
application-only update for a node with initialized identities and credential
records. It preserves InternalFS and QSPI Lua state; it cannot provision a blank
board. Optional BLE updates require accessible USB recovery after an
interrupted transfer; see [BLE field updates](BLE-FIELD-UPDATE.md#aborted-transfer-and-preservation-checks).

Use a MeshCore mobile app connected to another companion radio to administer
Pine over LoRa. Open **Pine-Relay's authenticated administrator console** and
send `bot status`, `bot limits`, then `source status`. Send ordinary encrypted
DMs to **Pine-Bot** for `!ping` and `!help`. Bundled source also provides
`!remember KEY TEXT` and `!recall KEY`; installed packages select their own
custom handlers.
Advertise the companion first so Pine learns its full public key. Repeat this
after Pine restarts: its contact and route caches are volatile.

For a host companion using a shared modem, the existing RF administration
tool can use Pine's native console without command tags or automatic replay:

```sh
python3 tools/hardware/admin.py --gateway MODEM_HOST \
  --target PINE_RELAY_KEY --seed-file /private/companion.seed \
  --untagged --no-command-retry command 'bot status'
```

The companion must have native administrator permission, or supply the saved
administrator password using `--password-file /private/password`. Keep both
files private (`0600`). A timeout leaves the command outcome unknown; inspect
status before repeating a mutation.

Use `bot membership SLOT` to inspect any of the eight simultaneous group
memberships, and `bot membership SLOT #tag|public|private NAMEHEX KEY32|off`
to change one. Public is a single membership and starts with commands denied.
`bot access dm|native|SLOT` reads the native command default and override count;
`bot access CONTEXT COMMAND MASK` edits bare/addressed execution, replies and
storage access. `bot thread CONTEXT NAME 0|16|32|48|inherit` narrows read/write
access for a named storage thread without changing the full native caller or
channel identity. `native` selects bot-owned events; their global event/shared
grants remain required. These settings save and apply without a reboot. See the
[shared channel policy examples](../runtime/BOT_RUNTIME.md#channels-and-native-command-policy)
for masks, private-key handling and the grants that remain required.
Lua thread descriptors and aggregate default/thread quotas are covered in
[named storage threads](../runtime/BOT_RUNTIME.md#named-storage-threads).

Use `bot radio` to inspect the Lua and shared modem's queued packets, confirmed
transmissions, failed/uncertain transmissions and RF time. A confirmed
transmission is not a delivery receipt. `bot status` keeps Lua readiness
separate from its last reported error.

Build the `nrfmast_fleet_lua` image for the XIAO nRF52840/Wio SX1262:

```sh
make -C firmware/nrf52840 build ENV=nrfmast_fleet_lua \
  LUA_ARCHIVE=/path/in/your/project/lua-5.5.1.tar.gz
```

Preparation verifies Lua 5.5.1 SHA-256
`1c4b4068d67061f2a2231ad2b5422e77acea1487ea9890f6320af614f4373dce`.
The build uses MeshCore companion-v1.17.1,
`d92964352441e53b93e8667b802e04f6e072b39e`.
The measured build uses Nordic PlatformIO 11.0.0 and
`framework-arduinoadafruitnrf52@1.10701.0+sha.d5413016`. Production Lua links
full newlib, not newlib-nano: Lua integer formatting and VM diagnostics require
64-bit formatted I/O, and allocation uses the full C library's `realloc`.
This profile uses the existing software crypto implementation. Its hash inputs
can live in flash or RAM, and its per-call hash/encryption contexts can run on
the separate radio and storage tasks. The inherited CC310 path does not
produce the bundled source's correct hash from flash.
The normal
application/bootloader/SoftDevice/InternalFS boundaries remain unchanged.
Select the application's `firmware.uf2` or DFU `firmware.zip`, not a full-device
replacement. Installation is RF-capable; coordinate the radio window first.

## Identities and nearby BLE

The repeater retains its identity, preferences, password, ACL, repeat setting
and PHY. The Lua bot uses the **existing bot identity**. The fleet image refuses
missing or corrupt `/_main.id` and `/_nrfbot.id`; it does not create replacements.
Keep private identity preservation files uncommitted with mode `0600`.
Never erase InternalFS when updating the application.

Native repeater administration is the primary management connection. A native
ACL entry or administrator login grants access to `bot` and `source` commands.
For optional private-DM administration, configure `bot owner KEY64` from that
console. The owner can use `!admin bot status` or `!admin source status`; its
full key and last accepted administration timestamp persist. Ordinary bot
users and BLE pairing do not grant repeater or source administration.

Nearby BLE uses the same bot identity and the existing secured companion
protocol. Provision its PIN locally using the [BLE guide](STATE-BLE.md).
There is no separate phone application to install. BLE is optional; RF
administration and repeater forwarding do not require it. Changing `bot name`
or the established `set bot.name` alias saves the Lua and companion label.
Names are 1–31 printable ASCII bytes without leading/trailing spaces.

## Shared API and capacities

Pine runs the production Lua VM and shared command engine described in
[BOT_RUNTIME.md](../runtime/BOT_RUNTIME.md). Typed exports and help, modules,
events, cooperative continuations, caller/conversation/channel/bot scopes,
grants, KV, transactions, notes, timers, personal reminders, JSON, utilities,
RF send/wait/trace/multitrace, forwarding and native inspection/administration
remain available. Grants and authenticated caller boundaries are unchanged.
Operator-installed source can also
[replace or wrap a builtin](../runtime/BOT_DEVELOPMENT.md#replace-or-wrap-a-builtin-without-rebuilding-firmware)
using `override_command` and `call_original`. It retains the builtin's schema,
policy and native caller context. Source replacement, retry, rollback and
removal use the QSPI metadata journal without changing Pine's identities
or scoped data. Overrides share the same constrained Lua session; they do not
enable HTTPS or create extra job/heap capacity.

| Resource | Pine limit |
| --- | --- |
| Editable package | 4,096 bytes, including declared modules |
| Custom exports / modules | 8 / 8; native command metadata remains complete |
| Concurrent Lua jobs | 2 total; at most 1 custom/event job, keeping a slot for native commands |
| Lua bot contact/route cache | 16 contacts; volatile |
| BLE companion contact cache | 8 contacts |
| Bot radio packet capacity | 4; incoming and outgoing packets share the pool |
| Lua allocator quota | 48 KiB; allocations also preserve 8 KiB physical free heap |
| VM wall time | Installed-source load 330 ms; default firmware-owned bundled recovery load 1 s; initialization 50 ms; active invocation/cleanup 20 ms; suspended I/O time excluded |
| Lua C-call budget | 32; deeper syntax/calls report `C stack overflow`; accepted nesting depends on the grammar |
| KV value / key | 128 / 32 bytes |
| KV capacity | 16 caller/conversation/channel slots plus 8 bot-reserved slots; 8 keys per scope |
| Atomic transaction | 2 mutations |
| Source/readback chunks | 48 bytes; sequential numbered chunks |
| RF text | Shared MeshCore packet text limit; not reduced to the KV value limit |
| Ordinary command cooldown | 0; busy jobs, storage, packets and airtime can still refuse admission |

These are allocation/admission limits, not a guarantee that every 4 KiB program
fits. Lua errors identify a parser, instruction, wall-time or memory limit.
Allocator pressure can refuse a package before the full logical quota is used.
Source initialization cannot perform storage or RF effects. Malformed source,
runaway initialization and allocation failures reject activation.
`source api` reports the compiled job capacity as `jobs=2`;
`source api atomic` reports the mutation limit as `transaction=2`.

Pine has **no WiFi/IP/HTTPS transport**. Network APIs return an explicit unsupported error before transport admission.
`rpc.text` formatting remains available. Wasm is not part of this image.

## Adaptive bot admission

Use `bot adaptive` in the authenticated repeater console to read the saved
selection and live load, allowance, caller, reservation and denial counters.
Adaptive admission defaults off. Send `bot adaptive on`, then `reboot` to
enable it; `bot adaptive off` also applies after reboot. Saving a selection
does not reset live reservations, and source replacement does not change it.

The production Lua engine uses the shared controller before admitting bot work
and before queuing outgoing packets. It samples one physical receive signal,
confirmed RF duration across all roles and the live repeater/companion queues plus
waiting Lua packets. A transmitting Lua packet is not also counted as waiting.
Congestion reduces bot allowance and caller shares; ordinary command cooldown
remains zero and the configured transmit-airtime limit remains in force.
Denied work is not automatically retried. `bot admission` reports the last
affected gate and the current `pause` reason separately: `none`,
`initialization`, `source-unloaded`, `source-publication` or `source-result`.
It reports `cooldown-ms=0` even while source admission is paused. For
`source-unloaded`, inspect `source status` for validation, activation or reload
failure. Native authenticated repeater administration and source recovery
remain available without a running Lua VM.

Production Lua stores this selection in shared bot metadata. The companion
carrier does not keep a second admission controller or use the native-only
`/pine-adapt` setting.
An unreadable setting keeps static admission running and reports
`policy-fault=1`. From the authenticated console, send `bot adaptive off`,
inspect the saved selection, then reboot. A successful save/readback clears
the warning; a failed repair retains it. This changes only the admission
setting, not identities, source or notes.

## Installing, reading and recovering source over RF

Use the existing [package authoring workflow](../runtime/BOT_DEVELOPMENT.md) and
[source protocol](../esp32/MAST_ADMIN.md#source-and-help-installation).
There is no Pine-specific language or package format.

1. Send `source api package` and `source status`.
2. Send `source begin ID16 SIZE SHA256`.
3. Send `source chunk ID16 INDEX HEX` for each sequential chunk of at most
   48 source bytes. Wait for its durable `ACK ID16 next=N`.
4. Send `source commit ID16`; poll `source status` until the live outcome is
   terminal, then check `source hash`.
5. Read `source read INDEX` chunks and verify the complete source hash.
   `source metadata` and `source helptext` describe the selected package.

A repeated matching `begin` resumes an interrupted upload after restart.
Retry only the identical numbered chunk when its reply is lost. A packet ACK
does not mean a source chunk or journal was committed.

Pine serializes validation and activation through **one VM worker**. Validation
temporarily pauses Lua command admission and cancels current Lua jobs. The
repeater, native encrypted administrator console and BLE carrier remain
independent. Rejected source reloads the retained previous code before Lua
admission resumes. A successfully validated candidate stays paused until
publication or explicit recovery finishes.
Already-admitted radio packets keep their adaptive reservations across the
pause, Lua cancellation, rejected candidates, publication and rollback.
Their physical terminal result settles the reservation; losing an admitted
packet's result retains its estimated charge rather than refunding it.
Recovery restores code, not its old Lua heap: globals and module caches restart,
and cancelled jobs do not resume. The selected source hash, durable
`source status` generation and `bot status` source activation generation remain
unchanged on rejection. Its `runtime_epoch` advances to reject prior I/O
completions. Recovery does not replay the already-dispatched startup event;
committed source activation or a full restart can dispatch it again.
If reloading the prior code fails, Lua admission remains blocked and status
names the reload failure. Use `source retry` from the native repeater console
or reboot; the native repeater administrator connection does not require Lua.
Use the repeater's native console during that pause: `!admin` is itself a Lua
job and is paused/cancelled with the other Lua jobs.

`source rollback` restores the previous compatible package/help; KV and grants
are not rolled back. `source retry` reloads the verified durable selection.
`source remove` selects bundled handlers and can recover a corrupt source
journal. Inspect terminal status rather than treating a durable selection as
successful live activation. After uncertain publication or TX, read status
before retrying; effects that already committed are not undone.

## Notes, deadlines and restart

The Lua filesystem uses QSPI `0x000000..0x180000` (1.5 MiB). The original
native note journal remains at `0x180000..0x200000` (512 KiB). A checked one-time
migration copies original notes into caller KV using both full identity keys.
The original journal stays available for application rollback or confirmed
Lua recovery. Its RAM caches are released before the Lua engine starts.

The filesystem ownership marker is in InternalFS. Once present, a failed mount
does **not** format the partition. Metadata replacement uses staged write,
readback and atomic rename; filesystem read/I/O failures block storage instead
of masquerading as absent metadata. KV publication retains its shared checked
file banks, authority journal and uncertain-outcome handling.

After restart, source selection, KV/notes, timers/reminders, grants, mesh policy,
owner and identities persist. Lua globals, jobs, contacts, routes and live
counter state restart. Rollback changes code, not already committed data.

Pine cannot synchronize time through NTP. An authenticated administrator must
send `bot time UNIX_SECONDS` after each restart and refresh it at least hourly.
That command also synchronizes the repeater and Lua bot message clocks.
`bot time` reports the current trusted UTC bounds. `bot reminders on` grants
private reminder creation/delivery. Pending deadlines remain durable while
time is untrusted; completed, cancelled, overdue, claimed or uncertain
deliveries are not rearmed. An uncertain RF transmission is not a confirmed
delivery.

### Confirmed destructive recovery

`bot lua provision` displays the warning. Only when ordinary source recovery
cannot repair the Lua filesystem, send:

```text
bot lua provision erase-lua confirm
```

**This erases Lua source, KV, timers/reminders, grants and the Lua owner record.**
It stops Lua workers, unmounts/reinitializes only their QSPI partition and
restores the original journal's notes. It preserves both identities, native
settings/ACL/BLE and the original note journal. Reboot to restart Lua, then
restore grants/owner/time and install the desired package. Notes added only to
Lua KV after migration are lost. This is not a normal source update.

If the original note region is still occupied by the retired external
filesystem, first use the explicit confirmation in
[original note provisioning](STATE-BLE.md#explicit-lab-provisioning-of-an-occupied-external-volume).
That operation can discard old external files; it is not automatic.

## Validation and resource checks

```sh
make -C firmware/nrf52840 production-lua-test \
  CRYPTO=/path/to/prepared/.pio/libdeps/nrfmast_fleet_lua/Crypto
make -C firmware/nrf52840 test
make -C firmware/nrf52840 size ENV=nrfmast_fleet_lua
make -C firmware/nrf52840 parser-stack-check
make -C firmware/nrf52840 production-lua-source-parser-test \
  SOURCE=/path/to/pinned/MeshCore LUA_ARCHIVE=/path/to/lua-5.5.1.tar.gz
```

The production target runs ABI-realistic native32 shared API tests, actual
LittleFS/NOR power-cut and failed-read tests, borrowed-manifest lifetimes across
suspended custom/native jobs, cancellation, replacement, rollback and failed
initialization, serialized worker replacement and KV/deadline restart tests,
plus the production three-port radio adapter under
ASan/UBSan. It needs a multilib C/C++ toolchain with eight-byte double alignment.
The shared native command-engine integration tests cover authenticated source
upload/readback/restart/rollback and sealed-journal recovery. The admission
test holds a simulated radio transmission across source pause, cancellation,
rejection, publication and rollback, then settles its reservation on completion.
The source-parser target sends 39-level named and global-function source through
the shared native `source begin/chunk/commit` implementation. Rejection keeps
the previous source hash/generation, restores Lua admission and leaves native
administration available; a subsequent update succeeds after cancelling the
rejected upload. The native32 retained-session/worker cases also check clean
rejection and native `!ping` recovery.

The shared Lua radio adapter exposes cumulative estimated receive airtime
through `receivedAirtimeMs()`. Each physical RF frame is counted before packet
validation or port-buffer drops; reading the three role copies does not add
airtime. Local transmissions do not count as reception. Public source and
aggregate RF counters include only transmissions confirmed complete by the
physical driver. A timed-out or aborted driver's active interval is tracked
separately and does not increase those RF counters.
The owning port and timeout watchdog retain the first physical completion signal
until the send is finished. The radio checks use a true-once driver signal and
cover all three ports, timeout retirement, the next send and clock rollover.
Radio measurement generation advances on successful binding or rebinding, so
admission rebases a fresh modem's RF/RX counters without discarding reservations.
PHY configuration has a separate generation; Lua source/runtime epoch changes
do not reset these radio counters or advance measurement generation.
Expiry or a rejected physical start reports a known zero RF duration, so its
adaptive reservation is refunded. A started transmission with an uncertain
outcome reports unknown RF duration and retains its estimated adaptive charge;
it is never reported as successful. Run just these radio checks with
`make -C firmware/nrf52840 production-lua-radio-test CRYPTO=/path/to/prepared/Crypto`.
Packet airtime estimates use the physical driver's current PHY.

Run those selected tests against a compact native worker from the repository
root (the build prints the runner path):

```sh
make -C firmware/esp32 bot-adaptive-test bot-adaptive-native-test bot-adaptive-admin-test bot-source-api-test ONCHIP_BOT_WASM=0 \
  TEST_FLAGS='-DONCHIP_BOT_COMPACT_PROFILE=1 -DONCHIP_BOT_JOB_LIMIT=2'
make -C firmware/esp32 bot-native-worker ONCHIP_BOT_WASM=0 \
  TEST_FLAGS='-DONCHIP_BOT_COMPACT_PROFILE=1 -DONCHIP_BOT_JOB_LIMIT=2'
BOT_NATIVE_WORKER=/path/to/that/bot-native-worker python3 -m unittest \
  internal.nativebot.worker_test.WorkerProcessTest.test_source_durable_upload_restart_rollback_remove \
  internal.nativebot.worker_test.WorkerProcessTest.test_rejected_candidate_preserves_deployment_and_reports_runtime_epoch \
  internal.nativebot.worker_test.WorkerProcessTest.test_interrupted_source_upload_resumes_on_restart \
  internal.nativebot.worker_test.WorkerProcessTest.test_sealed_source_emits_no_advert_until_recovered
```

Bundled initialization requests 35,354 bytes at peak; a model including
eight-byte allocator alignment/headers peaks at 41,584 bytes. The compact
session keeps custom metadata in eight entries and native metadata in flash;
every initialization checks that metadata against the Lua declarations.
Regenerate it after changing native declarations using
`PINE_DUMP_REGISTRY=1 firmware/nrf52840/.build/native/lua-vm`.
Declarations are checked individually against the sorted flash catalogue, with
a bounded registration mask; no whole-manifest temporary is allocated.
The compact session reserves two separate job-sized buffers after source
validation and parser garbage collection, so idle buffers do not consume the
parser's physical heap headroom. Read-only API and event proxies share one
metatable, but each retains its own backing values; writes to existing and new
keys remain denied. Neither optimization reduces the two-job capacity,
48 KiB Lua quota or 8 KiB physical reserve. A failed job-buffer allocation
rejects the candidate explicitly and releases partially allocated buffers.
The ARM initialization frame is 88 bytes. The production build runs
`parser-stack-check` against the actual compiler `.su` files. Its conservative
bound is 16,080 bytes of the 16 KiB VM stack, including a 2 KiB allowance for
lexer/code-generator/error/allocator leaf calls and RTOS/FPU context. It covers
named/global functions, blocks, expressions, assignments and unguarded parent
upvalue lookup. The check reads the nRF52 task's reservation in 32-bit stack
words, independently of the ESP32 task's byte-sized reservation.
Pine uses a 32 C-call budget within its 16 KiB VM task reservation; host/ESP
builds use 40. The compiler stack check calculates a bound.
Use `mem` and `bot memory` to inspect the running task's remaining stack.

The existing
[`firmware/runtime/plugins/examples/lab.lua`](../runtime/plugins/examples/lab.lua)
package has six exports for scoped KV,
durable timers and channel waiting. Retained initialization peaks at 35,682
logical bytes. A concurrent custom timer job and native note read peak at
38,424 bytes, leaving 10,728 bytes below the 48 KiB quota; the allocator model
peaks at 46,224 bytes. These are native32 measurements, not physical free heap.
On the XIAO, the retained package starts with 12,352 bytes of free heap.
A suspended custom timer and a native calculation both receive companion ACKs
and return their replies over RF; the sampled minimum free heap is 8,864 bytes.
The 8 KiB physical reserve remains enforced. Different source tables and
installed settings can change the available headroom; read `mem` on your node.

Data-heavy Lua tables can require more RAM than their source size suggests.
The 4 KiB, eight-export test catalogue with 32 table rows requires a
54,137-byte load peak (62,728 bytes in the allocator model), so Pine explicitly
rejects it at the configured 48 KiB quota. The test uses a larger simulation
quota only to measure that constraint; production does not raise its cap.
Keep runtime tables small, use persistent KV for data that need not stay in
Lua, or run larger in-memory datasets on an ESP32/host role.

Tasks reserve VM 16 KiB, storage 6 KiB, dispatch 10 KiB and callbacks 3 KiB.
Runtime objects, prewarmed storage and task stacks consume the linker heap
arena. Read `mem`, `bot memory`, `bot stats`, `bot diagnostics` and
`bot admission` for actual running state and allocation refusals.

Uploads that exceed the quota or physical reserve leave the active source
selected. A restart retains the selected source; refresh trusted UTC before
using deadlines.

### Stateful custom handlers over RF

Install the shared [lab package](../runtime/plugins/examples/lab.lua) to use
private caller-scoped data and durable timers:

```text
!save plan lunch
!read plan
!alarm tea 12
!timer-state tea
```

`!save` reports `created` or `updated`; `!read` returns the saved text.
`!alarm` waits cooperatively and replies `Timer tea claimed` once due.
Native `!ping` and the repeater's authenticated `get repeat` remain available
while it waits. A restart retains the source, caller KV and claimed timer;
it does not restart the completed timer. Refresh `bot time` after restarting
before creating new deadlines.

To check reception, read the replies on the companion over RF while the
repeater remains enabled. After restart, verify the retained value and
`claimed` timer without creating or rearming it.

## Pine deployment and application rollback

Use a checked application image and a maintenance window. Identify your
radio's stable `/dev/serial/by-id/` application and bootloader paths; they can
differ after DFU entry. Do not substitute another device if the selected
radio is absent. The public Pine bundle updates initialized nodes only.

1. Keep the existing repeater/bot key preservation files private (`0600`,
   ignored/uncommitted). Read `ids`, `mem`, `bot state` and native preferences
   before installation; record both full public keys for comparison afterward.
2. Retain a known compatible previous application for rollback. When migrating
   from native notes, retain that `nrfmast_fleet` application. Install only the checked
   application UF2/DFU ZIP, preserving InternalFS and the original note region.
   First Lua boot initializes only its retired 1.5 MiB QSPI partition if its
   ownership marker is absent.
3. Check both keys, saved BLE settings, names, repeat, your legal shared PHY
   and path width. Synchronize `bot time`,
   then use a companion radio for native RF administration,
   ordinary Lua replies and repeater traversal; nearby BLE is an optional check.
4. Check `mem`/`bot stats` while running one custom and one native cooperative job, persistence,
   malformed/runaway/OOM candidate rejection, readback, update/rollback,
   uncertain TX, restart and deadline no-rearm. During each Lua failure and a
   quarantined source journal, use the authenticated repeater console for
   `bot status`, `source status` and native `get repeat`; confirm its responses
   arrive over RF even when Lua admission is blocked. Record actual task high-water
   marks when sizing custom packages. Include a 39-level
   nested named/global-function upload, its parser-limit rejection and a
   subsequent native RF status request while recording VM stack high-water.
5. If startup or resource checks fail, install the retained compatible
   application through the **same bootloader** without erasing storage.
   A native-note rollback uses the original journal's notes, not subsequent
   Lua-only KV updates.
   Both identities, native preferences and BLE configuration remain in
   InternalFS. Keep Lua files for diagnosis; do not automatically format them.
