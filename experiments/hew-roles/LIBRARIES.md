# Willow host libraries and Hew runtime contracts

These adapters target Hew **0.6.0-rc7**. The checks below exercise the
transport, ownership and failure contracts used by Willow.

Use `hostlib` for bounded framing, owned byte streams and shell-free duplex
children. The modem owns one connection actor; relay, room and bot work runs
in separate actors. The native Lua/Wasm worker has a separate process and
an actor owning its channel and PID.

```sh
cd experiments/hew-roles
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
| `meshproto.management` | Pure typed status/LPP/neighbour encoders, native numeric formatting, UTC and UTF-8 limits. | 50 neighbour entries, 130 bytes of page entries; no packet authentication or I/O hidden in the codecs. |
| `meshproto.preferences` | Validate/change/encode/decode complete role-local preference values. | At most 330 encoded bytes; no mutation on decode failure; binary32 delays; bounded names, credentials and owner text. |
| `meshproto.radio` | Pure optional-request state machine with typed timeout/malformed/rejected failures. | One outstanding read, two-second deadline, 45-second cache; uncorrelated failure pauses sampling until a new connection. |
| `meshproto.cli` | Native RF text and measurement formatting from typed values. | Explicit unavailable readings and bounded command replies; authentication and commits remain in the role actor. |

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
  --reference /path/to/accepted/experiments/hew-roles/build \
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
`wire.hew`; there is no native packet decoder. Existing Go golden and
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
  The native boundary restricts destinations to loopback and
  `mqtt-meshcore-1.ve6slp.ca`, disables proxies, and enables certificate and
  hostname verification with TLS1.2 or newer. Tests use only loopback.
* `meshproto.mqtt_endpoint` validates operator `tcp`, `tls`, `ssl`, `ws` and
  `wss` URLs without dialing. TCP/TLS URLs require an explicit port, as the Go
  publisher's dialer does; WebSocket defaults are 80/443. Credentials, queries,
  fragments, invalid ports and other destinations are rejected with typed
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
`tests/std_net_select_probe.hew` times out a receive after 20 ms, then
successfully sends/receives on the same connection. A timed receive therefore
does **not** inherently require custom socket polling.

However, the executed saturated-socket test shows that an ordinary
`Connection.set_write_timeout(30)` deadline causes `Sink.send` to panic its
actor with `TCP I/O timeout`, rather than return `Err(SendError.Full)` or
another typed transport error. The caller can catch the actor failure and a
sibling continues, but normal shutdown returns 1 and the API cannot report
the committed prefix.

`tests/std_tcp_write_probe.hew` is the small reproducer; `make library-test`
provides its non-reading loopback peer and verifies this behavior in both
build modes. The service instead uses the minimal POSIX boundary beneath
`duplex` so an expected deadline is a value, with a measured prefix and no
actor crash. Its corresponding 30 ms saturation test passes without changing
the timeout. Promotion request: typed TCP error/timeout results and explicit
partial-write semantics, without converting normal network failures to panic.

## Compiler and process API reproductions

These checks intentionally fail on the installed rc7:

```sh
hew check tests/range_cast_probe.hew
hew check tests/select_none_probe.hew
hew check tests/select_send_probe.hew
hew check tests/process_pipe_probe.hew
```

* A literal `for value in 0..256` followed by `value as u8` leaves the element
  type unresolved. Binding the upper bound as `i64` works; production code
  keeps range loops rather than replacing them with manual counters.
* A `.None` timeout arm is not contextually typed from an enclosing
  `Option<bytes>` annotation. A separately typed empty value works.
* `Sink.send` is not an accepted `select` source; actor calls, tasks and stream
  receives are. The TCP timeout reproducer uses the supported socket timeout,
  not a simulated send cancellation.
* `std.process.Child` has no `stdin()` pipe interface. The working fallback
  is bounded argv plus a POSIX duplex socketpair, not shell quoting or a Go
  subprocess. Promotion request: owned stdin/stdout pipes or duplex IPC,
  nonblocking exit observation and explicit termination/reaping ownership.

Run these checks with `hew 0.6.0-rc7`; later releases need their own
compatibility checks. Public language references:
[actors](https://hew.sh/docs/actors/),
[supervision](https://hew.sh/docs/compare/supervision/),
[stdlib](https://hew.sh/docs/stdlib/) and
[language specification](https://hew.sh/docs/language-spec/).
