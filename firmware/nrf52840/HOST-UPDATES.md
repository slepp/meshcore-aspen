# Host-verified Pine application updates

Use `pine_update.py` to verify a signed application manifest before entering
Pine's retained serial DFU bootloader. It checks the trusted release key,
Pine product and selected hardware, application layout, dependencies, file hash
and inner Legacy DFU package. The tool uploads only the application.

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
     --current-sha PREVIOUS_RUNNING_APPLICATION_SHA256
   ```

   The tool checks that running hash, reads retained public identities/settings,
   selects the same USB serial in the bootloader and performs one app-only
   upload. It checks the new running hash and retained identity/PHY/BLE/source
   responses after startup. Solar manifests can be verified offline; use that
   board's established app-only recovery path for installation.
5. Read `ver`, the active role's status and `get application`. Check the saved
   source/notes and expected companion or peer connection. An RF transmission
   counter is not another radio's reception.

An uncertain upload or absent application response is **not permission to
replay the update**. Inspect the running application or identified bootloader,
then use the same verified app-only package if recovery is needed. The tool
does not clear storage or substitute a blank profile. Peer mode uses this USB
path; its exclusive peer GATT service does not expose BLE DFU.

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
