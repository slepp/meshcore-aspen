# Update an ESP MeshCore radio over WiFi

Aspen and Cedar can receive a signed **application-only** update on their
existing `/admin` connection. The radio writes its inactive OTA application
partition, checks the complete image, then selects it for the next boot.
Uploading does not reboot the radio. NVS, SPIFFS, role identities, stored bot
source, WiFi settings, bootloader and partition table are not uploaded or erased.
Keep a verified retained-state backup before changing firmware.
Schedule a low-traffic maintenance window: flash writes can delay radio dispatch
even though the upload runs outside the dispatch task.
The retained fleet layout has app0 at `0x10000` and app1 at `0x340000`, each
`0x330000` bytes. The updater reads the actual inactive partition size and refuses
an image that does not fit. Do not repartition as an update workaround:
partition-table changes can erase retained NVS/SPIFFS data and identities.

The first installation of this updater needs the existing USB application
flash path. Subsequent compatible applications can be uploaded without a cable.
Use the same provisioning mode, operator public key and
`ONCHIP_UPDATE_TARGET` as the installed application. Do not upload a provisioning
image or an image compiled with different operator/configuration defaults.
For a public Aspen application, complete [USB setup](PUBLIC_SETUP.md) first;
its private setup record supplies the update verification authority.
To change an initialized private application to the generic build, first
[save its runtime setup](PUBLIC_SETUP.md#move-an-initialized-private-build-to-a-generic-application).
This migration adds a setup record without replacing SPIFFS and keeps the same
verification authority.

## Prepare and upload

The application must be an ESP32S3 image containing the matching embedded field
target (`xiao-esp32s3` by default). The operator seed is the existing 32-byte
Ed25519 management signing seed; it stays on the operator's machine in a private
file outside the repository. A companion identity key is not an update key.

```sh
python3 firmware/esp32/esp_update_manifest.py \
  --image firmware.bin --target xiao-esp32s3 \
  --key-file "$OPERATOR_SEED_FILE" > update.json
```

Connect from the trusted management network. HTTP is not encrypted: do not send
the management password/session over an untrusted network. The signed image
protects firmware authenticity and integrity, not the HTTP login or traffic.
Use a private shell and avoid tracing commands or saving session tokens in logs.

```sh
RADIO=http://MAST_IP
read -r -s -p 'Management password: ' PASSWORD; printf '\n'
SESSION=$(printf '%s' "$PASSWORD" | curl --fail --silent --show-error \
  --data-binary @- "$RADIO/admin/login")
unset PASSWORD

curl --fail --silent --show-error \
  -H "X-Mast-Session: $SESSION" \
  -H 'Content-Type: application/octet-stream' \
  -H "X-Mast-Target: $(jq -r .target update.json)" \
  -H "X-Mast-SHA256: $(jq -r .sha256 update.json)" \
  -H "X-Mast-Signature: $(jq -r .signature update.json)" \
  --data-binary @firmware.bin "$RADIO/admin/update"
```

Expect `"state":"verified"` and `"reboot_ready":true`. A truncated transfer,
incorrect target/chip, oversized image, signature failure, image validation
failure or SHA256 mismatch leaves the current boot selection unchanged. An
upload error never requests a reboot. If the connection closes before the
response arrives, inspect status instead of uploading again:

```sh
curl --fail --silent --show-error -H "X-Mast-Session: $SESSION" \
  "$RADIO/admin/update"
```

Only after checking the verified result:

```sh
curl --fail --silent --show-error -X POST \
  -H "X-Mast-Session: $SESSION" "$RADIO/admin/update/reboot"
unset SESSION
```

The radio waits at least 1.5 seconds after sending that response before rebooting.
Log in again afterward, check status, then check role identities, saved source,
policy, contacts/channels and useful RF reception against the retained baseline.
Require `running_sha256` to equal the manifest's `sha256`, as well as a healthy
boot, before treating an uncertain reboot as completed. `running_sha256` hashes
the complete raw application image, including its appended hash, rather than
the ELF hash or the inner image hash. An empty value with `running_hash_error`
does not confirm the update; retry status and keep the outcome uncertain.
`boot_health` becomes `healthy` after 30 seconds of progressing radio/role dispatch
and a usable WiFi HTTP service (or a WiFi station disabled by its saved startup
setting).
Every selected Lua and Wasm deployment must also finish verified live activation.
A ready bundled VM does not substitute for a retained program that cannot be
opened, read, verified or initialized.
After a selected program initializes, ordinary command or scheduled-event errors
remain in the bot's diagnostics but do not restart the boot health window.
Source activation failures and invalid adaptive-admission policy still block
confirmation.

## Boot health and recovery

**Automatic rollback requires a rollback-enabled installed bootloader.** Arduino
2.0.17's ESP32S3 SDK enables rollback, but that does not identify the bootloader
already installed on a radio. Compare the retained bootloader backup against the
bootloader for the exact build before relying on automatic rollback. Application
uploads cannot install or change a bootloader. Without that verification, treat
USB recovery as required: no automatic-rollback promise is made.

`rollback_supported:true` means this application has observed its running slot
in IDF's `PENDING_VERIFY` state; `false` means rollback has not been demonstrated
on this boot. Arduino's `verifyRollbackLater()` hook prevents the framework from
confirming the image before application startup. The application confirms it
only after the health window. A watchdog reset or subsequent reboot before
confirmation makes a rollback-enabled bootloader choose the previous valid slot.
An unavailable AP or NTP server alone does not trigger repeated reboots.
A brownout, power cycle or deliberate role/reboot operation before
`boot_health:healthy` can revert an unconfirmed image. Check `running_sha256` and
`running_size` after any early restart. This window lasts until confirmation,
not a fixed five minutes; a normally healthy connected node confirms after
its 30-second health window.
If radio/roles or a connected WiFi station's HTTP service remain continuously
unhealthy for five minutes while an image is
unconfirmed and IDF finds a valid previous slot, the node attempts rollback once.
An AP outage alone does not run this deadline. Without a usable rollback slot,
the node stays in recovery for operator intervention.
If IDF returns an error from a rollback attempt, status reports USB recovery is
required; the node does not follow that failure with a normal restart.

For a selected-source startup fault, inspect `source status` and
`source wasm status` through native management. The bot dashboard reports
`source-recovery`; guest commands, including bundled handlers and overrides,
remain blocked. File/metadata failures retry three times with bounded backoff.
After repairing or restoring the affected file, use `source retry` or
`source wasm retry`. Journal failures also accept an explicit retry after the
retained journal is restored. Retry does not change the selected generation,
hash or stored program. A new uninterrupted 30-second health window starts
only after all selected deployments activate. A selected program with the bot
disabled cannot satisfy this activation requirement; enable it deliberately
before relying on OTA health confirmation.
An application with Wasm compiled out also remains unready if a Wasm selection
or unreadable Wasm journal is retained; restore a Wasm-enabled application rather
than silently dropping that program.

Do not remove a deployment to hide an unexplained startup fault. `source remove`
explicitly selects bundled Lua; `source wasm remove` explicitly removes Wasm
handlers. These operations do not erase caller data but do change the selected
program. A rejected later install/rollback retains the valid running program;
if selection was durably committed but live activation fails, the prior program
can still run, while OTA health remains unready until the selection activates.

Radio initialization and WiFi station initialization retry five times with
bounded backoff. A persistent startup/identity/storage failure keeps the node in
recovery instead of formatting storage, replacing an invalid identity, or
rebooting repeatedly. When its saved WiFi connection is available, `/admin`
login, update status and signed application upload remain available; CLI commands
are disabled. USB diagnostics identify the startup failure. A firmware upload
does not repair corrupt persistent configuration: restore a verified retained
backup or perform the documented role-specific recovery deliberately.
If status reports `rollback_ready:true`, an operator can explicitly return an
unconfirmed image to its previous valid slot:

```sh
curl --fail --silent --show-error -X POST \
  -H "X-Mast-Session: $SESSION" -H 'X-Mast-Rollback: previous' \
  "$RADIO/admin/update/reboot"
```

This uses IDF's rollback API after the response is delivered. It is unavailable
after health confirmation or when IDF cannot find a valid previous slot.

The dispatch task watchdog is armed before hardware/storage/role startup and has
a 60-second timeout; bounded initialization retries feed it between attempts.
Initialization also feeds it between board, radio, identity, modem and role
startup stages. Combined field firmware never automatically formats SPIFFS.
Flash streaming runs on the
HTTP worker, outside radio dispatch, with a 4 KiB transfer buffer, a ten-second
no-data deadline and a three-minute overall deadline.

USB application helpers `esp32_device.py` and `esp32_update.py` now read the live
partition table and both OTA-selection records before choosing an application
address. They refuse unconfirmed/corrupt selection rather than silently writing
`0x10000`. Private deployment scripts must use the same `tools.hardware.esp32_slot.live_app_slot`
helper or an equivalent IDF-aware procedure after the first WiFi update.
`make field-rebuild` also refreshes dispatch/HTTP wiring while retaining the
prepared role wrappers and build-clock header.

## API and checks

All three routes require `X-Mast-Session` from `/admin/login` and the existing
management Host/Origin checks:

| Route | Result |
| --- | --- |
| `GET /admin/update` | JSON status; no flash or boot-state mutation |
| `POST /admin/update` | Raw image, Content-Length, signed metadata; 200 only after verification and boot selection |
| `POST /admin/update/reboot` | Empty body; 409 unless an image is verified; schedules reboot after response delivery |

Signature input is UTF-8/ASCII with exactly these newline-terminated lines:

```text
meshcore-esp-update-v1
esp32s3
<ONCHIP_UPDATE_TARGET>
<unsigned decimal Content-Length>
<64 lowercase hexadecimal SHA256 characters>
```

The signature is 64 bytes encoded as 128 lowercase hexadecimal characters in
`X-Mast-Signature`. Target and digest are `X-Mast-Target` and `X-Mast-SHA256`.
The complete raw body is hashed; IDF validates the actual image/chip/revision and
the embedded target must match. Signed older releases may be installed: this is
not an anti-downgrade policy.

Status fields: `state` (`idle`, `receiving`, `failed`, `verified`), `received`,
`size`, `error`, `target`, `boot_health`, `recovery`, `reboot_ready` and
`rollback_supported` and `rollback_ready`. The existing HTTP worker handles one request at a time;
use client-side upload progress during transfer and server status afterward.
Verified uploads reject further uploads and web CLI commands until reboot.
`sha256` identifies the current authorized transfer (empty before one is
accepted); readback after a lost upload reply must match the intended manifest.
`running_sha256` identifies the actual running application after reboot.
`running_size` is that exact raw image's byte count (zero until a successful
running-image hash read).
`running_hash_error` names a read/verification failure. Running-image hashing is
performed on the HTTP worker and cached in RAM; no update metadata is written to
NVS or SPIFFS. Uploads with bytes beyond IDF's complete image length are rejected.

```sh
make -C firmware/esp32 esp-field-update-test
```

These native checks exercise signed metadata, partial transfers, bad chip/image,
oversize, bad hash/signature, busy handling, boot-switch/reboot failure gating,
health confirmation and OTA-selection-aware USB recovery. They do not replace a
retained-state WiFi upload/reboot acceptance check on each deployed radio.

Run the selected-source startup and update checks without a radio:

```sh
make -C firmware/esp32 lifecycle-test \
  BUILD="$PWD/.tmp/onchip-source-boot-health" \
  CONFIG="$PWD/firmware/esp32/platformio.ini.example" \
  BOT_INTEGRATION=1 ADMIN_INTEGRATION=1 \
  TEST_FLAGS='-DMESHCORE_MAST_ADMIN=1 -DMESHCORE_MAST_WEB_TEST=1 -DONCHIP_SOURCE_BOOT_HEALTH_TEST=1'
```

This checks retained Lua/Wasm file and journal failures, bounded and manual
retry, explicit removal, disabled-bot health, valid restarts and retained live
handlers after a rejected install or rollback. For a Lua-only build add
`ONCHIP_BOT_WASM=0`. For its single-session lifecycle, also append
`-DONCHIP_BOT_COMPACT_PROFILE=1 -DONCHIP_BOT_SINGLE_SESSION=1` to `TEST_FLAGS`.

For the reserved radio, retain a baseline of identities, saved settings, bot
source/data and the current application hash, then check:

1. Upload and confirm one compatible image, then upload and confirm a second
   image back to the other application slot. Match the manifest's full-file hash
   and byte count after each reboot.
2. Reject an image with an invalid signature or wrong target; the running image
   and selected boot partition must remain unchanged.
3. With a verified retained backup, a known-good previous slot and USB recovery
   available, exercise an intentionally unhealthy application. A supported
   pending image returns to the previous application within the five-minute
   unhealthy-role deadline. Recheck retained identities, settings and data.
4. With app1 active and confirmed, check that the USB application helper chooses
   app1 rather than writing app0.

An app-level rollback rewrites boot selection; that result alone does not prove
the bootloader handles a panic or power loss. Verify the installed bootloader
separately, or observe a bootloader-driven rollback after a reset while pending.
If an unexpected restart runs the baseline hash instead of the uploaded hash,
the requested release is not running. The UI must retain its uncertain-outcome
warning rather than treating a healthy older application as successful.

BLE firmware updates belong to the nRF/Pine guide. LoRa firmware transfer is
deferred.
