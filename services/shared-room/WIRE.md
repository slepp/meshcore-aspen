# Aspen room wire v1

Use JSON text WebSocket messages with the `aspen-room.v1.json` subprotocol and
`/v1/aliases/{alias}/socket`. The ready event identifies `version:1`; an offered
unsupported subprotocol is rejected. A client that omits subprotocol negotiation
uses the same v1 format. Additive optional fields can be ignored. A change to
required fields or representation needs a new version/subprotocol.

Both directions have a **4096-byte UTF-8 frame limit**. Correlation IDs contain
1..32 ASCII letters, digits, `_` or `-`. Keys and attempt hashes are lowercase
hexadecimal. A route is canonical standard base64 containing 0..255 opaque
bytes; decoding preserves NUL, non-UTF-8 and every other byte. Posts are bounded
UTF-8 text, not raw RF payloads. A future raw-packet operation would need its own
specified byte field; the current Worker accepts authenticated decoded operations.

Member replies fit the same frame budget. If the result would exceed it, query
longer hexadecimal prefixes. History comes one message at a time, so no batch
history DOM or replica is needed at the device. The frame itself can occupy one
reusable PSRAM buffer, with shallow views into it. Release it after converting
the operation into bounded RF bytes; do not retain views in another task.

## Encoding comparison

Representative payload sizes measured by `tools/measure_wire.py`:

| Frame | JSON v1 | CBOR, same fields | CBOR, binary keys/routes/proofs |
| --- | ---: | ---: | ---: |
| Login | 264 B | 229 B | 162 B |
| Original post (`hello`) | 245 B | 213 B | 149 B |
| History request | 257 B | 221 B | 154 B |
| History delivery (`hello`) | 363 B | 313 B | 225 B |
| New-post notification | 373 B | 323 B | 235 B |
| Client ACK | 182 B | 162 B | 105 B |
| Login result | 63 B | 44 B | 44 B |
| ACK result | 48 B | 35 B | 35 B |
| Ready | 139 B | 122 B | 90 B |
| Delivery, 255-byte route and 151 escaped control characters | 1592 B | 790 B | 619 B |

CBOR measurements include a one-byte version prefix. The binary-field variant
uses actual 32-byte keys/hashes, 4-byte proofs, 16-byte UUIDs and decoded route
bytes, retaining the field names. It is a comparison tool, not a second supported
service protocol. WebSocket framing, TLS records and TCP are excluded from the
payload table.

The native ciphertext fixture's 28-byte original post occupies 65 bytes in a
JSON/base64 `rx` envelope, versus 34 bytes with a six-byte version/type/request-ID
header and an opaque packet. That smaller envelope moves raw MeshCore crypto
termination into the Worker. The implemented architecture instead reuses native
crypto at the frontend and sends durable operations, so these raw sizes do not
compare equivalent service work.

**Production choice: JSON v1.** CBOR binary fields save about 39–42% on the normal
radio operations, but add a second schema/decoder path. The small frames and
single-message delivery model allow JSON to use a bounded, allocation-free parser.
Keep the readable JSON as the production/debug representation rather than
maintaining parallel JSON and binary implementations.

## Native parser and measured work

[`firmware/shared/cloudroom/CloudRoomWire`](../../firmware/shared/cloudroom/CloudRoomWire.h)
uses the maintained MIT [FreeRTOS coreJSON v3.3.1](https://github.com/FreeRTOS/coreJSON/tree/v3.3.1),
pinned at `14e969a3a4e14e1799031e0528dfdf39897a4ec7`. The unchanged C source and
license are vendored. Compile it with `JSON_MAX_DEPTH=8`, as the supplied Makefile
does. `JSON_Validate` checks syntax, UTF-8 and Unicode escapes; searches return
slices into the original buffer. The wrapper checks event types, version and
field bounds. A consumer must still validate the relevant result/message fields
before acting on them. `decodeString` and `decodeBytes` write directly to bounded
caller-owned final buffers. No JSON tree, token array or full-frame copy is made.

Host checks on Clunk (macOS arm64, Clang `-O2`, 20,000 parses per fixture) measured
roughly 0.4–1.3 microseconds for representative requests/deliveries and 4.0
microseconds for the escaped maximum delivery. These are host measurements;
ESP32 timing and total WSS/TLS peak memory remain to be measured on a device.
The parser made zero heap allocations; the compiled C parser has no allocator
references. The host `View`, `Event` and `Document` sizes are 24, 152 and 24 bytes.
A history delivery copies only its final 5 text bytes plus 9 route bytes; the
maximum sample copies 151 text plus 255 route bytes. Metadata remains in slices.

Work is one bounded validation scan plus searches for requested fields (each
search can rescan the bounded document). The device can iterate a members array
with `JSON_Iterate`, releasing each entry after processing it. The comparison
script's Python CBOR decoder builds objects and reports its host parse time and
traced allocations; those numbers are not an embedded CBOR benchmark. A different
embedded CBOR library could also avoid a DOM, but is not needed for this v1.

Reproduce the size comparison and focused parser checks:

```sh
python3 -m venv .tmp/wire-tools
.tmp/wire-tools/bin/pip install cbor2==5.6.5
.tmp/wire-tools/bin/python services/shared-room/tools/measure_wire.py
make -C firmware/shared/cloudroom test
```

The fixtures use public test keys and the committed native ciphertext vector.
The checks cover bounds, malformed JSON, Unicode/surrogate decoding, base64
canonical bits and binary bytes. They do not use credentials, connect to
Cloudflare, flash a device or transmit RF.
