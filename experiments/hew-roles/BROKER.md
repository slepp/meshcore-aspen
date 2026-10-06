# Local Hew MQTT broker

Build and run the standalone, in-memory Hew broker on an unused loopback port:

```sh
cd experiments/hew-roles
make broker
umask 077
printf 'host=127.0.0.1\nport=0\n' > build/broker-local.config
./build/hew-broker-release build/broker-local.config
```

`MQTT_BROKER_READY host=127.0.0.1 port=… protocols=3,4,5 memory_only=true`
names the listening port. Use `mosquitto_pub` and `mosquitto_sub` with
`-h 127.0.0.1 -p PORT -V mqttv311`. SIGINT or SIGTERM closes the listener and
its connections and emits `MQTT_BROKER_STOPPED memory_only=true`.

Use `-V mqttv31`, `mqttv311` or `mqttv5` for MQTT 3.1, 3.1.1 or 5.
Shared subscriptions and retained `$SYS/broker/…` metrics are available.
The intended local endpoint is **127.0.0.1:1883**, anonymous. Stop its existing
broker before starting this binary with `port=1883`; two brokers cannot own
the same endpoint. Validate an unused port before that handover.

The existing observer uses MQTT 3.1.1, QoS1 and retained online/offline status.
The broker does not change the observer's `meshcore/…` topics or own a
MeshCore identity. A broker restart loses all retained messages, subscriptions,
sessions and inflight messages, as does the in-memory Go broker. No identity
or state files are created or migrated.

## Private configuration

The sole argument is a configuration-file path, never credentials. Files
must be regular, owned by the process user, owner-only (for example mode
`0600`), nonsymlink, UTF-8 and 1..4096 bytes. Protect their directory with
mode `0700`. Lines are `key=value`; empty lines are allowed, duplicate or
unknown keys fail startup.

| Key | Default / condition |
| --- | --- |
| `host` | `127.0.0.1`; numeric IPv4/IPv6, or a hostname with credentials |
| `port` | `1883`; `0` asks the kernel for an unused port |
| `username` | Empty; must be supplied together with `password` |
| `password` | Empty; must be supplied together with `username` |

Anonymous access requires a literal loopback bind, including `127.0.0.1`
or `::1`; `localhost` and wildcard binds are not allowed anonymously.
Credentials permit other binds and are checked by SHA-256 followed
by constant-time comparison, evaluating both fields. All authenticated
clients have full topic access. Credentials and MQTT payloads are not
included in status or errors. TCP is unencrypted: use an external TLS
broker for an untrusted network.

An authenticated `0.0.0.0` or empty host binds dual-stack `::`, as does the
original Go listener on Linux. A scoped IPv6 address remains scoped.

## MQTT behavior and bounds

MQTT 3.1, 3.1.1 and 5 connections support clean and persistent sessions, live
session takeover, wildcard `+`/`#`, the `$` namespace exclusion, unsubscribe,
retained replacement/deletion, QoS0/1/2, wills, PING and keepalive deadlines.
Persistent reconnect resumes outstanding PUBLISH with DUP, or PUBREL after
PUBREC, preserving the packet identifier. Pending inbound PUBREC is replayed.
QoS2 routes once when the first PUBLISH is accepted; duplicate PUBLISH before
PUBREL does not route again. QoS1 retransmission may redeliver.

For overlapping subscriptions, live delivery uses the greatest subscribed
QoS, capped by the publication's QoS. Resubscribing sends matching retained
messages again. A retained zero-length publication clears its stored value
and reaches current subscribers. Normal forwarded publications clear RETAIN;
subscription-triggered delivery sets it. `#` and `+` do not match a topic
beginning with `$` unless the filter also begins with `$`, for live delivery.
The original retained lookup excludes `$SYS` but includes other `$` roots
when a wildcard subscribes; this broker preserves that behavior.

MQTT5 supports User Properties, payload format, content type, response topic,
correlation data, sorted subscription identifiers, No Local, Retain As
Published and Retain Handling. Topic aliases reset on reconnect. Session and
message expiry, delayed wills, Receive Maximum, Maximum Packet Size,
Request Problem Information, acknowledgement reasons and AUTH framing follow
the configured Mochi behavior. Username/password remains the authentication
method; AUTH does not introduce a new authentication provider.

Successful MQTT5 CONNACK advertises Receive Maximum **1024**. It does not
advertise Topic Alias Maximum or Maximum Packet Size, matching the original
broker's actual response. Empty MQTT5 client identifiers receive an assigned
identifier. Shared `$share/GROUP/FILTER` subscriptions select one subscriber
per exact group/filter; `$SHARE` is also accepted. Shared subscriptions do not
receive subscription-triggered retained messages.

Twenty retained `$SYS/broker/…` topics update once per second. `version` is
`hew-mqtt/1`; `system/memory` measures native allocator bytes and
`system/threads` counts POSIX threads, rather than Go heap/goroutines.
Ordinary client publications starting with case-insensitive `$SYS` are
discarded without a QoS acknowledgement.

| Bound | Behavior |
| --- | --- |
| Connected clients | 128; another valid CONNECT gets CONNACK server-unavailable |
| Open TCP connections | 256 including unconnected clients; CONNECT deadline 15 seconds |
| Inbound packet | Remaining Length + 1 <= 65536, matching the actual Mochi read boundary |
| Outbound QoS messages per session | 128; further deliveries are omitted with a quota status |
| Inbound pending QoS2 per session | 1024; overflow closes the affected connection |
| Pending writes per connection | 256; excess deliveries are dropped; control reads pause until space is available |
| Native send buffer | Requested 65536 bytes; the kernel may adjust it |
| Keepalive | `(interval + integer interval/2)` seconds; zero disables read/write timeout |
| Retained/session/subscription aggregate memory | No separate cap, as in Go; provision available RAM |
| MQTT3 disconnected session expiry | 4294967295 seconds |
| MQTT5 disconnected session expiry | CONNECT/DISCONNECT property; zero removes the session |
| Message/inflight expiry | 24 hours for normal publications and inflight state |

Mochi's configured “64 KiB packet” comparison excludes the variable-length
Remaining Length encoding: the largest accepted frame is **65539 bytes**.
Both implementations accept that boundary and reject the next body byte.
The Hew listener writes large frames by continuing only their uncommitted
suffix; it never retries an already-written prefix.

## Compatibility consequences

The configured Mochi v2.7.9 behavior is not a universal MQTT compliance
guarantee. These inherited behaviors affect clients:

* With MQTT5 Receive Maximum 1, the first acknowledged publication releases
  one deferred publication. That deferred flight is removed before its ACK;
  its ACK does not restore quota. Further queued delivery stalls until
  reconnect. The differential reconnect test preserves this behavior rather
  than claiming exactly-once delivery across that boundary.
* Unregistered nonzero inbound aliases with an empty topic are acknowledged
  but do not route a message. Register aliases again after reconnect. Alias
  zero closes the connection with reason 148.
* Message Expiry 0 uses the default 24 hours; positive values are capped at
  24 hours. Retained immediate wills omit Message Expiry; retained delayed
  wills can carry the elapsed delay as an expiry of 1 second. Retained wills
  still have the default 24-hour retention ceiling.
* Will Delay is clamped to Session Expiry at CONNECT, not recalculated after
  DISCONNECT changes the expiry. Reconnect with the same identifier cancels
  its pending will, including a clean start. A delayed will whose session has
  already expired is delivered live but need not be retained.
* One-byte MQTT5 DISCONNECT reason 4 is treated as normal disconnect;
  use `04 00` to request will delivery.
* Nonempty PINGREQ receives PINGRESP; zero-identifier PUBACK is ignored.
  Four continuation Remaining Length bytes wait for an additional byte
  before rejection. Embedded filter wildcards can be granted without useful
  matching. Will-topic wildcards, reserved MQTT5 subscription-option bits and
  out-of-range byte-format/info properties are also tolerated, as in the
  original broker.

Shared selection order and mixed overlapping Retain As Published settings
are not ordered by Mochi. Hew uses round-robin shared selection and combines
the retain setting. Reason codes are preserved; diagnostic wording and
generated identifiers are implementation-specific. TLS, persistence,
WebSocket listeners, enhanced-auth providers and external Mochi plugins were
not configured on the original local broker and are not provided here.

## Validation

```sh
make broker-test
```

This builds and executes the debug/release Hew binaries and a **test-only**
oracle using the application's actual `observer.StartBroker`. Production
uses neither a Go process nor a broker library. Tests choose private
ephemeral loopback ports and remove their private configuration files.

The oracle uses Mochi v2.7.9 as selected by the repository's `go.mod` and the
application's configured bounds, not a separate generic broker configuration.
The coherent differential lifecycle covers subscriber/publisher delivery,
retained replacement/deletion, QoS0/1/2, duplicate PUBLISH/PUBREL/PUBREC,
reconnect at both QoS2 phases, offline persistent subscriptions, live session
takeover with will, clean start, unsubscribe, shared live/offline sessions,
keepalive/PING, EOF will and graceful DISCONNECT across protocol levels 3/4/5.
MQTT5 scenarios check property forwarding, aliases, options/identifiers,
client limits, expiry, delayed-will cancellation/delivery and deferred quota
reconnect. Boundary groups check 128 clients, 128 inflight messages,
the actual maximum packet, malformed inputs, auth and supported protocol
inventory. External `mosquitto_pub`/`mosquitto_sub` exercise retained QoS0/1/2
for all three versions. Differential tests also check system topic schema
and updates, and a nonreading subscriber's dropped deliveries and continued
PING response while another client remains responsive. Hew-only groups
check private-config failures. Pure compiled checks cover UTF-8, filters,
integer/field/property ranges and expiry arithmetic.

Endpoint shutdown must finish within 10 seconds, including after all 128
client slots are filled, one is released and a replacement is admitted.
The Go host and oracle close listener clients from one registry snapshot
before stopping Mochi. This avoids the recursive registry read lock in
Mochi v2.7.9's shutdown lookup
([upstream issue #488](https://github.com/mochi-mqtt/server/issues/488)).
The dependency version, client bounds and MQTT shutdown DISCONNECT remain
unchanged. Run the production broker's concurrent shutdown regressions with:

```sh
make -C ../.. host-test HOST_TEST_PACKAGES=./internal/observer
```

`make broker-symbols` builds `build/hew-broker-symbols` with
`-g --opt-level 2` and the same native boundary. It does not run a profiler.
Profile only after the combined feature-complete system is running without
Go, as required by the deployment handover.

## Ownership boundary

`hostlib/mqtt_server.hew` and `hostlib/mqtt_properties.hew` contain generic
checked MQTT server primitives.
`broker.hew` owns protocol state, routing, QoS, queues, deadlines and expiry in
one serial event loop. `hostlib/listener.hew` owns a heap-backed TCP listener
resource; closing it closes every accepted socket, even on a checked error.
Opaque monotonically assigned socket tokens prevent a reused descriptor
from being mistaken for an earlier connection.

`listener_bridge.c` is limited to POSIX sockets, clocks/signals,
private-file reads, digest comparison and process resource metrics.
There is no MQTT parser, routing,
QoS/session policy, scheduler or broker runtime in C.
