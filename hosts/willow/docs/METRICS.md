# Willow host metrics

Scrape the running Hew dashboard's `GET /metrics` endpoint to retain host
health and traffic history. It uses the same listener as `/status` and `/readyz`:

```sh
curl --fail http://127.0.0.1:9080/metrics
```

The endpoint is public operational state, like `/status`. Keep the listener on
a trusted network. Metrics contain fixed role/source labels and numeric values;
they contain no identities, passwords, message text, endpoint addresses or
owner-command output. Administrative routes retain their authentication.

## Collect and graph

Add one job to an existing Prometheus-compatible collector, using the host's
reachable dashboard address. This example scrapes every 30 seconds with a
five-second timeout:

```yaml
- job_name: meshcore-willow
  scrape_interval: 30s
  scrape_timeout: 5s
  metrics_path: /metrics
  static_configs:
    - targets: ['HOST_LAN_ADDRESS:9080']
```

For VictoriaMetrics, use an existing vmagent and its remote-write connection;
Willow does not start another publisher. Import
[the Willow host dashboard](../../../dashboards/grafana/meshcore-willow-host.json)
and select the collector's Prometheus datasource. Keep `job_name: meshcore-willow`
for these queries. Choose **Willow host** to compare installations. This is a
separate feed from Aspen's ESP32 telemetry and Python bot metrics.

Each scrape shares the dashboard's bounded health refresh with simultaneous
status/readiness requests. A refresh requests six sources concurrently, with a
two-second upstream deadline. Completed results can be reused for 500 ms.
The dashboard performs no health polling while idle. Collector traffic causes
health reads, never an extra modem connection or an RF transmission.

## Read availability and measurements

| Metric family | Meaning |
| --- | --- |
| `willow_health_source_available{source}` | Six fixed sources: host, main companion and bot companion, each with status/readiness. Zero means that response failed or was unavailable. |
| `willow_health_source_age_seconds{source}` | Age of a successful cached response; not RF last-heard age. Failed sources omit this value. |
| `willow_role_health_available{role}` | Status includes that role. Missing role/status emits zero; its measurements are omitted. |
| `willow_role_ready{role}`, `willow_role_running{role}` | Reported readiness and running state. Unavailable readiness is omitted. Bot readiness includes the existing native-worker health boundary; it is not a subprocess restart counter. |
| `willow_radio_connected{role}` | Reported role radio connection. |
| `willow_companion_listener_active{role}` | Reported companion TCP listener state. |
| `willow_observer_mqtt_connected{role}` | Observer broker connection state. |
| `willow_observer_{observed,published,dropped}_total`, `willow_observer_queued`, `willow_observer_pending` | Outbox admissions, broker-PUBACK-confirmed publications, actual drops, queued records and a separate record awaiting PUBACK. QoS1 retries can deliver duplicates. |
| `willow_observer_dirty`, `willow_observer_saving`, `willow_observer_save_error` | Outbox persistence state. Error codes: 0 none, -1 commit failed, -2 rename committed but directory sync failed, -3 restore invalid/unreadable, -4 restoring. |
| `willow_observer_save_failures_total`, `willow_observer_expired_total`, `willow_observer_restored` | Save failures and TTL expirations since this outbox started; restored is a gauge of entries loaded at startup. |
| `willow_role_tx_{accepted,succeeded,failed,rejected,unknown,diagnostic_drops}_total` | Companion TX outcomes since that role started. Admission is not transmission; unknown remains uncertain. |
| `willow_role_tx_reported_airtime_seconds_total` | Confirmed reported RF airtime, converted from nanoseconds. |
| `willow_modem_{received_packets,sent_packets,errors}_total` | Shared physical modem counters, exported once from the repeater health snapshot. Do not add role copies of physical traffic. |
| `willow_modem_{noise_dbm,rssi_dbm,temperature_celsius,battery_volts}` | Supported, valid cached physical measurements. Missing callbacks emit no value. |

An HTTP 200 scrape can contain a failed source: check availability rather than
assuming HTTP success means every role is ready. Healthy sources remain visible
when another source fails. Unavailable measurements are omitted, not reset to
zero. The collector's `up` measures the scrape connection separately. The
Grafana dashboard preserves gaps and hides old measurements while `up` is zero.

Use reset-aware `rate()` only on the `_total` counters. Restarts can reset
counters; queues, connection flags and sensor values are gauges. Physical modem
counters can wrap or restart independently of host roles. State timelines show
sampled changes and may miss transitions between scrapes.

This endpoint does not export process CPU/RSS, native-worker process counters
or Lua/Wasm invocation internals. Use host/process monitoring for resource use;
a dashboard-process measurement would not describe the full Willow stack.
