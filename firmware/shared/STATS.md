# Node statistics

Use `stats help` in the ESP32 Management console, the nRF repeater
console, or a Go host Relay/Room console. These commands read measurements and
counters without clearing them or changing settings.

```text
stats
stats sensors
stats radio
stats signal
stats airtime
```

`stats` shows shared-radio counters on on-device profiles and role counters on
host roles. `stats help` lists the pages available on that endpoint.

## Measurements by profile

| Hardware/service | Available measurements | Unavailable with these board targets |
| --- | --- | --- |
| Aspen and Cedar: Xiao ESP32-S3 WIO/SX1262 | MCU die temperature; LoRa packet RSSI/SNR and noise floor; WiFi connection/RSSI; internal heap and PSRAM; radio queues, transmission outcomes and airtime; bot and observer counters | Battery voltage: the selected board target has no battery ADC configured. Ambient temperature, humidity, pressure, current and supply power have no configured sensor. |
| Pine: Xiao nRF52/SX1262 | MCU die temperature; BAT-pin voltage through the board's enabled ADC divider; LoRa signal/noise; heap; shared-radio queues, transmission outcomes, airtime and Lua counters | WiFi, PSRAM and environmental sensors are not configured. |
| Birch host roles | Role lifetime/RX/TX counters and available cached measurements from the attached modem | Modem sensors absent from the modem's board target remain unavailable. A host role does not acquire an environmental reading itself. |

MCU temperature is chip temperature, not outside air temperature. Pine's BAT
reading is a voltage measurement, not battery state of charge; the connected
battery or charging circuit determines the voltage present at that pin.

The app's native sensor telemetry uses supported Cayenne LPP types for actual
measurements. Unsupported battery voltage is omitted instead of appearing as
0 V. The fixed native status packet still uses the upstream zero-battery
sentinel; it does not mean a discharged battery. No memory, queue or other
operating statistic is disguised as a physical sensor.

## CLI pages

| Page | Aspen/Cedar Management | Pine repeater | Birch Relay/Room |
| --- | --- | --- | --- |
| `sensors` | Battery availability and MCU temperature | BAT voltage and MCU temperature | Available modem battery/MCU readings |
| `radio` | Shared physical RX/error counters and current queue/transmit state | Same | Cached physical modem RX/TX/error counters |
| `signal` | Last RF packet RSSI/SNR and calibrated noise floor | Same | Current modem RSSI and noise floor |
| `tx` | Confirmed, failed and uncertain transmission counters | Confirmed and unconfirmed transmission counters | Use native status and modem telemetry |
| `airtime` | Confirmed TX RF milliseconds and estimated RX milliseconds | Same | Measured TX/RX milliseconds when the provider supplies them |
| `admission` | Uptime, accepted and rejected transmissions | Uptime, started and rejected transmissions | Use role/modem counters |
| `memory` | Free, minimum and largest allocatable internal-heap block | Free/total heap and sampled minimum | Use host process metrics |
| `psram` | Total, free and minimum free PSRAM | Unavailable | Unavailable |
| `bot` | Active jobs, replies, rejected work and VM failures | Same | Use bot diagnostics |
| `vm` | Last Lua execution's peak allocation, instructions, elapsed time and stack headroom | Same | Use bot diagnostics |
| `observer` | MQTT connection, captured events, drops and publication errors | Unavailable | Use observer service metrics |
| `companion` | Current clients and accepted/rejected/dropped connection counters | Unavailable | Use companion service metrics |
| `role` | Use `role config ROLE` and bot diagnostics | Use native role settings | Role uptime and RX/TX packet counters |

Existing `bot stats`, `bot radio`, `companion stats`, `companion errors` and
`telemetry counts/times/tls` remain available. They give their existing,
service-specific diagnostics. On Birch, the existing `get stats` output is
unchanged; use `stats role` for the versioned fields.

## Output contract

Successful metric pages contain one ASCII line of space-separated `key=value`
fields:

```text
schema=1 scope=device battery_mv=unavailable mcu_temp_c=61.00
schema=1 scope=modem rx_packets=124 rx_errors=0 queued=0 transmitting=0
```

- `schema=1` identifies this field format. Parse by key, not field order.
- `scope` identifies the device, modem, role, bot, observer, companion or last
  VM execution. A Birch modem page describes its attached radio, not its host.
- Names carry units: `_bytes`, `_ms`, `_us`, `_s`, `_mv`, `_c`, `_dbm` and `_db`.
  Counts have no unit. Boolean fields are `0` or `1`.
- An unavailable measurement is the literal `unavailable`. Numeric zero is a
  measurement or counter value, not a substitute for missing data.
- Unknown pages and failed snapshots return `Error: ...`, not empty or stale
  successful data. No packet signal is reported before the first RF reception.
- Counter pages are at most 130 bytes before the native correlation prefix.
  Pages are separate so large counter values still fit encrypted RF replies.
- Device/modem counters reset on device restart; Birch role counters reset on
  role restart. They are not durable history. Multi-page reads occur at
  different times and are not a single atomic snapshot.

`tx_confirmed` means transmission completion was confirmed by the radio,
not that a destination received the packet. Aspen/Cedar keep uncertain outcomes
separate from failed transmissions. Pine's `tx_unconfirmed` includes timeout or
abort outcomes where RF emission cannot be ruled out. An accepted or started
transmission is not a delivery acknowledgement.

Birch's `stats role` TX count is its existing outbound-packet observation
counter, not a physical RF-completion counter. RX airtime on Aspen/Cedar/Pine is
estimated from received packet airtime. Pine's heap minimum is sampled by the
main loop, rather than an allocator-maintained historical minimum.

Observer `connected` reports the MQTT connection. `events` counts captured
events, not broker deliveries; `publish_errors` counts publication failures.
Inspect the private broker's fresh messages when checking reception.

The commands use each endpoint's existing authentication. Use encrypted
Management RF or the authenticated web console for Aspen/Cedar, native
administrator access for Pine/Birch, and the local serial console for Pine.
See [Android endpoint selection](ANDROID.md#choose-the-endpoint-for-app-settings)
and [Management administration](../esp32/MAST_ADMIN.md#administrator-workflow).
