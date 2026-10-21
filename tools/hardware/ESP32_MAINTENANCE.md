# Update an ESP32S3 radio application

Use `tools/admin.py firmware` to update an initialized Aspen-compatible ESP32S3
radio over its management connection. It uploads only the application, restarts
once and checks the running application, ready roles, public identities, radio
profile, selected Lua/Wasm programs and bot channel policies. NVS, filesystem,
bootloader and partition table remain in place.

Use a trusted management network. HTTP exposes the administrator password and
session to that network; the application signature does not encrypt HTTP.
Keep USB recovery accessible. Automatic rollback depends on the installed
bootloader; see [ESP field updates](../../firmware/esp32/ESP_FIELD_UPDATES.md).
Use an application built for the same hardware, update target, operator key and
saved setup mode. Obtain the application and its manifest from a trusted release
or your own build before signing it. The tool checks the file's size and SHA256
against that manifest.

Run on a POSIX host such as Linux or macOS; on Windows, use WSL. The command
uses POSIX file locks and directory syncing for its private maintenance receipt.
Run commands from the repository root. Keep the existing administrator password
and 32-byte operator signing seed in private (`0600`) files outside public
source. A companion identity key cannot sign an application update.

1. Save a fresh encrypted [node backup](../../NODE_BACKUP.md):

   ```sh
   umask 077
   python3 tools/admin.py --web "$RADIO_URL" --password-file "$PASSWORD_FILE" \
     backup download /private/radio-before-update.mcb \
     --volatile \
     --key-file "$OPERATOR_SEED_FILE"
   ```

   Download validates the encrypted archive. The maintenance command checks its
   authentication and archive again before connecting to the radio. The backup
   must contain this radio's current identities, configuration and useful data.
2. Read the running application:

   ```sh
   python3 tools/admin.py --web "$RADIO_URL" --password-file "$PASSWORD_FILE" \
     firmware status
   ```

   Keep `running_sha256`. Require an empty `running_hash_error`, `state: idle`
   and `boot_health: healthy` before updating.
3. Install the chosen application, using a new private receipt path:

   ```sh
   python3 tools/admin.py --web "$RADIO_URL" --password-file "$PASSWORD_FILE" \
     firmware update \
     --image /private/build/firmware.bin \
     --build-manifest /private/build/manifest.json \
     --key-file "$OPERATOR_SEED_FILE" \
     --backup /private/radio-before-update.mcb \
     --expected-current-sha256 PREVIOUS_RUNNING_SHA256 \
     --receipt /private/radio-update.json
   ```

   The build manifest must list the application's filename, `bytes` and
   `sha256` in its `files` array, as the Aspen release manifest does. The image
   must contain the selected field target (`xiao-esp32s3` by default; override
   with `--target`). An existing receipt is never overwritten by `update`.

The command records identities and settings, pauses a pinned sampler if requested,
requests `bot contacts flush`, signs and uploads the application, checks its verified hash/size, then requests one
restart. It waits up to 240 seconds for the intended application to become
healthy and compares identities, settings and selected programs. Administrator commands are spaced
600 ms apart. Success ends with `Application healthy` and a private receipt.
Selected custom programs retain their generation, slot, size and hash. Bundled
handlers remain selected and may change with the application.

The command reads the running version with `ver` and refuses an application
whose build manifest names an older version before it uploads anything or
pauses a sampler. Add `--allow-downgrade` for a deliberate downgrade; the
receipt records `candidate_version` and `running_version`. A manifest without
a version identity is not compared.

If storage errors prevent saving native state, an authenticated signed
application update can recover firmware with `--allow-unsaved-state`. Download
and validate a fresh encrypted backup first; this option requires its local
download file to be less than one hour old. Recent unsaved state may be lost
when the radio restarts. The receipt records the exception before upload.
The radio must report `unsaved_state_supported: true`; older firmware cannot
use this option. `allow_unsaved_state` reports the selection and
`state_flush_failed` reports a failed native save. The exception covers only
this application upload, boot selection and its requested restart. Ordinary
updates still require successful saves. Inspect an upload HTTP error and its
private `upload_failure` receipt field before deciding on recovery.

If a known read-only packet sampler must pause during flash writes, add both
`--sampler-slot SLOT` and `--sampler-sha256 EXACT_HASH`. The command verifies its
hash, Wasm runtime, saved/live enable state and budget before pausing it, then
restores and checks its original state. It refuses a different or sealed
program. Omit these options when no sampler needs pausing; do not supply the
hash of an unknown packet program just to disable it. Keep other installed
packet programs enabled unless they are explicitly pinned for this pause.

Defaults compare both packet program slots, all eight bot memberships, every paginated access/thread
rule, aliases, bot policy, role paths, autoadvert and setup status. Add stable,
read-only deployment settings with repeated `--retain-command`, for example
`--retain-command 'get sntp.server'` or
`--retain-command 'telemetry endpoint host'`. Avoid changing counters or runtime
connection state, which can legitimately differ after a restart. These selected
commands run before and after the update; do not pass commands that change
settings or expose credentials.

## Finish an interrupted attempt

A lost upload response does not trigger a second upload. The command reads
update status and restarts only if the intended image is verified. A lost
restart response does not trigger a second restart. The receipt records each
intent before sending it, so a process interruption also leaves the outcome
uncertain rather than authorizing a repeat.

Use the original receipt to finish checking that attempt:

```sh
python3 tools/admin.py --web "$RADIO_URL" --password-file "$PASSWORD_FILE" \
  firmware resume \
  --receipt /private/radio-update.json
```

`resume` never uploads an application. If the original upload was verified and
no restart attempt was recorded, it can request that restart once. Otherwise it
only polls for the intended running image, restores a paused pinned sampler and
checks identities, settings and selected programs. If the upload is incomplete, the baseline changed, the
application has not returned healthy or a setting differs, it stops
with the receipt path. Inspect the connection and current update status before
deciding on a new attempt. Do not manufacture a fresh receipt to replay an
uncertain operation. A receipt that records no upload can restore a paused
sampler, then requires a new update receipt.

A true `sampler_restore_pending` field means the sampler's original enable
state still needs checking; `resume` tries that restoration when the original
or intended application can safely accept commands. A separate `0600` `.lock` file beside the receipt prevents concurrent updates or
resumes from sending the same operation. Leave it in place; the lock releases
when the command exits. Keep receipts and encrypted backups private. They contain node configuration and are not release assets.

## Restart when native state cannot be saved

On firmware that reports `force_restart_supported: true`, `force-restart`
restarts the entire node without saving native state. Recent unsaved contacts,
messages and settings may be lost. Download an encrypted backup first when
storage permits. This command does not upload an application. If a verified
application is waiting for reboot, the restart selects and boots it; otherwise
the running application boots again.

```sh
python3 tools/admin.py --web "$RADIO_URL" --password-file "$PASSWORD_FILE" \
  firmware force-restart \
  --expected-current-sha256 CURRENT_RUNNING_SHA256 \
  --receipt /private/radio-force-restart.json
```

Use a new receipt. The command checks the running hash and firmware support
before sending one restart request. It records the running application, boot identifier, selected verified
application and restart intent first.
It waits for a new boot identifier, then requires the intended application and
ready roles. A healthy response from the old boot is insufficient.

If the reply is lost or the wait expires, use `resume` with that receipt.
Resume only reads status; it never repeats a forced restart. Older firmware
without this endpoint requires an available physical or remote power/reset
control to recover from an unsaveable native-state checkpoint.
