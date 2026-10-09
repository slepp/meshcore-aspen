# ESP32 radio dashboard

The WiFi KISS firmware serves a self-contained operator page at `http://DEVICE/`
on port 80. Live packet activity, signal levels, transmit allowance and
connected clients update over WebSocket. The page and status API are read-only
and unauthenticated; keep this unencrypted HTTP service on a trusted network.

Build-time settings are `KISS_HTTP_PORT` (default `80`) and
`KISS_DASHBOARD_NAME` (default `"MeshCore Radio"`, at most 63 ASCII bytes).
On the combined on-device mast, the saved KISS service name supplies the
heading and browser title instead. `role name kiss Aspen` updates that name
without rebuilding or restarting; the next status or live update shows it.
This changes neither an RF identity nor the DHCP/mDNS hostname.
There are no fixed IP addresses or device-specific identifiers in the page.
SSID, WiFi credentials, private identities, stored keys and client IP addresses
are not exported. Packet history contains only length, timing, outcome,
signal metadata and the first 16 raw RF bytes as hex.

Recent activity decodes that preview into compact lines: packet type, routing
mode, payload version and encryption; path prefixes; and visible envelope
fields. Transport codes appear in the header when present. `Path` shows
traversed flood hops; `Route` shows remaining direct hops, each with the hop
count and prefix width. Direct trace packets
carry signal bytes in the path field instead of contact prefixes.
Private request, response, text and returned-path envelopes show visible
source/destination prefixes; group envelopes show a channel hash. Encrypted
content is not decoded. A long path can fill the preview before any payload
prefix arrives, and an advert's name is beyond this 16-byte preview.

On-device ESP32 snapshots also expose up to 32 native companion contact records:
public key, name and advert type only. These are the companion's current
received/stored contacts, not names inferred from partial adverts. Contact
names take precedence over on-device role names for the same key. Multiple
matching keys stay ambiguous; a match within this list does not authenticate
the packet or exclude collisions with other mesh nodes. When contacts are
omitted because the list exceeds 32, a single visible name is explicitly
marked with `?` after the name. Unknown prefixes remain hex; collisions show
`[N matches]`. Contact types and repeated lookup caveats are omitted from the
packet rows. The expandable **Decode key** contains the notation and exported
contact count. Packet hex uses `...` for a truncated preview; the decode names
any field whose bytes are missing instead of repeating a generic preview warning.

Contacts are omitted when the companion is unavailable or restarting. Older
firmware falls back to on-device role names; standalone shared modems show hex
prefixes without names. Enumeration runs on the existing radio dispatch task;
HTTP reads only the published snapshot. No contact endpoint, extra radio
observer, channel keys, shared secrets or message content is added.

RX rows mean reception over RF; TX rows retain their queued-transmission
outcome, including unconfirmed transmissions. Local reflections do not enter
this history. RSSI/SNR are displayed only for RF reception. Activity ages use
whole seconds, then whole minutes, hours and days, updated by the existing
live snapshots. Age-only updates keep the same table rows; API timestamps,
packet bytes and newest-first ordering are unchanged.

Read the running firmware profile from `GET /api/status`:

```sh
curl --fail --silent --show-error http://DEVICE/api/status | jq -r .firmware_version
```

A standalone shared modem reports `birch-0.1.0-rc.1`; the on-device ESP32
profile reports `aspen-0.1.0-rc.1`, independently of saved service names.
This field is also present in live snapshots. `upstream_tag` and
`upstream_commit` expose the full MeshCore base identity. Legacy KISS `GET_VERSION`
remains the two-byte protocol version `1.0`, and its device-name response
remains the board manufacturer name.

HTTP requires an already configured WiFi connection. A credential-free
UART modem does not gain WiFi first-boot provisioning from this status field.

## Hostname and discovery

The default DHCP hostname is `meshcore-radio`. The bundled ESPmDNS responder
announces `meshcore-radio.local`, `_http._tcp` on port 80 and `_kiss._tcp`
on port 8001. Port overrides use the same `KISS_HTTP_PORT` and `KISS_TCP_PORT`
definitions as the servers, not separate discovery settings. HTTP is only
announced when the HTTP server starts successfully.

Hostname selection is explicit: `KISS_HOSTNAME`, then an existing
`WIFI_HOSTNAME`, then an existing `HOSTNAME`, otherwise `"meshcore-radio"`.
The default never overrides those build flags. The human-readable page title
is independent: the mast uses its saved KISS name, while radio-only builds use
`KISS_DASHBOARD_NAME`.

For additional radios, set a unique bare label in their build configuration:

```ini
  -D KISS_HOSTNAME='"meshcore-secondary"'
```

This gives `http://meshcore-secondary.local/` and
`meshcore-secondary.local:8001`. Labels must be 1-31 ASCII letters/digits or
interior hyphens, without `.local`. Invalid values fail compilation rather
than being silently truncated by Arduino's 32-byte hostname buffer.
Do not rely on automatic mDNS conflict renaming for multiple radios.

The responder starts during setup and handles subsequent address changes in
its SDK task; the RF loop does not poll or restart it. Discovery failures are
logged without stopping radio service. `.local` requires local multicast and
working mDNS resolution on the client OS. Hosts without mDNS support, and
routed/VLAN-separated deployments, should use their configured DNS/DHCP name
or an operator-supplied address instead.

## Build integration

The dashboard, multiplexer and queued protocol sources live in
`firmware/shared/`. From the repository root, `make firmware-prepare` copies
these files alongside `WifiKissMultiplexer.*` and `QueuedTxProtocol.h`
into `.tmp/MeshCore/examples/kiss_modem/`:

- `RadioDashboard.h`
- `RadioDashboard.cpp`
- `RadioDashboardPage.h`
- `RadioNetwork.h`
- `RadioFirmwareIdentity.h`

On-device builds also prepare `onchip/FirmwareIdentity.h`; the radio identity
header uses that existing profile definition rather than the standalone
modem version.

`firmware/esp32/wifi_kiss_main.cpp` replaces the generated upstream
`examples/kiss_modem/main.cpp`. The ESP32 Arduino SDK supplies
`esp_http_server` and `ESPmDNS`. The root `make firmware` target copies these
files during preparation.

## RF isolation and bounds

The radio loop alone records observations into a fixed 32-event ring and
60 one-second traffic buckets. Its observer path performs no allocation,
JSON formatting, HTTP calls or waiting. Every 500 ms it attempts to publish
a fixed-size snapshot. A lock-free atomic flag protects only a short memory
copy: if the HTTP reader is copying, the publisher skips that publication
instead of waiting. The HTTP reader releases the flag before formatting or
network I/O. A skipped-publication counter is visible in the API.

The ESP HTTP server runs in its own priority-1 task on core 0, separate from
the current board's Arduino/radio loop on core 1. It has an 8 KiB task stack,
three HTTP client sockets, a two-connection backlog, LRU eviction and
two-second send/receive timeouts for ordinary HTTP requests. At most two
sockets can subscribe to live updates, leaving a third for the page or
diagnostics. JSON uses two fixed buffers (24 KiB each for a shared modem,
40 KiB each for on-device ESP32 contact snapshots): one diagnostics response
and one shared live message, not a copy per subscriber.
Contact storage and the larger JSON buffers require both `MESHCORE_ONCHIP` and
the existing `ESP32` build flag, with `NRF52_PLATFORM` excluded. The ESP32
PlatformIO toolchain and ESP32 host-native lifecycle tests already set `ESP32`;
the focused contact test sets it explicitly. General on-device, host and Pine
builds retain the original snapshot layout and 24 KiB JSON capacity.

Measured sizes on the native 64-bit test ABI, compared with `7c2e4c8` before the
contact projection (six roles; ESP HTTP uses the SDK seam):

| Profile | `RadioStatus` bytes | `Snapshot` bytes | `RadioDashboard` bytes | JSON buffer bytes |
| --- | ---: | ---: | ---: | ---: |
| General, unchanged | 312 | 4176 | 39968 | 24576 |
| General on-device / Pine with `MESHCORE_ONCHIP`, unchanged | 1312 | 5176 | 42968 | 24576 |
| ESP32 on-device with HTTP, before contacts | 1312 | 5176 | 72744 | 24576 |
| ESP32 on-device with HTTP, with contacts | 3400 | 7264 | 113864 | 40960 |

Only the ESP32 contact profile adds 2088 bytes per snapshot and 16384 bytes
per JSON buffer. Its four-snapshot/two-buffer dashboard object grows by
41120 bytes on this ABI. These are native test measurements, not ESP32 target
heap readings; target structure sizes depend on the toolchain ABI. The
seven-role worst-size JSON fixture uses 34218 of the 40960-byte JSON buffer.
The compiled socket-capacity check reserves all KISS client slots, the KISS
listener, up to three internal HTTP sockets, three HTTP clients and one spare
sockets. The current ESP32-S3 SDK provides 16 slots for this configuration.
Other board/core arrangements require checking their SDK configuration;
this dashboard is not enabled in native PHY-less builds.

A priority-1/core-0 push task with a 2 KiB stack schedules HTTP work every
25 ms, with at most one work item outstanding. The HTTP task serializes
the newest available publication once and sends at most one 1 KiB WebSocket
fragment per subscriber per invocation. The shared message stays immutable
until active sends finish; intervening publications are coalesced. Each
subscriber holds only its descriptor, fragment cursor and deadlines.
The radio loop does no additional work.

Live socket I/O is nonblocking. Each subscriber retains at most one 1 KiB
outbound fragment with its header and byte cursor. Short writes and full
socket buffers resume on subsequent HTTP work ticks without repeating any
bytes. A two-second write/message deadline still retires stalled peers.

PING/PONG reads retain at most 131 bytes (header, mask and 125-byte control
payload), with a two-second completion deadline and one bounded pending pong
reply. TCP splits in the header, mask or payload are accepted. The receive
override preserves the first opcode across SDK callbacks until the frame is
complete; no callback waits for missing bytes or reads into the next frame.
SDK 4.4.7 delivers PONG callbacks when control-frame handling is enabled, but
its built-in full-frame reader does not retain partial reads, so these bounded
reads replace that reader for PING/PONG. CLOSE/EOF remains SDK-owned, without
a second queued deletion.

Ping/pong heartbeats run every five seconds; the five-second response deadline
starts when the ping bytes finish writing. Terminal serial diagnostics include
the close reason, latest publication, RX/TX cursor positions and ping/pong
counts. Closing slots are not reusable until SDK cleanup completes.

A slow ordinary page/status request can delay other HTTP work until its
timeout, but cannot hold an RF lock. Live subscribers do not block that task
waiting for network buffers. Under physical HTTP load, check RF completion and
KISS control latency on the device.
HTTP startup failure is logged and does not stop the KISS radio service.
Invalid saved RF configuration still stops service at boot under the existing
fail-closed persistence contract; it is not bypassed to start the dashboard.

## HTTP API v1

`GET /`, `GET /api/status` and the WebSocket upgrade at `/api/live` are registered. Unsupported methods/routes
are rejected by the SDK. There are no mutation handlers or CORS permissions.
Responses use `Cache-Control: no-store`, `nosniff`, frame denial and a CSP
that permits only the embedded static script/styles and same-origin connections.

`GET /api/status` returns one internally consistent JSON snapshot:

| Field | Meaning |
| --- | --- |
| `api_version`, `device_name`, `publication` | Schema 1, display name, successful publication counter |
| `firmware_version` | Running compiled profile version; independent of display name and KISS protocol version |
| `uptime_ms` | Extended device monotonic milliseconds, survives `millis()` rollover but not reboot |
| `wifi` | Connected flag, RSSI in dBm or null, sampled connection uptime in ms |
| `memory` | Free and minimum-ever free heap in bytes |
| `profile` | RF frequency/BW in Hz, SF, CR denominator, TX power in dBm, AF, CAD, interference threshold, generation, committed/fault flags |
| `scheduler` | Waiting count/capacity, transmitting/carrier-wait flags, actual shared credit/max/window in ms, owner slot (`-1` if none) |
| `kiss` | Connected count/capacity and bounded active-client list: zero-based slot, generation, negotiated flag, source AF/credit/RF time |
| `totals` | RX count/estimated time/errors; TX admitted/rejected/succeeded/failed/unknown counts and observed RF time |
| `history` | Capacity 32, overwrite count and newest-first `events` |
| `traffic` | Up to 60 chronological one-second buckets: `second`, `rx_packets`, `tx_packets`, `tx_rf_ms`, `rx_estimated_ms` |
| `publications_skipped` | Copy contention count; radio work was not blocked |

Each event contains `sequence`, `at_ms`, `direction` (`rx`/`tx`), native TX
`state`/`reason`, source slot/generation/job, packet `length`, `queue_ms`,
`rf_ms`, `estimated_ms`, `rssi_dbm`, `snr_db`, `preview_hex` and
`preview_truncated`. TX state/reason/source/job fields apply only to TX.
Admission increments its counter but does not occupy an event-history slot.
Rejected submissions without an admitted packet have zero length/no preview.

On-device ESP32 snapshots optionally add `contacts`: `capacity` (32), `total`
(native non-anonymous contact count), `truncated` and `items`. Each item has a
64-character lowercase `public_key`, `name` (up to 31 bytes) and numeric native
advert `type` (1 chat, 2 repeater, 3 room, 4 sensor). The array uses the native
contact iterator's order, skips anonymous reserved slots, and contains at most
32 records per publication. A present empty list means the companion currently
has no contacts; an absent field means contacts are unavailable. Native stored
contacts can remain after a restart and do not imply recent RF reception.

The endpoint admits at most two status requests per second across all
browsers; excess requests return **429** with `Retry-After: 1`. A temporarily
unavailable or more-than-three-second-old snapshot returns **503** with the
same retry hint. Serialization
overflow returns **500**, never partial JSON. This endpoint remains available
for scripts and manual diagnostics; the browser does not poll it.

### Realtime stream

Connect a native WebSocket to `ws://DEVICE/api/live`. ESP-IDF 4.4.7's bundled
WebSocket support is required at compile time; no extra library or subprotocol
is used. Each text message is a complete API-v1 snapshot with the same shape
as `/api/status`, fragmented on the wire into at most 1 KiB pieces. The first
message supplies current state; subsequent messages follow radio publications
(normally every 500 ms). Publication numbers can skip during contention or
coalescing. There is no replay queue, acknowledgement or client mutation
message. Ping/pong controls are supported; application messages are rejected.
An over-capacity, unresponsive or stale-source session is disconnected.
Browser `Origin`, when present, must match the HTTP `Host`; cross-origin
browser subscriptions are rejected. Non-browser clients may omit `Origin`.

The page maintains one socket, closes it when paused or hidden, and waits
for that socket to close before reconnecting. Reconnect uses exponential
backoff from one to ten seconds plus up to 500 ms jitter, reset by a valid
snapshot. A six-second snapshot watchdog marks data stale and reconnects
even if a connection still appears open. The server also closes subscribers
when its radio snapshot is over three seconds old. Last valid data remains
visible during failures. Pause, reconnect and filter controls affect only
that browser.

## Measurement semantics

- RX events come only from positive physical `recvRaw` results, including
  when no KISS clients are connected. Local sender-excluding reflections
  never enter RX counters, history or occupancy.
- RX RSSI/SNR come from the driver's received-packet measurements. TX signal
  fields are null. Unavailable/nonfinite RX readings are also null, not zero
  or the synthetic local-loopback marker.
- **RX time is an estimate**, calculated with the current radio's native
  airtime estimator. `rf_ms` is null for RX; it is not measured receive
  occupancy and is never deducted from the shared TX budget.
- TX RF time is the scheduler's start-to-observed-completion interval,
  including conservative timeout charging. It is not an independent RF
  hardware timer. Queue/carrier wait is separate and contributes no RF time.
- TX failure counts include pre-RF expiry/cancellation and start failures.
  Disconnected queued jobs are recorded as FAILED/DISCONNECTED with zero RF
  time. An already-started transmission retains its physical terminal result.
  Unknown does not mean success and does not cause replay.
- Charts spread completed operation durations across the relevant one-second
  buckets. Ongoing TX appears when it completes. RX estimated and TX observed
  series are separate, not summed into a fabricated utilization value.
- Dashboard totals are 64-bit, since boot. Per-client RF counters retain the
  existing protocol's uint32 millisecond wrap and reset on a new connection.
  These are physical/connection observations, not repeater/room-role counters.

## Validation

`make -f test_support/phy_parity/Makefile test` covers actual-modem observer
integration, queue-vs-RF timing, reflection exclusion, RF failure/timeout and
disconnect outcomes, ring bounds, rollover, signal availability, concurrent
snapshot consistency and actual serializer output. Stream tests exercise
fragment reconstruction, shared-publication coalescing, full subscriber
slots, slow-client retirement, slot cleanup, source staleness and heartbeat
expiry. Local socket pairs check full/partial/nonblocking I/O behaviour.
The ESP-handler tests split masked controls at every header boundary and
byte-by-byte through a maximum-size ping, force short writes and real socket
backpressure, reconstruct exact output bytes, and run twelve split-pong
heartbeat cycles. Malformed controls and read/write deadline expiry remain
failures rather than reconnect-shaped successes.
Node executes the shipped script with a controlled clock/transport to check
pause/hide/resume, reconnect, invalid updates, watchdog expiry and absence
of overlapping connections or HTTP polling.
The focused `make -f test_support/phy_parity/Makefile dashboard-test` target
also checks the browser's packet bounds, v0 decoding and unsupported versions,
one-/two-/three-byte paths, ambiguous prefix names, text-only rendering and
second/minute/hour/day age boundaries without rebuilding age-only rows.
Its contact bridge test checks bounded native enumeration, lifecycle
availability, public-only JSON escaping, snapshot copying and worst-size
on-device serialization.
`make -f test_support/phy_parity/Makefile dashboard-budget-test` checks contact
storage presence and JSON capacities for general, general on-device, Pine and
ESP32 profiles, and prints each profile's native structure sizes.

`make -f test_support/phy_parity/Makefile dashboard-browser` optionally uses an
installed Chrome/Chromium to render the page with a local fixture at
desktop/mobile widths over fragmented WebSocket messages, including CSP
enforcement and retained stale state after disconnection.
`make -f test_support/phy_parity/Makefile firmware-build` builds the isolated
ESP32 image without private configuration or hardware access.

On the radio-only profile, connect eight KISS clients and two live dashboard
viewers while sending RF traffic and requesting diagnostics. Include a slow HTTP
client; check RF completion, control latency, desktop/mobile display and the
saved profile after a reboot.
