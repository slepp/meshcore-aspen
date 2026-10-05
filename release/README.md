# Product versions and candidate builds

Aspen and Birch each use an independent Semantic Version. A fix to one product
does not require a release of the other. MeshCore's version identifies the
upstream base and is recorded separately.

| Product | Candidate tag | Stable tag | Next product fix |
| --- | --- | --- | --- |
| Aspen | `aspen-v0.1.0-rc.1` | `aspen-v0.1.0` | `aspen-v0.1.1` |
| Birch | `birch-v0.1.0-rc.1` | `birch-v0.1.0` | `birch-v0.1.1` |

Show users **Aspen 0.1.0 RC1 · based on MeshCore 1.17.1**. Subsequent candidates
use `-rc.2`, `-rc.3`, and so on. Use SemVer's usual patch/minor/major meaning;
while below 1.0, a minor release can change a supported API. An upstream update
is a product release too: choose the product bump according to its effect on
users, then record the new upstream tag and full commit. Never move a release
tag to another commit.

[`products.json`](products.json) is the version authority. Edit only the product
being released, run `python3 tools/product_versions.py`, then `make release-check`.
The generated firmware header and Go constants share the Birch identity.
Companion device information reports `aspen-0.1.0-rc.1` or `birch-0.1.0-rc.1`;
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
dependencies. Birch also requires Linux x86_64, Go 1.26.7 or newer, C++17, CMake
and OpenSSL development libraries for the native worker.
ELF inspection uses GNU `readelf` from binutils.

```sh
make release-check
python3 tools/release_candidate.py build --product aspen \
  --public-ref refs/heads/release/product-versioning
# Run on Linux x86_64 for the matching host + modem build:
python3 tools/release_candidate.py build --product birch \
  --public-ref refs/heads/release/product-versioning
python3 tools/release_candidate.py verify .tmp/candidates/EXTRACTED-CANDIDATE
```

The bundle name is `aspen-v0.1.0-rc.1-xiao-esp32s3-sx1262-SOURCE12.zip`.
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
distribution using the exact packaged worker. The current Obelisk toolchain
can produce a worker requiring GLIBC 2.43 (including `sqrtf@GLIBC_2.43`), so its
candidate is not an installable bundle for Debian 12 or Ubuntu 24.04. The Go
tools have separate ABI receipts; their requirements do not qualify the worker.
Building a portable bundle requires an agreed older distribution baseline and
its compiler, CMake, OpenSSL and cJSON development dependencies. Do not relabel
a newer-ABI binary as portable or silently replace its libraries.

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

Adopting this pattern does not publish a release or create/move tags. A final
`aspen-v0.1.0` requires its own acceptance; replacing `-rc.1` is a source change
and requires fresh builds, since the embedded product identity changes.

## Historical artifacts

Keep the website's `8b41d9c` Aspen application identified as
`1.17.1-slp-aspen`. The older draft `v1.17.1-slp-4439ad5` contains images from
`4439ad53548b4ad2610e9c94f0bcfa0e7239c336`. Neither is a build of the new public
history or of `aspen-v0.1.0-rc.1`. Do not rename or re-upload those binaries as a
new product release. Retain their original manifests and links until a separately
approved migration of downloads replaces them with verified new builds.
