# Direct native Aspen frontend

An Aspen ESP32 can build the optional Wi-Fi/WSS transport and radio task bridge
with `Xiao_S3_WIO_onchip_cloudroom`. **The profile cannot serve a room yet:**
its codec driver is disabled. Worker-owned room keys require a Worker-native
MeshCore codec and opaque RF API before a radio can connect and advertise.
Read [the trust boundary](TRUST.md) before configuring credentials. The existing
decoded API and Birch reference codec instead require keys at the frontends.

## Native transport and task bridge

- `firmware/esp32/LocalRadio.h`: attach one cloud-room source to the physical
  `WifiKissMultiplexer`, receive measured RF, reject local reflections, and submit
  all alias advertisements/responses/deliveries through `queueTransmit`. Only a
  final `SUCCEEDED` result means `sent`; acceptance does not. These methods stay
  on the dispatch task because its queues are not thread safe.
- `firmware/esp32/CloudRoomService`: implemented one network task, bounded
  eight-slot RX/TX/result SPSC queues and one dispatch-owned radio source for all
  aliases. Payload/frame buffers use PSRAM; task stack, synchronization and
  queue atomics remain internal. A short admission mutex makes disconnect
  invalidation atomic with RF queue admission. Already admitted RF is uncertain;
  unsubmitted work from an old connection is discarded. Operations are not
  automatically replayed after reconnect.
- `firmware/esp32/Runtime.cpp`: optional startup/loop and public-key presence
  registration. The compile profile reserves a sixth local radio source and two
  persistent alias sockets, reducing physical KISS clients to one so the stock
  16-socket SDK limit still holds. A one-alias profile can reserve one socket and
  use two KISS clients. Other existing profiles keep their current limits.
- `firmware/esp32/CloudRoomSocket`: uses the unmodified ESP-IDF4.4.7 WebSocket
  framing implementation around Aspen's trusted-time, SNI/certificate and PSRAM
  TLS admission transport. A pinned internal foundation header is needed by the
  SDK parent adapter; upgrading that SDK requires rechecking it. The wrapper
  bounds handshake/frame reads, verifies 101/subprotocol, preserves a ready
  frame coalesced with the upgrade, rejects fragments/extensions/server masks,
  and uses a mutable outgoing buffer for SDK masking. It sends no application
  pings or periodic requests; server pings receive the required pong.
- `firmware/runtime/BotHttps.cpp`: dedicated persistent TLS factory retains
  verification and the 32KiB radio heap reserve without leaving per-request
  metrics active for the connection's lifetime. The Home/Lua HTTP rate limit is
  not applied to this factory. Peer DNS runs on the network task, or an operator
  can supply a fixed numeric address while retaining hostname/SNI verification.
  The SDK's blocking DNS can delay this task; RF dispatch remains independent
  and overflow is bounded rather than building an unbounded RX backlog.
- `firmware/shared/cloudroom/CloudRoomWire`: implemented portable bounded JSON
  parser and binary/string decoders. Keep a reusable 4 KiB PSRAM frame buffer.
  Slices live only as long as that buffer. See [wire measurements](WIRE.md).
- Pinned native MeshCore `Identity`, `Utils`, packet and room helpers: expanded
  64-byte Ed25519 scalar/nonce key, ECDH, AES-128 ECB and truncated HMAC. Preserve
  the native key file, not a WebCrypto seed substitute.

The new [`internal/sharedroom`](../../internal/sharedroom) Go package is a native
protocol reference. Its `Codec` decodes authenticated ANON login, original TXT,
REQ history, PATH and ACK packets; encodes native login/path responses, signed
history and room advertisements; and binds proofs to the correct recipient key.
`Frontend` uses hibernating API sockets and one physical TX queue. `Submitter`
adapts Birch's final `SubmitWithReceipt` result. It adds no mandatory host process,
local history store or canonical membership database. There is no new Birch CLI.
This reference currently supports **unscoped RF**. Region-scoped flooding needs
payload-dependent transport authentication with the configured region key;
a static transport code is insufficient.

Two focused Go tests reuse the native C++ ciphertext/ACK fixture and exercise
native login/PATH encoding, signed delivery proofs, invalid MACs, ambiguous bare
ACK rejection and relay-loop rejection. Live ESP32/RF operation remains unchecked.

## Codec boundary and remaining integration

`CloudRoomDriver` receives opaque RF, API frames and final native receipts on
the network task, supplies immutable peers/public identities, and queues RF only
after authoritative dispatch permission. Its weak factory returns `nullptr`.
The transport/profile alone cannot connect or advertise an identity. No private
room key provider, NVS identity import, frontend crypto driver or automatic
advertisement is installed. This boundary supports an opaque Worker codec
without changing the radio scheduler/task bridge.

Provision endpoint, CA trust, frontend token and allowed room identities through
private configuration; keep tokens/expanded keys out of build logs, source and
release assets. Implement the pinned native-compatible Worker codec and opaque
RF API before enabling the approved central-key target. Provision no existing
local-room identity as part of this work.
The real frontend's public keys must match server ALIASES before advertising.

Before enabling a room, check that a companion can log in over RF, receive
catch-up through WSS and the radio scheduler, and post a message that a second
frontend receives in the same order.

## Focused validation

The optional profile compiled with MeshCore
`d92964352441e53b93e8667b802e04f6e072b39e`, PlatformIO espressif32 6.11.0,
Arduino 2.0.17 and ESP-IDF4.4.7 using public placeholder settings and WAMR off.
The disabled-driver image used 145,744 static RAM bytes and 2,017,377 flash bytes.
Those are compile sizes, not live TLS/queue heap measurements or an RF test.

`make -C firmware/shared/cloudroom test` generates its JSON fixtures and checks
bounded parsing/decoding and a host-thread SPSC transfer, including stale
generations and local RF reflection rejection. It needs Python 3 and C/C++
compilers, uses no credentials and does not contact a radio.
