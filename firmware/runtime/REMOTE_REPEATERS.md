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
bot repeaters discovery 900
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
An explicit path replaces that target's cached outbound route, including when
first adding an already known contact or reapplying the same saved seed. Other
targets' routes, pending requests and next-poll times remain unchanged.

Pause one target without losing its key, route, cooldown or last-good sample:

```text
bot repeaters enable ridge off
bot repeaters status ridge
bot repeaters enable ridge on
bot repeaters remove ridge
```

`enable` is saved and applies immediately. A disabled target sends no scheduled
monitor requests and reports `enabled=0`, `error=disabled` and `wait=disabled`;
retained voltage stays at its original sample time. Re-enabling respects the
existing poll and discovery cooldowns. Updating a disabled alias with `add` keeps it disabled.
`remove` deletes the saved target; a new alias added to that slot starts enabled.
Disabling or editing one target cancels only its pending request and cannot
undo a request already transmitted. Use `bot repeaters off` to stop all targets.

Install the example **additively**, retaining the current command program:

```sh
python3 -B tools/admin.py \
  --web http://RADIO_IP --password-file /path/to/private/management-password \
  source install monitor firmware/runtime/plugins/examples/remote-repeater-monitor.lua
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
bot repeaters config ridge
bot repeaters route ridge
bot repeaters timing ridge
bot repeaters storage
bot events
telemetry status
telemetry counts
```

After an eligible poll, expect `available=1 fresh=1 error=none`, the remote
uptime and a battery voltage if its hardware supplies one. `available=1`
alone means there is a last-good sample, which may be stale. The status
display retains that sample for diagnosis; use `fresh` before treating its
voltage as current. Battery percentage is not inferred from voltage.
`route ridge` reads the current outbound path without sending a packet:
`route=unknown` needs flood discovery, while `route=3:cc268a` uses that
three-byte hop. Routes learned from authenticated status/PATH responses
replace the previous path automatically. `repair=1` means three consecutive
failures require flood discovery on the next eligible poll; the saved discovery
cooldown still applies. `timing ridge` reports the waiting reason, seconds until
the next eligible poll/discovery and the last sample's UTC timestamp. Eligibility
does not reserve airtime or bypass another pending request. Learned routes are
rebuilt after a restart; adverts do not supply status or an outbound route.
`timing` without an alias reports the monitor start cooldown and
number of pending probes, without sending RF.
Use `!find-best-path PREFIX` to compare the learned route, saved seed and a
fresh flood route. It tests each distinct path twice, reports delivery and
mean latency, and keeps the learned path unless a candidate performs better.
See [route discovery and comparison](BOT_RUNTIME.md#discovery-targeting-and-owner-commands)
for timing and channel permissions.
See [Aspen telemetry setup](../esp32/TELEMETRY.md) if publishing is not already
configured. HTTP 2xx confirms receiver acceptance; query the receiver to
check ingestion.

`config ALIAS` reads the saved public key and frequency without sending RF.
`storage` reports free NVS entries, the required reserve and whether a policy
authority is saved. A failed policy change reports the affected storage step
and leaves live polling disabled. Inspect these results before retrying; do not
erase NVS or format SPIFFS to make room.

Import the [remote-repeater Grafana dashboard](../../dashboards/grafana/README.md)
to view voltage, sample age, monitor errors, signal, packet counts and airtime
from this native feed.

## Run commands on selected repeaters

Install the fleet command on Aspen to send a CLI command to selected repeaters
and receive each repeater's response in a private owner conversation. Check
`source api repeater-cli` through authenticated management before installing.

On each remote repeater, use its existing administrator session to grant the
command bot administrator access:

```text
setperm COMMAND_BOT_FULL_PUBLIC_KEY 3
```

Keep the existing configured targets and learned routes. A disabled target or
one on another frequency remains unavailable to fleet commands. CLI requests
use the same learned routes as monitoring. An unknown route starts a MeshCore
login and PATH exchange to establish the outbound and return routes.

This source requires Aspen 0.1.18 or a host worker with the read-only retry
option, and groups require `groups=1` in `source api repeater-cli`. The current
source calls `repeater.targets(text)`, so install it only with firmware that
reports `groups=1`.
Install the source additively:

```sh
python3 -B tools/admin.py \
  --web http://RADIO_IP --password-file /path/to/private/management-password \
  source install rcli firmware/runtime/plugins/examples/repeater-cli.lua
```

In an authenticated private DM from a trusted owner:

```text
!rcmd d1,d2,d4,d7,wye ver
!rcmd all get txdelay
!rcmd d1,d2 neighbors
!rcmd d1,d2 gps
!rcmd-write d1,d2 set flood.max 4
```

Select configured aliases or group names, separated by commas; case does not
matter, and a repeater named twice runs once. You can select every configured
repeater whose aliases fit in the command message. An unknown name refuses the
whole command before anything is sent; the reply lists the configured aliases
and groups. Existing command and radio-action permissions still apply.

`!rcmd` sends only read-only commands: `ver`, `board`, `clock`, `neighbors`,
`get ...`, `stats-core`, `stats-radio`, `stats-packets`, `gps`, `gps diag`,
`sensor list`, `sensor get ...`, `region`, `powersaving`, `ota status`, `log`
and `help`. Use `!rcmd-write` for everything else. Commands are matched without
regard to case or surrounding spaces and sent as typed.

A change sent to more than one repeater must be one of these, because a wrong
value can be corrected over the air afterwards:

- `set flood.max`, `set flood.max.advert`, `set flood.max.unscoped`
- `set txdelay`, `set direct.txdelay`, `set rxdelay`, `set af`
- `set advert.interval`, `set flood.advert.interval`
- `advert`, `advert.zerohop`, `clock sync`, `clear stats`, `discover.neighbors`

Every other change runs on one repeater at a time. That includes radio, power
and routing settings (`set radio`, `set freq`, `set tx`, `tempradio`,
`set repeat`, `set path.hash.mode`), identity and access (`set name`,
`set prv.key`, `setperm`, `password`, `set guest.password`), and restart,
shutdown, erase and OTA commands. One bad radio or power value sent to a whole
group can leave every member unreachable.

### Repeater groups

A group is a name for a set of configured repeaters, such as `all` for the
nodes you operate or `sherwood` for one site. Define groups on the monitor's
authenticated management connection:

```text
bot repeaters group all d1,d2,d4,d7,wye
bot repeaters group sherwood d1,wye
bot repeaters groups
bot repeaters group all
bot repeaters group sherwood -
```

Members are configured aliases or unique public-key prefixes of at least four
hex digits (`d12600` selects the target whose key starts with those digits).
The reply lists the aliases that were saved. `group NAME -` removes a group.
Up to four groups are saved with the repeater settings, in the same file as the
targets and without extra NVS entries. A group name follows the alias rules and
cannot match an alias; an alias cannot be added with a group's name.

`!rcmd all ver` and `!rcmd-write sherwood set flood.max 4` expand the group when
the command starts. Removing a target removes it from every group, and a group
left with no members is deleted. A new target never inherits memberships.
A change outside the group list above refuses a group of more than one member.

Requests run sequentially. The next target starts as soon as the previous
request completes and the radio can accept it. Background monitoring yields
to waiting interactive requests. Each request can wait up to 30 seconds for
admission. After transmission, the reply timeout is 15..60 seconds according
to airtime and hop count; a matching reply completes the request immediately.
Replies identify their target and split to fit the RF payload. Native capture
holds 256 bytes per target and marks longer responses as truncated. An `ok`
result means a matching response arrived; the remote response itself reports
whether the command succeeded.

Read-only commands can repair the route and retry once after a missing reply.
The discovery cooldown never holds back an owner command: an unknown or failed
route starts the MeshCore flood login and PATH exchange straight away. Owner
commands still run one at a time, and each reply reports its own outcome,
including a timeout. Their floods do not restart the monitor's cooldown; a
route learned from a successful repair is shared with monitoring.
Commands reuse the shared route and admin session. A successful CLI reply
records admin access for subsequent commands and route discovery.
Repair exchanges login and return paths, then sends the command. Its reply
confirms delivery.
Queries such as `gps` use `!rcmd`; changes such as `gps on` use `!rcmd-write`.
A login that confirms insufficient admin access reports that condition directly.
`bot repeaters config ALIAS` shows the bot's last observed CLI access as
`cli=admin`, `read`, `none` or `unknown`.

A timed-out write may have applied. It is sent once: read back the affected
setting before deciding whether to repeat it. Passwords and private keys do
not belong in the installed Lua source.

`repeater.targets(text)` expands a comma-separated list of aliases and group
names for the current trusted-owner DM. It yields a Lua array of distinct
aliases, or `nil, message` when the list is empty, too long or names something
that is neither a configured alias nor a group.

For another Lua command, `repeater.command(alias, text[, read_only])` yields
`{ok, text, reply, error, queued, transmitted, truncated}`. `text` retains line
breaks; `reply` replaces them with spaces for RF output. Native code checks the
current trusted-owner DM, target enable state, frequency and policy generation
at admission and completion. Scheduled events cannot execute remote CLI.
The monitor can remain off while the owner uses this API. The optional third
argument defaults to `false`; pass `true` for a query safe to repeat after
route repair. Writes are sent once.

## Replace or compose Lua sources

`source install NAME FILE` adds or updates one named Lua source.
`source list` lists the names; `source remove NAME` removes one.
All sources share a single environment, so they can call each other's exported
procedures and registered modules. They are separately initialized chunks,
not separate VMs. Local variables remain local to their chunk.

The whole source set, counting every file, is limited to **32,768 bytes and
16 names** on ESP32 and the Linux host (4,096 bytes and eight names on compact
Pine). If the current source exactly matches the firmware's
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

Keep a known-good file before replacing a command or monitor:

```sh
python3 tools/admin.py --web http://MAST --password-file OWNER_FILE \
  source export monitor known-good-monitor.lua
python3 tools/admin.py --web http://MAST --password-file OWNER_FILE \
  source install monitor changed-monitor.lua
# Restore only that file when needed:
python3 tools/admin.py --web http://MAST --password-file OWNER_FILE \
  source install monitor known-good-monitor.lua
```

Export creates a private `0600` file without changing the running source.
Reinstalling it leaves other named files, native permissions, durable notes,
thread data and Wasm selection unchanged. Any Lua source replacement rebuilds
the shared Lua environment, resets globals and cancels pending Lua work.
A custom source set must export a command or declare an event; add configuration
chunks to a working set rather than installing a configuration-only custom set.

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
`bot repeaters discovery SECONDS` sets the flood-discovery cooldown to
60..86400 seconds, default **900**. Both settings are saved. Existing monitor
records retain all targets and last-discovery times when read by this firmware,
and use the new 900-second default until explicitly changed. A subsequent save
uses version 3, which also stores per-target enable settings; older firmware cannot read it without restoring or
reprovisioning the monitor policy.
There is at most one pending monitor RF request. The next due target can
start after the previous request completes.
Requests use the shared queue and the bot's existing static/adaptive airtime
admission; the Lua period cannot bypass those limits. Busy or exhausted
admission sends no request.

An admitted request has a 30-second deadline. Replies must authenticate to
the configured full key and match the pending request, generation, grant and
deadline. Local reflection is not a remote sample. A correlated authenticated
PATH response can repair the outbound route. Status decoding uses the
56-byte native v1.18.0 stats layout from MeshCore revision
`81dbae592ffad40b4212bc0b37e82b38da08f4bc`; other wire layouts are not negotiated.

Timeout, malformed response or TX failure backs off from twice the owner
interval, up to the larger of that interval and the discovery cooldown. Three failed direct
requests permit flood route repair. An unknown route also needs a flood.
Scheduled monitor polls flood each target at most once per discovery period,
so a silent or offline repeater is not flooded every interval. The cooldown
applies only to these background polls: `!rcmd`, `!rcmd-write`, `!find-path`,
`!find-best-path` and other owner requests are never delayed by it and do not
restart it. The poll's flood timestamp is kept in memory and saved before queue
admission so the cooldown survives reboot. If saving fails, the request
proceeds and the cooldown remains active until restart; the storage error is
logged. Restart does not recover
volatile samples, attempt counters or pending requests, and cannot replay an
uncertain transmission. Future periodic polls remain eligible under the saved
cadence/cooldown; they are new samples, not retries of that request.

Policies and flood cooldowns use two SHA-256-verified SPIFFS files and a
37-byte NVS reference. A save verifies the inactive file before publishing
and reading back its reference. Insufficient NVS headroom rejects the save
without reducing the existing Lua data reserve. An unreadable policy or
uncertain configuration change disables live polling and reports the storage failure; repair
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
Last-good voltage and remote numeric stats retain their original RF sample
timestamp, including after a timeout or disable. Repeated uploads use the
same timestamp and values; they are not additional RF observations.
`sample_time_seconds` is derived from the trusted UTC clock at request time plus
the local elapsed time until its authenticated response. The Influx line uses
that time in nanoseconds. A peer without a successful sample has no numeric
stats. Samples are cached in RAM, not written to flash after every poll.

Current availability, freshness, poll errors, `wait_code`,
`next_poll_seconds` and `next_discovery_seconds` use receiver receipt time.
Wait codes are 0 eligible, 1 interval/backoff, 2 discovery cooldown, 3 clock
unavailable, 4 disabled and 5 frequency mismatch. Countdown fields describe
eligibility, not a promised transmission.

Stats include remote uptime, queue length, RX/TX packet and airtime counters,
direct/flood counts, duplicates, error flags/RX errors, noise, RSSI and SNR.
Zero battery millivolts means unsupported and omits `battery_volts`;
there is no battery charge percentage field. No keys, passwords or messages
are metrics. Three remote peers rotate through each existing 8,192-byte
publication alongside local metrics; 12 targets appear within four successful
batches. Rotation advances only after receiver acceptance. A failed publication
keeps that peer group for the next period's current snapshot; it does not replay
the previous request. Control-field receipt timestamps do not mean a new RF
sample occurred.

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
