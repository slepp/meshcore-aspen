# Product versions and candidate builds

Aspen and Birch each use an independent Semantic Version. A fix to one product
does not require a release of the other. MeshCore's version identifies the
upstream base and is recorded separately.

| Product | Candidate tag | Stable tag | Next product fix |
| --- | --- | --- | --- |
| Aspen | `aspen-v0.1.1-rc.1` | `aspen-v0.1.1` | `aspen-v0.1.2` |
| Birch | `birch-v0.1.0-rc.1` | `birch-v0.1.0` | `birch-v0.1.1` |

Show users **Aspen 0.1.1 · based on MeshCore 1.17.1**. Release candidates
use `-rc.2`, `-rc.3`, and so on. Use SemVer's usual patch/minor/major meaning;
while below 1.0, a minor release can change a supported API. An upstream update
is a product release too: choose the product bump according to its effect on
users, then record the new upstream tag and full commit. Never move a release
tag to another commit.

[`products.json`](products.json) is the version authority. Edit only the product
being released, run `python3 tools/product_versions.py`, then `make release-check`.
The generated firmware header and Go constants share the Birch identity.
Companion device information reports `aspen-0.1.1` or `birch-0.1.0-rc.1`;
the full identity must fit 19 ASCII bytes plus NUL. The dashboard exposes the
full upstream tag and commit. Host `ver` includes product and MeshCore versions.
Keep full source/build information in each candidate manifest.

Pine retains its existing `1.17.1-slp-pine` identity. Willow remains experimental.
This change does not introduce independent host, modem, worker, Pine or Willow
release streams. **Birch is one bundle containing the Go host tools, native bot
worker and the matching ESP32 modem, all built from the same product commit.**

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
  --native-image "$(cat .tmp/debian12-image-id)"
python3 tools/release_candidate.py verify .tmp/candidates/EXTRACTED-CANDIDATE
```

Use `refs/heads/main` only when it resolves to the selected signed source commit.
For a tagged source revision, pass its exact `refs/tags/TAG` instead. The selected
public ref must still resolve to the checkout's HEAD.

The bundle name is `aspen-v0.1.1-xiao-esp32s3-sx1262-SOURCE12.zip`.
Birch adds `-linux-x86_64`. Each has `manifest.json`, checksums, application and
separate initial-install images, source and relink archives, resolved dependency
inventory, exact build profile/hash, toolchain versions/compiler hash, upstream
tag/full SHA and public source full SHA. Hashes identify exact output bytes;
this workflow does not claim byte-for-byte reproducible builds. The manifest
always records an **unqualified candidate**. A successful build is one release
gate, not hardware acceptance.

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

Both products now prepare firmware in a fresh tracked-source tree. The builder
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

**Birch publication blocker:** the old generic UART image does not connect the
documented Go host, and the current WiFi modem compiles station credentials.
`platformio.birch.ini` builds a matching WiFi modem with blank credentials for
build inspection only. It cannot connect a newly installed modem. Qualify a
private provisioning path before distributing an installable Birch RC; never
put station passwords into a public image. No new provisioning behavior is
introduced by this release change.

## RC publication gates

1. Review and merge the version/tooling change. Select a clean, signed public
   source commit and rebuild the candidate at that exact commit.
2. Pass `make check`, firmware identity/staging checks and the relevant public
   provisioning/update tests. Review build logs, resolved dependencies, image
   layout and licensing/relink material. Verify every manifest and checksum.
3. Obtain separately authorized hardware acceptance for the selected board and
   application-update path, including retained identities/settings, companion
   information and compatible host/modem/worker operation. Disposition Birch's
   provisioning blocker before its RC.
4. Obtain publication approval for the named product, tag, exact source commit
   and candidate hashes. Create a signed component tag and draft release from
   that commit; verify GitHub's resolved tag and asset hashes before publishing.

Adopting this pattern does not publish a release or create/move tags. Changing
a candidate to a final version is a source change and requires a fresh build,
since the embedded product identity changes.

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

## Historical artifacts

Keep the website's `8b41d9c` Aspen application identified as
`1.17.1-slp-aspen`. The older draft `v1.17.1-slp-4439ad5` contains images from
`4439ad53548b4ad2610e9c94f0bcfa0e7239c336`. Neither is a build of the new public
history or of `aspen-v0.1.0-rc.1`. Do not rename or re-upload those binaries as a
new product release. Retain their original manifests and links until a separately
approved migration of downloads replaces them with verified new builds.
