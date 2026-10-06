# Make a release

Release source and firmware together from a tagged commit in
[`slepp/meshcore-aspen`](https://github.com/slepp/meshcore-aspen).
Firmware versions identify the MeshCore base and profile, such as
`1.17.1-slp-pine` for Pine. For Aspen and Birch, use the independent product
versions, supported modem contract and candidate workflow in
[Product versions and candidate builds](release/README.md);
[`release/products.json`](release/products.json) is their version authority.
Package manifests record the exact source commit,
dependencies, board layout and image hashes.

## Check the source

From a clean checkout, run:

```sh
make check
```

Run the firmware, Lua/Wasm and protocol checks for the components being
released using their build guides. Review the files and history included in
the release with a secret scanner. Classify findings against the test
fixtures and remove any live credentials or private deployment records.
Keep local configurations, signing seeds, backups and build outputs outside
the source archive.

Retain the copyright notices and licenses listed in
[THIRD_PARTY.md](THIRD_PARTY.md).

## Build firmware

Use the selected commit, pinned dependencies and the profile's documented
toolchain. Give each profile a separate output directory. Set
`SOURCE_DATE_EPOCH` to the source commit timestamp for builds that support it.

| Profile | Package | Build and update instructions |
| --- | --- | --- |
| Aspen | ESP32-S3 application and separate first-install files | [Product candidate build](release/README.md), [public setup](firmware/esp32/PUBLIC_SETUP.md), [WiFi updates](firmware/esp32/ESP_FIELD_UPDATES.md) |
| Birch | Go host tools, native bot worker and matching WiFi shared modem from one commit | [Product candidate build and provisioning gate](release/README.md), [modem and Go host setup](HOST_GUIDE.md) |
| Pine | nRF52840 application UF2, BIN and Legacy BLE DFU ZIP for initialized nodes | [Production Lua](firmware/nrf52840/PRODUCTION-LUA.md), [BLE updates](firmware/nrf52840/BLE-FIELD-UPDATE.md) |
| Willow | Experimental Linux Hew host sources, including native Base, broker and dashboard | [Build and service setup](experiments/hew-roles/README.md) |

Build generic images with per-node provisioning. Operators supply their own
identities, passwords, network credentials and signing keys. Keep
first-install boot files separate from application-update files.

Each ZIP should contain installation and update instructions, SHA-256
checksums, a board/layout manifest, dependency notices and the required
library relink materials. Check the extracted tools, image address ranges,
preserved storage regions and both baseline and replacement-library relinks.
State the supported board and update prerequisites in the installation guide.

Willow is source-built at its final installation path. Its compiled worker
binding requires the unchanged source and build inputs there; copied binaries
are not a relocatable host package. Run the selected Willow checks from its
guide and state any remaining qualification gaps in the release description.

## Create the release

For Aspen and Birch, follow [the product release procedure](release/README.md),
including Birch's modem provisioning requirement. Obtain the operator's
publication approval before creating the release. Keep test logs internal;
publish the package, factual build manifest, checksums and installation guide.

Tag the selected source commit and push the tag. Aspen and Birch require signed
product tags as specified in their candidate guide. Create a GitHub draft
release for that tag, then attach the packages, manifests and `SHA256SUMS`.
Link each package to its source revision and dependency licenses.

Review the draft's installation instructions and download links. Publish the
tagged source with the release so users can inspect and rebuild it. Enable
GitHub private vulnerability reporting and check the contact route in
[SECURITY.md](SECURITY.md).

Use a new versioned URL for each release. Keep earlier packages and their
checksums available. Update the [project download page](https://ve6slp.ca/projects/meshcore/#downloads)
with the version, supported boards and installation guides.

Download the published files and compare their sizes and SHA-256 hashes with
the release manifest.
