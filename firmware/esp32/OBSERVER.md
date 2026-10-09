# MQTT observer output

The shared modem and Go host can send MeshCore RF receptions to an approved
internal MQTT broker using the public observer packet contract. Select the wire
format explicitly; this does not enable another broker or change a radio
identity, channel, path policy, program or role selection.

Keep deployment destinations private. No public broker presets are installed.
Do not send a node's identity credentials, packet traffic or status to a public
service without separate authorization.

## Select an output contract

Existing consumers keep their current contract by default:

| Service | Internal event contract (default) | Observer firmware | Packet-capture reference |
| --- | --- | --- | --- |
| On-device build profile | `ONCHIP_MQTT_FORMAT=0` | `ONCHIP_MQTT_FORMAT=1` | `ONCHIP_MQTT_FORMAT=2` |
| Go host `mqtt.format` | omitted or `internal-v1` | `observer-v1` | `capture-v1` |

Current Aspen source builds accept saved MQTT settings through authenticated
Management administration. Keep the radio connected to one approved local
receiver; forwarding from that receiver is a separate host service. For the
public observer contract, configure:

```text
mqtt uri mqtt://LOCAL_RECEIVER:11883
mqtt format 1
mqtt iata YYC
mqtt name My observer
mqtt commit
reboot
```

`mqtt status` reports whether configuration is saved, staged or in error.
`mqtt uri`, `name`, `iata`, `prefix`, `audience`, `format` and `filter` read the
saved fields; append a value to stage a replacement. Use `-` for an empty URI
or audience. An empty URI disables the connection. `mqtt discard` drops staged
changes. A commit takes effect after reboot and never replaces the observer
identity. The running connection continues using its startup settings until then.
`mqtt filter` is a decimal 16-bit payload-type mask: bit `N` permits native
payload type `N`. `65535` includes all types; `0` excludes all packets.

Stage `mqtt username clear|HEX`, `mqtt password clear|HEX` and
`mqtt ca clear|HEX` through **encrypted Management RF**, not the browser command
box. Hex chunks append decoded bytes; clear a field before replacing it.
Limits are 128 username bytes, 256 password bytes and 4096 CA bytes.
Do not put credentials in the saved URI. These fields are not printed by
status or read commands. TLS requires a valid PEM CA bundle.
Saved settings use two SPIFFS files and a small NVS reference; a failed
commit/readback reports an uncertain saved outcome. Reboot and inspect
`mqtt status` before retrying. A missing configuration uses the build's initial
defaults; an invalid saved reference/file disables MQTT rather than selecting
another broker.

Aspen 0.1.2 and older builds need an application update to add these commands.
Their protected build profiles use `ONCHIP_MQTT_URI`, `ONCHIP_MQTT_FORMAT` and
`ONCHIP_MQTT_IATA` as initial settings. Update the Go host binary before adding
its MQTT configuration fields; an older host may reject them.

For the Go host, set `mqtt.format` to `observer-v1`, `mqtt.iata` to `YYC`,
and `mqtt.origin` to the observer's name. Keep the existing private `mqtt.url`,
credential environment variables, broker listener and role selection. Restart
only the host when applying its configuration.
Use `capture-v1` / format 2 for a receiver expecting the packet-capture
reference's transport route and comma-separated direct path.

Read the observer key from the dashboard or host status. Public-format topics
are `meshcore/YYC/UPPERCASE_OBSERVER_KEY/packets` and `.../status`.
On-device `mqtt prefix` / host `mqtt.topic_prefix` replace `meshcore`.
The on-device exact `ONCHIP_MQTT_TOPIC` override remains available.
On-device `roles/*` records remain separate retained internal metadata.

The public packet feed contains only RF RX, not completed or uncertain local
TX and not the shared modem's SNR=-32/RSSI=127 reflection. Local submitting
identities, transmission outcomes and malformed frames remain available in
the default internal contract and the radio dashboard.
In either public host mode, those reflections and frames without usable RF
metadata are excluded from the observer's `Observed` count as well.

## Packet and presence fields

Each packet has string fields `origin`, `origin_id`, `timestamp`, `type`,
`direction`, `time`, `date`, `len`, `packet_type`, `route`, `payload_len`,
`raw`, `SNR`, `RSSI`, and `hash`.

- `type=PACKET`, `direction=rx`; `origin_id`, `raw` and `hash` are uppercase hex.
- `timestamp` is UTC ISO 8601 with an explicit zone. `time` is `HH:MM:SS`;
  `date` is `DD/MM/YYYY`. All three describe reception, not MQTT publication.
  The on-device timestamp has one-second resolution and a `.000000` fraction.
- `len` counts complete wire bytes; `payload_len` excludes the header,
  optional four transport bytes, packed path-length byte and path hashes.
- `route=F` for flood/transport-flood and `D` for direct. Observer firmware's
  dialect also reports transport-direct as `D`; capture's dialect reports `T`.
  Signals are real RF values. Observer SNR uses one decimal place; capture
  retains the RF quarter-dB precision.
  Missing/non-finite signals, invalid envelopes and unavailable UTC are dropped.
- Observer `path`, when nonempty and direct-route, is an array of lowercase hop hashes.
  Flood receptions omit this direct-route field.
  Capture includes a comma-separated lowercase path only for route `D`.
  Packed path
  modes support one-, two- and three-byte hashes; mode four is rejected.
- `hash` is the first eight SHA-256 bytes of payload-type byte plus payload.
  TRACE inserts the packed path-length as a little-endian uint16 between
  type and payload. Paths, header route and transport codes otherwise do not
  enter the hash. This uses upstream `Packet::calculatePacketHash` /
  meshcore-go `PacketHash`, not a hash of the raw frame.

The two public dialects follow those source differences explicitly rather than
changing an existing consumer's path type or transport route silently. Raw wire
bytes and packet-content hashes are identical between dialects.

Status and LWT are retained JSON with `status=online` or `offline`, `origin`,
`origin_id`, UTC `timestamp`, `model`, `firmware_version`, `radio` and
`client_version`. The LWT timestamp is connection time, not the time of loss.
On-device planned reconnect/clock suspension attempts an offline status before
stopping the client. If UTC has become unavailable, offline status uses the last
trusted timestamp rather than an invented current time.
Host shutdown publishes offline status and allows a graceful MQTT disconnect
before closing the connection, avoiding a second, older LWT status.
On-device status refreshes every minute and when the shared PHY changes;
`radio` is the current committed PHY as `MHz,kHz,SF,CR`, or `unknown`.
The Go host reports metadata as `unknown` when it has no authoritative source.
It does not substitute a configured PHY for an unverified radio.
Native role metadata remains on the separate legacy internal topics
`<prefix>/<lowercase observer key>/roles/<role>` when using static internal
broker credentials. Identity-authenticated receivers get only the public
packets/status contract, without internal role metadata.
The build profile may set `ONCHIP_MQTT_MODEL` and
`ONCHIP_MQTT_FIRMWARE_VERSION`; an unspecified version is `unknown`.

On-device public status and LWT use QoS 1 and remain retained; packets use
QoS 0, the existing bounded eight-event queue, and no application replay after
disconnect. Legacy internal packets/status remain QoS 0. Host packets/status use QoS 1 and its existing
bounded queue. Public records do not have the internal `event_id`; consumers
must allow MQTT redelivery rather than treating the packet hash as a unique
reception identifier.
The MQTT worker retains its 8 KiB stack. A shared 3 KiB serialization buffer
holds packet/status output and transient CONNECT data, keeping JWT signing
scratch off the packet-formatting stack. ESP-MQTT copies the will and credentials
at client initialization; the temporary token buffer is then cleared.
After its first successful public RF packet publication, the device prints
the MQTT-worker and radio-loop minimum free stack in bytes. Radio metadata is
copied from the caller's existing dashboard snapshot, without another large
radio-status stack allocation.

## Identity authentication and transport

Static username/password authentication remains available for the internal
broker. A successful static-password CONNECT is **not** an identity exchange.

For an authorized receiver implementing MeshCore identity authentication,
configure on-device `mqtt audience` or host `mqtt.audience`.
The audience must match that receiver's expected audience, normally its DNS
name. Do not put static credentials in a JWT URI or host JWT configuration.

The identity exchange is the MQTT CONNECT itself; there is no challenge topic
or RF advert. Username is `v1_UPPERCASE_OBSERVER_KEY`. Password is:

```text
base64url({"alg":"Ed25519","typ":"JWT"}).
base64url({"publicKey":"UPPERCASE_OBSERVER_KEY","aud":"receiver","iat":UTC,"exp":UTC+86400}).
128_HEX_SIGNATURE_CHARACTERS
```

The two base64url segments have no padding. The Ed25519 signature covers their
ASCII `header.payload` bytes. Its final segment is hex, **not** ordinary JWT
base64url. The existing observer identity signs with MeshCore's scalar-prefix
key implementation; no seed conversion, new identity or key export is needed.
Tokens renew through a fresh CONNECT five minutes before expiry. A backwards
clock correction invalidates the connection and creates fresh credentials.
Optional owner/email claims are not sent by this adapter.

An authenticated host reflector can request a broker-specific observer token
with `POST /admin/observer-token`, using `X-Mast-Session` and a plain DNS audience
in the body. The radio signs with its existing observer identity; the private
key is not exported. This route requires the selected observer role and fresh
network UTC, sends `Cache-Control: no-store`, and limits successful signing to
one request per two seconds. Tokens expire after 24 hours. Keep tokens private
and renew them before reconnecting to their broker. Use the trusted management
LAN or a protected TLS tunnel: the radio's HTTP session and returned token are
not encrypted by HTTP. The token request does not add a second radio MQTT feed.

Public on-device MQTT waits for a fresh accepted network-clock sample before
connecting. It drops packets captured before UTC was ready and disconnects
when the accepted sample exceeds twice the configured SNTP interval.
Transient snapshot contention is retried up to four times. A previously
accepted sample remains usable through short dispatch stalls, but publication
suspends after 15 seconds without a fresh clock snapshot. A packet captured
before a newer SNTP sample uses the prior accepted sample when available,
never an extrapolation into time before the first accepted sample.
Role RTC/build time cannot mint a token.
MQTT connection errors leave an actionable observer fault until a successful
connection clears it; they are not displayed as generic connection progress.
Configure host OS time synchronization before enabling its observer. The host
rejects pre-2025 UTC and suspends identity sessions on backwards corrections.

On-device URI schemes are `mqtt://`, `mqtts://`, `ws://`, and `wss://`.
Secure schemes require saved `mqtt ca` (or initial `ONCHIP_MQTT_CA_PEM`);
certificate/hostname verification stays enabled. Include the receiver's actual
WebSocket path in its URI, often `/mqtt`. Go supports `tcp://`, `tls://`,
`ssl://`, `ws://` and `wss://`, with system CA roots by default and a verified
`tls.Config` available to library callers. No insecure TLS mode is added.
Use plain MQTT only on the trusted internal network.
For an IP-addressed private receiver, its certificate must match that address.
Aspen's legacy ESP TLS transport also needs the literal host in a DNS SAN;
include the IP SAN for other TLS clients. Keep hostname checking enabled.

TLS reduces the internal heap available for source-copy workers on a full-role
image. If `source commit` reports `copy worker unavailable`, inspect the source
hash/status: the active source is retained and the upload remains staged.
Use `source cancel` if abandoning the staged upload. Review the enabled roles
and transport's memory load before retrying activation; do not erase source
or identities to free memory.

On-device `mqtt filter` is a decimal 16-bit allowed-payload-type mask, default
`0xffff`; `0` suppresses packets without suppressing status. Host
`mqtt.packet_filter` uses the same numeric mask; omitted means all types.
The observer never subscribes to remote commands or transmits an identity
advert.

## Reflect one radio feed to approved receivers

`meshcore-observer-reflector` subscribes to one observer's public-format
`packets` and `status` topics on the local MQTT broker. It verifies identity,
UTC, RF direction, signal values, packet lengths, content hash and direct path
against the raw MeshCore packet. Unknown fields and internal role topics are
rejected; neither local transmissions nor modem reflection are forwarded.
The service never opens a radio connection or accepts remote commands.

Configure Aspen's local feed with format 1, a three-letter IATA and the desired
public observer name. Retain its static local-broker credentials. Give the host
only the Management password and the observer **public** key; Aspen 0.1.3 signs
broker-specific tokens through the authenticated token endpoint above.

```sh
install -d -m 700 "$HOME/.local/libexec" "$HOME/.config/systemd/user"
go build -o "$HOME/.local/libexec/meshcore-observer-reflector" \
  ./cmd/meshcore-observer-reflector
install -d -m 700 "$HOME/.config/meshcore-observer-reflector"
install -m 600 packaging/observer-reflector.json.example \
  "$HOME/.config/meshcore-observer-reflector/config.json"
```

Edit that private JSON: absolute queue/password paths, local source URL,
credential environment-variable names, node URL, observer key, IATA, name
and explicitly authorized WSS receiver URLs. Store the existing Management
password in the named owner-only file as 1..15 ASCII bytes **without a newline**.
Use HTTPS or a protected management tunnel. Plain HTTP requires
`trusted_management_lan=true`; its session and returned tokens cross that LAN
without encryption. Remote WSS uses system CA roots and hostname verification.
Tokens remain in memory; the observer's private key never leaves the radio.

Each destination has one connection. Its URL list is ordered failover, not
duplicate publication: configure a primary and backup for the same service
under one destination. Different destinations receive the same RF receptions.
No public receiver is selected by default. The service unit reads local broker
credentials from the existing `~/.config/meshcore-mqtt/environment`; adjust its
`EnvironmentFile` for a different local broker deployment.

```sh
"$HOME/.local/libexec/meshcore-observer-reflector" -check \
  -config "$HOME/.config/meshcore-observer-reflector/config.json"
install -m 644 packaging/meshcore-observer-reflector.service \
  "$HOME/.config/systemd/user/meshcore-observer-reflector.service"
systemctl --user daemon-reload
systemctl --user enable --now meshcore-observer-reflector.service
journalctl --user -u meshcore-observer-reflector.service -n 20 --no-pager
```

Look for `local observer feed subscribed`, a retained-status acknowledgement
for each destination, then `observer RF packets acknowledged`. These are MQTT
broker acknowledgements, not confirmation that a receiver's website indexed
the messages. No packet bytes, passwords or tokens are logged.

Accepted receptions are atomically saved before forwarding. Each destination
is removed from a packet's pending list only after its QoS 1 PUBACK; restart and
broker outages retain the remaining deliveries. At most 4096 pending packets
and one latest status are kept. A full queue reports incoming reception drops;
the radio's QoS 0 local feed cannot recover traffic missed before the host
saved it. Identical pending payloads coalesce. A crash after a receiver's ACK
but before its saved acknowledgement can redeliver that reception, so receivers
must tolerate duplicates. Keep queued destinations in the configuration while
draining them; corrupt state or unknown queued destinations stop startup rather
than discarding deliveries.

Reconnect sends the latest saved status. An online source status older than
three minutes becomes offline; the service does not invent online presence
while its local feed is unavailable. Shutdown and remote connection loss send
retained offline presence. Periodic source status continues to carry the radio's
actual firmware and committed PHY.

## Developer validation

The Aspen hardware check has an opt-in private WSS receiver:
`TestAspenPrivateHardwareReceiver`. Set `MESHCORE_ASPEN_PRIVATE_VALIDATOR=1`
and `MESHCORE_ASPEN_VALIDATOR_DIRECTORY` to an owned absolute artifact directory.
Set `MESHCORE_OBSERVER_TEST_HOST` to the checker's private or loopback IP address.
Supply `MESHCORE_ASPEN_OBSERVER_PUBLIC_KEY` from Aspen's read-only dashboard.
It binds a kernel-assigned port on that address,
accepts only the configured
observer identity and `meshcore/YEG/KEY/{packets,status}`, and never forwards.
The server key remains in memory; recorded authentication data contains
claims, not CONNECT passwords. Create `stop` in that directory to close it.
Application-only test builds need its generated CA and exact endpoint/audience.
Restore the approved persistent broker build afterward; the validator is not
an operational receiver.

```sh
go test ./internal/observer ./internal/app
make -C firmware/esp32 lifecycle-test
make -C firmware/esp32 observer-public-test observer-wire-test
```

`lifecycle-test` checks the unchanged default internal contract.
`observer-public-test` exercises the real on-device observer service through
the SDK seam in both public dialects: clock gating, identity credentials, renewal, status/LWT and
RF-only output. `observer-wire-test` signs with the pinned native implementation
and passes its packets/token through a scoped loopback MQTT identity validator.
It also compares the capture dialect's fields against the pinned upstream Python
formatter, downloaded read-only and checksum-checked into `.tmp` on first use.
Go tests exercise signature/subject/audience/expiry/future-time rejection,
renewal CONNECTs, LWT, transport paths, payload hashes, filtering and local WSS
certificate trust/rejection. These tests never connect to a public receiver.

To check a configured private broker:

```sh
python3 firmware/esp32/tests/observer_internal_broker.py
```

Set `service_address` in the private operator inventory to the broker's private
or loopback IP address. The command reads the protected broker
configuration/environment from `~/.config/meshcore-mqtt`,
publishes a synthetic identity's status and transport packet to that broker,
checks received fields/hash, and deletes only that synthetic
retained status. It does not open a radio, alter services or validate that
broker's password as MeshCore identity authentication.

### Pinned primary references

- [agessaman/MeshCore observer-firmware](https://github.com/agessaman/MeshCore/tree/c2c4cb59e34c9d305a23bfc5d30f16c6fc3e30b2),
  revision `c2c4cb59e34c9d305a23bfc5d30f16c6fc3e30b2`:
  `MQTT_IMPLEMENTATION.md`, `JWTHelper.cpp`, `MQTTConnectionPolicy.h`,
  `MQTTMessageBuilder.cpp`, `MQTTPayloadBuilder.cpp`, and `MQTTBridge.cpp`.
- [agessaman/meshcore-packet-capture](https://github.com/agessaman/meshcore-packet-capture/tree/375c180d5a819e5e9262f7b989586d7847abf9c8),
  revision `375c180d5a819e5e9262f7b989586d7847abf9c8`:
  README output format, `packet_capture.py`, and `auth_token.py`.
- Native parser/hash/signing: meshcore-dev/MeshCore
  `d92964352441e53b93e8667b802e04f6e072b39e`, the on-device build pin.
  Host parser/hash/signing: meshcore-go `v1.5.0`.
