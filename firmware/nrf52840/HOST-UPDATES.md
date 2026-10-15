# Host-verified Pine application updates

Use `pine_update.py` to verify a signed application manifest before entering
Pine's retained serial DFU bootloader. It checks the trusted release key,
Pine product and selected hardware, application layout, dependencies, file hash
and inner Legacy DFU package. The tool uploads only the application. Legacy 0.6.1 DFU applications must
end by `0xEA000`; saved InternalFS remains at `0xED000`.

**Signature enforcement is on the host.** The installed Legacy DFU bootloader
checks CRC, not this signature. It has no transfer timeout or automatic rollback.
This is not secure boot or device-enforced signed DFU. Keep USB recovery
accessible; do not erase, replace the bootloader or install a full-device image.

## Update an initialized XIAO Pine

1. Keep a verified, encrypted [node backup](../../NODE_BACKUP.md) containing
   identities, InternalFS settings and useful QSPI source/data. Protect the
   backup and operator seed; never publish them. The release is **update-only**:
   existing Pine identities, settings and initialized storage are prerequisites.
2. Download the matching application ZIP, manifest and `.sig`. Use the public
   key already trusted in `release/pine-update.pub`, not a replacement key
   supplied alongside an unfamiliar download.
3. Select `xiao-nrf52840-wio-sx1262` or `sensecap-solar` explicitly:

   ```sh
   python3 firmware/nrf52840/pine_update.py verify \
     --board xiao-nrf52840-wio-sx1262 \
     --zip pine-xiao-application.zip \
     --manifest pine-xiao.json --signature pine-xiao.json.sig
   ```

   Expect `Signed Pine VERSION application verified`. Signature, board or
   application mismatch stops before any device operation.
4. Read `get application` on the existing Pine and retain its SHA256.
   Set `MESHCORE_OPERATOR_CONFIG` to an owner-only (`0600`) JSON inventory with
   `"format": "meshcore-operator-inventory-v1"` and `"nrf_serial"` set to that
   Pine's exact USB serial. Serial deployment supports that explicitly
   inventoried XIAO, not a board chosen by its name or current port number:

   ```sh
   python3 firmware/nrf52840/pine_update.py flash \
     --board xiao-nrf52840-wio-sx1262 \
     --zip pine-xiao-application.zip \
     --manifest pine-xiao.json --signature pine-xiao.json.sig \
     --port /dev/serial/by-id/YOUR_PINE \
     --current-sha PREVIOUS_RUNNING_APPLICATION_SHA256 \
     --record /private/pine-update.json
   ```

   The tool checks that running hash, reads retained public identities/settings,
   selects the same USB serial in the bootloader and performs one app-only
   upload. It checks the new running hash and retained identity/PHY/BLE/source
   responses after startup. Before handover it saves a new `0600` update record
   containing the signed manifest, exact USB serial and retained public state.
   Installed custom Lua must retain its source selection, generation and hash.
   Bundled handlers must remain selected, but their bytes can change with the
   verified application; they are covered by its application hash.
   Keep that record with the backup until the update is checked; an existing
   record is never overwritten. If omitted, `--record` uses a unique file in
   `.build/` and prints its path. Solar manifests can be verified offline; use that
   board's established app-only recovery path for installation.
5. Read `ver`, the active role's status and `get application`. Check the saved
   source/notes and expected companion or peer connection. An RF transmission
   counter is not another radio's reception.

An uncertain upload or absent application response is **not permission to
replay the update**. Inspect the running application or identified bootloader,
then use the same verified app-only package if recovery is needed. The tool
does not clear storage or substitute a blank profile. Peer mode uses this USB
path; its exclusive peer GATT service does not expose BLE DFU.

## Recover an interrupted serial transfer

First check whether the application is running. If `get application` already
matches the signed manifest, finish the original retained-state checks without
resetting or uploading again:

```sh
python3 firmware/nrf52840/pine_update.py check \
  --board xiao-nrf52840-wio-sx1262 \
  --zip pine-xiao-application.zip \
  --manifest pine-xiao.json --signature pine-xiao.json.sig \
  --port /dev/serial/by-id/YOUR_PINE \
  --record /private/pine-update.json
```

This verifies the signed package and protected record again, then reads the
running application hash and retained identities/settings through the selected
USB port. It does not enter DFU or upload an image.
If Pine remains in serial DFU, select its exact bootloader USB path and
use the protected record from that transfer:

```sh
python3 firmware/nrf52840/pine_update.py recover \
  --board xiao-nrf52840-wio-sx1262 \
  --zip pine-xiao-application.zip \
  --manifest pine-xiao.json --signature pine-xiao.json.sig \
  --port /dev/serial/by-id/YOUR_PINE_BOOTLOADER \
  --record /private/pine-update.json
```

Recovery verifies the package signature again, matches the original board,
image and inventoried USB serial, and requires that device to already be in
DFU. It performs one application upload, then checks the running hash and the
original retained-state responses through the recorded application port.
Custom Lua retains its exact selection, generation and hash; bundled handlers
remain selected and come from the verified application.
It never resets a running application or chooses another attached board.
Keep power stable until those checks complete. A second timeout remains an
uncertain outcome; inspect the device before another explicit recovery attempt.

The record is needed because a stranded bootloader cannot report the prior
application or settings. It is not a node backup and cannot restore erased
state. Transfers made by other tools without this record keep their established
bootloader recovery procedure. Do not manufacture a record or erase storage
to bypass a mismatch.

## Build and sign an application

Use a clean, committed public source revision and the normal supported-profile
build. For each board, copy the resulting `firmware.zip` to its release filename
and create a factual manifest:

```sh
python3 firmware/nrf52840/pine_update.py manifest \
  --board xiao-nrf52840-wio-sx1262 --zip pine-xiao-application.zip \
  --source "$(git rev-parse HEAD)" --version VERSION --output pine-xiao.json
ssh-keygen -Y sign -n pine-update -f /private/release-signing-key pine-xiao.json
python3 firmware/nrf52840/pine_update.py verify \
  --board xiao-nrf52840-wio-sx1262 --zip pine-xiao-application.zip \
  --manifest pine-xiao.json --signature pine-xiao.json.sig
```

The signing key must match the pinned public release key. Publish the ZIP,
manifest and signature together, with source and supported setup documentation.
Keep private credentials, backups, helpers and test logs out of release assets.
Pine tags use `pine-vVERSION`; Pine GitHub releases use `--latest=false` so the
Aspen firmware remains the repository's latest release.
