# Set up and maintain a Pine node

Pine runs a MeshCore repeater and one optional role on a constrained nRF52840
radio. Start with an initialized **Seeed XIAO nRF52840 + Wio SX1262** and keep
USB recovery accessible. The XIAO application updates retain the installed
bootloader, identities, settings and QSPI data. They do not commission a blank
board or repair missing identity records.

The published [Pine 0.1.0 download](https://github.com/slepp/meshcore-aspen/releases/tag/pine-v0.1.0)
runs the Lua command bot. Room, packet programs, durable companion contacts and
channels, and BLE peer mode require a current source build. There is also a
`nrfmast_solar_lua` source profile for SenseCAP Solar Node P1/P1 Pro; the guarded
serial update helper currently supports the inventoried XIAO only. Solar
owners must use their board's established app-only update and recovery path.

## Update an existing node

1. Read `ids`, `get application`, `get radio`, `get tx` and `get repeat` from
   the existing Pine's local USB console at 115200 baud. Check that the public
   keys and hardware belong to the node you intend to update. Keep its current
   application SHA256.
2. Download a verified, encrypted [node backup](../../NODE_BACKUP.md) and retain
   its private operator seed. A firmware ZIP or companion contact export is
   not a backup of identities, source and settings.
3. Follow [host-verified application updates](HOST-UPDATES.md), using the
   correct board, independently trusted signing key and exact USB serial in
   your private operator inventory. Keep the protected update record for
   interrupted-transfer recovery. Never erase InternalFS or QSPI or replace
   the bootloader to install an application update.
4. Read `ver`, `get application` and the role status below. The application
   hash must match the selected manifest; identities and saved PHY must match
   the pre-update readings. Keep the old verified application package in case
   an explicit application rollback is needed. This bootloader has no
   automatic rollback.

For an unreleased source build, use the normal XIAO profile:

```sh
make -C firmware/nrf52840 build ENV=nrfmast_fleet_lua \
  LUA_ARCHIVE=/path/to/lua-5.5.1.tar.gz
```

The resulting app-only `firmware.zip` is in
`firmware/nrf52840/.build/upstream/.pio/build/nrfmast_fleet_lua/`.
[Build and sign its manifest](HOST-UPDATES.md#build-and-sign-an-application)
with your trusted release key before using the guarded updater. Do not select
the outer download ZIP, an unrelated full-device image or the upstream
baseline repeater image.

## Choose one optional role

Read `room status`, `packet api` and `peer status` before changing selections.
Saved selections take effect after `reboot`; a saved/live difference means
the pending selection has not yet been applied. Use the existing role's off
command and restart before selecting another role. Source and data remain
saved while their worker is paused.

| Role | Select from local USB or repeater administration | Read after restart | What runs with the repeater |
| --- | --- | --- | --- |
| Lua bot | `room enable off`, `packet role off`, `peer off`, then `reboot` | `bot status`, `source status`, `source hash` | Compact command Lua, optional secured BLE companion |
| Room | Local `room init` once, then `room enable on`, `reboot` | `room status`: `saved=1 live=1 Lua=paused` | Native Room, optional secured BLE companion; command Lua paused |
| Packet programs | `packet role on`, `reboot` | `packet api`: `role=1 live=1` | Bounded packet Lua, optional secured BLE companion; command Lua paused |
| BLE peer | [Configure both identified radios](BLE-PEER.md), then local `peer on`, `reboot` | `peer status`: `saved=1 live=1 ready=1` | Virtual repeater bridge; command Lua, Room, packet Lua and phone companion paused |

`peer on`, `peer off` and peer credentials require local USB. Room identity
creation is also local-only; ordinary Room and packet selection can use the
authenticated repeater console. Enabling a conflicting saved role is refused.
Pine has a 48 KiB command Lua allocation limit, no Wasm or IP transport, and
does not run all optional workers concurrently.

Configure [Room credentials before advertising](ROOM.md#choose-the-role).
Install a small [packet program](PACKET-PROGRAMS.md) only after the packet role
is live; no packet program is enabled by default. BLE peer setup needs two
radios and a separately generated shared virtual identity; physical repeater
and bot identities stay stored.

## Check normal operation

Read these commands in the authenticated repeater console or local USB:

```text
get name
get radio
get tx
get repeat
stats sensors
stats radio
stats memory
```

Match the PHY with your companion radio and use legal local frequency and
power. Source defaults are a low-power lab profile, not a regional channel
recommendation. Saved tuning takes precedence. `get repeat` must be `on` if
the node is intended to forward packets. The BAT sensor is battery voltage;
MCU temperature is not an ambient temperature measurement.

In Lua mode, advertise your companion once after Pine restarts, then send
Pine-Bot `!ping` and `!help` as ordinary encrypted DMs. The separate Lua contact
cache is volatile even when BLE companion contacts are retained. Keep native
repeater administration available for source recovery; it does not depend on
a working Lua program. Nearby [BLE pairing](STATE-BLE.md#provision-and-connect-ble)
uses a locally provisioned PIN and grants no repeater administrator permission.

Without fresh GPS time or a retained RTC, synchronize time after restart.
`bot time UNIX_SECONDS` sets trusted Lua and message time; inspect `bot time`
and refresh hourly when using reminders. The Solar profile can obtain time
from its GPS. Retained identities and settings do not imply a retained clock.

## Recover without clearing state

| Condition | Next action |
| --- | --- |
| Lua command fails but repeater administration works | Read `source status`; use `source retry` or `source rollback`, then inspect status/hash. `source remove` selects bundled handlers and retains scoped data. |
| Optional role fails to start | Read its status; save that role off and restart into Lua. Retain unreadable identity or configuration files for recovery. |
| Serial update times out | Inspect the running hash first. If still in identified DFU, use the original signed package and protected record with [`pine_update.py recover`](HOST-UPDATES.md#recover-an-interrupted-serial-transfer). |
| BLE update is interrupted | Follow [BLE recovery](BLE-FIELD-UPDATE.md#aborted-transfer-and-preservation-checks); USB can be required. Keep stable power and physical access. |
| InternalFS, identity or QSPI storage cannot be read | Stop normal maintenance and retain the protected backup. Do not erase storage, generate replacement keys or use destructive Lua provisioning as a routine repair. |

An upload timeout leaves completion uncertain. Check the running application
or exact bootloader before choosing a recovery action; the tools do not replay
uploads automatically. `poweroff` and `shutdown` are refused because SYSTEMOFF
can strand the node; use `reboot` or remove local power when appropriate.
