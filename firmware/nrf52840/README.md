# Standalone XIAO nRF52840 repeater and command bot

Run the shared production Lua bot and a MeshCore repeater on the Seeed XIAO
nRF52840 with Wio SX1262 using **`nrfmast_fleet_lua`**. Both share one radio and
keep their existing separate identities. Use an ordinary MeshCore mobile app
connected to another companion radio: open Pine-Relay's authenticated admin
console for `bot status` and `source status`, then send Pine-Bot `!ping` and
`!help` as encrypted DMs. No wired host, WiFi or nearby BLE connection is needed.
Start with [production Lua setup, limits and recovery](PRODUCTION-LUA.md).
Current Lua source builds also provide `backup help` through authenticated
repeater administration. Use the [encrypted RF node backup workflow](../../NODE_BACKUP.md)
to download InternalFS identities/settings and QSPI source/data without USB
access; retain the recipient's operator seed.
The [public Pine bundle](https://ve6slp.ca/projects/meshcore/#downloads),
`1.17.1-slp-pine` from earlier source `8b41d9c`, is **update-only**. It requires
initialized repeater/bot identities and credential records; it does not
commission a blank board. Application-only updates retain InternalFS and
QSPI Lua source/data. Never erase those filesystems to install an update.
Use `stats sensors`, `stats radio` and `stats memory` in the authenticated
repeater or local USB console to read BAT voltage, MCU temperature, radio
counters and heap. `stats help` lists the read-only [stats API](../shared/STATS.md)
pages; measurements retain their units and unavailable values are explicit.

For a nearby **no-wire firmware update**, use Pine-Relay's authenticated
administration to run `get update`, then `start ota confirm`, and transfer the
app-only `firmware.zip` with a Nordic Legacy DFU phone tool and Pine's saved BLE
PIN. [BLE field updates](BLE-FIELD-UPDATE.md) covers provisioning, package checks
and aborted-transfer recovery. Entry expires after 120 seconds; the retained
single-bank bootloader has no rollback or transfer deadline, so keep power
stable and the phone available until Pine returns. **Keep USB recovery
accessible: an interrupted transfer can require USB, so inaccessible
installation remains on hold.** No bootloader, SoftDevice or QSPI layout
change is needed.

These on-device nRF builds use the **`slp-pine`** profile. Repeater `ver`,
native owner information and companion device information identify
`1.17.1-slp-pine`; the companion protocol remains v13. `help` lists the
administration entry points. WiFi commands report that this platform has no
WiFi rather than exposing nonfunctional configuration.

```sh
make -C firmware/nrf52840 build ENV=nrfmast_fleet_lua \
  LUA_ARCHIVE=/path/in/your/project/lua-5.5.1.tar.gz
```

The other `nrfmast{,_rx,_fleet}` images retain the small native note bot.
The default `nrfmast_rx` cannot transmit. Optional secured BLE connects a
MeshCore companion client using the existing bot identity while the repeater
continues forwarding. The [native notes and BLE guide](STATE-BLE.md) covers
those images, USB PIN provisioning and companion operations.
If Pine reports an occupied retired external volume, its USB owner can use
[explicit lab note provisioning](STATE-BLE.md#explicit-lab-provisioning-of-an-occupied-external-volume).
That operation can destroy old external files; it preserves CURRENT InternalFS
keys/configuration and imports current checked notes/replay records. Boot does
not perform it automatically.

To opt into measured-load bot admission, use local USB or authenticated
repeater administration: `bot adaptive on`, reboot, then `bot adaptive`.
The saved setting defaults to off. It reduces bot work under local RX/TX or
shared-queue congestion, divides airtime among authenticated callers, and
leaves notes unchanged when admission is refused. `bot adaptive off` and a
reboot restore the existing static behavior. Native companion messages,
ACKs, adverts and repeater forwarding retain their existing rules. See
[adaptive admission](../runtime/BOT_RUNTIME.md#opt-in-adaptive-airtime-admission)
for measurement limits, rejection status and a short field case.
The status `last` field keeps the last denial: `no-radio-metrics`,
`congestion`, `caller-share`, `caller-slots-full` or `reservations-full`.
It starts as `allowed`; after recovery, use `congested` and `pending` for
current conditions rather than treating `last` as an active fault.
The native-note images keep the repeater and static bot running if their saved
adaptive record cannot be read. `bot adaptive` reports `policy-fault=1` until an owner successfully
saves `bot adaptive off` or `on`; reboot applies that saved selection.

```sh
make -C firmware/nrf52840 baseline
make -C firmware/nrf52840 test
make -C firmware/nrf52840 build                 # nrfmast_rx: physical TX disabled
make -C firmware/nrf52840 build ENV=nrfmast     # RF-capable image
make -C firmware/nrf52840 build ENV=nrfmast_fleet # Pine Bot, RF-capable fleet image
make -C firmware/nrf52840 size ENV=nrfmast
```

PlatformIO, Python 3.12+, a native C/C++ compiler and the PlatformIO ARM toolchain
are required. `SOURCE=/path/to/MeshCore` can select an existing Git checkout;
only the pinned Git tree is exported, not its working changes. If the default
checkout is absent, preparation fetches the release into this directory's
ignored `.build/`. All generated sources, dependencies, logs and images stay
under `.build/`. Production Lua preparation exports the shared engine into
that private tree; it does not modify the selected upstream checkout.

The deployable files are
`.build/upstream/.pio/build/nrfmast{,_rx,_fleet,_fleet_lua}/firmware.{uf2,hex,zip,bin}`.
Use UF2 or the upstream nRF DFU ZIP with the appropriate bootloader; the raw BIN
is an application starting at `0x27000`, not a complete bootloader/SoftDevice
replacement. Hardware targets require an explicitly selected USB port; there
is no automatic serial-port selection. `upload-rx` is hard-muted; the separate
`upload-rf` target installs the RF-capable image.
**Do not flash the baseline image:** it is the upstream repeater and can transmit.

## Interoperability boundary

Public adverts, direct messages and routes **over LoRa RF** are the compatibility
boundary. The repeater and bot use ordinary MeshCore messaging with companion
radios and users, without requiring changes to their software or to
community repeaters. Ordinary discovery and messaging must not depend on this
project's host services or management extensions.

Our own management extensions, application payloads and management clients are
allowed, provided their traffic traverses existing MeshCore routing through
unmodified community repeaters. Relays must not need to recognize an extension,
rewrite its payload or implement new routing behavior. Extension-specific
authorization must not become a requirement for ordinary user interaction.

The supported board is the XIAO nRF52840/Wio SX1262.
This image reuses upstream packet/authentication
behavior and native repeater administration; bot commands are ordinary message
text. The build and native checks pin 1.17.1. Later-release and extension
interoperability need checks against the selected radios and relays before
deployment.

## Hardware and management

The build exports MeshCore `companion-v1.17.1`,
`d92964352441e53b93e8667b802e04f6e072b39e`. Core routing is unmodified;
the build makes the repeater's existing `handleCommand` virtual so its native
authenticated/encrypted admin CLI can also dispatch the small bot settings.
Native ACL, message authentication, replay checks and replies are retained.
It uses its real `Xiao_nrf52` board/RadioLib variant and
`seeed-xiao-afruitnrf52-nrf52840` board, not a PHY-less variant:
D1 DIO1, D2 RESET, D3 BUSY, D4 NSS and D5 RXEN, with upstream SPI pins,
DIO2 RF switching and a 1.8 V TCXO. Nordic PlatformIO is pinned to `11.0.0`;
the release pins the MeshCore Adafruit core to `d541301` and RadioLib to
`6d8934836678d8894e3d556550475b37dce3e2b6`. The normal repeater linker layout
reserves the bootloader, SoftDevice and InternalFS.

Build defaults use the owner's test profile **912.525 MHz, BW 250 kHz, SF7,
CR5, 2 dBm**, not a worldwide channel recommendation. Select legal local
frequency, bandwidth and power before transmitting. Saved native preferences
take precedence; inspect `get radio` and `get tx` and match your companion.
Stopping a host test process does not mute installed RF-capable firmware:
it can still forward, advertise and reply. Use the hard-muted `nrfmast_rx`
image when the nRF must remain radio-silent.

The repeater retains native password/ACL login, `setperm`, encrypted remote CLI,
`set radio`, `tempradio`, timed restoration and `reboot`. Both applications follow
that one physical radio. Management permissions remain with the repeater;
bot commands grant no administrative permission. There are no role reservations
or mast/host role interlocks.

When migrating an existing companion, check `get repeat`.
Use native `set repeat on` when commissioning it as a
repeater. This preference is deliberately preserved, not silently overridden
by the image; the bot works independently of it.

On first installation, an empty admin password is replaced with a random
15-character value before any incoming packets are processed. Provision a
password using `password <chosen-password>` or a companion ACL entry using
`setperm <companion-public-key> 3` over the native 115200-baud serial CLI.
Existing nonempty passwords and ACL entries are retained. Do not put actual
keys or passwords in this source tree. Set/check the native clock with `time`
and `clock` if no battery-backed RTC is fitted. Companion private-key import/export
build flags remain disabled; the runtime wrapper permits local USB imports and
explicitly rejects private-key exports.

Without a retained RTC, the board can return to the upstream 15 May 2024
fallback clock after restart. Identity/preference persistence does not imply RTC continuity.
Resynchronize with serial `time <epoch>` or authenticated RF `clock sync`, then
request fresh adverts when needed. This image does not add a
battery-backed or persistent wall clock.

Read the current tuning, name, identities, memory and packet counters without
rebooting or requesting transmissions:
`make -C firmware/nrf52840 status ENV=nrfmast PORT=/dev/serial/by-id/DEVICE RECORD="$PWD/.tmp/nrfmast-status"`.
Use `ENV=nrfmast_rx` instead when inspecting a hard-muted image.

### Owner-lab validation helpers

These helpers target the owner's configured radios, not arbitrary public
installations. Read their device guards and mutation steps before use.
`fleet-profile` stages **Pine-Relay**,
three-byte repeater paths and **912.525/BW250/SF7/CR5/2 dBm**, preserving both
identities. It requires explicit `PORT` and a new private `RECORD` directory,
like `status`. Native radio preferences apply at the next reboot.
`hardware-check ENV=nrfmast_fleet` performs that reboot and verifies retained
identities, PHY, name and path mode. `upload-rf ENV=nrfmast_fleet` selects the
fleet application after explicit serial DFU entry; it does not erase InternalFS.
The build's bot label is only a default for an old unnamed identity record;
`set bot.name` changes and saves it without another firmware build.
`fleet-name`, with the same explicit `PORT`/private `RECORD` requirements,
changes only the repeater's name through native `set name Pine-Relay` and
verifies unchanged keys, tuning, repeat setting and path mode. This saved
name was verified across reboot. `runtime-check`, with the same explicit
`PORT`/private `RECORD` requirements, changes the bot label to a temporary check
name and then **Pine-Bot**, reboots and verifies saved names, both identities
and the shared PHY. It refuses to reboot a deliberately staged replacement
identity and never imports or rotates live keys.
It then synchronizes Pine's clock, adds the existing authorized lab admin public
key through native `setperm`, and verifies the saved-name/public-key controls
through independent encrypted RF from Birch's KISS radio. The gateway must
already have the committed lab PHY; it is neither retuned nor paused. Private
export is rejected over that RF administration path. The check retains existing
ACL entries and never transmits a private key.

### Runtime names, identities and the shared radio

Configuration requires a firmware containing these controls once, not a rebuild
for each change. The existing USB serial CLI and the repeater's native encrypted,
authenticated administrator CLI expose the following controls:

| Setting | Command | Persistence/application |
| --- | --- | --- |
| Repeater name | `set name Pine-Relay`, `get name` | Native saved preferences; subsequent adverts use the new name |
| Bot name | `set bot.name Pine-Bot`, `get bot.name` | Saved alongside its existing identity; immediate, no reboot |
| Name notification | `advert.zerohop`, `bot advert.zerohop` | Native signed direct adverts with no hops; queue receipt is not peer learning |
| Public identities | `get pub.key`, `get bot.pub.key` | Reports active and saved keys, plus `reboot=1` for a staged change |
| Deliberate identity import | `set prv.key <128-hex-native-private-key>`, `set bot.prv.key <128-hex-native-private-key>` | **Local USB only**; verified/atomically saved, reboot required |
| Shared RF profile | Native `set radio`, `get radio`, `set tx`, `get tx` | One physical authority for both roles; radio changes report reboot required, TX power applies natively |
| Capability limits | `get capabilities` | Identifies USB-only key import, disabled private export, shared RF and unsupported channel keys |

Names are 1-31 printable ASCII bytes without leading/trailing spaces. The bot
name is included in its native signed advertisement, not just a dashboard label.
Synchronize the native clock (`time <epoch>`) after a reboot before notifying
peers, so the new advert is newer than their saved contact. Zero-hop notification
uses upstream `sendZeroHop`; ordinary originated floods still use three-byte
paths. `make -C firmware/esp32 owner-field-fleet-names` coordinates the reserved
four-radio lab: saved names/restart checks, independently received signed
zero-hop adverts and stock companion contact learning. It does not rotate keys,
retune the fleet, grant role privileges or enable MQTT.
`owner-field-fleet-adverts` repeats the name readbacks and notification only,
without rewriting names or rebooting Aspen/Pine. Both operations restart the
Go host to use its native repeater/room zero-hop startup adverts. A truncated
stock USB self-info handshake permits one explicit Cedar reset/read-only retry;
failed RF operations are not silently replayed.
Identity writes use the existing native `IdentityStore` format, first verify a
separate complete record (including private bytes), then atomically replace the
target through LittleFS rename. Short writes or failed commits report errors.
Renaming after an intentional staged rekey preserves that staged key. Importing
the other role's active or saved identity is refused. No private key is returned
in CLI replies, and remote imports are refused even through the encrypted CLI;
signed-only plaintext packets never form a private-key provisioning interface.

The combined nRF image contains a repeater and a **DM-only** bot. BLE can expose
the existing bot/chat identity as a bounded native companion interface; it does
not create a third RF identity or change the repeater identity. Channel-key
management and channel commands remain unsupported. The [focused guide](STATE-BLE.md)
lists the companion operations and the repeater's exclusive shared-radio authority.

### Identity storage and update prerequisites

The existing repeater identity is `/_main.id`; the bot uses `/_nrfbot.id`.
The Pine fleet image requires both existing records and stops if either is
missing or corrupt; it never generates replacement role keys. Startup uses the
LittleFS mount-only entry point rather than upstream `InternalFS.begin()`'s
automatic formatting fallback. A failed mount stops startup without erasing
storage. Generic non-fleet images can initialize missing identities on an
already formatted filesystem. The companion's extra filesystem/QSPI features
are not enabled.

The 1200-baud reset enters a serial-only DFU bootloader, not a UF2 mass-storage
volume on the tested bootloader. **A companion API snapshot is not a full-device
backup.** A first-install reflash can replace existing state; use the initialized
Pine application's app-only update path to preserve its state.

## Commands and bounds

The RF-capable image attempts **one compact native identity advert**, as
**nRF command bot**, 20 seconds after boot. There is **no periodic bot advert
timer** and no automatic chat/help announcement. This initial signed flood
advert keeps normal MeshCore discovery working through community repeaters;
it is not a verbose bot announcement or a five-minute heartbeat. Receiving
repeaters' flood/region policies still apply.
The fleet profile defaults that bot to **Pine Bot**, until a saved runtime name
overrides it. Bot adverts and originated
private/channel floods use three-byte hashes; received routes retain their
native encoded width. The repeater's own path policy remains a saved preference.

The operator can request another native flood advert with the serial
`bot advert` command, for example when commissioning a late-joining companion.
For a name notification without relay forwarding, use native
`advert.zerohop` for the repeater and `bot advert.zerohop` for the bot.
The latter is available through USB or the existing authenticated, encrypted
repeater admin CLI. Both use upstream `sendZeroHop`, with an empty direct
route (no routing hashes); the saved three-byte flood policy is unchanged.
Queue acceptance does not establish that another radio learned the name.
An operator advert before the startup deadline replaces the automatic attempt.
A failed allocation is reported and is not retried automatically. All adverts
use the existing bounded packet pool, native dispatcher/CAD and shared PHY
airtime budget; neither startup nor the operator path bypasses that budget.
The repeater's own native discovery, adverts and operator preferences are
unchanged. Companion radios can
continue using normal received/shared contacts and learned routes.

The bot spends no recurring advert airtime without an operator request.
Send a companion advertisement first:
the bot learns eight chat, repeater or room contacts in RAM, replacing non-favourite contacts when
full. Contacts and learned paths must be rediscovered after restart.

Send an ordinary encrypted direct message to the bot:

| Command | Reply |
| --- | --- |
| `!ping` | `pong uptime_s=...`, from the actual nRF millisecond clock; not a fabricated RTT |
| `!signal` or `!path` | The received packet's last-hop SNR/RSSI and observed path |
| `!help` | The available commands and reply limit |
| `!remember key value`, `!recall key`, `!forget key`, `!list` | Durable personal notes, scoped to the full sender and bot keys; see [quotas and privacy](STATE-BLE.md#personal-notes-over-rf) |

Flood messages report observed hash count, hash width and a bounded hex path.
Direct messages report **remaining** path length and
`end_to_end_hops=unknown`: a consumed direct path cannot reveal the full route.
At most 18 path bytes are printed, with `...` when truncated. An empty path is
reported explicitly. RSSI is captured with the same RX frame; it is not read
later from an unrelated packet. Signal values describe the last RF hop, not
every hop or link quality at the sender.

The development command cooldown is **0**; the next command can proceed as soon
as the previous reply's native ACK releases the one-inflight slot. Commands
that change notes additionally use a device-wide 32-attempt burst and an
eight-attempt full-key-principal burst, replenishing one attempt per 15 seconds of
device uptime. Notes and replay timestamps use a checked 512 KiB external-QSPI
ring, separate from the internal identity filesystem. Reads and `!ping` have no fixed delay; see the
[write budget and flash cost](STATE-BLE.md#flash-write-cost-and-the-default-budget)
before using Pine as persistent storage. Commands
are explicitly rejected in USB diagnostics before changing notes when RF is
disabled, a reply is still awaiting its ACK, or no reply packet slot is free.
There are no automatic bot retries. Replies fit the
native 160-byte text limit. Other text, channel messages and CLI-data messages
do not invoke commands. Ordinary DMs carried by native transport-flood or
transport-direct routes also invoke the bot after the same MeshCore destination,
known-contact and message-MAC/decryption checks. Transport scope is a routing
policy, not a replacement for message authentication or administrative access.

ACKs, path learning and replies retain the native `BaseChatMesh` policy:
use a learned direct path when available, otherwise its unscoped flood fallback.
This bot has no configured outgoing scope and does not copy a request's
packet-specific transport codes onto a different reply. Relays' native
region policies still apply; a scope-only region can reject that unscoped
fallback. The repeater retains its separate native region support.

The bot uses upstream `BaseChatMesh` encryption, ACK/path discovery and packet
creation. Its pool has eight packets; the unmodified repeater has 32. Each radio
port has two bounded RX snapshots and its own duplicate table. One small
`mesh::Radio` adapter serializes access to the single upstream RadioLib driver:
only the transmit owner polls completion, no physical receive occurs during
TX, the other application pauses until TX completes, and a shared off-air
interval follows the repeater airtime factor. This conservative pacing has not
been tuned for throughput or battery life. Sensors, display, WiFi and MQTT
are omitted; BLE is optional and disabled until a USB-provisioned PIN and enable
setting are saved. Ports fan out received RF frames, not local transmissions between
identities. Let a companion learn the bot's own path; a manually constructed
direct route through the co-located repeater to the bot is not supported.

The `nrfmast_rx` image rejects all physical TX attempts, including repeater
forwarding, adverts, ACKs and replies. It therefore cannot provide working RF
management or bot replies; it is only for coordinated boot/heap inspection.
The serial `mem` command reports heap capacity, allocated/free heap, a sampled
free-heap low point, loop-task stack headroom, TX counters and RX queue drops.
`bot advert` queues a flood advert; it cannot bypass the RX image's hard TX disable.
The serial-only `ids` command reports both public identities for restart
comparison; the hardware checker saves those values only in private ignored
records, not in source or its console summary.

### Local RF and state boundary

Local radio operation is implemented, not dependent on HTTP or a wired host:
the native bot receives encrypted messages, reads actual packet metadata and
sends bounded native replies, while the native repeater forwards independently.
There is one half-duplex radio, not simultaneous independent links.

Both identities, native preferences and the forwarding setting persist across
restart. Native ACL storage remains with
the repeater. Bot contacts, paths, reply/timeout counters and rate-limit state
are RAM-only. Native personal-note commands use a bounded external-QSPI journal
with full-key scopes and durable mutation timestamps. The small native-note images have no arbitrary
file access, general KV API, handler registry or scripting interface. The
[notes and BLE guide](STATE-BLE.md) gives storage bounds and errors.

Pine has no IP transport or HTTPS. The `nrfmast_fleet_lua` image uses the shared
production Lua API, durable KV and source installer with explicit Pine limits;
see [its focused guide](PRODUCTION-LUA.md).

## Developer tests

`make test` runs hardware-safety checks plus ASan/UBSan native tests against the
actual pinned MeshCore packet, crypto and `BaseChatMesh` sources. Disposable
generated identities advertise, deliver encrypted flood/direct commands, and
decrypt the bot's actual outgoing replies. Cases cover reply shape/limits,
zero command cooldown, busy/full rejection without note mutation, captured
metadata, malformed frame bounds, simultaneous TX exclusion,
off-air spacing, rollover and hard RF disable. Native packet-pool leak checking
is disabled because upstream pools allocate for the process lifetime.
Simulated-clock checks count exactly one startup advert, no five-minute or
later periodic adverts, no automatic retry after pool exhaustion, and an
operator-triggered advert after the quiet period. A pre-startup operator advert
also suppresses the automatic attempt. No on-device soak is needed for these
timer checks.

A separate native `BaseChatMesh` peer communicates through a native `Mesh`
relay, with no direct peer/bot link. That case checks discovery, an initial
encrypted flood command, reciprocal path learning, subsequent direct commands,
and both directions' ACKs for one-, two- and three-byte path hashes. It uses the
upstream routing and cryptography.
The scoped variant uses upstream transport-code generation and sends an
encrypted command through the same relay, then verifies the native reply, ACKs
and learned direct route at each hash width. Transport-flood/direct receive
checks also reject damaged message MACs and packets addressed to another node.

## Repeating the bounded USB check

Coordinate serial access with other users. `NRF_PORT` and `NRF_DFU_PORT` below
must be the explicitly selected Seeed application and bootloader USB by-id
paths; they differ after the 1200-baud transition. Python `pyserial` is needed.
Each `RECORD` must be a new subdirectory: existing records are not overwritten.
Records use directory mode 0700 and file mode 0600.

```sh
make -C firmware/nrf52840 companion-backup PORT="$NRF_PORT" RECORD="$PWD/.tmp/nrfmast-backup"
make -C firmware/nrf52840 bootloader PORT="$NRF_PORT"
make -C firmware/nrf52840 upload-rx PORT="$NRF_DFU_PORT"
make -C firmware/nrf52840 hardware-check PORT="$NRF_PORT" RECORD="$PWD/.tmp/nrfmast-check"
```

`companion-backup` is for the previous companion image, not the new repeater's
text CLI. `upload-rx` checks the bootloader identity and always builds/uploads
the hard-muted environment. It cannot select `nrfmast`. `hardware-check` reads
native status, checks distinct identities and zero physical TX, performs one
native software reboot, and compares identity and selected preference readback.
For an authorized RF-capable installation, first read `get radio`, `get tx`
and `get repeat` on the existing native serial CLI, then use:

```sh
make -C firmware/nrf52840 bootloader PORT="$NRF_PORT"
make -C firmware/nrf52840 upload-rf PORT="$NRF_DFU_PORT"
make -C firmware/nrf52840 hardware-check ENV=nrfmast PORT="$NRF_PORT" RECORD="$PWD/.tmp/nrfmast-rf-check"
```

`ENV=nrfmast` makes the checker expect RF enabled and permits nonzero TX counts;
it does not enable RF on an RX image. The default check still requires RF
disabled and zero TX. Both modes verify identity and native preference
persistence across one software restart.

Use the
[production Lua validation procedure](PRODUCTION-LUA.md#validation-and-resource-checks)
for the separate `nrfmast_fleet_lua` image.
