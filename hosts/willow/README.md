# MeshCore Willow

Run a relay, room and Lua/Wasm command bot on a Linux computer connected to a
WiFi shared modem. Add a Base companion for the MeshCore app, a browser
dashboard and an MQTT broker. An optional observer publishes received radio
packets to your broker.

Download the [Willow 0.0.11 Linux bundle](https://github.com/slepp/meshcore-aspen/releases/tag/willow-v0.0.11)
to install without a compiler. It includes all four services, the program
worker, dashboard page, migration tools and source. Start below, or
[build from source](#build-from-source).

For Docker, use `ghcr.io/slepp/meshcore-willow:0.0.11` with the
[container setup and Compose example](docs/CONTAINERS.md). The same image runs
the host, Base, dashboard or broker with private persistent state.

## Install the Linux bundle

Install the runtime libraries and Python helpers. On Debian 12:

```sh
sudo apt-get install python3 python3-cryptography python3-nacl \
  libssl3 libcjson1 libsodium23 libstdc++6 zlib1g ca-certificates
```

Download `willow-v0.0.11-linux-x86_64.tar.gz` from the release above, extract it,
and select a new versioned installation directory:

```sh
tar -xzf willow-v0.0.11-linux-x86_64.tar.gz
cd willow-v0.0.11-linux-x86_64
python3 install-willow.py --prefix "$HOME/.local/lib/willow/0.0.11"
cd "$HOME/.local/lib/willow/0.0.11/hosts/willow"
```

The installer checks bundle files and never replaces an existing installation
or changes private node state. You can move a complete binary installation to
another prefix; its host verifies the bundled worker before use.
The bundle includes libcurl with HTTP, HTTPS, WS and WSS support and uses the
system's trusted CA certificates for TLS connections.

For a new node, set the modem's IPv4 address and the hexadecimal profile from
[the initialization guide](docs/SERVICE.md#start-with-new-identities), then run:

```sh
./build/hew-host-release init "$HOME/.local/state/willow" \
  --address "$MODEM_IPV4" --port 8001 --profile "$PROFILE_HEX"
python3 -B tools/willow.py check --state "$HOME/.local/state/willow"
python3 -B tools/willow.py run --release --state "$HOME/.local/state/willow"
```

The check reads configuration and verifies the worker without connecting to the
modem. Startup reports `ONLINE` and `BOT_READY native-extension`. The modem owns
radio tuning; Willow checks its profile without retuning it. Use a short private
state path so its Unix socket names fit the operating system's limit.

For an existing node, use [stopped-state migration and rollback](docs/MIGRATION.md)
instead of `init`. Never start two services with the same identity. The
[service updater](../../tools/INSTALL_WILLOW.md) accepts an extracted binary
bundle as its source, so an upgrade can retain the latest state without a compiler.

Run Base, the dashboard and the MQTT broker separately using their guides in
[docs/BASE.md](docs/BASE.md), [docs/SERVICE.md](docs/SERVICE.md#native-http-dashboard)
and [docs/BROKER.md](docs/BROKER.md). Their executables are under `build/`.

## Build from source

```sh
git clone https://github.com/slepp/meshcore-aspen.git
cd meshcore-aspen
make -j2 willow-check
cd hosts/willow
python3 -B tools/willow.py --version
```

Expected: `Willow 0.0.11`. Use the new-node initialization, check and run commands
above, or migrate an existing stopped node. From the repository root, `make
willow-build` builds the host, Base, broker and dashboard without running the
checks. The component's `make release` and `make check` targets remain available.
The launcher defaults to four scheduler workers; set
`HEW_WORKERS` explicitly to choose another count.
For production builds, build Hew with its supported `make hew` target and put
its `build/bin` directory on `PATH`. This selects matching optimized runtime
libraries. `hew build --release` optimizes the application; a compiler launched
from `target/debug` still uses that toolchain's debug runtime libraries.
For a new installation, [create private state](docs/SERVICE.md#start-with-new-identities)
with `build/hew-host-release init`, then use the state check and launcher above.
`make willow-check` runs the Hew applications against private loopback modem emulators,
checks restart and native-worker recovery, exercises interrupted persistence,
and compares protocol behaviour with Birch. Missing prerequisites fail the
command. The selected compiler, runtime and standard-library hashes are
recorded in ignored `build/toolchain.json`; the compiler version is printed.
The contributor CI runs this same command. The broader `make test test-release`
targets retain the complete debug/release protocol, management, login,
readiness and migration suites.

A compatible toolchain is Hew source
`dce0bc72da487bf4c9307df84b738c2caa58b8f7`, with its matching optimized
runtime and standard library. Normal builds select the current `hew` from
`PATH`. For a Hew source checkout, run `make hew` and select its `build/bin`
launcher. The published rc7 SDK predates Willow's actor syntax.
See [building a Linux bundle](docs/BINARY_RELEASE.md) for the Debian 12 build
and the included source, notices and relinking material.
Compiler-specific probe results are documented separately in
[tests/probes](tests/probes/README.md).

Production modules live in `src/`, native bridges in `native/`, launch and
build helpers in `tools/`, and checks in `tests/`. Internal Hew imports use
`willow.src` so applications and checks resolve the same modules.

Use the running host's [owner-only DM and inbox commands](docs/SERVICE.md#send-a-dm-and-read-the-owners-inbox)
to send as its native bot and read authenticated incoming DMs. For a version
upgrade, [rebind a complete frozen state copy](docs/MIGRATION.md#upgrade-a-frozen-willow-state)
with the newly installed launcher; do not replace a running installation.
Existing Linux user services can use the
[service updater](../../tools/INSTALL_WILLOW.md) for selected-service upgrades
and rollback using the latest stopped state.

For the browser dashboard, follow [the host dashboard setup](docs/SERVICE.md#run-the-host-dashboard).
It defaults to `http://127.0.0.1:9080/`; set `address=0.0.0.0` in its private
configuration for LAN access at `http://HOST_LAN_ADDRESS:9080/`. `/admin` opens
the same dashboard. View live role and repeater signal status without logging
in; connect under Administration to change settings or manage programs.
`/status` shows public state and `/readyz` reports readiness. Restarting only the
dashboard leaves the host roles, identities and companion listeners running.

To track repeater SNR and RSSI, [install the packet sampler and commands](docs/PACKET_GUESTS.md).
The bot replies, dashboard and signal metrics share its report; saved programs
and settings remain after restart, while signal sample rings start empty.
Signal rows combine matching first-byte prefixes by default. Expand a row to
see individual prefixes; means are weighted by sample count and grouped P95
shows the range of the individual P95 values.

For an isolated modem-to-broker trial, run `make observer-service-test`. It uses
localhost only and checks broker reconnect, modem reconnect and room login in
the production process. Other development checks are:

```sh
cd hosts/willow
hew --version
make native-worker
export BOT_NATIVE_WORKER="$PWD/build/native-worker"
make demo
make test
make migration-test
make readiness-test
make library-test
make supervision-test
make management-test
make native-login-test
./build/roles selftest
```

Requirements: Linux x86-64, the current `hew` compiler and libraries on `PATH`,
a C compiler, C++17 compiler and OpenSSL development headers, libcurl 7.86 or
newer development headers/library, `pkg-config`, `libcrypto.so.3`,
`libsodium.so.23`, Go matching the root `go.mod`, and
Python with `cryptography` and `PyNaCl`, CMake, Git, patch, PlatformIO's package
manager and cJSON headers/library. The full demo additionally needs Clang's
wasm32 target and `wasm-ld`. `BOT_JSON_INCLUDE` can select the cJSON header directory;
the default is the installed ESP32 SDK.
MQTT over WebSocket also needs libcurl built with `ws`/`wss` support.
The first native worker build downloads
pinned public C/C++ dependencies using the existing host-worker recipe.
Build products and generated test identities stay in ignored
`build/`.

`make broker-test` also needs `mosquitto_pub` and `mosquitto_sub` from
`mosquitto-clients` for the broker's standard-client checks.

`make demo` runs ten TCP service scenarios and then compares the bot's
serialized RF inputs and replies with the actual Go/native host. Results are
written to `build/service-results.json`. See [running the service](docs/SERVICE.md)
for configuration, startup output, queue limits and recovery behavior.

## Try a persistent role

For the fixture interface, create a **new synthetic identity**:

```sh
mkdir -p build/lab
chmod 700 build/lab
./build/roles identity build/lab/room.seed
./build/roles serve 3 @build/lab/room.seed 'Hew Room' room admin 3 - -
```

The program prints `READY PUBLIC_KEY`. Type:

```text
state build/lab/room.state
advert 1700000000
status
close
```

`state` obtains an exclusive lock and restores existing state, or selects a new
state file. Subsequent packet/ACL/tick transactions sync the replacement file
and its directory **before printing replies**. A failed or uncertain commit
stops that role and suppresses those replies. `close` stops new work; EOF shuts
down the process.

The example passwords are synthetic fixture values. This command-line harness
is not a credential-safe deployment interface: arguments can appear in the
process list. Do not supply real room/admin passwords or production keys.
Identity creation refuses an existing file. Identity and state files are mode
0600; keep their parent directory private. Losing the seed loses the role's
identity.

Role IDs are `1` bot/chat, `2` repeater, and `3` room. Width is `1`, `2`, or `3`
bytes per ordinary path hop. The last arguments are a 16-byte region key and a
16-byte group-channel secret, in hex, or `-` for disabled. Configuration is
supplied on each start. Committed RF changes override the supported role-local
settings; modem profile, native worker and scope configuration remain separate.
See [RF management](docs/MANAGEMENT.md) for commands and persistence behavior.

See [the fixture interface](docs/PROTOCOL.md) for authenticated input packets,
KISS stream fragments, queued-modem submissions, and time advancement.

## What runs in Hew

| Area | Implemented and checked | Bounded differences / exclusions |
| --- | --- | --- |
| Identity and crypto | Persistent Ed25519 seed identities; signed adverts; Ed25519-to-X25519 shared secrets; MeshCore AES-128-ECB with zero padding and two-byte HMAC; ACK proofs; group-channel authentication; matching-seed Birch identity migration | Expanded-only active identities without a matching seed require further implementation. Group authentication proves channel-secret possession, not a unique sender. |
| Frames | MeshCore payload version 0; all four route encodings; ordinary widths 1/2/3, 64-byte path and 184-byte payload limits; TRACE widths 1/2/4/8 with distinct SNR return path; KISS escaping, fragmented receive, malformed-frame recovery; per-port DATA/RX_META pairing and local-reflection classification; CONTROL discovery | TRACE originates/terminates at the retained companion Base; Willow forwards its route and return hops. |
| Repeater | Flood append, direct next-hop removal, packet dedup, local-reflection suppression, off/minimal/moderate/strict loop policies, configurable hop limits and airtime-based randomized delays, up to eight permitted region keys, independent home/default scope; configurable room forwarding and source airtime factor | 160 dedup hashes; up to 64 packets waiting for measured-SNR receive delay. Use the [owner region controls](docs/SERVICE.md#configure-a-roles-named-regions) for local region changes, not an RF region editor. |
| Room login | Authenticated password/admin/ACL login, explicit public-room opt-in, read-only guest mode, permission bits, replay rejection, flood PATH reply, direct/flood response routing, learned paths and multi-ACK policy; full service tables evict the least-active nonadministrator | 20 members; all-administrator tables refuse new identities. |
| Room delivery | Automatic service ticks; post retry ACK without duplicate history, 150-byte new-post retention, 32-post history, own-author suppression, six-second delay, round-robin pushes, recipient-key delivery ACK, learned paths, three-failure suppression, keepalive proof/count; migrated durable history and subsequent posts survive restart | Existing Go posts up to 151 bytes, including opaque RawText, are preserved. |
| RF management | Authenticated CLI, durable role settings and ACLs; binary status/telemetry/ACL; relay neighbour/owner and anonymous queries; measured CONTROL discovery; optional modem sampling | [Exact commands and remaining surfaces](docs/MANAGEMENT.md). Saved coordinates, role RTC offset, packet-log controls and role restart are supported. No RF named-region editor or remote identity import. |
| Default bot extension | Signed contact discovery; authenticated DM/group commands; default Lua commands and private KV; canonical retry dedup; delayed multi-path collection; native RF outcomes; independently persisted Wasm package alongside Lua | Existing C++ runtime, not reimplemented in Hew. Verified commands include ping, calc, remember, recall, mt, air, and Wasm wadd. Native owner administration and package operations use the [service controls](docs/SERVICE.md); inherited commands are not all exercised here. |
| Native bot regions | Separate home/default keys; known request scope, unscoped flood preservation, explicit fallback, default-scoped adverts and home-only unscoped adverts; serialized Go/native differential plus independent Go policy/code checks | Two registered bot keys, changed through local owner administration. Relay/room use their own permitted keys and explicit defaults. |
| Worker build binding | Actual worktree native worker built with pinned MeshCore/Lua/WAMR inputs; recorded source fingerprint, compiler commands and input/binary hashes; compiled-in manifest verification before startup/restart | Local build binding, not a signed/reproducible release. Arbitrary installed workers are refused; source/build tree must remain available. |
| All-Hew bot | Signed discovery; DM/group authentication; public/private gates; two-minute request dedup; targeted commands; private notes; deferred mt | Smaller command subset below; no Lua/Wasm or advanced native APIs in this mode. |
| Async/lifetime | Three role actors run bounded batches concurrently; eight RX packets per role per batch; independent native extension process; automatic ticks; bounded mailboxes and 32 outstanding TX jobs per port; worker faults leave relay/room connected | Disconnect cancels native collectors and marks pending TX unknown. A healthy worker retains contacts across modem reconnect; after worker restart, the native bot can recover signed contacts from its configured retained Base authority. Missing/invalid contacts require a fresh advert. The all-Hew bot has eight deferred jobs. |
| Supervision | Independent declarative role, socket-owner and native-process-owner branches; stable ChildRefs; two actor restarts per 60 seconds; committed-state reconstruction and role-local quarantine | Coordinator, epoch session/decoders and metadata actors are outside these budgets. Native hardware faults in the Hew process remain fatal; the native worker stays in a subprocess. |
| Shared modem | TCP/MKISS HELLO, extended CAPACITY, read-only CONFIG, SOURCE_POLICY, ROLE_PRESENCE warning, signal reporting, full airtime table, PHY statistics; correlated queued TX; reconnect and periodic profile fencing | Numeric IPv4 TCP only; no serial, TLS or modem configuration ownership. Three logical ports, or four with observer enabled. Required profile readback never retunes. Presence is advisory. Base uses its own ordinary connection rather than an aggregate port. |
| Base companion | Separate native process, retained identity, protocol-13 TCP endpoint, contacts, channels, message cursors, RF traffic and read-only health | Protocol coverage is partial; select destructive and key-management operations explicitly. See [Base setup and contracts](docs/BASE.md). |
| Owner and dashboard | Private owner/admin sockets; native-bot DM send/inbox, role commands and local region controls; six-role status and combined readiness | Owner commands retain authentication and permission checks. Dashboard monitoring does not own or retune the modem; see [service controls](docs/SERVICE.md). |
| MQTT observer | Port-3 RF observations; independently supervised outbox and publisher; Go-compatible topics, payloads and filtering; retained identity migration; QoS1 replay across broker reconnect; verified TLS/WebSockets and optional identity JWT | Configure your broker hostname or IP with `observer.url`. Output-only; no RF TX or MQTT commands. Bounded queue with batched durable checkpoints and a controlled-stop flush. Crash recovery starts at the last completed checkpoint. |
| Persistence | Private identities; atomic state replacement; directory sync; lock exclusion; persisted ACL/replay/cursors/paths/counters/notes; optional durable history; stopped role on commit failure; offline Birch migration and [final-state reconciliation into a new Go candidate](docs/MIGRATION.md#rollback) | Active sessions/pending ACKs/dedup/deferred calls are cleared. Native-retention history remains volatile; durable-replay history persists. Unsupported migration data is refused explicitly. No encryption at rest or guaranteed private-buffer zeroization in Hew. |

The supported bot subset is `!ping`, `!about`, `!path`, `!mt [1..30]`,
`!uptime`, `!help`, `!remember KEY TEXT`, `!recall KEY`, `!forget KEY`,
`!notes`, and the bounded `!admin` diagnostic. `!@KEY8 COMMAND` and
`!@FULLKEY COMMAND` select this bot. Names/targets currently use lowercase.
`!help`, `!uptime`, and `!admin` responses describe this harness, not the full
firmware runtime. Notes are private to a DM sender, limited to 64 entries,
32-byte keys and 120-byte values; note listing uses insertion order.
The Hew bot skips repeated ASCII spaces before a note key and its text, while
preserving internal text spacing. Missing keys/text and oversized arguments
return errors without changing notes. Note lengths are serialized in bytes;
every snapshot checks the note-size invariant before writing.

Use the default native worker for the existing Lua/Wasm bot behavior.
`worker=-` selects the smaller all-Hew command set described above.

## Validation

`make firmware-negotiation-test` runs debug/release Willow against the actual
shared-modem C++ firmware on loopback, including signal-report response opcodes,
the full airtime table, two independent compatibility connections, a busy
aggregate session and profile-mismatch diagnostics. `make test` includes it.

`make library-test` runs twelve debug/release groups for reusable byte/framing,
owned network/process APIs, deadlines and partial writes, argument fidelity,
resource cleanup, actual panic/supervision behavior and unaffected-role fault
isolation. See [library contracts and Hew runtime feedback](docs/LIBRARIES.md).

`make supervision-test` runs actual production owner/role panics, durable-state
reconstruction, independent budget exhaustion, missing/corrupt-state quarantine
and modem/native-child replacement in debug and release. See
[production supervision and remaining RF surfaces](docs/SUPERVISION.md).

`make migration-test` runs eleven groups using a snapshot generated by the actual
Go role store and committed native Lua/Wasm records. It checks dry-run and file
hashes, identity authority, ACL/replay/cursors/paths, source-lock refusal,
disjoint destinations, first-start integrity, preserved private notes/Wasm,
name-only NVS changes, scoped send/receive through an emulated modem, and
role-local quarantine after an initialized import loses its room snapshot.
The current-state fixture contains eight room members/twelve posts and four
relay administrators, with deliberately stale `state.v1.json` copies. Eleven
history deliveries are compared with actual serialized Go output, including
own-author suppression, raw bytes, 151-byte text, delivery ACKs and restart.
Saved attempt bytes and all four loop policies survive migration. Schema-only
inspection needs no runtime credentials and cannot stage a service; explicit
archive retention preserves large inactive backups separately from active roles.

`make readiness-test` runs six groups covering workerless airtime/jitter,
measured-SNR TRACE forwarding and counter bounds, explicit room admission and
administrator-safe eviction, persisted reconnect advert pacing, worker-only
failure recovery, old-job fencing, and contact-preserving reconnect with
collector cancellation.

`make test` runs:

* 20 Python test groups, driving the **compiled Hew executable**, including
  adversarial authentication/replay cases, room delivery, persistence/restart,
  native deferred bot completion/cancellation, queue exhaustion and framing.
* Compiled native session checks for fragmented/invalid worker IPC, signal
  metadata deadlines, physical-counter field ordering, failed-versus-unknown
  RF flags, failed socket writes and exactly-once terminal retirement.
* Worker-binding checks for manifest changes, changed inputs and rejection of
  an external worker before connection or identity creation.
* Three storage-recovery groups: SIGKILL after a real partial staging write for
  a first identity and an existing snapshot, live-lock contention, and rejection
  of unsafe staging objects. Fault injection is compiled only into the
  `roles-interrupt` test executable, never the normal service.
* A Go-generated oracle with 167 encryption vectors, 476 valid frame vectors,
  and 136 region/routing policy vectors.
* Existing `testdata/parity/native-events.jsonl`: all 256 ordinary path bytes,
  region transport code, encrypted datagram and signed/unsigned ACK fixtures.
* `TestHewRoomStateMachineDifferential`: identical encrypted login/post/retry/
  path/keepalive inputs to the actual Go room service and the Hew process.
  It compares responses and state, normalizing only login time/random fields.
* `TestHewRepeaterSerializedDifferential`: 99 actual Go/Hew cases:
  18 forwarding/duplicate/reflection/next-hop/loop cases across widths 1/2/3
  with `#ab` configured and unscoped flood admitted, 20 TRACE cases across
  widths 1/2/4/8, a scoped forward, and 60 loop-admission cases spanning all
  four policies, widths 1/2/3 and zero through four own-key occurrences.

`make demo` additionally checks real TCP sessions, metadata, bounded RX/TX
queues, room automatic delivery/ACK/keepalive, full-process identity and
replay persistence, native Lua/Wasm, authenticated group permissions, invalid
MAC rejection, canonical retry dedup, deferred multi-path collection, PHY
statistics, disconnect uncertainty, stale events and drift. Five identical
encrypted command inputs are replayed through the actual Go/native host;
comparison preserves headers, paths, addressing, flags and reply text,
normalizing only the reply's wall-clock timestamp.
Five additional scoped requests compare the same native worker under Go and
Hew, with Go scope selection and transport-code checks independent of the
worker. The emulator checks default-scoped adverts and home-only separation.

The built-in concurrency test runs two native producer actors, each submitting
500 distinct packets to one relay, checks bounded-channel rejection and
32-job modem saturation, then checks shutdown admission. The Python suite
repeats it five times.

Additional checks (compiler/API examples are in [tests/probes](tests/probes/README.md)):

```sh
hew check tests/main.hew
hew check src/service_main.hew
make test-release
TMPDIR="$PWD/build" MESHCORE_HEW_BIN="$PWD/build/roles" \
  go test -race ../../internal/roles ../../internal/policy -count=1
```

Go is a **test oracle only**, never started by the service. Crypto FFI uses
OpenSSL/libsodium; filesystem FFI supplies POSIX read/lock/sync/rename.
Transport FFI supplies individual socket/process syscalls; `src/hostlib` owns
framing, deadlines, retry policy and actor/resource lifetimes in Hew.
The relay/room and the optional smaller bot are Hew state machines.
The default bot is explicitly the existing native extension, with its own
protocol, authentication, Lua/Wasm interpreters and persistence.

## Developer notes

Start with `src/wire.hew` for wire formats, `src/roles.hew` for isolated state machines,
`src/modem.hew` for queued-job outcomes, and `tests/main.hew` for the explicit fixture
clock and protocol. `src/service.hew` and `src/session.hew` implement the running
host. `tests/oracle/main.go` imports the real pinned Go implementation.
The opt-in Go room differential test lives in
`internal/roles/hew_parity_test.go`.
The bot differential uses `internal/app/hew_parity_test.go`; its transcript
contains only generated synthetic identities and fixtures under `build/`.
The `tests/*.go.in` files are Go test overlays used by the Python differential
checks, not standalone Go packages.

Build with the current **Hew compiler and matching libraries** on `PATH`.
`HEW=/path/to/hew` selects a different installed compiler. Builds record module
dependencies and track the selected compiler, standard library and runtime
archives, so ordinary `make release` follows compiler and library upgrades.
Language references:

* [Actor ownership, tasks, mailboxes and backpressure](https://hew.sh/docs/actors/)
* [Standard library and file/channel lifetimes](https://hew.sh/docs/stdlib/)
* [Language specification and explicit FFI](https://hew.sh/docs/language-spec/)
* [Language tour](https://hew.sh/docs/language-tour/)

The FFI boundary borrows `bytes` inputs as pointers to
`{ptr, offset:u32, len:u32}`. Outputs use `hew_bytes_new` and transfer ownership
to Hew. No foreign pointer is retained after a call. POSIX read/commit and
blocking event waits use `#[offload]`; socket operations are nonblocking.
`wire.cut` uses checked indexing for byte slices.

### Protocol dependencies

* Go wire library: `github.com/meshcore-go/meshcore-go v1.5.0`
* Native fixture revision: `81dbae592ffad40b4212bc0b37e82b38da08f4bc`

The authoritative host roles are `internal/roles` and `internal/policy`.
The host bot executes the native C++ runtime through
`internal/nativebot/host`; `cmd/meshcore-bot-service` is its optional HTTPS RPC
sidecar, not the radio bot. `worker=-` selects the Hew bot subset; the absolute
verified `build/native-worker` path selects the shared native extension.
