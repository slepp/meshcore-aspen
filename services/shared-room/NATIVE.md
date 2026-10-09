# Direct native Aspen frontend

Aspen's generic `public_aspen` application bridges its shared MeshCore radio
directly to the Worker over WiFi/WSS. It includes native roles, Lua/Wasm, HTTPS
and the room frontend in one image. Install the
[private initial setup](../../firmware/esp32/PUBLIC_SETUP.md), then save one or
two room aliases through authenticated Management. Unconfigured frontends stay
disabled. Both aliases use one physical radio source and its TX scheduler;
the Worker holds their private room keys.

## Configure the generic image

Create the frontend token and room aliases using [private service setup](TRUST.md).
Keep an owner-only JSON file outside the repository:

```json
{
  "schema": 1,
  "enabled": true,
  "aliases": [{
    "host": "rooms.example",
    "address": "",
    "ca": "-----BEGIN CERTIFICATE-----\nYOUR_CA_PEM\n-----END CERTIFICATE-----\n",
    "token": "YOUR_FRONTEND_TOKEN",
    "id": "YOUR_ALIAS_ID",
    "name": "YOUR_ROOM_NAME",
    "public_key": "YOUR_ROOM_PUBLIC_KEY_64_HEX_DIGITS"
  }]
}
```

Replace the placeholders, including the complete PEM CA. Every field is
required. Add a second object for another alias; use a distinct public key
and endpoint/alias pair. Limits: hostname 127 bytes, optional IPv4 address
15, PEM CA 4096, token 256, alias ID 64, room name 32. IDs contain letters,
digits, `_` or `-`. Public keys are lowercase 64-digit hex. The hostname
has no scheme, port or path: transport is WSS on port 443 with
`/v1/aliases/ID/socket`. An empty `address` uses DNS; a fixed IPv4 address
keeps hostname/SNI and certificate verification.

```sh
chmod 600 "$HOME/.config/aspen-private/cloudroom.json"
python3 tools/hardware/admin.py \
  --gateway COMPANION_HOST --target MANAGEMENT_PUBLIC_KEY64 \
  --seed-file "$HOME/.config/aspen-private/admin-companion.seed" \
  --password-file "$HOME/.config/aspen-private/mast-password" \
  cloudroom configure "$HOME/.config/aspen-private/cloudroom.json"
```

`COMPANION_HOST` is your companion radio's KISS endpoint; use `--port` if it
does not listen on the tool's default port 8001. Supply its saved companion seed
and Aspen's Management public key. Credential uploads require encrypted
Management RF. Web administration can inspect configuration, save enable
changes or retain an existing private provider. Tokens remain in the private
file and encrypted upload, not command-line arguments or replies.

The helper validates the profile locally, uploads numbered 48-byte chunks and
checks its saved hash. A lost chunk reply can repeat that exact chunk. A lost
commit reply reads the hash without replaying the commit. `cloudroom status`
reports the current connections; `cloudroom config status` reports the saved
selection. **Configuration and enable changes apply after restart.** Restart
with the ordinary authenticated `reboot` command when ready.

For first boot on a blank board, `public_setup.py image --cloudroom-profile PATH`
includes the same record in the private setup image. On an initialized node,
use `cloudroom configure`; do not replace its filesystem.

| Management command | Parameters and result |
| --- | --- |
| `cloudroom status` | Current alias count, reserved sockets, connected WSS bitmask and pending advert bitmask |
| `cloudroom error` | Last connection error and minimum network-task stack remaining |
| `cloudroom advertise ALIAS` | Exact active alias ID; queues one room advert |
| `cloudroom config status` | Saved/enable/alias state, live alias count and upload state |
| `cloudroom config hash` | SHA256 of the saved record's content, or `none` |
| `cloudroom enable on\|off` | Saves the enable state; restart applies it |
| `cloudroom config api` | Upload ABI, record size, chunk size and application timing |
| `cloudroom config begin SHA256` | Full 64-digit lowercase content hash; same hash resumes this boot's upload |
| `cloudroom config chunk ID16 INDEX HEX` | Hash prefix, zero-based index, exactly 48 bytes except the final chunk; identical repeats accepted |
| `cloudroom config commit ID16` | Validates the complete record/CA, writes the spare file and publishes its selector |
| `cloudroom config abort` | Discards staging; leaves the saved configuration and live connections intact |
| `cloudroom config retain` | Saves a private build's running provider for a generic application update |

All these commands require direct authenticated Management RF or Web
administration; upload begin/chunk/commit additionally require encrypted RF.
Bot/source invocations are denied. A restart discards an
unfinished upload; begin it again from the private file. Invalid fields, CA or
incomplete writes leave the saved selection unchanged. An uncertain selector
write requires `config hash`/`config status` before retry. Damaged saved records
disable the frontend at boot and report an error; restore them from the private
node backup. NVS identities, role files and bot/packet programs are retained.
Encrypted node backups include the frontend settings and credentials.

## Move a private frontend build to the generic image

Build and application-update the private frontend with current source first,
keeping its existing configuration provider. Run `cloudroom config retain` and
`cloudroom config status` alongside the
[setup migration commands](../../firmware/esp32/PUBLIC_SETUP.md#move-an-initialized-private-build-to-a-generic-application).
Then application-update to `public_aspen`. The generic image loads the saved
frontends after restart; it uses the existing authorities, identities and
filesystem. A private provider still uses its compiled configuration until that
application update.

Custom applications can supply a `CloudRoomConfiguration` instead of saved
settings. The private `ONCHIP_CLOUD_ROOM_CONFIG_HEADER` may define the strong
`onchip::cloudRoomConfiguration()` provider; `CloudRoomConfig.cpp` includes it
in the `Xiao_S3_WIO_onchip_cloudroom` profile. Keep that header and the resulting image private.
The `Xiao_S3_WIO_onchip_cloudroom_probe` profile accepts the existing sealed
operator header path rather than putting credentials in compiler arguments.
The offline `https_profile.compile_image()` helper accepts that profile and a
sealed provider descriptor through `profile` and `cloud_config_fd`. Create the
descriptor with `memory_header()` and close it after compilation. The provider
contains the frontend token, CA and public alias metadata, not room private
keys. This build helper does not read or create a node backup.

After restart, inspect the connections and explicitly request an advert:

```text
cloudroom status
cloudroom error
cloudroom advertise ALIAS
```

`aliases` reports configured aliases, `wss-mask` marks connected WSS sockets,
and `advert-pending` marks explicit requests waiting for the network driver.
The companion receives the room advert over RF. Startup and status reads
do not request advertisements.

If `wss-mask` stays zero, `cloudroom error` reports the last failed connection
attempt. TLS errors name the failed phase and report the SDK code and available
internal heap; upgrade errors report the HTTP status and whether the v2
subprotocol was returned. HTTP 401 means the frontend token was rejected.
`stack-min` reports the network task's minimum remaining stack in bytes after a
connection attempt. No token, certificate contents or response body is included.

`CloudRoomService` shares Aspen's existing 16KiB HTTPS task with telemetry and
configured HTTP calls, rather than allocating another task stack. Each bounded
room poll follows pending HTTP work; a TLS reconnect can delay other network
work up to the existing connection timeout while radio dispatch continues.
The service has eight-slot RX/TX/result SPSC
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

The generic image reserves a sixth local radio source and two persistent alias
sockets, leaving one physical KISS connection under the 16-socket SDK budget,
two companion clients and two live dashboard viewers. This reservation stays
the same when the frontend is disabled. MKISS carries several logical ports on
that one KISS connection. Custom one-alias builds can reserve one socket and use
two KISS clients. The Home/Lua HTTP rate limit
(two calls per caller/four globally per minute) is bypassed by this dedicated
persistent transport.

The Worker codec supports ANON login, original TXT, directed REQ history, PATH,
bare/multipart ACK and signed room advertisements. Unscoped routing is the
default. Optional public `FRONTENDS[id].region` validates scoped routes 0/3
and scopes outgoing floods; the native driver forwards those bytes unchanged.
Configure the companion's outgoing scope explicitly. Public regions control
flood routing; ordinary direct packets remain native-compatible. Private `$`
regions require explicit keys and are rejected. See [regional setup](README.md#public-regional-routing).
Services with independent backends use flooded history and authenticated
native PATH ACKs through every frontend. A service with one backend can use
learned direct routes across its aliases and frontends.
Native TXT signed format identifies the original author's prefix; it is not
an extra Ed25519 signature on each history message. Existing clients still use
Reset Path/relogin when moving between radios.

## Focused validation

The generic image builds with native MeshCore
`d92964352441e53b93e8667b802e04f6e072b39e`, espressif32 6.11.0,
Arduino 2.0.17 and ESP-IDF4.4.7, with Lua 5.5.1 and WAMR 2.4.1.

```sh
make -C firmware/shared/cloudroom test
make -C firmware/esp32 cloudroom-preferences-test
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
The Go reference generates native packet fixtures. After deploying a configured
frontend, check adverts, login, posting and history from a companion radio.
