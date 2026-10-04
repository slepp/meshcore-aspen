# Manage Willow over the radio

Use a companion radio's remote-management client to log in to Willow-Relay or
Willow-Room with that role's administrator password. Start with:

```text
get name
get radio
stats sensors
get owner.info
```

The role replies over RF. `get radio` reads the profile verified during modem
negotiation; it does not tune the shared modem. Radio, frequency, power and
WiFi changes remain with the modem administrator.

Administrator commands require permission low bits `3`. Room guests can post
with low bits `2`; read-only members can receive history but cannot post or
run administrative commands. Channel-key possession does not grant management
access. Relay and room retain separate identities, credentials and ACLs.

## Supported requests

| Surface | Behavior |
| --- | --- |
| Login | Authenticated guest/admin/ACL login; direct or flood reply; learned return paths; persisted replay stamps. `allow.read.only=on` admits a room guest with a wrong password as read-only. |
| TXT management | Subtype 1 runs administrator commands. Relay subtype 0 also runs commands and emits its transport ACK. A repeated stamp does not rerun the command. Three-byte prefixes such as `A1\|` are preserved. |
| Binary REQ 1 | Echoed timestamp plus the native 56-byte relay or 52-byte room status body. Radio values and role counters remain distinct. Missing noise/RSSI/SNR use the native signed sentinel. |
| Binary REQ 2 | Room direct keepalive, sync cursor and pending-message count. |
| Binary REQ 3 | Cayenne voltage/temperature records for available measurements. No sensor data means no response, rather than invented readings. |
| Binary REQ 5 | Administrator-only ACL list: six key bytes and the full permission byte per entry. Room replies list administrators only. |
| Binary REQ 6/7 | Relay neighbour pagination and owner/version information. Owner text is shortened at a UTF-8 boundary to fit the actual reply path. |
| CONTROL | Discovery request `0x80`/`0x81`, reply `0x92`, echoed tag, measured quarter-dB SNR and requested 32/8-byte key. Four admitted attempts per 120 seconds. |
| Anonymous relay queries | Authenticated ANON_REQ without login: query 1 lists the wildcard region when no named registry is configured; query 2 returns name/owner; query 3 returns time/repeat flags. Direct requests must supply a valid return path. Four queries per 180 seconds. |

The neighbour cache holds 50 entries, matching the Go default. Only measured,
non-reflected, zero-hop signed repeater adverts and matching discovery replies
populate it. Pagination supports the native sort orders, offset and key width.
Binary PATH responses enforce the 184-byte encrypted payload limit before
committing an admitted request.

## Commands

Read commands include `help`, `help get|set|wifi|radio|region|owner|stats`,
`board`, `ver`, `clock`, `gps advert`, `get stats`, and
`stats [role|sensors|radio|signal|airtime]`.
Willow identifies itself as Willow, not Birch.

`get` supports name, public key, role, guest password, owner information,
radio/frequency/power, forwarding/loop/path limits, advert intervals, delays,
airtime factor, duty cycle, multi-ACK and read-only settings. `get lat` and
`get lon` report the saved coordinates; the host does not measure GPS position.

Role-local mutations:

```text
set name Willow-Room
set owner.info Operator|Station
set guest.password NEW_VALUE
password NEW_ADMIN_VALUE
set multi.acks 1
set allow.read.only on
set repeat off
set loop.detect moderate
set path.hash.mode 2
set flood.max 16
set flood.max.unscoped 16
set flood.max.advert 8
set txdelay 0.5
set direct.txdelay 0.2
set rxdelay 2
set af 2
set dutycycle 50
set advert.interval 60
set flood.advert.interval 3
set lat 53.5461
set lon -113.4938
gps advert prefs
```

Path mode `0/1/2` selects `1/2/3` bytes per hop. Retransmit factors retain
binary32 precision and require the negotiated airtime table when forwarding
is enabled. `rxdelay` accepts a finite factor in 0..20 and requires that table.
It holds received flood packets using measured SNR, verified spreading factor
and packet airtime. Holds below 50 ms are skipped; longer holds are capped at
32 seconds. Direct packets and local reflections bypass the hold. Each role
retains up to 64 waiting packets with their measurements; a full queue reports
the dropped packet. A score failure allows local processing but suppresses
forwarding. Restart discards waiting packets without replaying them.
Advert interval units are minutes and hours respectively;
zero disables that schedule. Successful manual preference changes disable
an initial local advert interval shorter than one hour.

`af` accepts any finite, nonnegative binary32 factor. `dutycycle` accepts
1..100 percent and converts it to `100 / percent - 1`. These change only that
role's source airtime budget, not the shared modem's physical profile.
The role commits the setting before asking the modem to apply it, then waits
up to five seconds for the exact readback before sending its reply. Other roles
continue while that role's packets wait. A rejected, malformed or missing
readback retains the committed setting and stops that role; restart the host
service after correcting the modem connection.

The saved preference profile controls startup behavior. `native-preferences`
clamps factors above 9 to 9 when the role loads; `durable-host-preferences`
retains the full saved factor. Fresh roles use the native profile. The private
config accepts `relay.preference_profile` and `room.preference_profile` when
an explicit startup override is needed. During connection negotiation, the
modem confirms each source factor before startup adverts are generated.
A failed reconnect negotiation retries without quarantining a healthy role.

Latitude must be finite and within -90..90 degrees, longitude within
-180..180. `gps advert prefs` includes those coordinates in signed adverts;
`gps advert none` omits them without clearing the saved coordinates.
`clock` reads the role's UTC clock. `clock sync` advances it to the authenticated
request timestamp plus one second; `time UNIX_SECONDS` sets a later uint32
Unix timestamp. Backwards changes are refused. The role's saved offset survives
restart and affects protocol timestamps, not the host clock, advert intervals
or transmission deadlines.

`setperm FULL_KEY PERMISSION` changes a member's full permission byte.
A value with low bits zero removes exactly one matching full key or
unambiguous key prefix. Administrator entries are never eviction candidates.
`advert` and `advert.zerohop` request signed advertisements. Relay commands
also include `discover.neighbors`, `neighbors`, and
`neighbor.remove KEY_PREFIX`. `room.post TEXT` creates an own-author room post.
Room `multi.acks=1` sends multipart ACK at 200 ms and ordinary ACK at 500 ms
when the sender has a known path.

CLI replies retain the native 155-byte UTF-8 limit and 1500 ms relay /
300 ms room delay. Password changes are limited to 15 bytes at a valid UTF-8
boundary; a multibyte character is not cut in half.
Malformed UTF-8 commands receive an error without applying a setting.

## State and failure behavior

RF changes to names, credentials, owner information and supported preferences
are committed with the ACL/replay transaction before replies are emitted.
They become overrides of startup settings. An initialized role never replaces
missing or corrupt committed state with fresh defaults.

Preference-only changes use `HEW5`; named-region state uses `HEW6`. Both retain
HEW4's member-attempt/history layout and append a validated preference record.
Preference version 2 stores coordinates, advert-location mode and RTC offset;
version 3 also stores binary32 receive-delay and source-factor values.
It retains the native or durable source-factor preference profile.
Versions 1/2 and HEW2/3/4 snapshots remain readable. Existing native NVS/SPIFFS files, the native bot command source and
its 3600 ms/minute budget are not modified by these setters.

Actor replacement loads those overrides from the committed snapshot. A failed
commit quarantines that role and suppresses its replies; healthy roles and the
modem remain available. The production fault test changes a room name over
authenticated packets, panics the actual role owner, resolves its stable
ChildRef replacement and reads the saved name while relay and bot continue.

Keep the state directory private: snapshots contain credentials. Rollback
must preserve both Willow and Go trees; Go does not read Hew snapshots. See
[migration and rollback](MIGRATION.md).

## Capture a role's packets

An authenticated administrator can run `log start`, `log stop` and `log erase`
over RF. Capture uses private JSONL in `STATE/relay.state.packet.log` or
`STATE/room.state.packet.log`. Read it locally, never over RF:

```sh
python3 -B willow.py packet-log --state /path/to/state --role room
```

RX records distinguish measured RF from local reflection; reflected records
have no measured SNR/RSSI. TX records describe confirmed transmissions.
`TX_RESULT` records retain rejected, failed and uncertain outcomes with source
generation, job ID, reason, queue wait, RF airtime and estimated airtime.
Accepted jobs are not logged as confirmed transmissions.

Capture stops at 4 MiB without replacing history, or after a private-file write
error. The role continues to serve requests and reports `PACKET_LOG_STOPPED`.
Archive or erase the file before restarting capture. Erasure waits for the role
transaction to commit; failed commits leave the retained log unchanged.
Role or host restart stops capture but retains the file; run `log start` again
when needed. A modem reconnect alone does not stop an otherwise healthy role's
capture. Terminal logging has a 96-record host queue; an overflow reports the
dropped count and retires the modem session rather than fabricating an outcome.

## Modem measurements

The service samples battery, noise floor, current RSSI, MCU temperature and
physical packet counters through read-only modem commands. It permits one
optional request at a time, with a two-second deadline and a 45-second cache.
Due profile checks take priority between optional requests.
Missing callbacks leave that measurement unavailable. A malformed or timed-out
uncorrelated reply pauses optional sampling until reconnection, without
retiring otherwise healthy roles or the modem.
Aggregate confirmed TX airtime comes from the shared modem's source-statistics
reply and has its own 45-second freshness limit. It remains available when an
optional sensor callback is missing. RX airtime is not inferred from packet
length or radio time estimates.

Role TX counters advance only for matching successful queued-TX outcomes,
not submissions, failures or uncertain disconnects. Queue depth counts this
role's outstanding modem jobs. Role counters restart with the role; room post
and push status fields count work since that role instance started.

## Role restart and remaining surfaces

An authenticated relay or room administrator can send `reboot` over RF.
The host commits the command's replay stamp before replacing only that role.
CLI reboot has no success reply; a legacy relay text command still receives its
transport ACK. The identity, saved preferences, membership, learned paths and
durable-replay room history remain. Active sessions, unfinished region input,
held packets, counters and packet capture stop with the previous role instance.
Outstanding transmissions from that instance become uncertain and are not
replayed or counted by the restored role; they may already have reached the modem.
The other roles, native bot, broker and modem connection keep running.
The restored role confirms its source airtime factor before replying again.
Run `make role-lifecycle-test` for isolated restart, failed-commit and admin
actor-recovery checks.

Identity import stays disabled, matching the deployed host policy. Shared PHY
setters always refuse role-local authority.
Received RF airtime and sticky modem error-event measurements are unavailable.
Source-policy statistics also supply the native bot independently.

The migrator retains owner information, read-only/multi-ACK settings, named
regions, discovery save timestamps, coordinates and RTC offset. RX delay and
source airtime factors use the verified modem airtime table and source-policy
confirmation described above. No existing channel/key is synthesized from absent
scope fields.

## Host-only checks

```sh
make management-test
make clock-location-test
make packet-log-test
make native-login-test
make supervision-test
make test-release
```

These use isolated fixtures, not a live radio. The Go differential sends the
same authenticated serialized inputs through the Go radio/Node/application
pipeline and the Hew role. It compares common command text, status, telemetry,
ACLs, CONTROL, neighbour pages and anonymous replies; version branding and
whole-second timing differences are handled explicitly.

`native-login-test` runs the existing `tools.hardware.admin.NativeClient`
against the production relay and room actors through a private loopback KISS
gateway. It checks administrator login, learned width3 paths of 0/1/21 hops,
CLI replies, and an RF-set name retained after a process restart. It preserves
the role's requested transmission delay; it does not model RF airtime or loss.

For a field login timeout, retain the failed attempt rather than treating modem
TX completion as authenticated reception. Check whether the new client's public
key was committed as an administrator in the stopped role state, then correlate
the ANON_REQ and PATH response with the gateway's received data. `NativeClient`
expects raw KISS port-zero data (`00`); it does not accept another logical port.
`ROLE_RX kind=7` records ingress before authentication; `SUBMITTED kind=8`
identifies a queued PATH frame. Neither establishes reception at the client.
Keep credentials and seeds out of diagnostics. A timeout does not by itself
identify a password, path, parser or RF failure.
