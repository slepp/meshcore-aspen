# Frozen host contract: mast-beta-v1

This names the implemented beta contract; it does **not** add a wire-version
field or discovery endpoint. Incompatible changes require a new contract.
The adversarial-review corrections below preserve native packet formats,
signed vectors and the source envelope. Existing beta build profiles must
explicitly provide `ONCHIP_MAST_PASSWORD`: role passwords no longer implicitly
grant mast ownership.
Target: XIAO ESP32-S3R8/Wio SX1262, existing persistent **management** identity,
independent of repeater/room/companion/bot selection. One handler program,
not a plugin SDK or an optional network RPC protocol.

The transport/source envelope below remains `mast-beta-v1`. The new
**`named-commands-v1` source API** is a separate, incompatible replacement for
the old anonymous Lua handler ABI: `source api` identifies it explicitly.
See [BOT_RUNTIME.md](../runtime/BOT_RUNTIME.md#restricted-handler-api) for named exports,
argument schemas, reserved diagnostics and migration. Existing source clients
retain chunk/hash/rollback framing but must upload the matching source API.

## Public users versus owners

MeshCore 1.17+ interoperability means ordinary **other users**, not just our
owners running a stock radio. Enabled repeater, room, companion and command-bot
identities must remain ordinary MeshCore participants: native signed adverts,
contacts, routing and role-appropriate message/login behavior. Public users
need no firmware/client modification, package installation, mast-admin login,
CLI correlation tag or knowledge of the source-transfer extension. A room's
normal access policy is separate from mast ownership.

Owners may use private management commands and deployment tooling. The source
chunk/receipt grammar below is encrypted application text inside existing
MeshCore `TXT_MSG` packets, not a new public packet type, routing header or
`MULTIPART` protocol. Unmodified community repeaters must be able to relay
login, commands and replies using native flood/direct routes without parsing
or implementing the private commands. Do not require public peers or transit
repeaters to participate in upload authentication, reassembly or activation.

Acceptance separates these concerns: an unrelated disposable stock identity
exercises public bot messaging without any admin login; an owner identity
exercises the private installer. Native routed-packet tests are not a
substitute for a physical unmodified-repeater test. Current physical evidence
and remaining role/route coverage are recorded in [MAST_ADMIN.md](MAST_ADMIN.md).

## Transport and authentication

Use MeshCore `d92964352441e53b93e8667b802e04f6e072b39e` framing/crypto.
Reference implementation: [admin.py](../../tools/hardware/admin.py). Reuse native helpers,
not a parallel encryption/framing implementation.

| Surface | Contract |
| --- | --- |
| RF login | Native `ANON_REQ`, addressed to the full management key; plaintext `timestamp:u32le + password`, optional NUL, native zero padding. The stock exactly-12-byte non-NUL password is supported. Empty password requires an exact trusted full companion public key; otherwise explicitly configured `ONCHIP_MAST_PASSWORD`, 1..15 bytes |
| RF login response | Native 13-byte response: timestamp[4], zero[2], admin=1, permission=3, random[4], revision=2. Flood login may return it inside native `PATH`/path-return framing; preserve encoded path/hash widths |
| RF command | Encrypted native `TXT_MSG`: `timestamp:u32le + flags:u8 + ASCII text`; use CLI flags `4`. Reply also uses CLI flags `4`; its timestamp is not a request-ID echo |
| RF binary reads | Native `REQ`: timestamp:u32le, type:u8, native padding; type 1=status, 3=telemetry, 7=owner information. Type 7 returns the echoed timestamp followed by `firmware_version\nmanagement_name\nowner_text`, without a required NUL. Long owner text is shortened to fit cipher padding and the return path; `get owner.info` reads all 119 bytes. Other role-specific requests remain unsupported |
| Public owner lookup | Native direct `ANON_REQ`: timestamp:u32le, type=2, encoded return path and native padding. Reply is timestamp echo:u32le, server clock:u32le, `management_name\nowner_text`; returned directly along the supplied path. No login or replay slot is required. Four valid queries per three-minute window; owner text is public |
| RF correlation | Send `hhhhhhhhhhhhhhhh|COMMAND` with a unique 16-lowercase-hex tag. Reply echoes that prefix; strip it before parsing. Existing two-hex tags and untagged CLI also work |
| RF limits/session | Text maximum 162 bytes **including tag**: 145 command bytes with the recommended tag. Four ordinary full principals plus a reserved compiled-owner record/session have durable timestamp high-water marks. Persist strictly increasing u32 timestamps across distinct requests and restarts; exact authenticated retries reuse their cached result. Sessions expire after 15 minutes idle and are lost on reboot. Successful login is limited to once/second per principal; invalid credentials do not consume that gate |
| Web login | `POST /admin/login`, raw password body, no newline or JSON. Success: HTTP 200, body is 32 lowercase hex characters |
| Web command | `POST /admin/command`, header `X-Mast-Session: TOKEN`, raw printable ASCII command body, 1..162 bytes. Success transport response: HTTP 200, raw command-result text, maximum 162 bytes; no RF tag |
| Web logout | `POST /admin/logout` with the same header; empty body allowed. Success body `Logged out` |
| Web session/errors | Two explicitly occupied sessions, fixed ten-minute elapsed lifetime, reboot invalidates them. Host must be the configured hostname, its `.local` name, the current WiFi IP or `ONCHIP_MAST_WEB_HOST`, on the configured HTTP port. Origin, if supplied, must equal `http://Host`. HTTP 400=bad body, 403=auth/origin/host, 429=capacity/rate/busy, 503=administration unavailable, 504=outcome unknown. Command-level errors can still arrive with HTTP 200 |

Use an independent radio for RF: either a stock USB/BLE companion or a KISS
gateway, never the mast's own same-device endpoint as an OTA substitute.
RF command/result text is native encrypted text, **not JSON**. Public web is
HTTP only: trusted LAN or appropriately configured TLS tunnel; a CLI should
omit `Origin`. Never send a mast session token to an unrelated host service.

Flood replies use pinned `chooseReplyRoute`/`chooseReplyScope`; authenticated
native PATH returns establish reciprocal direct routes. Transport-flood hints
are accepted without replacing payload authentication. Set
`ONCHIP_SERVICE_REGION` to the public hashtag region needed by restrictive
repeaters. The native region helper derives its key and recomputes transport
codes for each reply; incoming codes cannot be copied to another payload.
One configured public region is supported. An unknown/private region key
cannot be recovered from its short transport code.

Existing `meshcli` supports `add_contact KEY 2 NAME`, `reset_path NAME`,
`login NAME PASSWORD`, `cmd NAME COMMAND`, `wmt8` and bounded script files.
It sends the same command strings through the stock companion; no different
upload envelope is needed. Stock 1.17.1 generates CLI timestamps from its
own unique RTC sequence, so keep that RTC synchronized and let it construct
RF packets. Persisting host-side timestamp counters is necessary for clients
that construct native RF packets themselves, such as the KISS reference.
For the companion-radio administration workflow, see
[MAST_ADMIN.md](MAST_ADMIN.md#native-rf-and-web-access).
The bounded stock CLI test uses the already supported two-hex tag; this is
not a new RF envelope or a relaxation of hash/activation checks.

## Common commands and results

Commands are case-sensitive; integer fields below are unsigned decimal.
Successful command results and errors are plain text, without JSON wrapping.

| Command | Stable result/meaning |
| --- | --- |
| `status` | `roles applied=A saved=S generation=G bot-saved=B PHY=HZ,BW,SF,CR,DBM effective-gen=E temp=T` |
| `ver`, `board` | Firmware profile/version and platform description; protocol revisions are unchanged |
| `help TOPIC`, `wifi help` | Short syntax; an incomplete namespace returns `Error: usage: ...` |
| `get name`, `set name TEXT` | Read/change the saved Management name, not another role or identity |
| `get owner.info`, `set owner.info TEXT` | Management-local persisted text, 0..119 printable ASCII bytes; `|` separates lines |
| `get radio`, `get freq`, `get tx` | Effective shared PHY readback; persistent and temporary setters keep the existing explicit mast syntax |
| `get cad`, `set cad on\|off` | Effective shared hardware CAD; response-gated, saved for all modem users |
| `get int.thresh`, `set int.thresh N` | Saved shared interference threshold, `0..255`; `0` disables detection |
| `get agc.reset.interval`, `set agc.reset.interval N` | Saved shared AGC interval, `0..1020` seconds, rounded down to four-second units; `0` disables resets |
| `get rxboost`, `set rxboost on\|off` | Effective/saved boosted receiver gain; explicit error if the driver does not expose it |
| `get af`, `set af F` | Saved aggregate airtime factor, finite `0..9`; native role `af` remains a separate source budget |
| `bot status` | `bot applied=A saved=S ready=R state=TEXT` |
| `bot key` | `KEY ` followed by 64 lowercase hex characters, or an error if disabled |
| `roles MASK` | Save mask 0..15: repeater=1, room=2, companion=4, observer=8 |
| `bot on`, `bot off` | Save independent next-boot bot selection |
| `apply`, `reboot` | Accept a deferred reboot; `apply` is not hot role recreation |
| `wifi ssid HEX`, `wifi password HEX` | Encrypted RF only; save SSID 1..32 bytes or password 8..63 bytes/64 ASCII hex digits; `wifi password -` explicitly selects an open network |
| `get wifi.enabled/ssid/pwd/ip/status` | Separate field commands, native `> VALUE` replies; enabled is saved 0/1, status is Arduino WiFi status, IP is current station IPv4. Password readback requires encrypted RF |
| `set wifi.ssid TEXT`, `set wifi.pwd TEXT` | Encrypted RF only; literal printable text including spaces; an empty password explicitly selects an open network. Save first, then `wifi apply` |
| `set wifi.enabled 0/1` | Encrypted RF only; `off/on` aliases accepted. Save and defer station stop/start until after the response. Default enabled; disabling retains credentials and RF roles |
| `wifi apply`, `wifi forget` | Deferred reconnect using saved fields, or remove mast override for next boot |
| `wifi status` | On ESP32: `wifi saved=S ssid-bytes=N connected=C`; never credentials |
| `radio HZ BW SF CR DBM` | Deferred persistent PHY change |
| `tempradio SECONDS HZ BW SF CR DBM` | Deferred temporary PHY, 1..3600 seconds, then automatic restore |
| `trust KEY`, `trust none` | Save/remove additional full-key trust; compiled trust and role-local ACLs remain unchanged |
| `auth forget KEY` | Authenticated removal of a noncompiled principal's replay record and current session, freeing a slot. Not password revocation; anyone retaining valid credentials can log in again |
| `role password repeater\|room HEX` | Save/apply the active native role's administrator password; authenticated encrypted Management RF only |
| `job` | Latest deferred-control outcome, human-readable; **no host-visible job ID** |
| `help`, `source help` | Human-readable command usage |

`G` is durable role-journal u64; parse without floating-point loss. `E` is
boot-local effective-PHY u32, advancing on change and return, restarting on
boot. Booleans are 0/1. PHY bounds and bandwidth enumeration are in
[MAST_ADMIN.md](MAST_ADMIN.md#common-control-commands).

`Error:` denotes command failure; do not bind to the following prose.
`Saved ...` confirms the named durable save, not its later application.
`Accepted ...` confirms admission only. Retune/reboot/reconnect waits for
old-PHY reply TX completion or web-response send, then a 500 ms guard.
Inspect `job` and read back state; failure/timeout may leave saved and applied
state different. Serialize control workflows: there is one pending effect.
KISS `CONFIG GET` wire format is unchanged and reports effective PHY/generation.
CAD, threshold, aggregate `af` and shared control changes use the same response
fence and advance the effective generation, retiring old-generation jobs.
AGC/RX boost use a separate versioned control record, not an extension to the
18-byte KISS profile. Legacy installs retain a 30-second AGC interval and their
hardware's initial RX gain until an owner saves controls. Control commit or
hardware-application failures may leave saved and applied state different;
the radio stops admission until a successful restart restores valid controls.
`tempradio` requires a durable profile and returns to that complete profile,
not a preceding transient configuration; restoration does not write NVS.
Failed/cancelled acceptance transmission replaces the cached `Accepted`
result with `Error:`. Corrupt replay storage disables native/web administration
without halting signed role management, ordinary KISS or other native roles.

`role password ROLE HEX` requires the full sender of an active authenticated
Management RF session, not just a caller-supplied encrypted-transport flag.
Web, Lua/program administration and host owner sockets cannot perform this
write. `ROLE` is `repeater` or `room`; `HEX` decodes to 1..15 bytes in
`0x20..0x7e`, with no NUL or truncation. Invalid role/encoding/length changes
nothing. The operator client requires `role-password ROLE --new-password-file
PATH`, separate from the Management authentication `--password-file`.

Success is `Saved and applied ROLE administrator password; ACL/sessions
unchanged`, after native preference save and password readback. The new role
password is live immediately and survives a restart; existing ACL admissions
remain valid. Management/guest credentials, role identities, other preferences
and other roles are unchanged. An inactive/busy role reports no change.
Preference IO/readback failure restores the previous live administrator
password but reports saved state unknown; a partial file or a complete new
save is possible. A lost reply likewise does not establish failure. Verify
native role login/settings before retry or restart; never echo the password
or its hexadecimal command in outcomes.

## Source envelope, progress and receipts

Source is 1..4096 uncompressed bytes, source-only Lua 5.5.1: no NUL, binary
bytecode or compression. The same source commands run over authenticated RF
and web. ID is 16 lowercase hex characters; using SHA-256's first 16
characters is recommended, not required. HASH is full 64-lowercase-hex
SHA-256 of the exact source bytes.

The raw-KISS reference completes the native reciprocal PATH exchange after
flood login. It can retransmit a command twice within its bounded deadline,
changing only native attempt bits while retaining timestamp, text and
correlation tag. This avoids relay packet dedup without repeating an admitted
backend action. Login loss still requires a fresh login; timeout never means
success. Stock companions already own their native route handling.

| Command | Stable result/meaning |
| --- | --- |
| `source begin ID SIZE HASH` | `ACK ID next=N`; resume only an identical ID/size/hash manifest; another upload requires explicit cancel |
| `source chunk ID INDEX HEX` | `ACK ID next=N`; zero-based sequential index, exactly 48 decoded bytes except the final remainder; at most 86 chunks |
| `source commit ID` | `Accepted verification; source status reports durable activation outcome` |
| `source status` | `gen=G active=A prev=P size=S upload=ID_OR_none next=N/T; OUTCOME` |
| `source hash` | `SHA256 HASH gen=G` for the durable selected source; not proof of live activation |
| `source read INDEX` | `DATA HEX`, up to 48 source bytes, or `EOF`; legal indexes 0..85 |
| `source help TEXT`, `source help -` | Save/change or clear help, at most 96 printable ASCII bytes |
| `source helptext` | Raw help text, or `(no source help)`; not structured status |
| `source rollback`, `source remove` | Asynchronously validate/select previous source+help or bundled handlers; inspect status |
| `source cancel` | Cancel the upload, preserving active source |
| `source retry` | Retry loading the durable selection without changing its generation/hash; also retries exhausted bundled VM initialization |

ACK means chunk flush/readback **and** durable progress commit completed.
Identical duplicate chunks ACK without rewriting; conflicting duplicates and
gaps error. After a timeout/reboot, reauthenticate and repeat `begin` with the
same manifest, then continue at `next`. Retrying the same numbered chunk with
a fresh RF timestamp is safe. Do not blindly replay other mutations.

Source `G` is independent durable u32, advancing on activation/help changes.
Slots 0/1/2 are private durable slots; 3 means bundled. Do not expose paths
or assume a particular slot for the next install. Explicit corrupt-journal
recovery through `remove` resets its unusable generation to 1.

After commit/rollback/remove, poll status. Terminal live success contains
`source durably saved and active`; terminal disabled-bot success contains
`source saved; bot disabled; loads on next enabled boot`. `Error:` anywhere
in the status **OUTCOME** means failure; other outcomes are pending/diagnostic
prose, possibly truncated. Then require `source hash` to match the intended
content. Hash alone cannot distinguish saved selection from live success.
Transient live-load failures retain the prior already-initialized program,
reopen public command admission and perform at most three delayed retries.
The outcome remains explicitly erroneous until activation succeeds; after
exhaustion use `source retry`, rollback or removal. Retained live code may
therefore differ from the durable hash during recovery.
Normal bundled idle status has outcome `bundled source`.

For download, read declared `size` and exactly `ceil(size/48)` chunks; verify
length, full hash and unchanged generation/hash before and after reading.
Do not request index 86 to find EOF at the 4096-byte maximum.
Help is raw content and can itself start with `Error:`; do not treat it as a
machine-readable deployment result.

## Reuse and uncertainty

Existing signed [rf.py](../../tools/hardware/rf.py) status/update/receipt envelopes and
role journals are unchanged; reuse `query_with_context`, `frame_with_context`,
`verify_status`, `verify_receipt` and native KISS framing. Signed verified
requests have a one-second admission interval. Their operator verification
key is separate from native trusted-companion login. Role-local `setperm`
does not grant mast permission.

Firmware integration remains `MastAdmin::execute` plus its internal
reply-completion acknowledgment, and the existing
`stageSourceFile` / `pollSourceResult` / `activateStaged` source hooks.
Hosts do not call those hooks or invent a second installer protocol.

Disconnect, RF silence, HTTP 504 or missing acceptance means **outcome
unknown**, not rollback or guaranteed failure. Read status/hash before
retrying. There is no exactly-once mutation guarantee across lost replies
or reboot. Reference client/tests already exist in `admin.py`,
`tools/hardware/tests/test_admin.py`, `firmware/esp32/tests/beta_native.cpp`
and `tools/hardware/management_rf_checks.py`;
reuse or relocate them for the independent tooling lane.
