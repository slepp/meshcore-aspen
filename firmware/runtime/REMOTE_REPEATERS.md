# Monitor remote repeaters from a Lua bot

An on-device Lua bot can poll saved repeater identities without joining a
channel. On Aspen's HTTPS image, the existing telemetry publisher sends their
status and battery voltage to the same InfluxDB-line-protocol receiver as local
metrics. The Lua program chooses when to ask; native code owns the identities,
read permissions, radio admission, response matching and metric cache.

Use a bot and repeaters on the **same frequency, bandwidth, SF and CR**.
Adding a target never retunes the shared modem. A target on another frequency
reports `frequency` and sends no request. Changing the modem affects every
role, including its adverts; do not retune merely to test a monitor.

## Configure one target

Read the monitor bot's complete public key with authenticated management
`key bot`. On each remote repeater, use its existing administrator session:

```text
setperm MONITOR_BOT_FULL_PUBLIC_KEY 1
```

Permission `1` is the native read-only ACL entry. Save it on the repeater;
neither a repeater password nor an admin grant belongs in Lua. The monitor
sends encrypted native status requests under its bot identity. A saved read
entry establishes the shared secret without a password login.

On the monitor's authenticated management connection:

```text
bot repeaters add ridge REMOTE_FULL_PUBLIC_KEY 912525000 3:
bot repeaters interval 300
bot repeaters on
```

Replace the key and frequency with the target's actual values. Aliases have
1..16 lowercase identifier bytes; full keys are 64 hex digits. `3:` means a
known zero-hop direct path with three-byte hashes. For a known route, append
its complete outbound hop hashes, for example `3:010203040506`. Omit the
path when unknown; the first eligible request uses bounded flood discovery.
Frequency `0` explicitly permits polling on the modem's current frequency.
`add` updates an existing alias; duplicate target keys and the bot's own key
are rejected. Normal capacity is 12 targets; compact Pine capacity is four.

Install the example **additively**, retaining the current command program:

```sh
python3 -B tools/hardware/admin.py \
  --web http://RADIO_IP --password-file /path/to/private/management-password \
  source-install monitor firmware/runtime/plugins/examples/remote-repeater-monitor.lua
```

Then enable the scheduled event with `bot events 16`. Event masks are a
bitset: preserve any existing enabled bits when adding **16 scheduled**.
Both this event grant and `bot repeaters on` are required. They default to
off, survive restarts and remain separate from HTTPS, channel and storage
grants. On a native host's private owner connection, omit the `bot` prefix
for `repeaters` and `events` commands.

Inspect:

```text
bot repeaters status
bot repeaters status ridge
bot events
telemetry status
telemetry counts
```

After an eligible poll, expect `available=1 fresh=1 error=none`, the remote
uptime and a battery voltage if its hardware supplies one. `available=1`
alone means there is a last-good sample, which may be stale. The status
display retains that sample for diagnosis; use `fresh` before treating its
voltage as current. Battery percentage is not inferred from voltage.
See [Aspen telemetry setup](../esp32/TELEMETRY.md) if publishing is not already
configured. HTTP 2xx confirms receiver acceptance; query the receiver to
check ingestion.

Import the [remote-repeater Grafana dashboard](../../dashboards/grafana/README.md)
to view voltage, sample age, monitor errors, signal, packet counts and airtime
from this native feed.

## Replace or compose Lua sources

`source-install NAME FILE` adds or updates one named Lua source.
`source-list` lists the names; `source-remove NAME` removes one.
All sources share a single environment, so they can call each other's exported
procedures and registered modules. They are separately initialized chunks,
not separate VMs. Local variables remain local to their chunk.

The combined source-set envelope is limited to **4,096 bytes and eight names**,
not 4,096 bytes per file. If the current source exactly matches the firmware's
bundled command program, the installer retains it as a symbolic builtin entry
rather than copying its text into that budget. Other existing source text is
retained byte-for-byte. Removing the monitor does not remove the existing
program, identities, notes, KV or reminders.

Duplicate globals owned by different chunks, command names, module names or
event handlers reject the entire candidate. A shared procedure should have
one defining source. Existing bundled-command override rules still apply.
The installer checks the downloaded base SHA-256 at commit; a concurrent
source change rejects the update. Validation and durable activation use the
existing source journal, so a rejected candidate leaves the previous source
active. `rollback` restores the previous **whole source set**, not one file.
This requires durable source management; core-only RAM source activation
does not survive a restart.

The minimal replaceable monitor is:

```lua
function fleet_poll()
  local peer = repeater.next()
  if peer then repeater.status(peer) end
end
events.every(15, 'fleet_poll')
```

`events.every(SECONDS, FUNCTION_NAME)` declares one handler during
initialization, with a period of 15..86400 seconds. It receives
`ctx.kind == 'scheduled'`, has no channel or reply route, and runs only under
the owner's scheduled-event grant. Missed ticks are skipped, not replayed;
there is no overlapping event or backlog. A source reload/restart starts a
new cadence. `events.off('scheduled')` disables the subscription until reload
and fences pending work; it does not change the saved owner grant.

`repeater.next()` yields and returns an eligible owner-configured alias or
`nil`. `repeater.status(alias)` and passwordless `repeater.login(alias)` yield
immutable result tables with `ok`, `code` and `error`; ordinary failures do
not throw. A successful status contains the decoded fields, including
`battery_volts` only when supported. These APIs require a currently granted
scheduled event or an authenticated trusted-owner private DM, as well as the
native repeater enable policy. They are unavailable to public command callers
and channel handlers. A login grants only the repeater's saved read permission;
the example does not need to log in first.

Wasm does not implement recurring events, repeater calls or named Lua source
composition. Pine's compact Lua runtime can collect status, but has no HTTPS
publisher. Host collection uses the same native RF policy; automatic remote
metric export described here belongs to Aspen's telemetry service.

## Airtime, failures and restart behavior

The owner interval is 60..86400 seconds per target, default **300**.
Targets are initially staggered by 30 seconds, and there is at most one
monitor request per 30 seconds globally and one pending monitor RF request.
Requests use the shared queue and the bot's existing static/adaptive airtime
admission; the Lua period cannot bypass those limits. Busy or exhausted
admission sends no request.

An admitted request has a 30-second deadline. Replies must authenticate to
the configured full key and match the pending request, generation, grant and
deadline. Local reflection is not a remote sample. A correlated authenticated
PATH response can repair the outbound route. Status decoding uses the
56-byte native v1.17.1 stats layout from MeshCore revision
`d92964352441e53b93e8667b802e04f6e072b39e`; other wire layouts are not negotiated.

Timeout, malformed response or TX failure backs off from twice the owner
interval, up to the larger of that interval and one hour. Three failed direct
requests permit flood route repair. An unknown route also needs a flood.
Each target may flood at most once per hour: the trusted-UTC cooldown is saved
**before** queue admission and survives reboot. Restart does not recover
volatile samples, attempt counters or pending requests, and cannot replay an
uncertain transmission. Future periodic polls remain eligible under the saved
cadence/cooldown; they are new samples, not retries of that request.

Policies and flood cooldowns use two SHA-256-verified SPIFFS files and a
37-byte NVS reference. A save verifies the inactive file before publishing
and reading back its reference. Insufficient NVS headroom rejects the save
without reducing the existing Lua data reserve. An unreadable policy or
uncertain save disables live polling and reports the storage failure; repair
storage and inspect the saved policy before re-enabling. Do not erase NVS or
format SPIFFS to make room.

Disable with `bot repeaters off` or remove bit 16 from `bot events`.
Revocation/source replacement cancels pending continuations but cannot undo
a request already transmitted. Before downgrading to firmware that only
supports event masks 0..15, remove bit 16 and disable the monitor.

## Receiver fields

Each `meshcore_repeater` line has bounded `device` and `peer` alias tags, with
`available`, `fresh`, `error_code`, `attempts_total`, `failures_total`, and
`sample_age_seconds` when a sample exists. Fresh means a successful last
sample no older than twice the owner poll interval and no subsequent error.
Only fresh samples include voltage and remote numeric stats. On timeout or
disable, stale voltage is not emitted as a new observation.

Stats include remote uptime, queue length, RX/TX packet and airtime counters,
direct/flood counts, duplicates, error flags/RX errors, noise, RSSI and SNR.
Zero battery millivolts means unsupported and omits `battery_volts`;
there is no battery charge percentage field. No keys, passwords or messages
are metrics. Three remote peers rotate through each existing 6,144-byte
publication alongside local metrics; 12 targets appear within four successful
batches. Receipt timestamps do not mean a new RF sample occurred.

| `error_code` | Condition |
| --- | --- |
| 0 | No current error |
| 1 | Target unavailable |
| 2 | Native monitor disabled |
| 3 | Poll or flood cooldown not due |
| 4 | Shared modem or another monitor request busy |
| 5 | Trusted clock unavailable |
| 6 | Packet/airtime capacity unavailable |
| 7 | Response deadline expired |
| 8 | Malformed response |
| 9 | Source/grant/policy cancellation |
| 10 | Owner or remote read permission unavailable |
| 11 | Failed or uncertain transmission |
| 12 | Target frequency differs from the shared modem |

For VictoriaMetrics, query `meshcore_repeater_battery_volts` with the configured
`device` and `peer` labels and inspect `meshcore_repeater_fresh` and
`meshcore_repeater_error_code` alongside it. Historical samples remain in the
receiver; use a time window appropriate for the poll and publisher intervals.
