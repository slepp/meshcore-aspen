# Willow host libraries and Hew runtime contracts

These adapters use the current **Hew compiler and matching libraries** on `PATH`. The checks below exercise the
transport, ownership and failure contracts used by Willow.

Use `hostlib` for bounded framing, owned byte streams and shell-free duplex
children. The modem owns one connection actor; relay, room and bot work runs
in separate actors. The native Lua/Wasm worker has a separate process and
an actor owning its channel and PID.

```sh
cd hosts/willow
make library-test
```

This runs twelve host-library groups and management-codec checks in debug and release, including actual socket
backpressure, child retirement, actor panic and service fault injection.
Fixtures use private loopback connections and generated identities only.

## Typed contracts

| Module | Contract | Bounds and ownership |
| --- | --- | --- |
| `hostlib.binary` | Checked byte slicing, little-endian scalars and finite binary32 conversion return `Option`. | Scalar widths 0–8 bytes; float32 includes signed zero and subnormals, rejects NaN/infinity/overflow. Returned bytes own their data. |
| `hostlib.framing` | Length-prefix and SLIP encoders/decoders return typed results and explicit residual state. | Frames at most 4096 bytes; feed chunks at most 4096. A malformed length retires the stream; malformed or oversized SLIP records discard through the next END and resume. No device or role dependencies. |
| `hostlib.duplex` | `Read` distinguishes data, idle, EOF and failure. Writes return a count or `WriteFailure` with the committed prefix. | One resource-owning TCP actor, mailbox 32; reads at most 4096 bytes, writes at most 65536. Read/connect deadlines 0–30000 ms; configured write deadlines 1–30000 ms, default 1500. Numeric IPv4. |
| `hostlib.process` | `start(absolute_path, Vec<string>)` returns an owned child actor; exit status distinguishes exit code, signal and an already-reaped child. | No shell; 32 arguments, 16384 encoded bytes, NUL rejected. Empty arguments, spaces and Unicode are retained. Channel and PID have one resource owner; mailbox 32. |
| `hostlib.supervised_io` | Stable supervised TCP endpoint; restart retires the socket without an implicit redial. | Independent two-restart/60-second budget; the caller must fence and negotiate another protocol epoch. |
| `hostlib.events` | Socket readiness and explicit notifications enter bounded Hew streams; `select` waits until an absolute monotonic deadline. | Reference-counted Linux epoll/eventfd groups, up to 1024 registrations per group; one-shot read/write interests and nonreused registration keys. Watches own duplicate descriptors until removal. |
| `hostlib.deadline` | Distinct UTC and monotonic millisecond values with an explicit UTC-to-monotonic conversion. | Zero monotonic deadline is inactive; a deadline is reached at its exact boundary. Past UTC deadlines convert to the current monotonic time. |
| `meshproto.management` | Pure typed status/LPP/neighbour encoders, native numeric formatting, UTC and UTF-8 limits. | 50 neighbour entries, 130 bytes of page entries; no packet authentication or I/O hidden in the codecs. |
| `meshproto.preferences` | Validate/change/encode/decode complete role-local preference values. | At most 330 encoded bytes; no mutation on decode failure; binary32 delays; bounded names, credentials and owner text. |
| `meshproto.owner_state` | Checked owner phase, flag and reason codecs retain the v1 ledger values. | Phases 0–6, flags 0–7 and reasons 0–8; failed and uncertain sends remain fenced against late events. A local TX confirmation is distinct from a native recipient RF ACK. |
| `meshproto.radio` | Pure optional-request state machine with typed timeout/malformed/rejected failures. | One outstanding read, two-second deadline, 45-second cache; uncorrelated failure pauses sampling until a new connection. |
| `meshproto.cli` | Native RF text and measurement formatting from typed values. | Explicit unavailable readings and bounded command replies; authentication and commits remain in the role actor. |

`Role` keeps one `Preferences` authority and plain-data RX, delivery, scope,
discovery and diagnostic records in the same actor. HEW2–HEW6 snapshots retain
their existing layouts and ownership. Session port records bind each job ID and
pending source-policy readback to its negotiated generation; ordinary Base
sources have one port, while the host negotiates three or four.
Connection epochs, pending writes and cached measurements use plain-data records.
Socket watches, stream readers, child channels and actor references keep their
existing resource owners; grouping state does not open another connection or
create another identity.

```sh
make protocol-state-test
make rx-parse-test
```

This checks owner numeric codecs, invalid values, the six telemetry query stages,
one-port and host sessions, stale outcomes and exact deadline boundaries in debug
and release. Fixtures use generated values and closed or loopback connections;
the command does not transmit over RF.

RX checks count ingress and held-packet decodes. Immediate processing reuses the
validated ingress packet, so it adds no second parse; a held packet is decoded
once when its deadline arrives. Cases include local reflection, malformed input,
SF7 and a measured zero-score flood delay. These counters are in-memory only and
do not change role snapshots or radio packets.

The framing modules are direct std promotion candidates. The stream and child
modules are Linux adapters with reusable Hew policy: timeout handling,
partial-write continuation, typed errors, framing independence and actor
ownership. Their C boundary is syscall-oriented rather than a second service:
socket/connect/poll/send/recv, spawn/wait/signal and last-resort resource cleanup.
Worker fingerprint verification and cryptography remain separate native
boundaries.

## Readiness and timer ownership

```sh
make event-boundary-test role-timer-test bot-signal-test
```

The event checks exercise socket readiness, descriptor-number reuse, one-shot
rearming, explicit wake, shutdown races and routing two driver streams through
one native wait. The timer checks exercise exact advert and telemetry deadlines
and an idle room without pending deliveries. These commands use loopback sockets
and generated identities; they do not transmit over RF.

The host runs one readiness pump. Owner control and MQTT publishing each own a
child event group and stream; the pump routes child-group readiness without
reserving another offload worker for each driver. Hew drivers use `select` over
their streams and the earliest active deadline. MQTT queue admission, admin
replies, role incarnation changes and shutdown explicitly wake their driver.
Base, the dashboard and the broker each have their own process-local pump.

Remove a registration before closing its original descriptor, and ignore events
whose key has retired. A duplicate descriptor pins the original socket, so a
reused descriptor number cannot redirect an old registration to another
connection. `Watch` resources remove registrations when their owning actor
closes or fails. The service's `Driver` resource owns the stream reader and
native shutdown: it closes the reader and wakes an indefinite native wait
before pump stop/join, including during fault cleanup. Closing the reader also
releases a pump blocked on a full stream.

Zero-deadline reads attempt one nonblocking receive directly; they do not
offload a zero-timeout poll. Writes retain their bounded partial-write contract.
Libcurl supplies its current socket interests and timer to the same event
boundary; TLS and WebSocket buffering is drained in bounded turns. During
threaded DNS resolution only, when libcurl exposes neither a socket nor a timer,
the adapter checks progress after 100 ms.

Protocol timers remain distinct: receive metadata has a 30 ms deadline only
while a packet awaits metadata; TX outcomes, source-policy readback, MQTT ACKs,
keepalive, room deliveries and reconnect use their own deadlines. One-second
health/statistics and process-signal checks remain periodic. Completed role tick
tasks belong to a short turn scope, not the service's lifetime. The dashboard
reuses 32 HTTP RPC actors and two monitoring RPC actors instead of retaining a
new stopped actor for every request.

The native Lua/Wasm process uses change notifications for its VM, storage and
HTTPS queues. Its owner waits on stdin and an eventfd; worker threads wait on
a condition variable with a revision captured before checking their queues.
Only a changed atomic value notifies them. VM sleeps, pending HTTPS expiry,
durable timer checks and reminder checks supply active deadlines. Shutdown
wakes every worker before joining it. This host-only adapter leaves ESP32 and
nRF task scheduling unchanged.

### Compare idle CPU and retained heap

Build the candidate, then compare it with an unchanged accepted installation:

```sh
python3 -B tests/event_performance.py \
  --reference /path/to/accepted/hosts/willow/build \
  --seconds 60 --warmup 15 --warmup-cycles 16 --churn-cycles 64 \
  --report build/event-performance-results.json
```

Synthetic state and profiler sockets use private temporary directories under
the source root. For a long checkout path, set `MESHCORE_NATIVE_TEST_STATE_ROOT`
to an existing short absolute directory; complete Unix socket paths must fit
107 bytes on Linux. Executables and the path-bound worker stay in the selected
build directory.

Each isolated fleet runs a host and its native worker, two Base companions,
a broker and a dashboard with six synthetic identities. Each Base starts with
167 contacts and 256 retained messages. The script waits for combined readiness,
warms each fleet for 15 seconds and samples idle behavior for 60 seconds.
Before candidate sampling, it warms 16 client/RPC cycles, then runs 64
bounded cycles of companion connections, MQTT connections and dashboard
bot/role/status requests. After clients close and the fleet settles, descriptor,
epoll registration and thread counts must return to their warmed baselines;
live heap may grow by at most 128 KiB and actor counts by at most two.
Use `--churn-cycles 0` for an idle-only comparison.
It sends no RF commands and does not stop or configure the installed node.
Avoid concurrent builds and load tests during the comparison.

The report records binary hashes, CPU as a percentage of one core, live-heap
slopes, actor counts and message rates. The command fails unless total CPU falls
by at least 50%, profiled actor message traffic falls by at least 80%, each
candidate Hew process sends at most 500 idle messages/second and grows by at most 256 live bytes/second
and 128 KiB over the interval, and each actor count grows by at most two.
For a reference broker that predates actors and cannot start the runtime
profiler, pass `--synchronous-reference-broker`. Its CPU and RSS still count;
the report marks its unavailable heap/actor counters explicitly.
The CPU/message reductions and 500-message/second limit apply to idle samples,
not active client/RPC work. Native-worker RSS, descriptors and threads are
recorded separately: RSS includes allocator arenas and is not a live-heap
measurement. Reports remain private mode-0600 files.

For later state/type cleanup, pass `--nonregression` and select the deployed
accepted build as `--reference`. The checker runs matched finite warmup/churn
for reference, candidate and reference again. CPU and idle message limits use
the larger reference mean plus the larger of observed reference drift and two
standard errors of the sampled rates. The report retains both reference samples
and the resulting noise envelope. Heap, actor and ownership bounds remain the
same; the original 50%/80% improvement gates do not apply to this mode.

### Compare a finite Go/Hew packet workload

Build both hosts, then run the same 72-request workload against one loopback
modem fixture:

```sh
go build -o build/meshcore-reference ../../cmd/meshcore-host
HEW_WORKERS=4 python3 -B tests/matched_load.py \
  --go-binary build/meshcore-reference \
  --report build/matched-load-results.json
```

Go uses four ordinary sources; Hew uses one source with four logical ports.
Both use the same identities, retained native program, role names, PHY readback,
MQTT broker and request bursts. The checker first requires Go's actual
`per_role` startup and four-role readiness on that fixture. It compares
authenticated reply text, counts terminal transmissions and emulated recipient
ACKs, and records queue waits, CPU, descriptors, actors and Hew live heap.

The fixture models half-duplex RX/TX airtime from actual packet lengths at
SF7, 250 kHz bandwidth and CR 4:5. Its queue holds at most 64 submissions.
These modeled airtimes and peer ACKs are not physical RF measurements, and the
finite workload does not establish maximum capacity. Native-worker and Go RSS
remain allocator/arena proxies, not live-heap measurements. Reports are private;
failed fixture state is retained for inspection.

`hostlib.binary` reuses `std.encoding.binary` for fixed-width bit patterns.
Its adapter checks lengths and pads short integers before calling std getters,
whose own short-buffer contract aborts rather than returns `Option`.
Variable-width slicing uses the built-in owned `bytes` operations.
MeshCore's typed `Packet` codec and route/path checks remain Hew functions in
`src/wire.hew`; there is no native packet decoder. Existing Go golden and
serialized differential vectors exercise that protocol facade.
The new protocol modules are pure Hew rather than application-specific native
decoders. Checked float32 conversion is another std promotion candidate.
`make management-test` checks the codecs and their real role/service paths.

`duplex.write` retries only the remaining suffix during that invocation.
After failure, callers must retire the connection rather than replay an
entire partially transmitted frame. `committed=-1` means the actor stopped
before reporting a prefix; it does not mean zero bytes reached the peer.
Successful socket writes are not RF confirmations. The MKISS session still
requires the correlated terminal modem event; disconnect leaves pending
transmissions unknown.

Child retirement shuts down the channel, allows one second for exit, then
one second after SIGTERM and one after SIGKILL. Hew performs that policy and
polls exit status without blocking a scheduler thread. Reaping disarms the
owned PID before returning, so repeated retirement cannot target a reused PID.
Resource cleanup closes
the descriptor and force-reaps a still-owned child if normal retirement did
not finish. The final POSIX reap can still wait on an OS-uninterruptible child;
the API does not promise otherwise. Child stderr and environment are inherited,
not captured or filtered. This is process ownership, not a security sandbox.

## Observer publisher and transport

```sh
make observer-test
make observer-service-test
```

The first command exercises observer admission and publisher libraries.
The second runs the production release service through localhost modem and
MQTT connections, including broker retry, RF filtering, modem reconnect and
room login. [Service configuration](SERVICE.md#native-mqtt-observer) enables
native publishing from port 3 with the retained observer identity. Keep only
Base companion in the Go sidecar when Willow owns the observer.

* `hostlib.mqtt` builds MQTT3.1.1 clean-session CONNECT, QoS1 PUBLISH,
  PINGREQ and DISCONNECT bytes. Twenty-five frames match Go's pinned Paho
  encoder in each build mode. Its bounded incremental decoder retains
  complete packets before a malformed suffix, so a received PUBACK need
  not become an unknown outcome because a later packet is invalid.
* `hostlib.outbound` owns a TCP/TLS connection backed by libcurl
  connect-only. Hew drives the nonblocking connection and write deadlines;
  libcurl supplies asynchronous DNS, verified TLS and optional WebSocket
  framing, not MQTT or roles.
  The native boundary accepts the configured broker destination, restricts
  transport schemes, disables proxies, and enables certificate and hostname
  verification with TLS1.2 or newer. Tests use only loopback.
* `meshproto.mqtt_endpoint` validates operator `tcp`, `tls`, `ssl`, `ws` and
  `wss` URLs without dialing. TCP/TLS URLs require an explicit port, as the Go
  publisher's dialer does; WebSocket defaults are 80/443. Credentials, queries,
  fragments, malformed hosts and invalid ports are rejected with typed
  errors. The publisher uses the configured host/path; no replacement endpoint
  is selected.
* WebSocket connections require the `mqtt` subprotocol, verified TLS for WSS,
  and binary frames. Libcurl handles masking, fragmentation and control
  ping/pong. The adapter limits response headers to 16 KiB and individual
  incoming frames to 1 MiB, while delivering chunks of at most 4096 bytes.
  Text and oversized frames retire the connection without settling a pending
  MQTT publication. Partial sends report accepted payload bytes, not broker
  acknowledgement.
* `meshproto.observation` encodes `internal-v1`, `observer-v1` and `capture-v1`
  topics, status and raw packet events. 846 semantic vectors match the actual Go
  encoders in each build mode, including all packet headers, packed paths,
  reflected/missing signal, quarter-dB SNR, timestamps and filtering.
* `meshproto.observer_auth` creates the MeshCore identity JWT signing input and
  validates the resulting signature. The actual Go verifier accepts the native
  token. Identity signing remains in the existing crypto boundary; private keys
  never appear in observer payloads.
* `observer_queue` owns bounded admission (1–4096 records, mailbox 32) and one
  pending encoded event. `observer_client` owns the output-only MQTT connection
  (mailbox 16). Each has an independent two-restart/60-second supervisor and a
  stable child reference. Broker connection, writes and acknowledgements cannot
  hold the admission actor behind network I/O.
* The publisher requires a retained online status PUBACK before reporting
  connected. Packet publications are QoS1, nonretained; the same event bytes
  survive uncertain delivery and publisher replacement. Only the matching
  PUBACK settles that event. Broker acknowledgement does not confirm subscriber
  processing or a radio transmission.
* The connection has a three-second setup/control deadline, ten-second keepalive,
  three-second PINGRESP deadline and one-second reconnect backoff. Identity
  tokens renew five minutes before their 24-hour expiration and reconnect on
  clock reversal. Public formats wait for synchronized UTC.
  A static password without a username is rejected instead of sending an
  invalid CONNECT or silently discarding the password.
* Shutdown stops admission and allows at most one second for retained offline
  status and DISCONNECT. A replacement publisher respects a closed outbox and
  does not reconnect. Pending/queued observations are memory-only, as in Go:
  publisher replacement retains the outbox, but losing that actor or the process
  loses its observations. A fresh queue nonce fences acknowledgements from an
  earlier queue incarnation.
* Input chunks are at most 4096 bytes and MQTT bodies at most 1 MiB.
  Writes retain an accepted-byte prefix; acceptance is not a broker PUBACK.
  A resource-owning actor closes its connection on failure, and a stable
  supervised reference reaches its replacement.

The loopback broker suite exercises verified TLS/WSS, static and identity
authentication, keepalive, clock changes, status gating, duplicate/wrong ACKs,
unchanged retry payloads, WebSocket control interleaving/backpressure and actual
publisher panic. A PUBACK before EOF or a
malformed suffix remains counted as confirmed; a connection that ends before
PUBACK leaves the pending event available for retry. The broker fixture never
subscribes on behalf of Willow, and Willow has no inbound MQTT-to-radio path.

The transport build needs the installed libcurl development headers and
library, `pkg-config`, and OpenSSL for synthetic certificate tests.
`http`/`https` URLs in the transport probe select plaintext/TLS connect-only;
they are internal transport inputs, not operator MQTT configuration. Those
schemes send no HTTP request; `ws`/`wss` send the required HTTP upgrade.
Observer identity/configuration migration and modem configuration ownership
remain integration work. Base is a separate protocol-13 companion endpoint with its own
identity, listener, contacts, channels and retained messages; an observer
publisher is not its replacement.

## Failure behavior actually exercised

| Injection | Observed behavior on rc7, debug and release |
| --- | --- |
| Unsupervised Hew actor `panic` | Its caller receives failure; a sibling continues. Normal process shutdown returns 1 even when the caller handled the failure. |
| `one_for_one` supervised child panic | Only that child restarts; the original child reference works again; sibling state remains intact; shutdown returns 0. |
| Independent role supervisor budget exhaustion | Bot restarts once, then exhausts its own budget. Modem/relay/room counters retain state and advance to 3; main completes, but process shutdown returns 1. The runtime deliberately records an unrecovered supervisor fault globally. |
| Resource-owning actor panic | Its socket closes; the peer receives EOF and an unrelated actor continues. |
| Native `abort()` inside a supervised actor | The entire Hew process receives SIGABRT. An actor boundary cannot contain a native process abort. |
| Native bot process SIGABRT or SIGSEGV | Willow retires/restarts only the worker. Actual relay forwarding and authenticated room login continue in the same modem epoch. The replacement worker answers an authenticated `!ping`. |
| Room snapshot commit failure | That role stops and suppresses uncommitted replies. Relay/bot continue; reconnect does not advertise the failed room. Restart restores the last committed snapshot. |

Expected storage and I/O errors are typed values, not deliberate actor panics.
A quarantined role requires process restart after its storage fault is repaired.
The service does not blindly restart a stateful actor with fresh empty state.
Its modem and worker recovery have separate backoffs; persistent role failures
are quarantined. The independent-supervisor fixture checks the restart-budget
mechanism without introducing a shared root failure budget into the service.
`make supervision-test` separately tests the actual production branches,
committed-state reconstruction and stable owner replacement; see
[production supervision](SUPERVISION.md).
The C++ worker remains a process boundary because Lua/Wasm/native aborts need
stronger containment than actor supervision provides. Its existing invocation
budget remains **20 ms**, unchanged.

The rc7 runtime's `signal.rs` deliberately treats synchronous hardware faults
as process-fatal: ownership interrupted at an arbitrary native instruction
cannot safely resume. Its `exit_status.rs` also deliberately retains the
unrecovered-supervisor failure status. Neither behavior is reported here as
a compiler bug, and Willow does not attempt signal recovery or `longjmp`.
The `std.failure.CrashKind` documentation's SEGV/BUS `Fault` descriptions
need clarification against that native runtime boundary; an enum
classification does not establish recoverability.

## Standard networking: useful capability and one blocking gap

`std.net.Connection.split()` and `select` on its receive stream work.
`tests/probes/std_net_select_probe.hew` times out a receive after 20 ms, then
successfully sends/receives on the same connection. A timed receive therefore
does **not** inherently require custom socket polling.

However, the executed saturated-socket test shows that an ordinary
`Connection.set_write_timeout(30)` deadline causes `Sink.send` to panic its
actor with `TCP I/O timeout`, rather than return `Err(SendError.Full)` or
another typed transport error. The caller can catch the actor failure and a
sibling continues, but normal shutdown returns 1 and the API cannot report
the committed prefix.

`tests/probes/std_tcp_write_probe.hew` is the small reproducer; `make library-test`
provides its non-reading loopback peer and verifies this behavior in both
build modes. The service instead uses the minimal POSIX boundary beneath
`duplex` so an expected deadline is a value, with a measured prefix and no
actor crash. Its corresponding 30 ms saturation test passes without changing
the timeout. Promotion request: typed TCP error/timeout results and explicit
partial-write semantics, without converting normal network failures to panic.

## Compiler and process API reproductions

The [compiler probes](../tests/probes/README.md) record current type-check
results for revisions `3e9267d355` and `017db3b`. The range-cast and contextual
`Option.None` examples now type-check on both revisions. The send-select and
process-pipe examples still fail:

```sh
hew check tests/probes/select_send_probe.hew
hew check tests/probes/process_pipe_probe.hew
```

* `Sink.send` is not an accepted `select` source; actor calls, tasks and stream
  receives are. The TCP timeout reproducer uses the supported socket timeout,
  not a simulated send cancellation.
* `std.process.Child` has no `stdin()` pipe interface. The working fallback
  is bounded argv plus a POSIX duplex socketpair, not shell quoting or a Go
  subprocess. Promotion request: owned stdin/stdout pipes or duplex IPC,
  nonblocking exit observation and explicit termination/reaping ownership.

Run the focused checks with the current `hew` compiler and its matching
libraries on `PATH`. Public language references:
[actors](https://hew.sh/docs/actors/),
[supervision](https://hew.sh/docs/compare/supervision/),
[stdlib](https://hew.sh/docs/stdlib/) and
[language specification](https://hew.sh/docs/language-spec/).

## Build toolchain

```sh
git clone https://github.com/slepp/meshcore-aspen.git
cd meshcore-aspen
make -j2 willow-check
cd hosts/willow
python3 -B tools/willow.py --version
```

Expected: `Willow 0.0.11`. Follow the [installation and startup guide](../README.md)
for a new node, or migrate an existing stopped node. From the repository root, `make
willow-build` builds the host, Base, broker and dashboard without running the
checks. The component's `make release` and `make check` targets remain available.
The launcher defaults to four scheduler workers; set
`HEW_WORKERS` explicitly to choose another count.
For production builds, build Hew with its supported `make hew` target and put
its `build/bin` directory on `PATH`. This selects matching optimized runtime
libraries. `hew build --release` optimizes the application; a compiler launched
from `target/debug` still uses that toolchain's debug runtime libraries.
For a new installation, [create private state](SERVICE.md#start-with-new-identities)
with `build/hew-host-release init`, then use the state check and launcher in that guide.
`make willow-check` runs the Hew applications against private loopback modem emulators,
checks restart and native-worker recovery, exercises interrupted persistence,
and compares protocol behaviour with Birch. Missing prerequisites fail the
command. The selected compiler, runtime and standard-library hashes are
recorded in ignored `build/toolchain.json`; the compiler version is printed.
The contributor CI runs this same command. The broader `make test test-release`
targets retain the complete debug/release protocol, management, login,
readiness and migration suites.

A compatible toolchain is Hew source
`3e4970c379d0ca5856d4bb47c08d7d80a29934db`, with its matching optimized
runtime and standard library. This revision types declared supervisor
children as their actor and still takes `i64` millisecond arguments in
`datetime.format`; Willow does not type-check with older or newer Hew
revisions. Normal builds select the current `hew` from
`PATH`. For a Hew source checkout, run `make hew` and select its `build/bin`
launcher. The published rc7 SDK predates Willow's actor syntax.
See [building a Linux bundle](BINARY_RELEASE.md) for the Debian 12 build
and the included source, notices and relinking material.
Compiler-specific probe results are documented separately in
[tests/probes](../tests/probes/README.md).

Production modules live in `src/`, native bridges in `native/`, launch and
build helpers in `tools/`, and checks in `tests/`. Internal Hew imports use
`willow.src` so applications and checks resolve the same modules.

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
written to `build/service-results.json`. See [running the service](SERVICE.md)
for configuration, startup output, queue limits and recovery behavior.

## Development commands

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
See [RF management](MANAGEMENT.md) for commands and persistence behavior.

See [the fixture interface](PROTOCOL.md) for authenticated input packets,
KISS stream fragments, queued-modem submissions, and time advancement.

## Service implementation and protocol limits

| Area | Implementation | Limits and differences |
| --- | --- | --- |
| Identity and crypto | Persistent Ed25519 seed identities; signed adverts; Ed25519-to-X25519 shared secrets; MeshCore AES-128-ECB with zero padding and two-byte HMAC; ACK proofs; group-channel authentication; matching-seed Birch identity migration | Expanded-only active identities without a matching seed require further implementation. Group authentication proves channel-secret possession, not a unique sender. |
| Frames | MeshCore payload version 0; all four route encodings; ordinary widths 1/2/3, 64-byte path and 184-byte payload limits; TRACE widths 1/2/4/8 with distinct SNR return path; KISS escaping, fragmented receive, malformed-frame recovery; per-port DATA/RX_META pairing and local-reflection classification; CONTROL discovery | TRACE originates/terminates at the retained companion Base; Willow forwards its route and return hops. |
| Repeater | Flood append, direct next-hop removal, packet dedup, local-reflection suppression, off/minimal/moderate/strict loop policies, configurable hop limits and airtime-based randomized delays, up to eight permitted region keys, independent home/default scope; configurable room forwarding and source airtime factor | 160 dedup hashes; up to 64 packets waiting for measured-SNR receive delay. Use the [owner region controls](SERVICE.md#configure-a-roles-named-regions) for local region changes, not an RF region editor. |
| Room login | Authenticated password/admin/ACL login, explicit public-room opt-in, read-only guest mode, permission bits, replay rejection, flood PATH reply, direct/flood response routing, learned paths and multi-ACK policy; full service tables evict the least-active nonadministrator | 20 members; all-administrator tables refuse new identities. |
| Room delivery | Automatic service ticks; post retry ACK without duplicate history, 150-byte new-post retention, 32-post history, own-author suppression, six-second delay, round-robin pushes, recipient-key delivery ACK, learned paths, three-failure suppression, keepalive proof/count; migrated durable history and subsequent posts survive restart | Existing Go posts up to 151 bytes, including opaque RawText, are preserved. |
| RF management | Authenticated CLI, durable role settings and ACLs; binary status/telemetry/ACL; relay neighbour/owner and anonymous queries; measured CONTROL discovery; optional modem sampling | [Exact commands and remaining surfaces](MANAGEMENT.md). Saved coordinates, role RTC offset, packet-log controls and role restart are supported. No RF named-region editor or remote identity import. |
| Default bot extension | Signed contact discovery; authenticated DM/group commands; default Lua commands and private KV; canonical retry dedup; delayed multi-path collection; native RF outcomes; independently persisted Wasm package alongside Lua | Existing C++ runtime, not reimplemented in Hew. Verified commands include ping, calc, remember, recall, mt, air, and Wasm wadd. Native owner administration and package operations use the [service controls](SERVICE.md); inherited commands are not all exercised here. |
| Native bot regions | Separate home/default keys; known request scope, unscoped flood preservation, explicit fallback, default-scoped adverts and home-only unscoped adverts; serialized Go/native differential plus independent Go policy/code checks | Two registered bot keys, changed through local owner administration. Relay/room use their own permitted keys and explicit defaults. |
| Worker build binding | Actual worktree native worker built with pinned MeshCore/Lua/WAMR inputs; recorded source fingerprint, compiler commands and input/binary hashes; compiled-in manifest verification before startup/restart | Local build binding, not a signed/reproducible release. Arbitrary installed workers are refused; source/build tree must remain available. |
| All-Hew bot | Signed discovery; DM/group authentication; public/private gates; two-minute request dedup; targeted commands; private notes; deferred mt | Smaller command subset below; no Lua/Wasm or advanced native APIs in this mode. |
| Async/lifetime | Three role actors run bounded batches concurrently; eight RX packets per role per batch; independent native extension process; automatic ticks; bounded mailboxes and 32 outstanding TX jobs per port; worker faults leave relay/room connected | Disconnect cancels native collectors and marks pending TX unknown. A healthy worker retains contacts across modem reconnect; after worker restart, the native bot can recover signed contacts from its configured retained Base authority. Missing/invalid contacts require a fresh advert. The all-Hew bot has eight deferred jobs. |
| Supervision | Independent declarative role, socket-owner and native-process-owner branches; stable child references; two actor restarts per 60 seconds; committed-state reconstruction and role-local quarantine | Coordinator, epoch session/decoders and metadata actors are outside these budgets. Native hardware faults in the Hew process remain fatal; the native worker stays in a subprocess. |
| Shared modem | TCP/MKISS HELLO, extended CAPACITY, read-only CONFIG, SOURCE_POLICY, ROLE_PRESENCE warning, signal reporting, full airtime table, PHY statistics; correlated queued TX; reconnect and periodic profile fencing | Numeric IPv4 TCP only; no serial, TLS or modem configuration ownership. Three logical ports, or four with observer enabled. Required profile readback never retunes. Presence is advisory. Base uses its own ordinary connection rather than an aggregate port. |
| Base companion | Separate native process, retained identity, protocol-13 TCP endpoint, contacts, channels, message cursors, RF traffic and read-only health | Protocol coverage is partial; select destructive and key-management operations explicitly. See [Base setup and contracts](BASE.md). |
| Owner and dashboard | Private owner/admin sockets; native-bot DM send/inbox, role commands and local region controls; six-role status and combined readiness | Owner commands retain authentication and permission checks. Dashboard monitoring does not own or retune the modem; see [service controls](SERVICE.md). |
| MQTT observer | Port-3 RF observations; independently supervised outbox and publisher; Go-compatible topics, payloads and filtering; retained identity migration; QoS1 replay across broker reconnect; verified TLS/WebSockets and optional identity JWT | Configure your broker hostname or IP with `observer.url`. Output-only; no RF TX or MQTT commands. Bounded queue with batched durable checkpoints and a controlled-stop flush. Crash recovery starts at the last completed checkpoint. |
| Persistence | Private identities; atomic state replacement; directory sync; lock exclusion; persisted ACL/replay/cursors/paths/counters/notes; optional durable history; stopped role on commit failure; offline Birch migration and [final-state reconciliation into a new Go candidate](MIGRATION.md#rollback) | Active sessions/pending ACKs/dedup/deferred calls are cleared. Native-retention history remains volatile; durable-replay history persists. Unsupported migration data is refused explicitly. No encryption at rest or guaranteed private-buffer zeroization in Hew. |

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

## Service checks

`make firmware-negotiation-test` runs debug/release Willow against the actual
shared-modem C++ firmware on loopback, including signal-report response opcodes,
the full airtime table, two independent compatibility connections, a busy
aggregate session and profile-mismatch diagnostics. `make test` includes it.

`make library-test` runs twelve debug/release groups for reusable byte/framing,
owned network/process APIs, deadlines and partial writes, argument fidelity,
resource cleanup, actual panic/supervision behavior and unaffected-role fault
isolation. See [library contracts and Hew runtime feedback](LIBRARIES.md).

`make supervision-test` runs actual production owner/role panics, durable-state
reconstruction, independent budget exhaustion, missing/corrupt-state quarantine
and modem/native-child replacement in debug and release. See
[production supervision and remaining RF surfaces](SUPERVISION.md).

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

Additional checks (compiler/API examples are in [tests/probes](../tests/probes/README.md)):

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

## Source layout and interfaces

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
