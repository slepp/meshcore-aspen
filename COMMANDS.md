# Choose the command endpoint

Send `!ping` to the command bot for a reply, `help` to the Management console
for device controls, or open a Repeater/Room contact's administrator console
for that role's settings. These are separate endpoints and permissions.
Start with [Aspen setup](firmware/esp32/PUBLIC_SETUP.md) or the
[host guide](HOST_GUIDE.md) if the node is not yet configured.

| Endpoint | How to reach it | Controls |
| --- | --- | --- |
| Command bot | Private DM or a configured channel; commands start with `!` | Diagnostics, installed commands, personal notes, granted shared/network operations |
| Management | Authenticated native RF console, or trusted-LAN `/admin` | Roles, shared PHY, names, source/packet programs, service settings, backup |
| Repeater / local Room | That contact's native administrator console | Its own preferences, routing and native ACL |
| Companion/Base | Companion application over its TCP endpoint | Base contacts, messages and app-supported settings |
| Host native bot owner | Private same-UID Unix socket | That bot's source, policy, identity and scoped data |
| Worker-backed room alias | Companion application's Room login | Room membership, messages and history; its operator settings belong to the Worker |

The dashboard's `command-bot` is the RF bot. Its `bot` slot is the KISS
service label. In Management commands, `bot` and `command-bot` both select the
command bot; `kiss` selects the modem service. `key bot` therefore reads the
command bot's public key.

## Command-bot manual

Advertise from the sending companion, then DM the bot:

```text
!ping
!about
!help
!help remember
!remember shopping get tea
!recall shopping
```

Expect `Pong`, a short introduction with help pointers, permission-filtered
command pages, then a saved note and its text. `!about` describes what to do;
identity discovery belongs to adverts and owner `bot key`.
`!help NAME [PAGE]` is the runtime authority for an installed command's exact
arguments and example.

| Command | Parameters | Permission and consequence |
| --- | --- | --- |
| `!ping`, `!about`, `!version`, `!uptime`, `!status` | None | Public diagnostics where the context's command/reply masks allow them |
| `!test`, `!path`, `!signal` | None | This request's path/RF information; local reflection is identified |
| `!mt [SECONDS]` | Integer 1..30; default 5 | Collects duplicate request paths, then returns one result |
| `!trace [WIDTH:HEX]` | Native TRACE widths 1/2/4/8; bounded hex route; width 3 is invalid | DM or explicitly addressed channel; sends a TRACE and awaits its return |
| `!air [PAGE]` | Page 1..4 | Scheduler credit, queue, RF airtime and local TX counters |
| `!help [NAME] [PAGE]` | Optional command name; page 1..40; `!help PAGE` also works | Returns one requested help page, filtered by this request's permissions |
| `!plugins [PAGE]` | Page 1..4 | Active source, command/module counts and declared events |
| `!neighbors [PAGE]` | Page 1..16 | Cached signed adverts, observation ages, paths and available signal |
| `!remember KEY TEXT` | Key 1..32 bytes; text 1..120 | Authenticated private DM; writes/replaces that caller's note and confirms commit |
| `!recall KEY`, `!forget KEY` | Key 1..32 bytes | Authenticated private DM; reads or deliberately deletes that caller's note |
| `!notes [PREFIX]`, `!list-memories [PREFIX]` | Prefix up to 32 bytes | Authenticated private DM; lists keys; narrow the prefix if the reply is truncated |
| `!remind DURATION TEXT` | Integer seconds or decimal with one lowercase `s/m/h/d` suffix, 1 second..24 hours; text 1..120 | Private DM, reminder grant, trusted time and fresh direct route; saves a personal reminder |
| `!reminders`, `!cancel ID` | No argument, or decimal reminder ID | Private DM; reads state or cancels pending work; inspect uncertain TX before rescheduling |
| `!board put KEY TEXT`, `get KEY`, `list [PREFIX]`, `delete KEY` | Key/prefix up to 26 bytes; text 1..120 | Shared-state grant and storage masks; channel invocation must target the bot; writes/deletes are durable |
| `!weather PLACE`, `!service health`, `!service echo TEXT`, `!service weather PLACE` | Place up to 80 bytes; service arguments up to 120 | Authenticated private DM with network grant and configured service; may wait for HTTPS |
| `!admin COMMAND` | Bounded native owner command | Trusted full-key private DM only; permitted Management/owner operations, not secret uploads |
| `!calc EXPRESSION` | Up to 120 bytes; decimal `+ - * /` and parentheses; nesting up to 8 | Public utility; returns a numeric result or an input/arithmetic error |
| `!convert VALUE FROM TO` | Scalar value up to 32 bytes; case-sensitive units up to 4 bytes | Public utility; converts FROM to TO within one dimension |
| `!roll [DICE]` | `[N]dS[+/-K]`; default `1d6`; at most 12 dice, 1000 sides, modifier +/-10000 | Public utility using native randomness |
| `!choose A\|B\|...` | 2..8 nonempty choices, each up to 64 bytes; total up to 148 | Public utility; chooses one option |

The [runtime manual](firmware/runtime/BOT_RUNTIME.md#discovery-targeting-and-owner-commands)
covers custom registrations, targeting and execution limits; the
[utility reference](firmware/runtime/BOT_RUNTIME.md#bundled-utility-commands)
lists conversion units and arithmetic limits. A command may be denied by a
context/action/storage mask even when
its builtin permission is public. Channel nicknames are not user identities;
personal notes and owner administration cannot be granted by joining a channel.

Arguments are typed rather than split ad hoc by each command. A string argument
is one token and can be quoted; a text argument consumes the rest of the line.
Numbers/booleans are checked against the declared schema. Square brackets in
this manual mean an optional argument, not literal brackets to send. Missing,
extra or invalid arguments name the command and point to `!help NAME`.
The complete DM request fits 162 printable ASCII bytes; targets and command
names consume part of that capacity. Channel messages have a smaller text
limit because they include a sender nickname.
See [command declarations](firmware/runtime/BOT_RUNTIME.md#restricted-handler-api)
when extending that grammar in Lua.

### Address one bot on a channel

An operator can save up to four aliases with `bot aliases aspen,aspen-bot,a`.
Use `!@aspen-bot help`, a companion mention such as `!@[Aspen-Bot] help`, or
`!@BOTKEY8 help`, where `BOTKEY8` is the first eight public-key hex digits.
Aliases are case-insensitive on reception. A full public key also works.
There is no implicit alias derived from the display name.

`bot access CONTEXT default 12` permits addressed execution/replies and
denies bare execution/replies and storage. Context is `dm`, `native`, or
channel slot `0..7`. Mask bits are execute bare=1, reply bare=2,
execute addressed=4, reply addressed=8, read storage=16 and write storage=32.
An override uses `bot access CONTEXT COMMAND MASK`; remove it with `inherit`.
`bot thread CONTEXT NAME MASK` further restricts a named storage thread to
0/16/32/48. Both edits save and apply live.

**Before joining an addressed-only hashtag, disable the bot and restart it.**
`bot off` only saves the next boot selection. A new hashtag/private membership
initially allows mask 63 and clears its overrides; Public initially denies
commands. With the bot actually stopped, join, set/read back the access policy,
then save `bot on` and restart. On a native host, stop the bot before changing
the owner configuration. See [membership and mask details](firmware/runtime/BOT_RUNTIME.md#channels-and-native-command-policy).

For `bot access CONTEXT list OFFSET` and `bot thread CONTEXT list OFFSET`,
follow `next` until it equals the offset just requested. A terminal `next`
can be nonzero. Each request returns at most four overrides.

## Management manual

Use the existing Management password file; no connection is made by `--help`:

```sh
python3 tools/hardware/admin.py --help
python3 tools/hardware/admin.py --web http://RADIO \
  --password-file /absolute/private/node-password command 'help'
```

Use encrypted RF for secrets:

```sh
python3 tools/hardware/admin.py \
  --gateway COMPANION_KISS_HOST --target MANAGEMENT_PUBLIC_KEY64 \
  --seed-file /absolute/private/companion.seed \
  --password-file /absolute/private/node-password command 'status'
```

Global connection options precede the command. RF's gateway is another
companion radio on the same PHY, not the Management node. Web administration
needs a trusted LAN because plain HTTP exposes its password/session.

| Task | Start here | Focused manual |
| --- | --- | --- |
| Read device state or discover syntax | `status`, `roles`, `job`, `help`, `help TOPIC [PAGE]` | [Common commands and full help index](firmware/esp32/MAST_ADMIN.md#common-control-commands) |
| Select roles | `roles MASK` (0..15), `bot on\|off`, then `apply` | [Saved/live role selection](firmware/esp32/MAST_ADMIN.md#configure-only-the-authority-you-need) |
| Name or announce a role | `role name ROLE TEXT`, `role advert ROLE zerohop\|flood` | [Names, keys and advert routing](firmware/esp32/MAST_ADMIN.md#runtime-role-names-and-keys) |
| Change the shared PHY | `radio HZ BW_HZ SF CR DBM`, `tempradio SECONDS HZ BW_HZ SF CR DBM` | [Radio controls, units and ranges](firmware/esp32/MAST_ADMIN.md#common-control-commands) |
| Save aliases or channel/thread policy | `bot aliases`, `bot membership SLOT`, `bot access CONTEXT`, `bot thread CONTEXT` | [Bot owner controls](firmware/esp32/MAST_ADMIN.md#bot-facing-owner-administration-and-mesh-policy) |
| Install one Lua file | Python `source install NAME FILE` | [Named source composition](firmware/runtime/REMOTE_REPEATERS.md#replace-or-compose-lua-sources) |
| Replace/recover a bot program | Python `source replace FILE`, `source rollback`, `source reset` | [Source journal and recovery](firmware/esp32/MAST_ADMIN.md#source-and-help-installation) |
| Install a packet program | `packet api`; Python `packet install SLOT FILE`, `packet budget SLOT STAGES FUEL US CAPS`, `packet enable SLOT on` | [Two slots, budgets, fault/rollback behavior](firmware/runtime/PACKET_ENGINES.md#install-and-configure-a-program) |
| Configure HTTPS or metrics | `bot https status`, `telemetry endpoint status` | [HTTPS destination/grants](firmware/runtime/NETWORK_API.md), [telemetry](firmware/esp32/TELEMETRY.md) |
| Configure MQTT or shared rooms | `mqtt status`, `cloudroom config status`, `cloudroom status` | [MQTT](firmware/esp32/OBSERVER.md), [frontend settings and commands](services/shared-room/NATIVE.md#configure-the-generic-image) |
| Back up or update a node | Preserve a private encrypted backup; use application-only update | [Node backup](NODE_BACKUP.md), [signed application updates](firmware/esp32/ESP_FIELD_UPDATES.md) |

Role masks are repeater=1, room=2, companion=4, observer=8. Bot selection is
separate. `apply` and Management `reboot` restart the whole device and apply
saved boot selections; Management/KISS do not require an optional RF role.
PHY changes affect every role and host using the modem. Keep a matching
recovery connection before retuning.

`setperm KEY64 3` grants Management administration; `0` removes that saved
entry. Repeater/Room ACLs are separate. Secret WiFi, password, identity, CA/token
and CloudRoom uploads require the documented encrypted/protected transport.
Use protected-file CLI operations, not secret command-line arguments.
Identity rotation is deliberate and does not move identity-bound bot data.

Read `job`, the affected saved/live status and the selected hash after a lost
reply. A queued advert is an accepted transmit request. An uncertain command,
upload, restart or transmission is not a confirmed failure and should not be
blindly repeated.

## Native role manuals

Use [Repeater and Room console commands](firmware/esp32/ROLE_COMMANDS.md) for
role-local settings and persistence. Use the
[companion endpoint guide](firmware/shared/ANDROID.md) for app controls and
the [regional routing guide](firmware/shared/REGIONS.md) for scope policy.
Worker-backed aliases use the [shared-room service guide](services/shared-room/README.md).

On the host bot's private owner socket, omit `bot ` for bot policy commands:
`aliases`, `membership`, `access`, `thread`, `https`, `home` and `events`.
Source/data commands keep their existing names. The same Python source and
packet-free bot tooling accepts `--unix-socket /absolute/state/bot/native/admin.sock`.
That socket controls its bot, not sibling host roles or the modem PHY.
See [host status and owner access](HOST_GUIDE.md#host-status).
