# Heltec WiFi LoRa 32 V3 shared modem

Use a regular Heltec WiFi LoRa 32 V3 as the shared radio for Willow or another
host role over KISS TCP on port 8001. Host role identities remain on the host.
The modem includes a dashboard, but no on-device repeater, room, companion,
command bot, Lua, Wasm or shared-room frontend. Full Aspen needs PSRAM, which
this board does not have.

From the repository root, check the default profile with PlatformIO installed:

```sh
make -C firmware/esp32 heltec-modem-build
```

The target exports the pinned MeshCore source and builds in
`.tmp/heltec-v3-modem`, using two compiler jobs by default (`PIO_JOBS` overrides
that limit). It does not upload firmware. **The default image has empty WiFi
credentials; do not flash it expecting a usable WiFi connection.**

For a configured modem, make a private copy outside the repository:

```sh
umask 077
mkdir -p "$HOME/.config/aspen-private"
chmod 700 "$HOME/.config/aspen-private"
install -m 600 firmware/esp32/platformio.heltec-modem.ini \
  "$HOME/.config/aspen-private/heltec-v3.ini"
$EDITOR "$HOME/.config/aspen-private/heltec-v3.ini"
make -C firmware/esp32 heltec-modem-build \
  MODEM_CONFIG="$HOME/.config/aspen-private/heltec-v3.ini"
```

Keep the existing environment name. Set `WIFI_SSID`, `WIFI_PWD` and
`KISS_HOSTNAME` in that private file. Append explicit `LORA_FREQ` (MHz),
`LORA_BW` (kHz), `LORA_SF`, `LORA_CR` and `LORA_TX_POWER` flags for your legal
local PHY, matching the other radios. Connect the appropriate antenna before
booting. Keep the file and compiled credential-bearing application private.

The application is
`.tmp/heltec-v3-modem/.pio/build/Heltec_v3_kiss_wifi_build/firmware.bin`.

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
  TMPDIR="$PWD/.tmp/heltec-v3-modem" \
  PLATFORMIO_CACHE_DIR="$PWD/.tmp/heltec-v3-modem/.cache" \
  pio run -d .tmp/heltec-v3-modem -e "Heltec_v3_kiss_wifi_build" \
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
the dashboard and configure the host's modem endpoint as `ADDRESS:8001`.
A host owns the mesh identities; changing its radio does not move those keys
onto the Heltec. See the [host guide](../../HOST_GUIDE.md) for role setup.

## Board and radio wiring

The regular V3 uses an **ESP32-S3FN8**, with **8 MiB flash and no PSRAM**, and an
SX1262. It is not the S3R8 PSRAM configuration used by Aspen.

The profile extends MeshCore's `Heltec_lora32_v3` variant at
`companion-v1.17.1` (`d92964352441e53b93e8667b802e04f6e072b39e`):

| Signal | GPIO / upstream setting |
| --- | --- |
| SX1262 NSS / SCK / MOSI / MISO | 8 / 9 / 10 / 11 |
| SX1262 BUSY / DIO1 | 13 / 14 |
| SX1262 reset | Physically GPIO12; upstream uses `RADIOLIB_NC` |
| RF switch / TCXO | SX1262 DIO2 enabled / DIO3 at 1.8 V |
| LED / button / VEXT enable | 35 / 0 / 36 |
| I²C SDA / SCL | 17 / 18 |

Keep the upstream reset and TCXO behavior. Do not substitute GPIO12 for
`RADIOLIB_NC` just because it appears in the board schematic. OLED, optional
GPS and other peripherals have not been exercised by this modem build.

References: [Heltec V3 datasheet](https://resource.heltec.cn/download/WiFi_LoRa_32_V3/HTIT-WB32LA_V3(Rev1.1).pdf),
[Espressif ESP32-S3 ordering information](https://documentation.espressif.com/esp32-s3_datasheet_en.pdf),
and [pinned MeshCore variant](https://github.com/meshcore-dev/MeshCore/blob/d92964352441e53b93e8667b802e04f6e072b39e/variants/heltec_v3/platformio.ini).

## Build limits

`platformio.heltec-modem.ini` uses the upstream ESP32 platform 6.11.0 and
Arduino `default_8MB.csv` layout:

| Partition | Offset | Size |
| --- | --- | --- |
| NVS | `0x9000` | 20 KiB |
| OTA metadata | `0xe000` | 8 KiB |
| Application 0 | `0x10000` | 3264 KiB |
| Application 1 | `0x340000` | 3264 KiB |
| SPIFFS | `0x670000` | 1536 KiB |
| Core dump | `0x7f0000` | 64 KiB |

The initial compiled image uses 1,209,125 bytes of its 3,342,336-byte application
slot; `firmware.bin` is 1,209,536 bytes. Static RAM is 168,288 of the linker's
327,680 bytes, leaving 159,392 bytes before runtime allocations and task stacks.
Runtime free heap depends on WiFi and dashboard activity.

The compiled target has `KISS_PSRAM_PAYLOADS=0`, four TCP clients, a 12-request
queue, no local sources and no queued UART. The SDK provides 16 sockets. The
dashboard check budgets 12: four KISS clients, one KISS listener, three HTTP
internal sockets, three HTTP clients and one spare. mDNS and SNTP also need
UDP sockets at runtime; do not increase client counts based only on unused
static RAM. Two of the three HTTP client slots can be live dashboard streams.

Run the focused profile check without compiling:

```sh
python3 -m unittest discover -s firmware/esp32/tests -p test_heltec_profile.py
```

Use the configured PHY and a second radio to check reception after first boot.
A local TX completion alone does not establish RF reception.
