# Run MeshCore Willow

For an existing Birch node, use [the offline migration procedure](MIGRATION.md).
It keeps the relay, room, bot and observer identities and their supported saved state.
Then run:

```sh
python3 -B willow.py check --state /path/to/private/willow
python3 -B willow.py run --state /path/to/private/willow
```

`check` does not open the modem. `run` verifies state and the native worker,
then replaces the launcher process with the Hew service. Relay, room, bot and enabled observer
must no longer be running in Birch under the same identities.

For a separate native Base candidate, use [the Base companion guide](BASE.md).
It has its own TCP clients, frozen-state migration and ordinary modem source;
it does not occupy the four aggregate ports. The guide lists the remaining
differences before replacing the existing Base service.

Use `make demo` first. It starts private loopback modem emulators and the real
`build/hew-host` executable, exchanges packets, checks recovery, and removes
the synthetic service identities when each scenario finishes. It also builds
the repository's arithmetic Wasm example, provisions it into a private native
extension, and calls `!wadd 17 25` through KISS. The Lua bot remains active.

```sh
cd experiments/hew-roles
make native-worker
export BOT_NATIVE_WORKER="$PWD/build/native-worker"
make demo
cat build/service-results.json
```

The worker is built from this worktree using the Go host's native Make recipe,
with Lua and Wasm enabled. An installed worker or copied binary is not accepted.
See [worker build and verification](#worker-build-and-verification),
[the native worker protocol](../../internal/nativebot/PROTOCOL.md) and
[the interpreter build guide](../../firmware/runtime/WASM_RUNTIME.md).
The service does not start Go, invoke a shell, or connect to an RPC endpoint.

## Pin the installed build

Build in the final, revision-pinned worktree or installation root after the
coordinated source commit. Do not deploy from a changing development worktree.
Use a short absolute install path: the Go differential reference creates a
private Unix socket beneath `build/`. On Linux its complete pathname must fit
107 bytes. The differential checker reports an overlong path before starting
the reference worker; move the pinned worktree to a shorter path and rebuild
there so its native-worker binding remains correct.
Worker paths and compiler-input hashes are bound to that location; copying
only a worker or executable to another directory is not sufficient.

```sh
cd /path/to/pinned-root/experiments/hew-roles
make native-worker build/hew-host-release
make readiness-test migration-test library-test supervision-test
python3 -B build_worker.py --verify --worker "$PWD/build/native-worker"
```

Run migration from that same pinned root, so the generated private configuration
selects its worker. Keep the source/build inputs available and unchanged.
A service manager can invoke the runner directly:

```ini
[Service]
WorkingDirectory=/path/to/pinned-root/experiments/hew-roles
ExecStart=/usr/bin/python3 -B /path/to/pinned-root/experiments/hew-roles/willow.py run --release --state /path/to/private-willow
Environment=HEW_WORKERS=4
Restart=on-failure
RestartSec=5
TimeoutStopSec=10
```

Use four Hew scheduler workers for a small host deployment, including each
native Base and dashboard service. Set `HEW_WORKERS=4` in those services as
well. Without this setting the runtime selects workers from the machine's
available CPU count; extra workers can retain substantially more allocator
memory during repeated JSON reads. This setting does not change identities,
saved data or radio connection limits. The C++ Lua/Wasm worker is separate.

The installed release needs no FFI-path or `LD_LIBRARY_PATH` override.
The existing C bridge objects are linked into the
executable; its shared dependencies are system libraries. Keep the complete
build/source installation at its final path: `worker_identity.hew` binds the
absolute native-worker and input-manifest paths. `BOT_NATIVE_WORKER` selects
test helpers, not the production worker; migration writes the verified worker
path into the private configuration. The compiler is needed to build, not
to launch an already built release.

Run it as the user owning the private state. Run Base separately under its
companion identity, using [native Base](BASE.md) or an existing Go service
with `enabled_roles=["companion"]`. Disable the Go observer when enabling
Willow's observer. Do not run overlapping identities.
For a Go companion-only service, set `radio_session="per_role"` so it does not
occupy the shared modem's sole aggregate MKISS session.
These commands do not install or modify a system service themselves.

## Read health and administer the host

The running Hew host accepts health, DM status and inbox requests at
`STATE/owner.sock`, and scoped commands at `STATE/admin.sock`. These commands
use the running roles; they never start another host or worker.

Run the client as the service UID. The state directory must be mode `0700`,
the Unix `SOCK_SEQPACKET` socket mode `0600`, and the complete socket path at
most 107 bytes. Both endpoints verify the other's UID. There is no TCP control
listener. `BOT_READY native-extension` identifies native readiness;
`inbox` returning a cursor checks that the local control actor is responsive.
`OWNER_CONTROL_OFFLINE` means the control actor is unavailable; relay, room
and observer supervision remains independent.

Read actual actor, modem and MQTT health, or submit a scoped local setting:

```sh
python3 -B willow.py health --state "$STATE"
printf '%s\n' 'bot status' | python3 -B willow.py command --state "$STATE"
printf '%s\n' 'get name' | python3 -B willow.py command --state "$STATE" --role room
```

`health` returns public `roles` and `readiness` objects. Missing or stale
supervisor samples fail the request; disconnected radio or MQTT connections
remain visible in `readiness.not_ready`. Physical telemetry is omitted until
sampled and after its 45-second cache expires. The 256-entry `airtime_ms` table
comes from the modem, and `phy_configuration_generation` is its configuration
readback generation, not the host restart count.

`command` accepts the dashboard's bounded command set: bot/source/data
management and room/repeater names, forwarding and advert intervals. Named-region
commands manage the running role's region table and persist its selections.
Credentials,
private identities, shared PHY changes and reboot are
excluded. Role changes use the running role actor and commit its snapshot
before replying. Bot requests are correlated with the native worker's reply,
including when the selected source is unavailable but management is running.
Keep commands on stdin or in an owned mode-0600 `--message-file`. They are not
logged, published to MQTT or sent over RF.

An error reply exits 2; a transport failure exits 1 and may leave the outcome
unknown. Do not repeat a write after an uncertain outcome. Inspect the setting
first. `admin.clock` durably reserves request IDs before submission and is
preserved by frozen rebind and Go reconciliation. Commands use a separate
mode-0600 `STATE/admin.sock`, so an outstanding command does not hold up
health samples, DM status or inbox reads at `STATE/owner.sock`.
HTTP dashboard deployment is separate; these local commands do not open a
TCP listener.

## Run the local host dashboard

Build the native HTTP service with `make dashboard`. It reads the running
Hew host's private sockets and the Base and bot companion health endpoints;
it does not start another radio connection. Keep the companion listeners at
their existing ports. Give their health listeners separate loopback ports,
then configure the dashboard to own the original host health port:

```ini
address=127.0.0.1
port=9080
state=/absolute/path/to/willow-state
page=/absolute/path/to/meshcore/internal/app/admin_page.html
base_port=9081
bot_companion_port=9082
password_env=MESHCORE_LAB_ADMIN_PASSWORD
```

Save this configuration in an owned mode-0600 file. Run
`MESHCORE_HOST_ADMIN_HTTP=1 ./build/hew-dashboard-release /path/to/dashboard.conf`
with the role admin password already supplied by the service environment.
Open `http://127.0.0.1:9080/admin` and log in. Do not put the password in a
command argument or the dashboard configuration. Alternatively, set
`token_env` to the name of an environment variable holding an explicit
32-256-character admin token. Password login requires 12-256 printable
non-space ASCII characters; sessions expire after ten minutes.

`GET /status` returns public operational state. `GET /readyz` returns HTTP
503 when a role or its health connection is unavailable. Samples expire
after three seconds. These routes remain available with admin access
disabled. `base_port=0` or `bot_companion_port=0` disables a health source
for an isolated test; the corresponding role stays not ready.

Commands preserve the scoped bot, room and repeater permissions of the
host dashboard. HTTP 409 reports a rejected command; HTTP 504 means the
outcome is unknown. Inspect the affected state before another write.
Restarting the HTTP service drops login sessions, not role identities,
bot programs or saved role settings. Run `make dashboard-test` for the
isolated native HTTP, owner-command and host-restart checks.

## Configure a role's named regions

The room and repeater each own a separate table of up to 32 named regions.
For example, define a hierarchy and select its home and outgoing scope:

```sh
printf 'region def can ab edm\n' | python3 -B willow.py command --state /path/to/state --role room
printf 'region home ab\n' | python3 -B willow.py command --state /path/to/state --role room
printf 'region default ab\n' | python3 -B willow.py command --state /path/to/state --role room
printf 'region save\n' | python3 -B willow.py command --state /path/to/state --role room
```

`region` displays the hierarchy; `region list allowed` and `region list denied`
show flood admission. Home is the marked entry in that hierarchy. The outgoing
default scopes adverts and replies that have no permitted incoming scope.
Parentage does not inherit flood admission. A permitted private entry accepts
any of its four saved keys; replies use its first key.

An authenticated RF administrator can use `region load` to replace a table
with indented lines and finish with an empty line. Until that final line, the
running table remains unchanged. Another administrator cannot take over the
load; restarting the role discards its unfinished input. Region IDs, names,
parents, private keys, flags and selections otherwise survive restart. Run
`make region-test region-service-test` for the isolated command, RF, restart
and Go-reconciliation checks.

## Owner-command persistence and checks

`bot name NAME` updates both the native saved policy and its host startup
override before confirming success. Worker and host restarts retain that name.
`bot_name` in the host configuration supplies the initial name when no saved
override exists; change an installed bot's name through its owner command.

Check the local control contract without touching an installed service:

```sh
MESHCORE_GO_SOURCE=/path/to/current-go-checkout make admin-test
make owner-service-test
```

The Go reference checkout must include the room/repeater named-region dashboard
allowlist. The permission checker compares 520 commands against that checkout's
actual Go patterns in debug and release builds. The owner lifecycle uses a
private modem emulator and compiled native worker, including RF ACK matching,
independent inbox reads, local role commits, actor/modem health, bot-name
retention, restart handling and frozen Go reconciliation. It creates and removes
synthetic identities; it does not open the deployed radio or service state.

## Send a DM and read the owner's inbox

The host sends as its existing native bot, through that worker's contact
routing, admission, airtime budget and the host's tracked modem queue.
No bot program, grants or command policy change is required.

The recipient must be a **full 32-byte public key** with a fresh native direct
contact: a signed advert plus an authenticated learned path, both retained in
the worker's volatile contact table for at most ten minutes. After worker or
host restart, complete that exchange again. `fresh-direct-contact-required`
refuses a send without it; this interface does not invent a route or flood.

```sh
cd /path/to/pinned-root/experiments/hew-roles
# Keep this ID for later status checks; a new ID is a new transmission request.
REQUEST_ID=$(python3 -c 'import uuid; print(uuid.uuid4())')
printf '%s\n' 'Willow is ready; this message is from the Hew-owned bot.' |
  python3 -B willow.py send --state "$STATE" --request-id "$REQUEST_ID" \
    --to "$RECIPIENT_PUBLIC_KEY" --wait 50
python3 -B willow.py status --state "$STATE" --request-id "$REQUEST_ID"
python3 -B willow.py inbox --state "$STATE"
python3 -B willow.py inbox --state "$STATE" --after "$RETURNED_CURSOR"
```

Use stdin or `--message-file /private/message` (owned mode `0600`), not a
message argument exposed in process listings. Text is 1–162 printable ASCII
bytes; one final newline from stdin/file is removed. Neither sent nor received
plaintext is added to the service journal or observer output.

Send/status JSON separates `admitted`, `local_tx_confirmed`, and
`native_rf_ack`. `phase` is `pending-admission`, `admitted`, `queued`,
`local-tx-confirmed`, `native-rf-ack`, `failed`, or `unknown`. Local confirmation
means the modem reported a completed transmission, not delivery. A native RF
ACK is a matching MeshCore acknowledgement, not evidence that a human read it.
Wrong ACKs and local reflections do not acknowledge the send. The ACK wait
ends after 45 seconds; worker/modem/host interruptions leave unresolved
requests unknown, retaining any already confirmed flags.

Without `--wait`, exit 0 means the request/status was returned, not delivered.
With `--wait` (up to 120 seconds), exit 0 requires `native-rf-ack`; expiration,
failed and unknown outcomes exit 2. Transport/validation errors exit 1.
After any uncertain client result, query **the same ID**. Submitting the
same ID/content only returns its saved result; changed content is refused.
There is no automatic retransmission.

The private durable `owner.state` retains up to 128 request IDs and payload
digests, not messages or recipient keys. A full ledger refuses new requests
rather than evicting replay protection. Native sending has four active slots.
Do not delete the ledger to retry an uncertain send. It follows
[frozen upgrades and Go reconciliation](MIGRATION.md#upgrade-a-frozen-willow-state).

Inbox JSON contains full authenticated `sender`, native `timestamp`, `message`
and `sequence`. The sender is the contact whose key successfully decrypted
the packet, not its one-byte addressing prefix. It includes received plain
DMs, including bot commands, without changing their normal processing.
Retries/duplicates are suppressed before enqueueing. The inbox is RAM-only,
32 messages, with at most 16 returned per read; reads do not remove entries.
`dropped` reports overflow or native dedup-capacity drops. Continue with the
returned `generation:sequence` cursor. Host/control restart empties the inbox
and changes generation; an old cursor returns `inbox-generation-changed`.
Use a new `inbox` call without `--after` after acknowledging that loss.
Protect captured inbox output as private operator data.

`make owner-service-test` runs one localhost process lifecycle: owner submit,
real native encrypted packet, tracked TX, wrong/reflected/matched ACKs,
authenticated inbox with retry suppression, worker fault, host restart,
frozen rebinding and request-ledger preservation through Go and back.

## Start with new identities

The service runs relay on logical port 0, room on port 1, bot on port 2, and
the enabled observer on port 3. The shared modem must support extended MKISS capacity,
queued protocol v1, signal reporting, configuration readback and source policy.
All modes negotiate the complete 1–255-byte airtime table for forwarding delays.
Keep the modem's configuration owner separate; Willow only reads the PHY.

Create a **new** private directory and config, then supply its absolute path:

```sh
mkdir build/lab-service
chmod 700 build/lab-service
umask 077
cat > build/lab-service/config <<'EOF'
address=127.0.0.1
port=5000
profile=c806643690d003000705020000803f010000
worker=-
password=room
admin=admin
width=3
native_airtime=360
EOF
./build/hew-host "$PWD/build/lab-service"
```

Set `port` to the lab modem's TCP listener. The sample profile encodes
912.525 MHz, BW 250 kHz, SF7, CR5, TX 2 dBm, aggregate factor 1, CAD enabled,
and interference threshold 0. It is a **required readback**, not a request to
change the modem. Set all 18 bytes to the intended lab configuration.
The service sends only CONFIG GET and never claims configuration ownership.
HELLO is opcode `0x20`; CONFIG is `0x22`. Ordinary role delays are unchanged.

## Native MQTT observer

Stage the existing Go observer identity with [the migration helper](MIGRATION.md).
The service requires a private 64-byte scalar/prefix `observer.expanded` or,
when that file is absent, a 32-byte `observer.seed`. It never generates a
replacement when the observer is enabled. Migration selects the authoritative
Go key and preserves its format. For a new laboratory node only,
`./build/roles identity /absolute/private-state/observer.seed` creates a new
identity without replacing an existing file.

Add settings to the private `config` file:

```ini
observer.enabled=1
observer.url=tcp://127.0.0.1:1883
observer.format=capture-v1
observer.iata=YYC
observer.topic_prefix=meshcore
observer.origin=Willow
observer.queue_size=256
observer.packet_filter=65535
```

| Setting | Contract |
| --- | --- |
| `observer.enabled` | `0` by default; `1` reserves port 3 and starts the native publisher. |
| `observer.url` | Required when enabled. `tcp`, `tls`, `ssl`, `ws`, `wss`; explicit TCP/TLS port. Allowed destinations: localhost and `mqtt-meshcore-1.ve6slp.ca`. No embedded credentials, query or fragment. |
| `observer.format` | `internal-v1` default, `observer-v1` or `capture-v1`, matching Go's `mqtt.format`. |
| `observer.iata` | Required three uppercase letters for observer/capture formats. |
| `observer.topic_prefix` | Default `meshcore`; copy Go's `mqtt.topic_prefix` to retain topics. |
| `observer.client_id` | Defaults to `meshcore-observer-` plus the first 12 lowercase public-key hex characters, matching the Go host. |
| `observer.username`, `observer.password` | Optional static credentials; password requires username. Store only in the private configuration file. |
| `observer.audience` | Identity JWT audience; requires observer/capture format and no static credentials. Signs with the retained seed or expanded observer identity. |
| `observer.ca_file` | Optional absolute CA file path. Certificate/hostname verification remains enabled. |
| `observer.origin`, `observer.model`, `observer.firmware_version`, `observer.radio` | Copy the corresponding Go MQTT metadata unchanged. |
| `observer.queue_size` | 1–4096, default 256; plus one pending encoded observation. |
| `observer.packet_filter` | Decimal uint16 packet-kind bit mask. Empty/absent means all kinds, `0` means none. Applies to observer/capture formats, as in Go. |

Observer/capture topics are `PREFIX/IATA/UPPERCASE_PUBLIC_KEY/packets` and
`.../status`; internal topics omit IATA and use the lowercase public key.
Capture/observer formats require RF signal metadata and synchronized UTC.
Local reflections, missing metadata and filtered packet kinds do not publish.
Internal format retains raw frames and local-reflection/decode-error fields.
Only port 3 feeds the observer, so logical-port copies do not duplicate events.
The observer never submits RF packets or subscribes to MQTT commands.

`OBSERVER_CONFIGURED` reports the topic and queue size. `OBSERVER_READY` means
the modem negotiated the observer port and identity; it is not broker delivery.
`OBSERVER_MQTT ... connected=true` means the retained online status received
PUBACK. The phase may already be `packet-ack` when queued RF observations exist.
`connected=false` includes an actionable reason when the broker fails.
Packet publication is QoS1/nonretained: uncertain delivery retries the same
payload after reconnect, so subscribers must tolerate duplicate delivery.
`OBSERVER_STATS` reports final observed/published/dropped/queued/pending counts
on shutdown. `published` increments
only after the matching broker PUBACK, not when a broker first receives bytes.

SIGTERM stops admission, attempts retained offline status and MQTT DISCONNECT
within a bounded shutdown, then closes transports. Broker failure does not
restart the modem or RF roles. Modem reconnect does not restart MQTT. Queued
observations survive publisher replacement, but not process/outbox replacement;
the observer identity survives both through its retained identity file and the
`observer.initialized` identity guard.

Run `make observer-service-test` to drive the release service from an isolated
CAD-on modem through a localhost MQTT broker, interrupt one unacknowledged
publication, reconnect the modem and exercise room login. It writes
`build/observer-service-results.json` and removes its generated identities.

## Bot and role configuration

`worker=-` selects the smaller all-Hew bot. Replace it with the absolute path
to this worktree's `build/native-worker` to use the default Lua bot and any
already provisioned Wasm package.
Default advert names are `Willow-Relay`, `Willow-Room` and `Willow-Bot`.
Set `relay.name`, `room.name`, `bot_name` and `channel_name` in the private
configuration file to choose operator names.
`native_airtime` is that bot's own 360–3600 ms/minute command budget, not the
modem's PHY or aggregate airtime factor. The demo explicitly uses 3600 to run
its command sequence without waiting a minute for budget replenishment.

The example passwords are fixtures. Keep real configuration in the private
file, not command arguments. The directory must be owned by the service user
and mode 0700; config, seeds and state must be private regular files. No TCP
encryption or authentication is added to KISS: use a trusted, isolated link.

Optional fields:

| Field | Meaning |
| --- | --- |
| `channel=HEX32` | 16-byte group key. Hew roles and native bot use it. Empty/absent disables the channel. |
| `channel_name=NAME` | Native channel name, default `Willow`; preserved from NVS during migration. |
| `relay.name`, `room.name`, `bot_name` | Operator advert names, at most 31 printable ASCII bytes. |
| `native_setup=preserve` | Start one worker from existing NVS/SPIFFS without setup commands; apply only the configured bot name before source startup. Default `configure` provisions path/name/channel/airtime with a setup worker. |
| `relay.password`, `room.password`, `relay.admin`, `room.admin` | Per-role credentials, overriding common `password`/`admin`. |
| `relay.retention`, `room.retention` | `native` keeps history volatile; `durable-replay` retains up to 32 posts across restart. Migration preserves the saved profile. |
| `room.public=1` | Explicitly enable an empty room password; a nonempty password and public mode cannot be combined. `room_public=1` is also accepted. |
| `relay.home`, `room.home` | Home key metadata; never an implicit outgoing default. |
| `relay.regions`, `room.regions` | Comma-separated permitted 16-byte keys in hex, at most eight. |
| `relay.default_scope`, `room.default_scope` | Explicit outgoing scope key; empty means no default. |
| `relay.wildcard`, `room.wildcard` | `1` permits unscoped floods; `0` disables wildcard scope selection. Relay forwarding applies admission; room forwarding follows the Go room behavior. |
| `relay.repeat`, `room.repeat` | Enable (`1`) or disable (`0`) forwarding. |
| `relay.flood_max`, `relay.unscoped_max`, `relay.advert_max` | Hop limits, 0–64; equivalent `room.*` fields are available. |
| `relay.txdelay_milli`, `relay.direct_txdelay_milli` | Flood/direct airtime multipliers in thousandths, 0–2000; equivalent `room.*` fields are available. |
| `relay.local_advert_seconds`, `relay.flood_advert_seconds` | Periodic advert intervals; equivalent `room.*` fields are available. Zero disables that timer. |
| `region=HEX32` | Legacy single-key fixture setting for Hew roles; migration uses per-role regions/defaults instead. |
| `bot_home=HEX32` | Register a native-bot home region key. Recognized incoming requests may receive replies in this scope. Does **not** set the default outgoing scope. Native worker required. |
| `bot_default=HEX32` | Native-bot default outgoing scope, including ordinary adverts and replies without a known request scope. Empty/absent means no default. Native worker required. |
| `run_ms=NUMBER` | Stop after this many milliseconds; absent/zero runs until SIGINT/SIGTERM. |

Unknown fields or invalid private state stop startup rather than silently
using another identity. Negative run durations are invalid.

Fresh services use 0.5 flood and 0.2 direct retransmit factors. Migration uses
the saved factors exactly, including a saved 0.3 direct factor. The full
airtime table is negotiated even with `worker=-`; each delay uses
`t=uint32(float32(airtime)*factor)` and a sample in `0..5*t` inclusive.
At capacity, the service replaces the least-active nonadministrator. Existing
ACL logins remain valid and an all-administrator table refuses new identities.

The native bot recognizes at most the two configured region keys. A recognized
request's scope wins; an unscoped flood stays unscoped; otherwise the explicit
default applies. A known direct return path remains direct. Home alone never
scopes ordinary adverts. There is no region tree, editor or key rotation.

## Identity and restart

The modem connection, role workers and native worker owner are separate actors.
A declarative supervisor owns each role/transport branch, with an independent
two-restart/60-second budget and stable ChildRef. A role actor replacement
loads verified committed state, never a new empty role. See
[production supervision](SUPERVISION.md) for the actual ownership boundaries.
A native-worker failure restarts that child without closing healthy relay/room
sessions. A snapshot failure quarantines only that role and suppresses its
uncommitted replies; repair the storage condition and restart Willow to reload
its last committed state. [Library and runtime tests](LIBRARIES.md) describe
the measured failure boundaries, including process-wide native aborts.

An authenticated RF `reboot` restarts only its relay or room after the replay
stamp is committed; it does not restart the host, native bot or modem.
The role reloads its saved preferences and identity, confirms its source policy,
and resets its runtime counters and capture state. See
[role restart](MANAGEMENT.md#role-restart-and-remaining-surfaces).
Owner admin actor recovery restores its committed request clock and receives
the current native-management readiness within one second. An interrupted
command is not replayed; check its setting before submitting another write.

Before a fresh directory has any committed role snapshot, the service creates
`relay.seed`, `room.seed`, and `bot.seed` from OS entropy when absent. Each is a
32-byte Ed25519 seed.
Imported state requires all three existing seeds. The `roles.initialized`
public-identity record also fences accidental fresh initialization on later
starts. Missing/corrupt initialized snapshots quarantine the affected role.
It takes an exclusive
`service.lock` and restores role snapshots before accepting packets.
Use the migration helper rather than copying Go role directories into Willow.
Before returning those identities to Go, stop both writers and use
[the reverse reconciliation command](MIGRATION.md#rollback) to produce a new
Go candidate with final notes, posts, membership and replay state. Restoring
only `rollback-go/` would lose writes accepted during the Willow run.

`ONLINE epoch=N relay=... room=... bot=...` prints public identities after
session negotiation. `BOT_READY native-extension` means the native extension
accepted its identity and started its command source. It does not mean a
companion has heard its advert.

Room ACLs, replay stamps, confirmed cursors and learned paths survive a
process restart. Durable-replay post history survives too; native-retention
history and active sessions do not. Members log in again. Native private notes and installed Lua/Wasm packages survive
extension restart; signed contacts must be heard again after a worker/process
failure. A healthy worker keeps its contacts during modem reconnect. Keys never rotate
on ordinary reconnect. Missing or changed imported seeds fail startup.
Deleting a seed after initialization also fails startup, including older Willow
directories with snapshots but no initialization marker. Restore the original
seed; a partial pending seed is never adopted as its replacement.

After taking its exclusive lock, startup discards abandoned `.pending` files
for role seeds and snapshots (and native scopes when enabled). It deletes only
owned mode-0600 regular files with one link inside an owned directory that is
not writable by other users. Symlinks, hardlinks, directories and unsafe modes
fail startup without touching the committed target. A partial staging file is
never loaded or promoted. An interrupted first initialization with no committed
role snapshot can create the missing seed anew; an existing committed seed
stays unchanged. Malformed committed snapshots quarantine the affected role.

For a fresh `native_setup=configure` directory, configuration is applied over
private child IPC before the active
worker starts. A short-lived setup worker saves path width, bot name, channel
and airtime policy, then exits. Its startup TX requests are **rejected**:
setup never invents a transmission result. The active worker then reads the
saved settings. Imported `native_setup=preserve` skips this setup worker and
keeps saved source, packages, grants, channel/path/airtime policy and data.
A private `HNM1` name file makes a name-only policy update before source startup.
No management socket is exposed. Owner source deployment,
grants and data inspection are outside this service's CLI; the demo's
offline provisioning helper is a synthetic fixture, not a network admin API.
Before either worker starts, the host atomically writes its private `SCP1`
home/default scope file. The worker rejects malformed, nonprivate or symlinked
scope files; this file extends host-worker configuration, not the IPC version.

Stop with Ctrl-C or SIGTERM. The service retires pending jobs, closes both
connections, waits for its specific native child, and releases its lock.
Worker failure retires only its bot jobs as unknown and restarts only that
child, with retries capped at 30 seconds. Relay/room keep their TCP session.
Modem disconnect marks all unfinished transmissions unknown, cancels native
commands/collectors through the existing private `cancel` command, and rejects
offline TX requests. A healthy child is reused when the required PHY and
airtime table still agree; it is replaced if that check or cancellation fails.

Reconnect uses interruptible 0.5, 1, 2, 4, 8, 16, then 30-second waits.
`advert-times` persists relay/room flood scheduling before submission. Reconnect
adverts are zero-hop. Process restart cannot bypass the saved flood interval
(at least 60 seconds for startup adverts); periodic adverts follow the configured
schedule. A crash after reserving an advert may suppress that flood rather than
repeat an uncertain transmission.

## Packets, queues and outcomes

One TCP connection carries all three logical sources. Incoming DATA is held
per port for its following `RX_META`; orphan metadata is dropped. Idle
unpaired data is dispatched as **unmeasured**, never assigned invented RSSI.
The established `SNR=-32 dB, RSSI=127` marker identifies a local reflection.
The native extension can decline replies when real RF metadata is unavailable.

Each read is bounded to 4096 bytes. Role batches admit up to eight RX packets
per role, then run the Hew actors concurrently. `RX_DROPPED` reports excess
batch packets. The native process runs independently. Each logical source
permits 32 pending TX jobs; saturation rejects only that source's new
submissions. These are bounded queues, not lossless buffering.

| Output | Meaning |
| --- | --- |
| `SUBMITTED ... kind=... route=... at_ms=...` | The request was written to TCP; no RF result yet. Header fields distinguish a PATH reply (kind 8) from an advert (4) or ANON_REQ (7). `at_ms` is Unix milliseconds. |
| `ROLE_RX ... kind=7\|8 ...` | A parsed ANON_REQ/PATH packet reached an available role actor, before authentication or state commit. `origin` is 0 for measured RF, 1 for reflection or 2 when metadata is missing. This is not a login-success record. No decrypted payload is logged. |
| `TX ... state=1` | Modem queue accepted the job. |
| `TX ... state=2` | Modem confirmed transmission; remote delivery still requires its protocol ACK. |
| `TX ... state=3` | Modem reported transmission failure. |
| `TX ... state=4` | Outcome is unknown, including disconnect while pending. |
| `REJECTED ...` | Source capacity, packet or connection did not permit submission. |
| `MODEM_REQUEST_FAILED stage=... port=...` | A control write/read failed, the modem returned a hardware error, or its expected response opcode did not arrive. `timeout` and `shutdown` are reported only after waiting stops, not when unrelated metadata arrives. `actual` is the last observed control payload and can be RX metadata rather than the requested response. |
| `MODEM_READ_END ... reason=eof` | The modem TCP stream ended. This is distinct from an idle read. |
| `MODEM_READ_FAILED ...` | The socket read failed, with its errno or stopped-owner condition retained. |
| `MODEM_CONTROL_FAILED epoch=... stage=...` | A periodic control write failed, CONFIG readback was invalid, or its two-second response deadline expired. Write failures include the committed byte prefix; invalid readback includes expected profile and received control bytes. |
| `MODEM_SESSION_FAILED stage=...` | TX submission, confirmation bookkeeping, source generation or outcome tracking retired the session. At most one such diagnostic is emitted per session; uncertain outcomes remain uncertain. |
| `NEGOTIATION_REJECTED stage=... expected=... actual=...` | The reply failed the named handshake contract. CONFIG reports the required profile and actual bytes; CAPACITY identifies a missing aggregate session grant. |
| `OFFLINE ... no retune` | The connection is retired. Read the preceding stage-specific diagnostic before changing configuration. |

Modem failure and epoch-retirement records include `at_ms` in Unix milliseconds.
A CONFIG deadline also reports `overdue_ms`, the delay past its existing
two-second limit when the loop checked it. Neither TCP options nor deadlines
are changed by these diagnostics. Run `make modem-diagnostics-test` to exercise
EOF, reset errno, missing/truncated/mismatched CONFIG replies and TX failure
classification over loopback in debug and release.

`SET_SIGNAL_REPORT` (`0x19`) is acknowledged by `GET_SIGNAL_REPORT` response
`0x9a`, with byte `01` when enabled. A `CAPACITY` session grant of zero means
another connection owns the sole aggregate session: use `radio_session=per_role`
for the retained Go observer/Base service. Willow never displaces that session
or changes the shared PHY to resolve a negotiation failure.

Jobs correlate by port, source generation and job ID. Old-generation and
duplicate-final events are ignored. Uncertain writes and disconnects are
never replayed automatically. CONFIG is checked every two seconds; drift or
timeout closes the session and stops further admission. The next connection
must negotiate again. Role-presence overlap reports are advisory warnings.

The service polls modem PHY statistics for native `!air`. The emulator uses
explicit synthetic measurements and airtime values; its terminal timers
honor requested submission delays. None of those measurements describes RF.

## Scope and validation

`make firmware-negotiation-test` builds the existing C++ shared-modem firmware
harness against the bound native dependencies and runs Willow against its
loopback TCP endpoint in debug and release. It checks all three signal-report
acknowledgements with RX metadata interleaved before each reply, the complete
airtime table, coexistence with two independent
observer/Base connections, a busy aggregate session, and mismatched-profile
diagnostics. Captured requests contain no CONFIG SET or configuration-owner
claim from Willow. The harness uses its own synthetic PHY, never a live radio.
This target also runs as part of `make test`.

`make test` covers the all-Hew core, pinned wire/policy fixtures, and actual
Go room/relay comparisons. `make demo` covers ten socket scenarios and a
five-command Go/native differential using the **same serialized requests**:
ping, calculation, note write/read and Wasm arithmetic.
Another five-request differential checks known home, unscoped flood, unknown
scope fallback, direct-request fallback and known default. The Go oracle also
checks `policy.ChooseReplyScope` and transport codes independently. Startup
adverts use the default key, and a home-only restart keeps them unscoped.

Further socket checks include group authentication and private-note
permission, tampered DM rejection, canonical retry suppression, delayed
multi-path collection, room automatic delivery/ACK/keepalive, per-role queue
saturation, burst dropping, restart persistence, malformed negotiation,
disconnect uncertainty, stale results and profile drift.

Use [RF management](MANAGEMENT.md) for relay/room status, telemetry,
authenticated commands, ACLs, discovery and saved role-local preferences.
There is no dashboard, serial transport, companion management API or full
RF routing-policy editor.
Birch state migration supports the contracts and refusals listed in
[the migration guide](MIGRATION.md). Relay `loop=0/1/2/3` selects
off/minimal/moderate/strict admission; migration preserves the saved value.
Hop limits, scopes, retransmit delays and advert schedules are configurable.
Native Lua/Wasm commands retain the existing extension's resource limits and
permissions. The native invocation wall budget remains 20 ms in
`firmware/runtime/BotTypes.h`; `BotVm.cpp` emits
`Command wall deadline exceeded during invocation`. Go and Hew now start the
same verified worker, so that limit and error path are shared. A previous run
using the old installed binary observed the error under load; this does not
establish a deterministic Go reproduction. The service neither extends the
budget nor retries potentially effectful commands to conceal errors.

## Worker build and verification

```sh
make native-worker
python3 -B build_worker.py --verify --worker "$PWD/build/native-worker"
python3 -c 'import json; s=json.load(open("build/native-worker.json")); print(s["source_fingerprint"]); print(s["worker_sha256"])'
make all
```

The first build fetches public pinned dependencies into `build/native/`:
MeshCore `d92964352441e53b93e8667b802e04f6e072b39e`, Lua 5.5.1,
WAMR `b124f70345d712bead5c0c2393acb2dc583511de`, Crypto 0.4.0 and
CayenneLPP 1.6.1. It uses the existing Lua parser hooks, WAMR metering patches,
and `firmware/esp32/bot.mk` worker target. Host-only staging does not create
device profiles or weaken firmware build-directory guards.

`build/native-worker.json` records the base commit, source fingerprint,
dependency archive hashes, compiler versions/commands, worker hash and hashed
input inventory. `build/native-worker.inputs` binds the project/dependency
inputs, worker and linked system libraries. The generated `worker_identity.hew`
binds that manifest's digest and exact worker path into `hew-host`.
The service checks the binding before connecting and before each worker launch.
The demo and Go reference also verify the build record. Missing, changed or
external inputs fail with a rebuild instruction; there is no installed-worker
fallback. Rebuild `hew-host` after rebuilding the worker.

This binds a local build to its recorded inputs, not a signed release or a
reproducible-build claim. Keep the worktree/build tree available and unchanged
while running the service; do not rebuild it underneath a running host.

## Hew runtime boundary

The durable-history regression exposed malformed boolean interpolation in
rc7's O2 build of persistent `Role.status`: output extended beyond the boolean
text. Role status/cursor flags now select explicit `"true"`/`"false"` strings.
The same restart/history tests run against debug and release executables.
A narrow two-flag actor did not reproduce this failure; this is a tested local
workaround, not a general diagnosis of the compiler.

`service.hew` owns orchestration, `session.hew` owns negotiation/framing/job
correlation, and `roles.hew` owns the Hew state machines. `transport_bridge.c`
contains POSIX connect/poll/write, signal/clock, directory and shell-free
`posix_spawn` operations; it contains no role implementation.
Socket readiness and role, owner-control and observer notifications enter bounded
Hew streams. Drivers wait on those streams and active protocol deadlines rather
than repeatedly asking idle actors. The host shares one native wait pump across
its independently owned driver groups. [Readiness and timer ownership](LIBRARIES.md#readiness-and-timer-ownership)
describes registration cleanup, shutdown and the loopback checks.

Rc7's `std.process.Child` does not expose piped stdin/stdout. Run
`hew check tests/process_pipe_probe.hew` to reproduce the expected missing
`Child.stdin` diagnostic. `std.net` receive error behavior also prevented
recoverable transport handling in the initial probe. Status-returning
offloaded FFI avoids trapping on ordinary disconnect and bounds writes to
1.5 seconds. Borrowed FFI bytes are copied before return; child descriptors
and PIDs are owned by the service, including a child retained across modem epochs.

## Standalone local MQTT broker

`make broker` builds the standalone Hew broker without rebuilding host roles.
Its private configuration, ephemeral-port launch, protocol behavior and
validation commands are in [BROKER.md](BROKER.md). MQTT 3.1, 3.1.1 and 5,
shared subscriptions and system topics run through the actual Go broker's
differential lifecycle. Read the inherited quota and expiry behaviors before
handover. Broker memory contains no MeshCore identities; restart loses its
retained messages and sessions.
