# Go host and shared modem guide

Run a repeater, room server, companion/base and packet observer through one
Seeed XIAO ESP32-S3 with a WIO-SX1262 radio. Add a bot through its own KISS or
companion connection. The roles keep their own identities and messages while
sharing the same LoRa channel and airtime. Other MeshCore radios on the same
radio profile exchange adverts, messages and routed packets with them over
the air.

Use a WiFi/TCP modem to run selected roles on a Go host. The Birch UART
modem connects wired queued-protocol clients. You can also connect a
[serial or TCP packet monitor](#connect-a-host) without running the roles.
Start with the [setup chooser](README.md) if you have not selected a device
and role placement. This guide covers host configuration, identities, bot
connections, installation, radio maintenance and external KISS clients.
For standalone deployment, use the [ESP32 guide](firmware/esp32/README.md)
or [nRF52840 guide](firmware/nrf52840/README.md).

Host roles use the **Birch 0.1.0** product version. Their `ver` command reports the
MeshCore protocol/reference separately from the host implementation; companion
and native owner-information replies identify `birch-0.1.0`.
Use `help`, `help get`, `get radio` or `get freq` in a Relay/Room console.
The Birch 0.1.0 download contains the Linux x86_64 host tools and native
worker. Connect an already configured WiFi/TCP shared modem implementing queued
PHY v1, or build and privately provision the modem separately. The download
does not contain a modem image or station credentials. See the
[release guide](release/README.md) for compatibility and source builds.

Configure WiFi on the external modem. See the
[app endpoint guide](firmware/shared/ANDROID.md#choose-the-endpoint-for-app-settings).

## Find a task

- [Install the Birch host download](#install-the-birch-host-download).
- [Connect the public Birch UART modem](#connect-the-public-birch-uart-modem).
- [Build a WiFi modem and start the host](#start-with-a-wifi-radio-and-go-host).
- [Configure role placement and shared PHY authority](#configure-the-go-host).
- [Run native Lua and use its private owner socket](#host-status).
- [Open the authenticated host administration page](#host-browser-administration).
- [Understand identities, persistence and key changes](#state-compatibility-and-credentials).
- [Connect an external bot](#bot-connection).
- [Install the host service](#install-and-check).
- [Back up, upgrade or restore a host and modem](#upgrade-and-rollback).
- [Build/flash the WiFi modem](#build-and-flash).
- [Connect KISS tools](#connect-a-host) or [an ESP32 role with a remote modem](#esp32-roles-with-a-remote-modem).

Commands below run from the repository root. Choose legal local settings and make
every participating radio match frequency, bandwidth, SF and coding rate.

## Install the Birch host download

Use Debian 12 on Linux x86_64 and an existing configured WiFi/TCP shared modem.
The host needs `libcjson1` and `libssl3`; installing the download does not need
Go, a compiler or PlatformIO. Python 3.11 or newer runs the installer.

Download the ZIP, manifest and checksums from
[Birch 0.1.0](https://github.com/slepp/meshcore-aspen/releases/tag/birch-v0.1.0)
into a new directory. Extract the ZIP there, then run `sha256sum --check` on the
downloaded SHA256SUMS file: both the ZIP and extracted manifest must match.
Replace `SOURCE12` below with the suffix of the extracted directory.

```sh
python3 birch-v0.1.0-linux-x86_64-SOURCE12/install-birch.py \
  --prefix "$HOME/.local/opt/birch-0.1.0"
```

The installer refuses an existing prefix. It installs four binaries and a
private example configuration, but does not start services, connect to the
modem or change an existing host's identities, state or configuration.
Keep the source and native relink archives from the download.

Edit `~/.local/opt/birch-0.1.0/config/meshcore-host.json`: set
`radio_address`, select `enabled_roles` for the modem's available client slots,
and set the room password environment variable if enabling the room.
The installed example selects the bundled native Lua worker and follows the
modem's PHY without retuning it. Keep all listeners on trusted interfaces.

```sh
export MESHCORE_ROOM_PASSWORD='choose-a-private-room-password'
~/.local/opt/birch-0.1.0/bin/meshcore-host \
  -config ~/.local/opt/birch-0.1.0/config/meshcore-host.json -check
~/.local/opt/birch-0.1.0/bin/meshcore-host \
  -config ~/.local/opt/birch-0.1.0/config/meshcore-host.json
```

`-check` validates configuration without opening the radio or creating
identities. After starting the host, `http://127.0.0.1:9080/status` shows the
connection and selected roles; a MeshCore companion application connects to
the companion listener on port 5000 when that role is enabled.
Do not point a new installation at an existing host's state directory as an
upgrade procedure; use [retained-state upgrades](#upgrade-and-rollback).

## Connect the public Birch UART modem

An existing UART shared modem is for wired clients implementing the queued TX
contract below. It is not the WiFi/TCP modem used by the Go host setup, and the
Birch host download does not flash or configure it.

Connect a **3.3 V** USB-UART adapter to the XIAO ESP32-S3: adapter TX to
**GPIO44/D7 (modem RX)**, adapter RX to **GPIO43/D6 (modem TX)**, and common
ground. Use **115200 baud, 8N1**, without hardware flow control.
The board's USB port carries diagnostic output; the pins above carry KISS.

Use a client implementing the [queued TX contract](firmware/shared/QUEUED_TX_PROTOCOL.md)
over the UART connection. Each client negotiates a source and acquires the
owner lease for radio configuration. Choose legal local LoRa settings before
enabling RF clients.

For several roles on a Go host, follow
[WiFi modem and host setup](#start-with-a-wifi-radio-and-go-host).

## Start with a WiFi radio and Go host

Build the WiFi KISS firmware for the XIAO S3/WIO-SX1262. Set your WiFi
credentials in the generated configuration and flash the selected radio:

```bash
make firmware-config
$EDITOR firmware/platformio.local.ini
make firmware
make firmware-upload UPLOAD_PORT='/dev/serial/by-id/<radio-device>'
```

The radio serves KISS on port 8001 and its dashboard at
`http://meshcore-radio.local/`. If mDNS is unavailable, use the IP address
printed on the radio's USB serial console. Keep these services on a trusted
network; [network access matters](#network-safety).

Configure and start the Go applications:

```bash
make host-config
$EDITOR meshcore-host.json
make host-run
```

Set `radio_address` to the radio's address, choose the shared LoRa profile and
change the example room password. The example uses **910.525 MHz, BW 62.5 kHz,
SF7, CR 4/5**. Open `http://127.0.0.1:9080/status` for role and radio status,
or connect a MeshCore companion application to `HOST_IP:5000` on your trusted
LAN. [Go host configuration](#configure-the-go-host) covers identities, policies,
MQTT and the bot connection. Building firmware requires Git, Python 3 and
PlatformIO; building the host from source requires Go. Running the
[host download](#install-the-birch-host-download) does not require Go.

For the official Android app, choose **Connect → WiFi**, enter the companion
address and port **5000**, and enable **Auto Reconnect**. Do not use the KISS
port 8001. See [Android companion connections](firmware/shared/ANDROID.md) for emulator
addresses and native role access.

## Run the roles on the radio

For roles that must keep working without a host, use the
[standalone ESP32 guide](firmware/esp32/README.md#choose-an-image).
It covers the distinction between native/core-bot and durable beta/HTTPS
images, private configuration, destructive first-time filesystem provisioning
and ordinary updates. If a Go host also joins that mast, use modem PHY
authority and count extra role/client sockets as described below.

## How the shared radio works

The ESP32 controls LoRa tuning, transmit order, carrier access and the shared
airtime budget. With the Go host, repeater, room and companion each have their
own application state and identity. The MQTT observer receives packet events;
companion applications have separate sessions on the same base identity.
Transmitted packets also reach the other local roles, marked as local delivery
so they are not confused with a measured RF reception. Changes to physical
settings affect every role, and incompatible requests fail explicitly.

For an ESP32 running a role away from the radio, the
[remote-radio adapter](#esp32-roles-with-a-remote-modem) connects over WiFi TCP.
The standalone image runs the roles on the ESP32 itself.

## Configure the Go host

The host uses `meshcore-go` v1.5.0 for MeshCore wire protocols and
cryptography; its Go dependencies are pinned in `go.mod` and `go.sum`. It
includes an MQTT broker for the observer.

The default shared PHY is **910.525 MHz, BW 62.5 kHz, SF7, CR 4/5, TX 22 dBm**.
All roles use the same tuning. Startup advertises the repeater, room and base;
the observer is receive-only and identifies its MQTT feed rather than advertising
another on-air node.

The host enables SX1262 channel activity detection (CAD) by default, with an
airtime factor of 1 and interference threshold of 0. CAD defers transmission
when it detects channel activity; role TX/RX delays and randomized carrier retries
still apply. An explicit `phy_profile` with `"cad_enabled": false` retains that
choice. With modem PHY authority, the host follows the radio's saved CAD setting;
use `get cad` and `set cad on` in Aspen's authenticated management console to
inspect or change it.

The example host configuration sets `"require_parity": true`: the modem must
support negotiated queued transmissions, or startup fails explicitly. The
observer connection owns shared configuration; the other role connections
join without retuning. `"require_parity": false` selects the older compatibility
path. Build and flash the modem to apply firmware changes.

`enabled_roles` selects the host's `repeater`, `room`, `companion`, `observer`
and KISS `bot` services. Omit it to retain all five, or use `[]` for a radio
configuration link and status endpoint only. If `observer` is disabled, that
link stays up without starting MQTT. A disabled role does not bind its
listener, open a radio source or activate a staged identity. The separate
`bot_companion_listen` remains independently opt-in.

For a standalone mast radio supplying the PHY, set `"phy_authority": "modem"`
and keep `"require_parity": true`. By default, `"phy_tracking": "follow"`
lets the host read the mast's effective settings at connection, on heartbeat
and after a stale submission, without issuing CONFIG SET. The host uses those
settings for airtime and reception scores as the mast changes its profile or
returns from a temporary one. If the host must require its configured
frequency, bandwidth, SF, CR, power and PHY policy instead, set
`"phy_tracking": "fixed"`; a mismatch prevents that link from joining until
the mast returns to the configured profile. Select the roles you intend to
run on the host; each built-in mast role
can run once, and independent KISS clients can still use the same radio. The
mast has four external KISS slots, shared by the controller link, selected role
sources, optional bot companion and reserved KISS bot clients. The host queries
CAPACITY at startup; for older firmware without that query, set
`"radio_client_capacity": 4` explicitly. Leave `"phy_authority": "host"` for a
radio-only deployment; it keeps the previous owner-configures-PHY behavior
and eight-slot default. You can run a role on both the mast and host, including
while moving it from one to the other. Watch for duplicate forwarding and
extra airtime when both repeaters are active. The host reports overlapping
roles or identities in its log and `/status` `role_presence` entry; it does not
disconnect them. Ordinary KISS clients do not need to announce a role.

The example sets `"radio_session": "auto"`: it uses one negotiated MKISS
connection for the controller and up to three selected roles when the radio
supports it, falling back to separate links only if those links fit.
`"required"` refuses startup without sufficient MKISS ports; the default
`"per_role"` retains separate connections. The reported logical-port count
includes controller port 0. Additional roles (including an optional bot
companion) and KISS bot clients still need direct radio sockets. For example,
controller, repeater, room and companion fill a four-port session; a bot
companion plus two bot clients require **three additional physical sockets**:
one bot-companion socket and two bot-client sockets alongside the aggregate
session. That is four physical sockets, not one. The host checks the radio's
reported capacity before opening role links and will not silently omit a role
or replay an uncertain transmission after a reconnect.

The example configuration separates `repeater_policy`, `room_policy` and
`companion_policy`. These contain explicit overrides, not copies of every
default: omitted fields preserve saved preferences, while explicit zero can
disable an advert schedule. The example selects `path_hash_mode: 2` (three-byte
originated paths); the library default remains mode 0 (one byte). Local and flood
adverts have independent schedules. The deprecated `advert_interval_seconds`
is a flood-only override, used only when that role has no explicit
`flood_advert_seconds`. It no longer supplies an implicit one-hour override.
Repeater/room native defaults include a 120-second local interval and a
169,200-second flood interval.

Native TRACE is a **different path format** from these ordinary originated
paths: TRACE flags 0/1/2/3 select 1/2/4/8-byte hashes, respectively.
`path_hash_mode: 2` still originates three-byte ordinary paths, but does not
enable a three-byte TRACE. Trace clients must reject a requested three-byte
TRACE width explicitly, not silently substitute two or four bytes. The host
companion accepts native TRACE requests without rewriting their flags; this
does not imply that the separate on-chip Lua bot supports every native TRACE
width.

For a separate application using the standard companion TCP protocol, set
`"bot_companion_listen": "127.0.0.1:8106"` (disabled when omitted). The host
then starts `bot_companion` with its own persistent identity, application
document, radio source and lifecycle; the base at `companion_listen` and
existing KISS proxy keep their identities and endpoints. Its default name is
`"Shared Radio Bot"`; `bot_companion_name`, `bot_companion_policy` and
`bot_companion_retention` configure it independently. Setting `companion_policy`
does not configure `bot_companion`; use its own `bot_companion_policy`.
The example explicitly sets both to
`path_hash_mode: 2` so enabling the bot endpoint also uses three-byte paths.
Only its companion
reboot is enabled by default; opt-in `bot_companion_key_import` permits native
64-byte scalar-prefix key import to this role only for identity migration.
Import can be enabled only with `bot_companion_listen` bound to `localhost`,
`127.0.0.1` or `[::1]`. The companion protocol does not authenticate local
clients: other local processes with access to that listener can invoke import
while it is enabled. Disable the flag after the intended migration; factory
reset and private-key export remain disabled. Imported identity and application
state commit together under the state lock; other role identities, including
the KISS proxy's, stay independent.
The optional source joins without claiming physical configuration ownership.
It occupies one of the eight modem slots: with four built-in sources, its
source and the default two KISS proxy clients use seven. When enabled,
`bot_max_clients` is capped at three, leaving no spare slot at that maximum.
The host rejects shared local TCP ports before starting. `/status` includes
`bot_companion` with a distinct public key, endpoint and application lifecycle;
`/readyz` checks that role only when enabled.

An optional complete `phy_profile` uses `airtime_factor`, `cad_enabled` and
`interference_threshold`. Omitting the profile uses factor 1, CAD disabled and
threshold 0. Factor 0 is a real 100% refill setting, not a default sentinel;
factor 99 limits aggregate refill to 1%. This is separate from each role's
additional source limit.

The ESP32 commits the physical profile to NVS and restores it before serving
clients after a reset. Reconnecting host links verify that profile instead of
retuning it. The host also captures the modem's airtime estimates once at
startup and shares the lookup across roles; it does not substitute the Go
library's differently rounded estimate. The observer entry in `/status`
exposes this shared `airtime_ms` array, indexed by packet length.
`make test-host-airtime HOST_CONFIG=...` compares the running host's timing
inputs with direct modem queries without transmitting.

| Service | Default endpoint | Behaviour |
|---|---|---|
| Companion/base | TCP `HOST_IP:5000` | Standard MeshCore companion protocol; up to 32 concurrent applications share one base identity |
| Bot KISS | TCP `127.0.0.1:8105` | Independent persistent modem-side identity, crypto and actual PHY telemetry |
| Observer MQTT | TCP `127.0.0.1:1883` | Built-in broker; packet events and retained availability |
| Status | HTTP `127.0.0.1:9080/status` | Public identities, actual radio connectivity and cached physical readings |

The repeater forwards real RF traffic. The room implements password login,
membership, persisted history, signed delivery, acknowledgements and retries.
The base implements encrypted private/channel messages, contacts, channels,
adverts, room sessions, path learning and acknowledgements. Repeater, room,
base, observer and bot each have a separate persistent seed. The observer
does not transmit; an external bot application still owns its own on-air
identity and behaviour.

The radio owns arbitration and sender-excluding local reflection. Four physical
TCP connections belong to the built-in roles; bot connections are opened on
demand (two by default). This leaves two of the modem's eight slots available
for a direct monitor or other clients. Multiple companion applications share
the base's one physical connection and do not consume additional modem slots.
Locally reflected traffic is delivered to applications but is not forwarded
again by the colocated repeater.

### Host status

`GET /readyz` returns 200 while the applications, radio sources, listeners and
MQTT publisher are ready; otherwise it returns 503 with per-role reasons.
`/status` shows application, modem and observer details, including each role's
`state`, `application_started_at` and `application_error`. Monitor packet loss,
airtime limits and network outages alongside readiness.
By default, `bot.application_kind: "kiss_proxy"` and optional
`bot_companion.application_kind: "companion_endpoint"` describe the actual
running services, not a native Lua command bot. An opt-in production bot
reports `bot.application_kind: "native_lua"` after the shared native
CommandBot/Lua worker has restored its selected source and verified the
existing bot identity. The `kiss_proxy` and `native_lua` backends are mutually
exclusive for that identity; the separate `bot_companion` identity remains
independent.

#### Host browser administration

Open `http://127.0.0.1:9080/admin` after an operator enables one of these
authentication choices through the service's protected environment:

- **Reuse the configured role administrator password:** set
  `MESHCORE_HOST_ADMIN_HTTP=1`. The host uses the environment variable already
  named by `admin_password_env`; no new credential or host JSON edit is needed.
  The password must contain 12..256 printable, non-whitespace ASCII bytes.
  A missing/short password prevents startup rather than enabling weak login.
  This opt-in grants its holders native bot owner access, including source and
  private scoped data. Do not reuse a password shared with role-only operators
  who should not receive that additional authority.
- **Separate owner token:** configure
  `"admin_http_token_env": "MESHCORE_HOST_ADMIN_TOKEN"` and provision a random
  32..256-byte printable ASCII token in that variable. This choice takes
  precedence over password reuse. Machine clients may use that token directly
  in the authenticated header; the browser exchanges it for a session.

Without either opt-in, `/admin` stays disabled. Activation requires an
operator-controlled service restart. Do not put credentials in URLs or commit
them in configuration. Keep the loopback listener or use a trusted TLS tunnel;
loopback is not authentication. Plain HTTP exposes the login credential and
session; interception of a reused password also compromises its RF roles.

Enter the configured password/token in the page and choose **Connect**, then
**Refresh host monitoring**. The credential is sent once and cleared from the
input; subsequent requests use a random ten-minute session kept only in this
tab. There are four session slots and five login attempts per minute. Logout
revokes a session; expiry requires login again, without discarding the draft
or uncertainty fence. The authenticated status
view shows every running host role and its readiness-related diagnostics.
Native bot controls appear only with `bot_runtime: "native_lua"` and an enabled
bot. Requests need both an authenticated session/token and explicit admin intent; browser
requests must have the same origin. The native bot adapter connects only to the existing
same-UID `<state_dir>/bot/native/admin.sock`; it does not invoke a shell or
write host configuration. It requires a private socket owned by the host service
user and checks the connected server's Unix peer UID before sending a command.
Symlinks and sockets accessible to other users are rejected.
Bot-console `role config` and `role name` queries are limited to the native bot;
use the separate room/repeater controls for Go host roles.
When the host supplies running repeater/room owner adapters, a separate
role section is enabled independently of the bot runtime. Choose the role,
read its name/policy/radio, then explicitly save a name or forwarding policy.
These changes use the running role's native commands and persist after restart.
The scoped console also supports native path-hash mode and local/flood advert
interval setters; native validation errors and units remain authoritative.

| Task | Host `/admin` | Other owner interface |
| --- | --- | --- |
| Monitor roles, radio connectivity, observer/MQTT and bot state | Authenticated status snapshot | `/status`, `/readyz` |
| Running repeater/room name and repeat policy | Guided native owner controls; name preserves identity and both settings persist | Native app/role CLI |
| Repeater/room path-hash mode and advert intervals | Scoped role console using existing owner helpers; `get`/`stats`/help read back settings and telemetry | Native app/role CLI for other supported settings |
| Native bot name, shared/home/reminder/channel-wait/discovery grants | Guided controls; durable native helpers | Native owner CLI |
| Lua source up to 4096 UTF-8 bytes | Explicit resumable upload, then verify/install exact snapshot; editor edits stay separate | Owner CLI for verified source download, larger supported packages and recovery |
| Scoped KV/timer/reminder transfer | Allowlisted native `data` console; errors and scopes preserved | `admin.py` scoped file export/stage/restore for verified backups and no-rearm |
| Whole-host identities, configuration, credentials, programs and data | Encrypted **Node backup** download, independent of the native bot | [Paced encrypted RF retrieval](NODE_BACKUP.md) through a running repeater/room's administrator |
| Host role placement, listener/MQTT configuration and PHY authority | Monitoring only; no configuration overwrite or restart button | Operator host configuration and explicit restart |
| Chats, contacts, channels and role-specific settings | Not a chat client | Native MeshCore app on the companion endpoint, or permitted role owner CLI |
| Wasm packages, credentials, private identity/channel keys | No imports through this page | Protected native owner CLI / supported role transport |
| Radio WiFi provisioning, physical sensors or radio firmware | Belong to the companion/shared modem, not the host | That radio's supported administration/update interface |

A lost write reply blocks subsequent writes in this tab. Inspecting an
unrelated idle status does not unlock it. Source installation unlocks only
after a matching durable hash and terminal source status; other uncertain
writes require scoped owner readback before deciding on another operation.
Role inspection retains the affected role and setting even if the selector
changes; only an exact native `get` response unlocks that write. Error,
incomplete or different-role readbacks keep the fence. Values normalized by
native policy may require inspection through the owner CLI rather than
assuming the requested spelling was applied.
There are no automatic write retries. Forgetting the session retains the draft
and uncertainty fence; download unsaved source before closing the tab.

The host limits header/body reads to five seconds and response writes to fifteen
seconds. An incomplete body is rejected without submitting an owner command.
The native owner operation has its own eight-second deadline; a lost reply after
submission still requires scoped readback before another write.

#### Native worker setup

Build the native worker with `make -C firmware/esp32 bot-native-worker`,
then set `bot_runtime` to `"native_lua"` and `bot_native_worker` to the
**absolute path** of that built executable. For a stable service path, use
`make -C firmware/esp32 bot-native-worker-install`, which installs it at
`~/.local/libexec/meshcore-bot-native-worker`; use its expanded absolute path
in the configuration. `make host-install-binaries` updates host executables
without replacing the existing service unit or configuration.

Native-worker builds default to `ONCHIP_BOT_WASM=1`, allowing one Wasm package
alongside Lua under the same bot identity and grants. To omit the interpreter,
pass `ONCHIP_BOT_WASM=0` to the worker build/install targets. Keep
`bot_runtime: "native_lua"` for either build; the configuration name is
unchanged. See the [Wasm SDK and package workflow](firmware/runtime/WASM_RUNTIME.md)
for runtime selection, limits and the remaining physical checks.

The native bot also runs configured HTTPS weather/service, approved
HTTP GET/POST and named RPC commands through the same Lua API as the ESP32.
See [configured network setup](firmware/runtime/NETWORK_API.md) for local owner
socket configuration, TLS requirements and expected-hash package fetch.
Linux HTTPS requires a kernel-synchronized clock, not merely a plausible date.
Use the private owner socket's `home` command to inspect `clock=0|1`; repair
host time synchronization before enabling the network grant when it is `0`.
The endpoint configuration has four slots: reuse `home` for named RPC when
weather, HTTP status/echo and package fetch must coexist.

The default `kiss_proxy` keeps
the existing bot TCP listener and its configured client allowance. Native
mode instead reserves one real radio source, consumes no bot KISS clients,
and requires queued PHY parity. On a four-logical-port session, port 0
belongs to the controller; excess roles, including the native bot, require
their own available physical sockets. The mast alone schedules airtime,
priority and CAD. RF submission is not reported as transmission: native
commands receive correlated final PHY outcomes, and an uncertain outcome
is never replayed. The native `!air` command reads fresh source and
aggregate queue/credit/RF counters from the mast; stale or unavailable
snapshots are not fabricated. An effective PHY change stops the worker and requires
a supervisor restart with a new verified airtime table. There is no
per-bot retune of the shared radio. A new host bot defaults to
`path_hash_mode=2` (three-byte hashes), matching the deployed roles; its
native saved preference remains authoritative thereafter.

An independent companion radio can exercise the host bot over RF using the
installed upstream CLI:
`make stock-cli STOCK_SERIAL=/dev/serial/by-id/COMPANION STOCK_CLI_ARGS='infos'`.
Use `STOCK_CLI_ARGS='script /absolute/path/to/commands.meshcli'` for a command
sequence. This target does not provision or flash the peer; its script controls
any changes. Advertise the sending companion after a bot restart so the bot
can learn its full public key before receiving encrypted DMs.

For private operator management, the active native bot creates
`<state_dir>/bot/native/admin.sock` (0600 under a 0700 state directory).
Only a same-UID local Unix peer can submit one newline-terminated ASCII
command (up to 160 bytes) per connection. The existing `admin.py` accepts
`--unix-socket /absolute/state/bot/native/admin.sock` for the same source
install/update/download operations, scoped data exports/restores and individual
management commands:

```sh
make -C firmware/esp32 mast-cli \
  ARGS="--unix-socket $HOME/.local/state/meshcore-host/bot/native/admin.sock command 'source status'"
```

Native commands include
`source status`, `source hash`, `source begin ID16 SIZE SHA256`,
`source chunk ID16 INDEX HEX`, `source commit ID16`, `source rollback`,
`source remove`, `name NAME`, `channel off`, and
`channel NAMEHEX KEY32`. `advert.zerohop` requests a signed, pathless bot
advert with the saved name and unchanged identity while its durable source
is ready; the private reply reports queueing, not RF success. The verified
same-UID Unix peer is host owner authority for this command: it may announce
a rename immediately after startup through the native owner-only bypass,
limited to one accepted owner advert per minute per worker process and
subject to the ordinary radio queue and airtime gates. Public bot adverts
(including Lua) still obey the 15-minute interval; this does not change
on-chip RF-role authorization or delegate shared-PHY controls.
`path status` reads the saved hash mode;
`path 0`, `path 1` or `path 2` saves a mode for the next worker restart.
`adaptive` reads opt-in bot congestion admission; `adaptive on` or `adaptive off`
saves the selection for the next host restart. It defaults to off. Use
`make -C firmware/esp32 mast-cli ARGS="--unix-socket /absolute/state/bot/native/admin.sock command 'adaptive'"`
to inspect local load, allowance, pending TX reservations and denial reasons.
See [adaptive bot admission](firmware/runtime/BOT_RUNTIME.md#opt-in-adaptive-airtime-admission)
for caller fairness, measurement limits and a short field check. This uses the
existing physical modem queue over any supported transport; WiFi is not required.
`name` and `channel status` provide private readback; channel readback
contains only its name and a short key fingerprint, never the key.
This delegates to the **native** staged and
atomic source journal and the native bot settings; `source commit` is
only accepted for verification, so poll `source status` for durable/live
activation. Name changes apply live; channel/key changes require restarting
the worker. `data help` exposes the same private KV, timer and reminder
backup/restore commands as the on-device bot. Use `admin.py data-export`
or `data-restore` with `--unix-socket`; scheduler restores require `--no-rearm`.
See [scoped bot data](firmware/esp32/MAST_ADMIN.md#scoped-bot-data) for scope,
replacement and restart behavior. Do not enter channel keys in shell history or signed-only RF
commands. To change the host native bot identity, stage a protected native
key file with `admin.py --unix-socket ... key-import bot
--native-key-file ... --expected-public-key ...`, inspect `key bot pending`,
then run `key bot apply`. `key bot cancel` discards the stage. Staging and
host restart leave the active key unchanged; apply reloads only the bot worker,
keeps the shared radio source and other roles running, and queues a zero-hop
advert after the selected source is ready. Old notes, timers and reminders
remain bound to the old full key; they are not reassigned to the new bot.
See [host native bot identity changes](firmware/esp32/MAST_ADMIN.md#go-host-native-bot-identity)
for commands and failed-apply recovery. The owner socket is not
published on HTTP, Companion TCP, KISS, or plaintext RF. The source API
advertises the native command metadata, verified-channel and owner-granted
destination capabilities (`cmdmeta`, `mesh-chan`, `mesh-dest`) and configured
HTTPS (`https`), but manifest capabilities grant no permission by themselves.
Network commands require configured credentials and the explicit owner grant.
`make -C firmware/esp32 bot-native-host-test` exercises the
real native process through the Go host and a simulated queued mast. If a
durable source slot becomes unavailable, the bot stays faulted and rejects
ordinary commands while this private socket remains available for
`source status` and deliberate `source remove` recovery; it does not
silently execute the bundled source instead.

### Native bot owner grants

Use the bot's private owner socket to enable shared state, personal reminders
or event subscriptions. They default to off and survive worker restart.
Read the supported command families and saved/applied grants first:

```sh
python3 tools/hardware/admin.py --unix-socket "$HOME/.local/state/meshcore-host/bot/native/admin.sock" command 'help'
python3 tools/hardware/admin.py --unix-socket "$HOME/.local/state/meshcore-host/bot/native/admin.sock" command 'policy'
```

`shared on` allows bot/channel KV and named timer scopes; `shared off` withdraws
that access without changing caller/conversation storage permissions.
`reminders on` permits personal reminders from authenticated private DMs;
`reminders off` suspends pending reminders and fences new creation/dispatch
without deleting records. Reminders still need trusted UTC and a direct
return route. The owner socket does not act as an authenticated DM sender.
`reminders status` also reports scheduler UTC trust as `clock=0/1`.
Native HTTPS and the scheduler share the Linux kernel clock provider;
enable host NTP synchronization, then check the usable UTC bound:

```sh
timedatectl show -p NTPSynchronized
python3 tools/hardware/admin.py --unix-socket "$HOME/.local/state/meshcore-host/bot/native/admin.sock" command 'clock'
```

Expect `NTPSynchronized=yes` and `scheduler=1 reason=usable`. `clock` reports
`uncertainty-us` and `scheduler-limit-us=2000000`: reminders require the kernel
error bound plus sample measurement uncertainty to remain within two seconds.
Ordinary systemd-timesyncd 2048-second polls fit this budget, including the
approximately 1.024-second bound before a poll resets it. HTTPS requires kernel
synchronization but not this scheduler error budget; an excessive bound can
report `https=1 scheduler=0 reason=scheduler-error-bound-exceeded`. Check host
NTP health and the reported bound rather than relying on `NTPSynchronized`
alone. Setting a plausible date or changing a role RTC does not establish trust.
UTC uncertainty can add several seconds to a reminder's delay; deadlines and
dispatch use conservative bounds rather than treating sampled UTC as exact.
Synchronization loss or a clock step pauses creation/dispatch and reports
`clock=0`. After one second of stable, kernel-trusted samples, eligible pending
reminders can resume. Large forward corrections can mark them overdue;
listing/cancellation remain available while UTC is untrusted.
With `clock=1`, run `reminders on` through the owner socket, then send
`!remind 2m tea` in an authenticated private DM. Expect a pending ID and UTC
deadline; `!reminders` lists that caller's records and `!cancel ID` cancels
a pending record. The companion receives the later reminder over RF when its
direct return route is fresh. Already sent or uncertain records are not
retried after a worker restart.

`events MASK` accepts 0..15: 1 startup, 2 connectivity, 4 message, 8 node_status.
For example, `events 9` permits startup and node-status handlers; the active Lua
source must declare them. `events 0` withdraws all event subscriptions.
`shared status`, `reminders status` and `events status` read saved/applied
grants; events also reports `subscribed`, the currently effective handler mask.
`help grants` shows the write syntax. Boolean writes require exact `on`/`off`;
invalid arguments return `Error:` before changing policy.

`status` reads readiness, running job count and event counters. `cancel` stops
running commands/events and delayed collectors, not personal reminders.
Withdrawing a grant fences yielding work, but RF or storage effects already
admitted may have completed or have an unknown outcome. After a failed write,
inspect saved/applied readback before restarting: an old saved grant can
return when the runtime reloads. Re-enabling reminders can dispatch eligible
pending records, but does not replay sent or uncertain records or rearm a
scheduler restore. Full bot/caller identity scopes remain unchanged.

Package capabilities in `source api package` describe supported APIs, not
permission to use them. `source api grants` reads the host grants;
`source api storage`, `source api board`, `source api reminders` and
`source api events` include their saved/applied state. Event `active=0` and
`subscribed=0` mean no handler is currently permitted to run, even if a mask
is saved. Storage/reminder autonomous fields stay 0 until the reminder grant
and scheduler UTC trust are both applied. See the [native owner grant reference](internal/nativebot/PROTOCOL.md#native-owner-grants)
for cancellation and persistence behavior. These controls affect only the bot,
not another role's identity, access policy or the shared radio PHY.

The host's `observer.shared_phy` status shows its `requested` radio profile
and `effective` settings read back from the modem on connection or reconnect.
`valid`, `error`, `configuration_owner: "observer"` and `owner_connected`
identify an offline or mismatched connection; an offline radio has no
`effective` profile. Frequency and bandwidth are in Hz, alongside SF, coding
rate denominator, TX dBm, aggregate airtime factor, CAD and interference
threshold. The factor uses the modem's float32 wire precision.
`companion.companion_retention` similarly shows requested and active retention.
The modem dashboard shows live owner slot, airtime policies, queues and credits.
The host's status endpoint does not expose passwords or private keys.

### State, compatibility and credentials

State lives in `.state/host` by default, relative to the configuration file.
Private seed files have mode 0600; state directories have mode 0700. Back up the
whole state directory, not just the configuration. A corrupt identity fails
startup instead of silently creating a different node, and a process lock
prevents two hosts from owning the same state.

The storage layer also supports native 64-byte scalar-prefix keys in
`identity.expanded`; these take precedence over `identity.seed`. An import
preserves the legacy seed for migration rollback, and a corrupt imported key
never falls back silently to that older identity. Key persistence alone does
not rekey a running role: protocol exposure requires the role's lifecycle
integration. A post-rename directory-sync error means the replacement may
already be visible; callers must recover or fail closed rather than assume an
in-memory rollback restored disk state.

Role identity replacement uses a single private `identity-state.json` envelope
containing the native key and complete role document. Once present, that file
is authoritative for identity loading, export and subsequent state saves.
Legacy files remain a pre-import rollback snapshot, not another active store.
A logical reset can atomically publish a new key with an explicitly absent
document; startup then creates role defaults without reviving old files.
Malformed envelopes fail closed. Do not delete one to bypass a startup error.

Companion clients share contacts, 40 channel slots and base settings, but
have independent replies, negotiated protocol versions and inbox cursors.
The base and dedicated bot companion endpoints advertise MeshCore companion
protocol 13. A client requesting target version 2 or 3 in its device query
still receives the corresponding message format; that target is separate
from the device's advertised protocol version.
The last 256 incoming messages are persisted. Reconnecting applications replay
that retained journal: the standard companion protocol has no durable client ID
with which to distinguish a reconnect from a new application. Room history is
bounded to 32 posts and membership to 20 identities. A room does not echo a post
to its author; two applications sharing the base identity are the same author.

`companion_retention` selects `durable_replay` (the default host extension) or
`native_queue`. Existing saved history is preserved during migration. Bounded
replay is not exactly-once delivery. Battery/storage replies report actual
used and total KiB on the shared host filesystem, not a separate role quota.

Private-key export is disabled unless `companion_key_export` is true. Enabling
it permits connected companion applications to retrieve the base identity's
native scalar-prefix private key; use it only on a trusted connection. It does
not authorize identity replacement, another role's key access or radio reboot.

`companion_key_import` independently enables replacement of the base identity.
It commits the key and existing companion data together before activating the
new identity. Other roles are unchanged, and importing a sibling role's key is
rejected. Existing contacts, channels and messages are retained. Both key
management options are disabled by default.

`role_key_import` separately enables authenticated repeater/room
`set prv.key` commands. They durably stage a native scalar-prefix key and
reply with its public key, but keep the current identity active until that
role reboots or the host restarts. Activation preserves settings, history and
administrator access. Current and pending identities are checked across all
five always-present roles and the optional bot companion under the same state
lock, including companion imports, to prevent identity sharing.
This option is disabled by default; it does not enable remote key export or
storage erase.

Companion reboot restarts only the base application and its radio connection.
Factory reset additionally creates a fresh base identity and defaults, and is
disabled unless `companion_factory_reset` is true. Both operations disconnect
companion clients; neither resets the physical radio or sibling roles. Admission
is not a completion acknowledgement. A failed operation leaves the base stopped
with an `application_error` in `/status`. The last channel is temporarily changed
and restored by `make test-companion-reboot HOST_CONFIG=...`; use a staging profile.
`make test-companion-factory-reset HOST_CONFIG=...` is destructive: use only
disposable staging state with `companion_factory_reset` enabled. It checks
fresh identity/channel defaults, both clients reconnecting, unchanged sibling
identities and continued operation of the existing bot/modem connection.
Authenticated repeater and room `reboot` commands restart only the addressed
application and its radio connection, with no success reply, matching the
native command. Each role's `application_started_at` field in `/status`
changes after successful application activation, not after an ordinary radio
reconnect; it is null while the application is stopped.
`make test-role-reboot HOST_CONFIG=...` uses encrypted administrator commands
to restart the repeater and room separately. It checks application start times,
retained identity/name, unchanged siblings and continued bot/modem operation.
Use a staging profile and provide its configured `admin_password_env` to both
the host and checker; this check transmits over the radio.
`make test-role-rekey HOST_CONFIG=...` additionally replaces both staging role
identities. It requires `role_key_import` and is destructive: use disposable
state only. It checks the native staging reply, unchanged active identity before
reboot, activation afterward and encrypted access using the new identities.

Set `MESHCORE_ROOM_PASSWORD` before starting the example configuration. It uses
`room_password_env` to read your room join password from that environment
variable. An enabled room requires a configured password; startup refuses an
unset or empty value before opening the radio or creating identities.
For a deliberately open room, set `room_public=true` and leave
`room_password` and `room_password_env` empty.
`admin_password_env` is empty by default, so new roles have no password-based
remote administration. It does not authorize blank-password login or revoke
administrator credentials already saved by native management commands.
Use `mqtt.username_env` and `mqtt.password_env` for broker credentials.
Never place credentials in tracked examples.

**Reconfigure the host without rebuilding:** `repeater_name`, `room_name`,
`companion_name` and `bot_companion_name` provision new role state. Once a role
has saved state, its managed name takes precedence over that initial value;
editing the JSON alone does not rename it. An authenticated, encrypted room
or repeater administrator can send `set name NAME` (effective and durable on
success). Provision `admin_password_env` privately to enable password-based
administration on new roles. To deliberately rotate either role's identity,
temporarily enable `role_key_import`, send encrypted `set prv.key` from that
administrator, and reboot that role to activate its staged key. A successful
staging reply is **not** activation; do not send private keys by signed-only
unencrypted RF commands.
Repeater and room do not own chat-channel slots. Their encrypted `region`
commands change public auto-derived scopes and defaults immediately; private
`$` transport-region keys currently require private `repeater_policy.regions`
or `room_policy.regions` configuration and a role/host restart. That host
extension is not a native remote private-key management or keystore interface.

Trusted companion-protocol clients can change each companion or optional
`bot_companion` advert name and the keys in its 40 channel slots with
`SetAdvertName` and `SetChannel`; both changes commit before success and
survive restart without a rebuild. Identity import is separate and opt-in:
`companion_key_import` applies to the base, while `bot_companion_key_import`
requires a loopback-only listener. Companion TCP itself has no authentication
or encryption; bind it to a trusted network, and do not route secret-key
import or export over plaintext signed-only RF. Existing identities remain
unchanged unless deliberately rotated. The default KISS `bot` proxy is **not**
a native command bot: select the opt-in `native_lua` runtime above for
host-managed advert name, channel-key settings and shared-Lua commands.
Its stable identity and KISS clients are distinct from `bot_companion`.

All roles share **one physical radio profile**. With `phy_authority: "host"`,
change `radio`, `tx_power` or `phy_profile` in the private host config and
restart the host to apply and verify them. With `phy_authority: "modem"`,
use the mast's authenticated physical-profile management; the host follows
verified readback (`phy_tracking: "follow"`) or rejects a fixed-profile
mismatch. A companion client's conflicting per-role radio request returns
the native two-byte `UnsupportedCmd` error: change the modem-owned PHY through
the separately addressed authenticated mast-admin Management role instead.
Its binary error frame cannot carry a textual hint, and no role credential
grants physical-owner authority.
The mast's separately RF-addressable management role accepts authenticated
physical commands without WiFi; the web dashboard is another transport.
Repeater/room administrator credentials do not grant mast ownership.
Mainline `CommonCLI` also provides per-board `set radio` with a
reboot-to-apply reply, and native Companion can retune its own radio with
`SetRadioParams`. Neither is a safe independent retune for roles sharing a
single host PHY. The host's Go role/Companion handlers translate the upstream
wire contracts; they do not execute the C++ application handlers. The one-PHY
owner, private host configuration and readback are intentional host
integration boundaries, not additional per-role MeshCore radio settings.
Authenticated repeater/room `get radio` reports the current verified shared
profile in native CLI format; `get tx` reports verified shared power. Both
fail explicitly while readback is unavailable.

Unsupported hardware operations, conflicting client retunes, shared-radio
reboot and unauthorized identity replacement return explicit errors.
Exact supported companion commands and role administration limitations are documented
in [`internal/companion/doc.go`](internal/companion/doc.go) and
[`internal/roles/doc.go`](internal/roles/doc.go).

Battery, noise floor, instantaneous RSSI, MCU temperature and physical packet
counters are queried at connection time and every 15 seconds. Status JSON
includes validity flags; stale/offline snapshots fail explicitly. These counters
belong to the shared PHY, not to individual logical roles. A USB-only board may
report battery voltage zero. Per-packet RSSI/SNR remain attached to received
packets rather than being substituted with the instantaneous RSSI reading.
Companion battery and packet-total statistics are supported; per-route breakdowns
are unavailable and encoded as zero. Core/radio statistics requiring unavailable
airtime/queue measurements return a protocol unsupported response rather than
invented values. Room/repeater status, voltage and temperature requests use
actual available readings.

`role_tx` in status separates queue admission, rejection, successful/failed RF
completion and unknown outcomes. It records reported RF time, not host queue or
network delay. Unknown completion makes the accumulated airtime incomplete.
These role-lifetime counters survive TCP reconnects and reset on logical role
restart; shared hardware counters remain separate. Diagnostic logging does not
block radio ingress, and any dropped diagnostics are counted.

Radio links reconnect with a new KISS session while preserving role state.
An uncertain transmission is **never automatically replayed by the transport**;
protocol-level room/message acknowledgement logic owns any legitimate retries.

### MQTT observer

The default `internal-v1` output below preserves event IDs, nullable signals and
plain presence strings. To use the public observer packet/status contract, set
`mqtt.format: "observer-v1"` and configure its IATA and optional private-receiver
identity authentication in the [MQTT observer guide](firmware/esp32/OBSERVER.md).
Changing the format does not enable another broker or RF transmissions.

For an on-chip deployment that only needs an external MQTT server,
`make mqtt-broker HOST_CONFIG=...` runs the configured broker without opening a
radio or creating host-side role identities. Configure `mqtt.broker_listen` and
the username/password environment variables when accepting network clients.
For a persistent user service, run `make mqtt-install HOST_CONFIG=...`, put
the referenced credentials in `~/.config/meshcore-mqtt/environment` with mode
0600, then run `systemctl --user enable --now meshcore-mqtt.service`. The
broker uses its own configuration and executable, independently of the host
role service.

Topics use the observer's public key from `/status`:

```text
meshcore/<observer-public-key>/packets
meshcore/<observer-public-key>/status
```

Packet events are non-retained QoS 1 JSON with `event_id`, `timestamp`,
`observer_identity`, `raw_packet_hex`, decoded packet metadata and nullable
`rssi`/`snr`. Local reflections have `local_loopback: true` and null signal
measurements. Malformed packets retain their raw bytes and a `decode_error`.
Availability is retained `online`/`offline`, including a last will.

The queue is bounded and memory-only. MQTT outages do not block the radio;
overflow and delivery failures are logged. QoS 1 can duplicate events, so
deduplicate using `event_id`. Shutdown can discard a remaining backlog.
Set `mqtt.broker_listen` to `""` and `mqtt.url` to an existing broker to disable
the embedded broker. Anonymous embedded access is allowed only on a literal
loopback address; non-loopback binding requires credentials. Use a trusted
network/tunnel for its plaintext listener, or an external TLS broker.

### Bot connection

Configure `meshcore-go/meshcore-bot` with the host proxy rather than the
companion port:

```toml
nodeType = "kiss"
connection = "tcp://127.0.0.1:8105"
freq = 910.525
bw = 62.5
sf = 7
cr = 5
```

Keep its radio/power settings consistent with the host configuration.
The bot proxy supplies a persistent KISS crypto identity. The Go
`meshcore-go/meshcore-bot` derives application identities from configured bot
names; its on-air identity does **not** automatically become the proxy identity.
Run bot automation separately from `meshcore-host`.

The Python `meshcore-bot` uses the **companion TCP protocol**, not KISS. Give it
its own application identity by setting `bot_companion_listen` in the host
configuration (for example, `127.0.0.1:8106`), then configure the bot with:

```ini
[Connection]
connection_type = tcp
hostname = 127.0.0.1
tcp_port = 8106
```

Set `[Bot] startup_advert = flood` for multi-hop discovery, or `zero-hop` for
direct neighbors. This selects the bot's advert scope when its service starts.

The dedicated listener is disabled until configured. It keeps its own contacts,
channels, message history and private key, separate from both the base
companion and the KISS bot proxy. Keep `bot_companion_name` consistent with the
bot's configured device name. Copy the existing bot configuration and database
before changing its connection; test a passive copy on an isolated RF profile
before stopping the live service. Keeping the bot name and database alone does
not preserve the old device's on-air public key: an identity-preserving move
also requires securely importing its native private key into the dedicated
host role. Back up the host state before import, confirm the public keys match,
and power off the old radio before enabling the new one with that identity.
`meshcli get private_key` can read the old companion's 64-byte expanded key
into a mode-0600 private file. Enable `bot_companion_key_import` only for the
local migration, import with `meshcli set private_key` through a private
`meshcli script` file rather than passing the key as a command-line argument,
then turn the flag off and restart the host. The import command is available
to clients of the local listener while the flag is enabled; protect that
short-lived window and do not publish the export or command file.
Run `make host-readiness HOST_CONFIG="$HOME/.config/meshcore-host/config.json"`
against the deployed configuration to check the sixth identity and the bot's
own companion connection, not just the five-role default.

### Install and check

```bash
make host-install
systemctl --user enable --now meshcore-host
journalctl --user -u meshcore-host -f
```

Installation does not overwrite an existing
`~/.config/meshcore-host/config.json`. The user service stores state in
`~/.local/state/meshcore-host`; if moving a foreground deployment, stop it and
move its existing state there before starting the service to retain identities.
Optional secret environment assignments go in a private mode-0600 file at
`~/.config/meshcore-host/environment`. A user service survives logout only if
the user manager remains running; enable systemd lingering through the normal
system administration policy when deploying unattended.

```bash
make test
make host-readiness HOST_CONFIG="$HOME/.config/meshcore-host/config.json"
make test-host-live HOST_CONFIG="$HOME/.config/meshcore-host/config.json"
```

`make test` runs the Python and Go race tests and checks the native KISS modem
with the WiFi multiplexer. `make parity-native` runs the pinned MeshCore
reference harness; `make parity-check` compares fresh reference events with
the Go race tests. Use `HOST_TEST_PACKAGES=./internal/policy` to check only the
policy package. These checks run without a connected radio or host service.
On first use, native checks fetch the pinned MeshCore source and libraries
into `.tmp`; they need Git, PlatformIO, Python 3 and C/C++ compilers.

Readiness checks the configured role identities, two real base companion
connections, discovery of room/repeater adverts, bot KISS configuration and
the dedicated bot companion connection when enabled.
The live check **transmits**: it temporarily uses the last advertised channel
slot, restores it afterwards, verifies MQTT local reflection and independent
companion inboxes, logs into the room and requires an acknowledged post.
Do not run it while another application is editing that channel. Use the
two-radio checks below for over-air reception on an isolated frequency.

The Go-to-Go two-radio check uses an independent Go peer over another KISS
radio. Run a separate staging host and state directory using **912.525 MHz,
BW 250 kHz, SF7, CR 4/5, TX 2 dBm**.
Stop a production host first only if reusing its radio. Do not run conflicting
profiles against the same PHY.

```bash
make test-host-reconnect HOST_CONFIG=staging.json \
  UPLOAD_PORT='/dev/serial/by-id/<staging-kiss-radio>'
make test-host-rf HOST_CONFIG=staging.json PEER_RADIO=spare-radio.local:8001
```

The first target requires a running host, resets only the specified board and
requires client readiness to recover within 90 seconds. The second configures
the independent peer radio and requires genuine RF repeater forwarding, MQTT
publication with measured RSSI/SNR, group and room delivery to two companion
clients, remote room login/post/ACK and encrypted group/private downlinks.
Synthetic local reflections cannot satisfy
its RF assertions. It restores the temporary channel and removes its temporary
companion contact. An authenticated probe membership and test posts remain in
the room because the guest protocol has no membership-removal operation.
Restore any reused production radio's profile afterwards; the peer remains on the
isolated test tuning and does not transmit without a client.

To check over-air operation with a MeshCore companion radio, build the upstream
USB companion firmware for a separate Xiao S3/WIO-SX1262. With the host running
on the isolated RF profile, select that radio explicitly:

```bash
make stock-firmware
make stock-firmware-upload STOCK_SERIAL='/dev/serial/by-id/<companion-radio>' STOCK_DEVICE_MAC='<device-mac>'
make stock-firmware-boot STOCK_SERIAL='/dev/serial/by-id/<companion-radio>' STOCK_DEVICE_MAC='<device-mac>'
make test-stock-rf HOST_CONFIG=staging.json \
  STOCK_SERIAL='/dev/serial/by-id/<companion-radio>'
```

The checker sets the companion radio to the host's RF profile through its USB
companion interface. It exchanges encrypted text and ACKs in both directions,
logs into the room, posts and receives signed room messages, and checks a
three-byte path through one repeater. RF reception includes RSSI/SNR; local
delivery through the shared modem is identified separately. The path setting
is restored afterward.

Uploading erases the selected radio's existing identity and settings. It stays
in download mode until the boot target succeeds. With two spare radios, the
primary deployment can remain online during the check.

Before unattended deployment, observe RF reception, an AP outage, radio
restart and client reconnects over a longer period. An unreachable modem or
failed WiFi association blocks role deployment.

## Build the WiFi KISS firmware

### Radio dashboard

Open `http://meshcore-radio.local/` on the radio's LAN for its built-in operator
page. It displays incoming/outgoing packet events, measured signal levels,
queue occupancy, airtime budget, connected clients and recent traffic without
requiring an external server or Internet assets. HTTP runs separately from the
radio loop; history and HTTP clients are bounded.

Live updates arrive over WebSocket, with reconnect handling and pause/resume
controls. `make test-live-dashboard RADIO_HOST=...` exercises two simultaneous
live subscribers and HTTP diagnostics; run it alongside `test-live-multiclient`
to check the radio under concurrent operator and KISS traffic.

The page is read-only. RX airtime estimates and local reflections are labelled
separately from measured RF transmit time. `GET /api/status` provides the same
snapshot for other tooling. Host application readiness is a separate endpoint
on the Go host, not the ESP32.

Set `KISS_HOSTNAME` in the firmware build configuration when deploying multiple
radios. Use the DHCP address if the operating system or network lacks mDNS.
See [dashboard configuration and API](firmware/shared/DASHBOARD.md). HTTP and KISS
are trusted-LAN interfaces, not public Internet services.

### Upgrade and rollback

Back up a running installation before upgrading its host and modem together.
The host backup stops the standard user service and keeps configuration,
identities, messages, binaries and unit files in a private directory. Flash
images also contain private configuration; do not publish either backup.

```bash
make host-backup BACKUP_DIR=.tmp/release-backup
make firmware-backup UPLOAD_PORT='/dev/serial/by-id/<radio-device>' \
  FIRMWARE_BACKUP=.tmp/release-backup/modem.bin
make firmware-upload UPLOAD_PORT='/dev/serial/by-id/<radio-device>'
make host-install HOST_CONFIG=meshcore-host.json
systemctl --user start meshcore-host.service
curl --fail http://127.0.0.1:9080/readyz
```

`host-install` preserves an existing installed configuration. Update its
explicit policy settings deliberately; it does not replace saved identities.
Disconnect other direct modem clients during firmware maintenance.

To roll back, stop the service, restore its complete host snapshot and matching
flash image, then start it again:

```bash
make host-restore BACKUP_DIR=.tmp/release-backup
make firmware-restore UPLOAD_PORT='/dev/serial/by-id/<radio-device>' \
  FIRMWARE_BACKUP=.tmp/release-backup/modem.bin
systemctl --user start meshcore-host.service
```

Restore retains the replaced files privately instead of overlaying old data on
newer identity envelopes. If any operation fails, leave the service stopped and
resolve the error before restarting. These targets use the standard per-user
installation paths documented above.

### Build and flash

The firmware target is the Seeed XIAO ESP32-S3 with the WIO-SX1262 board. It keeps MeshCore's KISS commands and SetHardware extensions, while adding a frame-aware multi-client TCP service around the single radio.

```bash
make firmware-config
$EDITOR firmware/platformio.local.ini
make firmware
make firmware-upload UPLOAD_PORT=/dev/ttyACM0
```

`/dev/ttyACM0` is the default upload device. Override it when needed:

```bash
make firmware-upload UPLOAD_PORT=/dev/ttyACM1
```

Prefer persistent USB serial paths when several boards are attached. Select the
intended device from `/dev/serial/by-id/`; never infer it from USB enumeration
order.

```bash
make firmware-upload UPLOAD_PORT='/dev/serial/by-id/<radio-device>'
make firmware-reset UPLOAD_PORT='/dev/serial/by-id/<radio-device>'
```

`firmware-reset` reads the chip identity and resets without rewriting flash.
DHCP addresses can change; serial numbers identify hardware, not IP leases.

The default source ref is MeshCore `companion-v1.17.1`. To prepare a newer clean checkout:

```bash
rm -rf .tmp/MeshCore
make MESHCORE_REF=main firmware
```

Preparation also applies `firmware/shared/radio-reconfigure.patch`. RadioLib parameter
and power changes must leave the cached RX state so the receive loop restarts
with the new settings. Without this, KISS control readback can succeed while
the receiver remains deaf until its first transmission. The two-radio cold
reset check exercises this failure; sending an initial test packet from the
primary would mask it. A patch conflict with a newer upstream version fails
the preparation step instead of silently omitting the fix.

`WifiKissMultiplexer` accepts up to eight simultaneous TCP clients by default.
It reconstructs complete KISS frames, serializes requests through the single
`KissModem`, returns command responses and `TxDone` only to the originating
client, and broadcasts RF receive frames plus `RxMeta` to every client. Signal
report enable/disable is virtual per connection so one application cannot turn
metadata off for the others.

After a client's packet receives a successful `TxDone`, the firmware also
reflects that packet to every other connection without retransmitting it. The
reflection is followed by `RxMeta` bytes `80 7f`, reserved here to mean local
loopback rather than an RF measurement. The sender does not receive its own
reflection. The monitor prints this as `LOCAL LOOPBACK`.

Use a DHCP reservation so the roof radio keeps a stable address. USB serial at
115200 baud prints the assigned IP address and per-slot TCP client status. The
KISS service listens on port 8001. `KISS_MAX_TCP_CLIENTS` and
`KISS_REQUEST_QUEUE_DEPTH` can be changed in `platformio.local.ini`.

Each connection has a bounded 2,060-byte output queue. Socket reads and writes
are nonblocking, and input work is capped per poll so a busy connection cannot
monopolize the radio loop. A slow client's queue overflow is logged and that
connection is closed rather than blocking all other clients or silently
discarding part of a frame. Disconnects remove queued requests; an already
submitted request retains its ownership until completion, even if its TCP slot
has been reused.

WiFi association has one retry owner: the main loop retries every 30 seconds
without repeatedly calling `WiFi.begin()`. Once associated, it allows up to
120 seconds for DHCP instead of interrupting address acquisition. All-channel
scanning orders candidate APs by signal strength. Serial diagnostics include the
disconnect reason and free/minimum heap, but not the password. Use the disconnect
reason to distinguish authentication failure, AP availability and other causes:

```bash
pio device monitor --port /dev/ttyACM0 --baud 115200
```

`KISS_WIFI_TX_POWER` defaults to `WIFI_POWER_8_5dBm`, independently of LoRa
transmit power. On a XIAO S3, association with a strong mixed WPA2/WPA3 AP
failed at the default 20 dBm and at 19.5 dBm, but succeeded at 8.5 dBm.

The power limit can be overridden in `firmware/platformio.local.ini` using
an Arduino `wifi_power_t` constant, but validate association and sustained
traffic before raising it. Validate WiFi performance separately on other boards.

## Connect a host

Test with the included monitor. The port defaults to 8001, so a bare host is
enough:

```bash
python3 meshcore_kiss_monitor.py --tcp RADIO_IP
python3 meshcore_kiss_monitor.py --tcp RADIO_IP:8001 --json
make run-tcp RADIO_HOST=RADIO_IP
```

The monitor first reads the current settings and does not retune a matching
radio. If a change is needed on the shared firmware, it must obtain the
configuration-owner lease; an existing owner causes an explicit refusal.
It preserves transmit power and carrier/airtime policy, then releases the lease
and reconnects as an observer. Stock KISS configuration remains supported.

For an independently managed stack that does not use `meshcore-host`, configure
`meshcore-go/meshcore-bot` or another host stack with:

```toml
nodeType = "kiss"
connection = "tcp://RADIO_IP:8001"
freq = 910.525
bw = 62.5
sf = 7
cr = 5
```

Connect the router, room server, client/base, and observer directly to the same
`tcp://RADIO_IP:8001` endpoint. The radio serializes their transmissions and
provides the cross-client local receive path. All direct clients see the same
physical radio configuration, telemetry, and modem identity; role software
still owns routing, application state, persistence, and retries.

Validate the installed firmware with four simultaneous test connections and
three short RF transmissions. This preserves the current radio configuration
and exercises matching legacy setup commands; select the intended test frequency
beforehand. It checks reply ownership,
fragmented input, local reflection and metadata adjacency, sender exclusion,
a client reconnect, and a minute of concurrent control traffic:

```bash
make test-live-multiclient RADIO_HOST=RADIO_IP
make test-live-multiclient RADIO_HOST=RADIO_IP LIVE_TEST_SECONDS=600
```

Check genuine over-air reception separately, without transmitting or changing
the shared radio tuning:

```bash
make test-live-reception RADIO_HOST=RADIO_IP LIVE_TEST_SECONDS=120
```

This requires the same packet and adjacent RSSI/SNR metadata on two concurrent
connections. Local-loopback markers are excluded; a quiet RF channel times out.

## ESP32 roles with a remote modem

Run a selected repeater, room or companion role on an indoor ESP32 and keep
the SX1262 at the radio site:

```text
Indoor ESP32: role, identity and stored settings
    |
    | WiFi / TCP port 8001
    v
Mast ESP32 + SX1262: shared modem, tuning and transmit queue
    |
    v
RF mesh
```

Each role image uses the remote modem as its radio interface. The role device
owns its identity, routing and application state; the modem owns carrier
access and shared airtime. Select the repeater, room or companion image for
the indoor board. A Go host provides several roles on one indoor computer.

Configure the physical modem first, then give the role matching radio
preferences and its WiFi connection. Reconnection checks the committed
profile and restores the source policy. Transmit status distinguishes queue
waiting, RF completion and uncertain outcomes.

Build and upload the selected role:

```sh
make remote-radio-config
$EDITOR firmware/platformio.phyless.ini
make remote-radio-firmware PHYLESS_ENV=Phyless_Xiao_S3_repeater
make remote-radio-upload PHYLESS_ENV=Phyless_Xiao_S3_repeater \
  UPLOAD_PORT='/dev/serial/by-id/<indoor-esp32>'
```

Select `Phyless_Xiao_S3_room` for a room or `Phyless_Xiao_S3_client` for a
companion. Builds use a separate pinned source tree and configuration.
For the driver API and session contract, see
[the remote-radio adapter](firmware/shared/RemoteKissRadio.md).

## Standalone ESP32 roles

Use the [standalone setup guide](firmware/esp32/README.md#choose-an-image)
for image selection, private configuration, first flash, ordinary updates and
restart behavior. The core bot image and management-enabled beta image have
different source-persistence behavior; select the latter for durable
installation/rollback. The HTTPS image additionally reserves an outbound socket.
See [mast administration](firmware/esp32/MAST_ADMIN.md) for runtime controls
and [Lua development](firmware/runtime/BOT_DEVELOPMENT.md) for local testing.

## Network safety

KISS TCP has no authentication or encryption. Anyone who can reach the port
can submit transmissions and invoke exposed modem operations. Radio-only
firmware also allows configuration-owner operations. Every standalone mast
image rejects KISS PHY changes; management-enabled images provide
authenticated shared PHY changes through Management. Keep port 8001 on a
trusted VLAN or behind a host-to-site WireGuard tunnel. Do not expose it
directly to the Internet.

The companion endpoint grants application-level access to the shared base,
including its messages, channels and RF operations, without authentication.
The host defaults to `127.0.0.1:5000`. For a companion app on another device,
set `companion_listen` to the host's trusted-LAN address and explicitly set
`companion_allow_remote=true`. This opt-in also applies to `bot_companion_listen`.
Restrict access with a firewall or protected tunnel. Keep the bot and status
listeners loopback unless remote access is explicitly required.

## Licence

Original project code is licensed under [Apache-2.0](LICENSE). Upstream
MeshCore, dependencies and retained third-party material keep their own
licences and notices; this licence does not relicense them.
