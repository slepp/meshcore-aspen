# Standalone ESP32 setup and operation

Run a repeater, room, companion/base and optional Lua bot directly on a
Seeed XIAO ESP32-S3R8 with PSRAM and Wio SX1262. Each role owns its key and
state; all share one LoRa profile, transmit queue and airtime budget.
Repeater, room, management and bot RF work can continue without a Go host or
WiFi. WiFi provides KISS, companion TCP, the dashboard and optional networking.

For a generic application without compiled credentials, use
[offline USB first-boot setup](PUBLIC_SETUP.md). It supplies a private per-node
SPIFFS image and retains that configuration through app-only updates.
The [public Aspen bundle](https://ve6slp.ca/projects/meshcore/#downloads) is
`1.17.1-slp-aspen` from earlier source `8b41d9c`; it requires that setup before roles,
identities, administration or OTA can start. A private image with compiled
credentials uses the separate source-build procedure below.

New source builds use the **Aspen 0.1.0 RC1** product version, based on MeshCore
1.17.1. `ver` and companion device information report `aspen-0.1.0-rc.1`,
independently of saved device/role names,
keys and enabled services. Start administration with `help`, `help wifi` and
`get owner.info`; see the [app endpoint guide](../shared/ANDROID.md#choose-the-endpoint-for-app-settings).

See the [release guide](../../release/README.md) for candidate tags, manifests and
qualification; the historical website image above retains its original version.

For a host-based setup, use the [Go guide](../../HOST_GUIDE.md).
For nRF native notes/BLE, use the [nRF guide](../nrf52840/README.md);
the ESP32 Lua images below do not run on nRF.

## Choose an image

| PlatformIO environment | Available behavior | Choose it for |
| --- | --- | --- |
| `Xiao_S3_WIO_onchip` | Native repeater, room, companion, observer, KISS and signed next-boot role selection | Native roles without Lua |
| `Xiao_S3_WIO_onchip_bot` | Adds Lua command bot; custom live activation is RAM-only | Core runtime experimentation; bundled source returns after reboot |
| `Xiao_S3_WIO_onchip_beta` | Adds mast-wide encrypted RF/trusted-LAN web administration, durable source installation, rollback and scoped backups | Administrable standalone Lua node |
| `Xiao_S3_WIO_onchip_https` | Adds approved HTTPS services and telemetry; three external KISS sockets instead of four | Bots needing configured network operations |

Disabling a runtime role does not compile its implementation out. The optional
Wasm interpreter has a separate build flag: `ONCHIP_BOT_WASM=1` includes it,
and `ONCHIP_BOT_WASM=0` omits it. Bot builds default to `1`. See the
[Wasm runtime guide](../runtime/WASM_RUNTIME.md) for the C/Rust SDK, package operations
and field validation procedures.

The ordinary standalone profile has four external KISS slots, two companion
clients and two live dashboard viewers. Negotiated MKISS can carry up to four
logical ports on one TCP session; that includes the controller port. Additional
roles/clients may need direct sockets. Socket capacity and local role capacity
are different limits. All selected services still share the same physical radio.

## Configure and build

From the repository root:

```sh
make aspen-config
$EDITOR firmware/platformio.onchip.ini
```

Keep this mode-0600 configuration private. Set a legal local shared PHY with
`ONCHIP_RADIO_FREQ_MHZ`, `ONCHIP_RADIO_BW_KHZ`, `ONCHIP_RADIO_SF`,
`ONCHIP_RADIO_CR` and `ONCHIP_RADIO_TX_POWER`. The tracked example is an example,
not a regional channel recommendation. Match frequency, BW, SF and CR on the
companion radio you will use. Set the environment variables referenced by the
configuration through your protected provisioning process:

- `MESHCORE_HOSTNAME`, `WIFI_SSID`, `WIFI_PWD`;
- `ONCHIP_ADMIN_PASSWORD` for native repeater/room administration;
- `ONCHIP_ROOM_PASSWORD` for room users; an empty value permits public room login;
- `ONCHIP_MQTT_URI`; empty disables MQTT publishing;
- for beta/HTTPS: nonempty `ONCHIP_MAST_PASSWORD`, plus the trusted companion
  full public key and separate operator verification public key where required.

Do not put secrets in shell history, public build logs or source control.
The mast password is separate from role passwords. Trusted companion keys
and the signed-role operator key serve different authorization paths; see
[mast administration](MAST_ADMIN.md) before enabling remote changes.

Build an administrable bot image with Lua and the optional Wasm interpreter:

```sh
make -C firmware/esp32 bot-firmware \
  CONFIG="$PWD/firmware/platformio.onchip.ini" \
  BUILD="$PWD/.tmp/onchip-MeshCore" ENV=Xiao_S3_WIO_onchip_beta \
  ONCHIP_BOT_WASM=1
```

For approved network operations, select `ENV=Xiao_S3_WIO_onchip_https` in this
command and subsequent filesystem/upload commands. For native roles without
Lua, `make aspen-firmware` builds the baseline `Xiao_S3_WIO_onchip` image.
Build targets do not flash or restart hardware.
For a Lua-only image, select `ONCHIP_BOT_WASM=0`. Keep the same value when
provisioning or running PlatformIO directly against the prepared build tree.

## First flash and updates

**Blank board only:** provision its SPIFFS partition before the first combined
boot. **This replaces all SPIFFS role files, source and bot data.** It retains
NVS identities and the saved modem profile, but is not a backup or identity reset.
Never repeat provisioning to fix a failed mount on an existing node.

After building the selected beta image:

```sh
make -C firmware/esp32 provision-fs \
  BUILD="$PWD/.tmp/onchip-MeshCore" ENV=Xiao_S3_WIO_onchip_beta \
  ONCHIP_BOT_WASM=1 \
  UPLOAD_PORT='/dev/serial/by-id/<standalone-radio>' CONFIRM_FS_ERASE=erase-spiffs
(cd .tmp/onchip-MeshCore && ONCHIP_BOT_WASM=1 pio run -e Xiao_S3_WIO_onchip_beta \
  -t upload --upload-port '/dev/serial/by-id/<standalone-radio>')
```

On ordinary updates, rebuild and use **only the firmware upload** command;
do not provision SPIFFS again. Select the same environment throughout.
The root `aspen-upload` builds/uploads the baseline image, not the Lua beta
image. Back up source and scoped data before updates; never erase NVS merely
to resolve a PHY mismatch.

Expected after boot: `http://MAST_IP/` lists service names, public keys and
readiness; selected Base listens on `MAST_IP:5000`, KISS on `MAST_IP:8001`.
Beta/HTTPS also serves `http://MAST_IP/admin`. On a trusted LAN, log in with
the mast password and inspect the **Roles**, **Shared radio** and **Programs**
web UI sections before making changes.
Enable/disable selections are saved for **next boot**, not immediate role
creation. See [browser administration](MAST_WEB_UI.md).

Use a companion radio on the same PHY, send its advert, then DM the bot
`!ping`; expect `Pong`. A dashboard TX completion or local reflection is not
evidence that the companion received the message over RF.

### Channel activity detection

New shared-modem profiles enable hardware CAD before transmission. A saved
profile keeps its existing setting. On Aspen's management interface, use
`get cad` and `set cad on` (or `set cad off`); the change is saved after the
acceptance reply and applies to every role sharing the modem. Pending jobs
from the previous configuration fail `STALE`. Frequency, bandwidth, spreading
factor, coding rate, transmit power and repeater delay settings remain unchanged.
CAD adds a channel check; it does not replace randomized carrier retries or
the roles' normal transmit/receive delays, and cannot prevent every collision.

## Internal MQTT observer

Aspen can publish received radio packets directly
from firmware while its repeater, room, companion and command bot run.
It does not need the Python bot or a host role to collect packets. An MQTT
broker is still required; the broker does not need a radio.
The default internal format also reports local transmission outcomes;
observer-compatible formats publish RF receptions only.

Set `ONCHIP_MQTT_URI` in the protected build profile to your internal broker,
including its credentials, then rebuild and upload only the application.
Keep credentials out of command arguments and build logs. A plain `mqtt://`
connection is unencrypted: use it only on a trusted internal network.
For public observer JSON, identity-signed CONNECT authentication, CA certificates
and WebSocket paths, select the explicit contract in the
[MQTT observer guide](OBSERVER.md). The procedure below uses the default
internal event contract and internal TCP.

For `observer-v1`, configure your receiver's IATA code and subscribe to
`meshcore/IATA/UPPERCASE_OBSERVER_KEY/{packets,status}` instead.
That contract publishes RF receptions only; the checks below describe the
default internal contract. See the observer guide for fields and authentication.

Using an authenticated mast session, inspect `status`. Save `roles 15` to
select repeater, room, companion and observer for the next boot, then `apply`.
This retains role identities, native settings and installed programs. The
command bot has its own saved `bot on` selection. After boot, the dashboard
should show **MQTT observer / running**, along with the other selected roles.
An empty URI disables publishing even when the observer role is selected.

Read the observer public key from the dashboard. Subscribe to its topics
using broker credentials loaded from a private MQTT client configuration:

```sh
mosquitto_sub -h INTERNAL_BROKER -p INTERNAL_PORT \
  -t 'meshcore/OBSERVER_PUBLIC_KEY/#' -v
```

Expect retained `status=online`, retained `roles/*` records, and unretained
`packets` JSON. Send an advert from another radio: its reception must arrive
with `direction=rx`, real RSSI/SNR and `local_loopback=false`.
Request `role advert repeater zerohop` through mast administration: its
completed transmission must arrive with `direction=tx`, `tx_state=2` and
the repeater's public key in `source`. Room, companion, management and command
bot transmissions carry their own submitting identity, not the observer key.
Local reflection is not another RF reception.

Disconnect only the observer's MQTT connection to check recovery. The broker
should publish its retained `offline` last will; reconnect should publish
`online` and refresh role records without restarting the radio roles.
Packets use QoS 0 and an eight-event queue: observations lost while disconnected
are counted and are not replayed. This is a live packet feed, not a durable
packet archive or an automatic MeshMapper upload.

## What survives a restart?

| State | Ordinary firmware update or restart |
| --- | --- |
| Role/Management/bot keys and saved selections | Retained in NVS; disabling a role does not delete its identity |
| Native role preferences, ACLs, companion contacts/channels | Retained in their role files; native room history/membership and pending traffic have volatile limits |
| Beta/HTTPS selected source and previous rollback source | Revalidated from the durable source journal; a bad selection needs explicit recovery |
| KV, named timers and personal reminder records | Retained in verified files with NVS authorities, bound to full bot/scope/principal identity |
| Lua globals, module cache, coroutines, sleeps and receive waits | Lost; restart or source replacement creates a fresh source generation |
| Bot contacts and learned direct routes | RAM-only; advertise and establish fresh authenticated routes again |
| Companion TCP sessions and RAM replay cursors | Clients reconnect; replay is bounded, not exactly-once delivery |

Source rollback changes code, **not data, identities, grants or consumed timer
claims**. Scoped scheduler restore uses no-rearm; it does not send missed
reminders. A pending reminder can survive a restart, but needs enabled grants,
trusted time and a fresh authenticated route before dispatch. Unknown sends
and consumed claims are not retried automatically.

## Continue by task

- [Manage names, keys, grants and source](MAST_ADMIN.md#administrator-workflow).
- [Export/restore scoped data](MAST_ADMIN.md#scoped-bot-data).
- [Configure approved network aliases](../runtime/NETWORK_API.md) or [metrics](TELEMETRY.md).
- [Write/test Lua commands](../runtime/BOT_DEVELOPMENT.md) and [look up the API](../runtime/BOT_RUNTIME.md#lua-api-by-capability).
- [Recover WiFi](#wifi-loss-and-recovery), [understand shared PHY authority](#shared-phy-boot-authority),
  or [inspect role lifecycle](#role-scoped-lifecycle).

## Native integration reference

Integration code is Apache-2.0; generated native sources retain their original
notices. Build/checker inputs pin MeshCore
`d92964352441e53b93e8667b802e04f6e072b39e` (`companion-v1.17.1`).

## Build and integration

The root `onchip-*` targets use a clean `.tmp/onchip-upstream` clone and build
in `.tmp/onchip-MeshCore`, separate from other firmware builds. Preparation
accepts only dedicated `.tmp/onchip-*` build directories. Nested targets
accept explicit `UPSTREAM` and `BUILD`.
Preparation preserves an existing build-tree `platformio.local.ini`; an
explicit absolute `CONFIG` path replaces it with mode 0600. The root preparation
target installs the private `firmware/platformio.onchip.ini` after generation.
The disposable build directory has mode 0700 because compiled images can also
contain operator credentials.
Supply `MESHCORE_HOSTNAME`, `WIFI_SSID`, `WIFI_PWD`,
`ONCHIP_ADMIN_PASSWORD`, `ONCHIP_ROOM_PASSWORD` and `ONCHIP_MQTT_URI` through the
build environment. The example uses WiFi for KISS and companion clients. To connect a wired
queued-protocol peer on UART1, set `KISS_STREAM_ENDPOINT=1` in the private
profile and add `KISS_UART_RX_PIN`, `KISS_UART_TX_PIN` and `KISS_UART_BAUD`
build flags with values for the actual board wiring. The diagnostic USB
endpoint is separate from that UART.

```ini
  -D KISS_STREAM_ENDPOINT=1
  -D KISS_UART_RX_PIN=${sysenv.KISS_UART_RX_PIN}
  -D KISS_UART_TX_PIN=${sysenv.KISS_UART_TX_PIN}
  -D KISS_UART_BAUD=${sysenv.KISS_UART_BAUD}
```

The administrator password must be 1-15 bytes. An empty room
password permits public room login. An empty MQTT URI explicitly disables MQTT.
There is no default administrator secret: missing configuration refuses startup.
A blank administrator password loaded from native preferences also refuses
startup, and administrative commands cannot clear it. Native guest permissions
and ACL role bits are unchanged; public room login is not administrator access.
Do not check credentials or credential-bearing build logs into source control.
The `ONCHIP_REPEATER_NAME`, `ONCHIP_ROOM_NAME`, `ONCHIP_COMPANION_NAME`,
`ONCHIP_OBSERVER_NAME`, `ONCHIP_BOT_NAME` and `ONCHIP_MANAGEMENT_NAME` macros
accept operator labels up to 31 bytes.

`make aspen-prepare`:

1. Archives the pinned native tree into a disposable build directory.
2. Applies `firmware/shared/radio-reconfigure.patch` and the PHY-owned
   `firmware/shared/queued-dispatch.patch`.
3. Copies `firmware/esp32/wifi_kiss_main.cpp` and the multiplexer, queued
   protocol and dashboard files from `firmware/shared/` into the build tree's
   `examples/kiss_modem`.
4. Runs `firmware/esp32/prepare.py --upstream PINNED_TREE --target BUILD_TREE`.
   It generates separately named native applications from the patched build
   tree and small wrappers under `examples/kiss_modem/onchip`. Native mains
   and UIs are excluded. The companion preference type is renamed to avoid
   collision with `CommonCLI` preferences.
5. Installs the private `firmware/platformio.onchip.ini` created by
   `make aspen-config` as the build tree's `platformio.local.ini`.
   `make aspen-firmware` builds `Xiao_S3_WIO_onchip` with base64 and the
   ESP-IDF MQTT SDK.

After changing a firmware patch or `prepare.py`, run the full build target
again. `field-rebuild` only refreshes local runtime files; it does not regenerate
the native role applications or apply patches.

Ordinary preparation, native-test and firmware-build targets do not upload
or access hardware. The explicitly named `beta-lab-*` targets in the beta
guide are guarded hardware operations, not part of those build/test targets.
An existing, mountable SPIFFS partition is required. Blank hardware needs an
explicitly provisioned filesystem image; startup never formats a failed mount.
The combined image requires PSRAM for the dashboard's bounded snapshots and
JSON buffers. Allocation failure stops startup explicitly.

For the separately identified native command bot, use
`make -C firmware/esp32 bot-build-test` for public-profile, bot-enabled and
baseline ESP32-S3 builds. The opt-in environment adds restricted Lua command
handlers and a fifth local source without consuming KISS/companion clients.
See [the command bot guide](../runtime/BOT_RUNTIME.md) for native commands, resource limits,
measured image sizes and bounded SPIFFS source-loading/activation APIs.
Optional transfer compression belongs to the installer, not the bot core.
The separate opt-in `Xiao_S3_WIO_onchip_beta` adds authenticated native RF/web
administration, durable source/help installation and rollback through one
mast-owned backend. See [the beta administration guide](MAST_ADMIN.md) for
Make targets, current hardware evidence and remaining limits.

### WiFi loss and recovery

The radio retries association every 30 seconds while disconnected. Association
without an IPv4 address gets a 120-second DHCP interval before another attempt.
There is one retry owner; Arduino automatic reconnect is disabled. An invalid
saved credential record disables automatic joining until native `wifi apply`
supplies valid saved credentials. Retrying never formats storage, erases
credentials, creates an identity or changes the LoRa profile.

After AP or IP loss, old KISS and companion TCP sessions are retired. A restored
address starts the KISS listener and republishes mDNS. Failed listener,
dashboard or discovery starts retry at 30-second intervals. The HTTP and
companion listeners bind all local addresses; their workers remain running
across address changes. Dashboard network status and telemetry eligibility
follow usable association **and IPv4**, not association alone. The next
scheduled telemetry sample can use the restored connection.

Repeater, room, management, local bot and queued UART work continue over RF.
Companion RAM replay history remains available to a new TCP session. A TCP
client must reconnect; it must not blindly repeat an uncertain management
change or queued transmission. For the official app, use
[Auto Reconnect](../shared/ANDROID.md).

Run the targeted SDK and radio-session checks with:

```bash
make -f test_support/phy_parity/Makefile wifi-recovery-test stream-test
make -C test_support/companion_sessions test
```

### Shared PHY boot authority

The private operator profile must explicitly supply `ONCHIP_RADIO_FREQ_MHZ`,
`ONCHIP_RADIO_BW_KHZ`, `ONCHIP_RADIO_SF`, `ONCHIP_RADIO_CR` and
`ONCHIP_RADIO_TX_POWER`. The example stages **912.525 MHz, 250 kHz, SF7, CR5,
2 dBm**. Its `LORA_*` aliases derive all native defaults from those same values.
Missing settings or conflicting native defaults fail compilation; an old
private profile must be updated, not silently inherit upstream regional values.

This tuple is a required boot contract. On an unprovisioned device, the mux
commits it to NVS before enabling roles or queued clients. A matching saved
profile is restored without rewriting it; saved aggregate factor, CAD and
interference policy remain authoritative. A conflicting saved PHY, corrupt
record or failed commit refuses startup before applying that saved configuration
or sending role adverts. Radio-only boot policy is unchanged.

The combined image does not allow KISS clients to retune its committed PHY,
even if they claim configuration ownership. It accepts an identical CONFIG
SET without writing flash. To change the PHY, configure the image to match an
approved existing tuple or commit the new tuple with a radio-only image before
installing the combined image. Do not erase NVS to resolve a PHY mismatch: it
also contains durable role identities. Native roles reflect the mux profile;
repeater/room initial adverts are queued only after their readbacks synchronize.
Negotiated KISS clients can query read-only CAPACITY to see the four available
external TCP slots and four internal sources (three applications plus RF management).
The command-bot environment advertises five internal sources.

**Beta exception:** `MESHCORE_MAST_ADMIN=1` makes a valid saved PHY authoritative
after the initial explicit operator provisioning, so authenticated `radio`
changes survive reboot. Corrupt records still refuse startup, and KISS clients
still cannot retune. Temporary PHY changes never overwrite that durable
profile; automatic return advances the effective generation without a flash
write. Non-beta images retain the strict compiled/saved tuple match above.

### Saved on-device roles

The combined image reads one boot-time role profile before starting any native
role, companion listener or observer. With no profile saved, all existing roles
run as before. A profile can enable any combination of `repeater`, `room`,
`companion` and `observer`, including repeater+companion, repeater+room,
repeater+observer, a single role or none. Each built-in role runs at most once
on this image. KISS TCP, its `modem` identity, queued
UART (when configured), the radio scheduler and the dashboard are separate
from this selection. The dashboard keeps all six service entries and reports
unselected roles as `disabled`, without a public identity or source slot. The
existing `bot` entry is **only the KISS endpoint**, not a command bot. The
always-on `management` entry has its own NVS identity and radio source,
independent of all four selectable roles, the KISS bot and WiFi.
The opt-in bot image adds a seventh `command-bot` dashboard entry and a
separate `mc-onchip/command-select` next-boot selection. It does not change
these four role bits or the signed role-management journal.

Bits 0-3 represent repeater, room, companion and observer;
`RoleProfile::All` remains the default. Legacy version-1 profiles are read
unchanged. Signed RF changes write a version-2 checksummed single-key NVS blob
under `mc-onchip/role-profile`, containing mask, uint64 generation and uint64
nonce, using `nvs_set_blob` and `nvs_commit` in one transaction. Only a
subsequent boot applies the saved mask. A legacy local `saveRoleProfile()` is
refused once a signed generation exists; it cannot reset replay protection.
The signed role-selection path is separate from native role admin login.
The baseline image has no HTTP role-setting endpoint; the beta/HTTPS images
also support next-boot selection through authenticated Management's shared
RF/web backend. See [mast commands](MAST_ADMIN.md#common-control-commands).

### Signed RF role selection

First set `KISS_LOCAL_SOURCES=4` in an existing private
`firmware/platformio.onchip.ini` (the tracked example already does this).
With `cryptography` installed (`python3 -m pip install -r
firmware/esp32/requirements-sign.txt`), generate an **operator signing seed
off-device and outside this repository**:

```sh
python3 tools/hardware/rf.py init-key \
  --key-file "$HOME/.config/meshcore/mast-role-operator.seed"
```

Back up the mode-0600 seed securely. Put **only** the printed 64-digit
verification public key in the private PlatformIO profile, e.g. add
`-D ONCHIP_OPERATOR_PUBKEY='"${sysenv.ONCHIP_OPERATOR_PUBKEY}"'` under
`build_flags`, export that public key while building, then run
`make aspen-firmware`. No operator signing seed or management private key
belongs in that build profile or on the sender's gateway. If the verification
key is absent, the management entry reports `unprovisioned` and all RF
commands are ignored. Invalid key syntax refuses combined startup.

Read the **management** `public_key` (not the repeater, room, companion,
observer or KISS bot key) and `profile_generation` from the mast's
`/api/status` roles array before visiting the remote site. You can then
send one signed request from an **independent MeshCore KISS radio** tuned to
the same PHY; the gateway host is not the target mast:

```sh
python3 tools/hardware/rf.py send \
  --key-file "$HOME/.config/meshcore/mast-role-operator.seed" \
  --target MANAGEMENT_PUBLIC_KEY --generation 1 \
  --roles repeater,companion --gateway OTHER_RADIO_IP --port 8001
```

Read the saved and running masks over RF, including when mast WiFi is down:

```sh
python3 tools/hardware/rf.py status \
  --key-file "$HOME/.config/meshcore/mast-role-operator.seed" \
  --target MANAGEMENT_PUBLIC_KEY --gateway OTHER_RADIO_IP --port 8001
```

The signed, encrypted status reply returns `profile_generation` as a decimal
string, `saved_roles`, `applied_roles` and `sealed` as JSON. A query uses its
own random nonce, does not change roles or write NVS, and works even after a
failed update has sealed further writes. If NVS cannot be read, the command
reports unknown state rather than returning the last cached profile. Both
commands require a gateway that can carry unscoped floods in both directions;
the same operator key authorizes queries and changes.
The signed query contains `MCORE-ROLE-QRY-V1 || target_pubkey[32] ||
nonce:u64le`. Its encrypted reply signs `MCORE-ROLE-STAT-V1 ||
management_pubkey[32] || sender_pubkey[32] || nonce:u64le ||
saved_generation:u64le || saved_mask:u8 || applied_mask:u8 || flags:u8`.
Flags bit 0 says NVS readback is valid and bit 1 says writes are sealed;
invalid readback has zero generation and masks, not a cached estimate.

Use `--roles none` to disable all applications; management remains reachable.
Use a generation strictly greater than the saved generation, never reset it.
The tool chooses a random nonzero nonce by default (or use `--nonce N`) and
prints it. `--dry-run` produces a frame without connecting. Requests are a
single native MeshCore encrypted `ANON_REQ` **flood** packet, not a custom raw
payload; repeaters that forward unscoped floods can carry them. Management
rejects transport-scoped floods before saving a profile: transport codes
depend on each packet's payload, and the mast does not have a scope key to
produce a valid scoped receipt. A route limited to scoped floods cannot
deliver this command and its receipt. The 65-byte signed
domain `MCORE-ROLE-RF-V1 || target_pubkey[32] || generation:u64le ||
nonce:u64le || mask:u8` carries the **full target key**, followed by a 64-byte
Ed25519 signature and zero AES block padding. The operator signing key is
separate from the management device's NVS identity. The native two-byte
transport MAC is only an encryption check, never management authorization.

The receiver accepts at most one authenticated request or status query per
second; wait at least one second before querying a recent update. Malformed
requests, unknown role bits, wrong targets and bad signatures produce no
reply. A valid new generation commits to NVS before the mast sends a signed
`success` receipt; a valid repeat with the same saved generation, nonce **and
mask** returns `already-applied` without writing NVS. A valid stale or
conflicting request, or an NVS commit failure, returns `failure` (the latter
also seals further updates until reboot). All three receipts bind the full
management and ephemeral sender keys, generation, nonce and outcome under
the signed domain `MCORE-ROLE-RCPT-V1`, then travel encrypted as a native
MeshCore `RESPONSE`. Unscoped flood requests get an unscoped flood reply so
other repeaters can carry it back; a zero-hop request gets a zero-hop reply.
The mast sends at most one
response per accepted request, with only one outstanding management receipt;
an unavailable radio or full queue can prevent its delivery. It does not
retransmit a receipt automatically.

The sender waits up to 180 seconds for a receipt on the independent
gateway's RX KISS stream (`--wait-seconds` accepts 1..3600), retaining its
ephemeral private key until it decrypts, verifies and correlates the
signature. Gateway TX completion **is not** a mast receipt. If the deadline
expires, disconnects or the reply is lost, the RF/NVS outcome remains
unknown; closing the gateway connection can also cancel an unstarted TX.
Never automatically replay an uncertain request, especially with a new
generation or nonce. Inspect `profile_generation` or the saved role mask
with `status` or inspect NVS when connectivity returns. A verified success or
already-applied receipt confirms the mast saved the change, **not** that the
new role mask is running:
only the next successful boot applies it. The management identity and
journal survive application role disable/re-enable and WiFi loss. Overlapping
mast and host roles are permitted and reported to the operator, not blocked.
A bot program installer and scripting runtime are not yet included.

An unreadable, malformed, unsupported-version or corrupt saved profile logs
an explicit error and stops combined startup; it does **not** fall back to all
roles. Missing storage alone selects the legacy all-roles default without
writing a record. Disabling a role leaves its existing NVS key and SPIFFS
preferences, contacts and other files untouched. Staged identity changes and
reset intents remain pending until that role is enabled on a later boot;
disabled roles do not attach to the mux, initialize native state, advertise,
or receive scoped lifecycle commands. With companion disabled, TCP 5000 is
not bound and its network/diagnostic worker does not start. With observer
disabled, MQTT does not connect or load/activate its identity. Ensure a host
and mast do not enable the same repeater identity simultaneously: this profile
is not an exclusive cross-device placement or ownership protocol.

### Explicit blank-filesystem provisioning

Prepare the build using the private operator profile first:

```sh
make aspen-prepare
make -C firmware/esp32 buildfs \
  UPSTREAM="$PWD/.tmp/onchip-upstream" BUILD="$PWD/.tmp/onchip-MeshCore"
```

`buildfs` preserves that prepared configuration and uses its board/partition
settings to create `.pio/build/Xiao_S3_WIO_onchip/spiffs.bin`. The dedicated
`onchip-data` input contains only the non-secret layout marker, not identities,
credentials, contacts or example ACLs. The example explicitly selects SPIFFS.

For **blank hardware only**, the operator can run the following explicit
provisioning operation after choosing the actual upload port:

```sh
make -C firmware/esp32 provision-fs \
  UPSTREAM="$PWD/.tmp/onchip-upstream" BUILD="$PWD/.tmp/onchip-MeshCore" \
  CONFIRM_FS_ERASE=erase-spiffs UPLOAD_PORT=/dev/CHOSEN_PORT
```

This erases/replaces the entire SPIFFS partition, including any existing role
preferences, ACLs, contacts and channels. It does not erase NVS identities or the
saved PHY profile. It is never a dependency of firmware build/upload and is not
run automatically on mount failure. Provision SPIFFS before the first combined
boot; subsequent firmware updates retain it and must not repeat this operation.
The root `aspen-buildfs` and `aspen-provision-fs` targets invoke these recipes.

## Applications and storage

The native MyMesh implementations provide routing, adverts, authenticated
administration, ACLs, room login/post/ACK/retry and companion
contacts/channels/messages. Native source changes cover symbol isolation,
hardware ownership, identity bootstrap and scoped lifecycle. The native
companion offline queue remains intact; `CompanionSessions` is its sole
collector and TCP frontend.

Each role gets a checked NVS identity, a separately managed monotonic clock, its
own mesh tables/packet pool and a filesystem view rooted at `/repeater`, `/room`
or `/companion`. Identity creation uses ESP hardware entropy after WiFi driver
initialization; association with an AP is not needed. Corrupt/unreadable
identity records stop startup rather than generating a replacement. NVS commit
failure is an error. Normal native preferences/ACL/contact/channel files retain
native storage semantics; they are not the Go host's envelope store. Room post
history and guest membership retain the native volatile/reload behaviour.

Power-off, OTA, whole-device formatting, shared-radio changes, shared
hardware/stat reset, GPS/sensor mutation and unbounded packet file logging
are rejected explicitly. Companion private-key import/export, signing and BLE
PIN changes remain rejected; no privileged RF shortcut is added.

### Memory and persistence tiers

Use the device's different memory tiers according to their actual requirements,
not one interchangeable heap or a larger NVS partition:

| Tier | Placement and lifetime |
|---|---|
| Internal RAM | Radio/DMA buffers, ISR/cache-disabled state, task stacks, synchronization and SDK allocations that require internal memory. Preserve working space for WiFi and radio while HTTPS is active. |
| PSRAM | Task-owned role tables and packet pools, companion contacts, Lua heaps, dashboard snapshots, source/HTTP buffers and storage scratch space. Reuse large buffers instead of repeatedly allocating them. Copy packets into the mux's internal buffers before physical TX. |
| SPIFFS | Bulk durable state: native contacts/preferences, Lua source and telemetry endpoint/certificate data. Publish verified files through the existing recovery mechanisms; a successful RAM update is not a durable commit. |
| NVS | Small identities, settings, replay/recovery metadata and active-file references. Do not store large certificates or growing application datasets here just because the NVS API is convenient. |
| Go host | POSIX files and ordinary host RAM implement the same logical identity, scope, transaction and source-lifecycle contracts. ESP32 physical memory limits are not host hardware measurements. |

For matched image sizes, component attribution and internal-RAM/PSRAM
lifetimes, see the [device resource budget](resource-budget.md).

**Already implemented:** native role aggregates are PSRAM-only, as described
below. Telemetry persists its 4,620-byte endpoint in two verified SPIFFS slots
and publishes a 40-byte NVS reference; legacy endpoint blobs migrate without
erasing identities or lowering storage safety reserves. Lua and storage
workspaces also use PSRAM. These are volatile allocations, not another
persistence authority.

The combined ESP32 profile now provides **256 companion contacts**, up from
64, using an additional **36,096 bytes of PSRAM**. Its companion allocation
grows from 26,752 to 62,848 bytes without changing the native contact file
format or removing existing records. The native capacity byte advertises
`MAX_CONTACTS / 2`, so 256 is representable; 512 is not. Custom profiles may
select another positive even capacity up to 510. There is no automatic
eviction or corresponding change to the nRF52's capacity.

TLS admission now probes the SDK's two actual record-buffer allocations and
requires a 68-KiB aggregate work budget plus 32 KiB for radio/network work.
It does not require one 64-KiB block or invent extra large buffers for the
remaining handshake work. Local admission and SDK allocation failures report
`heap`, separately from connection failures; see [TLS admission](TELEMETRY.md#a-write-fails-without-an-http-response).

Configured CA date parsing runs before TLS client creation. A 4,120-byte PSRAM
cache retains the exact certificate bundle and its date bounds; every connection
still checks current trusted time and performs normal certificate/hostname
verification. This avoids reparsing certificates alongside live TLS buffers.
`telemetry tls`, `telemetry tls-heap` and `telemetry tls-blocks` distinguish
admission, connection and certificate failures without exposing credentials.

**Bot KV storage:** verified SPIFFS groups hold the 32 caller/conversation/channel
slots and eight bot-global slots. One 200-byte NVS authority selects the
committed group files. The storage worker rereads that authority but reuses
verified PSRAM while it is unchanged; mutations and uncertain publication
invalidate the cache. Scope quotas, transactions and scoped exports/restores
remain shared with the native host implementation. Timer and reminder payloads
still use NVS. See [KV storage](../runtime/BOT_RUNTIME.md) for file layout, capacity and
failure behavior.

Bot completion/admission/fault logs use the existing asynchronous diagnostic
queue rather than blocking the radio dispatch loop on USB. `bot diagnostics`
reports queued/dropped log records; a full log queue does not discard commands
or their retained fault/statistics state.

Ordinary command cooldowns are **zero**, on both ESP32 and the native host
worker. The fixed sender/channel pauses and command-count limits are removed;
bounded jobs, deduplication and radio queue/airtime admission remain. Saved
bot airtime allowances still apply. Future user fairness should charge consumed
airtime and adapt to shared-radio load; no adaptive controller is implemented.

### Clock authority and recovery

On-chip roles never bootstrap time from the ESP system RTC or companion contact
metadata. Software reset and flashing can retain a nonsensical ESP epoch;
native `bootstrapRTCfromContacts()` can also restore an old, future-dated
`lastmod`. Preparation generates a UTC build-day baseline instead. This is an
explicit offline estimate, not synchronized wall time. `SOURCE_DATE_EPOCH`
selects that day for reproducible builds; supported baselines are May 2024
through January 2100. Native contact records, including their timestamps, are
preserved byte-for-byte by this clock guard.

Standard SNTP starts asynchronously using `ONCHIP_SNTP_SERVER` (default
`pool.ntp.org`) and `ONCHIP_SNTP_INTERVAL_SECONDS` (default 3600, range
60..86400). Set the server to an empty string to disable it, or select a local
NTP server for an isolated network. The SDK callback only replaces a bounded
latest-sample mailbox; the dispatch loop applies it without waiting for DNS,
network time or an RF operation, accounting for elapsed time since receipt.
SNTP is standard unauthenticated network time; select a trusted server/network.
No new listener or socket-table reservation is
needed: SDK SNTP uses the lwIP UDP API. Native companion DeviceTime and
authenticated repeater/room time commands remain usable offline and retain
their forward-only rules. An explicit native time value is not silently clamped
or substituted.

Installing this image repairs the old seed on its first software boot, without
a power cycle, identity erase, contact erase or PHY change. Role clocks and their
unique-timestamp counters are reconstructed **before** native applications load
state. Native ACL replay timestamps are transient, not saved in `s_contacts`;
normal reconstruction therefore removes the old future login watermark while
retaining the native durable ACL. Room posts and non-admin room membership have
their normal native volatile lifetime. This is a boot repair boundary, not a
routine replay-state reset: scoped role reboots keep their clock, and neither
SNTP nor a contact refresh moves a running clock backward or clears a member's
replay watermark. Deliberately setting a future time through an authorized native
command still has native consequences; a software boot returns to the managed
baseline. No new remote clock-reset shortcut is exposed.

`GET /api/clock` uses the existing dashboard HTTP server and returns
`sampled_at_ms`, `build_epoch`, `sntp_enabled`, `last_sntp_epoch`, `sntp_age_seconds`,
`rejected_samples`, and three role entries with `role`, `epoch`, `source`
(`build`, `native`, `sntp`), `synchronized` and `enabled`. Disabled clocks are
not started or synchronized and report `enabled: false`, epoch zero.
`last_sntp_epoch: 0` means no accepted sample. `synchronized` requires a sample no older than two configured
intervals and agreement within two seconds; an ahead-of-network role is visible
as unsynchronized rather than silently rewound. HTTP returns 503 if the dispatch
snapshot is busy or more than three seconds old. Samples before the build
baseline or after January 2100 are rejected with a diagnostic. Loss of network
time does not stop offline roles. There is no flash time journal: a cold or
software boot without network/native synchronization starts at the build day,
not an invented persisted wall time.

### Role-scoped lifecycle

Native role state lives in explicitly allocated PSRAM, not the internal heap.
Each role owns one lifetime block containing its native application object
(including ACLs, room post history or companion contacts/channels/offline
frames), its unchanged 32/32/16-packet pool and queues, and its duplicate table.
Allocation uses only `MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT`; missing/exhausted
PSRAM logs the role and requested size, leaves that role faulted/offline and
uses the existing five-second retry. There is no fallback to internal RAM,
capacity reduction, identity replacement or WiFi-association prerequisite.

Block sizes follow the configured contact, history and packet-pool capacities.
Keeping these aggregates in PSRAM reserves internal memory for networking and
hardware operations. The dashboard reports current and minimum free memory,
including DMA-capable memory and its largest available block.

The dispatch task allocates before mux attachment and native construction.
Failed attachment or identity loading releases the allocation. On restart,
the mux generation is retired first; companion sessions are reset before native
destruction, then the entire role block is scrubbed and freed. Reconstruction
obtains a fresh block, so stale callbacks cannot refer to retained old storage.
Task-only native packets are copied by `LocalRadio::queueTransmit()` into the
mux-owned job before admission returns; PSRAM pointers do not reach physical
RF DMA. No ISR or cache-disabled callback accesses this storage.

`LocalRadio` RX/result rings, mux and physical buffers, companion network state
and its spinlock, observer/FreeRTOS queues, task stacks and SDK/crypto/DMA
allocations remain internal. Their capacities and ownership are unchanged.
SPIFFS and NVS operations retain native storage semantics; PSRAM holds only
the transient working state. The native lifecycle harness checks allocation
capabilities, exhaustion at boot/restart, partial-start cleanup, scrub-before-free,
bounded recovery, real wire behaviour and generation fencing under sanitizers.
Monitor internal and DMA-capable free memory under WiFi, MQTT, companion and
dashboard load, especially during reconnects.

Authenticated native repeater/room `reboot` flushes native preferences and ACL
state, retires that mux source, destroys the native application and reconstructs
it on the next loop pass. Sibling roles, physical radio configuration and the
aggregate airtime budget remain live. Native packet pools use fixed storage and
the original queue algorithms, so repeated reconstruction does not leak the
upstream firmware-lifetime allocations. In-flight transmissions cannot be
recalled, but their old generation cannot complete or charge a replacement
source. Volatile native state resets: room posts and non-admin room membership,
duplicate tables, connection state and pending messages are not a new durable
store.

Native `set prv.key HEX` runs only after native administrator authorization,
including CLI prefix/whitespace normalization. It validates and durably stages
the 64-byte private key before returning the native success/public-key reply.
The current application keeps its old identity until reconstruction. A failed
stage or state flush does not return success or restart the role. The existing
local-only `get prv.key` and `erase` timestamp gates remain local-only; this
image does not add an RF route or shared diagnostic-console shortcut to them.

The common authenticated mast backend also exposes runtime
`role name ROLE`, `role key ROLE [pending|rotate]` and
`role channel ROLE SLOT` controls for bot/repeater/room/companion.
Names persist without rebuilding; deliberate generated identity rotations
use the same staged journal and require restart. Native preference editing
requires an active role. Bot and companion support 128-bit application
channel keys (bot applies at restart, companion live); repeater and room
report no application group-channel key table. An operator-supplied native
identity can be staged with `key ROLE HEX128` through authenticated encrypted
Management RF only, including the Management identity itself; restart applies
it. Read back the pending full public key before reboot. Web/Lua imports and
private-key export remain unavailable. See
[operator-supplied identities](MAST_ADMIN.md#importing-an-operator-supplied-identity)
for protected-file handling and recovery.
The Management password can also be rotated at runtime through encrypted RF
with `password HEX`; it applies to new RF/web logins, while existing sessions
end on expiration or reboot. See
[Management password changes](MAST_ADMIN.md#changing-the-management-password).
`role name management TEXT` and `role name kiss TEXT`
persist/apply the administrator and KISS service names independently of optional
roles. `role advert ROLE zerohop` queues an explicit native zero-hop advert for
repeater/room/companion/bot/management; KISS and observer are not advert targets.
Queued does not mean delivered or learned by a peer. The shared multiplexer remains the sole physical
RF authority. See [runtime role configuration](MAST_ADMIN.md#runtime-role-names-and-keys)
for exact commands, failure semantics and old-identity bot-data behavior.

Companion CMD19 with the exact `reboot` token and CMD51 with the exact `reset`
token schedule companion-only operations. Both TCP clients are retired, all
frontend cursors/journal/request state is cleared, and new clients establish
fresh native sessions. The listener/network worker remains running. Reboot
retains identity, preferences, contacts and channels. Reset erases only
`/companion/` files and replaces only the companion identity; sibling prefixes,
NVS identities and the shared PHY profile remain untouched. Accepted commands
use native disconnect semantics, not a fabricated success frame.

One bounded intent per role is processed after its current native handler
returns. Reset commits its intent before stopping the role, scans at most eight
directory entries and removes at most one owned file per loop pass, then
activates the replacement identity only after erasure finishes. A storage
failure leaves that role offline with a diagnostic and a five-second recovery
retry; siblings continue. A failed reboot flush leaves the old role running.
Companion flush failures return a native file-I/O error to the requester.

Active identities retain the existing raw 64-byte values under the same
`mc-onchip` NVS keys. A staged key or reset temporarily uses a versioned
133-byte record containing active/pending keys and flags in that **same key**.
Reset recovery after power loss uses this marker, not a second storage
authority. Successful activation restores raw64 storage. Older images cannot
read pending records: finish lifecycle recovery before downgrading. Native
SPIFFS preference/contact files retain their existing non-transactional write
semantics.

## One physical scheduler

`KISS_LOCAL_SOURCES=4` allocates embedded source sessions independently from
`KISS_MAX_TCP_CLIENTS=4`. `LocalRadio` implements the PHY-owned queued Radio API
without TCP or WiFi dependency. Local and remote jobs compete in the **same**
12-job multiplexer queue, with native priority, delay, expiry, source factors
and one shared physical airtime budget. The native dispatcher does not run
another budget/CAD scheduler in queued mode.
Management reserves one source on every boot before the optional roles start.
Its native packet pool has four entries in about 3 KiB of PSRAM; its source has
eight fixed RX entries. Management sends no adverts or other unsolicited RF
packets, and adds no network socket or worker task.

Each local source has eight bounded RX entries and reserves separate admission
and terminal-result capacity for its transferred jobs. Job IDs cannot wrap and
be reused within a source lifetime. Only confirmed RF success fans out a local
reflection to sibling sources and external KISS clients. A source never receives
its own reflection. Real RX retains RSSI/SNR; local adapter measurements are
NaN for reflections, while native byte notifications use the explicit `80 7f`
unmeasured marker. Radio status reads the physical driver's last measured
signal rather than casting those NaNs to integers. Local reflection is marked and
cannot be RF-forwarded by the patched native dispatcher. Local RX overflow is
counted as receive loss. Rejected/failed/unknown TX is not reported as success.
Native queue status and core JSON report the source's actual waiting arbiter
jobs, not its dispatcher packet pool. Pending-work checks include its current
physical transmission. No second airtime budget is reconstructed.
`LocalRadio::getQueuedRadioStats` reads the mux's authoritative source and
aggregate credit, TX RF accounting, success/failure counters, generations and
aggregate queue/activity directly on the radio loop. Reading does not refill
credit or change scheduling. Unavailable snapshots leave the output untouched.
Native dispatcher RF totals and effective credit use this snapshot, not their
unused local scheduler state. RF accounting includes known timeout charging;
it is neither estimated TX duration nor RX airtime. Generated binary status and
radio-stats JSON preserve `UINT32_MAX` for unavailable TX seconds instead of
dividing the unknown millisecond sentinel into a plausible duration.

`KISS_STREAM_ENDPOINT=1` adds one separate queued UART source, consuming neither
a TCP slot nor one of the four embedded slots. It uses HardwareSerial1,
with a 1024-byte RX buffer and 512-byte TX buffer, independently of diagnostic
USB and WiFi association. The radio loop polls it with bounded reads/writes;
there is no blocking flush or separate radio scheduler. Set the flag to zero
to omit it. Pin and baud definitions are mandatory when enabled.
HELLO renews only the UART source generation; aggregate airtime credit survives.
Old queued jobs are retired, and an old in-flight transmission cannot complete
a new-session job. The adapter requires an explicitly committed physical radio
profile; compiled defaults alone do not pass its readiness check.

A HELLO clears software output, not bytes already in a hardware UART FIFO.
After a timed-out uncorrelated control request, the remote nRF52 caller must
establish a real transport reset/drain before calling `onLinkConnected()` again.
There is no blind automatic recovery or retransmission of uncertain jobs.
See [the queued adapter contract](../shared/RemoteKissRadio.md) for lifecycle details.

## Network endpoints and resource limits

- KISS TCP: port 8001, four external slots, usable by an external bot.
- Companion TCP: port 5000, two clients, native `<`/`>` framing.
  Synchronous replies, including SENT, and multipart contact enumeration stay
  bound to the requester's connection generation. RF response pushes are shared
  events sent to all currently connected clients, including newcomers; they
  are not requester-private or durable request correlations.
  The companion-owned `CompanionSessions` collector drains the
  native global offline queue into a bounded shared journal with independent
  client cursors; it owns version conversion, asynchronous fanout and TCP
  backpressure on its own network task. No second transport or native queue
  override is installed by the role integration.
  RF queries that clear native pending-request state share one admission lease;
  conflicting clients receive native `ERR/BAD_STATE`. Requester disconnection
  does not release it. Its native SENT timeout releases admission, not queued
  RF work; legacy same-peer late replies can remain ambiguous after expiry.
  Its diagnostic callback copies into an eight-entry queue without waiting;
  a separate 4 KiB-stack task performs USB output, so the callback itself never
  waits for console I/O. Diagnostic loss is counted
  and reported when the console drains. These tasks have firmware lifetime.
- HTTP permits two live WebSocket clients and three total client sessions,
  leaving one ordinary HTTP slot when both live clients are connected.
  Active sessions are not LRU-evicted.
- Optional MQTT uses a distinct, persistent observer identity in the existing
  `mc-onchip` NVS namespace. Default topics follow the host observer convention:
  `meshcore/<observer-public-key>/packets` and retained `.../status`
  (`online`/`offline`, with an offline last will). Set
  `ONCHIP_MQTT_TOPIC_PREFIX` to change `meshcore`; the legacy
  `ONCHIP_MQTT_TOPIC` override still selects an exact packet topic.
  The [observer-compatible formats](OBSERVER.md) instead use IATA-qualified
  packet/status topics, JSON presence and RF-only packets; format 0 keeps the
  internal fields and transmission outcomes described here.
  Retained `.../roles/repeater`, `.../roles/room`, `.../roles/companion`,
  `.../roles/observer`, `.../roles/bot`, `.../roles/management` and, in bot builds,
  `.../roles/command-bot` carry the same public service records as the dashboard.
  Records republish after reconnect; consume the observer
  presence topic alongside retained records to detect a disconnected device.
  The eight-event queue and one latest-status slot separate RF from the
  8 KiB-stack MQTT task. Combined firmware reserves a 16 KiB radio-loop stack
  for native packet processing, crypto and synchronous observer callbacks.
  Offline/backpressured observations are counted and
  dropped; reconnect is SDK-owned. Publishing remains QoS 0, without an
  accumulating retry outbox. Packet JSON includes `event_id`,
  `observer_identity`, `raw_packet_hex` (and the existing `raw` alias),
  `packet` metadata or `decode_error`, direction, uptime, signals, TX state,
  cumulative packet loss and separate publication errors. `timestamp` uses
  fresh network time when available and is `null` otherwise. RX has no
  invented transmitting-role identity.
  TX `source` includes slot/generation and service public identity captured at
  admission, so a late completion cannot be attributed to a replacement role.
  Source identifies the local submitting service, not the authenticated origin
  of the encrypted payload. A fixed 13-entry attribution table covers the
  arbiter's twelve waiting jobs plus one physical transmission.

### Service identities and readiness

Combined `/api/status` and `/api/live` snapshots add a bounded `roles` array:
six service records, or seven with the command bot. Each record has `role`,
mutable native `name`, lowercase
`public_key` (or null when unavailable), `state`, `ready`, `fault`,
`source_slot`, `source_generation` and `profile_generation` (a decimal
**string** to preserve the full uint64 range; meaningful for management).
The on-device services card displays
these records; recent activity matches native source slots by generation, not
by a misleading KISS client number. Radio-only JSON has no `roles` field and
its page hides the card.

Repeater/room/companion readiness reflects native lifecycle and shared-radio
availability. Faulted, recovering and rekeyed roles update without querying
private storage from HTTP. A refused reboot can leave the role running with a
fault describing the failed operation; a companion session-bridge fault makes
that service not ready. Observer readiness requires a connected publisher
and successful online publication; an empty MQTT URI displays `disabled`.
The KISS bot record uses the **existing `modem` NVS identity**, the same key
served by native KISS GET_IDENTITY/sign/ECDH operations. It does not create a
second bot key or replace an existing identity; it does not run commands.
Its readiness also checks the KISS listener, WiFi and radio profile.
Names and public keys are safe telemetry;
private keys, administrator passwords and MQTT credentials are never included.

The combined profile fits the **stock 16-socket SDK** without overriding its
configuration: four KISS clients + one KISS listener + three internal HTTP
sockets + three HTTP clients + two companion clients + one companion listener
+ one MQTT connection + one spare = 16. Three local radios and UART use zero
sockets. `Capacity.h` checks the selected limits, the actual companion class
capacity and the HTTP configuration against the SDK at compile time.
Radio-only deployment retains eight KISS clients and its original budget;
standalone companion tests may use four clients, outside the combined profile.
Monitor free memory and socket availability under simultaneous network load.

## Verification

`make -C firmware/esp32 test` executes the existing actual-KissModem arbiter
tests plus in-process local-source tests: priority,
bounded shared admission, WiFi-independent local traffic, terminal airtime,
success-only reflection, physical metadata, authoritative queue status and
command boundaries. The existing real-modem UART suite also runs with four TCP
slots and four local sources, covering renewal, in-flight fencing, partial
writes, overflow, budgets and coexistence. The local integration suite checks
escaped physical RX bytes and signal metadata with zero TCP clients connected.
Compile checks accept the exact
16-socket profile and reject eight combined KISS clients, four combined
companion clients and a 15-socket SDK. Build-path checks reject production and
PHY-less targets, including symlink aliases.
`make -C firmware/esp32 local-test` selects the local adapter, actual native
Dispatcher statistics and generation/budget/unknown-value checks without
rebuilding the separate Remote UART and HTTP suites.
`make -C firmware/esp32 lifecycle-test BUILD=ABSOLUTE_BUILD_TREE` executes the
complete generated native applications on Linux with hardware-only filesystem,
NVS, entropy, clock and board facades. It uses the firmware-installed base64
dependency and existing PHY crypto fixtures. Scenarios cover encrypted native
login/admin/guest commands, normalized rekey failure/success replies, identity
activation and ACL reload, real two-client TCP reboot/reset, in-flight source
retirement, scoped erase failure and durable recovery. Boot-profile cases cover
the default, selected combinations, single/empty profiles, atomic commit/read
failures, corrupt/unsupported records, non-hot-switch behavior and preservation
of pending role identities, companion files, disabled status, source slots and
unbound companion port until reenable. It also reconstructs each application
100 times and checks packet-pool recovery and sibling source
generations. Observer SDK-boundary tests also exercise persistent observer/bot
identities, retained role topics, reconnect, bounded drops, maximum packet
serialization and attribution across an in-flight rekey. The shipped JavaScript
renders the actual native snapshot fixture, including fault and radio-only
states. `TEST_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer'`
enables sanitizer coverage; leak detection remains enabled. These scenarios
also run as part of `test`.
`make -C test_support/companion_sessions test` exercises real-TCP companion
sessions at this profile's two-client capacity.

The image includes all three native applications. Link-time RAM is not runtime
peak heap. Check SPIFFS/NVS recovery, MQTT reconnect, RF room/repeater delivery,
minimum free heap and simultaneous network capacity on a spare device; see
the root README for standalone readiness and RF checks.
