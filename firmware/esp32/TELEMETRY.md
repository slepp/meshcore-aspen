# Optional device metrics

The ESP32 native HTTPS image can periodically send device metrics to an
InfluxDB-line-protocol receiver such as VictoriaMetrics. Publishing defaults
to **off**, including on an existing installation with no saved telemetry
setting. Runtime configuration survives application updates and restarts.
It does not change role selection, radio settings, identities or MQTT.

Use this guide after [building the HTTPS image](README.md#choose-an-image).
Choose a receiver, approved fixed IP, TLS hostname/CA and any required bearer
token. Then stage through encrypted Management RF, commit, enable publishing
and inspect `telemetry counts` after one configured interval. Expect `ok` to
increase for a completed 2xx response; admission alone is not delivery.
The [network guide](../runtime/NETWORK_API.md) covers bot HTTP/RPC; telemetry has its own
enable setting and does not borrow the bot's grant.

## Configure and check

Use the management identity's authenticated native RF CLI, or the command
transport in `/admin`. Both reach MastAdmin; repeater/room administrator
permissions do not grant this authority. Configuration works over RF with
WiFi disconnected. Delivery requires WiFi and the shared HTTPS worker's
trusted clock and validated endpoint/CA.

**CA and token staging require authenticated encrypted RF.** Plaintext
MastWeb rejects every `telemetry endpoint ca ...` and `token ...` command,
including clear operations. Non-secret endpoint routing, commit/discard,
enable/interval and status remain available through authenticated same-origin
`/admin`; the whole administration surface is not RF-only.

```text
telemetry identity
telemetry endpoint address 192.0.2.10
telemetry endpoint host metrics.example
telemetry endpoint port 443
telemetry endpoint path /write
telemetry endpoint ca clear
telemetry endpoint ca <CA-PEM-BYTES-AS-HEX-IN-CHUNKS>
telemetry endpoint token clear
telemetry endpoint commit
telemetry interval 60
telemetry on
telemetry status
telemetry counts
telemetry times
telemetry tls
telemetry tls-heap
telemetry tls-blocks
telemetry off
```

Replace the example address/hostname and append the CA PEM as repeated `ca HEX`
commands (at most 64 decoded bytes per command; use 56-byte chunks with the RF
client's nonce overhead). A hostname and path are each at most 121 bytes, CA
4096 bytes, optional bearer token 256 bytes. `token HEX` similarly appends
an optional credential; `token clear` stages unauthenticated ingestion.
The settings become active together only on `endpoint commit`.
`endpoint discard` discards staging. `endpoint status` reports configured/
staged and credential presence, never values; `endpoint host|address|port|path`
reads non-secret routing settings. Invalid commits retain the active endpoint;
uncertain persistence disables it until repaired. A commit cancels an older
pending write. Credentials belong in the token field, never in the path.

### Endpoint persistence and upgrades

The endpoint, CA and optional token live in two private SPIFFS files,
`/telemetry-a.bin` and `/telemetry-b.bin`. Each is bounded to 4620 bytes;
allow 9240 bytes plus filesystem overhead for updates. As with command-source
deployment, an update writes the inactive slot, flushes/closes it, and verifies
its size, contents and SHA-256 before publishing a small NVS reference.
The reference and selected file are read back before the endpoint becomes
usable. A restart reads only the referenced slot; it never selects an
uncommitted file or falls back to stale credentials.

An existing NVS `mc-onchip/telemetry-peer` endpoint migrates automatically on
first endpoint access, including startup, without reprovisioning or changing
its address, CA, token or enable/interval settings. Only after the SPIFFS copy
has passed readback does a 40-byte reference replace that same NVS key.
A failure before reference replacement retains the legacy blob. An uncertain
reference commit disables the live endpoint; restart validates whichever
record actually persisted. No filesystem formatting, identity changes or
old identity-bound bot-data deletion is involved. Downgrading to firmware that
only understands the legacy endpoint blob requires endpoint reprovisioning.

The ESP32's 20 KiB NVS has 630 entries, with a 126-entry garbage-collection
reserve. The old fixed 4620-byte blob consumed at least 148 entries even with a
short CA and no token (149 if it spans three NVS chunks). The reference uses
four entries, returning at least 144 entries: the reported 166-free-entry
deployment becomes at least 310 free, enough for the
unchanged 308-entry public Lua storage admission floor. Maximum-size endpoint
updates need only another four NVS entries while replacing the reference;
CA/token growth no longer competes with Lua data for NVS space. Other stored
data still consumes this shared capacity, so further writes can legitimately
reach the safety floor.

`telemetry endpoint status` reports `storage=error` and `configured=0` when
migration, commit or file verification fails. Publishing stays unavailable.
Repair the storage fault, then restart to retry a retained legacy migration,
or restage the endpoint and commit it through the existing management commands.
Do not erase NVS or format SPIFFS to make room: both contain other role data.

The address is a provisioned IPv4 literal, while `host` supplies TLS SNI and
certificate hostname verification. This reuses the existing HTTPS transport's
bounded-connect policy instead of adding blocking ESP32 DNS lookups.
Reprovision the address if DNS moves the receiver. HTTP, insecure TLS, alternate
certificate-verification bypasses, redirects and public Lua access are not
supported. This native POST feature does not enable general Lua HTTP/JSON APIs.

The interval is 30..86400 seconds, default 60. `on` also clears a suspended
HTTP error after the endpoint has been repaired. `off` cancels an outstanding
write; it cannot retract a request already received by the server. Persistence
errors disable live publishing and explicitly report that the saved outcome
is unknown. Inspect the setting again after repairing storage.

## Delivery and privacy

The publisher uses the existing in-process RadioDashboard totals and native
hardware/role/Lua statistics, not HTTP scraping or a second packet observer.
It emits one bounded batch (at most 6144 bytes) per period. There is no flash
history, unbounded queue or replay of failed samples. If WiFi, clock, endpoint
or admission is unavailable, that sample is dropped; the next period captures
current values. Counter differences still describe activity during a gap.
An outstanding request retains its worker slot until completion/cancellation
is acknowledged. Disable frees the sampling workspace after that acknowledgment.
The worker checks one lower-priority telemetry slot after at most one
interactive RPC, rotating the RPC cursor. A request has at most 15 seconds
waiting behind the current RPC, then at most 15 seconds of native HTTP work.
Cancellation is checked throughout writes/reads; a TLS connect may finish its
existing bounded SDK connect/handshake before cancellation is acknowledged.
There is no second TLS task/socket. The same worker can start in network-only
mode without a command-bot identity or enabled Lua role. Telemetry never
borrows `bot home` permission.

Only the shared native HTTPS worker performs networking. Telemetry does not
open another socket or send through Lua. Requests validate the CA, hostname
and certificate dates, use `POST` with `Content-Type: text/plain`, and do not
follow redirects. No Influx timestamp is supplied: VictoriaMetrics uses
**server receipt time**. `uptime_seconds` describes when the snapshot was
captured; receipt time is not presented as a trusted device wall-clock sample.

`telemetry counts` reports admissions (`attempts`), completed 2xx responses
(`ok`), failed admitted requests (`failed`), and samples discarded before
admission or by cancellation (`dropped`). These categories are not disjoint:
a timed-out request can be both failed and discarded. An admission is not
proof that a socket connected or any bytes reached the server.
`telemetry times` reports monotonic milliseconds since boot, zero meaning
no such event, not UTC. HTTP status and a finite error category are exposed;
server response bodies and credential values are not.
If a request completed with a valid 2xx before cancellation was observed, that
acknowledgment is still counted; disable does not rewrite delivery history.

Transport errors, HTTP 408/429 and 5xx back off exponentially from 60 seconds
to an hour (never faster than the configured interval). Each subsequent
attempt is a **new sample**, not a retry of the old body. Other 3xx/4xx
responses suspend delivery until an explicit `telemetry on`/configuration
change. A successful 2xx response clears backoff. An HTTP response alone
does not prove retention/queryability under a server's particular ingestion
configuration.

### A write fails without an HTTP response

Read the last completed worker attempt without opening a serial connection:

```sh
python3 tools/hardware/admin.py --web http://MAST \
  --password-file OWNER_FILE command 'telemetry tls'
python3 tools/hardware/admin.py --web http://MAST \
  --password-file OWNER_FILE command 'telemetry tls-heap'
python3 tools/hardware/admin.py --web http://MAST \
  --password-file OWNER_FILE command 'telemetry tls-blocks'
```

These read-only commands also work in an existing `/admin` session or through
authenticated RF. `tls` reports the attempt number, a finite `detail`, the
signed numeric SDK result, negotiated TLS cipher-suite ID, and peer-chain
certificate count/DER bytes (`peer=count/bytes`, bounded to eight/16 KiB).
`cached=1` means this request reused the configured CA date window. Zero-valued
heap snapshots or cipher/chain counts mean the phase was not reached or unavailable.
No endpoint, CA, token, certificate contents or remote error text is returned.
The existing `error=heap`/`error=transport` categories and retry policy remain
unchanged. `detail=none` means no local TLS diagnostic was recorded, not that
HTTP delivery succeeded; check `telemetry status` and `counts`.

| `detail` | Next check |
| --- | --- |
| `admission-total` | Available memory stayed below the TLS work, client or radio reserve budget for a second; check PSRAM and internal free memory. |
| `admission-block` | A TLS record buffer or the internal client context could not fit a contiguous block. |
| `admission-input`, `admission-output` | The first or second simultaneous record-buffer probe failed. |
| `admission-reserve` | Both probes fit, but their allocation overhead left insufficient remaining TLS work memory. |
| `client` | The client allocation failed. |
| `connect` | Use `sdk` to distinguish an SDK allocation failure from handshake, verification or connection failure. |
| `ca-parse`, `peer-certificate` | The configured CA or received chain failed parsing/date-window checks; a negative CA parse `sdk` can identify allocation failure. |
| `clock`, `certificate-time` | Repair SNTP availability or the certificates' validity period. |
| `connected-reserve`, `io-reserve` | Internal free memory fell below the radio reserve after connecting or during HTTP I/O. |

`tls-heap` reports internal free bytes before the attempt, with the pre-connect
parsed CA live (`ca`, zero on a cache hit), after successful SDK connect, at the failure decision, and after
client destruction. `tls-blocks` reports largest internal blocks before,
at failure, and after, plus **boot-wide** internal low-water marks before/after.
These are phase snapshots, not continuous peak measurements or allocation-owner
traces. The SDK can free failed-handshake state before returning; a recovered
`failure` value does not rule out an SDK allocation failure. Probes and concurrent
RF/WiFi/dashboard work can lower the global minimum. No new low-water drop does
not mean this attempt used no memory.

The diagnostic belongs to the `tls attempt` number, remains available during
backoff/pending requests and dropped samples, and is replaced only when the worker
acknowledges another telemetry completion. Read `tls` again after the two heap
commands if an attempt might have finished between them. It uses bounded RAM,
not NVS or SPIFFS history, and requires no new socket, task or HTTP listener.

`error=heap http=0` identifies local memory admission/reserve failure separately
from `error=transport`. Native home RPC reports `heap_unavailable` for the same
condition. The shared transport budgets **68 KiB of PSRAM for TLS,
8 KiB of internal memory for client/socket work and a further 32 KiB internal
radio/network reserve**. It probes the SDK's two simultaneous
record-buffer allocations, using `MBEDTLS_SSL_IN_BUFFER_LEN` and
`MBEDTLS_SSL_OUT_BUFFER_LEN` directly (16,717 bytes requested each, rounded to
16,720 by the pinned allocator) in PSRAM. With both probes live, free PSRAM must
cover the remaining TLS work budget, including allocator overhead.
Internal memory must also have a contiguous block for the SDK client context.
Both probes are released before creating the TLS client.
On a CA cache miss, admission is checked again after the date parse is freed;
both admission checks share the same one-second waiting limit.
Heap low-water metrics include these short allocation probes.
It allows up to one second for temporary pressure to clear.
The transport also checks the internal radio reserve
after connect and during request/response validation, closing on a shortfall.
The normal backoff applies; no failed payload is replayed.

The pinned Arduino ESP32 2.0.17 / ESP-IDF 4.4.7 SDK allocates **two independent
16,720-byte record buffers** and a **2,208-byte client context**, observed in
the on-target HTTPS phase measurements. Its
[record buffer definitions](https://github.com/espressif/mbedtls/blob/2b8e772fc1cb0732cda3bae7d1e9d6f4cfaf63d9/include/mbedtls/ssl_internal.h)
add protocol overhead to 16 KiB content.
The aggregate budget is an admission estimate. Use the per-attempt diagnostics
to distinguish a probe refusal, SDK allocation failure and a
post-connect reserve shortfall. SDK buffer/context size assertions require
review if a future SDK changes their bounds.

Admission cannot guarantee every later SDK allocation under concurrent load.
Actual SDK allocation errors, including nested MPI/ASN.1 allocation failures
and configured-CA parsing allocation failures, report `heap`/`heap_unavailable`
and close the client. Other connection/verification failures remain transport
errors; native RPC diagnostics retain the SDK error code and description.

Before starting bot workers, the runtime calls the SDK's
`mbedtls_platform_set_calloc_free` to select
`heap_caps_calloc(..., MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)` and `heap_caps_free`.
The allocation policy is process-wide: every SDK Mbed TLS client uses PSRAM.
The free hook accepts both PSRAM blocks and internal blocks allocated before
startup. Allocation failures remain explicit SDK errors.
Set `ONCHIP_TLS_PSRAM=0` at build time to retain the SDK's compiled
[internal allocator](https://github.com/espressif/esp-idf/blob/v4.4.7/components/mbedtls/port/esp_mem.c).
That mode requires 68 KiB of TLS work plus the 32 KiB radio/network reserve
in internal memory and performs the record probes there.

The Arduino
[client](https://github.com/espressif/arduino-esp32/blob/2.0.17/libraries/WiFiClientSecure/src/ssl_client.cpp)
parses its CA before `mbedtls_ssl_setup`, handshakes, then frees that parsed CA.
Native date validation parses and frees the CA **before creating any
client/context**. A single 4,120-byte PSRAM
cache retains only the exact PEM bytes and two date bounds, never SDK
certificate/key allocations. Matching bytes reuse the date window; changing
even one byte requires a fresh parse. A failed PSRAM cache allocation simply
uses an uncached pre-connect parse. The cache is scrubbed and freed with the
shared worker transport. Alternating home/telemetry CAs share that one cache.

The SDK still verifies each new connection's CA, hostname and chain, while the
native provider checks the cached anchor dates against fresh SNTP before connect
and the combined anchor/peer window throughout HTTP I/O. Cached dates never
cache a successful time-trust decision. Immediately after SDK connect, the
internal radio reserve is checked before any HTTP write.

Telemetry sample/body and endpoint copies also use PSRAM. The admission check
opens no socket and retains no work buffers between requests. The TLS context
and records are closed/freed per request; no second live context is created.
The NVS public-storage admission floor and garbage-collection reserve remain
308 and 126 entries respectively.

Compare the device's current memory statistics with its last published
`meshcore_device_heap_free_bytes` and `meshcore_device_heap_largest_bytes`.
The dashboard also reports DMA-capable free/largest blocks. Check native
HTTPS phase logs when an already-running, non-resetting capture is available.
The largest-free-block figure is not an allocation size or an allocation-owner
trace. Recorded post-close heap counts can return close to baseline while small
retained allocations still split the free space. The current network status
cannot identify those allocations' owners; do not infer a TLS buffer leak or
blame NVS from that figure alone. The allocation-fit check works with such
fragmentation rather than waiting indefinitely for an unnecessary 64 KiB block.

`make -C firmware/esp32 bot-https-device-test` exercises repeated telemetry
writes through the production SecureTransport with an SDK boundary shim. It
checks successful repeated POSTs with the fragmented layout
`{53236,16000,16000,16000,10528}`, rejection when two actual record buffers
cannot fit despite sufficient aggregate memory,
the exact radio-reserve boundary including allocation overhead, partial
allocation cleanup, local heap versus transport errors, failed-connect cleanup,
and repeated close. It also checks that the extra CA date parse cannot overlap a
client/context, cache invalidation after in-place CA changes, expiry with cached
dates, malformed/cyclic CA chains and uncached operation when PSRAM cache
allocation fails. A synthetic internal-allocation workload checks that separating
CA parsing from live TLS buffers avoids a reserve breach; those fixture sizes
are not measurements of the receiver's handshake.
It makes no network requests or device changes.

## Stable schema

Every measurement has `device=esp32-<12 lowercase hex eFuse MAC digits>`.
This hardware identifier survives friendly-name changes and MeshCore
identity-key rotation. There is no per-boot label. Only `meshcore_info` adds
the dashboard's friendly `name`, read from the persisted `role name kiss` setting
rather than the compiled WiFi hostname. Renames create a new info series, not new
radio/memory/counter series. `meshcore_role` adds one of seven fixed `role`
labels: repeater, room, companion, observer, bot, management, command-bot.
The bot slot describes the external KISS bot endpoint; command-bot describes
the on-device Lua service.

There are no sender, packet, exception-message, SSID, contact, private-message,
public-key, password or credential labels/fields. All fields are numeric;
VictoriaMetrics maps a measurement and field to `measurement_field`.

| Measurement | Fields and semantics |
| --- | --- |
| `meshcore_info` | `schema=1`, informational gauge |
| `meshcore_device` | `uptime_seconds`; `heap_{free,minimum,largest}_bytes`; `dma_{free,minimum,largest}_bytes`; `wifi_connected` (0/1); connected-only `wifi_rssi_dbm`, `wifi_uptime_seconds`; supported-only `psram_{total,free,minimum,largest}_bytes`, `battery_volts`, `mcu_temperature_celsius` |
| `meshcore_radio` | `rx_packets_total`, `rx_errors_total`, `rx_estimated_seconds_total`; `tx_{accepted,rejected,succeeded,failed,unknown}_total`, `tx_rf_seconds_total`; gauges `queued`, `transmitting`, `carrier_wait`, `fault`, `credit_seconds`; configured-only `frequency_hz`, `bandwidth_hz`, `spreading_factor`, `coding_rate` (denominator), `tx_power_dbm` |
| `meshcore_role` | Gauges `ready`, `identity_present`; when the source is available: `source_generation`, `queued`, `transmitting`, `credit_seconds`; source `tx_rf_seconds_total`, `tx_succeeded_total`, `tx_failed_total` |
| `meshcore_lua` | `jobs_in_use` gauge (occupied native job slots, including completions awaiting dispatch); `rejected_total`, `vm_failures_total`, `replies_total`, `busy_total`, `{sender,channel,global,airtime}_limited_total`, `events_{queued,dropped,failed,completed}_total`; after a VM sample: `last_vm_peak_bytes`, `last_vm_instructions`, `last_vm_seconds`, `last_vm_{load,init,invoke,cleanup}_seconds`, `last_vm_free_internal_bytes`, `last_vm_free_psram_bytes`, `last_vm_stack_free_bytes` |
| `meshcore_publisher` | `attempts_total`, `successes_total`, `failures_total`, `dropped_total`, sampled before the current batch is submitted |

Suffix `_total` means a cumulative counter; other fields are gauges. Heap
minimums and VM stack free space are low-water gauges. Last-VM fields describe
the latest completed VM operation (source load or invocation), not aggregate
heap use or running jobs. Phase fields use existing VM wall-time accounting:
330 ms load, 50 ms initialization and 20 ms active-invocation default limits.
Cached invocations have zero load/init work. Suspended native I/O, package
transfer and RF scheduling/delivery are not active VM execution or task CPU
time. A later completed operation replaces the last sample; these fields are
not phase maxima or a history of every initialization.
`coding_rate=5` means 4/5. RX airtime is an estimate; TX RF airtime is the
shared scheduler's observed duration, not acknowledged delivery.

Radio totals come exclusively from the physical shared-radio dashboard.
Local TX reflection is **not physical RX**. Accepted/rejected describe
scheduler admission; failed/unknown describe local physical transmit
outcomes, not messages received by peers. Per-role source counters are
subsets of the same radio activity: do not add them to physical totals.
Unattributable host-client transmissions remain in physical totals.

All counters reset on reboot. Radio dashboard totals are unsigned 64-bit;
driver RX errors, role source counters/airtime and Lua counters are unsigned
32-bit and wrap. Role source counters also reset on source reattachment;
`source_generation` identifies this as a gauge, not a timeseries label.
Use reset-aware `rate()`/`increase()` with suitable windows and inspect uptime
and source generation for gaps/resets. VictoriaMetrics stores numeric samples
as floating point; extremely large integer counters lose exact unit precision.

An unsupported sensor is **absent**, never a synthetic zero. The pinned XIAO
S3 WIO board has no `PIN_VBAT_READ` battery measurement and therefore omits
battery voltage. Its native ESP32 MCU-temperature API is used as-is; it is die
temperature, not ambient temperature, and is quantized by the upstream API.
Nonfinite temperatures are omitted. WiFi RSSI is omitted while disconnected.
PSRAM fields appear only when real PSRAM is detected. No humidity, RF
temperature or invented battery percentage is published.

For example, under VictoriaMetrics' default Influx mapping:

```promql
rate(meshcore_radio_rx_packets_total{device="esp32-<hardware-id>"}[5m])
meshcore_device_heap_free_bytes{device="esp32-<hardware-id>"}
rate(meshcore_radio_tx_rf_seconds_total{device="esp32-<hardware-id>"}[5m])
```

For fleet views, import the
[Grafana dashboard](../../dashboards/grafana/README.md) using a normal
Prometheus datasource pointed at your VictoriaMetrics query endpoint.
It groups native device, radio, role and Lua metrics alongside the optional
Python companion-bot feed. Report-age panels distinguish retained history
from current reporting; host and Pine coverage is described in the import guide.

## Endpoint and verification

VictoriaMetrics documents `/write` and `/influx/write` on single-node/vmagent
receivers, and a tenant-prefixed path on a cluster. Do not infer cluster
paths from the hostname. Configure the exact path for your receiver/proxy,
its certificate trust and any authentication using the native runtime commands
above. Do not put tokens or passwords in the path.

After one interval, read `telemetry counts`, `telemetry times` and
`telemetry tls` through authenticated management. A 2xx response confirms
that request completed; server receipt time and device snapshot uptime are
different clocks. Turning publishing off does not erase samples already held
by the receiver. Missing WiFi/time/admission skips samples without replay.

### Local checks without a device

```sh
make -C firmware/esp32 telemetry-test
make -C firmware/esp32 telemetry-storage-test
make -C firmware/esp32 telemetry-https-test
```

These fixtures cover encoding, verified storage and the shared TLS transport.
They do not flash or enable a radio.

### Check an authorized receiver

```sh
make -C firmware/esp32 telemetry-endpoint-test \
  TELEMETRY_WRITE_URL=https://metrics.example/write
```

**This sends a sample to the specified receiver.** It writes one bounded
`device=host-protocol-fixture` sample, validates certificates and rejects
redirects. It checks the host-side protocol, not physical ESP32 publishing.
Use only a receiver you administer and expect to receive test data.

References:
[VictoriaMetrics Influx ingestion and mapping](https://docs.victoriametrics.com/victoriametrics/integrations/influxdb/),
[Influx line protocol](https://docs.influxdata.com/influxdb/v2/reference/syntax/line-protocol/).
