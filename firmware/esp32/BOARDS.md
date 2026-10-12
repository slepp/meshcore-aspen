# Aspen ESP32 radio boards

Build full Aspen for the radios below. All run the native roles, Lua and Wasm
command bot, dashboard and shared-room frontend. Choose the exact board and
radio variant before building; the wiring, PSRAM mode and update target differ.

| Radio | Flash / PSRAM | PlatformIO environment | Signed update target |
| --- | --- | --- | --- |
| Seeed XIAO ESP32-S3R8 + Wio SX1262 | 8 MB / 8 MB octal | `public_aspen` | `xiao-esp32s3` |
| Heltec WiFi LoRa 32 **V4 R8**, SX1262 | 16 MB / 8 MB octal | `public_aspen_heltec_v4_r8` | `heltec-v4-r8` |
| LilyGO T-Beam S3 Supreme **SX1262**, 8 MB QSPI PSRAM | 8 MB / 8 MB quad | `public_aspen_tbeam_supreme_sx1262` | `tbeam-supreme-sx1262` |

The additional Heltec and LilyGO ports have been compiled with the complete
Aspen application. Physical boot, RF, battery readings and WiFi/OTA on these
boards still need testing on hardware. Aspen uses its web dashboard; these
profiles do not provide an on-board display interface or GNSS setup.

The V4 R8 has different peripheral pins from the 2 MB PSRAM Heltec V4.
Do not use its image on a V4 or V3. Heltec V3 lacks the PSRAM required by full
Aspen; its separate KISS modem image remains available. The T-Beam Supreme
LR1121 and ordinary T-Beam use different radio hardware. T3-S3 boards with
4 MB flash cannot hold two current full Aspen applications plus role storage.

## Build from source

Use the repository's pinned MeshCore revision and installed PlatformIO tools.
From the repository root, select one environment from the table:

```sh
ASPEN_ENV=public_aspen_heltec_v4_r8
ASPEN_BUILD="$PWD/.tmp/onchip-boards"
make -C firmware/esp32 bot-prepare \
  BUILD="$ASPEN_BUILD" \
  CONFIG="$PWD/firmware/esp32/platformio.public.ini.example" \
  ONCHIP_BOT_WASM=1
pio run -d "$ASPEN_BUILD" -e "$ASPEN_ENV" -j 2
APP="$ASPEN_BUILD/.pio/build/$ASPEN_ENV"
```

These generic applications contain no WiFi password or operator authority.
Select legal radio settings and connect an antenna suitable for that frequency
before booting.
The Heltec's external amplifier affects radiated power; the configured SX1262
power is not the complete antenna output power.

## First install on a blank board

The browser installer and published XIAO ZIP select XIAO hardware. For these
source builds, use USB and the files from the selected `APP` directory above.
Stay in the repository root and retain the selected `APP`; skip the XIAO
bundle download and extraction steps in the offline guide. Prepare USB tools:

```sh
python3 -m venv "$HOME/.local/share/aspen-setup-venv"
. "$HOME/.local/share/aspen-setup-venv/bin/activate"
python3 -m pip install 'esptool==4.5.1' 'platformio==6.1.19' 'cryptography>=37,<46'
pio pkg install --global --tool 'platformio/tool-mkspiffs@2.230.0'
ESPTOOL="$HOME/.local/share/aspen-setup-venv/bin/esptool.py"
MKSPIFS="${PLATFORMIO_CORE_DIR:-$HOME/.platformio}/packages/tool-mkspiffs/mkspiffs_espressif32_arduino"
```

Follow the [private JSON profile and operator-key instructions](PUBLIC_SETUP.md#make-your-private-setup-image),
then return here to build the filesystem image. Use the same `APP` for
configuration and firmware:

```sh
python3 firmware/esp32/public_setup.py image \
  --profile "$HOME/.config/aspen-private/profile.json" \
  --output "$HOME/.config/aspen-private/setup-spiffs.bin" \
  --partitions "$APP/partitions.bin" --mkspiffs "$MKSPIFS"
```

**The following writes are for a blank board only. Replacing role storage or
the partition table on an existing node destroys saved identities and data.**
Hold BOOT while connecting USB. Select the radio by its USB serial path and
confirm its MAC before writing, especially with several boards connected:

```sh
PORT='/dev/serial/by-id/<selected-radio>'
python3 "$ESPTOOL" --chip esp32s3 --port "$PORT" --after no_reset read_mac
python3 firmware/esp32/public_setup.py install-config \
  --image "$HOME/.config/aspen-private/setup-spiffs.bin" \
  --partitions "$APP/partitions.bin" \
  --backup "$HOME/.config/aspen-private/before-setup-spiffs.bin" \
  --esptool "$ESPTOOL" --port "$PORT"
```

The installer accepts the standard 8 MB and 16 MB dual-slot layouts, refuses
an existing different partition table, and backs up SPIFFS before writing.
It refuses initialized NVS or SPIFFS by default. Keep all private JSON, setup
images and backups outside source and public build directories.

For Heltec V4 R8 set `ASPEN_FLASH_SIZE=16MB`; for XIAO or T-Beam Supreme use
`ASPEN_FLASH_SIZE=8MB`. The initial OTA selection file comes from the installed
Arduino framework, rather than the application build directory:

```sh
ASPEN_FLASH_SIZE=16MB
BOOT_APP0="${PLATFORMIO_CORE_DIR:-$HOME/.platformio}/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin"
python3 "$ESPTOOL" --chip esp32s3 --port "$PORT" --after no_reset write_flash \
  --flash_size "$ASPEN_FLASH_SIZE" \
  0x0 "$APP/bootloader.bin" \
  0x8000 "$APP/partitions.bin" \
  0xe000 "$BOOT_APP0" \
  0x10000 "$APP/firmware.bin"
```

Release BOOT and reset after both writes. Expect `Public setup loaded`, ready
roles and the configured PHY in the USB log. With WiFi enabled, open the
reported address and `/admin`. Check free PSRAM, role public keys and battery
voltage, then send a normal message to another radio on the same PHY and check
its reception. Do not assume a local TX completion means another radio heard it.

## Updates and layouts

Use [signed application-only updates](ESP_FIELD_UPDATES.md) after first setup.
Each profile embeds the update target in the table; the running application
rejects a signed update for another target. Keep the selected board's original
partition table. Do not write bootloader, partitions, OTA selection or SPIFFS
during application maintenance.

| Flash | App0 / App1 offsets | Each app slot | SPIFFS offset / size |
| --- | --- | --- | --- |
| 8 MB | `0x10000` / `0x340000` | `0x330000` | `0x670000` / `0x180000` |
| 16 MB | `0x10000` / `0x650000` | `0x640000` | `0xc90000` / `0x360000` |

Both layouts retain NVS at `0x9000` and OTA selection at `0xe000`, with a
separate final 64 KiB coredump partition.

## Wiring references

Profiles inherit MeshCore's board support at
`d92964352441e53b93e8667b802e04f6e072b39e`; Aspen does not duplicate its pin
definitions. Heltec uses `variants/heltec_v4_r8` and `qio_opi` memory;
T-Beam Supreme uses `variants/lilygo_tbeam_supreme_SX1262`, its `TBeamBoard`
power-management helper and `qio_qspi` memory. The Supreme profile explicitly
enables `BOARD_HAS_PSRAM`, which its upstream board descriptor omits.

For board identification, pin maps and amplifier specifications, see the
[Heltec V4 R8 manufacturer guide](https://github.com/HelTecAutomation/HeltecWiKi/blob/main/docs/devices/open-source-hardware/esp32-series/lora-32/wifi-lora-32-v4_r8_board/wifi-lora-32-v4_r8_board.md)
and the [LilyGO T-Beam Supreme hardware guide](https://github.com/Xinyuan-LilyGO/LilyGo-LoRa-Series/blob/master/docs/en/t_beam_supreme/t_beam_supreme_hw.md).
