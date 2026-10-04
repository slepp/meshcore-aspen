# GPS clock synchronization

GPS-equipped radios start their GPS receiver at boot and process fixes in the
radio loop. After several valid UTC-bearing RMC sentences, UART GPS sets the board
RTC. RAK12500 I2C GPS sets it after a fresh navigation response with a valid fix,
valid date/time and fully resolved UTC. Aspen also updates its enabled on-device role clocks; Pine passes the UTC
sample into its existing trusted-time adapter for Lua timers and reminders.
The node does not wait for a fix before starting its radio roles.

## Choose the board

| Variant | GPS and clock behavior |
| --- | --- |
| Aspen on XIAO ESP32-S3 + WIO-SX1262 | No onboard GPS. Build-day role clocks provide offline startup; SNTP supplies trusted UTC when WiFi is available. |
| Aspen/shared modem using a GPS-equipped upstream ESP32 board | Retains that board's GPS UART, power controls and location provider. The shared startup enables GPS and runs the sensor and board RTC loops. Valid NMEA UTC reaches Aspen's role clocks. |
| Pine `nrfmast_fleet_lua` on XIAO nRF52840 + WIO-SX1262 | No onboard GPS in this board definition. Trusted UTC comes from paired BLE companion set-time or authenticated repeater administration. |
| Pine `nrfmast_solar_lua` on SenseCAP Solar Node P1 Pro, or P1 with the GPS module installed | Uses upstream `SenseCap_Solar` GPS, battery measurement and QSPI wiring. P1 Pro includes GPS and batteries; P1 does not. GPS starts on every boot, including a boot without a paired phone. Battery readings remain available through `stats sensors` and telemetry. |
| Birch host roles | Use the host's existing verified system-clock provider. A GPS fix on the shared modem is not transferred to or trusted by the Linux host. Keep the host clock synchronized independently. |

The firmware source is pinned to MeshCore
[`d92964352441e53b93e8667b802e04f6e072b39e`](https://github.com/meshcore-dev/MeshCore/tree/d92964352441e53b93e8667b802e04f6e072b39e).
### Solar P1 Pro hardware and bootloader

[Seeed's hardware specification](https://wiki.seeedstudio.com/meshtastic_solar_node/)
identifies the XIAO nRF52840 Plus, Wio-SX1262, P1 Pro's XIAO L76K GPS and four
3350 mAh batteries. Its
[MeshCore installation guide](https://wiki.seeedstudio.com/get_started_with_meshcore_solar_node/)
selects **Seeed Studio SenseCAP Solar** for both models, explicitly refers to
the P1 Pro USB port and describes the `Xiao-Boot` / `Solar Node` UF2 drive.
There is no separate P1 Pro wiring or firmware target in those instructions.
Use `nrfmast_solar_lua`; no second board alias is needed.

The pinned `variants/sensecap_solar/{variant.h,variant.cpp}` defines:

| Function | Arduino pin | nRF52840 pin |
| --- | --- | --- |
| MCU UART TX / RX | D6 / D7 | P1.11 / P1.12 |
| GPS enable / standby | D18 / D0 | P1.05 / P0.02 |
| Battery ADC / read enable | D16 / D19 | P0.31 / P0.14 |
| QSPI clock / chip select | D21 / D22 | P0.21 / P0.25 |
| QSPI IO0 / IO1 / IO2 / IO3 | D23 / D24 / D25 / D26 | P0.20 / P0.24 / P0.22 / P0.23 |

GPS runs at 9600 baud; external flash is P25Q16H. The board manifest selects
S140 **7.3.0** (firmware ID `0x0123`), with the v7 linker layout and bootloader
settings at `0xFF000`. Seeed documents the shared firmware/USB update path,
but does not specify the installed SoftDevice version or independently list
these numeric pins. Confirm the actual unit's bootloader/SoftDevice during
an authorized maintenance window. GPS cold-start acquisition, antenna operation
and battery calibration still require the physical node.

Seeed's installation procedure includes flash erase. Erasing storage removes
node identities and saved settings: back up the node before following an
installation procedure. No device operation is part of the compile checks below.

### GPS capability inventory

The staged source retains the two GPS providers present in the pin. This
inventory distinguishes source compatibility from an application board profile:

| Board/provider | UTC integration | Build/application support |
| --- | --- | --- |
| SenseCAP Solar P1 Pro; P1 with GPS fitted | UART NMEA, board RTC and Pine trusted UTC | `nrfmast_solar_lua` compiled with Lua. Manufacturer uses the same Solar target for both models. |
| Heltec Tracker | UART NMEA, board RTC and Aspen role clocks | `GPS_tracker_onchip` compiled with Lua and Management. |
| RAK4631 with RAK12500 fitted | Existing SparkFun u-blox I2C driver, fresh valid UTC, board RTC and registered trusted-time callback | Pinned `RAK_4631_repeater` compiled with the UTC hook and SparkFun 2.2.29 (upstream requirement `^2.2.27`). There is no deployable Pine RAK profile here. |
| Other upstream UART GPS definitions | Same NMEA UTC hook, using the board's existing RTC, pins and power controls | Source integration retained; individual board/application builds and physical GPS acquisition are not claimed by the representative checks above. |
| XIAO ESP32-S3/WIO and XIAO nRF52840/WIO | No GPS provider/wiring in these application profiles | Non-GPS Aspen/Pine builds compiled; existing SNTP or authenticated set-time fallback remains available. |
| An added receiver on a board without a GPS definition | No supplied receiver wiring, power control or provider | Unsupported optional hardware until a board definition supplies those connections. `ENV_INCLUDE_GPS` alone does not add hardware support. |
| Birch host role | Host system clock | Shared-modem GPS is not a Linux host time authority. |

The other UART-provider definitions include Heltec Tracker V2, T096/T1/T114,
Tower V2, RC32, V3/V4/V4 R8 and Mesh Solar; LilyGo T-Beam SX1262/SX1276,
T-Beam Supreme/1W, T-Deck, T-Echo/Card/Lite and T-Impulse Plus; Seeed T1000-E,
MeshTracker X1 and Wio Tracker L1; ThinkNode M1/M3/M5/M6/M7/M9; Nano G2 Ultra,
KeepTeen LT1, MeshAdventurer, Meshnology W12, Station G2/G3, ProMicro,
RAK3112/3401/WisMesh Tag, GAT562 EVB Pro/Tracker Pro/30S Mesh Kit and MuziWorks
R1 Neo. Some require an optional receiver, and some individual environments
disable GPS. Preserve the selected environment's GPS flags, UART pins and
sensor dependencies; this list does not add Pine profiles for those boards.

RAK4631 and GAT562 30S Mesh Kit select the common WisBlock I2C detection path
when GPS is enabled. Detection, I2C address, power pins, location scaling,
satellite readings and battery measurement remain upstream behavior. The
adapter checks SparkFun's fresh `getPVT()` result before reading validity and
UTC; a failed poll cannot renew clock trust from cached navigation data.
Valid-date/time and fully-resolved flags must all be set. The year/month are
checked before SparkFun's table-based epoch conversion, then the common UTC
range gate applies. Other RAK-named boards do not automatically select that
I2C path.

## Read the result

On Pine, use the native repeater console:

```text
clock
bot time
stats sensors
```

`bot time` reports `trusted=1 source=gps` and UTC bounds after a fix. Before a
fix, trusted-time work waits for a valid sample; non-GPS time-setting commands
continue to work. The GPS stays active for periodic clock refreshes. An invalid
fix, invalid checksum or cached fix without new UTC sentences cannot refresh
clock trust. Pine's trusted sample expires after one hour; a new fix restores it.
Clock trust is not restored merely from saved deadlines or the RTC after restart.

On Aspen, read `/api/clock`. Enabled role clocks report `source:"gps"` and
`synchronized:true` after synchronization. `gps_epoch` and `gps_age_seconds`
describe the accepted GPS sample; the existing SNTP fields still describe SNTP.
SNTP remains available as a fallback. Shared UART transport uses UART2 on
GPS-equipped ESP32 boards, leaving UART1 for GPS; select separate physical pins.

GPS reception requires a suitable antenna and sky view. A cold receiver may
take time to obtain a fix. Check the board, antenna and GPS power/UART wiring if
the clock remains untrusted; a firmware build alone cannot establish RF/GNSS
reception.

## Build and validate without a device

Stage Pine with its existing `prepare.py` using a **new** output beneath
`firmware/nrf52840/.build`, then select `nrfmast_solar_lua`. Supply the same
verified Lua 5.5.1 source archive used by the production Lua build:

```sh
python3 firmware/nrf52840/prepare.py --source /path/to/pinned-MeshCore \
  --output firmware/nrf52840/.build/solar-check --lua-archive /path/to/lua-5.5.1.tar.gz
pio run -d firmware/nrf52840/.build/solar-check -e nrfmast_solar_lua -t buildprog
```

These commands compile only. They do not flash, format storage, alter identities
or change a running node's radio profile. Use the existing Pine installation
and backup procedures before a separate hardware maintenance window.

`firmware/esp32/tests/test_gps_time.py` runs the actual pinned NMEA location
provider with MicroNMEA and the staged RAK12500 provider against a GNSS API
fixture. It exercises fresh and cached fixes, date/time validity, fully-resolved
UTC, epoch conversion limits, board RTC updates, trusted UTC publication,
unchanged location readings, expiry and reacquisition. The RAK board compile
checks the adapter against the real SparkFun library.

Run the native GPS tests from the application repository root with **git**, a
C++17-capable **c++** compiler and these explicitly configured dependencies:

- `GPS_TEST_UPSTREAM`: an unmodified MeshCore Git checkout at
  `d92964352441e53b93e8667b802e04f6e072b39e`. The tests read provider/role sources
  from that commit and require the working `src` headers to match it. A staged
  application build directory is not a substitute for this checkout.
- `GPS_TEST_NMEA`: the directory containing `MicroNMEA.h` and `MicroNMEA.cpp`,
  normally `<board-build>/.pio/libdeps/<environment>/MicroNMEA/src` after an
  existing pinned-board PlatformIO build has installed its sensor dependencies.

Reuse available dependencies; the test does not clone or download firmware.
Missing paths or a different upstream revision fail with setup instructions,
rather than skipping the checks. Replace these placeholders with local paths:

```sh
GPS_TEST_UPSTREAM=/path/to/pinned-MeshCore \
GPS_TEST_NMEA=/path/to/MicroNMEA/src \
  python3 -m unittest firmware.esp32.tests.test_gps_time -v
```

`clock_network.cpp` separately tests GPS role-clock publication, millis rollover
and SNTP-enabled/disabled fallback.
