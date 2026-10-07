# Aspen opaque room wire v2

Use JSON text frames with `aspen-room.v2.json` and
`/v1/aliases/{alias}/socket`. The ready event identifies `version:2`, alias,
public key and name. An unsupported offered subprotocol is rejected. A client
omitting negotiation receives the configured mode's format. Public deployments
use the default `MODE="opaque"`; legacy v1 requires explicit decoded mode.

Both directions have a **4096-byte UTF-8 frame limit**. Correlation IDs contain
1..32 ASCII letters, digits, `_` or `-`. Packet fields are canonical standard
base64 containing 1..255 raw MeshCore bytes. Transmit IDs are UUIDs; delay is
0..30000 milliseconds and priority is an unsigned byte. See the
[three-operation API](README.md#small-opaque-api).

RF packets use native payload version zero and unscoped flood/direct routes.
Path encoding uses the upper two bits for hash width and lower six for hop
count, bounded by native's 64 path bytes and 184 payload bytes. Region-scoped
transport is rejected until a region-key contract is supplied. The Worker
tries every permitted room/client prefix and requires exactly one native
MAC-authenticated decode. Bare ACKs are resolved against owned pending proofs
when all configured aliases share one backend; ambiguous proofs are rejected.
Services configuring multiple backends send flooded history through all frontends and require the native
encrypted PATH+ACK. Its room/client MAC resolves even identical short proofs.
Mutable packet paths do not affect RF-attempt
deduplication. Only original TXT format enters durable history.

`firmware/shared/cloudroom/CloudRoomWire` uses unchanged MIT FreeRTOS
coreJSON 3.3.1, pinned at `14e969a3a4e14e1799031e0528dfdf39897a4ec7`,
with `JSON_MAX_DEPTH=8`. It validates bounded UTF-8 JSON and returns slices,
then decodes packet bytes directly into the final radio buffer. It builds no
DOM or history cache. Release frame views before the reusable PSRAM frame
buffer is overwritten. `OpaqueFrontend` retains only bounded operation queues
and in-flight dispatch IDs, and checks version/public metadata before RF work.

A 28-byte native post is 86 bytes in a typical v2 JSON request including
correlation ID. A binary representation could save bytes, but the small
bounded JSON keeps one readable schema and a compact embedded parser.
`tools/measure_wire.py` retains the earlier decoded v1/CBOR comparison for
reference; those decoded envelopes are not the central-key transport.

```sh
make -C firmware/shared/cloudroom test
```

The focused portable tests check malformed JSON, bounds, Unicode decoding,
canonical base64, exact opaque bytes, ready validation, receipt priority and
reconnect generations. They use public fixtures and transmit no RF.
