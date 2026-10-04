# Update Pine nearby over Bluetooth

Pine can accept an **application-only Nordic legacy BLE DFU ZIP** through its
existing Adafruit bootloader. No USB cable is needed for the transfer. The
repeater administrator must deliberately arm the running application, and the
phone must authenticate using Pine's saved six-digit BLE PIN.
The running image must already contain this guarded entry feature. Install
that image once through the existing Pine-only USB application DFU deployment
path before depending on no-wire updates; this does not replace the bootloader.

**Important:** the retained bootloader overwrites the application in place.
There is no signed-image trust, rollback or bootloader-session deadline.
The 120-second deadline below bounds *entry from the running application*,
not an ongoing or abandoned bootloader transfer. Keep the phone nearby, power
stable and the verified ZIP available until Pine has restarted. Interrupted
transfers can require another BLE transfer or physical USB recovery. Do not
start an unattended update that must automatically resume mesh operation.

## Prepare and arm

Use the production `nrfmast_fleet_lua` image. Build without uploading:

```sh
make -C firmware/nrf52840 build ENV=nrfmast_fleet_lua \
  LUA_ARCHIVE=/path/in/your/project/lua-5.5.1.tar.gz
python3 firmware/nrf52840/ble_field.py package \
  --zip firmware/nrf52840/.build/upstream/.pio/build/nrfmast_fleet_lua/firmware.zip
```

This checks the manifest, Nordic init packet, CRC, application vector table and
reserved-flash boundary. The production ZIP contains only `firmware.bin`,
`firmware.dat` and `manifest.json`; Nordic legacy manifest `dfu_version` is
`0.5`, device type is `0x52`, and the build requires SoftDevice ID `0x123`.
The BIN starts at `0x27000`; its end must be below `0xED000`.
Neither a raw fullflash BIN nor a Secure DFU ZIP is a substitute.

Provision the BLE PIN through Pine's existing local USB console before taking
it into the field (`set bot.ble.pin SIX_DIGITS`). Do not put the PIN in logs,
scripts or package names. Saved companion BLE can stay off: arming starts
temporary BLE with that PIN without changing the saved setting.

`get bot.ble` reports saved enable state and `pin=configured` or `pin=unset`;
it never reveals the PIN. There is no `get bot.ble.pin` export command.
Retrieve the value privately from the operator's protected provisioning
record. If it is unavailable and no authenticated companion connection remains,
provision a replacement through the local USB console; do not guess it or
dump device settings. `set bot.ble.pin SIX_DIGITS` accepts only 100000–999999
and saves it for the next reboot. RF administration cannot run this setting.
An already paired companion can also save a replacement with the standard
`CMD_SET_DEVICE_PIN` frame (opcode plus four little-endian PIN bytes).
The secured standard device-info response contains the saved PIN at bytes
4–7 as a little-endian integer; keep that field out of diagnostics and logs.
After reboot, bonds are cleared and the client must pair with the saved PIN.
`set bot.ble on` is only needed for persistent companion BLE, not temporary
update arming.

Open **Pine-Relay's authenticated MeshCore repeater administration** through
your companion radio, or Pine's local console:

```text
get update
start ota
start ota confirm
```

`get update` reports declared BLE DFU capability, the bootloader version
captured by the Arduino core (`0xMMmmpp`), and the bootloader timeout limitation.
`start ota` only returns the warning. `start ota confirm` pauses bot work and
source administration, then opens a 120-second DFU-entry window. Repeating
it does not extend the deadline. BLE pairing alone never arms an update.
Without a saved PIN, or in an image built with `NRFMAST_BLE_DFU=0`, entry is
refused. A PIN change must be applied by reboot before arming an already
running BLE interface. Ordinary companion BLE remains available outside the window, but
DFU-control writes are rejected.

## Transfer with Nordic tools

On Android, use Nordic **Device Firmware Update** (or nRF Toolbox's DFU
feature); select the validated ZIP and the explicitly identified Pine device.
Pair with the saved PIN when requested. Use a tool supporting **Legacy DFU
with buttonless application entry**, not only the newer Secure DFU protocol.
nRF Connect can discover/read the service and launch a supported DFU flow;
ordinary GATT file writes are not a firmware transfer.

The application exposes:

| Item | Value |
| --- | --- |
| Legacy DFU service | `00001530-1212-efde-1523-785feabcd123` |
| Control / packet / revision | `1531` / `1532` / `1534` in the same UUID base |
| Application revision characteristic | `0x0001` |
| Bootloader revision (upstream legacy transport) | `0x0008` |
| Application control access | bonded, encrypted, authenticated PIN/MITM connection, armed window |

### Linux with an existing Legacy DFU library

[recrof/nrf_dfu_py](https://github.com/recrof/nrf_dfu_py) is an existing
MeshCore ecosystem tool using Bleak and Nordic Legacy/buttonless DFU. Its
README lists Adafruit/Seeed XIAO compatibility. The installed
`adafruit-nrfutil.py dfu` only provides `genpkg` and `serial`; it is not a
Linux BLE uploader. Secure-DFU-only tools are also incompatible with this
legacy service.

The operator can activate the external tool without changing
Pine's bootloader:

```sh
git clone https://github.com/recrof/nrf_dfu_py firmware/nrf52840/.build/nrf-dfu-py
git -C firmware/nrf52840/.build/nrf-dfu-py checkout --detach \
  409e4b75d55c8d75859022217dfba0db33730f16
```

Use an environment with `bleak` already available, and establish Pine's
PIN/MITM bond through the host's normal BlueZ pairing flow before arming.
No PIN is passed on a command line or distributed with the ZIP. The external
CLI can automatically select a bootloader by service UUID; **do not use that
selection when other nRF boards are present**. The focused wrapper reuses
its `NordicLegacyDFU` library but requires explicit application and bootloader
addresses and refuses revision `0x0001` as a transfer target:

```sh
# After authenticated `start ota confirm`, within its 120-second window:
python3 firmware/nrf52840/ble_dfu.py \
  --zip firmware/nrf52840/.build/upstream/.pio/build/nrfmast_fleet_lua/firmware.zip \
  --adapter hciN --app-address PINE_APP_ADDRESS --boot-address PINE_BOOT_ADDRESS

# Retry only the previously identified Pine bootloader after an interruption:
python3 firmware/nrf52840/ble_dfu.py --boot-only \
  --zip firmware/nrf52840/.build/upstream/.pio/build/nrfmast_fleet_lua/firmware.zip \
  --adapter hciN --boot-address PINE_BOOT_ADDRESS
```

Do not guess the bootloader address or select by the name `DFU`/`AdaDFU`.
Identify Pine's exact bootloader address during a controlled update; it can
differ from the application address. The wrapper validates
the app-only ZIP and pinned external library digest, leaves high-MTU mode off,
and uses standard library handover/transfer calls without implementing a new
protocol. A transfer result still needs node/data/RF checks; aborted-transfer
wireless recovery is not established by the wrapper's software tests.

Pine stages the installed Adafruit `BLEDfu.cpp` unchanged except for a gate
before its standard handover, renaming its C++ symbols. The gate accepts only
the standard start operation (one byte `01`, or app-only `01 04`), checks
bonded, authenticated BLE security and checks the monotonic deadline. The Adafruit
handover carries the selected peer's bond/address into the bootloader.
The original `NRF52Board::startOTAUpdate` path is never called by Pine's CLI.

The bootloader can change the advertised name/address and service handles.
Let the Nordic DFU tool reconnect; do not select another nRF board by name.
Its bond/peer handling is bootloader-controlled, not a Pine ACL.
The packaged CRC detects transfer corruption; it is **not a signature**.
The stock bootloader also supports other image types: once handover occurs,
the application cannot enforce the ZIP contents or restore its ACL. Only use
the validated, trusted app-only ZIP; control physical/BLE access during DFU.

If entry is not used within 120 seconds, Pine disconnects BLE and restarts
with its saved BLE configuration and bot source. `stop ota` cancels the window
and schedules the same restart. It cannot cancel a transfer once the
bootloader has taken control. Remote `poweroff`/`shutdown` are rejected
because SYSTEMOFF would strand an inaccessible Pine.

## Aborted transfer and preservation checks

**Keep Pine accessible by USB; mast deployment is on hold.** Its installed
bootloader `0.6.1` completed a 643,736-byte BLE application update and restarted
with the saved role names, PHY, policy and Lua source. However, after a transfer
was interrupted at 8,200 bytes, a fresh start returned Nordic response `100102`
(invalid state). The standard reset command left USB-only recovery.
Application-only USB DFU restored Pine; a subsequent complete BLE update worked.

An interrupted transfer can leave the current bootloader session active.
Reconnecting does not guarantee that a new transfer can start, and resetting
does not guarantee BLE will return. Keep the verified app-only ZIP and USB
recovery available. Do not erase flash or replace the bootloader to recover.
Move Pine out of reach only after interrupted-transfer recovery works wirelessly.

Application-only updates do not target InternalFS identities, native ACLs,
radio settings, BLE provisioning, Lua QSPI source/KV/timers, notes or caller
state. Save a private baseline of both public role keys, settings, source hash
and representative caller data; verify those values after each update.
The update preserves the configured PHY and path width; choose legal local
settings rather than copying the owner's test profile.
Boot stops on unreadable identities or existing filesystems;
it does not format them or regenerate required fleet keys. A transient SX1262
initialization failure retries with 1–30 second backoff without touching state.

### Read-only source and caller-bank checks

Through authenticated Pine-Relay administration, capture `source hash`, both
public role keys and the saved radio/ACL settings before arming. For each
known full caller identity whose KV must survive, use its lowercase 64-digit
public key:

```text
bot data help
bot data export kv caller CALLER_KEY64
bot data status
bot data read ID16 0
bot data read ID16 1
```

Poll status through `PENDING`/`BUSY` until
`EXPORTED SHA256_HEX64 ID16`; the ID is the hash's first 16 hex digits.
Read increasing indices until `EOF`. Each `DATA HEX` row carries at most
48 snapshot bytes. Save the complete snapshot privately, verify its SHA-256,
and repeat the export after restart to compare the same caller KV and selected
source hash. Do not compare live counters, heap, Lua globals or runtime epochs.
For bot-scoped KV use `bot data export kv bot KEY64` with 64 zero digits.
`conversation` and `channel` exports also require their full nonzero principal
identity; they are separate scopes, not aliases for caller data.

These commands export frozen RAM snapshots; they do not restore or erase
committed banks. Do **not** use `begin`, `chunk`, `stage`, `restore`, provisioning
or source replacement as a preservation check. Timers/reminders can legitimately
advance or claim work, and completed/claimed jobs must not be rearmed for a
byte-for-byte comparison. Keep snapshots and caller data out of public logs.

Identify your radio by its stable USB serial path before recording its BLE
address. Select the host Bluetooth controller explicitly; do not let a scan
or recovery attempt connect to another nearby board.

1. Verify the saved PIN, `get update` version and capability before deployment.
2. Check that an unarmed or unencrypted DFU write is refused; confirm regular
   companion operations still work.
3. Arm and abandon application entry; check the restart at 120 seconds, saved
   BLE mode and unchanged identities/data. Also check `stop ota`.
4. Arm, authenticate and transfer the ZIP using Nordic Legacy DFU. Confirm
   the new firmware version, preserved source/data/settings and RF reception.
5. With physical recovery available, interrupt a transfer and retry the ZIP.
   Record whether the installed bootloader reconnects to that phone or needs
   USB recovery. This is not rollback testing.

Source/build tests do not exercise a real BLE transfer or aborted-transfer
recovery. Record the installed bootloader version and phone compatibility
before relying on field DFU; the interrupted-transfer USB recovery limitation
above remains.

## Restore UTC automatically

An authorized companion can refresh UTC with the existing MeshCore v13
`CMD_SET_DEVICE_TIME` frame: opcode `6` followed by four little-endian UNIX
seconds. Pine accepts it only through the bonded PIN/MITM BLE companion connection;
the accepted time also refreshes Lua's trusted UTC for one hour. An ordinary
MeshCore app's connection-time synchronization therefore restores trusted
time after a restart. Use a host service for long-running autonomous refresh:
send the same standard command every 30 minutes and after reconnection.
Alternatively, an authorized host using the repeater admin connection can
send the existing `clock sync` or `time UNIX_SECONDS` command every 30 minutes;
a successful native clock update refreshes the same trusted Lua clock.
For a persistent operator-side refresh, prefer `bot time UNIX_SECONDS` through
the existing encrypted repeater-admin CLI. Its exact success text is:

```text
Authenticated UTC accepted for 1h; pending deadlines retained; completed/claimed jobs never rearmed
```

The sender must have the repeater's existing native admin permission and pass
its encrypted-message/replay checks; possession of an arbitrary seed is not
enough. `time`/`clock sync` refresh Lua trust only when the native command
actually succeeds (`OK - clock set:`), not when it refuses an equal/backwards
clock. `bot time` accepts an explicit authorized correction and refreshes even
without a forward clock step. Refresh after restart and every 30 minutes; this
does not require a continual BLE phone connection or a new mesh protocol.
The operator-side [`time_refresh.py`](time_refresh.py) helper uses this existing
admin connection:

```sh
python3 firmware/nrf52840/time_refresh.py \
  --gateway SHARED_MODEM_HOST --port 8001 \
  --seed-file PRIVATE_ADMIN_SEED_FILE \
  --target-key PINE_REPEATER_PUBLIC_KEY64
```

Use the existing authorized admin seed in a protected private file; do not
print it or provision a new radio identity. The host must report
`NTPSynchronized=yes` through `timedatectl`; the helper refuses to change Pine's
clock otherwise. It logs in, writes current UTC explicitly with `bot time`,
reads status and requires `trusted=1`. It refreshes every 1800 seconds and
retries connection or synchronization failures after 30 seconds. Add `--once`
for one refresh attempt. The helper already supplies this refresh/retry loop;
no additional scheduler authoring is required. For an installed operator
service, use its existing lifecycle management rather than starting a duplicate
loop.
The `bounds=EARLIEST..LATEST` values are inclusive UNIX UTC seconds, not
milliseconds or uptime: immediately after a refresh they bracket the accepted
time by two seconds. Uncertainty grows by one second per ten minutes; trust
expires after one hour and both bounds become zero. The `source=admin-console`
label also covers an authorized RF admin, not only a physical console.
Expiry is processed even when the bot is inactive or has no pending jobs.
An expired sample stays untrusted through uptime rollover and status queries;
only a new authorized refresh can restore trust.
Unpaired traffic and timestamps outside the supported range are refused.
The first paired-companion time sample after boot, and the first sample after
trust expires, have no correction limit beyond the supported UNIX-seconds
range. The bonded companion is a time authority: a phone with an incorrect
clock can establish incorrect trusted UTC for a one-hour window; further
accepted refreshes renew that window. Keep its clock synchronized; an
authorized `bot time UNIX_SECONDS` command can correct it immediately. The
`source` label identifies who supplied the time, not an independent check that
it is accurate.
While the last trusted sample is less than one hour old, companion corrections
more than two seconds behind or 300 seconds ahead of Pine's running clock are
refused without changing the clock or trust sample. A sample up to two seconds
behind renews trust while retaining the running time, avoiding backward clock
steps from drift or whole-second rounding; it fits the initial two-second
uncertainty in the trusted bounds. An
authenticated repeater owner can use `bot time UNIX_SECONDS` to make a larger
correction explicitly. After expiry or reboot, a valid authorized companion
sample can establish UTC even if the unsynchronized clock is ahead of it.
An incorrect future sample therefore cannot permanently prevent correction.

The focused helper uses that same standard frame, not a new protocol:

```sh
python3 firmware/nrf52840/ble_field.py inspect --adapter hciN --address PINE_ADDRESS
python3 firmware/nrf52840/ble_field.py time --adapter hciN --address PINE_ADDRESS --repeat
```

It requires `bleak` in the selected host Python environment and an already
authorized connection to Pine. It never scans for/selects a device, arms DFU
or writes a firmware image. Run it only after selecting Pine's verified
address and controller; it does not configure BlueZ pairing for you.
Connection failure stops the helper; a supervising host service should
reconnect and refresh again. Do not invent UTC from uptime, build time or
radio reception. If no authorized time refresh arrives within one hour,
Lua deadlines remain paused until a fresh sample is accepted.
`bot time` reports the last time authority as `source=paired-ble`,
`source=admin-console` (authorized RF owner or local console), or `source=none`
after boot. The source label remains visible after expiry; `trusted=0` and
cleared bounds indicate that it no longer authorizes deadline execution.

## Bootloader and watchdog boundary

The installed MeshCore Adafruit Arduino framework is `1.10701.0` (upstream
Arduino 1.7.1). Preparation verifies the known `BLEDfu` hook before staging it.
The retained bootloader is not rebuilt, signed or replaced.
Adafruit's [0.6.1 main.c](https://github.com/adafruit/Adafruit_nRF52_Bootloader/blob/0.6.1/src/main.c)
and [bootloader.c](https://github.com/adafruit/Adafruit_nRF52_Bootloader/blob/0.6.1/lib/sdk11/components/libraries/bootloader_dfu/bootloader.c)
show indefinite BLE DFU and explicit feeding of all running hardware watchdog
channels. Its [single-bank implementation](https://github.com/adafruit/Adafruit_nRF52_Bootloader/blob/0.6.1/lib/sdk11/components/libraries/bootloader_dfu/dfu_single_bank.c)
erases the application before receipt. These references describe the library
family; `get update` and real transfer checks establish Pine's installed build.
No hardware watchdog is enabled by this update feature: a watchdog cannot
supply the missing bootloader timeout and must be separately accepted across
BLE/USB recovery before enabling it. A bounded post-handover session or
rollback requires a separate bootloader decision.
