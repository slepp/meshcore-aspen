# Queued PHY protocol, version 1

Ordinary records use KISS port 0, SETHARDWARE (6); negotiated MKISS sessions
also use ports 1–3 as described below. Integers are unsigned
little-endian unless stated otherwise. IDs 0x20–0x26 and 0xFA are unused by
MeshCore companion-v1.17.1. Ordinary control responses retain their existing
wire formats. A client must negotiate before using extensions.

| Request | Payload | Response |
|---|---|---|
| 0x20 HELLO | version:u8, claim-configuration-owner:u8 (0/1) | 0xA0: version:u8, reason:u8, connection-generation:u32, configuration-generation:u32, job-capacity:u8, owner:u8 |
| 0x21 SUBMIT | version:u8, connection-generation:u32, nonzero-job-id:u32, priority:u8, remaining-not-before-ms:u32, expiry-from-admission-ms:u32, packet:1..255 bytes | 0xFA lifecycle record |
| 0x22 CONFIG | version:u8, operation:u8; GET=0 has no further fields; SET=1 adds expected-configuration-generation:u32, profile:18 bytes | 0xA2: version:u8, reason:u8, configuration-generation:u32, profile:18 bytes |
| 0x23 SOURCE_POLICY | version:u8, connection-generation:u32, airtime-factor:IEEE754-float32 bits:u32 | 0xA3: version:u8, reason:u8, effective-factor:IEEE754-float32 bits:u32 |
| 0x24 STATS | version:u8 | 0xA4: version:u8, configuration-generation:u32, aggregate-credit-ms:u32, aggregate-RF-ms:u32, aggregate-successes:u32, aggregate-failures:u32, source-credit-ms:u32, source-RF-ms:u32, source-successes:u32, source-failures:u32, queued:u8, transmitting:u8 |
| 0x25 CAPACITY | version:u8 | 0xA5: version:u8, external-TCP-slots:u8, local-source-slots:u8 |
| 0x26 ROLE_PRESENCE | version:u8, role-id:u8, full-public-key:32 bytes | 0xA6: version:u8, reason:u8, source-generation:u32, applied-native-role-mask:u8, warning-flags:u8 |

### Shared TCP KISS session (additive MKISS v1)

One host can share one physical TCP KISS connection among four logical sources.
Send the ordinary port-0 HELLO first. Port 0 CAPACITY `[0x25, 1]` keeps its
existing response `[0xA5, 1, external-TCP-slots, local-source-slots]`; it
does not enable a session. To enable virtual ports, send port-0 CAPACITY
`[0x25, 1, 1]` after HELLO. The response is
`[0xA5, 1, external-TCP-slots, local-source-slots, 4, 1]`: four KISS ports
including port 0, and one available aggregate physical session. These counts
exclude the KISS command byte. Invalid or unsupported probes return the
ordinary hardware error, allowing an auto-mode host to reconnect using
independent TCP clients. Only one physical client can negotiate an aggregate
session at a time. It occupies **one** physical TCP slot; other clients keep
their independent KISS connections. The combined image retains four physical
TCP slots. A second aggregate probe receives the same five-byte response
with `sessions=0` while the first session is active. The host can retry on a
fresh connection rather than falling back to direct clients; the busy probe
does not change the first session.

The shared modem enables TCP keepalive **only** after the aggregate CAPACITY
probe succeeds: 45 seconds idle, 10 seconds between probes, three unanswered
probes (roughly 75 seconds to detect an unresponsive TCP peer, subject to
network and lwIP timer delays). Quiet but healthy hosts acknowledge TCP probes
without application traffic. The modem logs and closes the connection if it
cannot set every socket option; it never advertises a session without
keepalive. On a dead transport the modem releases all four logical sources
and their owner lease. The next host must establish its own HELLO and CAPACITY
exchange; `sessions=0` does not permit a competing client to take over a
connection. Already admitted work with an uncertain RF result remains unknown
and must not be replayed. Direct KISS clients retain their existing socket
behavior. TCP keepalive detects unreachable peers, not a responsive but
stalled application; the Go host's normal heartbeat handles application
health.

The negotiated socket carries KISS ports 0 through 3. Port 0 retains its
ordinary HELLO reply, owner lease, CAPACITY and legacy controls. Ports 1–3
each accept SET/GET_SIGNAL_REPORT before their own HELLO, since the Go KISS
modem sends SET_SIGNAL_REPORT on connect. The setting survives the first
HELLO. Other commands require HELLO; each child has an independent connection
generation, CONFIG readback, source policy, advisory role/key
announcement, TX job-ID sequence, RX signal-report setting and airtime
statistics. The host claims the configuration lease only on port 0; a port
that does not own it cannot SET. The physical scheduler, RF credit and
configuration generation remain shared. Received DATA and its RxMeta are
delivered to each negotiated logical port, subject to that port's signal
setting; successful local TX is reflected once to every other port and direct
client, never to the sender. Commands and responses retain their existing
payloads: the KISS command byte's high nibble identifies the logical port.

Each logical port has a bounded output ring, drained fairly into the one
physical TCP output ring without splitting a KISS frame. Child frames leave
one maximum-size frame of ring capacity for port-0 control. If the shared
socket cannot carry a port's output before its ring fills, the aggregate connection
is closed so all its roles reconnect; independent direct clients remain
connected. A new port HELLO retires that port's old pending jobs and gives it
a new generation. Losing the physical connection retires all four logical
sources and releases their leases. Already-started RF may finish, but no
old-generation result goes to a replacement connection. Treat accepted but
unterminated work as **unknown** on reconnect; never resubmit it under a new
job ID. The dashboard `/status` `kiss.session` field reports the physical
slot and each virtual port's generation, policy, credit and buffered output.

ROLE_PRESENCE has exactly 34 request payload bytes and 8 response payload
bytes (excluding KISS command and subcommand). Version is 1. Role IDs 0, 1,
2, 3 mean repeater, room, companion, observer; mask bits use those same
positions. The TCP source generation is the one from HELLO. Valid announcements
always return reason `NONE` (0), **including duplicate roles or keys**.
Malformed requests, unknown IDs, wrong versions, all-zero keys, requests
before HELLO and non-TCP requests return `INVALID` (1) with zero warning flags;
they do not close or fault the connection. Announcements can be repeated or
changed on one connection; the latest replaces its prior advisory presence.
They retire on disconnect, never gate TX/RX or change the configuration-owner
lease. Clients need not announce; ordinary KISS clients remain unchanged.

Warning flags are independent bits: `0x01` = the announced native role is
currently running on the mast; `0x02` = another currently connected TCP client
announced the same role; `0x04` = the full key matches a native identity
(including modem/management or a running native application identity);
`0x08` = another currently connected TCP client's announced full key matches.
Native identity matches include active modem, management and running native
application identities; replacements are reflected after a lifecycle restart.
The applied mask is the boot-loaded profile, not the saved next-boot profile;
an applied role that fails to start is not flagged as an active role.
No bit is a denial or an exclusive lease. Other clients' announcements and
disconnections can change flags, so hosts should reannounce to refresh the
snapshot when useful; warnings are not unsolicited updates. Absent or
unannounced clients cannot be identified. Announcements are unauthenticated
self-reports, not proof of identity or ownership. Signed RF updates may save any
otherwise-valid role profile regardless of announcements or warnings; selected
roles start only after reboot.
The mast also emits a change-triggered serial diagnostic, with the announced
role and full public key, when overlap flags change to a nonzero value. The
current dashboard role
schema has states and faults but no advisory-warning field; no overlap is
misreported there as a role fault or unavailable PHY. The TCP response is
the machine-readable warning surface.

Forwarding is likewise operator-controlled, not reserved by role name. Native
room forwarding may be enabled via persisted `_prefs.disable_fwd == 0`
(default disabled); native companion forwarding may be enabled via persisted
`_prefs.isRepeatEn()` (default false). These can forward alongside a repeater
on either device, even when their role IDs differ. Presence warnings describe
announced role/key overlap, not actual forwarding policy or an RF interlock.

### Beta placement scope and future instances

The four role IDs classify applications; they do not grant exclusive ownership
or imply an enduring hardware limit on instances. This beta starts at most
**one on-chip instance per role** (repeater, room, companion, observer), selected
by the boot-applied profile. Two TCP sources may announce the same role with
distinct keys: the response sets the other-TCP-role bit, not the
other-TCP-key bit, and both remain connected.
The applied mask reports **presence**, not counts or instance IDs. One TCP
connection can report only its latest announcement;
generic KISS connections and unannounced applications are not counted. KISS TCP
clients remain independent logical clients of the one RF PHY regardless of the
selected native role mix and never need presence announcements. The radio-only
image retains its KISS service; a UART stream endpoint, where compiled, is
likewise independent. A native observer is passive on RF; a host observer may
publish received traffic following MeshCore MQTT conventions without claiming
exclusive RF or preventing other observers.

Multiple native rooms or multiple simultaneous announcements per connection
will need a versioned instance-aware discovery format (identity plus instance
ID, count/pagination and explicit source generations), not reinterpretation of
the v1 bit mask. Boot configuration and the signed profile journal will need
an instance list with stable per-instance identities and migration from the
current mask. Resource admission must budget each instance's local radio
source, packet pool/PSRAM, persistent state, sockets and shared PHY airtime;
additional KISS streams require independent source generations and output
queues. These are future placement and capacity changes, not a reason to
reject repeated role IDs in the current advisory protocol.

Profile: frequency-Hz:u32, bandwidth-Hz:u32, SF:u8, CR:u8, power-dBm:u8,
aggregate-airtime-factor:IEEE754-float32 bits:u32, CAD-enabled:u8,
interference-threshold:signed-i16. The profile is 18 bytes; CONFIG SET request
and CONFIG response are both 24 bytes. SOURCE_POLICY request is 9 bytes and
response is 6 bytes. All counts exclude KISS command and hardware-subcommand
bytes.
Both factors are finite nonnegative float32 values, preserving native
repeater/room preferences rather than quantizing them to companion tuning
precision. Global and source credit/refill use native float32 arithmetic.
Runtime factors above 9 are valid: native `dutycycle 1` means factor 99, and
native companion tuning has a uint32 thousandths field. The native
preferences-loader clamp to 9 is not a runtime PHY-policy limit.
Negative/non-finite unchecked `af` inputs are rejected to protect budget
arithmetic. Very large positive factors can prevent TX admission while
control queries remain live.

Lifecycle 0xFA: version:u8, connection-generation:u32, job-id:u32,
state:u8, reason:u8, queue-wait-ms:u32, measured-RF-ms:u32,
estimated-RF-ms:u32 (23 bytes). States: rejected-before-admission=0,
accepted=1, succeeded=2, failed=3, unknown=4. Reasons: none=0, invalid=1,
full=2, stale-generation-or-ID=3, not-owner=4, busy=5, expired=6,
start-failed=7, RF-timeout=8, disconnected=9, not-configured=10.
Job IDs strictly increase within a negotiated connection generation, so an
ambiguous submission must never be retried with a new ID. Rejections are final.
Accepted work can fail before RF starts (expiry); measured RF duration is zero.

Delay/expiry use monotonic uint32 milliseconds, each at most 0x3fffffff.
Expiry zero means no queue expiry. Queue wait is independent of the RF timeout.
Selection is lowest priority amongst eligible jobs, FIFO ties. The physical
modem reselects after budget/CAD waits. Legacy DATA has class 4 and uses the
same scheduler and aggregate budget; its terminal notification remains
`F8 01` (success) or `F8 00` (failure/unknown). It does not negotiate priorities.

CAPACITY is read-only and available to any negotiated connection, including a
non-owner. HELLO remains a 12-byte response; clients that do not recognize
CAPACITY can still use the existing protocol. The combined mast image reports
four external TCP slots and four local sources (three applications and the
always-on signed RF management receiver). A radio-only image
reports its own compiled limits.

Only the connection holding the configuration-owner lease may SET; a
disconnected owner releases its lease without resetting configuration or
aggregate credit. SET uses compare-and-swap generation and is rejected while
RF jobs are queued/in flight. Rejoining clients GET and verify instead of
retuning. A no-change SET is idempotent. Legacy SET_RADIO (exactly 10 bytes)
and SET_TX_POWER (exactly one byte) return standard F0 success only when they
exactly match the committed, non-quarantined physical profile. These are
no-ops, including while jobs wait or transmit: no retuning, flash writes,
owner claims or configuration-generation changes occur. They do not require
HELLO negotiation. Fresh compiled defaults need an owner CONFIG commit before
these legacy no-op setters can succeed. Mismatches, malformed setters, reboot
and KISS timing changes remain rejected. Other
non-mutating native controls remain independent of TX lifecycle.
Extended clients must successfully GET/SET CONFIG before submitting. A later
configuration generation invalidates other clients' readback; their submissions
are rejected as stale until they read and validate the new profile. Legacy
DATA continues to use the current physical profile.

The ESP32 stores the committed operator profile in NVS namespace `mesh-phy`,
key `profile`: one atomic blob containing `MCP`, storage version byte `1`,
then the 18-byte profile. NVS supplies blob integrity checking. The first
owner SET and actual profile changes commit before RF reconfiguration and
CONFIG success. An unchanged, already committed profile does not write flash.
GET and TCP reconnect do not write or retune.
In the combined on-chip image, the saved operator PHY profile is also a
runtime limit: CONFIG SET only accepts an exact reassertion of the committed
profile, even from the configuration owner. Valid changes to the profile
return `NOT_OWNER` without writing NVS, changing radio settings or advancing
the configuration generation. Radio-only images retain owner SET behavior.

Boot restores and validates this record before starting the network service.
Only an absent namespace means a fresh device that may use compiled defaults.
An existing namespace with a missing, unreadable or invalid record fails
closed with a serial diagnostic; the radio service does not start. No automatic
erase or fallback is attempted by the modem. A runtime write/commit failure
returns existing reason `NOT_CONFIGURED` and blocks both extended and legacy
TX, including when the failed commit might have reached flash. CONFIG GET
also reports the fault. An explicit successful owner SET or a reboot that
successfully restores the durable record can recover service.

Source factors remain connection-local. Aggregate credit survives TCP
reconnects but restarts with its native initial allowance after a device reboot;
neither credit nor per-source policy is written to flash.

The physical scheduler uses Mesh's randomized 120/240/360 ms carrier retry,
4000 ms maximum busy duration, maximum-packet half-airtime admission, 100 ms reserve and one-hour
token window. Role delay is applied once; no KISS 500 ms/p-persistence layer
is used. Global and optional source budgets refill at 1/(1+factor).
Successful RF occupancy and conservative failed/uncertain occupancy are
charged independently of network/queue latency. Failure charging is an
intentional safety extension. Source policy changes clamp existing credit;
they do not grant fresh credit. Aggregate credit survives all TCP reconnects.
Statistics counters wrap modulo 2^32; they are not native role counters.
Blocking hardware CAD counts as queue wait, not RF airtime or RF timeout.
The retry interval starts after the busy check completes and must strictly
elapse before the next check, matching Mesh rather than the base Dispatcher's
fixed 200 ms default. Eligible priority
and expiry are re-evaluated after CAD, immediately before transmission.

RX DATA/RxMeta adjacency and one successful sender-excluding reflection are
preserved. Neither acceptance, start failure nor unknown RF outcome causes
reflection. A disconnected sender's already-started RF operation completes
without delivering a terminal result to a replacement connection.

Go: `radio.Config.RequireParity`, `ConfigurationOwner`, and optional
`PHYProfile` choose negotiated mode. `Link.Radio()` preserves `node.Radio`
and implements `node.TxRadio` without a host scheduler/budget.
`SetSourceAirtimeFactor(context.Context,float64) error` is available on Link
and its returned radio. `PHYStatistics(context.Context)` returns distinct
aggregate/source physical statistics. Queued mode requires the negotiated
capability; explicit legacy mode uses KISS DATA/TxDone.
Both setters use native float32 and verify exact effective bits in readback.
Conversion from float64 to native float32 is the only factor rounding.
Factor zero is valid and means 100% refill rate, not a default-value sentinel.

`PHYProfile` fields are `AirtimeFactor float64`, `CADEnabled bool`,
`InterferenceThreshold int16`. `Config.PHYProfile` is `*PHYProfile`; nil means
factor 1, CAD false, interference threshold 0. Normal host configurations default
to a non-nil profile with factor 1 and CAD enabled; an explicit profile or saved
modem configuration retains its chosen CAD setting. A non-nil profile uses its fields
verbatim, including factor zero. `Config.RequireParity` and
`Config.ConfigurationOwner` are bool; only the designated first connection
claims ownership. `Config.TXResultHandler` is `func(TXResult)`. Existing
`Address string`, `Radio hardware.RadioConfig`, `TxPower uint8`, and
`Logger *slog.Logger` remain unchanged.

The durable-host preference profile intentionally retains valid runtime
factors above 9 across logical restarts. This is distinct from native firmware's
boot/load clamp to 9. The PHY never truncates such preferences during source
policy application or TCP reconnection. Role persistence owns that durable
profile; the modem's connection-local source policy is not a disk store.

`Config.TXResultHandler` or `Link.AddTXResultHandler` observes admission and
terminal outcomes separately. `TxRadio.Enqueue` reports bounded local
submission, not RF success; MCU queue rejection arrives as `TXRejected`.
It never waits for lifecycle notifications inside the receive callback.
An ambiguous transport write retains the submitted job and reports unknown
through its lifecycle callback, rather than returning a false queue-full
result that could invite a caller to replay it.
`Link.SendData` waits for the terminal outcome without a queue-residence
timeout. Disconnection resolves every outstanding job as unknown, never
replays it, and still permits normal reconnection. Result callbacks must not
block the receive loop.
`TXResult.PacketBytes() []byte` returns an owned copy of the original submitted
wire packet for every outcome, including rejected, failed and unknown jobs.
The stored packet is private; mutating a returned copy cannot affect another
callback or a later outcome. Correlate role logs through these bytes rather
than guessing FIFO order or job IDs. Acceptance is not TX success.
An existing owner Link also reconnects using readback rather than reapplying
its profile. Host-authority links and modem-authority links with fixed PHY
tracking require the configured profile; a changed profile takes that link
offline until it matches again. Modem-authority links with follow tracking
adopt the mast's valid effective profile on connection, heartbeat or stale
submission rejection, including a temporary switch and its return. No host
link silently overwrites an authorized mast configuration change on reconnect.

The modem restores its committed profile, or uses compiled defaults only on a
fresh device, with usable aggregate credit;
legacy TCP clients do not need a Go host or owner connection to transmit.
The configuration-owner lease controls later profile changes, not data access.

Root configuration loaders can call `(radio.PHYProfile).Validate() error`
before opening a link; `Open` also validates before connecting. Validation
accepts finite nonnegative values through MaxFloat32 without modifying them.
The wire narrows once to native IEEE754 float32, and configuration readback
must match the serialized profile exactly. `PHYProfile` has no JSON tags;
root configuration owns its JSON naming.

Host reconnect remains readback-only after a successful initial configuration.
Failed reconnects back off from 2 seconds to a 30-second cap; in fixed tracking
a profile mismatch selects the cap immediately and leaves the link offline.
Logs include the next retry delay, and `ErrPHYProfileMismatch` permits error
classification.

## Bot proxy integration

Go exports `QueuedProtocolVersion`, `HWQueuedHello`, `HWQueuedSubmit`,
`HWQueuedConfig`, `HWQueuedSourcePolicy`, `HWQueuedStats`, `HWQueuedTXEvent`.
Call `radio.SharedPHYCommandErrorCode(command, payload)` before forwarding
hardware commands. Zero means this authority guard permits forwarding;
nonzero is a native KISS HW_ERR code to return locally. Keep existing legacy
identity/configuration/reboot handlers: the guard does not replace them.

HELLO has exactly two payload bytes, `[version, claim_owner]`, with ownership
at offset 1. Only `[1,0]` is allowed through the bot endpoint. CONFIG GET
`[1,0]` is allowed for mandatory generation/profile readback; CONFIG SET and
all other CONFIG forms are rejected. Submission,
statistics and source policy continue to be validated by the physical modem.
Legacy DATA passes through unchanged and receives the physical TxDone.

## Native PHY-less adapter boundary

`RemoteKissRadio` uses queued protocol v1 by default. The patched native
Dispatcher passes priority and remaining delay to the physical scheduler,
correlates admission with terminal outcomes and keeps queue wait separate from
RF time. The modem performs carrier access and airtime accounting. On disconnect
or an uncertain write, pending jobs resolve UNKNOWN without automatic replay.
Joining requires HELLO, CONFIG readback, matching radio settings, source-policy
readback and an authoritative airtime table. See
[the native adapter contract](RemoteKissRadio.md) for the full sequence.

Explicit `RemoteKissRadio::Mode::Legacy` uses KISS DATA/TxDone instead of queued
admission. Legacy clients can join through exact-match configuration no-ops but
cannot change the owner-managed profile or share queued scheduling semantics.
