# Repeater, Room and companion controls

Open the Repeater or local Room contact in a companion application, log in as
its administrator and use that contact's console for role-local settings.
Use the separate **Management** contact for the shared radio, other roles,
bot/packet programs and network services. Start with
[choosing a command endpoint](../../COMMANDS.md).

These commands describe Aspen's on-device native roles. Host roles and Pine
have their own [host](../../HOST_GUIDE.md) and
[Pine](../nrf52840/PRODUCTION-LUA.md) controls. All roles on Aspen use the same
physical frequency, bandwidth, SF, CR and TX power.

## Connect and inspect

The Repeater and Room each have an administrator password and native ACL.
In the app, authenticate to that role and check that it reports **Repeater
Admin** or **Room Admin**. A Management grant does not grant either role's
console, and a role administrator does not become a Management administrator.
Room guest login is separate from Room administrator login.

The Python client can use that native console over encrypted RF:

```sh
python3 tools/hardware/admin.py \
  --gateway COMPANION_KISS_HOST --target REPEATER_PUBLIC_KEY64 \
  --seed-file /absolute/private/companion.seed \
  --password-file /absolute/private/repeater-password \
  --untagged --no-command-retry command 'get rxdelay'
```

Use the app's Room login/console for the local Room. Management's web console
is not a proxy to either role's console.
Password/seed files are mode 0600. Match the existing shared PHY on the sending
companion; this command never retunes it.

Start with `ver`, `board`, `get name`, `get public.key`, `get role`,
`get radio`, `get repeat` and the relevant delay settings. Native console
settings return `> VALUE`; successful setters normally return `OK`.
An `Err`, `Error` or unknown-command response is a failure.
Native getters use MHz/kHz in `get radio`; Management's `radio` setter instead
uses integer Hz. Keep those units separate.

## Shared role settings

`get FIELD` reads a role preference; `set FIELD VALUE` updates it. Preference
changes remain saved across application-only updates and restarts.
Use full argument values and inspect readback after each change.

| Field / command | Parameters | Effect |
| --- | --- | --- |
| `get name`, `set name TEXT` | Display name up to 31 printable bytes; native console rejects `[ ] \ : , ? *` | Name in subsequent signed adverts; identity unchanged |
| `get owner.info`, `set owner.info TEXT` | Up to 119 bytes; `\|` separates lines | This role's owner information |
| `get lat`, `set lat DEGREES`; `get lon`, `set lon DEGREES` | Decimal coordinates | Saved role location used by its advert policy |
| `get repeat`, `set repeat on\|off` | Explicit selection | Enable/disable that role's forwarding |
| `get rxdelay`, `set rxdelay BASE` | Decimal 0..20; 0 disables RX delay | Role-local receive processing delay using native reception scoring |
| `get txdelay`, `set txdelay FACTOR` | Decimal 0..2 | Flood reply/forwarding delay as an airtime factor |
| `get direct.txdelay`, `set direct.txdelay FACTOR` | Decimal 0..2 | Direct reply/forwarding delay as an airtime factor |
| `get multi.acks`, `set multi.acks COUNT` | Native byte count | Extra native ACK behavior; the shared scheduler still owns transmission |
| `get path.hash.mode`, `set path.hash.mode MODE` | 0/1/2 mean 1/2/3-byte ordinary hashes | Originated native flood path width |
| `get flood.max`, `set flood.max HOPS` | 0..64 | Native flood hop limit |
| `get flood.max.unscoped`, `set flood.max.unscoped HOPS` | 0..64 | Additional limit for unscoped floods |
| `get flood.max.advert`, `set flood.max.advert HOPS` | 0..64 | Additional limit for flooded adverts |
| `get loop.detect`, `set loop.detect MODE` | `off`, `minimal`, `moderate`, `strict` | Native role loop-detection policy |
| `get advert.interval`, `set advert.interval MINUTES` | 0 disables; otherwise 60..240; stored in two-minute units | Native zero-hop advert schedule |
| `get flood.advert.interval`, `set flood.advert.interval HOURS` | 0 disables; otherwise 3..168 | Native flood advert schedule |
| `advert.zerohop`, `advert` | No arguments | Send this role's advert locally or by flood |
| `setperm KEY64 PERMISSIONS` | Full public key; permission byte 0..255; use 3 for admin, 1 for read-only, 0 to remove | Applies this role's ACL entry; native ACL-file saving is delayed |
| `reboot` | No arguments | Restart this role's instance; Management `apply` restarts the device |

The native Repeater/Room minimum local advert interval is 60 minutes.
Odd interval values are rounded down to two-minute units; read back the saved
value. The device-wide `set autoadvert off` Management setting suppresses
startup/periodic adverts while leaving these saved intervals and manual
commands intact.

Use Management `role name ROLE TEXT` and `role advert ROLE zerohop|flood`
when operating several roles from one admin connection. A queued advert
announces the current role name and identity using the selected route.

Native `get acl` prints the full ACL only on that role's local serial console;
it is not an RF paginated ACL command. The app's binary ACL reader uses the
native ACL request. Management `get acl KEY64|1..5` reads a different ACL.
Keep role grants explicit and use a complete public key rather than an
ambiguous prefix.

The native role `reboot` path can end the console without a text response.
Reconnect and inspect the role before retrying. It preserves saved preferences,
identity and state. A local role erase is destructive; use a private node
backup and the documented recovery procedure instead of erasing to fix a
configuration error.

## Repeater routing

Region policy selects which floods the Repeater forwards and which scope its
own packets use:

```text
region
region default
region put local
region allowf local
region default local
region save
```

Use the [regional routing manual](../shared/REGIONS.md) for `put`, `get`,
`remove`, `allowf`, `denyf`, `home`, `default`, `def`, `list`, `load` and
`save`, including hierarchy and persistence. Changing region policy does not
change frequency or channel encryption. If another radio's advert uses a
scope this role denies, it is not forwarded.

`neighbors` reports role-local native neighbor observations.
`neighbor.remove PUBLIC_KEY_HEX` removes an entry. The command bot's
`!neighbors` is a separate cache with its own observations.
`discover.neighbors` sends a Repeater discovery request; use it deliberately
because it transmits RF.

## Local Room guest access

The local Room stores native messages and serves login catch-up under its
own identity. Its password and ACL are distinct from a Worker-backed room
alias.

| Command | Parameters | Effect |
| --- | --- | --- |
| `set guest.password TEXT` | Up to 15 printable bytes; empty deliberately permits password-free guest login | Replaces this Room's guest credential |
| `get allow.read.only`, `set allow.read.only on\|off` | Explicit selection | Allows/denies read-only admission when the supplied guest password does not match |
| `setperm KEY64 3` | Full administrator key | Authorizes that key as this Room's administrator |

Set a nonempty guest password and `allow.read.only off` for password-protected
guest history and posting. Management `room access` reports the role's
password-free/password-protected guest setting without exposing the credential.
Native `get guest.password` exposes it to that role administrator; do not
collect or publish it in routine status logs.

To recover/change a native role administrator password, use Management's
protected-file `admin.py role-password repeater|room --new-password-file PATH`
operation. It saves/applies the new credential while retaining that role's
ACL and active sessions. Do not place a password in a console command or
shell argument; the upstream native `password TEXT` response echoes it.
See [role password recovery](MAST_ADMIN.md#recovering-a-repeater-or-room-administrator-password).

## Companion/Base and shared-device controls

Applications on Aspen's companion TCP endpoint share one Base identity,
contacts and saved channel configuration. The application sends native binary
commands, not the Repeater/Room text grammar. Management can rename or advertise
it with `role name companion TEXT` and `role advert companion zerohop|flood`.
`role config companion` reports its channel capacity;
`role channel companion SLOT` reads metadata. Follow the
[companion application guide](../shared/ANDROID.md) for connection and
supported settings.

Aspen rejects role-console mutations of the shared PHY, CAD, interference/AGC,
RX boost, hardware sensors, GPS, bridges and board shutdown. Use Management
`radio`, `tempradio`, `set cad`, `set int.thresh`, `set agc.reset.interval`
and `set rxboost` for the supported device-wide controls. See
[Management parameters and ranges](MAST_ADMIN.md#common-control-commands).
Companion private-key/radio mutations are also refused by the shared-device
adapter; Management's explicit identity workflow preserves staging and
restart semantics.
