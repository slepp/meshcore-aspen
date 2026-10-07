# Product versions and candidate builds

Aspen and Birch each use an independent Semantic Version. A fix to one product
does not require a release of the other. MeshCore's version identifies the
upstream base and is recorded separately.

| Product | Candidate tag | Stable tag | Next product fix |
| --- | --- | --- | --- |
| Aspen | `aspen-v0.1.5-rc.3` | `aspen-v0.1.8` | `aspen-v0.1.9` |
| Birch | `birch-v0.1.0-rc.1` | `birch-v0.1.0` | `birch-v0.1.1` |

Show users **Aspen 0.1.8 · based on MeshCore 1.17.1**. Release candidates
use `-rc.2`, `-rc.3`, and so on. Use SemVer's usual patch/minor/major meaning;
while below 1.0, a minor release can change a supported API. An upstream update
is a product release too: choose the product bump according to its effect on
users, then record the new upstream tag and full commit. Never move a release
tag to another commit.

[`products.json`](products.json) is the version authority. Edit only the product
being released, run `python3 tools/product_versions.py`, then `make release-check`.
The generated firmware header and Go constants share the Birch identity.
Companion device information reports `aspen-0.1.8` or `birch-0.1.0-rc.1`;
the full identity must fit 19 ASCII bytes plus NUL. The dashboard exposes the
full upstream tag and commit. Host `ver` includes product and MeshCore versions.
Keep full source/build information in each candidate manifest.

Pine retains its existing `1.17.1-slp-pine` identity. Willow remains experimental.
This change does not introduce independent host, modem, worker, Pine or Willow
release streams. **Birch 0.1.0 RC1 is a Linux x86_64 host-tools and native-worker
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
Aspen supports the documented XIAO ESP32-S3R8/PSRAM plus Wio SX1262 setup.
Birch's initial host target is Linux x86_64 with its matching WiFi/TCP modem.
Versioning alone does not qualify a different board, protocol or upstream base.

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

The Aspen bundle name is `aspen-v0.1.8-xiao-esp32s3-sx1262-SOURCE12.zip`.
Its files include `manifest.json`, checksums, application and separate
initial-install images, source and relink archives, resolved dependency
inventory, exact build profile/hash, toolchain versions/compiler hash, upstream
tag/full SHA and public source full SHA. Hashes identify the packaged files.
The host-only Birch name is `birch-v0.1.0-rc.1-linux-x86_64-SOURCE12.zip`.
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
RC requires an existing configured compatible modem, or an operator source
build with private provisioning. Never put station passwords into a public
image. Installing the host does not flash or provision the radio.

## Publish a release

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

## Historical artifacts

Keep the website's `8b41d9c` Aspen application identified as
`1.17.1-slp-aspen`. The older draft `v1.17.1-slp-4439ad5` contains images from
`4439ad53548b4ad2610e9c94f0bcfa0e7239c336`. Neither is a build of the new public
history or of `aspen-v0.1.0-rc.1`. Do not rename or re-upload those binaries as a
new product release. Retain their original manifests and links until a separately
approved migration of downloads replaces them with verified new builds.
