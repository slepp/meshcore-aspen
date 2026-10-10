# Heltec WiFi LoRa 32 V3: build-only shared modem

Build the shared modem for a regular Heltec WiFi LoRa 32 V3 without on-device
Aspen roles or PSRAM:

```sh
make -C firmware/esp32 heltec-modem-build
```

Run this from the repository root with PlatformIO installed. The target exports
the pinned MeshCore source, stages the shared modem and public profile, then
compiles into `.tmp/heltec-v3-modem`. It does not read a private WiFi configuration
or upload firmware. The profile deliberately has empty WiFi credentials.

**Do not flash this build-only image.** It does not provision a usable network,
and build success does not establish working RF, startup heap or board recovery.
It is not a radio-silent image. The partition table is a build choice, not a
migration plan for an existing device; replacing a device's flash layout can
erase its settings and identity.

This is a modem for host roles, not a full Aspen port. The modem retains the
existing Birch modem version string. It includes the KISS TCP interface and
dashboard, but no local repeater, room, companion, command bot, Lua, Wasm,
CloudRoom frontend or on-device administration. Host role identities remain
on the host.

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

The initial compiled image uses 1,203,393 bytes of its 3,342,336-byte application
slot; `firmware.bin` is 1,203,808 bytes. Static RAM is 168,288 of the linker's
327,680 bytes, leaving 159,392 bytes before runtime allocations and task stacks.
This is not a measurement of free heap while WiFi and the dashboard are active.

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

The next hardware decision is a separately authorized modem startup/RF check
with retained-device-state handling and an explicit network/PHY configuration.
Full Aspen roles need a separate internal-memory design; this profile does
not enable them.
