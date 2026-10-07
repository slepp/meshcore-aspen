# Native CommandBot process protocol (v1)

`make -C firmware/esp32 bot-native-worker` builds the long-lived production
`bot-native-worker` from the pinned firmware CommandBot/worker, parser,
permissions and MastSource, with the native NVS/SPIFFS adapters. Run with:
an **already existing** absolute private state directory owned by the process
user, mode `0700`, with no symlink components. It must contain pre-existing
`nvs/` and `spiffs/` child directories, each mode `0700` and owned by the
process user. Each storage adapter locks its own directory exclusively. No
hardware, WifiKissMultiplexer, test peer or RF scheduler runs in this process.
Go alone owns the radio and sends its authoritative identity and PHY data.
All native `Serial` diagnostics go to stderr; stdout carries binary frames
only. EOF ends the worker; a malformed frame, I/O fault or storage fault is
fatal. HELLO supplies the identity for this worker lifetime: its volatile
`mc-onchip/command-bot` readback is never a second persistent key authority.
The Go active/pending identity envelope owns deliberate changes.

An optional `scopes` file in the private root configures host-only transport
regions before startup: exactly `"SCP1" | homeKey[16] | defaultKey[16]`
(36 bytes). Zero keys mean absent. It must be a regular mode-0600 file owned
by the worker user; symlinks, malformed data and read errors fail startup.
Missing file preserves the compiled default. The home key is recognized for
received requests but never implicitly becomes the outgoing default. Known
request scope wins; unscoped flood replies stay unscoped; other flood replies
and ordinary adverts use the explicit default. Known direct return paths stay
direct. Changes require worker restart. This bounded two-key setting does not
change IPC v1 or introduce region-tree management.

An optional private `host-name` file contains `"HNM1" | name[1..31]`.
Before the source starts, the worker validates and applies this name through
the existing mesh-policy store, preserving channel-wait and destination policy.
Other NVS records and SPIFFS packages/data are untouched by this override.
The file must be an owned mode-0600 regular file with one link. A bad file or
uncertain name commit stops startup. A host importing native state can use this
override without running a setup worker or resetting radio/source/grant policy.

The optional argument `--dormant` prepares a candidate for identity
apply. It restores and validates the selected source but keeps RF command
admission, reminders and startup adverts disabled. Go drops RF receptions
and rejects any TX request until activation; dormant READY alone does not
make the host role ready.

Willow supplies `--willow-contacts` to recover public signed contacts from its
existing Base authorities. For state `VERSION/willow/native`, the worker reads
`VERSION/base` and `VERSION/secondary` using an owned mode0700 directory and
owned mode0600 committed document. An existing `identity-state.json` takes
precedence over `companion.json`; an invalid active envelope never falls back
to a stale companion document. No identity, route, source or data is imported
or written. Full keys and signed adverts feed the existing 16-slot Core
recovery callback before encrypted-message authentication. More than16
distinct valid keys with one peer hash reject the lookup. `bot contacts`
reports cache occupancy and recovery counts.

Without this argument, the worker retains advert-based discovery. The two
optional arguments may appear in either order; duplicates and unknown
arguments fail startup. These arguments do not change IPC v1 frames.

Every frame is `length:u32le | tag:u8 | body`, where `length` includes the tag
but not the 4-byte length field, and **1 <= length <= 4096**. Integers are
little-endian; floats are IEEE-754 binary32 bit patterns, little-endian.
There are no KISS escaping bytes on this pipe: packet payloads are raw decoded
MeshCore RF packet bytes, not whole encoded KISS serial frames. Unknown
tags/versions, trailing bytes, invalid fields and incomplete frames (5-second
timeout) terminate the process. Never treat a frame write as RF completion.

Go -> native:

| Tag | Body (exact order; no padding) |
| --- | --- |
| `0x01` HELLO | `version:u16=1`, canonical expanded scalar-prefix private identity `[64]`, `freqHz:u32`, `bwHz:u32`, `sf:u8`, `cr:u8`, `txPower:u8`, `noiseFloor:i16` (0=unavailable, otherwise a verified modem reading), `airtimeMs[256]:u32[256]`; **1104-byte payload including tag**. Frequency must be 150–960 MHz, bandwidth 7800–500000 Hz, SF5–12, CR5–8, reported TX power 0–30 dBm and noise floor -150–0 dBm. This profile describes the shared modem; HELLO does not change hardware TX power. The Go operator configuration retains its 22 dBm requested-power limit. SF5/6 receive scores are zero; SF7–12 keep their existing thresholds. `airtimeMs[n]` is the Go radio's authoritative estimated transmit duration in milliseconds for a raw packet of length `n`. Index 0 is unused; indexes 1..255 must be positive, monotone and <= 3,600,000. HELLO must be the first frame and sent exactly once. The helper binds the identity in volatile NVS and publishes its derived public key in READY for Go to verify before serving commands. |
| `0x02` RX | `rssi:f32`, `snr:f32`, `signal:u8 (0=measured RF, 1=local reflection, 2=unmeasured remote)`, `packet:[1..255]`. Measured RF requires finite RSSI in [-150,0] and SNR in [-32,32]. Local and unmeasured receptions carry no invented measurement; neither is counted as a measured RF reception. Reception queue holds 8 packets; overflowing it drops a packet, never causes fake TX success. |
| `0x03` TX_RESULT | `token:u32`, `state:u8`, `reason:u8`, `queueMs:u32`, `rfMs:u32`, `estimateMs:u32`, `hasRfMs:u8 (0/1)`; exactly **20 bytes including tag**. State and reason values are those of `firmware/shared/QueuedTxProtocol.h` (`REJECTED=0`, `ACCEPTED=1`, `SUCCEEDED=2`, `FAILED=3`, `UNKNOWN=4`; `NONE=0`, `INVALID=1`, `FULL=2`, `STALE=3`, `NOT_OWNER=4`, `BUSY=5`, `EXPIRED=6`, `START_FAILED=7`, `RF_TIMEOUT=8`, `DISCONNECTED=9`, `NOT_CONFIGURED=10`). Go sends provisional local ACCEPTED after submitting to the mast, followed by **exactly one terminal** result for each token; the mast can still return REJECTED as that terminal result. It may send REJECTED alone if submission failed locally. ACCEPTED is admission only, **never** final RF success. `hasRfMs` distinguishes known from unknown RF completion; `SUCCEEDED` requires `hasRfMs=1` and `reason=NONE`. Never claim successful RF merely because a request was enqueued. |
| `0x05` PHY_STATS | `sourceGeneration:u32`, `configGeneration:u32`, `aggregateCreditMs,sourceCreditMs,aggregateRfMs,sourceRfMs,aggregateSuccesses,aggregateFailures,sourceSuccesses,sourceFailures:u32[8]`, `aggregateQueued:u8`, `transmitting:u8 (0/1)`; **43 bytes including tag**. Go queries the actual mast queued-PHY snapshot and sends it on startup and periodically while available. Native timestamps the received snapshot and marks it unavailable after 3 seconds; it never fabricates source/aggregate credit or RF counters for `!air`. Configuration drift fails closed and requires a fresh worker/profile. |
| `0x04` ADMIN | `requestID:u32`, `command:[1..160]` printable ASCII bytes (no NUL, control bytes or trailing padding); **6..165 bytes including tag**. Only locally authenticated Go administration may use this private pipe; no independent worker authentication is implied. The request ID is opaque and echoed unchanged. `help` lists native command families; see owner grants and source/radio commands below. Invalid lengths/bytes terminate the process without an ADMIN_REPLY. |
| `0x06` ACTIVATE | No body; exactly one byte including tag. Accepted once, only for a dormant worker after source READY. Go sends it after the identity commit. Enables RF commands and attempts one owner zero-hop advert. |
| `0x07` OWNER_SEND | Optional owner feature: `requestID:[16]`, `recipient:[32]`, `text:[1..162]` printable ASCII. Uses the active native identity, fresh direct contact, admission and airtime policy, then ordinary TX_REQUEST/TX_RESULT tracking. It is not an ADMIN command or a raw packet injection facility. |
| `0x08` OWNER_ENABLE | `enabled:u8 (0/1)`. Enables/disables owner callbacks. **Disabled by default**, so existing Go consumers receive no unsolicited owner frames. The owning host authenticates its local caller; this private pipe is not independently exposed. |

Native -> Go:

| Tag | Body (exact order; no padding) |
| --- | --- |
| `0x81` TX_REQUEST | `token:u32`, `priority:u8`, `delayMs:u32`, `expiryMs:u32`, `packet:[1..255]`; `delayMs` and `expiryMs` are the native queued-radio relative scheduling values, not absolute wall timestamps. Queue depth is at most 13. Tokens increase monotonically during the process lifetime. Go must route requests through its real shared-radio scheduler and return correlated TX_RESULT events; the helper has **no alternate scheduler**. A startup advert TX_REQUEST may precede READY. |
| `0x82` READY | `publicKey:[32]`, `ready:u8=1`; emitted once only after both selected durable sources have been validated, copied, staged and activated on the real BotWorker (or their journals select the bundled Lua / empty Wasm source). Verify key against the Go identity. This is aggregate deployment readiness; native dispatch separately admits commands owned by each available runtime. |
| `0x83` STATUS | `ready:u8`, `fault:u8`, then `observationsDropped,malformed,duplicates,rejected,vmFailures,replies,traces:u32[7]`; exactly **31 bytes including tag**, periodic at most once per second after HELLO. Ready reflects current durable/live source agreement, not merely the temporary bundled VM; it can fall to 0 during a source change or recovery. A fault flag denotes bot source/VM issues without exposing private error text; stderr has diagnostics. |
| `0x84` ERROR | `code:u8`, emitted best-effort before a fatal exit: `1=invalid HELLO`, `2=storage initialization`, `3=identity binding`, `4=bot startup`, `5=invalid/truncated input`, `6=I/O or storage runtime fault`, `7=source initialization timeout`. Always treat the subsequent exit as authoritative, even if this frame cannot be delivered. |
| `0x85` ADMIN_REPLY | `requestID:u32`, `reply:[0..256]` ASCII bytes (not NUL-terminated); **5..261 bytes including tag**. Exactly one reply per well-formed ADMIN request; command rejection is an `Error:` reply, not an RF packet. Replies may interleave with TX_REQUEST and STATUS. Correlate by request ID; never log secret-bearing ADMIN requests. |
| `0x86` MANAGEMENT_READY | `publicKey:[32]`, `ready:u8=1`. Emitted once after identity bind, CommandBot startup and durable source journal inspection, even if a journal needs operator recovery. Go verifies the key and starts the private same-UID admin socket. This is **not** aggregate application READY. A failed or sealed runtime's custom commands are closed; the healthy runtime and native diagnostics remain available. Use `source status` / `source wasm status`, then the affected selector's `retry` or explicit `remove`, through authenticated administration. |
| `0x87` ACTIVATED | `advertQueued:u8 (0/1)`; exactly two bytes including tag. Confirms activation and whether the zero-hop advert entered the native queue, never terminal RF success. |
| `0x88` OWNER_SEND_STATUS | `requestID:[16]`, `phase:u8`, `flags:u8`, `reason:u8`; exactly 20 bytes including tag. Phases: 1 admitted, 2 queued, 3 local TX confirmed, 4 matched native RF ACK, 5 failed, 6 unknown. Flags independently retain bit 0 local TX confirmed, bit 1 native RF ACK, bit 2 admitted. Reasons: 0 none, 1 invalid text, 2 fresh direct contact required, 3 native capacity/radio unavailable, 4 clock unavailable, 5 airtime/admission denied, 6 TX failed/uncertain, 7 ACK deadline expired. Four native slots; 45-second ACK deadline. No automatic retransmission. |
| `0x89` OWNER_INBOX | `sender:[32]`, `timestamp:u32`, `text:[1..162]`; full sender key selected by authenticated native decryption, excluding self/reflection. Canonical timestamp/text/full-key duplicates (including changed retry bits/padding) are suppressed for 120 seconds in 64 entries. A frame containing only the tag reports a dedup-capacity drop. Forward plaintext only to the authenticated owner's private inbox, never the journal or raw-packet observer. |

Willow exposes the optional owner feature through its
[same-UID Unix control socket](../../experiments/hew-roles/SERVICE.md#send-a-dm-and-read-the-owners-inbox),
not by sharing the worker pipe. Its durable request IDs prevent automatic
replay across worker, host or Go handover. The worker remains the same
Go-compatible native bot when this feature is not enabled.

The process samples monotonic time for scheduling and system UTC for bot
timestamps. If the initial source is not ready after 30 seconds the helper
exits with failure. Lua/MeshCore random bytes use kernel
`getrandom` with fail-closed behavior; the bot key always comes from Go. No simulated/random-seeded radio input
is accepted. Nonblocking stdin keeps the dispatch loop running between partial
frames; stalled stdout fails closed instead of accumulating unbounded frames.
On worker exit Go must treat unresolved admitted RF tokens as **unknown**.
If Go changes its radio PHY, it must stop this worker and restart it with a
fresh HELLO/airtime table before forwarding more packets or requests; v1 has
no live profile-update message.

`discovery [status|on|off]` reads or immediately persists/applies native type-3
base telemetry and path-discovery replies. Missing settings default to **off**;
`on` explicitly permits all signed-advert contacts whose native encrypted
request authenticates. It does not grant management access or alter any
other role's ACL. `off` persists the denial. Commit/readback failure disables
live replies and reports an uncertain saved outcome. The host has no board
sensors and replies with the request tag plus zero padding, never invented
voltage, MCU temperature, location or environment readings. It uses the
shared firmware's native PATH/RESPONSE routing, one-response-per-second limit
and bot airtime budget. A recovering/unready source does not answer.

`adaptive [on|off]` reads or saves the native bot's opt-in adaptive airtime
admission selection. It defaults to off and applies on worker restart.
The existing RX and PHY_STATS frames supply packet-length RX estimates and
aggregate physical TX/queue measurements; no new wire frame is required.
Reflections never add RX airtime, and other host role copies are not added.
Adaptive reservations settle from TX_RESULT terminal outcomes, not ACCEPTED
or queue delay. Missing/stale PHY_STATS refuses adaptive bot work; native
startup/owner adverts and ACK/discovery behavior keep their existing rules.
See [adaptive admission](../../firmware/runtime/BOT_RUNTIME.md#opt-in-adaptive-airtime-admission).

After installing the rebuilt worker and restarting the host service, use the
existing private owner socket (replace the path with the selected bot's state
directory):

```sh
python3 tools/hardware/admin.py --unix-socket /absolute/bot-state/admin.sock command 'discovery status'
python3 tools/hardware/admin.py --unix-socket /absolute/bot-state/admin.sock command 'discovery on'
```

Expect `saved=1 live=1`; use `discovery off` to revoke. This does not make a
companion/Base with denied telemetry answer, nor let a bot answer a repeater
status request. See [native app behavior and wire details](../../firmware/runtime/BOT_RUNTIME.md#native-app-ping-and-path-discovery).

The ADMIN bridge invokes the **existing** `MastSource::execute` grammar after
`source `: `status`, `hash`, `api`, `api package`, `metadata`, `begin ID16 SIZE
SHA256`, `chunk ID16 INDEX HEX`, `commit ID16`, `rollback`, `remove`, `retry`,
`cancel`, and the other native source read/help/API commands. `ID16` is 16
lowercase hex digits; SHA256 is 64 lowercase hex digits; chunks contain
at most 48 bytes (96 lowercase hex digits). Begin is idempotent for the same
upload, chunks acknowledge durable progress (`ACK ID16 next=INDEX`), and
commit replies *Accepted verification* before asynchronous validation,
durable journal commit and live activation. Poll `source status` and STATUS;
do not treat that acceptance as activation. Upload progress and deployment
survive a worker restart, with the same NVS `mc-mast-admin/source` journal and
SPIFFS slot files as firmware. Source package capabilities reflect the native
worker build: HTTPS is **off** without a production HTTPS worker.

## Native owner grants

Use `help` for supported native commands and `help grants` for grant syntax.
`status` reads source readiness, running job count and event counters. `policy`
or `source api grants` reads all three grants' saved and applied values.
An unsupported command returns `Error:` with the next help command; malformed
ADMIN frames still terminate the worker as described above.

| Command | Readback or effect |
| --- | --- |
| `clock` | HTTPS/scheduler clock usability, `uncertainty-us`, `scheduler-limit-us` and denial `reason`. |
| `shared [status]` | Saved/applied access to bot/channel KV and named timer scopes. Caller/conversation scopes retain their existing principal checks. |
| `shared on` / `shared off` | Persist and apply the existing CommandBot shared-state policy. Defaults off. |
| `reminders [status]` | Saved/applied personal reminder grant and scheduler UTC trust (`clock=0/1`). |
| `reminders on` / `reminders off` | Persist and apply the reminder grant. Defaults off. Off fences outstanding dispatch and creation and suspends pending reminders; it does not delete records. |
| `events [status]` | Saved/applied event mask and `subscribed`, the current grant intersected with handlers declared by the active source. |
| `events MASK` | Persist and apply a decimal mask 0..31: 1 startup, 2 connectivity, 4 message, 8 node_status, 16 scheduled. 0 withdraws all subscriptions; the default is 0. |
| `cancel` | Cancel running commands/events and delayed collectors. Personal reminder records/grants are unchanged. Already admitted RF/storage effects may have completed or have an unknown outcome. |

Boolean writes accept only exact `on` or `off`; invalid arguments/masks return
`Error:` before changing policy or grant epochs. Successful grants survive worker
restart under the existing native NVS settings. Read saved/applied state after
any failed write: runtime denial may have applied even when persistence is
uncertain, and prior saved policy can return on restart. Grant changes reuse
CommandBot's existing epoch fences; withdrawal denies old yielding shared-state
operations, reminder dispatch and event work, as well as new unauthorized work.
An RF request already admitted by the modem cannot be recalled.

The owner Unix socket configures device policy, not a Lua caller principal.
It does not make local input an authenticated private DM. Personal reminders
still require an authenticated sender's full public key, trusted UTC and a
direct return route. Records remain scoped to the original full bot/caller keys.
The host worker publishes conservative millisecond UTC bounds from the same
Linux kernel clock provider used by native HTTPS. `adjtimex` must report no
`STA_UNSYNC`, `STA_CLOCKERR` or `TIME_ERROR`. Scheduler publication requires its
error bound plus sample measurement uncertainty to be at most **two seconds**.
This permits the normal approximately 1.024-second bound near the end of a
2048-second systemd-timesyncd poll. HTTPS retains synchronized-kernel acceptance
without imposing the scheduler's numeric error budget; `clock` can therefore
report `https=1 scheduler=0 reason=scheduler-error-bound-exceeded`.
Scheduler UTC intervals retain the entire measured error, round outwards and
are limited to five seconds of width including publication measurement;
uncertainty can delay creation deadlines and delivery. Publication advances
with monotonic elapsed time and expires after three seconds without a fresh
sample. Setting a plausible date or changing a role RTC does not establish
scheduler trust.

Loss of synchronization or a wall-clock discontinuity relative to monotonic
time revokes scheduler trust immediately. After a backward step, a forward
step outside the measured uncertainty, or an untrusted sample, the provider
requires one second of coherent kernel-trusted sampling before republishing.
An excessive error bound revokes scheduler publication but does not revoke
kernel synchronization; fresh samples within budget can resume immediately.
Creation and dispatch pause while `clock=0`; listing and cancelling existing
personal reminders still work. After recovery, pending reminders can dispatch
only within the existing 60-second overdue window. A large forward correction
can therefore mark them overdue, but never replays a claimed or sent effect.
Re-enabling reminders may dispatch still-pending records while their deadline
is eligible; sent, claimed/unknown or no-rearm-restored records are not replayed.
`cancel` is not a reminder deletion command. The caller can use the Lua
`reminder.cancel` operation through an authenticated private DM; owner backup
and restore remain under `data help` and require `no-rearm` for scheduler data.

`source api package` lists compiled APIs, never granted permissions.
Host `source api storage` and `source api board` append shared-state
`saved`/`applied`; `source api reminders` appends its grant `saved`/`applied` and scheduler `clock`.
Storage `autonomous-reminders` and reminder `autonomous` are 0 unless the
reminder grant and scheduler UTC trust are both applied; private-DM and route
checks still apply when these fields are 1.
`source api events` reports `active=0` when there are no effective subscriptions,
and includes `saved`, `applied` and `subscribed` masks. A saved event grant can be
nonzero while `subscribed=0` because the source has no matching handlers or
the source is being changed. These grants do not authorize a script to change
its own policy, send private data from event handlers, or control another role
or the shared PHY.

`make -C firmware/esp32 bot-native-clock-test` checks the shared provider and
the actual worker through the Go owner socket and encrypted RF packets.
It exercises reminder creation/list/cancel, caller isolation, restart delivery
and no replay, source/grant withdrawal, the timesyncd uncertainty ramp/poll reset,
excessive uncertainty, untrusted time, forward/backward
corrections and `no-rearm` restore. The production executable reads the real
kernel and ignores clock fixture environment variables. A separately built
worker with `MESHCORE_HOST_CLOCK_TEST=1` accepts a protected local fixture for
deterministic clock transitions; do not install that test executable.

`name NAME` persists the native `BotMeshPolicy` and applies the live advert;
NAME must be 1..31 printable ASCII bytes without `:`. `channel off` or
`channel NAMEHEX KEY32` persists `BotRadioPolicy` (1..31 printable non-NUL
decoded name bytes, exactly 16 nonzero key bytes; hex may be upper/lowercase).
Channel changes apply only after restarting the worker with a fresh HELLO;
replies explicitly say **reboot required**. Neither command rotates identity
or changes the shared radio PHY. Identity requests are handled by the Go owner
socket, never by native ADMIN or RF/Lua commands. The channel
key is never included in a reply or worker diagnostics. No source/admin route
exists on KISS, Companion, radio commands, or stdout text: stdout carries
only framed private Go-channel replies. Go must authenticate/authorize admin
access before sending ADMIN and keep the state directory and pipe private.
`airtime status` reads the saved native bot budget, and `airtime 360..3600`
persists its ms/minute allowance for the next worker startup. The mast's
physical airtime and queue authority remain unchanged. Replacing the native
worker applies zero ordinary command cooldown to existing saved policies;
there is no cooldown command or state reset.
`advert.zerohop` sends the saved bot name and unchanged public identity in a
signed, pathless direct advert only while the durable source is active. The
same-UID Unix peer is the host owner for this narrow command and invokes the
canonical owner-only zero-hop notification without waiting 15 minutes after
startup or rename. Successful owner notifications have a separate one-minute
cooldown per worker process; rejected queue/airtime requests do not consume it.
The public bot/Lua API retains its ordinary 15-minute interval; both paths
keep queued-radio admission and bot airtime budget.
On-chip RF-role authorization is unchanged; no MastAdmin credential or
shared-PHY authority is delegated. The private reply confirms local queueing,
**not** terminal RF success; verify the actual mast RF receipt.

## Coordinated identity apply

The private Go socket accepts `key bot`, `key bot pending`, `key bot cancel`,
`key bot HEX128` (the protected-file CLI sends this), and `key bot apply`.
`role key bot` aliases the public-key inspection/apply grammar. No generated
rotation or private key export command is exposed.

Active and pending keys share `<state_dir>/bot/identity-state.json` (0600).
Until that envelope exists, the existing seed/expanded key remains authoritative.
Startup never activates a pending key. Native identity reads use only HELLO;
an older NVS key mirror is ignored and omitted on the next snapshot commit.
Existing native data with a missing Go key authority blocks startup instead
of generating a replacement identity.

Apply serializes owner commands, retires the old worker and waits for its
radio callback and submission goroutines, without closing the shared source.
Accepted old mast requests may still transmit. Their unresolved results are
unknown, and neither old packets nor coroutine work are replayed. A dormant
candidate restores the same source/grants and scoped storage, then Go verifies
its HELLO/READY key before atomically promoting the pending key and sending
ACTIVATE. Pre-commit failures reload the old key and retain the pending stage.
An indeterminate durable commit leaves the bot stopped; owner readback and
`key bot apply` retry use the saved authority, not a guessed rollback.
Activation failure after a committed key also leaves the bot stopped, with
the saved new key. A retry republishes/syncs that saved authority before
activation. Other roles and the radio PHY continue unchanged.

KV, timers and reminders stay under their original full bot identity.
Source installation and grants remain device policy. See
[operator commands](../../firmware/esp32/MAST_ADMIN.md#go-host-native-bot-identity).
