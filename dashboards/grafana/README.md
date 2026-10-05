# MeshCore fleet dashboard

Import [`meshcore-fleet.json`](meshcore-fleet.json) to see ESP32 resources,
physical RF traffic, on-device Lua activity, companion-bot health and returned
relay status. Configure a telemetry publisher or bot status poll to supply the
data. The dashboard reads it from VictoriaMetrics.

Import [`meshcore-repeaters.json`](meshcore-repeaters.json) for repeaters polled
over RF by an on-device Lua monitor. Select **Monitor device / Remote repeater**
to see battery voltage, RF sample age, monitor errors, uptime, queues, signal,
packet counters, duplicates, receive errors and airtime. Configure the
[remote repeater monitor](../../firmware/runtime/REMOTE_REPEATERS.md) first.
The fleet dashboard links to this focused view.

## Import and select a node

1. In Grafana, add a **Prometheus** datasource pointing at your authorized
   VictoriaMetrics **query** endpoint. For single-node VictoriaMetrics this is
   the server base URL, not `/write` or `/api/v1/query`. For a cluster or proxy,
   use its operator-provided tenant query base. Set authentication and CA trust
   in the datasource's protected settings, never in this JSON.
2. Set the datasource scrape interval to the actual sample cadence
   (normally `60s` for the ESP32 publisher). Use **Dashboards → New → Import**,
   upload the JSON and select that datasource at the import prompt.
   Grafana 10+ and its built-in Prometheus, text, stat and time-series panels
   are sufficient.
3. Start with the report-age panels. Select **Native device / Native role** for
   the ESP32 feed. Select **Bot mesh / Bot site** for a Python bot, then
   **Polled node**, **Probe route** or **RF work class** for its detail panels.
   These are independent feeds, not an identity join.
4. The default graph range is 24 hours and refresh is one minute. Variable
   choices are discovered from series in the selected time range. Expand the
   range if a stopped device is missing from a dropdown. **History** controls
   report-age and last-reported-uptime lookback independently (default seven
   days).

The native `device` label is a stable hardware ID. The friendly name appears
only on `meshcore_info_schema`; use
`meshcore_info_schema{device="your-device"}` in Explore to look up `name`.
Renames leave historical info series, so check their sample times.

The Python feed uses configured `mesh` and `site` tags as the bot selector.
Assign a unique pair to each bot
with `vm_extra_tags`. Shared or missing tags cannot distinguish multiple bots;
the dashboard cannot recover overwritten aggregates. Without those configured
tags, leave these filters at All (`.*` also matches absent labels).
## What each setup can report

| Feed | Measurements | Configuration |
| --- | --- | --- |
| ESP32 native HTTPS publisher | Hardware, heap/DMA/PSRAM, physical radio, seven role slots, service-wide Lua counters and latest completed VM sample | Enable publishing in an HTTPS image; see [device telemetry](../../firmware/esp32/TELEMETRY.md). |
| Python `meshcore-bot` | Passive traffic, database/runtime state, exporter and RF scheduler health | Configure `Mesh_Metrics_Service` and passive collection. |
| On-device Lua repeater monitor | Native returned status and supported battery voltage in the remote-repeater dashboard | Save read-only ACL entries and same-PHY targets; enable the scheduled program and native HTTPS publisher. A Pine repeater can be a remote target. |
| Python bot status polls | Returned repeater status, neighbour reports, LPP sensors and probes | Configure reachable poll targets and jobs in the bot. This is a separate feed from the on-device monitor. |

Native Lua panels show aggregate service counters, occupied job slots and
the latest worker stack sample. Python panels show the bot's database/WAL
sizes and exporter health. Keep these feed selectors separate when a Python
bot and Go host roles share a radio.

## Read values and gaps

- Report age uses VictoriaMetrics `tlast_over_time()` through the normal
  Prometheus datasource. This is the only MetricsQL-specific function used.
  Native age is receiver receipt age; Python sample age uses original
  collection timestamps retained during retries. Neither is RF last-heard age.
  Last-reported uptime is explicitly a historical sample, not an extrapolation.
- Age colors are green below 180 seconds, yellow to 900 seconds, then red.
  Change these thresholds for your publishing/poll cadence; no sample is
  **No sample**, not zero or healthy. Ages cannot find devices outside History.
  The JSON defines no alerts.
- Ordinary graphs preserve gaps. Grafana's legend shows the last value **in
  the selected range**, which may be old. Check report age before interpreting
  connection, readiness, source state, signal or sensor values as current.
  A publisher disconnected from the receiver cannot report its failure live.
- Only actual monotonic counters use reset-aware `rate()`. Native/source
  counters reset at reboot or source attachment; some wrap at 32 bits.
  Bot counters reset at process restart, and edge-hook counters also reset on
  eviction. Native rate queries use a one-minute minimum sample interval;
  polled status counter queries use five minutes (the default status cadence),
  giving Grafana a rate window of at least four intervals. Adjust target
  **Min step** if your cadence differs. Gaps with fewer than two counter
  samples remain gaps, not zeros.
- `meshcore_packets_total` is a **trailing-window gauge despite its suffix**.
  Its observed packet rate is count/window-seconds, not `rate()`. Message,
  command, graph, population and path-window summaries are gauges too.
- Physical native RX excludes local TX reflection. Native accepted/rejected
  means scheduler admission; succeeded/failed/unknown is a local TX outcome.
  Unknown is uncertain, not failed or confirmed. Role RF counters are subsets
  of the shared radio, not additional traffic. Job outcomes and recorded
  command replies are not peer acknowledgements.
- Native RX airtime is estimated; TX airtime is observed RF duration.
  Heap/DMA pools overlap. Heap minima and worker stack free space are
  low-water gauges. Last-VM fields are the latest completed source load or
  invocation, not maxima or a record of every operation.
- VM phase fractions use default limits: load 330ms, initialization 50ms,
  active invocation 20ms. Cached invokes have zero load/init. Suspended I/O,
  package transfer and RF wait are excluded. Change the denominators if your
  runtime limits change. Cleanup has no invented budget.
- Unsupported battery/PSRAM/temperature fields are absent. WiFi RSSI is
  connected-only. Signal-window means are gated by valid sample count.
  Directed probe-link SNR can remain from a previous successful probe after a
  later failure; inspect the probe outcome and sample time.

## Panel-to-metric map

VictoriaMetrics' default Influx mapping is `measurement_field`. Braces below
denote field alternatives, not literal metric names.

| Dashboard section | Metric names / query semantics | Labels used |
| --- | --- | --- |
| Fleet | `meshcore_device_uptime_seconds`, `meshcore_bot_uptime_seconds`: exact report ages; native last reported uptime. `meshcore_role_{ready,identity_present}`: sampled state. | Native `device`, `role`; bot `mesh`, `site` |
| Native memory/connection | `meshcore_device_heap_{free,minimum,largest}_bytes`, `meshcore_device_dma_{free,minimum,largest}_bytes`, `meshcore_device_psram_{free,minimum,largest,total}_bytes`, `meshcore_device_wifi_{rssi_dbm,connected}`, `meshcore_device_{battery_volts,mcu_temperature_celsius}`: gauges. | `device` |
| Shared radio | `meshcore_radio_{rx_packets,rx_errors,tx_accepted,tx_rejected,tx_succeeded,tx_failed,tx_unknown}_total`: rates; `meshcore_radio_{tx_rf,rx_estimated}_seconds_total`: seconds/second, displayed as percent; `meshcore_radio_{queued,transmitting,carrier_wait,fault,credit_seconds}`: gauges. | `device` |
| Role source | `meshcore_role_tx_{succeeded,failed}_total`, `meshcore_role_tx_rf_seconds_total`: rates; `meshcore_role_{queued,transmitting,source_generation,credit_seconds}`: gauges. | `device`, `role` |
| Lua | `meshcore_lua_jobs_in_use`: gauge; `meshcore_lua_{vm_failures,rejected,busy,sender_limited,channel_limited,global_limited,airtime_limited,events_queued,events_completed,events_failed,events_dropped,replies}_total`: rates; `meshcore_lua_last_vm_{seconds,load_seconds,init_seconds,invoke_seconds,cleanup_seconds,instructions,peak_bytes,free_internal_bytes,free_psram_bytes,stack_free_bytes}`: latest-operation gauges. | `device` (no plugin/bot label) |
| Companion bot | `meshcore_bot_{connected,radio_offline,reconnecting,uptime_seconds,command_queue,capture_queue,capture_queue_capacity,collection_duration_seconds,collection_last_success_timestamp_seconds}`: gauges; `meshcore_bot_{collection_failures,status_skips,edge_counter_evictions}_total`: rates. | `mesh`, `site` |
| Delivery | `meshcore_exporter_{failures,dropped_lines}_total`: rates; `meshcore_exporter_{queue_bytes,oldest_age_seconds,last_success_timestamp_seconds,last_request_duration_seconds}`: gauges. Zero success timestamps are suppressed in age graphs. | `mesh`, `site` |
| RF work | `meshcore_radio_work_{queue_depth,running,oldest_queued_seconds,running_seconds}`: gauges; `meshcore_radio_work_{completed,failed,cancelled}_total`: rates, not packet outcomes. | `mesh`, `site`, `work_class` |
| Passive RF/commands | `meshcore_packets_total / meshcore_packets_window_seconds`; `meshcore_packets_window`: category counts; `meshcore_packets_{snr_db,rssi_dbm}_{min,mean,max}` gated by matching `_samples`; `meshcore_command_{requests,replies,without_reply}`: window gauges. | `mesh`, `site`, `route_type`, `payload_type`, `command`, `kind` |
| Sources/storage/populations | `meshcore_source_ok`, `meshcore_storage_{database,wal}_bytes`, `meshcore_nodes_heard_nodes`, `meshcore_graph_{edges,edges_exported}`, `meshcore_paths_nodes`, `meshcore_neighbours_links`: gauges. | `mesh`, `site`, `source`, `window` |
| Returned node status | `meshcore_node_{uptime,bat,tx_queue_len}`: gauges (`bat / 1000` yields volts); `meshcore_node_{nb_recv,nb_sent,recv_errors,direct_dups,flood_dups,airtime,rx_airtime}`: **unsuffixed monotonic counters**, rates. | `mesh`, `site`, `node`; `role` retained in legends |
| Poll/probe | `meshcore_poll_ok`, `meshcore_probe_ok`: outcome gauges; `meshcore_link_snr`: directed link gauge. | Poll `node`, `kind`; probe/link `route`; links `src`, `dst`; all have `mesh`, `site` |

### On-device remote-repeater dashboard

The native monitor uses `device` (monitor hardware ID) and `peer` (saved alias),
not the Python feed's `node`, `mesh` or `site`. Each HTTPS report includes up to
three peers in rotation. With 12 targets and a 60-second publisher interval,
each peer reports at least every 240 seconds. Polling remains independently
limited to the configured interval, normally 300 seconds per remote repeater.

| Panels | `meshcore_repeater_` fields |
| --- | --- |
| Report age / RF sample age / status | `available`, `fresh`, `sample_age_seconds`, `error_code` |
| Battery / uptime / queue | `battery_volts`, `uptime_seconds`, `queued_packets` |
| Signal / noise | `last_rssi_dbm`, `noise_dbm`, `last_snr_db` |
| Packets and rates | `{rx_packets,tx_packets,rx_flood,rx_direct,tx_flood,tx_direct}_total` |
| Duplicates and errors | `{direct_duplicates,flood_duplicates,rx_errors}_total`, `error_flags` |
| Airtime and fractions | `{tx,rx}_airtime_seconds_total` |
| Monitor attempts / failures and rates | `{attempts,failures}_total` |

These are the native 56-byte status fields returned by the configured
MeshCore 1.17.1 repeaters. Their last RSSI/SNR describe the remote radio's last
received packet, not the monitor-to-repeater link. Additional sensors,
neighbour discovery and firmware-version discovery are not requested.
Future firmware-specific fields need an explicit target capability contract.

RF sample age adds elapsed receiver time to the cached age in the last report.
Report age separately shows whether the monitor is still publishing. Live
status and voltage suppress reports older than 360 seconds; adjust that limit
and the age thresholds if publishing less often. Voltage also requires the
native `fresh` flag. Unsupported voltage is absent, never zero or an estimated
percentage. History graphs preserve past values and gaps.

Error code `12` means **frequency mismatch**: no request is sent and the shared
modem is not retuned. On the current Aspen setup, four 910.525 MHz targets
remain fenced while Aspen stays on 912.525 MHz. The dashboard does not make
those repeaters appear online. The monitor-result panel maps every native
error code; remote `error_flags` are a separate raw firmware field.

Rate targets use a five-minute minimum step, allowing a window of at least
four normal polls. Repeated publication of a cached sample is not additional
RF traffic. Rates of the narrow duplicate counters can be affected by wraps
as well as reboots.

### Other emitted fields inspected

Not every raw series gets a panel. The native complete schema is maintained in
[TELEMETRY.md](../../firmware/esp32/TELEMETRY.md#stable-schema), including
`meshcore_info_schema`, WiFi uptime, PHY configuration and the native
`meshcore_publisher_{attempts,successes,failures,dropped}_total` counters.
Publisher counters describe the preceding request; they are useful in Explore
but cannot establish live receiver reachability.

The external Python emitter also supplies these families/fields:

| Measurement | Other fields and interpretation |
| --- | --- |
| `meshcore_bot` | `consecutive_send_failures` gauge; `probe_skips_total` counter (retained at zero with queued probes). |
| `meshcore_exporter` | `attempts_total`, `sent_lines_total` counters; `queue_batches`, `last_http_status` gauges. |
| `meshcore_radio_work` | `wait_seconds_total`, `run_seconds_total`: cumulative job duration counters. |
| `meshcore_packets` | `with_hash`, `unique_hashes`, `repeat_observations`, `wire_bytes`, `wire_byte_samples`, `malformed_records`, signal `_samples`, `window_seconds`: trailing-window gauges. Wire bytes exclude radio framing. |
| `meshcore_packet_prefix`, `meshcore_packet_hops` | `window` gauges with `bytes_per_hop` or `hops` buckets, including unknown. |
| `meshcore_messages` | `messages`, `senders`, `snr_samples`, `snr_db_mean`, `rssi_samples`, `rssi_dbm_mean`, `hop_samples`, `hops_mean`, `hops_max`, `window_seconds`: window gauges, `kind=dm/channel`. |
| `meshcore_commands` | `requests`, `replies`, `window_seconds`: aggregate command-window gauges. |
| `meshcore_paths`, `meshcore_path` | Aggregate `nodes`, `nodes_exported`, `window_seconds`; per-`pubkey` `variants`, `new_variants`, `max_hops`, `age_seconds`: gauges, not route-change counters. |
| `meshcore_neighbours`, `meshcore_neighbour` | Aggregate `links_exported`; per-`src/dst` `age_seconds`, `observations`, `snr_db_last`, `snr_db_best`, `snr_db_mean`: cached/lifetime-row gauges. Reading them starts no discovery. |
| `meshcore_edge`, `meshcore_edge_observed` | `graph_observations`, `age_seconds`, `hop_position`: stored-summary gauges. Only `meshcore_edge_observed_total` is an accepted-observation-hook counter. `src_kind/dst_kind` distinguish full keys from unresolved prefixes; never merge by a short prefix. |
| `meshcore_heard` | `adverts`, `hops`, `age_seconds`: retained-contact gauges with `node`, `pubkey`, `role`. Contact adverts are not a rateable counter contract. |
| `meshcore_node` | `sent_flood`, `sent_direct`, `recv_flood`, `recv_direct`, `full_evts`: monotonic status counters; `noise_floor`, `last_rssi`, `last_snr`: gauges. Self stats can include additional numeric firmware-returned fields, not a fixed sensor schema. |
| `meshcore_poll`, `meshcore_probe` | `duration_seconds`, probe `hops`, local-poll `airtime_cost` (zero), unresolved-contact `reason_not_found` (one): gauges. The dashboard does not infer remote-poll airtime cost. |
| `meshcore_repeater_neighbours`, `meshcore_repeater_neighbour` | `count`, `returned`; per-`repeater/neighbour` `snr`, `age_seconds`: returned neighbour-table gauges. Optional RF job. |
| `meshcore_telemetry` | Optional LPP `value` gauge with `node`, `pubkey`, `channel`, `sensor`. Sensor names/units come from returned LPP; no unitless mixed-sensor panel is provided. |

## Metric sources and troubleshooting

The native metric schema is defined by `Telemetry.cpp`,
`TelemetryService.cpp` and the [telemetry guide](../../firmware/esp32/TELEMETRY.md).
Python bot series follow
[`agessaman/meshcore-bot`](https://github.com/agessaman/meshcore-bot),
revision `bac3bd868f7d271e36a69e65764c33d5a9fb5b50`, in its metrics and
radio-work service modules.

Check the JSON and native formatter locally:

```sh
python -m json.tool dashboards/grafana/meshcore-fleet.json >/dev/null
python -m json.tool dashboards/grafana/meshcore-repeaters.json >/dev/null
make -C firmware/esp32 telemetry-test
```

In Grafana Explore, query `meshcore_bot_uptime_seconds` or
`meshcore_device_uptime_seconds` to find a feed. Check its labels and selected
time range, then inspect its report age:

```promql
time() - tlast_over_time(meshcore_device_uptime_seconds[7d])
```

Expand the time range to find older samples. Configure the appropriate
publisher or status-poll target for an empty panel. Battery, temperature,
PSRAM and sensor panels depend on the board's available readings.
