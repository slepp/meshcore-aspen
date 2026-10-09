# Join an Aspen room from the web

Open the room Worker's HTTPS address on your desktop or phone. Select a channel,
enter your operator-created username and account password, and choose **Join
room**. The operator grants each account access to particular channels and sets
its display name. [Create accounts](#operator-created-accounts) before enabling
account login. A development service without `WEB_USERS` keeps the display-name
and shared room-password flow.
Aliases pointing to the same backend show the same ordered messages; another
backend has its own conversation.

For the same flow in a Linux terminal, [build and run the terminal
client](../../clients/room-tui/README.md). It uses the same account grants and
shared history, with a separate locally stored desktop keypair.

The interface follows the system's light/dark preference, including changes
while it is open. Text and controls also adapt to browser text sizing and
high-contrast settings. Tab moves through controls; the mobile channel drawer
keeps keyboard focus inside until you choose a channel or close it with Escape.
The skip link goes directly to the conversation. Screen-reader status updates
announce new incoming messages and saved posts without rereading the history.

The interface uses IP only. Web posts are stored in the same SQLite history as
native radio posts, then delivered to connected browsers and eligible radio
members. A radio member's confirmed history cursor advances only on its native
ACK. Viewing a message on the web does not acknowledge it for a companion.
The web identity is separate from any companion or management identity.

Radio authors show the name from their latest signed MeshCore advert received
by a connected frontend. The Worker verifies the full public key, signature and
configured public-region scope before saving a name, and sends changes to joined
clients. Names survive a Worker restart; newer signed timestamps replace older
names. The directory retains up to 4096 recently advertised identities per
backend. A sender without a received name still shows its key fingerprint.
Every message also shows a fingerprint: names can repeat, and an advert means
that key chose a label, not that the label is a verified person. Only full keys
are used for lookup; matching short prefixes never merge people.

## Writing and reconnecting

A display name uses 1..24 UTF-8 bytes and cannot contain a colon or control
character. Web posts store `Name: text` in the shared history. The entire string
must fit 151 UTF-8 bytes; a name therefore uses part of the message budget.
The web timeline displays the name beside the body, while radio history
includes the prefix in the text. There is no separate long-form web version.

Send with the arrow or Enter; Shift + Enter adds a line break. Drafts survive a
page reload. If the post response is lost, **Check / retry** sends the same
stored UUID and text. The Worker returns the already committed message when it
exists. The browser never retries an uncertain post automatically and keeps
its pending post through reloads or an expired login. Rejoin, then check it.
Changing the display name before checking a committed post does not rewrite it.

If a saved post is slow to reach a companion radio, do not resend it from the
browser. The radio reader advances only after its native history ACK. Opaque
radio delivery retries that same stored message up to three times, after
30, 60 and 120 second waits. If that budget is exhausted, rejoin the room in the
companion app to resume its pending history; reset its path first if necessary.
Reloading the browser or sending an advert does not reset that radio state.

Each message shows **Saved** separately from its RF delivery status. **Queued**
includes history waiting behind an earlier unacknowledged message. **Transmitted,
awaiting ACK** means the frontend confirmed transmission, not that a companion
received it. **Transmission uncertain** and **transmission failed** show the
frontend's actual result. **RF ACK** counts native recipient acknowledgements;
those remain after a Worker restart. Counts refer to room-alias/client sessions,
so one companion joined to two aliases can account for two deliveries.

**Paused** means the selected frontend is offline or its authorization/scope no
longer matches the pending delivery. **Retrying** shows another dispatch attempt;
**retry budget exhausted** means no more automatic attempts remain, but a late
native ACK can still complete delivery. The next retry time is shown while its
frontend is eligible. The active channel refreshes the latest 100 messages'
status every five seconds while the page is visible. Older pages show their
status when loaded. An unavailable status is displayed explicitly. Existing
history without retained ACK evidence is not relabeled as acknowledged.

Joined channels remain connected while you switch between them. Badges count
new messages arriving in another channel during this page session. Reopening
the site restores its selected channel and login cookies, then catches up from
the shared history. Earlier messages load in pages of 100. `HISTORY_LIMIT`
applies to both browser history and radio catch-up.

For account login, this browser generates an Ed25519 keypair compatible with
MeshCore. Its private key is nonextractable and stays in this origin's IndexedDB
key store; the public key is its message author. A one-use, two-minute challenge
binds its ownership signature to the service origin, alias, username and key.
Each browser profile or terminal client is a separate device, even when both use
the same account. A device key belongs to one account; use another browser
profile or terminal state directory for another account.

The browser stores the username, name preference, drafts and uncertain post
IDs/text/author in local storage. Passwords are not written there.
Cookies grant access for up to 30 days. Leaving a room ends its browser session
but keeps unsent work; clearing site data loses the identity and unsent work.
There is no device-key transfer or key backup flow yet. A browser cannot export
the private key; keep its profile if you need to keep that device identity.
Account login does not create radio membership, announce this desktop over RF,
select a home tower, or add direct messages. Display names are not unique.

## Operator-created accounts

From `services/shared-room`, create a private account file. The command prompts
twice for a password without echoing it; use a long, unique passphrase of at least
16 UTF-8 bytes. Passwords are salted PBKDF2-SHA256 hashes with 100,000 iterations,
the Workers WebCrypto iteration limit, and logins are limited to ten attempts
per address, per backend, per minute. Keep this temporary password login behind
HTTPS; passkeys and person/device linking are not implemented.

```sh
node tools/web-users.mjs \
  --file "$HOME/.config/aspen-room-operator/users.json" \
  --username alice --name Alice --aliases A,SharedB
node tools/web-users.mjs \
  --file "$HOME/.config/aspen-room-operator/users.json" \
  --upload --account CLOUDFLARE_ACCOUNT_ID
```

Use alias IDs from `ALIASES`, not room labels. The tool creates a `0700` directory
and `0600` file, refuses public files and symlinks, and uploads `WEB_USERS` only
as a secret after checking the active Worker binding types. Never put it in
Wrangler `vars`. Uploading even an empty `{}` switches desktop login to accounts
and disables shared room-password web login; radio passwords stay unchanged.
Run the creation command again to replace a password, name or grants. Use
`--username alice --remove` followed by `--upload` to revoke an account.
Any change to an account or its room configuration invalidates its sessions;
its signed-in devices must log in again, retaining their keys and unsent work.

For unattended local setup, `--password-file PRIVATE_FILE` reads an
operator-owned `0600`/`0400` file instead of prompting. It may end with one
newline. The tool does not delete that file. Keep account files outside the
checkout and do not pass passwords as command arguments.

Before switching an existing service to accounts, confirm or retry any uncertain
posts under their old login. Old shared-password messages remain in history
with their original author; new account devices get real public-key authors.
Clients refuse to resend an old pending post under a different author, because
that would defeat deduplication. Do not clear browser storage to bypass that
warning.

## Browser API

These routes use room-scoped session cookies, not `FRONTENDS` bearer tokens.
Requests and responses are JSON; responses are not cached. Mutating requests
use `Content-Type: application/json` and an `Origin` matching the Worker.
Login and sockets require HTTPS, except for local development on localhost.
The browser API does not accept radio operations or expose room private keys.

| Route | Request / result |
| --- | --- |
| `GET /v1/web/rooms` | Public `{rooms:[{id,name,publicKey}],maxPostBytes:151,loginMode,deviceProtocol}`; no login required |
| `POST /v1/web/rooms/{alias}/challenge` | Account mode: `{username,publicKey}`; returns `{nonce,message,expires,protocol:"aspen-room.device.v1"}` |
| `POST /v1/web/rooms/{alias}/login` | Account mode: `{username,password,publicKey,nonce,signature}`; development room-password mode: `{identity,name,password}`. Returns `{author,name,username,expires,maxPostBytes}` and a scoped HttpOnly cookie |
| `GET /v1/web/rooms/{alias}/session` | Returns the current browser author, name and expiry |
| `POST /v1/web/rooms/{alias}/logout` | Ends this session and closes its sockets |
| `GET /v1/web/rooms/{alias}/history` | Latest 100 visible messages, oldest first, and `{more,floor,profiles}` |
| `GET .../history?before={seq}` | Earlier page; `more` means more earlier history |
| `GET .../history?after={seq}` | Catch-up page; `more` means another forward page |
| `POST /v1/web/rooms/{alias}/posts` | `{id:"<UUID v4>",text}`; returns `{message,duplicate}` only after storage has synced |
| `GET /v1/web/rooms/{alias}/socket?since={seq}` | Same-origin WebSocket with `aspen-room.web.v1`; cookie authentication |

Browser history, post results and socket messages include an `rf` summary on
each message: nonnegative integer `recipients`, `queued`, `sent`,
`acknowledged`, `uncertain`, `failed`, `paused`, `retrying`, `exhausted` and
`attempts`, plus `nextRetryAt` (Unix milliseconds or `null`). The five phase
counts (`queued`, `sent`, `acknowledged`, `uncertain`, `failed`) sum to
`recipients`; pause/retry/exhaustion counts can overlap those phases. `attempts`
is the largest cumulative dispatch count for a recipient, not a claim that
every dispatch reached RF. Native radio frames and their retry limits are
unchanged. The web status fields do not contain ACK proofs or private keys.

Posts deduplicate by backend, stable web author and UUID. Reusing a UUID for
different content returns 409. A duplicate's original alias, display name,
timestamp and sequence remain unchanged. History includes canonical `seq`,
`timestamp`, `originAlias`, `author`, `clientTimestamp`, `text`, and nullable
`webName`. Radio messages have no `webName`.

The socket starts with `{type:"ready",version:1,alias,publicKey,name}`, followed
by `{type:"profile",profile}` and `{type:"message",message}` in sequence order,
including the sender's posts. Profile updates can arrive independently.
Each profile has `{publicKey,name,advertType,timestamp,source}`; source is
`radio` for a signed advert or `desktop` for an account-authenticated key.
Match full keys, keep the newest timestamp and never merge ambiguous prefixes.
It replays up to 100 messages after `since`. A further backlog produces
`{type:"catchup",cursor}`; load forward history, then send
`{op:"sync",since:<last sequence>}` to align the connection. Merge history and
socket events by `seq`, because a catch-up response can overlap live messages.
The DO uses hibernating sockets, checks session expiry/configuration after wake,
and retains radio delivery state independently.

Browser history cursors are read positions, not RF delivery proofs. The service
does not promise immediate radio reception, unique display names, attachments,
or a web identity shared with a companion.
