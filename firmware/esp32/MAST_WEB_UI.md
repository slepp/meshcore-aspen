# Manage an ESP32 mast in the browser

Open `http://MAST/admin` on a trusted LAN and log in with your Management
password. This is the **mast administration web page**: it
manages the shared physical modem, on-device roles and command programs.
The official MeshCore mobile application remains the interface for ordinary
chats, repeater and room use. The public `/` page remains a read-only radio
stats page.

The public Aspen and `Xiao_S3_WIO_onchip_beta` builds include this page. The password is
independent of the repeater/room passwords. See [mast administration](MAST_ADMIN.md)
for supported hardware, configuration and build commands.

For a public Aspen image, complete [offline USB setup](PUBLIC_SETUP.md) first;
without a valid setup record, administration and roles cannot start.
For source-built images, use [image selection and provisioning](README.md#choose-an-image).
On an existing node, log in and refresh roles, effective radio and
source status before choosing a write. For CLI/RF and the private native-host
owner socket, use the [administrator workflow](MAST_ADMIN.md#administrator-workflow).
The host bot's Unix socket is not itself an HTTP endpoint. For the separate
opt-in authenticated host page, use [host browser administration](../../HOST_GUIDE.md#host-browser-administration).

**HTTP sends the password, session and private backups in plaintext.** Use
an isolated trusted LAN or an appropriately configured authenticated TLS
tunnel. Do not expose the HTTP listener to the Internet. The page blocks
secret-setting commands before confirmation or HTTP transmission: WiFi
`password` and combined `wifi SSIDHEX PASSHEX`, `role channel ROLE SLOT NAMEHEX KEY32`,
private `key ROLE HEX128` imports, telemetry `endpoint ca/token` and
`bot https ca/token`. Use encrypted Management RF for ESP32 secret provisioning;
backend authentication and transport restrictions still apply.

## Choose the operation

| Section | What you can do | What to inspect afterward |
| --- | --- | --- |
| Roles | Compare saved and running selections; save enable/disable for each role; rename roles/services; queue a zero-hop advert | Refresh roles; apply/reboot only when you want saved selections to run. Names preserve keys. Queued adverts do not confirm reception |
| Radio | Load the current channel into an editable form; save and retune; use a timed temporary channel | `job` and current radio settings. Every role and KISS client shares the change; queued old-channel packets can fail `STALE` |
| Bot programs | Read bot state, selected package metadata, hash and runtime/module capabilities; edit, upload/resume, install, roll back, restore bundled commands or retry loading | `source status` plus `source hash`. Durable selection can differ from retained live code during recovery |
| Services | Read telemetry/TLS, configured RPC, bot grants and companion TCP/WiFi diagnostics; configure telemetry publishing/interval or bot grants | Native saved/applied state and backend errors, not just admission |
| Bot data backups | Export a selected family/scope; validate a backup; restore it separately | `data status`; export/read back the affected scope after an uncertain restore |
| Firmware | Upload an application-only image and signed operator manifest to the inactive application slot; restart separately | Verified image hash, then running image hash and healthy boot after reconnect/login; [WiFi update workflow](MAST_ADMIN.md#wifi-application-updates) |
| Command console | Send supported commands that do not have a guided control | Command help and the affected state. Read-only commands run directly; writes require confirmation |

Program capabilities, role diagnostics and storage limits are under expandable
details. The radio stats page links back to administration on on-device builds
and groups memory readings under device health.

The page uses the same `/admin/login`, `/admin/command` and `/admin/logout`
endpoints and native command grammar as `admin.py` and encrypted RF
management. Page workflows share one serialized command mailbox.
Another administrator can still change state, so workflows check manifests,
stages and identity again before committing.
Firmware uses authenticated `GET/POST /admin/update` and the separate
`POST /admin/update/reboot`; native command writes are unavailable while an
application awaits reboot.
See [ESP application updates](ESP_FIELD_UPDATES.md) for the initial retained
USB installation, application-slot layout and rollback/recovery requirements.

### Role selection and radio changes

**Enable/disable after restart** changes a role's next-boot selection. It does not
recreate the running role. **Restart to apply settings** stops active
connections and commands and invalidates the session; log in after the mast
returns. Management and KISS remain independent of the role mask. Bot selection
is saved separately.

Runtime names take 1..31 printable ASCII bytes without `:`. Native role name
edits need an active role. The KISS name is a service label and has no RF
advert. Advert controls queue zero-hop transmissions and consume shared
airtime; a companion receiving the advert over RF is a separate observation.

Radio forms are never overwritten by a refresh. Copy the last read effective
profile explicitly if you want to edit it. Retuning can make RF management
unreachable: choose a legal profile and retain a LAN or radio recovery route.
Temporary settings last 1..3600 seconds and return to the complete durable
profile, not a prior temporary profile. The backend rejects another retune
while a temporary profile is active. It does not expose a separate saved-PHY
readback, so the page labels its displayed profile **effective**.

### Upload and install a command program

1. Select **Load program status**. Guided uploads require
   `named-commands-v1` and Lua 5.5.1 reported by `source api`; unknown runtimes
   are not assumed compatible.
2. Edit source, select **Load installed source**, or **Use hello example**.
   Download the editor draft before closing the tab; drafts are memory-only.
3. Select **Upload draft** and confirm the exact byte count/hash.
   It sends `source begin` and numbered 48-byte chunks, honoring durable
   `ACK ... next=N` progress. This does **not** validate or activate code.
4. Select **Install uploaded draft** and confirm again. This commits
   the uploaded snapshot, not newer edits in the editor, then polls native
   status and checks the selected hash and changed generation. Before commit,
   the page reasserts the exact ID/size/hash with the native resumable `begin`
   operation; an upload replaced by another administrator is rejected.

An install replaces the whole custom command/module set. There is no
per-plugin installer. Package headers and schema/capability transitions are
validated by the native backend; its errors remain visible. Optional package
signature verification remains in the [package tooling](../runtime/BOT_DEVELOPMENT.md),
not the browser.

Readback uses the declared byte count, exact chunk lengths, SHA256 and unchanged
generation/hash; it never requests index 86 at the 4096-byte limit. Invalid,
changed or oversized readback leaves the editor untouched. Bundled source can
exceed the editable/download envelope. The browser does not truncate it or
pretend it can reinstall that truncated source.

**Restore previous program**, **Restore bundled commands**, **Retry selected
program**, **Cancel upload** and help changes require explicit confirmation.
They preserve editor text. Rollback/restoration can cancel running commands but
retain bot data. Inspect status before deciding that a selected program is
live; a hash alone is not activation success.

### Export and restore private scoped data

Refresh storage capabilities before using the transfer controls. Choose
`kv`, `timers` or `reminders`, then a scope and its full principal:

- `caller` or `conversation`: the full nonzero user public key;
- `channel`: the full verified channel digest, not a nickname or channel secret;
- `bot`: 64 zeros;
- personal reminders: `caller` only.

Each export is one 2422-byte BKD/BTD/BRD v1 snapshot. The page checks the frozen
manifest, whole-file/content hashes, family, scope, principal and unchanged
running bot key before downloading. It does not display payload values.
Backups omit source, private identities, native credentials and grants.
Keep downloaded files private: browsers cannot enforce filesystem mode `0600`.
A complete caller backup needs separate KV, timer and reminder exports; these
are not a cross-family transaction.

Select a backup file to inspect its envelope locally. **Validate backup**
uploads it and waits for native `STAGED ID`, but does not restore.
**Restore backup** is a separate confirmation and checks that the
bot identity and native stage still match.

- KV restore replaces all keys in the encoded scope, including deleting newer
  keys absent from the backup.
- Timer/reminder staging and restore require the **no-rearm** checkbox. The
  native restore command includes `no-rearm`: existing fields/terminal states
  remain; matching pending work is cancelled; missing pending records import
  as cancelled. Restore selects one family/scope; it is not a cross-family
  transaction. See the image's [scoped data contract](MAST_ADMIN.md#scoped-bot-data)
  for its storage publication and recovery behavior.
- `UNKNOWN` means the committed outcome is unconfirmed. Already transmitted
  reminders cannot be undone. Export/read back before another explicit restore.
  Readback exports remain available while uncertain mutation controls are fenced.

Reload/logout loses the browser's staged handle; explicitly stage again rather
than replaying a prior restore. **Clear transfer** discards RAM
upload/export/stage data, not committed records.

## Errors, expiry and unavailable controls

`Accepted`, `Queued`, upload acknowledgments and `STAGED` are labeled as
admission/progress, not applied or delivered. Source activation requires a
terminal native status and verified changed manifest. Data restore requires
the matching native `COMMITTED ID`.

Lost connections, HTTP 504 and unknown backend results are uncertain.
The page never automatically retries writes and fences subsequent mutations.
Use **Check pending changes**, refresh affected state and,
for data, export/read back before another explicit operation. Pending/unknown
data status keeps that fence in place. Source upload recovery is an explicit
resume of the same hash/size/ID, not automatic replay of a commit.
Inspection remembers the affected Lua/Wasm runtime and restore scope.
Command-level errors, malformed/incomplete reads and an unrelated idle result
cannot unlock a mutation. All affected operations must be confirmed, not
just one successful read. A durable radio save has no separate saved-PHY
readback, so effective PHY alone does not resolve an uncertain save.

HTTP 403 invalidates the local session. Sessions have a fixed ten-minute
lifetime, with two server slots; logout frees a slot immediately. Tokens stay
in the tab, never cookies/local storage, URLs or command history. Login errors,
HTTP rate/busy limits and command-level `Error:` results remain actionable.
Expired sessions disable device controls without deleting editor drafts.
Local source editing, hashing and draft download remain available when logged out.
Read-only refreshes never change source or radio form text.

## Other administration tasks

| Task | Interface |
| --- | --- |
| List command exports and modules | Send `!plugins [PAGE]` or `!help [PAGE]` to the bot |
| Provision WiFi, HTTPS credentials or private keys | Encrypted Management RF; see [administrator workflow](MAST_ADMIN.md#administrator-workflow) |
| Verify a command-package signature before upload | [Package tooling](../runtime/BOT_DEVELOPMENT.md) |
| Chat and manage companion contacts | MeshCore companion application |
| Run additional local host owner commands | `admin.py --unix-socket` |

Safe settings use the reported backend capabilities and native errors.
`bot https` reads bounded native help and `bot https status` reads redacted
endpoint readiness; neither requires a write confirmation. WiFi status,
channel metadata and public/pending identity queries remain available.
The separate opt-in host page uses its same-UID native owner socket; use
`admin.py --unix-socket` for other permitted local owner commands.

On images with runtime management-password support, the console accepts the
safe `password` status and `password help` queries without a write confirmation.
Password setters are blocked by this page and the Web backend: use encrypted
Management RF. A saved override replaces the compiled fallback; existing
authenticated sessions remain valid until reboot. No password value is shown
in public status.

## Test and build without changing a radio

```sh
make -C firmware/esp32 mast-admin-ui-test
make -C firmware/esp32 mast-admin-browser-test
node firmware/esp32/tests/host_admin_browser.mjs
make -C firmware/esp32 beta-client-test
make -C firmware/esp32 beta-build
```

The JS fixtures execute the shipped page script against native command/result
shapes, including expiry, exact 4 KiB reads, resumed uploads, draft preservation,
scoped data validation, stage/commit separation and uncertain outcomes.
The browser target uses Node 22+ and system Chrome (`CHROME=/path/to/chrome`
overrides the executable), a loopback fixture, and project-local scratch
files. It checks real forms, disabled controls, DOM escaping and the existing
content-security policy. It does not contact hardware.
The browser fixtures also check command-level/malformed readback errors,
Wasm-targeted inspection, mixed restore scopes and signed firmware
upload/reboot outcomes. The host fixture checks token authentication,
capability-disabled controls, restricted secret transfer, source snapshots
and retained drafts. It also checks scoped relay/room control independently
of the native bot, different-role reads after a lost write and exact setting
readback before unlocking. The host HTTP adapter tests use local owner-socket
fixtures and real native role engines with durable restart readbacks:

```sh
mkdir -p .tmp/admin-http-go
TMPDIR="$PWD/.tmp/admin-http-go" go test ./internal/app -run 'TestHostAdmin|TestHostRoleAdmin'
```

`beta-build` uses public build-only settings and reports ESP32 flash/RAM size;
it neither uploads nor restarts a device. The native HTTP/backend fixture is:

```sh
make -C firmware/esp32 lifecycle-test BOT_INTEGRATION=1 ADMIN_INTEGRATION=1 \
  TEST_FLAGS='-DMESHCORE_MAST_ADMIN=1 -DMESHCORE_MAST_WEB_TEST=1'
```
