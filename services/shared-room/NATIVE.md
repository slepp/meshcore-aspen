# Direct native Aspen frontend

An Aspen ESP32 can bridge its shared MeshCore radio directly to the Worker over
Wi-Fi/WSS. The `Xiao_S3_WIO_onchip_cloudroom` profile includes the opaque driver,
transport and task bridge. Its `cloudRoomConfiguration()` provider returns
`nullptr` by default, so a public build has no credentials or cloud identities
and opens no connection. No device has been flashed by this change.

A private build supplies a `CloudRoomConfiguration`: one or two peer endpoint,
CA, token and alias records, plus matching public keys and names. Both aliases
use one physical radio source and one existing TX scheduler. Room expanded
private keys stay in the Worker. Configuration remains immutable for the
service's lifetime. Follow [private setup](TRUST.md) before enabling a provider.

`CloudRoomService` runs one network task with eight-slot RX/TX/result SPSC
queues. Dispatch-owned `LocalRadio` receives RF, filters local reflections,
submits dispatches and polls final receipts. The network task owns WSS and the
bounded `OpaqueFrontend` driver. Payload/frame/operation queues use PSRAM;
atomics, synchronization and the task stack stay in internal memory. A short
admission mutex makes disconnect invalidation atomic with physical queue
admission. Unsubmitted work from an old generation is discarded; already
admitted RF remains uncertain. Reconnect does not replay operations.

The driver validates v2 ready metadata, forwards opaque packets and accepts
opaque transmit events. Terminal receipts retain their dispatch binding and
are serialized before queued RF observations. RX overflow is best-effort and
bounded; clients recover through fresh requests. An operator action can call
`requestCloudRoomAdvertisement(alias)` to request one signed advertisement.
There is no default advertisement schedule or application keepalive.

`CloudRoomSocket` wraps the unchanged ESP-IDF4.4.7 WebSocket framing code around
Aspen's persistent TLS transport, preserving trusted time, SNI/certificate
verification and the 32KiB radio heap reserve. It bounds handshake/frame reads,
checks 101/v2 subprotocol, preserves a ready frame coalesced with the upgrade,
and rejects fragmented, extended or masked server frames. Protocol pings get
pongs. DNS runs on the network task; a fixed address can retain hostname/SNI
verification. Blocking DNS can delay that task while RF dispatch continues.
A pinned internal SDK foundation header is included for the parent adapter.

The profile reserves a sixth local radio source and two persistent alias
sockets, reducing physical KISS clients to one under the stock 16-socket SDK
budget. A one-alias profile can reserve one socket and use two KISS clients.
Other profiles retain their existing limits. The Home/Lua HTTP rate limit
(two calls per caller/four globally per minute) is bypassed by this dedicated
persistent transport.

The Worker codec supports unscoped ANON login, original TXT, directed REQ
history, PATH, bare/multipart ACK and signed room advertisements. Region-scoped
transport requires actual region-key authentication and is rejected here.
Services with independent backends use flooded history and authenticated
native PATH ACKs through every frontend. A service with one backend can use
learned direct routes across its aliases and frontends.
Native TXT signed format identifies the original author's prefix; it is not
an extra Ed25519 signature on each history message. Existing clients still use
Reset Path/relogin when moving between radios.

## Focused validation

The profile builds with native MeshCore
`d92964352441e53b93e8667b802e04f6e072b39e`, espressif32 6.11.0,
Arduino 2.0.17 and ESP-IDF4.4.7, using public placeholder settings and WAMR off.
Compile size is not a live TLS/heap or RF measurement.

```sh
make -C firmware/shared/cloudroom test
cd services/shared-room
npm run typecheck
npm test
npm run build
```

The portable command generates its own JSON fixtures with Python 3 and needs
C/C++ compilers. It uses no credentials or radio connection.

Portable checks cover parser bounds, opaque packet handling, ready identity,
final receipts under RX backpressure, local-reflection rejection and stale
connection generations. Worker fixtures cover two frontends sharing a room,
login/catch-up, native ciphertext and ACKs, ordered history, reconnect and
hibernation, including identical concurrent history from independent backends.
The Go reference uses pinned native-compatible primitives for
independent fixture generation. A live two-device RF test remains an operator
step after private configuration and deployment; there is no host daemon or
proactive history replica required by the native frontend.
