# Directed MeshCore session link

Use one persistent companion connection per configured Base to send a DM to a
pinned operator and receive DMs through a durable local mailbox. The Copilot
extension owns a foreground child listener and contributes:

- `meshcore_send({text, feed?})`: send once to the configured full target key.
- `meshcore_inbox({cursor?, limit?})`: read messages after a returned cursor.
- `meshcore_link_status({})`: connection identity, reconnect state and recent sends.

Incoming messages enqueue a quoted, explicitly untrusted notification in the
current session, including the sender prefix, resolved key when available, and
receiving Base. Message text does not authorize commands or change instructions.
Only the local operator can authorize an action. Notifications enqueue at most
one batch of 20 between session-idle events, rather than accumulating an
unbounded prompt queue. Reading the inbox does not acknowledge notifications.

## Configure and activate

Use an existing Python environment containing the `meshcore` SDK. Create a
private, untracked configuration file with mode `0600`:

```json
{
  "target": "<operator's full 64-lowercase-hex public key>",
  "default_feed": "Example-Base",
  "feeds": [
    {
      "name": "Example-Base",
      "host": "<companion host>",
      "port": 5000,
      "public_key": "<Base's full 64-lowercase-hex public key>"
    }
  ]
}
```

Configure only companion services, not a KISS shared-modem endpoint. Pin each
Base's **actual** advertised name and full public key using device information
from a read-only SDK connection and the operator inventory. A hostname or service
label is not necessarily the radio's name. Up to eight feeds are supported;
names and endpoints must be distinct. A partially invalid configuration fails
before opening connections. A valid unavailable feed retries independently.
There is no automatic subnet scan or unsolicited extension startup in this repo.

Reserve one companion slot per feed; existing companion clients are not
disconnected. Some native services permit only two clients. Do not start a
standalone listener and the extension against the same state directory: the
second listener fails instead of opening another connection.

After reading the Copilot extension authoring guide and scaffolding a
session-scoped `meshcore-directed-link`, prepare its loader:

```sh
<sdk-python> -m tools.meshcore_link.install \
  --session <copilot-session-workspace> \
  --config <private-config.json> --state <private-state-directory>
```

Then call `extensions_reload({})` in Copilot. This reloads **all** extensions;
coordinate with other session work.
Inspect `extensions_manage({operation:"inspect", name:"meshcore-directed-link"})`,
then call `meshcore_link_status`. Each reachable feed should say `connected`,
with its verified device identity. Reload after changing configuration.
The private loader pins the interpreter, project, configuration and state paths;
it contains no passwords. Keep the loader, configuration and mailbox untracked.

## Standalone commands

The listener can instead run in the foreground:

```sh
<sdk-python> -m tools.meshcore_link --state <private-state-directory> \
  serve --config <private-config.json>
```

From another shell:

```sh
<sdk-python> -m tools.meshcore_link --state <private-state-directory> status
<sdk-python> -m tools.meshcore_link --state <private-state-directory> inbox --cursor 0
<sdk-python> -m tools.meshcore_link --state <private-state-directory> send \
  "Build finished; reviewing results." --feed Example-Base
```

Messages are limited to 160 UTF-8 bytes. Sends never broadcast, advertise,
retune, change identities, alter credentials/ACLs or configure channels.
The target must exist in the sending companion's freshly retrieved contact table, and its
six-byte wire prefix must match exactly one full contact key. Unknown targets
and collisions fail before admission.

## Results, recovery and limits

The durable send ledger distinguishes `admitted`, `rejected`, `acknowledged`
and `uncertain`. `tx: submitted` means the companion returned `MSG_SENT`, **not**
that physical transmission was independently observed. `tx: confirmed_by_ack`
means the matching native ACK arrived. The ACK subscription starts before
submission; early ACK codes are retained until the response supplies the
expected four bytes. No ACK by 30 seconds, disconnect, or lost submission
response produces `uncertain`. Never automatically retry uncertain messages,
especially commands: the recipient may already have received them.
Interrupted admitted entries become uncertain on listener restart.

The listener uses SDK message fetching and event subscriptions, checks liveness
and missed mailbox pushes every 15 seconds, and reconnects with 1–30 second
backoff. It verifies both Base name and full key before fetching messages.
Each feed has one shared send/receive TCP connection. A second send on a busy
feed is not admitted. TCP upgrades or identity changes remain visible in status.
TCP companion links are not independently authenticated by this tool; use
trusted local/tunneled endpoints. Public-key pinning checks radio-reported identity,
not the authenticity of an arbitrary TCP server.

Incoming source information is a six-byte prefix from the companion protocol.
A full key is labeled `unique_contact` only if one contact matches; otherwise
the message is labeled `ambiguous` or `unresolved`, without guessing an identity.
Even uniquely resolved messages remain untrusted data. Dedup uses the received
prefix, sender timestamp, message type and text across feeds, retaining the
first receiving feed. Identical messages sent within the same timestamp second
can merge; a repeat beyond the retained dedup window can appear again.
SDK text decoding may already have replaced invalid wire UTF-8.

The private `0700` state directory retains 512 messages, 2,048 dedup fingerprints,
256 sends and a notification cursor. SQLite allocation is capped at 8 MiB;
request size is 16 KiB, inbound text 1,024 bytes and local clients 16. Inbox
`gap: true` means older messages were evicted; use `oldest_cursor` to locate
the retained range. Status returns the latest 20 sends. There is no disk log.
An incoming malformed message or storage/connection error is visible in feed
status. A failed enqueue leaves the notification cursor unchanged. A crash
between enqueue and cursor acknowledgement may repeat a notification.

The extension's attached child exits on stdin close or termination, and never
detaches. Listeners survive tool calls and context compaction, but not CLI exit,
extension reload, or `/clear`; durable messages and send outcomes survive if
the same private state directory is reused. `/clear` or reload reconnects the
same configured feeds. Messages during downtime depend on each companion's
own bounded queue. The inbox contains private DMs: protect or delete its state
directory when retiring the session.

## Validate without sending RF messages

```sh
<sdk-python> -m unittest discover -s tools/meshcore_link/tests -p 'test_*.py' -v
node --test tools/meshcore_link/tests/notifications.test.mjs
node --experimental-vm-modules --test tools/meshcore_link/tests/extension.test.mjs
node --check tools/meshcore_link/extension.mjs
```

Tests use fake companion and session events; they do not send messages or change
radio settings. Live validation should check `status` and distinct names/keys
only. Coordinate the first real directed send with the operator.
