# Move Birch roles to Willow

Willow can take over the relay, room, native bot and observer identities from a stopped
Birch snapshot. The helper reads the snapshot without writing its files and
stages a **new** private directory. It does not connect to the modem.

Keep the shared modem, its configuration owner and Base companion service
separate. During transition, disable Birch's relay, room, bot and observer before starting
Willow. Never run both hosts with those same role identities.

For the existing Go compatibility service, the selected-role configuration is
`enabled_roles=["companion"]` with `bot_runtime=""`,
`bot_native_worker=""` and `bot_companion_listen=""`. Set
`radio_session="per_role"` on that compatibility service: the shared modem has
one aggregate MKISS session, which Willow uses for its four logical ports.
Base uses one independent TCP connection. Preserve the separate PHY
configuration owner, Base identity and existing endpoints. If the Go observer
previously owned PHY configuration, reassign that responsibility explicitly:
Willow only verifies readback and never configures the modem. A newer Go configuration loader
requires `companion_allow_remote=true` for an intentionally existing
`0.0.0.0:5000` listener; this is not an instruction to create a new exposure.

The separate [Hew Base candidate](BASE.md) uses `migrate_base.py stage` and
`rollback` on frozen companion role directories. That helper preserves the
Go-compatible authority envelope and all retained companion state in new
private directories. It does not change the four-role migration procedure or
authorize stopping the existing Base service.

## Build and inspect

From this worktree:

```sh
cd experiments/hew-roles
make native-worker all
make migration-test
make readiness-test
```

Use a frozen private copy of the stopped Go state, not its live directory.
The expected active role directories are `repeater/`, `room/`, `bot/`, and
`observer/` when migrating the observer.
An existing host `.lock` must be available exclusively. A snapshot without a
lock must still have been frozen by its operator.

Inspect the saved schema before runtime credentials are available:

```sh
python3 -B migrate_go.py --source /path/to/frozen-birch --schema-only
```

This checks active identities, role conversion and committed native storage,
without printing identities or creating a destination. It does **not** validate
runtime passwords, modem settings or unsaved scope settings.

An identified inactive archive beside the role directories can be retained with
`--retain-directory ARCHIVE_DIRECTORY_NAME`. Repeat that option during staging.
The archive is copied only to `rollback-go/`, never selected as active state.
Compatibility roles, including `bot_companion`, are retained separately.
Unknown directories require explicit identification; do not delete them to pass
inspection. Active files are bounded at 32 MiB, explicitly retained archive files
at 128 MiB, and the complete snapshot at 128 MiB.

Create a mode-0600 private Willow configuration file. Supply:

| Setting | Required value |
| --- | --- |
| `address`, `port` | Shared modem's numeric IPv4 address and TCP port. |
| `profile` | Exact 18-byte CONFIG readback in hex. This is checked, never applied to the modem. |
| `password`, `admin` | Existing runtime credentials, unless saved overrides or explicit `relay.password`, `room.password`, `relay.admin`, `room.admin` supply them. |
| `bot_home` | Explicit native bot home key in hex, or empty if deliberately disabled. Home is not an outgoing default. |
| `bot_default` | Explicit outgoing default key in hex, or empty if deliberately disabled. |
| `observer.enabled` | `1` to move publishing to Willow; `0`/absent keeps the publisher disabled. |
| `observer.url` and other `observer.*` settings | Copy the frozen Go `mqtt` configuration using [the service setting names](SERVICE.md#native-mqtt-observer). Do not use a different topic prefix, format, audience or filter unintentionally. |

The observer follows Go's identity authority: active identity/state envelope,
then `identity.expanded`, then `identity.seed`. Migration stores the selected
64-byte scalar/prefix as `observer.expanded`, or a selected 32-byte seed as
`observer.seed`, even when publishing is disabled. `migration.json` records
its public key and `identity_format` (`expanded` or `seed`). A stale auxiliary
Go seed never replaces an active expanded identity. Native public-key
derivation and JWT signing use the selected key directly.
All original observer/Base files, including stale auxiliary identities,
remain byte-for-byte in `rollback-go/`. Malformed/pending active identities
and nonempty observer application state are refused. MQTT queues are in
memory in both hosts and are not migrated.

The runtime configuration is supplied explicitly, not read from the Go state
directory. Translate Go `mqtt.packet_filter` to a decimal uint16 mask and use
`observer.firmware_version` for its firmware metadata. Preserve credentials in
the private configuration file. For the current CAD-on profile use
`profile=c806643690d003000705020000803f010000`: 912.525 MHz, BW250 kHz, SF7,
CR5, TX2, factor1, CAD1, threshold0.

Do not put credentials, seeds or private channel keys in command arguments.
The helper imports the native channel name, key, three-byte path width and
airtime budget directly from committed NVS. Conflicting configuration is
refused. If a saved native `scopes` file exists, the supplied home/default must
agree with it. Without that file, obtain these settings from the frozen Go
configuration or private inventory; they cannot be inferred from a radio key.

Names default to `Willow-Relay`, `Willow-Room` and `Willow-Bot`. Optional
`relay.name`, `room.name` and `bot_name` override them without changing keys.
The native channel retains its saved name; do not rename it during migration.

```sh
python3 -B migrate_go.py \
  --source /path/to/frozen-birch \
  --config /path/to/private-willow-config
```

This inspection prints public identities, conversion counts, a source
fingerprint and worker hash. It creates no destination. Fix any reported
unsupported feature before continuing; do not delete source data to make the
check pass.

## Stage and start

Choose a destination that does not exist and is disjoint from the source:

```sh
python3 -B migrate_go.py \
  --source /path/to/frozen-birch \
  --config /path/to/private-willow-config \
  --destination /path/to/new-willow-state
python3 -B willow.py check --state /path/to/new-willow-state
python3 -B willow.py run --state /path/to/new-willow-state
```

For the optimized executable, build `make build/hew-host-release` and add
`--release` to the runner command.

The new tree uses mode-0700 directories and mode-0600 files. `migration.json`
records source/output hashes, public identities and the verified native worker.
`rollback-go/` retains the complete frozen source file contents, including
legacy state and compatibility-role data. The source is rechecked during
inspection and staging. An interrupted destination is never reused: retain it
for inspection and choose another new destination.

Before its first start, the runner checks every recorded output hash. On every
start it checks imported identities and worker binding. Missing migrated seeds
are an error, never a request to generate replacements. Runtime state changes
are allowed after the first-start marker.

Watch for `ONLINE`, `BOT_READY`, `OBSERVER_READY` and
`OBSERVER_MQTT ... connected=true`; check the names and unchanged public keys
from a companion radio. Stop with SIGTERM. The service closes its transports
and native worker and releases its lock.

## What survives

* The active identity follows Go's authority order: identity/state envelope,
  expanded identity, then seed. Relay/room/native bot still require a retained
  seed deriving that exact active identity; the observer also supports native
  expanded keys. Legacy rollback documents never override an active envelope.
* Relay/room ACL order, permission bytes, replay stamps, sync cursors, known
  paths, saved `Attempt` bytes, role clock and posted/pushed counters are converted
  into Hew snapshots. The saved attempt byte is retained metadata, not a pending
  transmission: Go's current delivery path also chooses a fresh random attempt.
  Durable-replay history is preserved in order, including authors, timestamps
  and raw text bytes. Subsequent accepted posts remain durable. Active
  `state.json` is used, never an older `state.v1.json` backup.
* Named regions retain insertion order, IDs, parents, flags, home/default
  selection and up to four private transport keys per entry. The table holds
  32 entries. Owner `region` commands and their saves survive restart in the
  role snapshot, alongside wildcard admission, forwarding flags, hop limits,
  retransmit factors and advert intervals. All four saved relay loop policies
  are preserved; `loop=0/1/2/3` means off/minimal/moderate/strict. Each role
  retains its saved path hash mode. Home is management metadata; adverts use
  the outgoing default scope, matching the host role's current behavior.
  A missing saved region registry does not establish a home region:
  reconcile the operator's private inventory before choosing runtime scopes.
* Relay/room latitude, longitude, advert-location mode and signed RTC offset
  are preserved. Coordinates retain binary64 precision; outgoing adverts
  contain signed integer microdegrees only when `gps advert prefs` is enabled.
  The RTC offset changes role protocol time, not host or modem scheduling.
* Existing relay/room `packet.log` bytes are copied to the corresponding native
  role log. Reconciliation returns the latest log, including an intentional
  erase, to the new Go tree. Logs are limited to 4 MiB. Capture itself is
  disabled after a role or host restart in both hosts; retained logs do not
  automatically enable it.
* Native NVS and SPIFFS records retain source, packages, grants and private
  application data byte-for-byte. Startup uses `native_setup=preserve`: no
  setup worker, default source reinstall or channel/grant reset occurs.
  Before source startup, the existing native policy writer changes only the
  bot's mesh name. The migration test compares all other NVS records and
  SPIFFS files and runs the retained Lua note and Wasm command.
  A saved 3600 ms/minute policy remains 3600; migration does not apply the
  fresh-service default or demo settings.

Native interpreter deadlines are unchanged: the existing 20 ms invocation
wall budget still applies. An interpreter budget error remains an error; the
service does not fabricate a response or raise the budget to hide scheduling
load. Willow and Go use the same bound native worker contract.

## Refused data and remaining service work

The helper currently refuses more than 32 retained posts or 20 members;
pending identities; relay/room/bot expanded identities without a matching seed;
unsupported counters or paths; more than 32 regions or four private keys per
region. Receive-delay factors in 0..20, transmit-delay factors in 0..2 and
finite, nonnegative source airtime factors retain binary32 precision.
The saved native or durable preference profile is preserved: native startup
clamps airtime factors above 9 to 9, while the durable profile retains them.
Invalid coordinates,
advert-location modes or clock offsets are refused. Unknown fields/files, unfinished native writes and malformed
native records are errors, not silent resets.

Willow supports [RF status, telemetry, discovery and role administration](MANAGEMENT.md).
RF changes to supported settings persist in HEW6. The helper refuses invalid
or unrepresented data rather than replacing it with defaults.
Base companion continues outside Willow in Go. The observer's MQTT output is
native Hew when enabled.

## Upgrade a frozen Willow state

Build the new release and worker in a **new final installation directory**;
do not overwrite an installation used by a running service. Use that version's
launcher to rebind a new copy of the complete stopped state:

```sh
cd /path/to/new-pinned-root/experiments/hew-roles
python3 -B willow.py rebind --source /path/to/frozen-willow \
  --state /path/to/new-willow
python3 -B willow.py check --state /path/to/new-willow
```

First stop Willow, disable automatic restart, and wait for its native worker
and all NVS/SPIFFS writers to exit. Freeze the whole private tree only then.
Rebinding checks the available service lock and rejects sockets, nonprivate
files and unfinished `.pending` transactions. Locks/hash checks do not make
copying a running tree safe. A clean host shutdown removes `owner.sock`; after
a crash, recover/stop the original owning version before freezing.

The command never opens the modem or changes its source. Only `config`'s
absolute `worker` setting and `migration.json`'s binding/config hash change.
Seeds, active expanded observer identity, snapshots, native program/NVS/SPIFFS,
owner request ledger and `rollback-go/` stay byte-for-byte. It does not require
the retired installation to remain executable. The new installation's worker
and inputs must verify. The completion record is `migration.json` with
`mode="staged"` and `rebind.version=1`; stdout reports `mode="rebound"` only
after the new state passes `check`. Do not promote an interrupted candidate.

After validation, point the stopped unit's executable and state path at the
new installation/candidate, then start that one owner. Keep the old frozen
state and installation. No extra FFI environment settings are required.
Base-only Go and the independent observer broker need not restart for a
Willow-only upgrade. After new activity, rolling back must reconcile that
latest state; restoring the pre-upgrade state loses durable writes.

Before changing an installed service, check recovery using synthetic state:

```sh
python3 -B tests/upgrade.py \
  --reference /path/to/old-pinned-root/experiments/hew-roles
```

This loopback check creates a new room post, owner DM ledger entry, native note
and admin request clock in the candidate, then rebinds into a **new** directory
using the previous installation. It checks the previous host can read those
writes and separately runs the Go recovery application. If the previous Hew
version cannot read the current state, use the checked Go recovery path instead.
It does not open a physical modem or copy running node state.

## Rollback

Return to Go with `reconcile_go.py`, not by restoring `rollback-go/` alone.
The original snapshot predates new notes, posts, member changes and replay
timestamps. Even an authenticated read-only query can advance a replay stamp.

1. Stop Willow cleanly, wait for its native worker to exit, and disable any
   automatic restart. Keep its complete state directory, including `config`,
   `migration.json`, `rollback-go/`, every seed/snapshot and `native/`.
2. Stop the Go Base-only service and any other writer of the current Go tree.
   Freeze private copies of **both** trees. A directory copy while either
   writer is running is not a coherent snapshot.
3. Reconcile into a destination that does not exist:

   ```sh
   cd /path/to/current-pinned-root/experiments/hew-roles
   python3 -B reconcile_go.py \
     --source /path/to/frozen-final-willow \
     --go-source /path/to/frozen-current-go \
     --destination /path/to/new-go-candidate
   ```

`--go-source` preserves newer independent Base/companion files. It refuses
competing changes in any transferred role directory, including stale legacy
files: do not run the old relay, room, bot or observer during handover.
Omit this argument only when the pre-transition Go tree is still the complete
state you want for independent roles.

The command opens no modem or broker connection, changes neither input tree,
and never selects the candidate for a running service. It checks available
writer locks, source hashes and native record integrity, then writes mode-0700
directories and mode-0600 files. A successful candidate has
`willow-reconciliation.json` with `mode="reconciled"`, file hashes and role
counts. A failed/interrupted candidate without that completion record must
not be run; retain it for inspection and choose a new destination.

### Returned state and configuration

* Retained seeds and active identity envelopes remain authoritative. A legacy
  `state.json` beneath an active envelope stays unchanged; the envelope's
  `state` receives the updates.
  An expanded observer key is checked against the original active Go key;
  reverse conversion preserves both that original authority and any stale
  auxiliary Go seed exactly.
* Relay/room ordered membership, permissions, replay timestamps, confirmed
  sync cursors, learned paths, attempt bytes, clocks and counters come from
  the final committed Hew snapshots. Retained history includes its authors,
  timestamps and UTF-8 or raw text. Saved names, credentials, read-only/multi-ACK
  settings and supported preferences take precedence over startup defaults.
  Receive-delay factors are retained; waiting inbound packets are runtime-only
  and are never replayed during conversion or rollback.
* The final named-region table replaces the original registry, including
  intentional removals. Names, IDs, parentage, flags, private key rotations,
  home/default selection, next ID and discovery modification time follow
  the committed snapshot. Edited raw scope settings apply over the migrated
  baseline; unchanged startup configuration does not erase later owner
  changes. A new raw region key receives a private `$willow-...` name and unused ID.
* Fields not represented in Hew remain from the original Go document,
  including surviving members' additional fields. Opaque independent files
  remain byte-for-byte. This preservation does not make an older Go binary
  understand future fields: Go rejects unknown role-document fields and can
  normalize supported fields during its next save.
* The complete final `native/` replaces candidate `bot/native/`, after NVS,
  SPIFFS and policy validation. Updated/added/deleted notes, source, packages,
  grants, scopes and name policy follow the final tree; deleted files are
  not resurrected by unioning it with the old one.
* The owner request ledger becomes private `willow-owner.state` in the Go
  candidate. Go does not operate this file; a later forward migration restores
  it as `owner.state`. Thus a host transition does not make an old request ID
  eligible for retransmission. Use the converter from the owner-control
  release: earlier converters intentionally reject this new state file.
  Inbox plaintext is volatile and is not part of either snapshot.

**Reconciled relay/room documents use `durable-replay`.** Go saves immediately
on startup; native retention would otherwise delete returned replay stamps
and non-admin room membership. The completion record reports previous and
returned retention. Leave Go's retention override unset or use
`durable-replay`; do not override recovered preferences with old startup
policy. Active sessions and pending transmissions are not restored: clients
must log in again.

Posts accepted under **native retention** were never saved. The converter
cannot recover them and refuses a native-retention room whose post counter or
clock advanced after migration. For a trial requiring retained history, the
source Go room must already use `durable-replay`; forward migration preserves
that setting. Ordinary bounded-history eviction remains bounded in both hosts.

Go's saved management passwords are limited to 15 bytes, while its runtime
configuration permits 63. Short credentials become independent per-role
saved overrides. Longer credentials are returned in the **private**
completion record's `required_go_runtime`; command output prints only their
key names. Apply `room_password` to the Go room password configuration and
`admin_password` to the environment variable named by `admin_password_env`.
Do not print these values or put them in command arguments. Conflicting long
credentials that cannot share Go's common runtime settings are refused,
never truncated.

Retain the original Go modem/MQTT configuration separately: those connection
settings are not Go state files. Restore the observer's retained identity and
appropriate private configuration, not its volatile MQTT queue. Keep Willow
stopped while validating/selecting the candidate, and never run the same role
identity on both hosts. Use a disposable candidate copy for validation that
will mutate Go state; retain the reconciled original until promotion succeeds.
The helper does not restart services or change the chosen Go state path.

Missing/pending identities, modified rollback originals, unknown Willow
files/settings or snapshot versions/tails, unfinished transactions, regressed
clocks/counters, nonrepresentable preferences and all-Hew bot activity are
refused. Do not delete data to bypass a refusal. The original pre-transition
Go tree and frozen Willow tree remain available for recovery.

## Isolated checks

`make migration-test` creates actual Go saved-role fixtures and native Lua/Wasm
state in private temporary directories and removes the
synthetic identities afterward. It checks frozen-source hashes, active-envelope
authority, replay/ACL restoration, lock and path refusal, startup tampering,
name-only NVS changes, three-byte scoped adverts, native scoped/unscoped
replies and relay scope admission. A current eight-member/twelve-post room and
four-administrator relay take precedence over deliberately older backup files.
Eleven retained deliveries are compared with actual Go frames, with ACK-confirmed
cursors, raw-byte and maximum-length posts, restart, and new durable posts.
Saved attempt bytes survive load/save/restart; malformed attempt tails fail
without replacing live state. Schema-only inspection cannot stage sentinel
credentials. An explicitly retained 33 MiB archive and a compatibility identity
remain intact, while active-role/archive confusion is refused.
No hardware or live profiles are used.

`make reconciliation-test` runs one localhost lifecycle using the release
service and native worker: import actual Go state, replace/add native notes,
change relay/room preferences, add a room member and post, receive and ACK
history, change region/home/default settings, then restart Willow. It builds
a new Go candidate with a newer independent Base file and starts the actual
Go application against a localhost modem. Authenticated relay/room queries
and both retained-note reads must succeed; saved history and the other member's
confirmed cursor must survive Go's startup save. The same lifecycle checks
untouched semantic conversion and refusal of a live writer, malformed tail
and competing Go writes. It contacts no existing modem or MQTT endpoint.

For a stopped snapshot and its newly staged Willow tree, verify the selected
private-key operations against Go without starting either host:

```sh
make build/identity-bindings build/oracle
python3 -B tests/identity_bindings.py \
  --source /path/to/frozen-go --staged /path/to/new-willow-state
```

This compares relay, room and observer public keys, deterministic signatures
and ECDH-derived ciphertext using the production crypto boundary and Go's
identity loader. Go reads disposable private identity copies; the source and
staged trees are unchanged. Output contains public identities and match results,
not private keys or shared secrets.
