# Original T-Beam v1.2 shared modem

Use an original LilyGO T-Beam v1.2 as the shared radio for Willow or another
host role. The host runs the repeater, room, companion and command bot and owns
their identities. The T-Beam provides queued KISS TCP on port 8001 and a web
dashboard. These profiles build the Birch 0.1.2 modem application, rather than
the standalone Aspen application.

The original T-Beam has **4 MB flash and 8 MB QSPI PSRAM**, an ESP32 and an
AXP2101 power-management chip. It is different from the ESP32-S3 T-Beam Supreme.
Full Aspen does not fit the original board's two standard OTA application
slots. Removing Wasm, HTTPS and the shared-room frontend still leaves too
little flash for the Lua bot plus Mast administration. A native-only build
fits by also removing the bot and Mast administration, including signed OTA;
use host roles on this board instead.

## Select the radio and build

Identify the installed LoRa chip and frequency band from its module label or
the original purchase record. The v1.2 board name alone does not select it.
These profiles cover **SX1276** and **SX1262** modules; do not substitute an
SX1278 or use a profile for another frequency band without checking its wiring
and antenna.

From the repository root with PlatformIO installed, select one environment:

```sh
# SX1276 radio:
MODEM_ENV=TBeam_v12_SX1276_kiss_wifi_build
# For an SX1262 radio, use this instead:
# MODEM_ENV=TBeam_v12_SX1262_kiss_wifi_build
make -C firmware/esp32 tbeam-modem-build MODEM_ENV="$MODEM_ENV"
```

The target requires an explicit environment, stages the pinned upstream source
in `.tmp/tbeam-v12-modem` and builds with two jobs by default. It does not read
hardware or upload firmware. `PIO_JOBS` changes the compiler job limit.
**The tracked configuration has empty WiFi credentials; do not flash the
default image expecting a working network connection.**

For a configured modem, copy the configuration outside the repository:

```sh
umask 077
mkdir -p "$HOME/.config/aspen-private"
chmod 700 "$HOME/.config/aspen-private"
install -m 600 firmware/esp32/platformio.tbeam-modem.ini \
  "$HOME/.config/aspen-private/tbeam-v12.ini"
$EDITOR "$HOME/.config/aspen-private/tbeam-v12.ini"
make -C firmware/esp32 tbeam-modem-build \
  MODEM_ENV="$MODEM_ENV" \
  MODEM_CONFIG="$HOME/.config/aspen-private/tbeam-v12.ini"
```

Keep the environment names and inherited board sections. Set `WIFI_SSID`,
`WIFI_PWD` and `KISS_HOSTNAME` in the private `[tbeam_modem]` section. Append
explicit legal `LORA_FREQ` (MHz), `LORA_BW` (kHz), `LORA_SF`, `LORA_CR` and
`LORA_TX_POWER` flags there. Match the other radios' PHY, select a power supported
by the installed module and connect the appropriate antenna before booting.
Keep the configuration and compiled credential-bearing application private.

The application is
`.tmp/tbeam-v12-modem/.pio/build/ENVIRONMENT/firmware.bin`.

## Install over USB

Build the private configuration above before uploading. Connect the antenna
and a USB data cable, then list the serial devices:

```sh
pio device list
ls -l /dev/serial/by-id/
```

Match the board's USB serial number to its device entry. Unplug and reconnect
that board if needed to identify it. Close serial terminals using the port.
Set the complete device path below; do not use a different attached radio's
port.

**This USB installation replaces the firmware, bootloader and partition table.**
On an existing node, preserve valuable identities, settings and data before
continuing: changing the layout can overwrite them or make them inaccessible.
Use this first-install path for a new board or a radio you intend to repurpose;
it is not an Aspen application-only update.

From the repository root, upload the configured build to that exact device:

```sh
UPLOAD_PORT='/dev/serial/by-id/REPLACE_WITH_THIS_BOARD_USB_SERIAL'
test -c "$UPLOAD_PORT" && \
  TMPDIR="$PWD/.tmp/tbeam-v12-modem" \
  PLATFORMIO_CACHE_DIR="$PWD/.tmp/tbeam-v12-modem/.cache" \
  pio run -d .tmp/tbeam-v12-modem -e "$MODEM_ENV" \
    -j 2 -t upload --upload-port "$UPLOAD_PORT"
```

PlatformIO selects the bootloader, partition table and application offsets
from this board's profile. Do not upload only `firmware.bin` at address zero.
If the uploader cannot enter download mode, hold BOOT, press and release RESET,
then release BOOT and retry the upload. Press RESET after a completed upload
if the board remains in download mode.

These profiles have been compiled; physical boot, RF, WiFi/dashboard memory
use and recovery still need testing on the actual board. The modem does not
include Aspen's signed application-update service.

After installation and a successful WiFi join, open the reported address for
the dashboard and point the host at `ADDRESS:8001`. See the
[Willow guide](../../hosts/willow/README.md) or
[Go host guide](../../HOST_GUIDE.md) for host roles. Test reception with another
radio on the same PHY; local TX completion alone does not establish reception.

## Memory, layout and wiring

The profiles retain the upstream `min_spiffs.csv` dual-slot layout:

| Partition | Offset | Size |
| --- | --- | --- |
| NVS | `0x9000` | 20 KiB |
| OTA metadata | `0xe000` | 8 KiB |
| Application 0 | `0x10000` | 1920 KiB |
| Application 1 | `0x1f0000` | 1920 KiB |
| SPIFFS | `0x3d0000` | 128 KiB |
| Core dump | `0x3f0000` | 64 KiB |

`BOARD_HAS_PSRAM` comes from the existing `ttgo-t-beam` board descriptor.
`KISS_PSRAM_PAYLOADS=1` uses the existing PSRAM packet-buffer allocation and
keeps the modem within the original ESP32's internal RAM. It requires working
PSRAM on the board; there is no internal-memory fallback. Radio ISR/DMA
buffers retain their existing internal-memory allocation.

The profiles inherit MeshCore's `LilyGo_TBeam_SX1276` or `LilyGo_TBeam_SX1262`
variant at `81dbae592ffad40b4212bc0b37e82b38da08f4bc`, including its radio,
PMU and battery wiring. They do not add a display interface or GNSS setup.
See the [LilyGO original T-Beam hardware guide](https://github.com/Xinyuan-LilyGO/LilyGo-LoRa-Series/blob/master/docs/en/t_beam/t_beam_hw.md)
for memory, radio variants and the v1.2 power-management connections.
