# Set up a public Aspen radio over USB

Use a Seeed XIAO ESP32S3R8 with a Wio SX1262 and the generic
Aspen application. The application needs your **private SPIFFS setup
image before it starts RF or administration**. Provision your credentials
offline over USB. This ESP32-S3 profile uses `.bin` files with esptool.
Get the [public Aspen bundle](https://ve6slp.ca/projects/meshcore/#downloads)
(`1.17.1-slp-aspen`, earlier source `8b41d9c`) or build the generic image using the
offline checks below. Keep public application files separate from your private
per-node setup image. Current source builds identify `aspen-0.1.1`; see the
[release guide](../../release/README.md) before substituting a candidate for the
historical download.

The application supports repeater, room, companion, Management and Lua/Wasm
roles on one shared modem. The companion service is TCP port 5000 when WiFi is
enabled; KISS is port 8001. Management uses your initial password and optional
trusted companion public key. Each on-device identity is generated locally
after valid setup; no identity seed is in the public application or setup
profile.

**SPIFFS provisioning is for a blank board only. Replacing SPIFFS on an existing
node destroys its role preferences, ACLs, contacts, channels, source programs,
Lua/Wasm data and caller banks. An application update must never run
`install-config`, `uploadfs`, an erase command or a full/factory flash.** NVS,
including existing node identities and saved shared radio settings, is never
written by the setup installer.

## Make your private setup image

From the repository root, create an owner-only directory outside the repository:

```sh
umask 077
mkdir -p "$HOME/.config/aspen-private"
chmod 700 "$HOME/.config/aspen-private"
install -m 600 firmware/esp32/public-profile.example.json \
  "$HOME/.config/aspen-private/profile.json"
```

Edit that private JSON file locally. Set `admin_password` (native role
administrator) and `mast_password` (Management/Web login) to your own 1..15-byte
printable ASCII passwords. An empty `room_password` permits guest room access;
set it if guest access needs a password. The example deliberately has an
invalid operator key so it cannot be used unchanged.

`operator_public_key` is the lowercase, 64-hex-digit **Ed25519 verification
public key** belonging to your operator signing seed. Generate an operator key
using the existing helper if needed:

```sh
python3 tools/hardware/rf.py init-key \
  --key-file "$HOME/.config/aspen-private/operator.seed"
```

Copy only the printed verification public key into the profile. Keep the seed
on the operator's host, outside the repository and image; do not paste a seed,
private identity key or signing key into any profile. Leave
`trusted_companion_public_key` empty or supply your own companion's public key.
It is separate from the signing authority.

Choose legal radio settings **before enabling any role**. The example is
912.525 MHz, 250 kHz bandwidth, SF7, CR5, TX 2 dBm, with three-byte paths; this
is not authorization to use that frequency or power in your location.
Frequency and bandwidth are integer Hz. Supported bandwidths are 7800, 10400,
15600, 20800, 31250, 41700, 62500, 125000, 250000 and 500000 Hz. Frequency must
be 150..960 MHz, SF 5..12, CR 5..8 and TX 0..22 dBm. Select a supported regional
frequency and antenna before booting.

`roles` is a bitmask: repeater=1, room=2, companion=4, observer=8; zero disables
these four roles, not Management or the command bot. `path_width` is 1..3 bytes.
Set `wifi_enabled` to `true` and supply `wifi_ssid`/`wifi_password` for a station
connection, or leave WiFi disabled for RF-only operation. An empty WiFi password
means an open station network. WiFi setup does not create an AP. There are no
MQTT credentials in this minimal setup; MQTT is disabled in the public profile.
All documented JSON fields are required; unknown, duplicate, wrong-type and
out-of-range fields are rejected without printing their values.

Set `APP` to the extracted public bundle directory containing `partitions.bin`
and `firmware.bin`, or your local generic build output, and build your private
filesystem image with the installed
PlatformIO SPIFFS tool:

```sh
APP="$PWD/public-aspen"
ESPTOOL="$HOME/.platformio/packages/tool-esptoolpy/esptool.py"
MKSPIFS="$HOME/.platformio/packages/tool-mkspiffs/mkspiffs_espressif32_arduino"
python3 firmware/esp32/public_setup.py image \
  --profile "$HOME/.config/aspen-private/profile.json" \
  --output "$HOME/.config/aspen-private/setup-spiffs.bin" \
  --partitions "$APP/partitions.bin" --mkspiffs "$MKSPIFS"
```

Replace `public-aspen` with your extracted bundle's actual directory.
For the local build below, use
`.tmp/public-aspen-setup-validation/.pio/build/public_aspen`.

`image` accesses no hardware. The output is a private mode-0600 regular file;
the tool refuses symlinks, public directories and overwriting an existing file.
**Never put the JSON, private SPIFFS image, backup or operator seed in a public
ZIP or build directory.** A public ZIP may contain only the generic application,
initial-install boot files and this invalid example.

## First install: blank board, USB bootloader

Hold BOOT while connecting USB, then select the USB port explicitly. Keep the
board in its ROM bootloader until both configuration and application have been
written. This operation needs esptool **4.5.1** and its Python dependencies.

```sh
PORT=/dev/ttyACM0
python3 firmware/esp32/public_setup.py install-config \
  --image "$HOME/.config/aspen-private/setup-spiffs.bin" \
  --partitions "$APP/partitions.bin" \
  --backup "$HOME/.config/aspen-private/before-setup-spiffs.bin" \
  --esptool "$ESPTOOL" --port "$PORT"
```

The installer checks the connected chip, partition table and NVS/SPIFFS
contents, saves a private **SPIFFS-only** backup, then writes and verifies only
the existing `0x180000`-byte SPIFFS partition at `0x670000`; it does not write the
separate coredump partition. It leaves USB bootloader mode active. NVS is
inspected in memory for blankness; its identity/private-key records are not
exported. Existing storage is refused by default. An already-run generic app
may have initialized NVS; this counts as existing storage too.

For a truly blank first install, write the bootloader, unchanged partition
table, initial OTA selection and app0. These exact commands use the PlatformIO
build outputs; the release's initial-install files must match them:

```sh
python3 "$ESPTOOL" --chip esp32s3 --port "$PORT" --after no_reset write_flash \
  --flash_size 8MB \
  0x0 "$APP/bootloader.bin" \
  0x8000 "$APP/partitions.bin" \
  0xe000 "$HOME/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin" \
  0x10000 "$APP/firmware.bin"
```

Reset the board after both operations. USB diagnostics should report
`Public setup loaded; saved role preferences and identities retained`, then
the selected native roles becoming ready. With WiFi enabled, use
`http://aspen.local/admin` or the printed IP address and your `mast_password`
to log in. The dashboard shows the roles and shared PHY; confirm the intended
frequency/power before sending messages. With WiFi disabled there is no Web UI;
use your companion radio for authenticated Management RF interaction.

Missing/malformed setup, failed SPIFFS mount or a wrong layout reports
`Public setup ...; RF and administration disabled; restore private setup over USB`.
It must not generate/broadcast identities, start RF listeners or confirm an OTA
application as healthy. There is no unauthenticated network repair path.

For an intentional **destructive SPIFFS replacement** on an existing node,
first retain its original SPIFFS backup securely and understand that NVS
identities/shared PHY may survive while role data is lost. Run the same
`install-config` command with a new backup filename and
`--confirm-replace-spiffs-with-backup`. It refuses to proceed unless it can
create the private backup. Do not use this as an update or password-change
procedure. Use existing authenticated role/Management commands for ordinary
setting changes.

## Optional telemetry and HTTPS

The Aspen 0.1.1 public application includes native HTTPS and telemetry, with
no configured endpoint, CA or token. It sends no telemetry until an
administrator configures the collector and enables publishing. Use
[telemetry setup](TELEMETRY.md) through authenticated Management RF to stage
credentials, then read `telemetry status` and `telemetry counts`.
Use `help syslog` for independent UDP logging; it needs no HTTPS collector.
The HTTPS profile reserves one network socket for its worker and accepts up
to three KISS clients and two companion TCP clients. UDP syslog uses no
additional socket-table slot.

## Later updates: application only

The two application slots remain app0=`0x10000`, app1=`0x340000`, each
`0x330000` bytes. Public updates keep SPIFFS and NVS: passwords, operator
verification authority, role preferences, identities, programs and caller
banks remain. Saved runtime settings override the initial setup defaults.
The persisted operator verification key remains the authority for signed
updates targeting `xiao-esp32s3`; the generic app supplies no signing authority.
See [signed field updates](ESP_FIELD_UPDATES.md) for the existing Web/BLE flows.

For USB app-only recovery/update, find the currently selected, health-confirmed
slot rather than guessing `0x10000`. This command reads only partitions and OTA
selection, not NVS identities:

```sh
SLOT="$(PYTHONPATH=. python3 -c \
  'import sys; from pathlib import Path; from tools.hardware.esp32_slot import live_app_slot; address,size=live_app_slot(sys.argv[1],sys.argv[2],sys.argv[3],Path(sys.argv[4]).read_bytes()); print(hex(address))' \
  "$ESPTOOL" "$PORT" "$HOME/.config/aspen-private" "$APP/partitions.bin")" &&
python3 "$ESPTOOL" --chip esp32s3 --port "$PORT" write_flash \
  "$SLOT" "$APP/firmware.bin"
```

A pending, invalid or ambiguous OTA selection is refused; confirm or roll back
the previous signed update first. This writes **one application slot only**.
Do not write a merged/factory image, bootloader, partitions, OTA selection,
SPIFFS or NVS for ordinary updates.

## Build and offline checks

Use the pinned native revision
`d92964352441e53b93e8667b802e04f6e072b39e`, ESP32 platform 6.11.0, and
`platformio.public.ini.example`, not a private fleet profile or `.env.dev.local`.
From the repository root:

```sh
env -i HOME="$HOME" USER="$USER" PATH="$HOME/.local/bin:/usr/local/bin:/usr/bin:/bin" \
  PYTHONDONTWRITEBYTECODE=1 \
  make -C firmware/esp32 bot-firmware \
  BUILD="$PWD/.tmp/public-aspen-setup-validation" \
  CONFIG="$PWD/firmware/esp32/platformio.public.ini.example" ENV=public_aspen
make -C firmware/esp32 public-provisioning-test
make -C firmware/esp32 public-update-test
```

The fixed setup record `/public-setup.bin` is 336 bytes: `MCP` plus version 1,
role mask/path width, little-endian radio integers, bounded zero-padded ASCII
credentials/public keys, WiFi enable byte, zero reserved bytes and a SHA256
digest over the first 304 bytes. The digest detects damaged records; it is not
authentication. Physical USB access and private handling of the image are the
setup trust boundary. Existing fleet builds without
`MESHCORE_PUBLIC_PROVISIONING=1` keep their compiled defaults.

Startup loads the private setup before board, WiFi, radio or identity
initialization. Signed updates use its operator verification key, and OTA
confirmation requires valid setup as well as healthy roles. Non-public builds
retain their compiled credentials and settings.
