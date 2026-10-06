# Run a separate Hew Base companion

Build the companion, stage a frozen role into a **new private directory**, and
run `build/hew-base-release CONFIG.json`. The process serves MeshCore companion
TCP clients and opens one ordinary source connection to the shared modem. It
does not reserve Willow's aggregate MKISS ports or change the physical profile.

**Stop the previous companion service before starting its retained identity
here.** Check the SDK and RF commands your installation needs before handover.
Runtime `/readyz` checks the running connection and state; protocol coverage
remains partial.

## Build and stage

```sh
cd experiments/hew-roles
make base-service
python3 -B migrate_base.py stage \
  --source /path/to/frozen/base \
  --destination /path/to/private/base-hew
```

The build uses Hew **0.6.0-rc7**, a C compiler, OpenSSL and libsodium. Production
does not start Go, a shell, or an RPC worker. Go is used by the test oracle only.
The compiler-version check protects the native byte-return ABI.

The frozen source and destination parent must be owned directories with mode
0700. Input files must be owned regular files with mode 0600, without hard links
or symbolic links. Neither the destination nor its `.staging` sibling may
already exist. The helper checks existing role locks, rejects a changing source
and pending identity transitions, and never generates an identity.

The authoritative identity and document are selected in this order:

1. `identity-state.json`: version-1 Go authority envelope, native 64-byte
   scalar/nonce-prefix identity, embedded companion document.
2. `identity.expanded` and `companion.json`.
3. `identity.seed` and `companion.json`.

A corrupt envelope fails instead of falling back. All retained files, unknown
document fields and unknown preferences are copied unchanged. The migration
manifest reports the selected authority, counts and file hashes; it does not
claim that a radio or running service was checked.
An authoritative `state:null` is a reset marker, not permission to load the
retained old document. Staging keeps that marker and its key unchanged; startup
initializes fresh role data using that identity and the explicit configuration.

## Configure and run

Write an owned 0600 JSON configuration, at most 4096 UTF-8 bytes:

```json
{
  "state_dir": "/path/to/private/base-hew",
  "radio_address": "10.0.0.10:8001",
  "companion_listen": "127.0.0.1:5000",
  "companion_allow_remote": false,
  "required_profile": "c806643690d003000705020000803f010000",
  "status_listen": "127.0.0.1:9081",
  "status_role": "companion",
  "enable_restart": true,
  "enable_factory_reset": false,
  "enable_key_export": false,
  "enable_key_import": false,
  "run_ms": 0
}
```

Use that radio address only for an authorized installation. The example profile
requires 912.525 MHz, BW250 kHz, SF7, CR5, TX2, airtime factor 1 and CAD enabled
with threshold 0. Startup checks the modem readback and confirms the saved
source airtime factor. A different readback fails; it does not retune the modem.
The running service rechecks readback and samples optional physical measurements
every 15 seconds. A malformed reply, unexpected hardware error, query timeout or
changed profile retires that source connection. The saved name, location,
channels and preferences remain authoritative.

`required_profile` is exactly 18 bytes encoded as 36 hexadecimal characters.
The Base checks it before opening the modem or loading role state: frequency
150–960 MHz, a supported LoRa bandwidth, SF5–12, CR5–8, reported TX power
0–30 dBm, a finite nonnegative binary32 airtime factor and CAD flag 0 or 1.
The final signed 16-bit interference threshold retains its wire meaning.
An invalid field prints `BASE_CONFIG_ERROR required_profile: ...` on stdout and
exits with status 1; correct the JSON profile before restarting. Reported power
is readback, not permission to change the hardware; the Go operator
configuration still limits requested TX power to 22 dBm.

SF5 and SF6 use receive score zero. With `rxdelay` enabled, the Base calculates
the same score-zero delay as the Go companion: values below 50 ms do not hold a
packet, and holds are capped at 32000 ms. The SF7 default and scoring thresholds
for SF7–12 are unchanged.

```sh
build/hew-base-release /path/to/private/base.json
curl --fail http://127.0.0.1:9081/readyz
curl --fail http://127.0.0.1:9081/status
```

`BASE_LISTEN` reports the bound TCP port and public key after negotiation.
Connect a MeshCore companion client to the configured TCP endpoint. Protocol
13 advertises 350 contacts, 40 channels, 256 retained messages and the saved
path hash mode. Each client has its own history cursor, signing buffer and
scope selection. Reconnecting starts a fresh client cursor.

TCP has **no authentication**. The default companion bind is literal loopback.
Binding elsewhere requires `companion_allow_remote=true`. The authorized
`0.0.0.0:5000` setting opens an IPv6 wildcard listener with IPv4 compatibility,
matching a dual-stack `[::]:5000` endpoint. The status listener always requires
literal loopback. Omit `status_listen` to disable it.

`GET /status` returns `{"companion":{...}}` with the actual public key,
application lifetime, connection, listener, state and queued-transmission
accounting. `GET /readyz` returns `ready`, a `not_ready` map keyed by the role,
and `mqtt_connection_checked:false`; disconnected or faulted roles return HTTP
503. Both endpoints accept bounded HTTP/1.0 or HTTP/1.1 requests and close the
connection. The dashboard gateway can retain port 9080 and read this service
on 9081. An independent second instance uses `status_role:"bot_companion"`
and its own loopback status port, for example 9082.

Status includes the cached physical modem telemetry with explicit `has_*`
flags and `telemetry_counter_scope:"shared_physical_modem"`. Missing callbacks
leave those flags false; disconnect invalidates the sample immediately and
samples older than 45 seconds are unavailable. Requests never trigger a live
measurement. Battery command 20 includes the shared state filesystem's used
and total KiB, not a per-role quota. An unavailable battery or unrepresentable
filesystem capacity returns `BadState`. Statistics command 56 supports the
actual firmware packet counters; core/radio subtypes remain `UnsupportedCmd`,
matching the Go application bindings. Local and permission-filtered incoming RF
telemetry use the measured battery voltage. No additional sensor provider or
measured GPS fix is configured.

`companion_policy` accepts the Go policy override names. Only supplied,
non-null fields replace saved values; explicit zero and false are applied.
Unknown saved preferences remain in the document. `advert_interval_seconds`
sets the flood advert interval only when a policy flood interval is absent.
Optional `companion_retention` accepts `native_queue`/`durable_replay` or their
hyphenated document spellings. Omit it to retain the saved profile.

Identity export/import remain disabled unless explicitly enabled. Enabling them
on an unauthenticated listener lets its clients export or replace the identity.
Import commits a Go-compatible authority envelope before activating the new
native scalar-prefix key, retains contacts/channels/history/preferences, and
clears transient requester and signing state. **Do not enable these flags as
part of a handover that previously disabled them.**

Command 19's `reboot` marker admits a logical application restart; accepted
requests send no completion ACK. The independent driver closes clients,
retires uncertain source jobs, flushes the role, then reloads the same
identity/state and TCP endpoint. Its directory lock stays held throughout.
It does not reboot or tune the shared modem. `enable_restart` defaults true;
set it false to leave that callback unsupported.

**Factory reset erases this role's identity, contacts, messages, channels,
location and preferences.** Command 51 is unsupported unless
`enable_factory_reset:true` is explicitly set for `status_role:"companion"`.
It is never enabled for the independent bot companion. After the old role
closes, reset atomically commits a new expanded identity and fresh document,
then starts the role with the configured name and policy. The retained old
seed cannot override that authority. `companion_name` sets the fresh name
(default `Shared Radio Base`); a fresh profile uses durable replay unless
`companion_retention` selects native queue. Keep the previously disabled
factory-reset flag disabled during handover.

A second companion uses a separate process, configuration, role directory,
identity, endpoint and ordinary source connection. It does not consume an
aggregate port or send a Base role-presence announcement.

## Retention and transmission

Both `native-queue` and `durable-replay` documents are supported. Direct and
signed room messages are acknowledged only after their journal/cursor commit.
At 256 records, channel records are reclaimed first. Durable direct history
can be reclaimed only after every connected reader has fetched it. An unread
full direct journal refuses the new record and withholds its ACK. A directory
fsync failure after rename faults the role rather than reporting a safe commit.

The source queue admits at most 32 outstanding transmissions. TCP permits 32
clients, with 512 response frames per writer, bounded framing, and 10-second
partial-frame/write deadlines. Signing buffers are bounded at 8192 bytes;
requests, login fences, traces and pending ACKs are bounded at 128 each.
Delayed RF reception is bounded at 256 packets. Packet dedup uses the native
160-entry fingerprint cache.

Repeat forwarding and RX delay use saved preferences, native float32 timing,
path widths and priorities. Direct ACK relay preserves multipart copy spacing.
Trace route hashes have widths 1/2/4/8; their received path contains SNR bytes.
Local reflection does not supply RF signal measurements or trigger forwarding.

The modem's accepted, succeeded, failed, rejected and unknown events drive
`companion.role_tx`. Only terminal reported RF airtime is summed. An uncertain
completion clears `airtime_complete`; queue wait and estimates are not added.
`application_started_at` and `role_tx.since` are UTC RFC3339 strings. No MQTT
connection is checked by this standalone service.

After a source disconnect, existing TCP connections, client history cursors,
pending RF requests and the application lifetime remain in this process.
Local commands continue; new RF transmissions return `BadState` until a new
ordinary source is negotiated. Dial/negotiation runs independently, with
bounded retry backoff. Outstanding old-source jobs become uncertain and are
never replayed automatically. Recovery rechecks the required physical
profile, reapplies the saved source factor and refreshes airtime estimates.
RF frames received during negotiation, including a partial final KISS frame,
are handed to the running receiver. Losing confirmation of a source-policy
change faults the role instead of retrying an ambiguous mutation.

## Stop and reverse handover

Send SIGTERM and wait for `BASE_STOP ... state_saved=true` and process exit.
Outstanding jobs are uncertain, not successful transmissions. Freeze the
stopped candidate before copying it back:

```sh
python3 -B migrate_base.py rollback \
  --source /path/to/frozen/base-hew \
  --destination /path/to/private/base-go-new
```

The reverse operation copies the updated Go-compatible authority, identity,
contacts, messages, channels, preferences and replay sequence to a new
directory. It never overwrites the original Go snapshot. Load that new directory
with the ordinary-source Go configuration; do not start both applications with
the same identity. Reverse staging is an offline file operation, not a running
service switch.

## Local checks

```sh
make base-local-test base-service-test
make native-worker
BOT_NATIVE_WORKER="$PWD/build/native-worker" \
  python3 -B ../../internal/nativebot/worker_test.py \
  WorkerProcessTest.test_hello_spreading_and_reported_power_boundaries
make base-symbols
MESHCORE_HEW_BASE_SERVICE="$PWD/build/hew-base-symbols" \
  python3 -B tests/base_service.py
```

`make test-release` uses `MESHCORE_NATIVE_TEST_STATE_ROOT` for short private
native fixture directories under the source root; Go `TMPDIR` stays in
`build/`. For direct Go native-worker tests, set the fixture root separately
so the complete `bot/native/admin.sock` pathname is at most 107 bytes on Linux.

The local Go differential compares native command responses, independent
reader/signing behavior, production-sized state, durable capacity/refusal,
restart, private-message semantics, channel wire packets and raw TX priority.
It also compares admission/validation responses for command numbers 1–65.
Receive-policy cases compare SF5/6 score-zero delay with Go at three airtimes,
including the minimum-hold gate and maximum-hold clamp. Service fixtures boot
SF5, SF6, SF7 and SF12 without modem configuration writes, and reject invalid
required profiles before any modem connection. The worker HELLO fixture checks
those spreading boundaries and the reported-power limit without RF.
The RF differential compares login, status, telemetry, binary, anonymous and
path-discovery request payloads and requester-owned replies, direct raw data
and channel datagrams with all ordinary path widths, flood and maximum-sized
datagrams with both journal cursors, all four TRACE widths/flags, and control/raw
diagnostic pushes. Its fixtures use valid channel hashes and distinct inbound
packets so originated-packet dedup does not substitute for reception.
Valid RF packet comparisons remain a selected set, not an all-command RF
differential.

The actual TCP/source scenarios use synthetic identities, a bounded modem
emulator, and the upstream MeshCore Go SDK. They check a 167-contact/256-message
handover, both client cursors, encrypted DM/native ACK, CLI, room login/history
and cursor persistence, channel 39, signed advert import, source-factor apply,
identity authority/restart/rollback, repeat/delay/trace, uncertain outcomes,
source/client limits, dual-stack binding and frozen-input refusal.
Source-outage checks keep both TCP clients and their cursors, exercise offline
commands, account an uncertain job without replay, receive an advert split
across negotiation, and check role-keyed health after reconnect.
The measurement scenario checks actual synthetic hardware replies, filesystem
capacity, packet statistics, self/RF voltage telemetry, cached HTTP flags,
periodic readback mismatch and source retirement without tuning commands.
Both seed and changed expanded-identity reverse handovers are loaded through
Go's authority reader and companion engine in a separate copy; the frozen
candidate remains unchanged.
Two independent engines share the same synthetic modem endpoint with distinct
identities and ordinary sources. That scenario checks per-role journals,
commit refusal without an RF ACK, successful retry, disabled secondary reset,
cached health and slow-writer retirement without starving either engine.
Logical lifecycle checks retain the selected port and identity on restart,
exercise opt-in destructive reset, and load its new authority through Go.
They require no physical radio or public MQTT server.

`base-symbols` uses the same native bindings with `-g --opt-level 2`. Building
and running it is an ABI/lifecycle check. Profiling belongs after the complete
replacement is qualified and Go has stopped.

## Protocol coverage

The named scenarios below cover specific SDK and RF commands, not the full
companion API. `protocol_coverage` reports `partial` independently of runtime
readiness. Before changing the owner of a companion identity, check any other
commands required by your radio, client application or automation.

See [`internal/companion/doc.go`](../../internal/companion/doc.go) for the full
native companion contracts and extensions. These differences are deployment
decisions, not additional state formats or identity-generation gates.
