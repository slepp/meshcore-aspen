# Product downloads and releases

Choose the product for the device or service you run. Each product has its own
version and release tag; an update to one does not require updating the others.

| Product | Source version | Download and setup |
| --- | --- | --- |
| Aspen | `0.1.14` | ESP32 standalone node: [USB setup and application updates](../firmware/esp32/PUBLIC_SETUP.md) |
| Birch | `0.1.3` | Linux x86_64 host and native worker: [host installation](../HOST_GUIDE.md#install-the-birch-host-download) |
| Pine | `0.1.3` | XIAO nRF52840 application update: [field setup](../firmware/nrf52840/FIELD-SETUP.md) |
| Willow | `0.0.11` | Hew host services: [installation and retained-state upgrades](../hosts/willow/README.md) |

Get packages from the [GitHub releases](https://github.com/slepp/meshcore-aspen/releases).
Source versions can be ahead of published downloads. Product tags use
`aspen-vVERSION`, `birch-vVERSION`, `pine-vVERSION` and `willow-vVERSION`.
Keep each release attached to its original source commit.

## Select the right download

**Aspen** targets Seeed XIAO ESP32-S3R8 + Wio SX1262. The bundle separates the
application from bootloader and partition files. For an existing node, back up
its useful state and use the application update path: it retains identities,
settings and programs. Use the [initial USB setup](../firmware/esp32/PUBLIC_SETUP.md)
only when commissioning a board. Keep private provisioning and node backups
outside release archives. [Heltec V4 R8 and T-Beam Supreme SX1262 profiles](../firmware/esp32/BOARDS.md)
are also available for source builds; read each board's wiring and installation
limits before selecting a target.

**Birch** is a Linux x86_64 host-tools and matching native-worker bundle for an
already configured queued-v1 WiFi/TCP shared modem. It includes four host
binaries, the installer, example configuration, host guide, source, dependency
notices and native relink material. It does not flash or provision the modem.
The manifest records the required shared libraries and minimum symbol versions;
compare them with the host before installation. The normal release build uses
Debian 12, GCC 12, OpenSSL 3 and cJSON.

**Pine** updates an initialized Seeed XIAO nRF52840 + Wio SX1262 with its retained
bootloader, identities, credentials and QSPI storage. Pine 0.1.3 includes Lua,
Room, packet programs, retained companion contacts/channels and BLE peer mode;
[select one optional role](../firmware/nrf52840/FIELD-SETUP.md#choose-one-optional-role).
The download contains app-only UF2/BIN and an inner Nordic Legacy DFU ZIP,
source references, dependency notices and Arduino LGPL relink material. The
outer download ZIP is not a DFU package. Use the signed application manifest
with the [host-verified updater](../firmware/nrf52840/HOST-UPDATES.md), or follow
[BLE field updates](../firmware/nrf52840/BLE-FIELD-UPDATE.md). Keep stable power
and USB recovery accessible: interrupted BLE transfer can require USB recovery.
Never erase InternalFS or QSPI, replace the bootloader/SoftDevice or flash the
application BIN at address zero to install this update. SenseCAP Solar has a
separate source profile and board-specific update path.

**Willow** runs the host, Base, observer and dashboard services in Hew. Use the
[Willow setup guide](../hosts/willow/README.md) for the supported download,
build prerequisites and service configuration. Retain the existing private
state directory during an upgrade and keep one running owner per identity.
Source builds use the current in-PATH Hew compiler with its matching optimized
runtime and standard library; see the guide's build command.

## Versions and interfaces

[`products.json`](products.json) is the version authority. Update only products
being released, run `python3 tools/product_versions.py`, then `make release-check`.
Each product advances its own version series independently of the MeshCore dependency.
Generated firmware, Go and Willow identities use those versions. Companion
firmware identities fit 19 ASCII bytes plus NUL; for example `aspen-0.1.14` or
`pine-0.1.3`. The dashboard and manifests retain the full upstream and product
source revisions.

The current MeshCore base is `companion-v1.18.0` at
`81dbae592ffad40b4212bc0b37e82b38da08f4bc`. Product versions and protocol
versions are separate:

| Interface | Version | Requirement |
| --- | --- | --- |
| Queued PHY | 1 | Host and modem implement the negotiated queue/ownership contract |
| Native bot worker | 1 | Host and worker share the HELLO/READY and framing contract |
| MeshCore companion | 13 / 14 | Aspen uses upstream protocol 14; host Base and Pine retain protocol 13. Existing fields and 20-byte firmware identity remain compatible. |
| Dashboard JSON | 1 | Existing fields and additive source identity fields |
| Wasm application ABI | 1 | MeshCore application SDK contract |

Use normal SemVer patch/minor/major meanings. Below 1.0, a minor release can
change a supported API. Record upstream changes separately. Changing an RC
version to final changes the embedded identity and requires a new build.

## Build Aspen or Birch downloads

Build from a clean, signed commit already available in the public repository.
The selected public ref must resolve to the checkout's exact HEAD. The packager
uses tracked public profiles and new source/object directories, without private
provisioning files. Use Python 3.13, PlatformIO and the contributor build
dependencies. Birch also requires Linux x86_64 and Docker; its builder fetches
the Go SDK selected by `go.mod` and compiles in the Debian 12 image.

```sh
make release-check
python3 tools/release_candidate.py build --product aspen \
  --public-ref refs/heads/main

# Build Birch's Debian 12 build image.
dockerfile_sha=$(sha256sum release/Dockerfile.debian12 | cut -d' ' -f1)
docker build --iidfile .tmp/debian12-image-id \
  --label "org.meshcore.release.dockerfile-sha256=$dockerfile_sha" \
  -f release/Dockerfile.debian12 release
python3 tools/release_candidate.py build --product birch \
  --public-ref refs/heads/main \
  --native-image "$(cat .tmp/debian12-image-id)" --host-only
python3 tools/release_candidate.py verify .tmp/candidates/EXTRACTED-CANDIDATE
```

For a product tag, use `--public-ref refs/tags/TAG` instead. Aspen packages are
named `aspen-vVERSION-xiao-esp32s3-sx1262-SOURCE12.zip`; host-only Birch packages
are `birch-vVERSION-linux-x86_64-SOURCE12.zip`.

Packages include a factual manifest, hashes, public source, resolved dependency
inventory, notices and relink material. Firmware manifests also record image
layout, board profile and compiler/toolchain details. The packager checks the
pinned Crypto, CayenneLPP, Lua and WAMR inputs, and base64 when used, before
compilation. Inspect a reported dependency mismatch rather than overwriting an
unknown local dependency. Other PlatformIO resolution rules remain recorded in
the manifest; the firmware build is not fully dependency-pinned.

Birch's packaged ELF requirements are inspected against GNU `readelf` and the
Debian 12 baseline. Compilation uses newly extracted source and runs without
external network access after dependency downloads.

The optional combined Birch build without `--host-only` contains a blank-credential
modem for build inspection. It is not an installable modem download. Operators
building a WiFi modem must provision it privately; never publish station passwords.

Pine source builds and application manifest signing use the commands in
[host-verified Pine updates](../firmware/nrf52840/HOST-UPDATES.md#build-and-sign-an-application).
Willow packaging is documented with its [host setup](../hosts/willow/README.md).

## Publish a product release

1. Build from the selected clean, signed public source commit. Check package
   hashes, image layout, installation instructions and licensing/relink files.
2. Test changed behaviour and the relevant install/update path, including
   retained identities, settings and programs. Reuse established results for
   unchanged code. Check the exact Birch worker on its supported Debian 12 host.
3. Create the signed product tag and GitHub release.
   Attach packages, manifests, signatures where used and checksums. Verify
   downloaded asset hashes. Exclude private keys, credentials and node backups
   from assets. Use `--latest=false` for Birch, Pine and Willow so
   Aspen remains the repository's latest firmware download.

## Room terminal client

The terminal client is versioned independently in `clients/room-tui/VERSION`
with tags `room-tui-vVERSION`. From a clean, signed public source commit, use
the Go SDK selected by the client's `go.mod` and Python 3.12 or newer:

```sh
python3 clients/room-tui/package_release.py \
  --public-ref refs/heads/main --out .tmp/room-tui-vVERSION
cd .tmp/room-tui-vVERSION
sha256sum -c SHA256SUMS
```

Each static Linux amd64/arm64 archive includes source, the project license and
dependency notices. Check version/help and changed sign-in or restart behaviour;
execute arm64 on ARM or an emulator as well as checking its ELF architecture.
Publish the signed tag with `--latest=false` and verify downloaded hashes.
Upgrades replace the executable while retaining the private state directory.
