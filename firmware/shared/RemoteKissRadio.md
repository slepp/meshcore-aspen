# Queued remote-radio adapter

`RemoteKissRadio` defaults to required queued protocol v1 over a duplex
Arduino `Stream`. The radio-free ESP32 uses `Esp32RemoteKissRadio` and its
reconnecting WiFi/TCP stream to a separate ESP32 shared modem. There is no automatic
legacy fallback. `RemoteKissRadio(stream, RemoteKissRadio::Mode::Legacy)` is
the explicit compatibility mode and does not provide queued scheduling parity.

Apply `firmware/shared/queued-dispatch.patch` to upstream revision
`d92964352441e53b93e8667b802e04f6e072b39e`. Copy `RemoteKissRadio.h/.cpp`,
`Esp32TcpKissLink.h`, `QueuedTxProtocol.h` and `RadioNetwork.h` into
`src/helpers/remote/`.
The patch leaves real-chip radio behaviour selected by default.

## ESP32 commissioning

The repository-owned board binding and TCP link are in
`firmware/remote-radio/esp32/`. Preparation copies the board binding into
`variants/phyless_xiao_s3/` in the generated upstream project; that directory
name is a framework convention. Run `make remote-radio-config` to create
`firmware/platformio.phyless.ini`, configure it, then use
`make remote-radio-firmware` and `make remote-radio-upload`.

The TCP adapter sets `KISS_WIFI_TX_POWER` after starting WiFi STA and before
association. Its default is `WIFI_POWER_8_5dBm`, matching the tested physical
XIAO configuration. Operators can override it with another ESP32 WiFi power
constant. Setup failures stop initialization with a credential-free diagnostic.
Initialization is idempotent and does not reset a running connection.
`make -f test_support/phy_parity/Makefile esp32-link-test` checks startup order,
default/overridden power and hostnames, idempotence and explicit setup failures.

The device hostname defaults to `meshcore-phyless`, independently of
`KISS_MODEM_HOST`, which names the remote radio service. Set `KISS_HOSTNAME` to
a unique DNS label per device. Existing explicit `WIFI_HOSTNAME` or `HOSTNAME`
settings are preserved when `KISS_HOSTNAME` is absent. Shared `RadioNetwork.h`
validation requires 1..31 ASCII letters, digits or interior hyphens, without
a `.local` suffix. This configures the station hostname, not an mDNS server.

Before flashing, replace the sample credentials and pin all five native
`LORA_FREQ` (MHz), `LORA_BW` (kHz), `LORA_SF`, `LORA_CR` and `LORA_TX_POWER`
build settings to the operator's committed physical profile. Do not commission
with inherited upstream radio defaults. Saved role preferences must also
match. None of these adapter settings retunes the physical radio.

## Native boundary

The additive `mesh::Radio` methods are:

```cpp
bool supportsQueuedTransmit() const;
bool queuedReady() const;
bool setQueuedSourcePolicy(float factor);
bool queueTransmit(const uint8_t* bytes, int length, uint8_t priority,
                   uint32_t remaining_delay_ms, uint32_t expiry_ms,
                   uint32_t& job);
bool pollQueuedResult(mesh::QueuedTransmitResult& result);
bool lastReceiveWasLocal() const;
bool getQueuedRadioStats(mesh::QueuedRadioStats& stats) const;
```

`QueuedTransmitResult` contains `job`, protocol-v1 `state` and `reason`,
`queue_ms`, `rf_ms`, `estimated_ms`, and `has_rf_ms`. Admission and terminal
results remain separate even when both arrive in one stream read. A local
disconnect/write uncertainty has state UNKNOWN and `has_rf_ms=false`;
an MCU terminal result has actual occupancy available, including conservative
RF-timeout charging. Admission is never logged as successful transmission.

`queueTransmit` returning true transfers outcome ownership, not RF success.
An ambiguous write returns true and produces a correlated UNKNOWN; it must
not invite replay. False means no ownership transfer. `setQueuedSourcePolicy`
accepts a finite nonnegative native float request; `queuedReady()` stays false
until exact effective-bit confirmation. Runtime factor 99 and zero are valid.

The patched Dispatcher retains at most 12 packet/job correlations. It passes
native priority and delay at submission, without waiting for local eligibility,
CAD or a second airtime budget. The MCU selects eligible work across sources.
Ordinary native jobs submit expiry zero: they have no queue-age limit, matching
the native Dispatcher. Explicit caller expiry is forwarded unchanged, including
a deadline before eligibility. The MCU alone enforces that expiry and its RF
timeout; the client runs no terminal or queued-job-age timer. Admission
acknowledgement and serialized controls retain bounded transport deadlines,
so lost connections resolve UNKNOWN without replay while healthy long-waiting
jobs remain owned.
`logQueuedTxResult(Packet*, const QueuedTransmitResult&)` exposes exact states
for native application diagnostics; existing success/failure hooks still run.

Packets submitted while unavailable or over capacity fail explicitly through
the existing TX-failure hook; they are not silently retained for later replay.
The native packet manager still owns delayed inbound processing.

## Joining and reconnecting

The bounded, serialized join is:

1. HELLO v1, without claiming configuration ownership.
2. CONFIG GET, checked against native radio preferences and the pinned profile.
3. Exact-match SET_RADIO confirmation. Negotiated queued v1 guarantees this
   operation is a no-op and acknowledges it only for a committed,
   non-quarantined profile. This verifies durability, which CONFIG GET alone
   does not expose. It cannot retune, write flash or acquire ownership.
4. Source-factor readback, signal metadata enable/readback and noise-floor read.
5. Actual GET_AIRTIME for every length 0..255, followed by CONFIG revalidation.
6. An initial authoritative STATS snapshot before declaring readiness.

The airtime table is immutable while ready and unavailable until complete.
Malformed replies, deadlines, policy mismatch or changed configuration leave
the adapter offline. Reconnect repeats verification and restores source policy,
without CONFIG SET or radio retuning. TCP backs off to 30 seconds after a
quarantined connection. Pending jobs resolve UNKNOWN and are never replayed.
Job IDs do not reset across reconnects; retired generation events are ignored.

UART requires a physical service endpoint implementing queued v1, not an
unmodified legacy KissModem UART. Call `onLinkDisconnected()` and
`onLinkConnected()` for real transport resets. A timed-out uncorrelated control
request requires a genuine session reset/drain before reconnect; a UART client
must not blindly retry it into a stream containing late responses.

### Optional physical UART endpoint

Build the physical multiplexer with `KISS_STREAM_ENDPOINT=1` to enable one
additional duplex `Stream` source. The flag defaults to zero, which omits its
buffers. Public integration methods are:

```cpp
bool attachStream(Stream& stream);
void pollStream();
bool streamFaulted() const;
uint32_t streamOutputOverflows() const;
```

Attach a dedicated, already-configured `HardwareSerial` once; a second
attachment fails without replacing the first. The caller owns its lifetime and
must provide explicit RX/TX pins and baud. Never attach diagnostic `Serial`.
Call `pollStream()` on the radio loop even when WiFi is disconnected. Only this
loop may access the endpoint; the stream must accurately report nonblocking
write capacity through `availableForWrite()`, with no competing writers.

The stream has its own source slot after the TCP and local-source slots. It
consumes neither TCP sockets nor local-source capacity. Jobs use the existing
shared queue, priority selection, aggregate credit and source policy.
Input work is limited to 512 bytes per call. Its output ring is 2060 bytes;
unsolicited RX/reflection traffic cannot use the final 1030 bytes reserved for
control and completion traffic. Writes honour current transport capacity and
partial progress, with no flush wait.

Only HELLO is accepted before negotiation. Every successful HELLO retires the
previous UART generation, queued jobs, configuration acknowledgement, source
policy and owner lease, then returns a fresh generation. Malformed or refused
HELLO requests do not renew or acquire ownership. Aggregate credit survives
renewal. An old generation's already-started RF operation still finishes and
charges the aggregate exactly once, but cannot complete a new job or charge
the new source. A renewed session can receive that old successful transmission
as a local reflection; its old terminal event is not delivered.
Rejections of late SUBMIT frames retain the request's generation in the event,
so an old frame cannot terminate a newly admitted job with the same ID.

Output overflow increments the cumulative in-memory counter, logs a diagnostic
and faults only this source. Pending UART jobs and buffered output are retired;
TCP/local writers and an already-started RF operation continue. A valid fresh
HELLO is required to recover. Normal commands cannot revive a faulted session.
Retirement clears the mux's software output, not bytes already accepted by a
hardware UART FIFO. Clients must drain/reset stale input before retrying a
timed-out uncorrelated control exchange.

`make -f test_support/phy_parity/Makefile stream-test` exercises the actual
modem and adapter with the endpoint disabled, enabled alone, and enabled with
three local sources. `stream-firmware-build` compiles default and stream-enabled
ESP32 physical images from public configuration. For a wired deployment, use a
private board profile with dedicated UART pins and baud.

In queued mode, DATA must be followed immediately by an exact RX_META frame
(port zero, SETHARDWARE, F9 and two signal bytes). Intervening control/data
frames, malformed escapes, aborted frames and receive-buffer overflow retire
the pending association with a diagnostic and receive-error count. Ordinary
empty KISS delimiters and fragmented valid metadata remain supported.
Already-complete buffered packets survive later framing errors or queue
overrun; metadata for a dropped newer packet cannot modify them. Explicit
legacy mode retains its previous metadata compatibility behaviour.
Missing metadata also drops the packet rather than fabricating RF readings.
The established local-reflection marker becomes
`Packet::_localReflection`; local packets reach application processing without
synthetic signal logging, RX-airtime charging or RF retransmission.
The patched native `Mesh.cpp` blocks direct ACK and multipart-ACK forwarding
before replacement packets can be created. Addressed ACK callbacks still run,
multipart views retain local/signal metadata, and applications can create
legitimate new replies.

## Authoritative statistics

`getQueuedRadioStats()` copies the existing MCU STATS response into the
`mesh::QueuedRadioStats` type from the patched `Dispatcher.h`. It returns false
without modifying the output when statistics are unavailable. The snapshot
contains the connection `generation`, `configuration_generation`, local
`captured_ms`, and these explicitly scoped fields:

- `aggregate_credit_ms`, `source_credit_ms`
- `aggregate_rf_ms`, `source_rf_ms`
- `aggregate_successes`, `aggregate_failures`, `source_successes`, `source_failures`
- `aggregate_queued`, `aggregate_transmitting`

RF counters are MCU-reported accounting, never queue time or a LoRa formula.
They include conservative charging after an uncertain RF timeout, not just
confirmed successful transmissions. Per-job state/reason and `has_rf_ms` retain
that distinction. Source counters belong to the current physical source
generation and restart on reconnect; aggregate counters/credit survive client
reconnects. The snapshot does not supply measured RX duration.

After the initial snapshot, a refresh starts no sooner than one second after
the previous response. It shares the existing single outstanding control
transaction with source-policy changes; it is not a scheduler or budget.
The three-second control deadline still applies. Policy changes, disconnects,
faults and snapshots aged four seconds or more are unavailable. Invalid length,
version, configuration generation, credit domain, queue count or transmitting
flag quarantines the session and resolves outstanding transmissions UNKNOWN.
No malformed or partial snapshot becomes visible.

The Dispatcher exposes the same snapshot and validity-aware helpers:

```cpp
bool tryGetRemainingTxBudget(uint32_t& credit_ms) const;
bool tryGetTotalAirTime(uint32_t& rf_ms) const;
```

In queued mode they return the lesser of the reported source/aggregate credit
and the reported current source RF counter, respectively. False leaves the
output untouched. Existing scalar getters use these values, or the explicit
`Dispatcher::UNKNOWN_RADIO_STAT` (`UINT32_MAX`) sentinel when unavailable;
callers must not display the sentinel as a physical value. Prefer the boolean
APIs, which also disambiguate a valid counter reaching `UINT32_MAX` before
wrapping. Real-chip scalar behaviour is unchanged.

`getTotalAirTimeSeconds()` preserves the unknown sentinel while converting
valid readings. The patch wires all three native applications' binary seconds
fields to this helper. Repeater/room JSON formatting passes explicit validity
to `StatsFormatHelper`, whose existing callers retain millisecond-input
behaviour by default. Unknown seconds remain `UINT32_MAX`, never the plausible
`4294967` seconds obtained by dividing an unknown millisecond sentinel.

`getReceiveAirTime()` remains the native accumulated RX **estimate**, not
measured occupancy. The wire STATS queue count covers all physical sources;
it cannot identify which source is transmitting or provide a remote
per-source waiting count. Never label aggregate occupancy as a role queue,
or substitute the unused native packet-manager queue. The in-process mux
source getters below provide that distinction for embedded roles.

## Transport diagnostics

USB companion applications keep transport diagnostics separate from the binary
companion stream. `make peer-diagnostics PEER_SERIAL=...` reads the bounded
diagnostic history through native Stats subtype `0x80`, including failures and
their numeric transport details. Repeater and room consoles retain text output.

## Identity generation

PHY-less target `radio_new_identity()` should return
`RemoteKissRadio::newIdentity()`. This supplies the entire Ed25519 seed from
ESP32 hardware entropy, with no
millisecond-clock fallback. Hardware entropy failure stops identity creation.
ESP32 identity generation must occur after WiFi has enabled the hardware
entropy source, as in the TCP target initialization.

`getRngSeed()` now supplies hardware randomness for native non-cryptographic
PRNG callers, but a 32-bit `StdRNG` seed is not a substitute for `newIdentity()`.
Existing persisted identities are not replaced.

## Validation

`make -f test_support/phy_parity/Makefile test` compiles the actual patched
Dispatcher, Packet and StaticPoolPacketManager with the adapter. It also
connects the adapter through socket streams to the actual pinned KissModem
and physical multiplexer, checking negotiation, no retuning, priority,
measured completion and sender-excluding local reflection.

`make -f test_support/phy_parity/Makefile queued-firmware-build` builds native
repeater, room and companion applications for ESP32 using
`firmware/remote-radio/esp32/platformio.ini.example`. No command accesses hardware.

## Embedded telemetry and UART wiring

Local application queue telemetry can use the mux's read-only, radio-loop
methods `uint8_t sourceQueuedCount(uint8_t slot) const` and
`bool sourceTransmitting(uint8_t slot) const`. The count includes only waiting
jobs belonging to the currently attached source generation, not control
requests or current RF. The boolean identifies that generation's current RF
operation. Inactive slots return zero/false; out-of-range slots additionally
log an invalid-query diagnostic. Neither getter changes scheduling, credit or
expiry processing. Combine the two to determine pending physical work.

The optional UART endpoint requires dedicated operator-selected pins and baud;
it speaks queued v1 rather than legacy serial KISS. The ESP32 remote-role target
uses `newIdentity()` for fresh identities.

Native packet-manager outbound-queue getters still describe local state, not
MCU queued jobs. Embedded applications must use the source getters above;
remote applications can expose the explicitly aggregate STATS queue snapshot.
The native scalar RF/budget getters use the unknown sentinel described above;
use the validity-aware getters when displaying these measurements.
