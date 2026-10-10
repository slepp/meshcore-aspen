# Pine personal notes and native BLE companion

This guide's note commands and limits describe `nrfmast_fleet` and the other
small native-note images. For `nrfmast_fleet_lua`, use
[production Lua setup and persistence](PRODUCTION-LUA.md); the secured BLE
companion settings below still apply. Primary Lua/source administration is
the normal mobile app → another companion radio → LoRa → Pine administrator
console. Nearby BLE is optional.

Pine runs a MeshCore repeater and a separately addressed bot on its one SX1262.
Send encrypted DMs to **Pine-Bot** to save personal notes. A paired native
MeshCore companion client can also use the **existing bot identity** over BLE.
The repeater keeps its own identity and forwards without BLE, USB, WiFi or a
host process.

```sh
make -C firmware/nrf52840 test
make -C firmware/nrf52840 build ENV=nrfmast_fleet
```

Use the existing Pine identity files; do not generate or import replacements.
The fleet image refuses missing or corrupt `/_main.id` or `/_nrfbot.id`.
All images mount InternalFS without its automatic-format fallback. An
unmountable filesystem stops startup without erasing it.

## Personal notes over RF

Advertise your companion first so Pine knows your full public key. Send normal
encrypted direct messages to the bot, waiting for each application reply.
The development cooldown is **0**; native ACK completion and shared-radio
capacity, rather than a fixed pause, determine when the next command can run:

| DM | Expected reply |
| --- | --- |
| `!remember antenna Spare antenna in the toolbox` | `remembered antenna` |
| `!recall antenna` | `antenna=Spare antenna in the toolbox` |
| `!list` | `notes 1/4: antenna` |
| `!forget antenna` | `forgot antenna` |
| `!recall antenna` after deletion | `Error: notes key not found` |

Remembering an existing key overwrites only that principal's value. Remembering
its identical value or forgetting an absent key is rejected with
`Error: notes no change; write timestamp not committed`. That reply does not
promise a durable timestamp update; use a fresh timestamp for later changes.
Keys are case-sensitive, 1–16 ASCII letters, digits,
`_`, `.` or `-`. Values are 1–96 bytes with no ASCII control bytes. A value can
contain spaces. Replies fit MeshCore's 160-byte text limit.

**Scope is the complete authenticated sender public key plus the complete bot
public key.** Names, channel nicknames, short key prefixes, BLE pairing and
repeater ACL entries do not select or share a principal's notes. Changing a
name preserves access; changing either full key selects another scope. Notes
under an old key remain stored, count against device limits and do not become
readable under a replacement key. Received routes can use any native path
width; originated floods use the existing three-byte paths.

Only the native normal-DM callback dispatches these commands, after destination,
known-contact, decryption and message-MAC checks. Channel messages, CLI-data
messages and signed-message nickname prefixes cannot invoke them. Ordinary
transport-flood/direct messages use the same authenticated-DM boundary.

### Bounds and failures

* **4 live notes per principal/bot pair**, **16 live notes for the device**.
  Nothing is silently evicted. `!forget` frees a live-note slot.
* **8 principal/bot pairs** have durable write timestamps. These records remain
  after the last note is forgotten, preventing an old authenticated mutation
  from recreating a deleted note after restart. New mutations require a
  nonzero sender timestamp newer than that pair's last committed write.
  Reads do not write flash. Synchronize the **sender's** clock and send a new
  DM if Pine reports `stale write timestamp`.
* Quota replies distinguish `principal quota 4 notes`, `device quota 16 notes`
  and `device quota 8 full-key principals`. Anyone who sends an authenticated,
  known-contact DM can consume their own bounded scope; this does not grant
  administration of Pine.
* Accepted writes store one **3,404-byte** note/timestamp snapshot in the next
  4 KiB sector of an external-QSPI ring. Header, payload and footer are read
  back before publishing checked commit markers in the **preceding committed
  sector**, outside the sector being erased. There are **128 sectors**
  in the **512 KiB** note region, `0x180000..0x200000`. A successful mutation
  erases one sector; it does not rewrite identity filesystem metadata.
  InternalFS remains **28 KiB**, holding identities, preferences, ACLs, BLE
  settings and the one-time checked migration marker.
* **Persistent writes have a separate device-wide and full-key-principal
  budget.** At boot, the device can attempt **32** commits and each principal/bot
  scope can attempt **8**. Each bucket replenishes one credit per **15 seconds of
  monotonic device uptime**, up to its initial capacity. A commit attempt spends
  one credit from both buckets **before flash access**, including a failed
  attempt. Exhaustion returns `Error: notes device write budget exhausted` or
  `Error: notes principal write budget exhausted`, with the remaining retry
  seconds and `timestamp not committed`. Reads, `!ping`, invalid/stale/quota
  requests and explicitly rejected no-ops do not spend credits or rewrite
  notes. Sender timestamps and BLE clock synchronization cannot refill credits.
  USB notes reset does not refill them; a device restart restores the RAM
  burst. Eight RAM budget scopes remember full bot/sender keys, including
  failed first writes; a different sender does not inherit a failed sender's
  exhausted bucket. A fully refilled budget slot can be reused. If all eight
  are partly spent, a new scope receives `write principal slots busy` without
  writing flash. Ordinary note senders cannot reboot Pine.
* A staged write or readback failure reports
  `Error: notes commit failed; previous notes retained`. A failure during final
  publication reports `commit outcome uncertain`, disables notes and requires
  a reboot followed by `!recall`/`!list`; **do not replay that write blindly**.
  A torn publication marker or corrupt latest committed snapshot disables
  notes, leaving RF roles running. A partial erase of the ring's oldest sector
  does not invalidate the intact latest snapshot: its checked predecessor
  publication still names the current generation. Restart retains that
  generation and its replay timestamps, and the next mutation can reuse the
  interrupted erase target. Earlier snapshots are not substituted for a
  corrupt latest committed snapshot. Unpublished staging is not adopted.
* There is **no fixed command delay** and one normal DM awaiting its ACK across
  the bot and BLE client. A command received while that slot is occupied,
  without a free reply packet slot, or with physical TX disabled is rejected
  **before note or replay-timestamp mutation**. USB diagnostics name that
  capacity/connection condition. No deferred command is retained or replayed.
  The native packet ACK can still arrive: it confirms packet reception, not
  admission or a note commit. Once admitted, a commit can succeed even if
  physical transmission later fails or the reply's ACK times out. Neither
  requests nor replies are automatically retried; use a fresh request timestamp
  and `!recall`/`!list` to resolve an uncertain result.

### Flash-write cost and the default budget

InternalFS's pinned Adafruit LittleFS **v1.7** uses 128-byte logical blocks over a
4 KiB physical flash cache. Its directory metadata is not moved away from the
fixed root page by ordinary note replacements. A native check links that
actual LittleFS and flash-cache code, models the seven-page InternalFS layout,
and performs the same create/write/close/readback/rename sequence. Across 64
3,404-byte replacements with identity/preferences/BLE fixture files present,
the root page was erased **3–7 times per replacement**, **228 times total**.
Data-page erases are additional.

Pine instead uses the XIAO variant's **P25Q16H 2 MiB QSPI flash**. Startup requires
JEDEC **`856015`**, mounts the existing external LittleFS volume read-only and
traverses its live allocations. If any note-region sector is allocated, the
volume cannot be read, or the chip differs, notes stay disabled and nothing is
formatted or erased. A checked owner-provisioned marker instead retires that
old volume, so subsequent boots do not mount or traverse it. Without that
authority, the external volume remains read-only throughout normal operation.
Moving ordinary writes to another LittleFS v1 file alone would
still concentrate directory wear; the ring has no per-command directory write.

The **32-device / 8-principal** initial credits allow creating and deleting all
16 device / four principal notes without a write-only pause. The **15-second**
refill permits four steady mutations per minute across the device. At that
rate, one external sector is revisited every **32 minutes**, rather than
erasing the internal identity root three to seven times per mutation. Native
NOR tests perform 300 successful commits with exactly 300 erases, distributed
over 128 sectors with per-sector counts differing by one. This is a write-cost
bound, not a flash-life claim. Failed attempts and repeated authorized reboots
also consume endurance.

Run the backend check without touching hardware:

```sh
make -C firmware/nrf52840 backend-test
make -C firmware/nrf52840 native-test
```

### Migration and recovery

#### Explicit lab provisioning of an occupied external volume

The USB owner can retire Pine's old external filesystem and initialize the
existing **`0x180000..0x200000`** note region. **Retired external files may be
lost, and the old filesystem must no longer be used.** Only those 128 sectors
are erased/programmed; external root metadata and all other chip bytes remain
untouched. This is not a whole-chip format or a secure erase of retired files.
CURRENT role keys, preferences, BLE settings and notes/replay records live in
InternalFS and are preserved. Back up both current role keys privately first.

After explicit device-owner approval, use the exact Pine USB by-id connection:

```text
bot notes provision
bot notes provision retire-external migrate-internal confirm
bot state
ids
```

The first command only prints the destructive warning and current readiness.
The full confirmation command requires a valid checked `/pine-notes` record,
including coherent principal replay timestamps. Missing, corrupt or inconsistent
InternalFS notes are rejected before region erasure; they are not interpreted
as an empty store. It writes and reads back a checked **owner-intent** marker,
erases and verifies every byte of the defined region, imports the complete
note/principal snapshot, then writes and reads back **completion authority**.
Only then can note commands run. Success reports `notes=ready`,
`notes_jedec=856015` and **`notes_guard=retired`**; recheck the full identities
and saved configuration.

An interrupted operation leaves notes disabled. Startup never resumes erasing
or imports an empty replacement. Inspect the reported condition after reboot;
if it says provisioning is interrupted, repeat the same full owner confirmation
to reimport the still-checked InternalFS record. An uncertain completion can
already have committed its authority: reboot and inspect readiness rather than
blindly repeating. **Completed provisioning rejects repeats**, preserving later
notes and replay timestamps. A missing or corrupt provisioned journal is not
replaced from the stale InternalFS copy. An invalid owner marker never grants
permission to bypass the old allocation guard.

The existing 28-byte checked `/pine-notes.qspi` marker records normal migration
(version 1), normal reset-in-progress (2), owner provisioning-in-progress (3),
owner-provisioned completion (4), or provisioned reset-in-progress (5), bound to
this exact region and payload size. The separate **`bot notes reset confirm`**
retains its intentionally destructive note/replay-reset purpose. A provisioned
reset preserves retirement authority across interruption and completes back
to version 4. It cannot replace pending owner provisioning.

On the first successful start, Pine imports the checked `/pine-notes` record
and its retained replay timestamps into QSPI, verifies the published snapshot,
then atomically writes `/pine-notes.qspi` in InternalFS. No command mutation is
admitted before that marker succeeds. A restart between these steps resumes
the published snapshot rather than repeating the QSPI erase. The old InternalFS
record is retained but never restored as a fallback after migration. An erased,
missing or corrupt migrated journal requires explicit operator recovery.

The ring uses **`PNJRNL2`** snapshots. The first snapshot publishes itself;
each later snapshot is prepared in the next sector, then published by a checked
32-byte slot/generation descriptor in its predecessor. That descriptor is
outside the erase target and remains the boot-time authority if the latest
snapshot's header, footer or payload is damaged. A prepared next snapshot
alone cannot advance replay timestamps. A complete newer snapshot without a
valid publication is ambiguous, including a lost publication descriptor;
notes fail closed rather than treating a damaged committed snapshot as
unpublished staging. A torn descriptor also remains a storage error, not an
older-state fallback. Incomplete staging before these publication phases is
ignored and the next new mutation can reuse its sector.

**Do not downgrade to firmware that writes the original full-size external
LittleFS volume or the old InternalFS notes record:** it can overwrite the
reserved sectors or create a separate, stale replay history. Do not format
the external chip or erase InternalFS. Application-only DFU retains role keys,
preferences, BLE settings and the note ring. Back up both role keys privately
before flashing; inspect the startup QSPI diagnostic and `bot state` afterward.

`bot state` reports `notes_jedec`, `notes_guard`, `notes_mount_result`,
`notes_traverse_result` and `notes_overlap_blocks`. `clear` with zero mount and
traversal results means the existing volume was readable and had no live
allocation in the note region. `occupied` reports overlapping live blocks;
`unreadable` reports the failing filesystem result. `not-checked` means the
chip/region check was not reached. `retired` means a checked USB-owner marker
authorizes the fixed note region; the old filesystem mount/traversal was skipped,
so its result fields do not describe current legacy-volume readability. Stop
before note mutations unless `notes=ready`, `notes_jedec=856015` and the guard is
`clear` or `retired`. Do not format the chip or use notes reset to bypass an
allocation/readability failure.

For a readable but occupied volume, USB **`bot notes volume`** lists allocated
4 KiB block addresses, directory metadata pairs, and filenames, sizes and final
data-block addresses. It opens files read-only for their metadata; it does not
read or print file contents, format the volume or change either filesystem.
The final line reports mount, traversal and scan results. Treat the inventory
as incomplete if any result is nonzero or `limited=1`: it allows up to eight
subdirectories, 32 entries and 95-byte paths, and rejects non-printable names.
Compare the allocated blocks against a proposed note region before changing
its bounds. This command does not change the fixed note region or bypass its
startup guard. Inventory is disabled once checked owner intent/completion has
retired the volume.

Notes and historical snapshots are plaintext in local flash, not encrypted at rest or securely erased
by forgetting a key. No note values or key list are copied into the BLE inbox,
USB logs or repeater administration replies. `bot state` reports only readiness,
aggregate note/principal counts, limits, filesystem usage and BLE inbox drops.

The USB-only command **`bot notes reset confirm` erases every principal's notes
and replay timestamps**, including scopes under retired bot keys. It does not
change either role identity or the radio. Use it only when that device-wide
loss is intended; it is the operator recovery for corrupt note state or exhausted
principal records. It writes a reset-in-progress marker before erasing only the
note region, so a power interruption cannot silently restore an earlier replay
watermark. An interrupted reset leaves notes disabled; repeat this explicit USB
command to finish. It cannot bypass the chip or live-volume allocation guard.
Retired InternalFS copies are removed logically, not securely erased.

## Provision and connect BLE

BLE starts disabled and has no default PIN. With physical access to your
radio, use its native 115200-baud USB CLI:

```text
get bot.ble
set bot.ble.pin <your-six-digit-PIN>
set bot.ble on
reboot
```

Choose **100000–999999**; `123456` is not a compiled default. Keep the actual
PIN out of command logs and committed configuration. Then pair a native
MeshCore companion client with **MeshCore-Pine-Bot** using that PIN. The native
self-info handshake reports Pine-Bot's existing full public key, its saved
name and the actual shared radio profile. It does not expose Pine-Relay as a
second BLE endpoint or create a phone-specific RF identity.

PIN and enable settings persist in a separately verified, atomically replaced
InternalFS record. A corrupt BLE record disables BLE without stopping either
RF role. `get bot.ble` never prints the PIN. RF administration cannot provision
BLE; use USB or the native PIN-change command over an already paired link.
Enable and PIN changes apply after reboot, without rebuilding firmware. The
native paired PIN command returns its usual OK after saving; reconnect after
the next reboot with the new PIN.

**Bonds are cleared on each boot.** Pair again, forgetting the old OS pairing
if necessary. This prevents an old bond from bypassing a saved PIN change.
`set bot.ble off` followed by `reboot` disables BLE while preserving notes,
names and both keys. Bot name changes apply to RF immediately; the BLE
advertising label is refreshed at reboot.

Pairing authorizes the listed bot companion operations. It does **not** add
the bot key or the phone to the repeater's native administrator ACL. To manage
a different repeater over RF, explicitly authenticate with its password or an
ACL grant to Pine-Bot's full key. Pine does not locally reflect its own radio
transmissions: a phone connected as Pine-Bot cannot send a radio DM to itself
or use RF loopback to administer its co-located repeater.

### Native companion interface

The image reuses the pinned upstream nRF `SerialBLEInterface`, Nordic UART
service, encrypted/MITM pairing, secured-connection gate and frame transport.
The protocol is MeshCore companion **v13**, from
`companion-v1.17.1` / `d92964352441e53b93e8667b802e04f6e072b39e`.
Frame numbers are extracted from that pinned native source. This is a bounded
implementation of its existing commands, not a separate phone protocol.
The upstream BLE DFU service is omitted so pairing does not grant firmware
replacement or reset access.

The table below describes the smaller native-note images. Production Lua adds
the persistent companion operations in the following section; it retains the
same secured BLE connection and shared-radio authority.

| Supported native operations | Bounds/consequences |
| --- | --- |
| Device query, self-info, clock read/sync, battery/storage | Self-info follows actual repeater-driven PHY changes, including timed temporary tuning. Clock sync cannot move time backwards. |
| Contact sync, full-key get/update/remove, path reset, signed advert import, self export | Eight RAM contacts; chat/repeater/room discovery. One imported advert may await the bot loop; another import returns native `ERR_CODE_BAD_STATE` until it is processed, without consuming another packet slot. Contacts/routes need rediscovery after reboot. Duplicate six-byte destination prefixes are rejected rather than selecting an arbitrary contact. Received advert blobs are not retained for export/share. |
| Normal encrypted DM and CLI-data send/receive | Same bot key, native encryption and routing; one normal send awaiting ACK. Four ordinary incoming messages retained in RAM, with explicit USB drop count when full. Note command traffic is not mirrored to this inbox. |
| Native remote login, status request and encrypted CLI replies | One outstanding login/status transaction, matched to a full remote key; remote native permissions apply. No automatic login or grant. |
| Bot advert and saved bot name | Zero-hop by default or requested three-byte flood advert. Queued transmission does not confirm RF reception. |
| Native PIN change, auto-add/custom-variable readbacks, tuning/statistics read | PIN is saved for reboot; auto-add policy is bounded and fixed. Physical RX/TX/error counters describe the shared modem; path-type counts and airtime describe the bot dispatcher. |

Radio/TX/tuning/path-policy writes, private-key import/export, signing services,
factory reset, reboot and channel operations return native `RESP_CODE_DISABLED`.
Unknown unsupported commands return `ERR_CODE_UNSUPPORTED_CMD`; malformed
supported frames return `ERR_CODE_ILLEGAL_ARG`.

**Shared radio authority remains with the repeater's USB or authenticated,
encrypted native administrator CLI.** Use existing `get/set radio`, `get/set tx`
and `tempradio` there. Both RF roles follow the single modem; BLE has no second
radio preference record and cannot silently retune it. Repeater names,
passwords, permissions and forwarding policy retain their existing native
storage. No Lua, WiFi, HTTP or host process is needed for notes or BLE.

### Production Lua companion persistence

On `nrfmast_fleet_lua` and `nrfmast_solar_lua`, enable and pair the companion
using the PIN procedure above. The existing bot/carrier identity now retains:

- Eight contacts, their outgoing routes, favourite flags and epoch-based
  advert/message-sync metadata. The separate 16-entry Lua contact cache is
  still volatile; this does not merge the two tables.
- The most recent cached signed advert for each retained contact. Native
  contact export and zero-hop share use that advert after restart. Received
  route bytes are removed from the cached advert without changing its signed
  payload. Manually entered contacts have no signed advert until one arrives.
- Four named 128-bit companion channels, initially empty. The normal app's
  channel get/set and channel-text send/receive commands work. A channel change
  does not join or grant access to a Lua bot membership. Group-data commands
  remain disabled.

The app still sees companion protocol v13. No new identity, administrator
secret, BLE permission or radio-tuning authority is introduced. Paired users
can read/change **companion** channel keys, as in the native companion protocol;
they cannot obtain private identity keys or administer the repeater/Lua source
merely by pairing. Channel send success means queued, not RF reception.
DM and channel messages share the existing four-message volatile inbox.

Explicit app contact/channel edits, removals and path resets are saved before
success is returned. Storage failure leaves the old live selection; an
uncertain commit blocks further mutations until restart. Learned RF contacts,
adverts and route changes are grouped into a save no earlier than 30 seconds
after the first change, including while the phone is disconnected. Those
unsaved updates can be lost to a power cut. Writes share eight initial attempt
credits and replenish one every 15 seconds of monotonic uptime; rejected
mutations do not silently queue. Reconnect/read back after a failure instead of
assuming an edit took effect.

The 2,888-byte versioned, identity-bound record lives at
`/metadata/pine-companion/state`. It uses the existing checked metadata staging
and atomic rename and is included in encrypted node backups. Corrupt or
unreadable records are retained and disable durable edits rather than being
replaced with empty state. `get custom variables` in the companion protocol
reports the storage condition; USB startup also reports load failures.
Saved epoch timestamps are not used to grant fresh clock trust. Connections,
pending ACKs, uptime deadlines, shared-secret caches and inbox messages are
not restored.

Only an enabled production companion allocates its 2,888-byte retained
snapshot; a save temporarily needs a second snapshot plus filesystem buffers.
The channel table is four entries. Lua keeps its existing 48 KiB quota and
8 KiB physical-heap reserve; allocation or storage pressure can reject a
companion edit. Neither contact table is enlarged.

**Destructive `bot lua provision erase-lua confirm` also erases this companion
state.** Ordinary source install/rollback, application-only updates and device
restarts do not.

Run the focused protocol and QSPI lifecycle checks without hardware:

```sh
make -C firmware/nrf52840 companion-state-test
python3 -m unittest discover -s firmware/nrf52840/tests -p test_production_profile.py
```

## Check a configured node

Select the radio's stable application and DFU USB paths and schedule a
maintenance window. Record the two public identities, names, radio settings,
forwarding and path width. Keep its private backups locally.

1. Update the selected application through serial DFU, preserving InternalFS
   and QSPI. Native notes use `nrfmast_fleet`; production Lua uses
   `nrfmast_fleet_lua` and its [update prerequisites](PRODUCTION-LUA.md).
2. Read `ids`, names, `get radio`, `get tx`, `get repeat`,
   `get path.hash.mode`, `mem` and `bot state`. Compare the two full public keys
   and saved settings with the pre-update record.
3. Provision your six-digit PIN, enable BLE and restart. Synchronize time,
   connect a companion client, read contacts and exchange a DM with a second
   radio. Check the received message and its acknowledgement.
4. Exercise remember, recall, list and forget from two authenticated callers.
   Check that each caller's notes survive restart in its own scope.
5. Observe forwarding, free heap and loop-task stack headroom during note
   writes with BLE connected and disconnected.

### Connect from a phone

Use a MeshCore companion application on a Bluetooth-capable phone.
Keep the provisioned PIN private.

1. Scan for **MeshCore-\<bot name\>**. Check rejection with an incorrect PIN, then
   connect with the provisioned PIN.
2. Confirm companion v13, the saved bot name and its recorded full public key.
   Check the displayed shared PHY and synchronize the clock if needed.
3. Advertise, discover an independent companion, and exchange a normal DM
   in both directions. A message to Pine-Bot itself is not an RF loopback test.
   Shared-PHY writes and private-key operations must remain disabled.
4. Observe repeater forwarding and `mem` with BLE connected, then disconnected.
   If Pine restarts, forget the old OS pairing and pair again: bonds are cleared
   on each boot. Recheck names, both public keys and your configured PHY.

## Lua development

Use [production Lua](PRODUCTION-LUA.md) for scripting and its allocation,
parser-stack and runtime checks. To measure the Lua core's link footprint
separately, build the development probe:

```sh
make -C firmware/nrf52840 lua-probe \
  LUA_ARCHIVE=/path/to/lua-5.5.1.tar.gz
```

The Lua 5.5.1 archive must match SHA-256
`1c4b4068d67061f2a2231ad2b5422e77acea1487ea9890f6320af614f4373dce`.
The probe is build-only. Flash the production Lua image for command handlers,
storage and RF operation.
