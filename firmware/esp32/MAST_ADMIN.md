# Administer an ESP32 mast or native host bot

Use this guide to configure roles, names, keys and grants; install or recover
command source; and back up scoped data. Start with
[ESP32 provisioning](README.md#choose-an-image) or
[Go native Lua setup](../../HOST_GUIDE.md#host-status).
For client implementation, use the
[mast-beta-v1 wire contract](MAST_BETA_PROTOCOL_V1.md).
For the public Aspen bundle, complete [offline USB setup](PUBLIC_SETUP.md)
before trying RF, browser administration or signed updates. Its private
setup record supplies the initial credentials and operator verification key.

The opt-in `Xiao_S3_WIO_onchip_beta` image adds native MeshCore login/CLI and
an authenticated `/admin` web editor to the existing **management** identity.
It runs on the XIAO ESP32-S3R8/Wio SX1262 shared-radio mast. Repeater, room,
companion and command-bot roles can all be disabled without disabling mast
administration. Ordinary KISS connections remain available.

Open `http://MAST/admin` for saved/running role selection, runtime names and
adverts, shared radio changes, source upload/install/recovery, scoped bot
backups and service diagnostics. The [browser guide](MAST_WEB_UI.md) walks
through these controls and their confirmations.

The management identity owns shared device settings, including the physical
radio profile. Native role administration owns that role's local settings;
repeater/room ACLs do not grant mast-wide authority. A role may therefore
reject `set freq`, `set radio` or `tempradio` while those operations remain
available through authenticated management's `radio` and `tempradio` commands.
RF management and `/admin` reach the same backend. Any delegated role-side shared control must carry
explicit management authority and use that backend.

The native HTTPS image also supports [optional device metrics](TELEMETRY.md)
through `telemetry help`. It is disabled until explicitly provisioned through
this same RF/web management backend; role administrator ACLs are unchanged.

### Remote logs and memory checks

On an ESP32 mast, enter these commands through authenticated Management RF
or the browser's command console:

```text
set syslog 192.0.2.10:514
get syslog
syslog test
get syslog.stats
get diagnostics
stats system
stats memory
stats psram
stats vm
```

Replace the destination with your UDP syslog collector's IPv4 address. The
default port is 514. The setting survives reboot; `set syslog off` stops remote
logging. The collector receives RFC 5424 `local0.info` messages from the device
hostname, with a missing timestamp (`-`); use the collector's receipt time.
Allow UDP from the radio on the collector and keep this unencrypted traffic
on a trusted network.

Logs include boot/reset reason, WiFi and UTC transitions, companion connection
errors, telemetry error/recovery and numeric Lua/Wasm execution measurements.
Packet contents, caller commands, script error text and credentials are not
forwarded. UDP is fire and forget: `submitted` counts local stack submissions,
not reception. `failed`, `limited` and `offline` count transport/encoding
failures, the eight-message-per-second limit and disconnected WiFi.

The existing diagnostics worker drains an eight-record queue without blocking
radio dispatch. Queue data uses PSRAM when available; the control block and
4 KiB worker stack remain internal. Full queues and unavailable USB output
increment the cumulative `get diagnostics` counters. `stats system` reports
the reset reason, maximum active loop duration and loop-start gap in
microseconds, remaining diagnostics stack low-water in bytes and whether the
queue uses PSRAM. These measurements reset at reboot. Use `stats memory`'s
minimum free heap and largest block alongside `stats vm` and `stats psram`
when checking headroom during source updates or HTTPS activity.

## Administrator workflow

### CLI prerequisites

The browser needs no Python installation. From the repository root, prepare
an isolated environment for `admin.py`, then check help without connecting
to a node:

```sh
python3 -m venv .tmp/admin-venv
. .tmp/admin-venv/bin/activate
python3 -m pip install -r firmware/esp32/requirements-sign.txt
python3 tools/hardware/admin.py --help
```

Use `source --help` for named-file installation, export and manual recovery;
use `data --help` for scoped records. Put connection options before the command:

```sh
python3 tools/hardware/admin.py --unix-socket /path/to/bot/native/admin.sock \
  source list
python3 tools/hardware/admin.py --unix-socket /path/to/bot/native/admin.sock \
  source install monitor monitor.lua
```

`source install NAME FILE` changes one named Lua file. `source replace FILE`
replaces the complete source set. `source rollback` restores the previous
generation without restoring data; `source reset` selects bundled commands.
Existing `source-install`, `install`, `rollback`, `remove`, `data-export` and
`data-restore` forms remain aliases. Use `command 'help'` for the connected
endpoint's native administration topics.

The manifest supplies `cryptography` and PyNaCl. If you already have an
isolated Python environment, skip its creation and activate that environment
instead.

### Choose an owner connection

| Connection | Administers | Security and next command |
| --- | --- | --- |
| Encrypted authenticated Management RF | ESP32 mast, shared PHY and local roles/bot | Use a stable companion identity on the same PHY; [native RF login](#native-rf-and-web-access) |
| `http://MAST/admin` or `admin.py --web` | Same ESP32 backend | Trusted LAN only; HTTP exposes password/session/private backups; [browser workflow](MAST_WEB_UI.md) |
| `admin.py --unix-socket .../bot/native/admin.sock` | Go native bot source, identity, policy and scoped data | Local same-UID owner; 0600 socket under 0700 state; not shared-PHY or sibling-role authority |
| Host `http://127.0.0.1:9080/admin` (opt-in) | Host role monitoring; running relay/room names and policy; native bot names/grants, Lua installer and scoped consoles | Configured role admin password or separate owner token exchanged for a ten-minute session, explicit intent and same-origin requests; [activation and capability table](../../HOST_GUIDE.md#host-browser-administration) |

Private key, WiFi, channel and CA/token imports must use the allowed encrypted
RF or protected local socket route. Plain HTTP is not a secret-provisioning
transport, even after login. Native repeater/room passwords and ACLs do not
authorize mast-wide settings. Package permissions/capabilities do not create
owner trust.

The ESP32 page's **Check pending changes** targets the runtime and data operation
whose reply was lost, even if the runtime selector has since changed. Native
command errors and invalid/incomplete readbacks leave writes blocked. A Wasm
commit requires Wasm status and its matching durable hash; a scoped restore
requires that transfer's committed ID. An unrelated successful readback does
not clear a different pending operation. Draft text remains unchanged and no
uncertain write is automatically repeated.

### WiFi application updates

On an update-capable ESP32 image, open the **Firmware** section and choose
**Check update / running image**. It reports the target, transfer state, boot
health and rollback readiness. Provisioning
an application-slot layout and trusted release operator key still requires
the supported initial USB installation.
Use the [ESP application-update guide](ESP_FIELD_UPDATES.md) for retained
partition/bootloader requirements, recovery and the authenticated HTTP API.

Use the release operator's application-only `.bin` and signed JSON manifest.
The release operator can generate the manifest without exposing the signing
seed to the browser:

```sh
python3 firmware/esp32/esp_update_manifest.py \
  --image /absolute/release/application.bin --target xiao-esp32s3 \
  --key-file /absolute/private/operator-seed > application-manifest.json
```

Select both files, then choose **Verify and upload application**. Local
target/size/digest checks run before the request; the device verifies the
operator signature, ESP32-S3 application image and digest before selecting
the inactive application slot. Never use a merged full-flash binary.
**Reboot into verified application** is a separate explicit action.
Reconnect and login, then check the exact running image hash and healthy
boot status. A verified inactive slot, an admitted reboot or a reachable
page does not establish that the intended application is running.

Lost upload replies remain fenced until readback reports the same verified
image digest, target and byte count. Lost/admitted reboot outcomes remain
fenced until the running digest and byte count match and boot health is healthy. If an
older backend lacks those digest fields, use the supported owner/update
checks; this page will not invent confirmation or repeat the operation.
The backend's `sha256` identifies the signed complete raw application file.
Its `running_sha256` covers the running application through IDF's verified
image length, including its appended hash. The updater rejects trailing bytes
so those digests cover the same bytes. A healthy matching hash while an update
still reports `verified` is not a completed reboot; inspection also requires
the new boot's idle update state and no running-hash read error.

In the MeshCore app, use the Management contact's console for mast commands.
Start with `status`, `roles`, `help`, `get name`, `get owner.info` or `role help`.
`help` returns the first of three topic-index pages; request `help 2` or
`help 3` for the rest. `help TOPIC [PAGE]` returns one requested syntax page,
including `help wifi 2` for text setters and `help wifi 3` for hex setters.
Topics are grouped by role, shared device and owner/source operations, with
alphabetical names within each group. `help bot 3`, `help channels`,
`help threads` and `help repeaters` lead to policy and monitoring controls.
One request returns one page; the radio does not send unsolicited help bursts.
Other bare command namespaces return a usage error. Standard name/owner and
WiFi commands map to Management's own settings;
repeat/delay/region settings still belong to the Relay or Room.
See the [app endpoint compatibility table](../shared/ANDROID.md#choose-the-endpoint-for-app-settings).

### Recover a bot's known contacts after restart

On Aspen, the bot can recover a caller's identity and name from the running
on-device companion's saved signed adverts when a DM arrives after restart.
Run `bot contacts` to inspect the 16-slot contact table and the recovered and
rejected counts. The lookup rechecks the complete public key and native advert
signature; it does not transmit the saved advert or write another contact cache.
Recovery scans are limited to one per second and 16 matching candidates.

Recovered contacts have no bot-specific route or RF observation. `!neighbors`
continues to list received signed adverts, not restored contact knowledge.
Routes must be learned through the bot's own native path exchange; companion
routes, login sessions and administrator grants are never copied. A recovered
identity does not grant owner access.

The companion role must be running and retain the caller's signed advert blob.
If the role is stopped, the blob is missing or invalid, or the bot's contact
table is full, the caller must send a new advert that the bot receives.
Receiving a fresh signed advert replaces restored knowledge with an actual
observation. Other host and Pine profiles keep their existing role-local
contact behavior; `lookup=none` means this companion recovery source is absent.

### Pine BLE application updates

Use Pine's authorized native console and the
[BLE field-update guide](../nrf52840/BLE-FIELD-UPDATE.md), not the ESP32 HTTP
firmware controls. `get update` reports the bootloader version and update
capabilities. `start ota` displays the warning; `start ota confirm` opens the
120-second PIN/MITM-checked app-mode gate for standard Nordic legacy DFU.
Arming requires the existing repeater administrator ACL or physical USB access;
BLE requires the configured current PIN. Pine has no HTTP update endpoint.

That gate does **not** limit the bootloader transfer session. The retained
single-bank bootloader erases the application during update and has no
application rollback or guaranteed abandonment timeout. Use an app-only ZIP.
If a transfer is interrupted, retry over BLE; recovery without a cable must
be checked on the installed radio before relying on it in the field. If BLE
retry is unavailable, physical recovery is required. `stop ota`
revokes the app-mode gate and reboots; it cannot stop a transfer after the
bootloader has taken control.
The retained bootloader checks ZIP CRCs, not release-operator signatures;
confirming arming accepts this update path rather than the signed ESP32 flow.

### Inspect before changing

For a trusted-LAN ESP32 connection, put the password in an existing protected
0600 regular file, then run from the repository root:

```sh
python3 tools/hardware/admin.py --web http://MAST \
  --password-file /absolute/private/mast-password command 'status'
python3 tools/hardware/admin.py --web http://MAST \
  --password-file /absolute/private/mast-password command 'source status'
```

Expected: saved/applied role selections, effective PHY/generation, and selected
versus live source state. Read `source api`, `source hash`, `bot policy` and
`job` as relevant. Host owner-socket examples use `--unix-socket
/absolute/state/bot/native/admin.sock` instead of the web/password options;
use `source status` there, since its command set is bot-scoped.

### Configure only the authority you need

1. **Roles:** `roles MASK` and `bot on|off` save next-boot selection. Inspect
   saved/running state, then use `apply` when ready to disconnect clients and
   activate it. Management/KISS stay independent.
2. **Names:** use `role name ROLE TEXT`; this preserves keys. Use
   `role advert ROLE zerohop` to announce the name to radios in range.
   See [runtime names/keys](#runtime-role-names-and-keys).
3. **Keys:** back up first. Staging does not apply a replacement. Read
   active/pending keys and use the documented role restart or host `key bot apply`.
   **Changing a bot key does not transfer its notes/timers/reminders to the
   new identity.** See [key imports](#importing-an-operator-supplied-identity)
   and [host bot identity](#go-host-native-bot-identity).
4. **PHY:** Management owns every role/client's radio profile. `radio` changes
   it durably; `tempradio` returns to the complete durable profile. Retuning
   can lose your RF route; retain a recovery connection and match all radios.
5. **Grants:** enable only needed `bot shared`, `bot reminders`, `bot home`,
   event masks, destinations, channel waits or forwarding. Missing grants
   default off. Compare saved/live readback after changes or failed saves.
6. **Network:** approve fixed aliases/destinations, TLS CA/hostname and protected
   tokens, commit/read back, then grant use. ESP32 requires fresh accepted SNTP;
   native Linux requires a kernel-synchronized clock (`home` reports `clock=0|1`).
   Use the four endpoint slots deliberately; named RPC can reuse `home`.
   See [network setup](../runtime/NETWORK_API.md).

### Install, update, roll back or recover

Use the [local Lua workflow](../runtime/BOT_DEVELOPMENT.md) first, then
[source installation](#source-and-help-installation). Source is at most 4096 B,
including package metadata. One install replaces the whole custom generation,
not one independently sandboxed plugin; a previous generation is retained for
schema-compatible rollback. Native diagnostics remain reserved.

| Decision | Command / expected outcome |
| --- | --- |
| Inspect current source | `source status`, `source hash`, `source metadata`; selected and live are separate outcomes |
| Install/update | `bot-plugin-install PACKAGE=...` via owner transport; poll terminal status and verify hash/live generation |
| Resume interrupted upload | Same `begin` ID/size/hash and acknowledged next chunk; never blindly repeat a commit or unrelated write |
| Abandon staging | `source cancel`; active source/data remain |
| Return to previous code | `source rollback`; schema compatibility required; data and grants are not rolled back |
| Restore bundled commands | `source remove`; explicit removal of custom selection, retaining scoped data |
| Retry a valid durable selection | `source retry`; inspect status/hash, do not infer activation from admission |

Readback/hash validation must match the selected generation. A disabled bot
can receive a verified durable selection without starting its RF identity.
Activation/restart cancels suspended jobs; already admitted storage/RF/network
effects may have occurred. `Accepted`, `Queued`, an upload ACK or a transport
ACK is not live activation or recipient delivery.

### Back up and recover without replay

Export active source and separate KV/timer/reminder scopes before destructive
work. [Scoped exports/restores](#scoped-bot-data) omit keys, credentials, grants
and source; they are not a full-node backup. Host identity recovery needs the
protected host state backup too. **SPIFFS provisioning erases role files and
bot data; NVS erasure can destroy identities.**

KV restore replaces its complete encoded scope, including deletion of newer
keys absent from the snapshot. Timer/reminder restore requires **no-rearm**:
matching pending work is cancelled, missing pending work imports as cancelled,
and terminal/consumed claims do not become runnable. Families are separate
transactions. Unknown restore or send outcomes require readback, not replay.

After restart, clients reconnect, bot contacts/routes rebuild, and fresh
time/route authority gates scheduled work. If source or storage is corrupt,
inspect the explicit fault and recover through Management/owner socket;
do not factory-reset or format merely to clear an error.

## Build the management image

```sh
make -C firmware/esp32 beta-test
make -C firmware/esp32 beta-build
```

`beta-test` runs host-native fixtures, including requested help pages, named
role readback, literal and hex WiFi setters, safe SSID rendering, private
password readback and the 145/146-byte tagged boundary. It does not connect
to a device or transmit RF.
To run just the Management CLI fixtures, use
`make -C firmware/esp32 admin-cli-core-test`.

`beta-build` uses a public build-only profile and does not upload. An operator
build uses `bot-firmware`, an explicit configuration, and
`ENV=Xiao_S3_WIO_onchip_beta`. Supply a nonempty `ONCHIP_MAST_PASSWORD`
(1..15 bytes, as in native MeshCore), and optionally the full
`ONCHIP_TRUSTED_COMPANION_PUBKEY`. The beta includes Lua even when the bot's
runtime role is disabled. No other board/transceiver is supported here.
The mast password defaults to empty and is independent of repeater/room
`ONCHIP_ADMIN_PASSWORD` and their native password changes. An operator may
intentionally configure identical strings; it is never inferred. A compiled
trusted full companion key has a reserved replay record and RF session.

Create and host-test packages with the
[Lua developer workflow](../runtime/BOT_DEVELOPMENT.md). The authenticated mast CLI
installs package source through the existing resumable RF or web transfer and
can report status, diagnose faults, reboot, roll back or restore bundled
commands.

Current sources use **`named-commands-v1`**, reported by `source api`: define
`function hello(name) reply('Hello '..name) end` as the entire source.
Native declaration inspection generates registration and a bounded string
schema; optional `command('hello','name:string:32','Greet a person')` supplies
narrower bounds/help. `!hello slepp` receives
`Hello slepp`; `!help hello [PAGE]` describes its arguments. `!help PAGE` lists
all currently permitted commands rather than truncating discovery. Optional
declaration arguments add an export, permission and example:
`command('hello','name:string:32','Greet','hello','dm','!hello Sam')`.
Permissions restrict dispatch/help; they never grant native authority. One durable source set
can map eight custom command exports. Native diagnostics remain available
and cannot be replaced. Local functions and `_`-prefixed global helpers are
not automatically exposed as commands, but can cooperate in the shared VM.
The web editor's **New hello handler** button creates
this template. Legacy `return function(e)` sources require explicit migration;
the installer, identity and role-journal wire formats are unchanged.

Declarations also accept optional export, permission and example strings:
`command(name, schema, help[, export[, permission[, example]]])`. Permission
labels are `Public`, `Private`, `Owner`, `Channel`, `Shared`, `Reminder`, and
`Home`; they describe native admission and do not grant trust or capabilities.
Use `!help [PAGE]` or `!help NAME [PAGE]`, `!plugins [PAGE]`, and
`!neighbors [1..16]` for bounded discovery. `!plugins` reports the actual
active manifest/modules and source generation: one source installation or
rollback replaces the whole command set, not isolated plugins.
`!admin bot|status|source-status|roles|reboot` is available only to a
trusted owner in an authenticated DM; credentials and bulk data remain on the
native management path. Custom/stateful channel commands require an explicit
`!@KEY8 COMMAND` or full-key target; untargeted read-only native channel queries
use bounded response jitter and correlation tags.

**Read-only diagnostics:** `!about`, `!version`, `!uptime`,
`!status`, `!signal` and `!air [1..4]` report the bot's actual public identity,
upstream/Lua build metadata, boot uptime, readiness/role/WiFi state, this
request's RF metadata and bounded scheduler snapshots. They use the existing
DM/selected-channel admission and airtime policy. No credentials or private
fault text are returned; battery, image hash and airtime history are explicitly
unavailable. Local reflections are not reported as measured RF. TX totals
describe the local scheduler, not remote delivery, and wrap/reset as unsigned
32-bit counters.

`source api diagnostics` reports
`Diagnostics about,version,uptime,status,signal,air readonly=1 channel=verified air-pages=4 battery=unavailable counters=u32`.
The six command names are newly reserved: rename/reinstall conflicting custom
functions or aliases before upgrading. Eight custom exports and existing
source/state journals are unchanged. Help respects the request's scope,
grants and RF reply limit. See
[BOT_RUNTIME.md](../runtime/BOT_RUNTIME.md#read-only-diagnostics) for exact
snapshot/page semantics.

### Bot-facing owner administration and mesh policy

An authenticated private DM `!admin bot help` to the **command-bot** identity
uses the same full-key trust, durable replay state and backend as mast
management. Grant the native administrator role with `setperm KEY64 3` on
Management's authenticated CLI/web connection (see [Management ACL](#management-acl)).
Channel nicknames, group membership and Lua declarations cannot authorize it.
`!admin bot status`, `bot stats`, `bot log`, `bot admission`, `bot policy`,
`bot limits`, `bot mesh`, `bot contention`, `source status`, `roles MASK`,
`bot cancel` and `reboot` provide practical inspection/configuration/recovery.
Prefix every command with `!admin`. `bot log` is the current fault/state
snapshot, not a historical log. Reboot/apply still wait for actual reply TX.
Bulk source/data transfers and native credentials remain on management CLI/web;
the independent management identity remains the recovery route when the bot
cannot start.

These commands work through either authenticated backend:

| Command | Effect |
| --- | --- |
| `bot name [TEXT]` | Read or persist/apply a 1..31-byte printable bot display name, excluding `:`; never rotate its key |
| `bot destination SLOT [KEY64\|off]` | Read/set/revoke one of four additional native DM destinations; full keys, fresh authenticated direct routes, no flood fallback |
| `bot membership SLOT [off\|public\|#tag\|private NAMEHEX KEY32]` | Read or save/apply one of eight simultaneous group memberships; one Public, initially denied; private keys are never printed |
| `bot aliases [off\|aspen,aspen-bot,a]` | Read saved/applied targets or save/apply up to four aliases; no implicit display-name alias; [addressed-only channels](../runtime/BOT_RUNTIME.md#discovery-targeting-and-owner-commands) |
| `bot access dm\|native\|SLOT [default\|COMMAND\|action_NAME [MASK\|inherit]]` | Read or save/apply native bare/addressed execution, replies and storage access; `list OFFSET` lists bounded overrides; [mask bits and examples](../runtime/BOT_RUNTIME.md#channels-and-native-command-policy) |
| `bot thread dm\|native\|SLOT NAME [0\|16\|32\|48\|inherit]` | Read or save/apply a named thread's read/write restriction; `list OFFSET` lists overrides; [thread names, quotas and native events](../runtime/BOT_RUNTIME.md#named-storage-threads) |
| `bot channel-wait [on\|off]` | Read or grant selected-channel follow-up waits; off by default, no authenticated individual sender |
| `bot mesh` | Applied name, destination count, channel-wait grant and epoch |
| `bot discovery [status\|on\|off]` | Persist/apply native base telemetry and path-discovery replies for signed contacts; default off, no location/environment or management grant |
| `bot limits` | Zero command cooldown; worker/I/O bounds; default VM load/init/active limits of 330/50/20 ms; native TX airtime admission |
| `bot contention` | Read-only jitter/suppression counters and its unauthenticated, best-effort semantics |
| `bot cancel` | Cancel other running commands/events and delayed collectors; admitted effects can have committed; autonomous reminders unchanged |

Name/destination/channel-wait policy occupies one checked 166-byte NVS record.
Revoking grants fences in-flight epochs before persistence; a failed save is
reported and does not silently restore live authority. Regranting does not
revive older jobs. `bot policy` continues to report the legacy slot-0 channel
and saved path/airtime settings. `bot channel` and `role channel bot SLOT`
still require a reboot; `bot membership`, `bot access` and `bot thread` apply immediately
without applying pending path/airtime changes. All eight memberships and
command overrides survive restart.

`bot discovery on` is a separate explicit disclosure setting: it permits
base telemetry requests from all authenticated signed-advert contacts, not
just management owners. It applies immediately and survives restart; `off`
keeps the previous deny behavior. Companion/Base telemetry modes and
Relay/Room ACLs remain unchanged. See
[native app Ping and path discovery](../runtime/BOT_RUNTIME.md#native-app-ping-and-path-discovery)
for request types, reply limits and the distinction between a discovery result
and an installed return route.

For connection troubleshooting, `companion stats` reads the native TCP client,
message-journal and role-reset counters. `companion errors` distinguishes
stalled-client disconnects, output overflow, unread-journal eviction, malformed
frames and native bridge faults. Counters cover the time
since device boot; reading them does not clear history, restart the companion
or change contacts. Use these commands through authenticated management RF or
web access without opening a serial port.

On a configured channel, use a configured `!@ALIAS COMMAND`,
`!@BOTKEY8 COMMAND` (or a full public key) for
custom commands, boards, TRACE and state changes. The eight hex digits are the
bot key prefix, not a display nickname. Untargeted read-only queries use a
250..1250 ms jitter window and `[q:XXXXXXXX]` correlation marker for best-effort
suppression. Any channel member can forge such a reply; it is never owner
authentication or write-leader election. Explicit targets bypass suppression.
`!plugins [PAGE]` describes the real active shared source/modules;
`!neighbors [PAGE]` reports signed-advert observations and cached paths, not
current physical adjacency. See [BOT_RUNTIME.md](../runtime/BOT_RUNTIME.md#discovery-targeting-and-owner-commands).

The runtime retains one cooperative function/global
environment and per-invocation coroutines. `source api` retains the existing
runtime contract; `source api package` additionally describes package capabilities.
The package compatibility API remains `named-commands-v1`; runtime support is
validated separately by `source api`.
Durable caller/conversation KV, explicit shared bot/channel keys, timer sleep,
native DM sends to the caller or explicitly granted full-key destinations, and
TX/ACK/text/TRACE waits are implemented. Text waits default to the authenticated
caller; an additional granted peer must first be contacted by a send in the
same invocation. Channel waits use
`mesh.wait{kind='channel', ...}` and are default-off until owner-enabled.
The compact `source api mesh` reply reports `compose=dm,channel,trace`,
`dm=caller+owner-fullkey[1-4]`, typed ACK/text/channel/TRACE waits, the
owner-gated channel-wait flag, bounded pair-DM forwarding and multitrace, and
no raw packet API.
Direct TRACE completion and queued adverts remain native. `bot shared on|off` persists/applies the optional shared-KV
grant through this same authenticated CLI/web backend; `bot shared` reads it.
The existing `Shared KV` reply wording is retained, but the owner grant covers
both explicitly selected bot-global/channel KV and named timer scopes. Channel
scope binds to the verified native group key digest, not a nickname or
individual sender identity. Channel callers cannot obtain private/DM/bot-global
authority. Revocation fences old pending epochs immediately even if its
persistence fails; regranting does not revive them.
Owner mesh configuration is available through the authenticated native
management path: `bot name`, `bot destination SLOT KEY64|off`,
`bot channel-wait on|off`, and bounded `bot limits`, `bot contention`,
`bot cancel` and `bot log` views. Destination grants are full-key, explicit and revocable;
they do not make nicknames or arbitrary script-supplied keys authoritative.
See [BOT_RUNTIME.md](../runtime/BOT_RUNTIME.md) for exact limits and the usable
bundled personal notes and granted shared channel board. Configured HTTPS RPC
uses approved native endpoints. General HTTP/JSON and named RPC are also
available in the current tree; see [NETWORK_API.md](../runtime/NETWORK_API.md).
Installed devices require the matching network-enabled build and configuration.
DM sends may target only the current caller or one of the explicit
owner-granted full-key destination slots; this is not an arbitrary Lua-selected
destination or a transit-forwarding grant. The pair-DM forwarding operation
below remains separately scoped and authenticated.

`source api mesh` reports supported DM/channel/TRACE composition, filtered
text/ACK/channel/TRACE waits, bounded sequential multitrace and
`forward=pair-dm`. Channel waits are owner-gated and disabled by default.
`source api paths` reports ordinary path byte counts separately from explicit
and inferred on-chip TRACE widths.
Through this same authenticated RF/web backend,
`bot forward FROM64:TO64` saves/applies one explicit full-key sender/destination
pair (distinct nonzero keys, neither the bot). The 141-character command fits
native text framing including the existing nonce. `bot forward` reports saved
and applied state; `bot forward from|to` reads back each key separately.
`bot forward off` disables live access and invalidates old invocation epochs
before its durable write/readback; failures are explicit, and a failed write
can retain the old saved policy for reboot. Regrant cannot revive old handles.
The Lua operation forwards only a recent copied/authenticated follow-up DM,
re-encrypted as the bot with full original-sender attribution to a destination
with a fresh authenticated direct route. There is no flood fallback, generic
relay, arbitrary destination or private-to-channel forwarding. See
[BOT_RUNTIME.md](../runtime/BOT_RUNTIME.md#explicit-pair-dm-forwarding) for exact
quotas and limitations.

`source api storage` reports
`Storage kv=2 scopes=caller,conversation,bot,channel timer=set,get,cancel,wait autonomous-reminders=1`.
`source api notes` additionally reports
`Notes remember,recall,forget,notes,list-memories private-dm=1 kv-list=1 keys=8 key-bytes=32 text=120 overwrite=explicit`.
Existing API readbacks and management transfer/journal formats are unchanged.

## Event grants

Event subscriptions use `bot events MASK`: add bits 1 for startup, 2 for
connectivity, 4 for messages and 8 for node status. `bot events 0` revokes all;
the default is off. Source must also declare its named handlers using
`events.on`. `bot events` shows the saved/effectively subscribed masks and
bounded-dispatch counters; `source api events` describes the API. Event grants
do not grant private-user KV, a radio reply route or HTTPS access. Bot/channel
state additionally requires `bot shared on`. See
[granted event subscriptions](../runtime/BOT_RUNTIME.md#granted-event-subscriptions).

## Scoped bot data

Use the same owner RF login or authenticated web session to export or restore
one scoped record family: KV, durable timers, or personal reminders. The bot
must be running with its existing persistent identity.
For a Go-host native bot, use `--unix-socket /absolute/state/bot/native/admin.sock`
instead of `--web` and `--password-file`; the same-UID owner socket uses the
same formats, scope checks and non-rearming scheduler restore rules.
Choose `caller`, `conversation`, `channel` or `bot`, or append `-thread` to
export that origin's named threads. Supply the full user key or verified channel
identifier, or 64 zeros for `bot` and `bot-thread`. Channel identifiers are
digests, not channel secrets. There is no public Lua data-export capability.

```sh
python3 tools/hardware/admin.py --web http://MAST --password-file OWNER_FILE \
  data-export notes.bkd --scope caller --principal FULL_USER_PUBLIC_KEY
python3 tools/hardware/admin.py --web http://MAST --password-file OWNER_FILE \
  data-restore notes.bkd
```

The destination is created exclusively with mode 0600. Restore accepts a private
regular file, rejects wrong size/version/digest/bot identity, stages and checks
it on the storage worker, then explicitly commits the encoded scope. It replaces
all keys in that scope, including deleting newer keys absent from the backup.
Thread-family exports include all encoded thread names for the selected full
principal. Restore leaves its default scope alone, and refuses to exceed the
origin's eight-key quota across default records and all threads. Export both
default and `-thread` scopes when preserving a complete origin.
The default `--kind kv` preserves the original KV-only operation: scheduling
journals remain unchanged. Use `--kind timers` for durable named timers in any
supported scope, or `--kind reminders --scope caller` for personal reminders.
A complete caller-data backup needs all three exports; other scopes need KV
and timers. Each family is a separate snapshot, not a cross-family transaction.
Source, native credentials, grants and identities are never included.

```sh
python3 tools/hardware/admin.py --web http://MAST --password-file OWNER_FILE \
  data-export timers.btd --kind timers --scope caller --principal FULL_USER_PUBLIC_KEY
python3 tools/hardware/admin.py --web http://MAST --password-file OWNER_FILE \
  data-export reminders.brd --kind reminders --scope caller --principal FULL_USER_PUBLIC_KEY
python3 tools/hardware/admin.py --web http://MAST --password-file OWNER_FILE \
  data-restore reminders.brd --no-rearm
```

**Scheduler restore policy is explicitly non-rearming.** `--no-rearm` (raw
`data restore ID no-rearm`) is mandatory. It merges the backed-up names/IDs:
existing records retain their current fields and terminal state; matching
pending records are cancelled. Missing records are imported, but pending
records become cancelled, never runnable. Other live records remain untouched;
they are not evicted to make room. Reminder IDs and original source generations
are preserved, and rewritten revisions strictly exceed both live and backup
high-water values. ID conflicts across principals, exhausted revisions, corrupt
records and insufficient slots/headroom fail before importing any record.
An active reminder delivery blocks reminder restore; an already transmitted
message cannot be undone. New delivery requires an explicit fresh schedule,
with a new revision/ID. An old backup cannot resurrect a consumed notification
even after its terminal slot has been reused.

Each scheduler family restore publishes its complete merge **atomically**.
There is no cross-family transaction.
Before scheduler import and ordinary timer/reminder I/O or delivery transitions,
the storage worker checks the committed KV authority and completes any pending
recovery. Unavailable/corrupt KV state blocks import rather than allowing it to
consume recovery headroom.
`COMMITTED` means the selected family authority committed and read back;
`UNKNOWN` can mean that the complete merge published without a verified response.
Reboot loads the committed family, without replaying a staged restore. Export/read
back before another explicit stage/restore. If publication did not occur,
existing pending records remain unchanged and can still execute. Missing old
records never regain delivery authority from an imported file.

For a raw authenticated CLI/web command, use `data help`:
`data export [kv|timers|reminders] SCOPE PRINCIPAL64`, then poll `data status`. An `EXPORTED SHA256 ID`
result freezes a 2,422-byte snapshot; `data read ID INDEX` reads 48-byte chunks.
KV recovery has a ten-second limit. On a newly provisioned filesystem, the
empty-store scan runs before the first KV operation's two-second window.
KV export and restore also start their two-second operation window after
recovery; keep polling `data status` while it reports `PENDING`. Requests
against initialized KV data retain their existing two-second deadline.
The first write creates all KV file banks with a separate ten-second
initialization window; later writes retain the two-second window.
The family-tagged version-1 formats are `BKD`, `BTD` and `BRD`; each checks the
full bot identity, scope/principal, sorted unique records, unused padding and
SHA256. Timer/reminder payloads retain checked native v1 records (including
deadline, state and revisions); no Lua VM or RF routing objects are exported.
Upload with `data begin ID SHA256`, ordered `data chunk ID INDEX HEX`, then
`data stage ID`; `ID` is the first 16 hex digits of the whole-file SHA256.
Only `STAGED ID` permits `data restore ID`. Inspect `data status` for
`COMMITTED`, `REJECTED`, or `UNKNOWN`. A repeated restore of the completed ID
returns its result without reapplying it. A new begin/stage/restore is a new,
explicit owner operation, not an idempotent retry across reboot.

Upload/stage buffers are bounded RAM, not an active installation: `data clear`
or reboot discards them. Reboot loads an already published restore from
its committed file authority before allowing KV reads/writes; it does not
activate an uncommitted stage.
An `UNKNOWN` result requires export/readback before another restore. A changed
source generation cancels pre-publication work; a published set still recovers.
The existing small NVS partition can reject restores for insufficient
metadata/compaction headroom. All three families require space for inactive
SPIFFS banks. Timers/reminders automatically migrate the deployed checked NVS
payloads on first access, preserving ownership and consumed claims, then reclaim
only those payload keys. Each family uses one 72-byte NVS authority and verified
PSRAM records. Do not erase NVS or format SPIFFS during upgrade; older scheduler
implementations cannot read this file-backed state. See
[scheduler storage](../runtime/BOT_RUNTIME.md#timerreminder-storage-and-upgrade) for sizes,
recovery and focused native checks.
Capacity checks precede imports; later I/O failure
can still produce the explicit unknown outcomes above. Source export/rollback
remains the separate `source` workflow.

**Source-only personal notes:** bundled ordinary Lua now supplies
`!remember <key> <text>`, `!recall <key>`, `!forget <key>`, `!notes [prefix]`
and `!list-memories [prefix]` (the same `notes` function). These use the
authenticated full caller key in private nonlocal DMs only. Channels fail
explicitly even with `bot shared on`; nicknames never supply private identity.
The separate bundled `!board` command selects verified/granted channel KV.
The public native-backed `kv.list([prefix][, scope])` yields like the existing
KV methods and returns a read-only sorted snapshot of up to eight keys, not
values, with count/truncation/RF-safety metadata. Keys remain shared between
functions, installed source generations and reboot, not per-function silos.

RF notes accept 32-byte keys and 120-byte text within the 162-byte packet
limit. Writes report created/replaced and committed; deletion distinguishes
deleted/committed from already absent. Failures and uncertain commits are
errors, never automatic retries. Long lists report truncation and request a
narrower prefix; oversize/non-ASCII values are explicitly rejected for RF
display without deleting or changing raw KV data. All five names are newly
reserved: rename/reinstall conflicting custom handlers before upgrade.
Eight custom exports, owner/core storage reserves, shared-scope grants and
legacy KV records are preserved.
See [BOT_RUNTIME.md](../runtime/BOT_RUNTIME.md#bundled-personal-notes) for
API return values and storage limits.

**Shared channel board:** `!board put <key> <text>`, `!board get <key>`,
`!board list [prefix]` and `!board delete <key>` now use yielding
`kv.*(..., 'channel')`. The owner must explicitly select the native channel
and enable the existing authenticated `bot shared on` grant. All holders of
that channel secret can read, overwrite and delete all entries; nicknames
provide no individual identity or ownership. Public hashtag secrets are
derivable from their names, so this is deliberately a shared board, not
private membership authorization. DMs are explicitly unsupported: there is
no hashtag-to-secret selector or fallback to personal notes.

Board keys use stable raw prefix `board:` and 1..26-byte public names;
text is 1..120 bytes within the full packet limit including channel nickname.
Other raw channel keys and private DM notes remain separate. The prefix
avoids accidental collisions, not access by trusted installed Lua. All channel
KV shares eight entries and the 32 public journal slots; bot-global reserves,
write headroom and native rate/airtime policies remain intact. Put/delete
report explicit created/replaced/deleted/absent outcomes only after checked
storage; unknown commits remain errors, without automatic retries. Grant
revocation fences pending epochs, including after a commit; regrant does not
revive them. Switching source or rebooting preserves the board.

`source api board` reports
`Board put,get,list,delete channel=verified grant=bot-shared dm=denied prefix=board: key-bytes=26 text=120 quota=shared-8`.
`board` is newly reserved: rename conflicting custom functions/aliases before
upgrade. The 2,243-byte firmware-owned chunk keeps existing cumulative VM
budgets, 4,096-byte editable uploads and eight custom exports. `kv.list` now
additionally exposes immutable `suffixes`, each matching key minus the exact
requested prefix; existing `keys`/count/truncation/RF-safety fields are unchanged.
Long board listings preserve whole names and say `truncated; narrow prefix`;
oversize/non-ASCII raw data produces a visible error, not partial success.

**Utility commands:** `!calc <expression>`, `!convert <value> <from> <to>`,
`!roll [dice]` and `!choose <a>|<b>|...` now use protected ordinary Lua paths
and frozen native `utility` helpers. Authenticated private DMs and explicitly
selected/verified `#example1` traffic can use them without granting a nickname
individual identity. Existing channel opt-in, rate, dedup and airtime policies
are unchanged; notes/reminders remain private-only.

`calc` parses decimal `+ - * /` and parentheses, never incoming Lua. It limits
input to 120 bytes, nesting to eight, tokens to 64 and operations to 32;
nonfinite/divide-by-zero/overflow/underflow inputs fail. Results have ten
significant display digits. `convert` covers case-sensitive temperature,
distance, mass, speed, duration and data units; `!convert 1 MiB B` differs
intentionally from `!convert 1 MB B`. `roll` defaults to `1d6`, supports
`[N]dS[+/-K]` up to 12 dice/1000 sides/10000 modifier magnitude, and displays
every die. `choose` accepts 2..8 nonempty trimmed alternatives, each at most
64 bytes within 148 input bytes. Native rejection sampling is unbiased and
bounded, using ESP SDK entropy. Helper calls share an eight-per-invocation
bound and the normal active-time budget.

`source api utility` reports
`Utility calc,convert,roll,choose calls=8 digits=10 arithmetic-depth=8 tokens=64 ops=32 dice=12d1000 choices=8 channel=verified`.
All four command names and `utility` are newly protected; rename conflicting
custom source before upgrade. A separate firmware-owned 633-byte Lua chunk
extends the bundled functions under the same cumulative VM budgets, without
expanding the 4096-byte editable upload limit or eight custom exports.
`!help` prioritizes installed names and explicitly marks list truncation.
See [BOT_RUNTIME.md](../runtime/BOT_RUNTIME.md#bundled-utility-commands) for
grammar, all units, rounding, randomness constraints and native helper limits.

**Weather and service commands:** `!weather PLACE` and
`!service weather PLACE` share one bundled Lua function over the
native `rpc.call("home","weather",...)` coroutine worker. A place is required
(1..80 bytes). Replies include the provider location, Celsius, numeric code and
service-reported observation age. `!service health` takes no arguments and
reports process health only; `!service echo TEXT` accepts 1..120 bytes.
Unknown names/arguments fail; callers cannot choose an endpoint/URL.

These commands keep the default-off native `bot home on` grant, fixed
configured TLS endpoint, fresh trusted time and operation allowlist.
Authenticated private DMs only: even a verified opted-in `#example1` request
cannot acquire network access. Enabling weather on the reference service does
not enable it in the firmware mask. Remote disabled/stale/offline/timeout
errors and invalid/oversize replies stay explicit, without cached/fabricated
success. `rpc.text(text,limit)` escapes non-ASCII bytes/backslashes and marks
truncation, keeping full formatted replies within 162 printable bytes.

`source api services` reports
`Services weather,service home=health,echo,weather private-dm=1 grant=bot-home place=required place-bytes=80 echo-bytes=120`.
Both names are newly reserved: rename conflicting source before upgrade.
A 1,501-byte firmware-owned Lua chunk adds the commands under existing
cumulative budgets, leaving 4,096-byte editable uploads and eight custom
exports intact. The older test probe export is now `probe_weather`.

The yielding `timer.set/get/cancel/wait` primitives persist bounded named
deadlines in a separate checked journal, using the same scope authority. They
require fresh SNTP to create/claim deadlines and never use build-day or native
RTC fallback. Wait durably claims once before resumption; overdue,
replacement/cancellation and uncertain commit outcomes are explicit. A claim
does not prove RF delivery and is not automatically retried. On reboot an
explicit new waiting invocation supplies current principal/route authority.
`timer.wait` now tolerates up to five minutes of continuous clock untrust,
with a hard 24-hour-six-minute live-wait lifetime; expiry does not mutate its
journal. Claim/overdue still require fresh trusted time.

**Personal reminders:** `bot reminders on|off` is the explicit
default-off autonomous-dispatch grant; `bot reminders` reports saved/applied
state. Off invalidates live dispatch before its checked persistence operation,
but holds pending jobs rather than deleting them. Failed persistence can retain
the prior saved grant for reboot. The same authenticated native RF/web backend
handles these commands independently of repeater selection.

The public bot now bundles `!remind <duration> <text>`, `!reminders` and
`!cancel <id>` through `reminder.after/list/cancel`. They require authenticated
private DMs; channels and nicknames cannot supply the principal. These three
names are newly reserved: existing custom handlers with those names must be
renamed/reinstalled before upgrade; custom export capacity remains eight.
`source api reminders` advertises eight slots, two pending per full caller,
120-byte text, 1..86,400-second delay and a 60-second overdue window.
No surviving Lua invocation is needed after reboot, but dispatch requires
fresh trusted time, source/radio readiness and a new authenticated direct PATH
for the recipient. No fallback flood, automatic route discovery or RF retry.
Durable states distinguish pending, cancelled, overdue, sent (**TX**, not
delivery) and unknown. A consumed claim is never replayed after reboot or
uncertain TX. See [BOT_RUNTIME.md](../runtime/BOT_RUNTIME.md#reboot-autonomous-personal-dm-reminders).

Separate bot-global reserve banks add eight KV/two timer slots without
modifying existing records. Public caller/conversation/channel writers cannot
consume those banks; public timer/reminder creates and scheduler imports require
308 free SDK NVS entries (public KV requires 197)
to retain core/compaction headroom. Existing partitions are not silently
evicted or resized, and unrelated NVS exhaustion still fails explicitly.

The newer source adds `rpc=1` to `source api` for the bounded Lua helper.
`bot home` reports native HTTPS configuration, SNTP clock trust and saved/applied grant without
revealing endpoint credentials; `bot home on|off` uses this same authenticated
RF/web backend. Enabling fails if the HTTPS profile or verified endpoint
configuration is absent, or fresh accepted SNTP time is unavailable. The pinned
SDK lacks certificate date checks; the native transport separately validates
leaf/intermediate/configured-CA dates before HTTP writes and rechecks time trust
through response completion. Follow [network setup](../runtime/NETWORK_API.md) for
the matching runtime configuration and [Lua API](../runtime/BOT_RUNTIME.md#approved-http-json-and-rpc)
for results and caller authorization.
The independent checked `bot-home` record defaults
off and does not change role/source/replay journal formats. This slice adds no
unauthenticated network configuration. HTTPS-enabled images additionally
accept `bot https status|discard|commit`, `bot https endpoint ...`,
`bot https ops home MASK` (health=1, echo=2, weather=4),
`bot https ca NAME HEX`, `bot https token NAME HEX`, `bot https rpc ...`,
`bot https drop NAME` and `bot https unmap SERVICE OPERATION` through the
existing authenticated mast CLI. CA/token chunks require the encrypted RF
admin path: the plaintext HTTP web console explicitly rejects those two verbs.
Chunks are append-only bounded hex (at most 128 hex characters in the native
helper, 96 recommended for any 24-byte endpoint name under the 162-byte mast
command limit), not printable
secrets in status or replies. Stage and explicitly commit to a checked
double-slot SPIFFS record, then explicitly grant `bot home on`; staging does
not grant HTTPS to an RF caller. `bot https` provides bounded command help.
See [BOT_RUNTIME.md](../runtime/BOT_RUNTIME.md#native-https-configuration-and-limits)
for limits, redaction and recovery guidance. Do not store command transcripts
or credentials in source control.

## Native RF and web access

Public interoperability is independent of owner tooling. Other users must see
each enabled repeater, room, companion and command-bot identity through normal
MeshCore adverts, contacts and role behavior, without installing our software
or learning our admin commands. Public bot messaging needs no mast-admin
login; normal room access policies still apply. Owners may use private
extensions, but transit community repeaters must only need native MeshCore
packet/routing behavior, never an upload handler or our firmware.

RF administration uses native `ANON_REQ` login: timestamp and password, or
an empty password from a full administrator public key. Login replies use the
pinned native 13-byte response layout, including path-return framing for flood
requests. Subsequent encrypted `TXT_MSG` CLI messages support plain and CLI
text types. Pinned reply-route/scope policy and authenticated native PATH
exchanges learn reciprocal routes, preserving 1/2/3-byte hash widths.
An unknown reciprocal path uses a native flood reply. Scoped requests are
accepted; configure `ONCHIP_SERVICE_REGION` for the public hashtag region
required by restrictive repeaters. Its native transport codes are recomputed
per reply, not copied from the request; arbitrary/private region resolution
is not implemented.
No arbitrary-file use of native `MULTIPART` is introduced.
Source chunks and receipts are encrypted CLI text inside those same native
packets; their private grammar does not change the public wire protocol.

### Management ACL

Management supports five persisted native ACL entries in addition to the
existing singleton `trust KEY|none` and compiled recovery owner. To grant five
operators, repeat these commands for each operator's **full 64-hex public key**
on an authenticated Management connection:

```text
setperm KEY64 3
get acl KEY64
```

The readback reports `permissions=3 admin=1`. Each operator can then use
`login mast ""` and encrypted native CLI commands on Management, and
`!admin bot status` in a private authenticated DM to the command-bot identity.
No password or private-key import is required to grant a public key.
Repeater and room have separate native ACLs: configure `setperm KEY64 3` on
each of those role connections independently.

`setperm` accepts a full key and a decimal permission byte, 0..255. It uses
MeshCore's native lower-two-bit role: 0 removes the entry, 1 is read-only,
2 is read-write, and 3 is administrator. Only the administrator role grants
passwordless Management login or bot owner authority; other permission bits
do not promote a guest/read-only/read-write caller. Management does not add
read-only or read-write login modes. `get acl` reports capacity; `get acl 1`
through `get acl 5` inspect one entry per reply.

Use `setperm KEY64 0` to revoke an ACL grant. New passwordless logins and new
bot owner commands are denied; existing Management sessions retain their
normal 15-minute expiry, and knowledge of the Management password still
allows password login. Replay timestamps are retained. If the key also has
legacy singleton trust, run `trust none` before downgrading/removing it;
otherwise `setperm` returns an error rather than claiming revocation.
The compiled recovery owner's authority cannot be revoked this way.
`trust KEY` replaces only the legacy singleton, and `trust none` clears only
that singleton; neither modifies the ACL.

ACL changes are committed and read back before live admission. An ACL storage
error disables ACL-derived authority and reports an error; compiled and valid
legacy recovery authority remain available. After an uncertain write, inspect
the saved `mc-mast-admin/acl` record and restart before retrying. A valid
committed change may apply after restart even if its readback failed. A
corrupt record is never replaced with default grants by these commands.

The Management permission record is a checked extension of the native
ACL semantics. Upstream `ClientInfo::isAdmin()` determines authority.
Upstream `ClientACL::save/load` use role-local contact files, have no checked
commit result and accept truncated reads, so Management does not share those
files or their transient shared-secret/route state.

An authenticated CLI session lasts 15 minutes. Six ordinary full RF principals
and the separately reserved compiled owner can have simultaneous sessions.
Ten noncompiled principals have durable timestamp high-water marks: the
existing four replay slots remain intact, and six additional slots
accommodate five ACL owners plus the retained singleton. Version-1 settings,
WiFi, singleton trust and the reserved compiled-owner replay record retain
their existing contents; no identity changes are made.
WiFi settings and the five Management ACL entries share two checked 352-byte
SPIFFS files and one 40-byte NVS reference. Both legacy records are validated
before migration. The old ACL metadata is reclaimed only after the combined
file and its reference read back correctly. Missing records retain their
existing defaults. A missing or corrupt authoritative configuration disables
saved WiFi credentials, singleton trust and ACL grants; the compiled owner and
Management password remain available for recovery. Whole-node backups include
the configuration files and reference.
The ten ordinary high-water marks use two checked 400-byte SPIFFS files and
one 40-byte NVS reference. An older primary/extra replay pair is migrated only
after the new file and reference read back correctly; its keys, timestamps and
slot numbers are retained. Reclaiming the old metadata leaves the Lua storage
reserve unchanged. A missing or corrupt authoritative file disables native
administration rather than accepting old timestamps. Whole-node backups include
these files.
Wait for backup creation to finish without sending further RF administration
commands: those commands update replay files. If replay state changes during
snapshot creation, the backup reports changed settings/files rather than
publishing mismatched files and references. Browser status reads do not advance
RF replay timestamps.

**Downgrade:** firmware predating these file-backed formats cannot read the
new settings or replay authority. Saved WiFi credentials and native
administration may be unavailable. Keep a compatible application when updating
an initialized radio; do not erase settings or replay records to restore access.
Authenticated `auth forget KEY` removes a noncompiled replay record and
session to free a slot; it does not revoke retained credentials. Use a stable companion identity rather
than generating another identity for every command. A companion owns
its native CLI timestamps; keep its RTC synchronized. The raw-KISS reference
client instead keeps its timestamp alongside its seed and correlates replies
with a 64-bit CLI tag. Never run concurrent raw-KISS clients sharing the same
seed/timestamp file.

If RF login receives no reply, use authenticated web administration or another
working management connection to run `auth status FULL_PUBLIC_KEY`. It reports
that key's trust, last accepted timestamp and occupied replay slots.
`auth status` shows overall occupancy; `auth peer 1` through `auth peer 10` show
the full keys and timestamps without changing them. If all ten replay slots are
occupied, identify an obsolete principal before using `auth forget KEY`.
Changing permissions or singleton trust alone does not free a replay slot.
Session capacity rejection does not consume a timestamp; retry after an
obsolete session expires or use `auth forget` for an obsolete principal.
A replay commit/readback failure disables native administration until restart.

Host-native checks for ACL permissions, all five Management/bot callers,
legacy records, reboot, revocation and storage failures:

```sh
make -C firmware/esp32 admin-cli-core-test ONCHIP_BOT_WASM=0
make -C firmware/esp32 admin-cli-core-test ONCHIP_BOT_WASM=1
```

Web Host headers are allowlisted even when Origin matches: configured hostname,
`.local`, actual WiFi IP, or explicit `ONCHIP_MAST_WEB_HOST`, using the HTTP
listener's port. Tokens use occupied slots and unsigned elapsed expiry, including
millisecond-counter wrap. A corrupt replay record returns unavailable instead
of stopping the shared runtime. It does not disable the independent signed
operator-key management path.

**Prefer the installed `meshcli` and a companion radio for ordinary RF
administration.** Connect with `meshcli -s /dev/serial/by-id/YOUR_COMPANION`
(or `-t COMPANION_HOST -p COMPANION_PORT` for a native companion TCP endpoint,
not a KISS socket). With the companion on the mast's PHY, use these interactive
commands; replace the public key and password placeholders:

```text
add_contact MAST_PUBLIC_KEY 2 mast
reset_path mast
login mast ADMIN_PASSWORD
cmd mast status
wmt8
cmd mast "bot status"
wmt8
```

For an already trusted full companion key, use `login mast ""` instead.
`cmd` does not request a delivery ACK; read the native encrypted CLI reply
with `wmt8`. If the reply is already queued, retrieve it with `sync_msgs`.
Ordinary commands need neither correlation tags nor a separate RF client.
After login, the app's native status and telemetry requests also work on the
management identity. `meshcli` exposes them as `req_status mast` and
`req_telemetry mast`. Status uses the upstream repeater layout and reports the
management role's packet, queue and airtime counters, device uptime and board
battery reading. Telemetry uses native Cayenne LPP with measurable board voltage,
available MCU temperature and permitted sensor readings. Unsupported board
voltage is omitted from telemetry; native status retains the zero-battery
sentinel. These RF requests are separate
from `telemetry status`, which reports the optional VictoriaMetrics publisher.
File deployment alone needs the bounded chunk/receipt, hash and activation
logic below; those commands still use the same native login and CLI framing.

Existing signed `rf.py` requests, status and receipts retain their wire
formats and role journal. Their verification key remains
`ONCHIP_OPERATOR_PUBKEY`, distinct from a trusted companion key. Native
repeater/room `setperm` still changes that role's ACL, not mast authorization.

For web access, open `http://MAST/admin`. Login uses the same configured admin
password. Two sessions, each expiring after ten minutes, are available;
logout revokes a session immediately. Commands require the session header,
and browser cross-origin mutations are rejected. The web server uses a bounded
mailbox to invoke the same dispatch-task backend as RF; HTTP callbacks do not
access the PHY or run Lua.

An HTTP `504` from `/admin/command` means that mailbox did not finish within
two seconds. For a mutating command, inspect the saved/current state before
retrying: a timeout alone does not establish whether the change occurred.
After connectivity recovers, `bot diagnostics` reads the command bot's queued
and dropped serial-log counts without opening USB. `sink=async` uses the shared
diagnostic worker; `sink=unavailable` means no queue was installed. `queued`
counts copied log records, not confirmed USB delivery. `dropped` includes a full
or unavailable queue and messages exceeding the fixed formatting buffer.
These unsigned 32-bit counters reset on startup and are not cleared by reading.
Current faults remain available in `bot log`, even when a log record is dropped.

Command VM completion, admission and fault logs do not wait for USB on the
dispatch thread. They share the existing eight-entry companion diagnostic queue;
there is no disk log or additional worker on a combined companion/bot image.
A stalled consumer discards new diagnostic records when that queue fills,
not radio commands. This bounds that logging path; it does not make every
management operation finish within two seconds or identify the cause of a
particular timeout. Keep [telemetry TLS diagnostics](TELEMETRY.md#a-write-fails-without-an-http-response)
and `companion stats`/`companion errors` separate when correlating a stall.

**HTTP is not encrypted.** Use a trusted LAN or an authenticated TLS tunnel.
Do not expose this beta HTTP service to the Internet. The editor computes
SHA-256 locally without depending on browser `crypto.subtle` or a CDN.

The supplied reference client supports raw-KISS RF and authenticated web
access; it is not required for ordinary stock-companion administration:

```sh
make -C firmware/esp32 mast-cli ARGS='--gateway GATEWAY --target MANAGEMENT_PUBLIC_KEY --seed-file /private/companion.seed --password-file /private/mast-password command status'
make -C firmware/esp32 mast-cli ARGS='--web http://MAST --password-file /private/mast-password command status'
```

Credential files must be private regular files, mode `0600`. An RF seed is
32 bytes. Password files contain the password only, with no trailing newline.
Omit the password file for trusted-full-key RF login. The gateway is a separate
KISS radio, not the target mast; its PHY must match the mast.

## Common control commands

| CLI command | Behavior |
| --- | --- |
| `status` | Applied/saved native role masks, durable role generation, saved bot selection and effective PHY/generation |
| `stats [TOPIC]`, `stats help` | Versioned read-only hardware/runtime statistics; named fields and units, no counter reset; see the [stats API](../shared/STATS.md) |
| `ver`, `board`, `help [TOPIC] [PAGE]` | Read the firmware profile/platform or one requested help page; `help` / `help 2` / `help 3` list every topic; `help wifi [PAGE]` and `wifi help [PAGE]` are equivalent |
| `get name`, `set name TEXT` | Read/save Management's display name, preserving its key |
| `get owner.info`, `set owner.info TEXT` | Read/save Management's own 0..119-byte owner text; `|` separates lines; native binary owner information uses the same record |
| `get radio`, `get freq`, `get tx` | Read the effective shared radio configuration |
| `get autoadvert`, `set autoadvert on\|off` | Read or save/apply the device-wide startup/periodic advert switch; manual app/admin adverts remain available |
| `roles MASK` | Save mask 0..15: repeater=1, room=2, companion=4, observer=8; preserve the signed role journal |
| `roles`, `roles list [1\|2]` | Read named applied/saved role bits, then separate bot selection and Management/KISS availability; no save or reboot |
| `bot status`, `bot key` | Applied/saved bot selection and readiness, or its separate public identity |
| `bot stats`, `bot admission` | Read-only reply/rejection/VM/notice counters; last admission gate, remaining retry wait, active jobs and distinct throttle/busy counters |
| `bot diagnostics` | Read-only command serial-log queue/drop counters; no USB connection or counter reset |
| `bot on`, `bot off` | Save next-boot bot selection independently of the other roles |
| `role help`, `role config ROLE` | Discover runtime name/key/channel/advert capabilities; ROLE is bot, repeater, room, companion, management or kiss |
| `role name ROLE [TEXT]` | Read or persist/apply a display name without changing its identity |
| `role advert ROLE zerohop` | Queue a native zero-hop advert for a running RF role; KISS is not an advert target |
| `role key ROLE [pending\|rotate]` | Read the saved public key, inspect a pending key, or deliberately generate/stage a replacement; restart applies it |
| `key ROLE`, `key ROLE pending`, `key ROLE cancel` | Read active/pending public identity, or cancel an unapplied key change; includes management |
| `key ROLE HEX128` | Stage an operator-supplied native 64-byte private identity; authenticated encrypted Management RF only |
| `password`, `password help` | Report the Management password source or setter syntax; never return the password |
| `password HEX` | Save a 1..15-byte Management password for new RF/web logins; encrypted Management RF only |
| `role password repeater\|room HEX` | Save/apply the active role's administrator password; authenticated encrypted Management RF only; use the secret-file CLI below |
| `role channel ROLE SLOT [off\|NAMEHEX KEY32]` | Read channel metadata or configure a native 128-bit group key; never return the PSK |
| `apply`, `reboot` | Reboot after the acceptance reply; `apply` uses the saved role selections |
| `wifi ssid TEXT`, `wifi password TEXT` | Encrypted RF only; save literal printable SSID/password text; `wifi password -` explicitly selects an open network |
| `wifi ssid hex HEX`, `wifi password hex HEX` | Explicit byte setters; SSID supports UTF-8/control bytes except NUL; no text/hex guessing |
| `get wifi.enabled`, `get wifi.ssid`, `get wifi.ip`, `get wifi.status` | Saved enable/credential configuration and current connection readback; printable ASCII SSIDs return `> SSID`, other bytes return `> hex HEX`; status is the Arduino WiFi status number |
| `get wifi.pwd` | Password readback over encrypted Management RF only; do not publish the response or collect it in ordinary diagnostic captures |
| `set wifi.ssid TEXT`, `set wifi.pwd TEXT` | Save literal text over encrypted Management RF; spaces are retained, and an explicitly empty password selects an open network |
| `set wifi.enabled 0`, `set wifi.enabled 1` | Save and defer WiFi stop/start until after the response; disabling keeps RF roles/admin running and preserves credentials |
| `wifi apply` | Reconnect using saved credentials after the response |
| `wifi status`, `wifi forget` | Report saved/connected state without exposing credentials; removal requires encrypted RF and restores compiled/SDK credentials on the next join |
| `radio HZ BW_HZ SF CR DBM` | Save and apply the shared PHY after the old-PHY acceptance transmission |
| `tempradio SECONDS HZ BW_HZ SF CR DBM` | Apply a temporary PHY for 1..3600 seconds, then restore the complete durable profile |
| `trust FULL_PUBLIC_KEY`, `trust none` | Set/remove the additional persisted full companion key; a compiled trusted key is unchanged |
| `setperm KEY64 PERMISSIONS` | Save a Management ACL permission byte using MeshCore's native role mask; role 3 grants Management/bot administration, role 0 removes |
| `get acl [KEY64\|1..5]` | Inspect Management ACL capacity or one full-key permission entry; reports effective administrator authority and retained legacy/compiled trust |
| `job` | Latest deferred-control outcome, including failures |

PHY ranges are 150..960 MHz, an enumerated SX1262 bandwidth up to 500 kHz,
SF5..12, CR5..8, and 0..22 dBm. Follow local spectrum rules. The lab profile
uses **912.525 MHz, 250 kHz, SF7, CR5, 2 dBm**.

WiFi field setters save configuration without reconnecting. Use `wifi apply`
after saving SSID and password. The enabled setting defaults to `1` and remains
saved across restarts; `on`/`off` are also accepted setter values. If the enable
record cannot be read, startup leaves network joining disabled and reports the
error; recover through encrypted RF.
If the response transmission fails, the saved setting can differ from the
running connection. Inspect `get wifi.enabled`, `wifi status` and `job` before
retrying.
HTTP WiFi provisioning is refused by both the page and backend.

Text setters preserve every byte after the separating space, including leading
and trailing spaces, quotes, backslashes and `|`. They do not unquote text or
guess hex: both `set wifi.ssid 4142` and `wifi ssid 4142` save the four-character
SSID `4142`; `wifi ssid hex 4142` saves `AB`. CLI input remains printable ASCII. Encode UTF-8 or
other SSID bytes as hex; SSIDs are 1..32 decoded bytes and cannot contain NUL.
Passwords accept an explicitly empty value, 8..63 bytes, or 64 ASCII hex
characters for a PSK. `get wifi.pwd` returns the exact saved value, including
spaces, only on encrypted Management RF; keep that response private.

With the default 16-hex tag and `|`, each command and reply has **145 content
bytes** within the 162-byte text limit. A maximum SSID plus password cannot fit
the combined `wifi SSIDHEX PWDHEX` form: save them separately. A 63-byte password
fits `wifi password hex HEX` with the tag; a 64-byte PSK in that long alias is
146 content bytes and does not fit. Use `wifi password TEXT` (78 bytes) or
`set wifi.pwd TEXT` (77 bytes) for that PSK. Neither form drops the tag.

**CLI migration:** before Aspen 0.1.9, the short `wifi ssid` and `wifi password`
forms decoded hex. Scripts encoding bytes must now include `hex` explicitly.
The combined legacy `wifi SSIDHEX PWDHEX` form retains its existing meaning.
To save literal text beginning with `hex `, use `set wifi.ssid` or `set wifi.pwd`.

`roles` is the same read-only first page as `roles list 1`. Its `applied/saved`
pairs describe the boot selection and next-boot selection; `roles list 2`
shows the bot separately. Management remains available with mask zero.
Observer is a mask bit, but has no `role config observer` command; KISS is the
shared modem service, not an RF role or mask bit. `roles N` still **saves a mask**,
not a list page. Apply saved selections only when you intend to reboot.

### Runtime role names and keys

These controls use existing role preferences and the checked identity staging
journal, not rebuilt defaults or a new authentication system. `command-bot`
is an alias for `bot`. Names accept 1..31 printable ASCII bytes without `:`,
persist across restarts and apply to subsequent native adverts/replies.
Bot names may be edited while the bot is disabled. Repeater, room and companion
preference reads/writes require the role to be running and not restarting;
enable it with `roles MASK` and `apply` first if needed. A saved name alone
does not transmit an advert.

`role name management TEXT` changes the separately RF-addressable mast
administrator's name. `role name kiss TEXT` changes the KISS service label
(the dashboard's `bot` slot). Each uses a small
versioned NVS name record with checked commit/readback; neither changes installed
WiFi/owner credentials, keys, source, policy, radio profile or role data.
Both apply live and survive reboot. Management works without WiFi and with all
optional application roles disabled. KISS and observer do not gain RF adverts.
Service key/channel changes are not exposed by this grammar.

Use `role advert ROLE flood` to announce repeater, room, companion, bot or
management through the mesh. For `Aspen-Bot`, run:

```text
role advert bot flood
```

Flood adverts use the role's current identity, native name/type, default region
and configured path-hash width. Repeater, room and companion use their own
default region; bot and management use the node's service region. Relays must
allow that region to forward the advert. KISS and observer have no advert
identity. The `/admin` page has a zero-hop/flood selector.

After renaming, use `role advert ROLE zerohop` for an immediate local
announcement. This uses the role's native advert data and current identity,
then native `sendZeroHop`: wire header `0x12`, direct route, zero path hashes.
Management advertises a native repeater-type CLI endpoint but never forwards
packets. The shared scheduler/airtime guards still apply. The authenticated owner
bot control skips the 15-minute advert interval so a rename can be announced
immediately after startup or a public advert. Bot flood adverts and public/Lua
bot adverts retain that interval, including after an owner zero-hop advert.
A busy, inactive or limited role returns an error. Success means **queued**,
not received over RF. Adverts
retain native RTC timestamps: peers may ignore older/equal timestamps, especially
after a restart without fresh clock synchronization. Confirm actual RF reception
and radio name readback separately. Plain native `advert` also floods the
native role's identity, not the bot's identity.

To keep a node quiet while retaining its existing contacts, use
`set autoadvert off`, then check `get autoadvert` for `saved=off live=off`.
The setting survives restart and suppresses repeater/room startup and periodic
adverts plus the bot startup advert. It leaves the native per-role advert
intervals unchanged; `set autoadvert on` resumes them. Companion and Management
have no automatic adverts. The companion app Advert button, native `advert`,
and authenticated `role advert ROLE zerohop|flood` still send deliberately.
Operator-installed Lua/Wasm programs can also explicitly request adverts;
disable such scheduled requests in those programs if configured.

The setting does not retract an already queued or transmitted advert. Set it
before changing mesh profiles, allow existing transmissions to finish, and
restart on the old profile if startup adverts were already queued. Without a
saved setting, automatic adverts retain their previous enabled behavior.
An unreadable or invalid setting disables automatic adverts at boot and reports
an error; repair it with `set autoadvert off` before retuning.

For example, persist service labels using the authenticated CLI or
`/admin` command backend:

```text
role name repeater Hill-Relay
role name room Hill-Room
role name companion Hill-Base
role name bot Hill-Bot
role name management Hill-Admin
role name kiss Hill-KISS
role advert repeater zerohop
role advert room zerohop
role advert companion zerohop
role advert bot zerohop
role advert management zerohop
```

`role key ROLE` reads the saved active **public** key without activating any
pending record. `role key ROLE rotate` generates a fresh native identity and
durably stages it; `role key ROLE pending` returns only its pending public
key. Both explicitly say that a restart is required. An already pending
change, missing initialized identity or storage failure is an error, not a
second rotation. The running identity is unchanged until role reconstruction
or device restart. `apply`/`reboot` still wait for the acceptance reply to
transmit. The `role key` commands generate identities and return public keys only.
Importing an existing private identity uses the separate encrypted-RF-only
`key` command below. No command exports private identities, and signed-only
role-management envelopes do not carry secret keys. Existing encrypted native
repeater/room administrator key staging remains separate. Routine application
updates and name/channel changes do not rotate any identity.

| Role | Application group-channel configuration |
| --- | --- |
| Bot | Slot 0; save a PSK and name, then reboot to apply |
| Companion | Slots 0..7 in this profile; save/apply live in its native channel store |
| Repeater / room | No application group-channel keys: repeater forwards native ciphertext; room is a native DM service |

`NAMEHEX` encodes 1..31 printable ASCII bytes; `KEY32` means exactly **32 hex
characters / 16 key bytes**, not a 32-byte key. All-zero keys are rejected;
use `off` to disable the slot. Readback includes a short key fingerprint,
never the PSK. A disabled companion slot neither receives zero/previous-key
traffic nor sends text/data, and stays disabled after restart. Companion
channel-save failures return errors (also through the stock companion API).
Failed preference/channel writes may leave the saved state uncertain; the
common role API restores the previous live value and does not claim a
transactional filesystem rollback.

Trusted-owner bot DMs support `!admin role help`, `role config`, `role name`
and `role key`/`role advert` (prefix each with `!admin`). Channel-secret provisioning stays
on the existing encrypted management CLI or authenticated web interface,
not the Lua-facing admin bridge. Plain HTTP itself does not provide
confidentiality: use the encrypted RF CLI or a protected management network
for secret provisioning. There is no private-key provisioning over the
signed-only role-management protocol.

Key rotation changes data ownership, not just a label. Bot KV, timers and
reminders remain bound to the old full bot identity; channel data remains
bound to the full channel-key identity. Old records are neither silently
migrated nor erased and still consume their bounded storage slots. Old
reminders are not delivered by the new bot identity, and scoped snapshots
cannot be restored under another identity. Deliberately clear unwanted
scoped data while its original identity is active. Owner grants and source
installation remain node policy, not identity-private records.

All four roles share **one physical radio profile**. Use `radio` or
`tempradio`, not per-role frequencies; native preference reporting reflects
that authority. Ordinary path widths 1/2/3 are separate from TRACE's
power-of-two width encoding.

### Go-host native bot identity

The Go host's same-UID owner socket can stage and apply an operator-supplied
native bot key without restarting the host or reconnecting its shared radio
source. Use this only for `bot_runtime: "native_lua"`. Other host roles keep
their keys and remain running. RF peers, HTTP clients and ordinary Lua cannot
rotate this identity.

Keep the raw 64-byte native scalar-prefix key in a **0600 regular file**.
The CLI rejects symlinks, seeds, unclamped scalars and a mismatched expected
full public key before opening the socket. Do not put private keys in shell
arguments or history. The expected public key is 64 hex digits:

```sh
SOCKET="$HOME/.local/state/meshcore-host/bot/native/admin.sock"
python3 tools/hardware/admin.py --unix-socket "$SOCKET" command 'key bot'
python3 tools/hardware/admin.py --unix-socket "$SOCKET" \
  key-import bot --native-key-file /absolute/selected-native.key \
  --expected-public-key FULL_PUBLIC_KEY_HEX
python3 tools/hardware/admin.py --unix-socket "$SOCKET" command 'key bot pending'
# To discard the stage instead: command 'key bot cancel'
python3 tools/hardware/admin.py --unix-socket "$SOCKET" --timeout 80 command 'key bot apply'
python3 tools/hardware/admin.py --unix-socket "$SOCKET" command 'key bot'
```

Staging reports `KEY <new-key> pending; apply required`; the running identity
and dashboard remain unchanged. A Go restart retains the stage but does not
apply it. Cancel clears only the pending key. A different pending key or
another role's active/pending identity is rejected.
Importing the already active public key reports `already active; no apply
required` without changing its bytes or any pending stage.

Apply reloads only the native bot worker. The bot temporarily stops accepting
commands while its selected source becomes ready. It commits the new identity,
enables the worker and attempts one signed zero-hop advert with the saved name.
The reply reports `applied; zero-hop advert queued`. If it reports `not queued`,
retry `advert.zerohop` once the radio queue is available. The dashboard,
`key bot` and future HELLO all use the saved new full key.
The companion must learn the new key before sending encrypted bot commands;
the bot also needs a fresh companion advert after its worker reload.

Old bot-private notes, timers and reminders remain under the old **full**
identity, inaccessible to the new key. Scoped snapshots from the old key
cannot be restored into the new one. Nothing silently moves or erases those
records. Source installation and owner grants remain device policy; channel
keys, other roles' keys and radio PHY are unchanged. Saved bot preferences
that normally apply at worker startup also take effect during apply.

In-flight bot coroutines end at reload. Packets already admitted by the mast
may still transmit with the old identity; unresolved outcomes are unknown,
never automatically replayed. Before committing, candidate startup or
validation failure reloads the old identity and retains the stage. A failed
durability check or activation after commit leaves only the bot stopped.
Inspect `key bot` and `key bot pending`, then retry `key bot apply` after
correcting the reported worker/storage fault. The retry uses the saved
authority; do not infer the active key from a timed-out apply request.

The host saves active/pending keys together in
`<state_dir>/bot/identity-state.json` (0600). Its existing seed is retained
but no longer selected once that envelope exists. Native NVS is not a second
identity store: each child receives the key through Go HELLO.
If native bot data exists but its Go key authority is missing, startup fails
instead of generating a replacement key. Restore that bot's identity before
starting it again.

Run `make -C firmware/esp32 bot-native-host-test` for the production worker's
queued-mast integration tests, including stage/cancel/apply/restart,
candidate failure, shared-source retention and full-identity data isolation.
These tests create disposable local keys and do not touch a radio.

### Importing an operator-supplied identity

Use the existing encrypted Management RF login to install a prepared MeshCore
identity without rebuilding firmware. Supported roles are `bot` (alias
`command-bot`), `repeater`, `room`, `companion` and `management`. The role must
already have an initialized identity. KISS and observer are not import targets.

The CLI reads a mode-0600 raw native key file, derives its full public key and
compares it with the expected manifest key before opening the RF connection:

```sh
make -C firmware/esp32 mast-cli ARGS='--gateway INDEPENDENT_RADIO_IP \
  --target MANAGEMENT_PUBLIC_KEY --seed-file /absolute/operator.seed \
  key-import bot --native-key-file /absolute/selected-native.key \
  --expected-public-key EXPECTED_FULL_PUBLIC_KEY'
```

Install the CLI dependencies from `requirements-sign.txt` first. The native
file contains exactly 64 expanded private bytes, not a 32-byte seed, hex text
or a seed/public-key pair. The CLI rejects web imports, hides
unexpected remote/error text that could echo the key, verifies public readback,
and does not reboot. An unknown reply or readback requires inspecting
`key ROLE pending` before proceeding. Existing `command` mode refuses private
`key ROLE HEX128` arguments; use `key-import` instead.

`key ROLE HEX128` stages exactly 64 native private-key bytes encoded as 128 hex
digits. Use tooling that reads a protected key file and suppresses request
logging; do not put private keys in shell arguments, history, web editors or
chat transcripts. The backend rejects secret imports from web and Lua callers.
Signed-only operator messages do not provide this command. The native encrypted
RF handler wipes its plaintext command buffers after handling the request.
Use canonical `bot` rather than `command-bot` when importing through a companion
with a 16-digit request nonce: the longest canonical command is 143 bytes
(`management`), or 160 including the nonce and separator, exactly the native
companion text limit. Do not truncate a request to make it fit.

The reply contains only `Pending PUBLIC_KEY; reboot required; peers must learn
new key`. Compare the **entire derived public key** with the prepared manifest
before applying any changes. Prefixes come from the supplied valid identity;
they are not separately editable fields. These public operations are safe to
use for readback:

```text
key command-bot
key command-bot pending
key management pending
key help
```

The running role continues using its old identity until reconstruction.
Management requires a device restart; reconnect and log in at its new public
address afterward. Read the active key again and request
`role advert ROLE zerohop` after application. Names, channel keys, native
preferences, owner credentials and shared PHY are not changed by import.
Full-key contacts, owner trust and ACLs may need explicit updates when one of
their principals changes. Review those dependencies before switching the
companion used to control the device.

Repeating the same pending import is idempotent; importing an already-active
identity reports that no reboot is required. A different pending key is
rejected. To withdraw an unapplied change, inspect it and use `key ROLE cancel`;
the active key is retained. An in-progress native role reset cannot be
overwritten or cancelled through this command.

Imports also reject a full public key already active or pending for another
on-device role, including stored modem/observer identities. This compares
derived public identities, not private-byte equality or short prefixes.
Missing other-role records are allowed; unreadable or corrupt records reject
the import without writing. This bounded check adds no persistent records and
does not modify other roles. It applies to this supplied-key import command,
not the separate native role-local staging commands.

A failed write/readback returns an explicit unknown outcome. Check the pending
public key before reboot; retry only the same intended key, or explicitly cancel
the pending change. Import uses the existing single-key identity journal:
64 bytes active, 133 bytes with a pending identity, three additional NVS entries
per staged role. Activation returns to the active format. No new identity store
or partition is introduced. Existing bot data remains bound to its old full
identity as described above; import does not migrate or erase it.
Each role is staged separately, not as an atomic fleet-wide transaction.
Keep the old and new public-address manifest until every requested role has
restarted and its active key has been read back.

The field helper reads the existing protected fleet manifest and imports keys
without placing private material in arguments or logs:

```sh
make -C firmware/esp32 owner-field-rekey-aspen KEY_MANIFEST=/absolute/path/to/manifest.json
make -C firmware/esp32 owner-field-rekey-aspen-base KEY_MANIFEST=/absolute/path/to/manifest.json
```

The first operation replaces Relay, Room, Bot and Admin and reconnects through
the new management identity. The separate Base operation requires those four
replacements to be active; switch the chat client's expected sender afterward.
Both operations preserve names/settings, verify full active public keys after
reboot, and observe signed zero-hop adverts on Birch.

### Recovering a repeater or room administrator password

Management can replace an active ESP32 repeater or room administrator password
without knowing the old role password. Use an already authorized Management RF
connection; role administrator or guest access alone is insufficient.
The server checks the full authenticated Management sender and encrypted RF
transport. Web, local bot-owner sockets and Lua `node.admin`/`!admin` cannot
perform this operation. No plaintext web password form is provided.

Create a new protected credential file without printing the value:

```sh
install -d -m 700 .tmp/owner-credentials
python3 - <<'PY'
import os, secrets, string
fd = os.open(".tmp/owner-credentials/new-role-admin-password",
             os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
with os.fdopen(fd, "wb") as output:
    output.write("".join(secrets.choice(string.ascii_letters + string.digits)
                         for _ in range(15)).encode("ascii"))
PY
python3 tools/hardware/admin.py \
  --gateway COMPANION_KISS_HOST --port COMPANION_KISS_PORT \
  --target MANAGEMENT_FULL_PUBLIC_KEY --seed-file /absolute/private/companion.seed \
  --password-file /absolute/private/management-password \
  role-password repeater --new-password-file .tmp/owner-credentials/new-role-admin-password
```

Omit `--password-file` only when the companion's full key already permits
trusted-key Management login. It authenticates **Management**; the separate
`--new-password-file` supplies the new **role** administrator credential.
Files must be private regular files, not symlinks, containing exactly 1..15
printable ASCII bytes without a newline. Raw `command 'role password ...'`
arguments are rejected to keep the new credential out of process arguments
and shell history. Select `room` in a separate call to update that role; the
other role is not changed automatically.

Expected: `Saved and applied repeater administrator password; ACL/sessions
unchanged` (or `room`). The native preference writer and password readback must
both succeed before this response. The running role accepts the new credential
without a restart, and its saved preferences retain it for subsequent boots.
Existing native ACL admissions/sessions remain valid, just as with a native
role password change. This does **not** revoke earlier administrators.
Identity, name, room guest password/access, routing, Management password/trust
and other roles are not changed. The role must be active and outside a lifecycle
transition.

An unavailable or inactive role is reported as unchanged. A write/readback
failure restores the running role's old administrator password but reports
saved state as **unknown**: the native preference file may have been partly
written or may already contain the new credential. A missing RF reply is also
an unknown outcome. Verify role login and settings before any retry or restart;
do not blindly replay the write or assume that saved and running state agree.
Keep both credential files private until recovery is resolved.

After a successful update, reconnect the official app using the new role
credential and check for **Repeater Admin** or **Room Admin**. Refresh status
and settings without applying unrelated changes. Application-only flashing
does not itself replace saved role passwords. For Go host roles, configure the
host's private `admin_password_env` instead; see
[Android host administrator setup](../shared/ANDROID.md#enable-administrator-login-on-host-roles).

### Changing the Management password

Log in to the existing encrypted Management RF identity and send
`password HEX`, encoding a nonempty password of 1..15 printable ASCII bytes as
2..30 hex digits. Use protected-file tooling that suppresses request logging;
do not place credentials in shell arguments, history or plaintext web commands.
Web/Lua callers cannot set the password. `password` reports only whether the
current credential comes from runtime storage, the compiled initial password,
or is disabled. `password help` reports the syntax without reading a secret.

The new password applies immediately to **new RF and web logins**, and survives
restarts. The compiled password is used only before a runtime password has been
saved; it is not an alternate login after rotation. Existing authenticated
sessions remain valid until their normal expiration (RF 15 minutes, web 10
minutes), logout where supported, or reboot. Verify a new login before issuing
`reboot` to end existing sessions. Trusted public keys, role-local administrator
passwords, identities, WiFi credentials and radio settings are unchanged.

A failed write/readback reports an unknown outcome: keep the current session
and check a new login with the intended password or a trusted key before
restarting. A corrupt or unreadable password record disables password login,
without falling back to the compiled credential. An already authenticated
session or trusted-key RF login can replace that record with `password HEX`.
Keep a verified trusted-key login for recovery before rotating the password.
There is no password export or implicit reset command. Firmware predating this
runtime credential record still uses its compiled password; do not assume the
override applies after a firmware downgrade.

The checked record uses 52 bytes (four NVS entries) in the existing
`mc-mast-admin` namespace. Like native role/WiFi credentials and identities,
the underlying NVS record is not encrypted at rest; protect physical access
and storage images. Bot-data export excludes this namespace. Runtime password
configuration does not add another authentication or transport system.

### Command channel and path policy

The optional native command-channel policy uses the same authenticated backend:
`bot channel #commands` (or `off`), `bot path 3`, `bot airtime 1200`, and
`bot policy`. These save a separate checked record and apply at reboot.
`bot adaptive [on|off]` reads or saves opt-in local-load-aware bot admission;
the saved choice applies at reboot and defaults to off. The readback includes
live/saved selection, measurement availability, local load, allowance scaling,
caller slots, pending TX and denials. See
[adaptive admission](../runtime/BOT_RUNTIME.md#opt-in-adaptive-airtime-admission) before
enabling it; missing measurements refuse adaptive work rather than inventing
radio availability.
Channel nicknames confer no caller/owner authority; private KV and caller DM
operations remain unavailable to channel handlers. See
[the channel contract](../runtime/BOT_RUNTIME.md#optional-hashtag-command-channel).
`role-path 3` persists/applies three-byte paths to the active native repeater,
room and companion plus the independent management service's originated
flood fallback, without logging that companion into a role administrator
account. Management uses its own checked `mc-onchip/origin-path` record
(default one byte); it does not depend on a selected repeater, companion or
bot. Existing role journals and signed status/receipt formats are unchanged.
Requester-return paths still preserve the request's width. Bot originated
ACK/reply floods use its separately saved `bot path` policy.
`role-path` reports each current width; zero means inactive. Failed
multi-role saves explicitly report possible partial changes. `room access`
reports inactive, open or password-protected guest access without revealing
or changing the preserved room credential.

### Applying radio and WiFi changes

RF retuning follows transmission of the acceptance reply. HTTP retuning
follows sending its response. A 500 ms guard allows the connection to settle;
an active RF transmission finishes before the change. Old-generation queued
jobs fail with `STALE`. `CONFIG GET`, dashboard data and radio statistics
report the effective generation so a following host can refresh its PHY.

Failed acceptance transmission cancels the deferred action. Inspect saved
and applied state after a lost reply: durable settings and their later
apply/reconnect have separate outcomes. A temporary profile returns to the
saved PHY, including after a reboot.

WiFi credentials survive application updates. Configure them through the
owner connection and inspect connection status after applying a change.

## Source and help installation

The device installs **one source set containing up to eight named handlers**
in the shared bot environment. A source package is plain Lua plus a compact
Lua-comment header for package name/version, runtime/API, required native
capabilities and data-schema compatibility. Package metadata and the Lua
candidate are checked before the source generation changes. All cooperating
functions remain in one source set; there is no per-function VM or second
upload protocol. To replace or wrap a builtin, declare
[`override_command(name, export)`](../runtime/BOT_RUNTIME.md#explicit-builtin-overrides-and-original-dispatch)
in that source. The builtin's schema and authorization stay in force;
`call_original` invokes its bundled handler without re-entering the override.
`source api overrides` reports this contract. Native management is never delegated to Lua. A
source-recovery fault quarantines custom handlers, not initialized native
diagnostics.

```sh
make -C firmware/esp32 bot-plugin-install PACKAGE=handlers.bot.lua \
  MAST_CLI_ARGS='--web http://MAST --password-file /private/mast-password'
make -C firmware/esp32 bot-plugin-status \
  MAST_CLI_ARGS='--gateway GATEWAY --target MANAGEMENT_PUBLIC_KEY --seed-file /private/companion.seed --password-file /private/mast-password'
make -C firmware/esp32 bot-plugin-rollback \
  MAST_CLI_ARGS='--web http://MAST --password-file /private/mast-password'
```

The browser provides source read/edit/install, source help read/change,
rollback and removal. Both transports use the same bounded numbered source
chunks and durable activation journal. Direct source uploads remain available
for recovery and editing; package metadata is a comment in the uploaded Lua
source and consumes part of the 4,096-byte source envelope. The CLI's
`package-install` checks the device API/capability report and current schema
before beginning transfer. Firmware repeats the metadata/schema check before
activation and enforces the declared previous-schema compatibility on
rollback. See [BOT_RUNTIME.md](../runtime/BOT_RUNTIME.md#compact-package-metadata-and-compatible-schema-changes)
for the compatible transaction-based migration contract.

Both transports use these bounded commands:

| Command after `source` | Result |
| --- | --- |
| `begin ID16 SIZE SHA256` | Admit/resume an upload with a 16-hex identifier, 1..4096 uncompressed bytes and full SHA-256 |
| `chunk ID16 INDEX HEX` | Persist the next numbered chunk, at most 48 bytes; return `ACK ID16 next=N` only after readback and progress commit |
| `commit ID16` | Start asynchronous source verification and durable activation; poll `status` for the final result |
| `status`, `hash` | Deployment generation, selected/previous slots, upload progress, live outcome and full source hash |
| `metadata`, `api package` | Report the active package schema and the device's Lua runtime, API version and supported metadata capabilities |
| `read INDEX` | Read at most 48 source bytes as hex; clients verify the complete hash and stable generation |
| `help TEXT`, `help -`, `helptext` | Save/change/remove or read up to 96 bytes of source help |
| `rollback`, `remove` | Restore the previous source/help or select bundled handlers |
| `retry` | Retry the verified durable selection; inspect terminal status/live outcome |
| `cancel` | Cancel a staged upload without changing the active source |

There is one admitted upload. Indexes are sequential; gaps are rejected with
the required next index. Identical duplicate chunks are acknowledged without
rewriting; conflicting duplicates fail. Repeating `begin` with the same
identifier/size/hash resumes after disconnect or reboot. The client retries
only the same numbered chunk on a timeout, not arbitrary administrative
mutations. At the 4 KiB limit there are 86 chunks; hex and native framing have
real airtime costs. No bytecode or compression is accepted.

Upload data, fixed staging source and three bounded source slots reside in
SPIFFS. A single NVS journal atomically records the deployment, previous slot,
per-slot help and upload progress. A candidate never overwrites the current
or previous slot. The worker checks size/hash and compiles/initializes Lua
under the existing VM limits; a copied slot is read back before the journal
switch. Activation then uses the bot's `stageSourceFile`/`activateStaged`
hooks. The result distinguishes durable selection from successful live
activation. Disabled bots can receive a verified durable program without
starting a bot radio identity; it loads on the next enabled boot.

File copy/readback and Lua validation run on the existing bounded worker,
not radio dispatch. Copying uses its existing source buffer, at most 256-byte
SDK operations and a separate 2-second deadline. The source read/hash deadline
remains 100 ms. Lua loading/compilation has a separate 330 ms aggregate ceiling;
initialization has a separate 50 ms ceiling, while active invocation and
one-shot cleanup retain their 20 ms ceilings. Suspended native I/O and RF
waiting do not consume active execution time; their own deadlines and airtime
limits still apply. Individual
SDK calls are not preemptible. A source's byte size alone does not guarantee
it can compile within the VM budget.

Bad candidates leave the active program unchanged. Corrupt deployment
journals or failed boot-time source checks quarantine custom command admission
but retain initialized native diagnostics and mast control. Use `source rollback` for a valid
previous slot, or explicitly `source remove` to recover a corrupt journal
and restore bundled handlers. That recovery resets the unusable source
journal, not native role identities or their signed generation. SPIFFS is
never automatically formatted. Complete filesystem or NVS hardware failure
is not repaired by this installer.

## Developer checks

Run the native administration tests from a source checkout:

```sh
UBSAN_OPTIONS=halt_on_error=1 make -C firmware/esp32 beta-test \
  TEST_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
```

The suite exercises login and replay handling, HTTP commands, source
installation, restart and rollback, scoped storage and injected failures.
Hardware commissioning helpers select a specific bench inventory and require
private backups. Use the public setup and update guides for your own radio.
See [contributor setup](../../CONTRIBUTING.md) for local dependencies.
