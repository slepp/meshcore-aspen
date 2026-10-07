# Direct native Aspen frontend

The target frontend is an Aspen ESP32 connected directly to this Worker over
Wi-Fi/WSS. Birch is optional reference/development code. The Worker and its
bounded wire contract are implemented; direct WSS connection and ESP32 runtime
registration remain to be implemented. No firmware release profile or hardware
has been changed for this service.

## Existing code to reuse

- `firmware/esp32/LocalRadio.h`: attach one cloud-room source to the physical
  `WifiKissMultiplexer`, receive measured RF, reject local reflections, and submit
  all alias advertisements/responses/deliveries through `queueTransmit`. Only a
  final `SUCCEEDED` result means `sent`; acceptance does not. These methods stay
  on the dispatch task because its queues are not thread safe.
- `firmware/esp32/Runtime.cpp`: add optional cloud-service startup/loop beside the
  repeater and other native roles. All aliases share that one radio source. Check
  `KISS_LOCAL_SOURCES` and native identity-presence capacity when enabling it.
- `firmware/runtime/BotHttps.cpp`: reuse the trusted-time, SNI/certificate and
  PSRAM TLS admission work through a dedicated WSS transport. Its underlying
  transport can maintain a connection; Home/Lua HTTP's call-rate limit is not
  appropriate for room operations. Existing transport expects a fixed numeric
  peer address and measures a request lifecycle, so DNS and long-lived WSS
  diagnostics need a small deliberate extension, not use unchanged.
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

Two focused Go tests reuse the committed native C++ ciphertext/ACK fixture and
exercise native login/PATH encoding, signed delivery proofs, invalid MACs,
ambiguous bare ACK rejection and relay-loop rejection. Those establish codec
behavior for the fixtures; live ESP32/RF interoperability is still outstanding.

## Next runnable milestone

Add an optional native C++ cloud-room service with one network task and bounded
RX/TX/result queues. The dispatch task drains measured RF into the network task
and is the only task touching `LocalRadio`. The network task owns native decoding,
API calls and WSS reconnect. It queues a native response only after `respond:true`,
and submits a history packet only after `prepare` returns `transmit:true`.
One connection per advertised alias follows the existing small API. Budget each
TLS connection before opening it and keep the radio heap reserve; aliases can
be capped to the configured device budget without adding transport multiplexing.

Provision endpoint, CA trust, frontend token and allowed room identities through
private configuration; keep tokens/expanded keys out of build logs, source and
release assets. Enable the feature explicitly, defaulting off, and reuse the
existing native key storage rather than changing the active local-room identity.
The real frontend's public keys must match server ALIASES before advertising.

The demonstrable outcome is one Aspen logging a client into an alias, receiving
catch-up through WSS, submitting its physical RF response via the existing
scheduler, and committing a post that a second frontend sees in the same order.
A subsequent scoped deployment/flash decision can use that concrete build;
there is no extra release ceremony or history-replication subsystem.
