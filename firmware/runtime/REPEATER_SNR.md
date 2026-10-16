# Nearby repeater signal

Aspen can keep the last 256 received SNR and RSSI measurements for nearby
repeater hops in PSRAM. The dashboard shows average SNR, nearest-rank P95,
average RSSI, sample count, total heard and last-heard age. `!snr` lists the
three most frequently heard reported hops; `!rssi` is an alias that shows the
same SNR and RSSI measurements together.

The example uses the shared report text formatter and bounded decimal helper;
use firmware that supports `report:format(...)` and `tostring(number, places)`.

Use any case-insensitive hex prefix of one to six digits: `!snr d1` can match
`D12600`. A single match shows its measurements and last-heard age. Multiple
matches are all included when their compact measurements fit the reply;
otherwise the bot asks for a longer prefix. Matching searches the reported
hops, including their original one-, two- or three-byte labels.

The Wasm packet program owns the sample rings and calculations. Command Lua,
the dashboard and telemetry read its numeric report; they do not keep separate
sample histories. No polling or extra RF transmissions are needed to sample.

## Install on Aspen

Use firmware whose `packet api` response includes `report=1` and capability
`8`. Check both packet slots first and export an occupied slot before replacing
it. The example command reads slot 0; change `node.packet_report(0,hop)` if using
slot 1. These commands assume slot 0 is unused:

```sh
make -C firmware/esp32 bot-wasm-examples
python3 tools/admin.py --web http://DEVICE --password-file /private/node-password packet install 0 .tmp/wasm-examples/repeater-snr.wasm
python3 tools/admin.py --web http://DEVICE --password-file /private/node-password packet budget 0 1 100000 20000 9
python3 tools/admin.py --web http://DEVICE --password-file /private/node-password packet enable 0 on
python3 tools/admin.py --web http://DEVICE --password-file /private/node-password source install snr firmware/runtime/plugins/examples/snr.lua
```

`source install` preserves the other named command sources. Read `packet 0
status`, `stats` and `timing` after traffic arrives. The program must remain
`live=1` without faults, and `/api/status` should include `repeater_snr` under
`packet_reports`. Disable it with `packet enable 0 off` to stop sampling.

## What the measurements mean

Only raw RF receive events with a nonempty flood path are sampled, once before
role fanout. The last traversed hop is the transmitter heard by this radio.
Normal and transport flood paths with one-, two- or three-byte prefixes are
supported. Direct routes contain remaining destinations rather than a reliable
transmitter identity, so direct routes and empty paths are excluded. Local
reflection and generated packets are excluded too.

Prefixes retain their width: `D1` and `D12600` are separate buckets. Prefixes can
collide and do not authenticate a repeater identity. Packets from a moving
forwarder can enter a bucket; the radio cannot determine whether it is stationary.

The program retains 24 hops and reports the 16 heard most often. When full, a
new hop replaces the least frequently heard retained hop, breaking ties by age.
Each ring holds 256 measurements; averages and P95 use only those measurements.
Heard totals count observations since that bucket was created. Higher SNR is
better; P95 describes the stronger end of the observed distribution.

A hop returning after an hour without a sample starts a fresh ring. Stale hops
are removed opportunistically when attributable packets arrive; a quiet radio
keeps its final report. Always check last-heard age. Restarting Aspen or reloading
the packet program clears all rings. The guest uses one 64 KiB PSRAM linear
memory, including rings, histogram and report storage.

## Grafana and VictoriaMetrics

Import the [nearby repeater signal dashboard](../../dashboards/grafana/README.md)
to compare paired SNR/RSSI rings, counts and RF last-heard age. Report hold
controls how long an exported ring remains visible without another upload.

With existing telemetry enabled, `meshcore_packet` sends all complete rows that
fit the existing 8192-byte upload after device and remote-repeater metrics,
tagged by `device`, `slot`, `report` and `label`. A full 16-hop signal report can
fit one upload; normal device and remote-repeater metrics typically leave room
to export it in one or two uploads. Larger combined reports continue in the
next accepted batch.
Failed uploads retry the same position with a fresh snapshot. Older firmware
sends one row per upload, taking up to 16 minutes at a 60-second cadence; the
dashboard's default 20-minute hold also covers those receivers. The fields become:

- `meshcore_packet_mean_db`, `meshcore_packet_p95_db`, `meshcore_packet_rssi_mean_dbm`
- `meshcore_packet_samples`, `meshcore_packet_window`, `meshcore_packet_heard_total`
- `meshcore_packet_last_seen_ms`, `meshcore_packet_last_seen_utc`

Filter by `report="repeater_snr"` and `label="D12600"`. Upload time is the metric
timestamp; `last_seen_utc` is the last RF observation time when the radio's clock
was trusted, otherwise zero. Use it to distinguish a quiet link from fresh data.

## Local checks

`make -C firmware/esp32 packet-lua-test` runs the real Wasm sampler against known
quarter-dB samples, ring wrap, path widths, 24 retained hops and the shared
`!snr`/`!rssi` prefix matching and reply bounds. It also checks report rollback,
capability and numeric bounds.
`make -C firmware/esp32 telemetry-test` checks complete-row batches, acknowledgement
rotation, retry positions and bounded encoding.
These checks do not transmit or change a connected radio.
