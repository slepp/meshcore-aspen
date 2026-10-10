# Manage Aspen and install a command over LoRa

Use a companion radio to log in to Aspen's **Management** identity and read
or change node settings. To add a Lua file over LoRa, use the supplied Python
client with a separate KISS gateway radio. Neither route needs WiFi on Aspen
or a firmware reflash. The [website walkthrough](https://ve6slp.ca/projects/meshcore/operate/radio-admin/)
shows the two connections and labelled example conversations.

## Choose the endpoint and connection

| What you want to do | Destination | Connection |
| --- | --- | --- |
| Run `!hello mesh` or `!help` | Bot identity | Ordinary encrypted bot DM; no admin login |
| Read settings or run an admin command | Aspen Management identity | Companion's native repeater/admin console, or `meshcli` |
| Upload a Lua file from your computer | Aspen Management identity | `admin.py` through a separate raw-KISS gateway |
| Manage Pine | Pine-Relay/repeater identity | Pine's authenticated admin console; see the [Pine guide](../../firmware/nrf52840/README.md) for its supported commands and limits |
| Manage a host role locally | Host owner endpoint | Its private owner socket; see [host setup](../../HOST_GUIDE.md) |

Management and the bot have separate public keys. Use the full Management
public key from your existing node setup or contact, not the bot's key.
Management advertises as a repeater-type CLI endpoint but does not forward
packets. Public channels, nicknames and BLE pairing do not grant admin access.
Ordinary bot DM `!admin` is not a file-upload or credentials transport.
Trusted full-key bot owner controls are bounded; use Management for bulk source
and data administration.

The sender and Aspen must use the same frequency, bandwidth, spreading factor
and coding rate. Retain existing identities and settings. Obtain the existing
Management password or already authorized full companion key from the node
operator. Do not extract private keys from an app or radio to follow this guide.
The installed firmware must support the command you want: read its help and
capabilities rather than assuming a download contains every change on `main`.

## Read settings through a companion radio

In a MeshCore app that exposes the native repeater administrator console,
select the **Management** contact, log in with its admin password and send
`status`, then `bot status`. Use the repeater/admin console, not ordinary chat
to the bot. App menu names vary; if your app does not expose this console,
use `meshcli` with your companion radio instead.

Connect to a native companion over USB:

```sh
meshcli -s /dev/serial/by-id/YOUR_COMPANION
```

Alternatively use `meshcli -t COMPANION_HOST -p COMPANION_PORT` for native
companion TCP. This is **not** a KISS socket. In the interactive console:

```text
add_contact MANAGEMENT_PUBLIC_KEY64 2 mast
reset_path mast
login mast ADMIN_PASSWORD
cmd mast status
wmt8
cmd mast "bot status"
wmt8
cmd mast "source status"
wmt8
```

Replace the public-key/password placeholders locally. Do not share console
history containing a password. For an already authorized full companion key,
use `login mast ""`. Login uses native encrypted `ANON_REQ` with timestamp and
password (or an authorized full key with an empty password). Subsequent
commands and replies use native encrypted CLI `TXT_MSG`; this is the existing
Management protocol, not a public bot command.

`cmd` does not request a delivery ACK. `wmt8` waits for the CLI reply; if already
queued, use `sync_msgs`. A lost reply is an uncertain outcome, not a confirmed
failure. Inspect affected settings and `job` before repeating a mutation.
After login, `req_status mast` reads native device counters and
`req_telemetry mast` reads permitted native Cayenne LPP sensor data.
`telemetry status` instead describes the optional metrics publisher.

## Add one Lua file through a KISS gateway

This route needs an **independent MeshCore KISS radio** reachable over TCP.
`--gateway` is that radio's host, not Aspen's Management address, an app BLE
connection or native companion TCP. KISS carries the client's packets; the
Python client owns a separate sender identity and performs encrypted Management
login. Keep the KISS network private or behind a protected tunnel.

To prepare a separate gateway, use the
[WiFi modem setup](../../HOST_GUIDE.md#start-with-a-wifi-radio-and-go-host)
on a different radio, not the target node. A Go host is not required just to
use that modem's KISS port.

From the source repository root, follow the
[Python prerequisites](../../firmware/esp32/MAST_ADMIN.md#cli-prerequisites)
if your environment is not ready, then run `python3 tools/hardware/admin.py --help`.
Provide an existing private 32-byte Ed25519 sender seed file and a password
file, both regular files with mode `0600`. The password file contains only
the password, without a trailing newline. The seed is the client's identity;
it need not be the mobile app's identity. Use an operator-managed client seed,
not a private key copied from a companion. Omit `--password-file` only if that
client's full public key is already authorized.

### If you need a new client identity

Create it once, offline, in an existing private directory outside the source
checkout. This does not change the node or grant authority. Replace the path;
the script refuses to overwrite an existing file and prints only the public key:

```sh
python3 - /absolute/private/rf-client.seed <<'PY'
import os, secrets, sys
from cryptography.hazmat.primitives.asymmetric import ed25519
from cryptography.hazmat.primitives import serialization
seed = secrets.token_bytes(32)
fd = os.open(sys.argv[1], os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
with os.fdopen(fd, "wb") as output:
    output.write(seed)
    output.flush()
    os.fsync(output.fileno())
public = ed25519.Ed25519PrivateKey.from_private_bytes(seed).public_key()
print("Client public key:", public.public_bytes(
    serialization.Encoding.Raw, serialization.PublicFormat.Raw).hex())
PY
```

Log in with the existing Management password, or ask its authorized operator
to grant this exact public key. Do not generate another seed for each upload.
Retain the seed and the client's adjacent `.clock` timestamp file; its directory
must be writable. Use one active client per seed so concurrent clients do not
compete for the same replay state. Keep both files private and out of commits.

### Upload the greeting

1. **Read capabilities and export before changing anything.** Substitute your
   own gateway, full target public key and protected file paths throughout:

   ```sh
   python3 tools/hardware/admin.py \
     --gateway KISS_GATEWAY_HOST --port 8001 --target MANAGEMENT_PUBLIC_KEY64 \
     --seed-file /absolute/private/rf-client.seed \
     --password-file /absolute/private/node-password \
     command 'source api sources'
   python3 tools/hardware/admin.py \
     --gateway KISS_GATEWAY_HOST --port 8001 --target MANAGEMENT_PUBLIC_KEY64 \
     --seed-file /absolute/private/rf-client.seed \
     --password-file /absolute/private/node-password \
     download saved-source.lua
   chmod 600 saved-source.lua
   ```

   Expect `format=meshcore-sources/1` and `commit-base=sha256` for named
   sources. `download` writes a new file and verifies complete readback;
   the following `chmod` makes the saved source private.
   Keep it out of public commits. `source-list` lists existing file names;
   `source-export NAME NEW_FILE` exports one installed file. Bundled-command
   references are not separate installed files to export.

2. **Save `hello.lua` as UTF-8 with LF newlines and a final newline:**

   ```lua
   function hello(name)
     return "Hello " .. name
   end
   command("hello", "name:string:32", "Greet a name",
           "hello", "public", "!hello mesh")
   ```

   You can also use the supplied `firmware/runtime/plugins/template.lua`.
   Test without RF using the [greeting recipe](../../firmware/runtime/BOT_RECIPES.md#add-a-greeting-without-replacing-other-files).
   Choose a source name not already used by an unrelated file, and avoid
   duplicate command names.

3. **Install and check the selected source:**

   ```sh
   python3 tools/hardware/admin.py \
     --gateway KISS_GATEWAY_HOST --port 8001 --target MANAGEMENT_PUBLIC_KEY64 \
     --seed-file /absolute/private/rf-client.seed \
     --password-file /absolute/private/node-password \
     source-install greeting hello.lua
   python3 tools/hardware/admin.py \
     --gateway KISS_GATEWAY_HOST --port 8001 --target MANAGEMENT_PUBLIC_KEY64 \
     --seed-file /absolute/private/rf-client.seed \
     --password-file /absolute/private/node-password status
   ```

   The client reads the existing set, composes the named file, transfers the
   candidate and conditionally commits against the original hash. Other named
   files remain. A concurrent source change or namespace collision rejects
   installation rather than overwriting that change. The complete encoded set
   shares the 4096-byte limit and eight-command registry.

   Look for `Durable chunk N/TOTAL`, then `source durably saved and active`.
   The client checks the final selected SHA-256. A disabled bot instead reports
   `source saved; bot disabled; loads on next enabled boot`; it cannot answer
   until enabled. Do not enable or restart a bot without inspecting its existing
   channel policy. A timeout ending with `Activation still pending` means read
   `source status` and `source hash`, not immediately repeat the install.

4. **Open an ordinary bot DM** from a separate companion on the same PHY.
   Advertise your companion so the bot learns its contact. Send `!hello mesh`;
   expect `Hello mesh`. Send `!help hello` for generated help. This conversation
   uses the bot's identity, not Management and not an admin session.

Named installation retains other files. Whole-set `install` and
`package-install` replace that runtime's entire installed command set; export
first. Source activation does not erase identities, settings, scoped KV or
notes. It cancels that runtime's pending work. Durable source returns after a
restart; reverting code does not undo data changes or packets already sent.

## What happens on the radio

The uploader logs in, sends `source begin`, numbered `source chunk` commands,
then `source commit`, and reads status/hash. A **durable chunk ACK** acknowledges
readback and saved progress, not merely reception of a native packet.
Commit acceptance starts asynchronous verification; it is not activation.

For understanding the wire commands, this fixture is the **whole 144-byte
`hello.lua` above**, not a named source bundle. **Do not send this raw commit
on a node whose installed files you want to keep:** it replaces the whole Lua
set. The tutorial's `source-install` composes a different bundle with its own
size/hash. The fixture's SHA-256 is
`aff977000622bb048a971afd190e1c4bfd4ec5f00447d62311e2185e8cecbcf5`.

```text
source begin aff977000622bb04 144 aff977000622bb048a971afd190e1c4bfd4ec5f00447d62311e2185e8cecbcf5
ACK aff977000622bb04 next=0
source chunk aff977000622bb04 0 66756e6374696f6e2068656c6c6f286e616d65290a202072657475726e202248656c6c6f2022202e2e206e616d650a65
ACK aff977000622bb04 next=1
source chunk aff977000622bb04 1 6e640a636f6d6d616e64282268656c6c6f222c20226e616d653a737472696e673a3332222c202247726565742061206e
ACK aff977000622bb04 next=2
source chunk aff977000622bb04 2 616d65222c0a20202020202020202268656c6c6f222c20227075626c6963222c20222168656c6c6f206d65736822290a
ACK aff977000622bb04 next=3
source commit aff977000622bb04
Accepted verification; source status reports durable activation outcome
source status
source hash
```

Status includes generation/slot/progress fields plus the live outcome; wait
for `source durably saved and active` and a matching `SHA256 … gen=N`.
Generation and slot values depend on the existing node, so they are not fixture
constants. Never substitute this fixture's hash for your actual uploaded file.

Chunks are sequential, at most 48 source bytes (48 except the final chunk).
Matching `begin` resumes saved progress after a restart; an identical repeated
chunk is safe, but a gap or conflicting duplicate fails. The Python upload
loop makes at most three attempts for an identical chunk on timeout. It polls
activation up to 20 times with 0.5-second pauses; RF round trips add time.
At 4096 bytes, 86 chunks plus their replies cost real airtime. Do not paste a
whole file into an ordinary DM, compress it or upload Lua bytecode.

The RF transport can additionally retransmit a command twice with the same
timestamp/text to retrieve Management's cached outcome. These are not new
administrative attempts after an unknown result. `--no-command-retry` disables
transport retransmissions; bounded identical-chunk upload retries still apply.

## Commands to use over authenticated Management RF

Start with read-only requests. In `meshcli`, wrap each native command as
`cmd mast "COMMAND"` and read its reply with `wmt8`. With the Python client,
use the same connection options followed by `command 'COMMAND'`.

| Task | Commands to discover/check | Change and consequence |
| --- | --- | --- |
| Device health | `help`, `help TOPIC PAGE`, `ver`, `board`, `status`, `stats help`, `job` | Read-only; use the node's help for supported topics |
| Roles and names | `roles`, `roles list 2`, `bot status`, `role config bot`, `role name management`, `bot key` | `role name ROLE TEXT` persists without replacing a key; `roles MASK`, `bot on/off` save boot selection; `apply` restarts |
| Shared radio | `get radio`, `get freq`, `get tx` | `radio HZ BW_HZ SF CR DBM` changes every role's PHY after the old-PHY reply; `tempradio SECONDS …` restores the durable profile later |
| Adverts | `get autoadvert`, `role help` | `set autoadvert off/on`; `role advert ROLE zerohop/flood` transmits deliberately; queue acceptance is not RF reception |
| Native sensors | Companion `req_status mast`, `req_telemetry mast`; `stats sensors` | Measurements depend on board and permissions; unsupported telemetry values are omitted |
| Bot diagnostics and jobs | `bot status`, `bot stats`, `bot admission`, `bot log`, `bot limits`, `bot diagnostics` | `bot cancel` stops bot work; inspect help and affected job before changing limits |
| Channels and admission | `bot aliases`, `bot membership SLOT`, `bot access CONTEXT list OFFSET`, `bot thread CONTEXT list OFFSET` | Joins reset to permissive default 63 and clear overrides; disable/restart before creating an addressed-only membership, then configure default 12 before enabling |
| Program lifecycle | `source api sources`, `source status`, `source hash`, `source metadata` | Named Python install preserves other files; `source retry`, `source rollback`, `source remove`, `source cancel` have distinct recovery effects |
| Management authority | `get acl`, `password`, `key management pending` | `setperm FULL_KEY64 3/0` grants/removes a saved admin entry; `trust FULL_KEY/none` controls additional singleton trust; staged key rotation needs restart and changes identity |
| WiFi | `wifi status`, `get wifi.enabled`, `get wifi.ip` | Save credentials over encrypted RF only; `wifi apply` reconnects; disabling WiFi retains RF Management |
| Native services | `bot https status`, `telemetry endpoint status`, `mqtt status`, `cloudroom status` | Availability varies by build; use each service guide for destinations, grants and protected credential uploads |
| Restart | `status`, `job` | `apply` or `reboot` restarts the device; inspect running state before retrying an uncertain restart |

See the [Management command catalog](../../COMMANDS.md#management-manual),
[exact command syntax/ranges](../../firmware/esp32/MAST_ADMIN.md#common-control-commands),
[Management ACL](../../firmware/esp32/MAST_ADMIN.md#management-acl) and
[channel policy](../../firmware/esp32/MAST_ADMIN.md#command-channel-and-path-policy).
Credentials, key imports and bulk data use the documented protected-file
operations, not public channels or secret command-line arguments.
Native role ACLs are separate from Management authorization.

## Recover without erasing the node

* **No reply:** confirm Management's key, login, route and complete PHY; use
  `sync_msgs`. Do not retune production radios just to troubleshoot a tutorial.
* **Chunk reply lost:** inspect matching `source begin` progress. Only resend
  the identical numbered chunk; the current program stays selected.
* **Commit or install uncertain:** read `source status` and `source hash`.
  Do not blindly resend commit, reboot or another mutating command.
* **Candidate rejected:** inspect the error. The old program remains selected.
  Fix the file/namespace/limits locally before a new explicit installation.
* **Verified selection cannot run:** inspect status and bot readiness, then
  explicitly choose `source retry`, `source rollback`, or `source remove`.
  Removal restores bundled handlers and removes the installed set; rollback
  restores code/help, not data.

Native Management remains available when the bot is disabled or custom source
is quarantined. Source repair is not provisioning: do not format a filesystem,
erase flash or rotate identities to install or recover a command.
