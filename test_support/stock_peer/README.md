# MeshCore USB companion radio

Builds the upstream MeshCore `companion-v1.17.1` USB companion application for
the Xiao S3/WIO-SX1262. The radio exchanges messages with the host over RF;
the checker connects to its standard companion interface over USB.

From the repository root:

```sh
make stock-firmware
```

## Radio profile

This image starts at **912.525 MHz, BW 250 kHz, SF7, CR5, TX 2 dBm** on a
newly erased device. This is the owner's test profile, not a worldwide channel
recommendation. Set legal local frequency, bandwidth and power and match the
host before exchanging messages. Existing saved preferences can override
compiled settings; inspect/change them explicitly rather than erasing a radio
whose identity or contacts must survive.

The companion sends no startup advert with fresh preferences. Explicit
commands or received traffic can still trigger transmissions.

## Build output

The build uses a separate upstream checkout at `.tmp/stock-MeshCore` and writes
the firmware images under:

```text
.tmp/stock-MeshCore/.pio/build/Stock_Xiao_S3_WIO_companion_usb/
```

`firmware-merged.bin` includes the bootloader, partition table and application.

## Flash a radio for RF checks

Upload requires an explicit device path and its expected hardware MAC. There
is no default port or MAC, and a mismatch stops the operation before erase.
Use a device whose identity and saved settings can be erased.

The upload target enters ROM download mode through esptool's normal automatic
reset, verifies the chip MAC, then erases and writes flash in one connection:

```sh
make stock-firmware-upload \
  STOCK_SERIAL='/dev/serial/by-id/<companion-radio>' STOCK_DEVICE_MAC='<device-mac>'
```

This discards **all old flash state**, including identity, contacts, channels,
preferences, NVS and old OTA images. The merged image is written at `0x0`;
the radio remains in download mode until the separate boot step.

After upload reports a verified write, explicitly boot the selected device:

```sh
make stock-firmware-boot \
  STOCK_SERIAL='/dev/serial/by-id/<companion-radio>' STOCK_DEVICE_MAC='<device-mac>'
```

The boot target checks the MAC again before resetting. If automatic download
entry fails, use the board's BOOT/reset controls and repeat the upload; never
boot after a failed write. `PYTHON` and `ESPTOOL` are overridable for an alternate
PlatformIO installation.

## Board and serial protocol

The board is `seeed_xiao_esp32s3`, SX1262 with TCXO 1.8 V:

| Signal | GPIO |
| --- | --- |
| NSS / DIO1 / RESET / BUSY | 41 / 39 / 42 / 40 |
| SCLK / MISO / MOSI | 7 / 8 / 9 |
| RXEN / DIO2 RF switch | 38 |

The board manifest retains `ARDUINO_USB_MODE=1`,
`ARDUINO_USB_CDC_ON_BOOT=1`, 8 MB flash and Arduino variant `XIAO_ESP32S3`.
USB uses `ENABLE_USB_INTERFACE`, `Serial` at 115200 and standard companion
framing: `<` plus uint16-LE length from host; `>` plus uint16-LE length from
device.

Command 11 frequency is uint32-LE **kHz** (`912525`); bandwidth is uint32-LE
**Hz** (`250000`), followed by SF7/CR5 and optional repeat=0. Command 12 power
is signed dBm (`2`). Commands 21/43 use uint32 thousandths for RX delay base
and airtime factor, not the custom shared-PHY float32 format.
Read back the active radio profile after boot.

A true power cycle may be necessary after flashing: USB/software resets can
retain an invalid ESP32 RTC value, and native CMD6 rejects moving the clock
backwards. Remove all power to restore the fallback clock's power-on baseline.
Saved contact timestamps can bootstrap the clock, so disposable test state
may also need clearing before power cycling an incorrectly dated test device.

`make stock-peer-probe STOCK_SERIAL=...` reads the companion's device query and
clock. Optional `STOCK_RESET=1` triggers a
software boot; it is not a power cycle and does not replace the RTC recovery
procedure. First filesystem setup must finish before a companion handshake can
succeed.
