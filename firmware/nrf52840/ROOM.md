# Run a Room on Pine

Pine's `nrfmast_fleet_lua` and `nrfmast_solar_lua` source profiles can run a
MeshCore Room alongside the native repeater and secured BLE companion.
**Room mode replaces the Lua command worker**, rather than adding another large
role to its remaining heap. Switching modes retains the repeater and companion
identities, Lua source selections, notes and other QSPI data.

Build the application without uploading:

```sh
make -C firmware/nrf52840 build ENV=nrfmast_fleet_lua \
  LUA_ARCHIVE=/path/in/your/project/lua-5.5.1.tar.gz
```

Use the existing Pine application-only update procedure and a verified private
[node backup](../../NODE_BACKUP.md). Do not erase InternalFS or QSPI, replace the
bootloader, or commission a blank board with this profile. The Pine 0.1.0
download predates Room support; these commands require a newer source build.

## Choose the role

Connect to Pine's local USB console at 115200 baud:

```text
room status
room init
room id
room enable on
reboot
room status
```

`room init` creates a new, independent Room identity only if none is stored.
Repeating it leaves that identity unchanged. It requires the local USB console;
an unreadable identity is retained for recovery, never regenerated. `room id`
returns only the Room's public key.

After restart, `room status` should report `saved=1 live=1` and `Lua=paused`.
The repeater keeps forwarding with its existing identity and PHY. The companion
keeps its bot identity and saved contacts/channels. The Lua worker does not run:
custom commands, events and timers do not execute in Room mode. Do not switch
modes while relying on an active Lua job.

Configure the Room before advertising it:

```text
room set name Pocket Room
room password YOUR_PRIVATE_ADMIN_PASSWORD
room set guest.password YOUR_PRIVATE_GUEST_PASSWORD
room advert.zerohop
```

Passwords use the upstream 15-byte limit; keep them out of logs and shell
history. The Room starts with a random administrator password, not the
repeater's password. An empty guest password permits passwordless guest login;
set it before advertising if the Room is private. Automatic Room adverts
default to off. `advert.zerohop` queues a local advert; `room advert` queues a
flood advert. A queue reply is not confirmation of RF reception. A companion on
the same PHY receives the Room advert over RF and can open it in the MeshCore
app using the Room's own credentials.

Use Pine-Relay's authenticated administration or the local console for
`room status`, `room id`, role selection and prefixed Room commands. A Room
administrator can also use the upstream Room command interface directly.
Device-wide reset, erase, identity import, radio tuning and sensor changes are
not Room commands: use the existing repeater administration for supported
device settings. The Room cannot change another role's PHY or identity.

Return to Lua using the local console or Pine-Relay administration:

```text
room enable off
reboot
source status
source hash
bot status
```

The saved Lua source loads again. Room identity, preferences and administrator
ACL remain for the next switch. A selection change takes effect only after
restart; until then `saved` and `live` can differ. If Room state is unreadable
or its allocation fails, Pine leaves that state intact and keeps native repeater
administration available. Inspect `room status`, restore the affected private
backup, or select Lua and restart.

## Limits and retained state

The Room uses MeshCore's Room role from revision
`d92964352441e53b93e8667b802e04f6e072b39e`, with separate persistence paths and
bounded storage. It does not use Aspen's PSRAM layout.

| Resource | Pine limit or behavior |
| --- | --- |
| Room clients | 4 total, including administrators; least-active non-administrators can be evicted |
| Full administrator table | New clients are refused; existing administrators are retained |
| History | 8 posts, each up to 151 text bytes; oldest posts overwritten |
| History after restart | Empty; Room posts and guest sessions are RAM-only |
| Native packet pool | 6 packets shared by Room receive, reply, advert and history work |
| Shared-radio receive snapshots | 2 for the Room; a full snapshot queue drops new arrivals |
| Region entries | 8 |
| Room object and fixed packet storage | At most 16 KiB; `room status` reports compiled object bytes |
| Backup task in Room mode | Separate 6 KiB stack; only created when the existing QSPI volume is mounted |
| Physical heap reserve | 8 KiB minimum at Room allocation; no additional Lua command VM |
| Native history work | One client/post push per scheduled pass; up to 4 ACK timeout checks |

The Room identity (`/_pineroom.id`), mode (`/room-enabled`), preferences
(`/room-prefs.json`), administrator ACL (`/room-s_contacts`) and regions
(`/room-regions2`) live in InternalFS, separately from repeater settings.
Preference, ACL and region writes use temporary files and readback before
rename. Failed saves retain the prior files; ACL background saves retry after
five seconds. `setperm` follows the upstream ACL permissions; only administrator
records are retained through restart.

When the existing Lua QSPI filesystem is available, `backup help` remains
available in Room mode through repeater administration. The encrypted snapshot
includes Room files as well as existing identities, settings, source and data.
If the backup task cannot start, return to Lua mode before maintenance.
Do not publish node backups with firmware images.

## Focused checks

After preparing the pinned source, run:

```sh
make -C firmware/nrf52840 room-test production-lua-radio-test peer-link-test
python3 -m unittest discover -s firmware/nrf52840/tests \
  -p test_production_profile.py
```

The Room test runs the staged native role and checks independent identity
creation, isolated preferences, administrator-table pressure, retained ACL,
failed-save recovery and queued native adverts. The shared-radio test checks
transmission completion and timeout accounting. These commands do not access
hardware or transmit RF.
