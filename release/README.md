# Product versions and candidate builds

Aspen, Birch, Pine and Willow each use an independent Semantic Version. A fix to one product
does not require a release of the other. MeshCore's version identifies the
upstream base and is recorded separately.

| Product | Series | Current source version | Next product fix |
| --- | --- | --- | --- |
| Aspen | Maintenance | `0.1.12` | `aspen-v0.1.13` |
| Birch | Maintenance | `0.1.1` | `birch-v0.1.2` |
| Pine | Experimental | `0.1.0` | `pine-v0.1.1` |
| Willow | Experimental | `0.0.8` | `willow-v0.0.9` |

Source versions can be ahead of published releases. The latest published
Willow source release is [0.0.8](https://github.com/slepp/meshcore-aspen/releases/tag/willow-v0.0.8);
the current published Aspen firmware is [0.1.11](https://github.com/slepp/meshcore-aspen/releases/tag/aspen-v0.1.11).

Show users **Aspen 0.1.12 · based on MeshCore 1.17.1**. Release candidates
use `-rc.2`, `-rc.3`, and so on. Use SemVer's usual patch/minor/major meaning;
while below 1.0, a minor release can change a supported API. An upstream update
is a product release too: choose the product bump according to its effect on
users, then record the new upstream tag and full commit. Never move a release
tag to another commit.

[`products.json`](products.json) is the version authority. Edit only the product
being released, run `python3 tools/product_versions.py`, then `make release-check`.
The generated firmware header, Go constants and Willow version modules share
this authority. Companion device information reports `aspen-0.1.12`,
`birch-0.1.1` or `pine-0.1.0`;
the full identity must fit 19 ASCII bytes plus NUL. The dashboard exposes the
full upstream tag and commit. Host `ver` includes product and MeshCore versions.
Keep full source/build information in each candidate manifest.

Willow is an experimental Hew source-build release. Its native
worker is verified against the local source/build tree, so the source release
must be built at its final installation path. Use its
[setup and retained-state upgrade guide](../hosts/willow/README.md).
**Birch 0.1.1 is a Linux x86_64 host-tools and native-worker
bundle for an already configured queued-v1 WiFi/TCP shared modem.** The host
and worker share one product commit. Modem firmware is built and privately
provisioned separately; the host download contains no blank-credential modem
image. Start with [host installation](../HOST_GUIDE.md#install-the-birch-host-download).

## Supported contracts

| Interface | Current version | Compatibility |
| --- | --- | --- |
| Queued PHY | 1 | Host and modem must both implement the negotiated v1 queue/ownership contract |
| Native bot worker | 1 | Host and native worker must share the HELLO/READY and framing contract |
| MeshCore companion | 13 | Companion clients use the existing v13 fields and 20-byte firmware identity |
| Dashboard JSON | 1 | Existing fields remain; `upstream_tag` and `upstream_commit` are additive |
| Wasm application ABI | 1 | Existing MeshCore application SDK contract |

These are protocol versions, independent of product SemVer. The current base is
`companion-v1.17.1` at `d92964352441e53b93e8667b802e04f6e072b39e`.
The published Aspen image targets XIAO ESP32-S3R8 plus Wio SX1262. Additional
[Heltec V4 R8 and T-Beam Supreme SX1262 source profiles](../firmware/esp32/BOARDS.md)
build the full application; physical boot, RF and OTA testing remain to be done.
Birch's initial host target is Linux x86_64 with its matching WiFi/TCP modem.
Choose the exact board variant and its update target before installing.

## Build an unpublished candidate

Start from a clean checkout of a signed commit already available in the public
Aspen repository. The command checks that the selected public ref resolves to
that exact HEAD. It uses tracked public profiles and a reduced environment,
creates new build/output directories, and never reads private provisioning files.
Use Python 3.13, PlatformIO, the pinned ESP32 toolchain and the contributor build
dependencies. Birch also requires Linux x86_64, Docker and the public Go 1.26.7
SDK. Its host tools and worker build in the disposable Debian 12 image below.
ELF inspection uses GNU `readelf` from binutils.

```sh
make release-check
python3 tools/release_candidate.py build --product aspen \
  --public-ref refs/heads/main
# Run on Linux x86_64. Build a new image with the public Dockerfile hash label:
dockerfile_sha=$(sha256sum release/Dockerfile.debian12 | cut -d' ' -f1)
docker build --iidfile .tmp/debian12-image-id \
  --label "org.meshcore.release.dockerfile-sha256=$dockerfile_sha" \
  -f release/Dockerfile.debian12 release
python3 tools/release_candidate.py build --product birch \
  --public-ref refs/heads/main \
  --native-image "$(cat .tmp/debian12-image-id)" --host-only
python3 tools/release_candidate.py verify .tmp/candidates/EXTRACTED-CANDIDATE
```

Use `refs/heads/main` only when it resolves to the selected signed source commit.
For a tagged source revision, pass its exact `refs/tags/TAG` instead. The selected
public ref must still resolve to the checkout's HEAD.

The Aspen bundle name is `aspen-v0.1.12-xiao-esp32s3-sx1262-SOURCE12.zip`.
Its files include `manifest.json`, checksums, application and separate
initial-install images, source and relink archives, resolved dependency
inventory, exact build profile/hash, toolchain versions/compiler hash, upstream
tag/full SHA and public source full SHA. Hashes identify the packaged files.
The host-only Birch name is `birch-v0.1.1-linux-x86_64-SOURCE12.zip`.
It includes four host binaries, the installer and example configuration,
host guide, source, native relink material and dependency notices. Its
`public_birch_host` profile and `host_only` scope require an external queued-v1
modem and contain no firmware/partition images or firmware build receipts.
Omitting `--host-only` retains the older combined host/modem build for inspection;
that blank-credential modem is not an installable radio download.
The manifest records build information, not test status. Run the relevant
software and device checks before publishing; keep test logs internal.

Linux x86_64 identifies the architecture, not support for every distribution.
Each Birch manifest records the ELF interpreter, required SONAMEs and minimum
GLIBC, GLIBCXX, CXXABI and OpenSSL symbol versions for all four host binaries.
The builder cross-checks its ELF inspection with GNU `readelf`; the portable
Python verifier reads each packaged ELF and rejects a receipt that lowers or
otherwise changes its actual requirements.
Compare these with the intended host before installation and qualify that
distribution using the exact packaged worker. Birch now builds its Go tools and
worker in Debian 12 with GCC 12, OpenSSL 3 and cJSON. The image starts from a
pinned Debian 12 build-pack digest and adds CMake and cJSON development headers.
Its exact image ID, Dockerfile hash, package/library hashes and compiler receipts
are recorded. The verifier checks those receipts against the archived public
Dockerfile and packaged ELF files, and rejects symbol requirements above the
Debian 12 baseline. Fresh source and object directories prevent reuse of objects
from a newer host. Module downloads use a new public dependency cache; compilation
runs without external network access. Distribution support still requires the
exact packaged worker's software qualification results. Earlier host-native
candidates requiring GLIBC 2.43 retain their original receipts and limitations.

Firmware candidates prepare firmware in a fresh tracked-source tree. The builder
checks Crypto 0.4.0, CayenneLPP 1.6.1 and, where used, base64 1.4.0 against their
SHA256-pinned registry archives. A changed, missing or extra source file stops
the build; inspect or restore the named dependency rather than overwriting it
blindly. PlatformIO's generated `.piopm` metadata must identify the matching
library and version. The native worker uses freshly extracted package files.
Lua 5.5.1 and WAMR archives are checked before staging; extracted interpreter
sources and compiled objects from another build are not reused.

The manifests record the checked archives and the prepared Lua/WAMR source
files actually used. Relink archives include those inputs, and verification
checks their bytes offline. Firmware receipts also inventory all resolved
PlatformIO library files. Other PlatformIO libraries, framework packages and
toolchains retain their existing resolution rules: their resolved versions and
compiler hashes are recorded, but this is not a fully pinned PlatformIO build.

For first Aspen installation, follow [offline USB setup](../firmware/esp32/PUBLIC_SETUP.md).
Keep private SPIFFS setup and identity backups outside the public bundle. Ordinary
updates write only the selected application slot and retain node identities,
settings and programs; use the existing update guide. The bundle distinguishes
the application from initial-install bootloader/partition files.

**Birch modem setup:** the old generic UART image does not connect the
documented Go host, and a source-built WiFi modem compiles station credentials.
`platformio.birch.ini` builds a matching WiFi modem with blank credentials for
build inspection only. It cannot connect a newly installed modem. The host-only
download requires an existing configured compatible modem, or an operator source
build with private provisioning. Never put station passwords into a public
image. Installing the host does not flash or provision the radio.

## Publish a release

### Room terminal client

The independently versioned terminal client uses
`clients/room-tui/VERSION` and tags `room-tui-vVERSION`; it does not bump a
firmware product or replace the latest Aspen firmware download.

From a clean, signed source commit already published on `main`, with the
Go version pinned in the client's `go.mod` and Python 3.12 or newer:

```sh
python3 clients/room-tui/package_release.py \
  --public-ref refs/heads/main --out .tmp/room-tui-v0.1.0
cd .tmp/room-tui-v0.1.0
sha256sum -c SHA256SUMS
```

The packager builds from a new archive of tracked public source. It emits
static Linux amd64/arm64 archives, per-platform manifests and `SHA256SUMS`.
Each archive includes source, the Apache project license and Go/dependency
license notices. Its manifest records the exact source revision, Go version,
dependency versions/sums and binary hash. `room-tui --version` reports the
client version and source revision without opening device state.

Check the packaged executable's version/help and sign-in/restart behavior;
reuse the established Worker/browser/TUI integration results for unchanged
code. Execute arm64 version/help on ARM or a user-mode emulator as well as
checking its ELF machine type and static linkage. Publish with a signed
`room-tui-vVERSION` tag and `gh release create --latest=false`, then verify
downloaded asset hashes. Upgrading replaces only the executable; retain the
existing private state directory.

### Firmware and host products

1. Build from the selected clean, signed public source commit. Check package
   hashes, image layout, installation instructions and licensing/relink files.
2. Test changed behavior and the relevant install/update path, including retained
   identities, settings and programs. Reuse established results for unchanged
   code. Birch host-only publication needs the exact packaged worker and
   installation path checked on Debian 12; modem setup remains separate.
3. With the operator's publication approval, create a signed product tag and
   GitHub release. Attach the package, manifest and checksums; check downloaded
   assets against the local files. Do not publish private backups or test logs.

Changing an RC to a final version changes the embedded product identity and
requires a new build.

## Pine application download

Pine 0.1.0 uses `pine-0.1.0` and the product tag `pine-v0.1.0`
for the `nrfmast_fleet_lua` update.
Its manifest identifies the exact signed public source commit, pinned MeshCore
base, board profile and application images. Older version labels remain attached
to their original releases; updating the firmware label does not replace node keys.

The download updates an initialized XIAO nRF52840 + Wio SX1262 using its retained
bootloader and saved identities/credentials. It includes app-only UF2/BIN and
Legacy DFU ZIP, checksums, dependency notices and replaceable Arduino LGPL
relink material. It does not commission or erase a board, replace the
bootloader/SoftDevice, or migrate the QSPI layout. Interrupted BLE updates can
require USB recovery; keep the node physically accessible. Start with the
[Pine update prerequisites](../firmware/nrf52840/README.md).

## Willow 0.0.8

Publish observations to your own MQTT broker by setting `observer.url` to its
hostname, IPv4 address or bracketed IPv6 address. TCP, TLS, WS and WSS use the
existing publisher, credentials and topics. TLS certificate and hostname
verification remain enabled; use `observer.ca_file` for a private CA.
Migration retains the configured broker and observer identity. Stored data
and wire protocols remain compatible. See [observer setup](../hosts/willow/docs/SERVICE.md#native-mqtt-observer).

## Willow 0.0.7

The normal offline host check also uses short private paths for modem-negotiation
fixtures. This fixes a stale test import left by the earlier socket-path cleanup;
all five negotiation scenarios run at both optimization levels. Production
behaviour, stored state and wire protocols are unchanged from Willow 0.0.6.

## Willow 0.0.6

Install receive-only Lua/Wasm packet programs through the existing native bot
worker. Measured RF packets supply repeater SNR/RSSI history to commands,
the dashboard and metrics. Program selection, budgets and sources survive a
host restart; signal sample rings restart empty and survive modem reconnects.
See [packet programs](../hosts/willow/docs/PACKET_GUESTS.md).

The installer waits for the launched Hew process before checking its executable
and binds the dashboard page to the selected installation. Upgrades copy the
latest stopped state and retain the previous installation for rollback.
Existing identities, notes, history and wire protocols remain compatible.

Use current Hew with its matching optimized runtime and standard library,
built through `make hew` and selected through `build/bin`. The normal Willow
CI command remains `make -C hosts/willow -j2 check`.

## Willow 0.0.5

Build the host, Base, broker and dashboard from `hosts/willow/`. Production Hew
modules are under `src/`, native bridges under `native/`, administration and
migration tools under `tools/`, and the setup guides under `docs/`.

The host retains complete and split RF frames received during modem negotiation,
including reconnect and final presence announcements. `tools/willow.py check`
uses the same offline settings validation as startup. Unsupported snapshot file
types, including FIFOs, fail promptly.

The dashboard exports native host metrics at `/metrics`. Observer delivery
checkpoints retain queued work across controlled restarts, with batched saves
and bounded expiry. Existing Linux user services can use the
[source-build updater](../tools/INSTALL_WILLOW.md), which copies the latest
stopped state and retains the previous installation for rollback.

Run `make -C hosts/willow -j2 check` with the current Hew compiler and matching
libraries. See [setup, prerequisites and the compatible toolchain](../hosts/willow/README.md).
Keep one running owner per identity and build at the final installation path.
Shared-modem stalls can briefly return HTTP503 while connections recover.

## Willow 0.0.4

Use the shared administration tool with `--willow-socket` to install, export
and roll back Lua sources or Wasm packages through Willow's private owner
socket. Named Lua updates retain bundled commands and unrelated sources;
hash-conditional commits reject a stale source set. Wasm remains independently
installed alongside Lua. Event grants are explicit owner settings and are
not enabled by installing a script. See the
[Willow service controls](../hosts/willow/docs/SERVICE.md).

## Aspen 0.1.12 and Willow 0.0.3

The native bot retains a bounded cache of verified signed contacts and its own
learned routes across restarts. Dirty state is checkpointed every five minutes;
unchanged state causes no writes. Contact observations and known paths are
retained for seven days; direct-send freshness remains ten minutes.
Restored entries wait for trusted UTC
before use and are never counted as new RF reception. Configured repeater route
seeds remain separate from learned routes.

Aspen's companion also coalesces signed advert blobs and contact metadata on a
five-minute deadline. Reads can use pending adverts immediately. Failed saves
retain pending work, and the stored advert format remains readable after an
application rollback.

Controlled Aspen restarts and application updates flush pending native state.
`bot contacts` reports checkpoint state; `bot contacts flush` saves pending
learned observations. A route command can wait up to 30 seconds for the shared
radio opportunity, then has ten seconds for the probe. Cancellation and access
changes are checked before transmission. Rolling SNR/RSSI samples remain in
PSRAM and restart empty; VictoriaMetrics retains their exported history.

Use the [application maintenance command](../tools/hardware/ESP32_MAINTENANCE.md)
for a signed application-only update with an authenticated backup and retained
state checks. Its receipt lets an interrupted operation resume status checks
without repeating upload or restart.

Willow builds with the current Hew compiler and matching libraries on `PATH`.
Frozen-state rebinding validates a private staged copy before publishing the
new state directory. A clean parallel build prepares the native worker's radio
objects before compiling the worker. Follow the
[upgrade and latest-state rollback procedure](../hosts/willow/docs/MIGRATION.md#upgrade-a-frozen-willow-state).

## Aspen 0.1.11

Aspen's configurable image includes native shared-room frontends, saved Lua/Wasm
packet programs and HTTPS. Bracketed companion mentions use the saved bot aliases.
Subsystem syslog and telemetry retain their saved configuration.

KISS frame/wire buffers, observer settings/JSON and companion history/output
queues use PSRAM without internal-memory fallback. Queue capacities and protocol
contracts are unchanged; radio/DMA buffers, locks and task stacks remain internal.
Matched full-image builds reclaim 47,480 internal linker RAM bytes from the
four latest buffer moves. Application-only updates retain identities, settings,
installed programs and data; first installation still needs private setup.

## Aspen 0.1.10, Birch 0.1.1 and Willow 0.0.2

Aspen preserves the effective WiFi credential pair when changing only the SSID
or password, including literal and hex commands. USB setup accepts both
supported narrow-bandwidth integer spellings and the first-install guide
starts from the downloaded ZIP and its included source and boot files.

Birch's `-check` reads required password environments and checks the configured
native worker before a radio connection or state creation. Relative state
paths resolve from the configuration file, independent of the current working
directory. The browser command selector matches the native contacts,
membership, access and thread commands. Standard user-service backups include
the configured native worker and state; downloaded hosts have a separate
stopped, versioned-prefix upgrade and rollback route.

Willow source archives carry their release commit and epoch, so the native
worker builds outside a Git checkout and does not adopt an enclosing
repository's revision. Build and retain the extracted tree at its final path.

## Aspen 0.1.9

Management help groups role, device and owner/source controls into requested
pages, including channels, threads, remote repeaters and distributed rooms.
`roles list` reports applied and saved selections without changing them.
The operator client adds `source` and `data` command groups while retaining
its existing flat names.

`wifi ssid TEXT` and `wifi password TEXT` now save literal printable text.
Scripts sending encoded bytes must use `wifi ssid hex HEX` and
`wifi password hex HEX`; the combined legacy hex form remains supported.
Credentials still require encrypted RF, and saving does not reconnect until
`wifi apply`. Application-only updates retain identities, settings and source.

## Aspen 0.1.1

This version adds saved Management/bot ACL entries, autonomous repeater-route
recovery, named Lua file replacement, dashboard contact/activity reporting and
optional UDP syslog through the existing diagnostics worker. Source bundles
include the release builder's checked dependency archives and relink inputs.
Birch's product version is unchanged.

Use `help syslog` to configure logging and `stats system`, `stats memory`,
`stats psram` and `stats vm` to inspect runtime headroom. App-only updates
retain identities, settings and installed programs. Initial bootloader,
partition or filesystem writes are a separate first-install operation; back
up the node before using them.

## Aspen 0.1.8

After restart, Aspen's bot can recover a DM caller's identity and name from the
running on-device companion's saved signed advert. `bot contacts` reports
recovered contacts and rejected lookups. Full keys and advert signatures are
rechecked within the existing contact budget; no new persistent cache is
written. Missing or stopped companions and missing/invalid adverts require
a new advert from the caller.

Recovered contact knowledge does not supply RF measurements, bot-specific
routes or administrator access. `!neighbors` continues to report received
adverts. See [contact recovery](../firmware/esp32/MAST_ADMIN.md#recover-a-bots-known-contacts-after-restart).
Application-only updates retain identities, settings and installed programs.

## Aspen 0.1.7

A bot can join eight channels simultaneously, including one Public channel,
alongside private DMs. `bot membership`, `bot access` and `bot thread` save and
apply channel selection, bare/addressed execution and replies, and read/write
restrictions. Public starts with commands denied. Existing native owner/shared
grants remain required.

Lua and Wasm can use named storage threads under the same full native caller,
channel or bot identity without multiplying the existing key or timer quotas.
Default storage is unchanged; export default and thread families separately.
See [native channel policy and threads](../firmware/runtime/BOT_RUNTIME.md#channels-and-native-command-policy)
and [single-file Lua replacement and manual restore](../firmware/runtime/REMOTE_REPEATERS.md#replace-or-compose-lua-sources).

Shared-room frontends leave a radio turnaround window before history delivery.
The Worker owns the room identity; frontends deliver live messages and offline
catch-up through their companion-radio connections. Public applications still
need private frontend configuration to connect.
Application-only updates retain identities, settings and installed programs.
Older firmware rejects expanded policy and thread records; make a private
preservation backup before a firmware downgrade.

## Aspen 0.1.5

Use `bot repeaters storage` to inspect free NVS entries and the space required
to save the monitor policy. Failed policy updates report the affected storage
operation; `bot repeaters config ALIAS` shows a target's public key and configured
frequency without sending RF. Management `ver` and bot hop replies use shorter
operator-readable output; ready-to-copy path commands retain their wire format.
Management replay timestamps and WiFi/ACL settings move from bulky NVS records
to verified file pairs and small references, preserving all ten replay slots,
credentials, grants and the existing Lua storage reserve.
See the [administration downgrade warning](../firmware/esp32/MAST_ADMIN.md#native-rf-and-web-access)
before installing an older application on an initialized radio.

The source includes a Worker-owned native room codec and an optional direct
WSS radio frontend. Public images contain no room credentials or configured
aliases; private setup is required before a frontend can connect or advertise.
See
[native shared rooms](../services/shared-room/NATIVE.md).
Application-only updates retain identities, settings and installed programs.

## Aspen 0.1.4

Use the host observer reflector to forward one local radio feed to configured
MQTT services through durable per-destination queues and Aspen-signed tokens.
The source bundle also includes the shared-room Worker, native reference codec
and bounded JSON parser. Direct on-device Aspen WSS/runtime wiring is not yet
available. The application retains the saved setup, identities, settings and
programs from 0.1.3; update only the application slot.
See [observer reflection](../firmware/esp32/OBSERVER.md) and
[shared-room setup](../services/shared-room/README.md).

## Aspen 0.1.3

This version adds saved observer MQTT settings and a non-destructive route from
a configured private application to a generic application. A host reflector can
request broker-specific observer tokens without exporting the radio's private
identity. Backup RF reads fill the native admin reply budget using Base64 while
retaining the legacy hex command. Ordinary Lua event errors remain visible but
no longer block confirmation of an otherwise ready application boot.
See [runtime setup migration](../firmware/esp32/PUBLIC_SETUP.md#move-an-initialized-private-build-to-a-generic-application),
[MQTT settings](../firmware/esp32/OBSERVER.md) and
[node backups](../NODE_BACKUP.md).

## Aspen 0.1.2

This version adds device-generated compressed, operator-encrypted whole-node
backups. Aspen downloads over authenticated WiFi or paced Management RF;
current Birch host and Pine Lua source builds use the same archive and operator
tool. Saved snapshots and interrupted downloads can resume after a restart.
See the [node backup guide](../NODE_BACKUP.md) for commands, privacy, storage
limits and the distinction from scoped bot-data restoration.
