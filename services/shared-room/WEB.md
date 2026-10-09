# Join an Aspen room from the web

Open `https://aspen.ve6slp.ca` on your desktop or phone. Select a channel, enter
your operator-created username and choose **Join with passkey**. Your device
asks for its usual PIN, fingerprint or other passkey verification. The operator
sets your display name and grants access to each channel.
[Register your first passkey](#operator-created-accounts) using the operator's
private enrollment link, or [link a new device](#link-another-device) from one
that is already signed in. Password sign-in is disabled in passkey mode.
Aliases pointing to the same backend show the same ordered messages; another
backend has its own conversation.

The configured deployment also serves `https://aspen.ve6slp.ca`; its
`workers.dev` address remains available to the radio frontend, but passkey
sign-in uses only the configured `PASSKEY_ORIGIN`. Change or remove
the custom-domain route in `wrangler.jsonc` when deploying in another Cloudflare
zone. Browser keys, cookies and unsent work belong to an origin: opening the
other address creates a separate browser device. Keep the original origin's
site data to retain its identity and drafts.

For the same flow in a Linux terminal, [build and run the terminal
client](../../clients/room-tui/README.md). Press Enter to show its five-minute
QR/copy-code request, then approve it in your signed-in browser. It joins the
same account and history with a separate locally stored device keypair.

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
The conversation puts the author, time and message first. Choose the small
message-details button beside a message to see its full device key, source and
radio delivery status. Names can repeat, and an advert means that key chose a
label, not that the label is a verified person. Only full keys are used for
lookup; matching short prefixes never merge people.

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

The radio icon and small count beside a message show retained **radio
acknowledgements**, not human read receipts. There is no **Seen** count. A
warning icon marks uncertain or failed transmissions or an exhausted retry
budget. Ordinary waiting and paused counts stay in **Message details** rather
than repeating under every message. Messages in the timeline are saved in the
room; an uncertain browser post stays beside the composer until checked.

In message details, **Waiting for transmission** includes prepared deliveries
and history blocked behind an earlier unacknowledged message. It counts
room-alias/client sessions, not positions in a frontend's transmit queue.
**Transmitted, awaiting ACK** means the frontend confirmed transmission, not
that a companion received it. **Transmission uncertain** and **transmission
failed** show the frontend's actual result. **Radio ACK** counts retained native
recipient acknowledgements. The denominator combines sessions still behind
this message and sessions with recorded dispatch evidence; it is not a fixed
audience or a count of people online. One companion joined to two aliases can
account for two deliveries.

**Paused** means the selected frontend is offline or its authorization/scope no
longer matches the pending delivery. **Retried** means another dispatch was
attempted;
**retry budget exhausted** means no more automatic attempts remain, but a late
native ACK can still complete delivery. The next retry time is shown while its
frontend is eligible. Paused overlaps waiting/transmitted/uncertain/failed
counts; do not add it to them. The active channel refreshes the latest 100
messages' status every five seconds while connected and visible, with one
request at a time. Older pages show status when loaded and refresh when their
message details are opened. An unavailable status is named in message details.
Existing history without retained ACK evidence is not relabeled as acknowledged.

Radio sessions, pending history and ACK-confirmed cursors live in the Worker's
SQLite storage and survive restarts or a radio disappearing. A disconnected
frontend pauses its sessions; a silent radio on a connected frontend can still
be waiting for an ACK. New messages do not advance that radio's cursor.
An ACK advances its history, allowing the next message to transmit. Persistent
sessions can therefore leave the same waiting/paused counts on many messages.
Browser catch-up instead uses sequence numbers in the current page and reloads
stored room history after reconnecting; it does not report what a person read.

Joined channels remain connected while you switch between them. Badges count
new messages arriving in another channel during this page session. Reopening
the site restores its selected channel and login cookies, then catches up from
the shared history. Earlier messages load in pages of 100. `HISTORY_LIMIT`
applies to both browser history and radio catch-up.

For account login, this browser generates an Ed25519 keypair compatible with
MeshCore. Its private key is nonextractable and stays in this origin's IndexedDB
key store; the public key is its message author. A one-use, two-minute challenge
binds its ownership signature to the service origin, action and device key.
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

## Link another device

On the new browser, choose **Link this browser from a signed-in device**. In the
TUI, select a room and press Enter. Both show a QR code, a 24-character copy code
and the requesting device's full public key. The request expires after five
minutes; restarting the TUI or closing the request requires a new code.

On a signed-in browser, scan the QR code or open **Devices**, paste the code and
choose **Inspect device request**. Compare the full key with the requesting
device before choosing **Approve device**. Never approve an unsolicited code.
The requesting device claims the approval once and joins with its own key.
Nothing copies the approving device's key or passkey.

In **Devices**, add a second passkey or revoke a device. Revocation ends that
key's account and room access, including existing room sessions. It also
cancels unclaimed approvals made by that device. Keys and drafts are not
deleted. A revoked key cannot be reassigned or re-enabled; use a new browser
profile or TUI state directory. Keep a second passkey or another approved
device available before losing access to your only sign-in device.

## Operator-created accounts

From `services/shared-room`, create an account and upload its name and room
grants as a secret. `--passkey` does not ask for or save a usable password.
Existing account records can be retained without changing device authors or
drafts; their old password hashes are no longer accepted for sign-in.

```sh
node tools/web-users.mjs \
  --file "$HOME/.config/aspen-room-operator/users.json" \
  --username alice --name Alice --aliases A,SharedB --passkey
node tools/web-users.mjs \
  --file "$HOME/.config/aspen-room-operator/users.json" \
  --upload --account CLOUDFLARE_ACCOUNT_ID
```

Use alias IDs from `ALIASES`, not room labels. The tool creates a `0700` directory
and `0600` file, refuses public files and symlinks, and uploads `WEB_USERS` only
as a secret after checking the active Worker binding types. Never put it in
Wrangler `vars`. Radio passwords stay unchanged. Run the creation command again
to change a name or grants. Use
`--username alice --remove` followed by `--upload` to revoke an account.
Any change to an account or its room configuration invalidates its sessions;
its signed-in devices must log in again, retaining their keys and unsent work.

Enable the account authority at the canonical origin:

```sh
CLOUDFLARE_ACCOUNT_ID=CLOUDFLARE_ACCOUNT_ID npx wrangler deploy --keep-vars \
  --var WEB_AUTH_MODE:passkey \
  --var PASSKEY_ORIGIN:https://aspen.ve6slp.ca \
  --var PASSKEY_RP_ID:ve6slp.ca
node tools/web-passkeys.mjs \
  --users "$HOME/.config/aspen-room-operator/users.json" \
  --username alice --origin https://aspen.ve6slp.ca \
  --enrollments "$HOME/.config/aspen-room-operator/enrollments.json" \
  --link "$HOME/.config/aspen-room-operator/alice-enrollment.txt" \
  --upload --account CLOUDFLARE_ACCOUNT_ID
```

The enrollment tool stores only token hashes in `WEB_ENROLLMENTS`. Deliver the
private link file only to its account owner. It expires in 30 minutes and
registers the first passkey once; the page removes the token from its address
bar before using it. The owner completes passkey verification in a real browser.
Additional passkeys are added from **Devices** on an approved device.
Never put enrollment tokens, account records or room keys in Wrangler `vars`.

The `Accounts` SQLite Durable Object owns passkeys, global device/account
bindings, approvals and account sessions. Its `v2` migration adds a namespace;
it does not recreate or erase `ROOMS`. Keep both migrations and bindings when
deploying. `--keep-vars` retains the live origin and mode on later deployments.
Changing origin creates a new browser key store; changing RP ID can make old
passkeys unusable.

Registration and sign-in require user verification and check the exact origin,
RP ID, challenge, credential and authenticator counter. Challenges last two
minutes and are single use. Account endpoints allow 60 requests per source
address per minute; queues are bounded. Account and room cookies last up to 30
days, use distinct paths and are HttpOnly/Secure/SameSite=Strict. Previously
issued account-mode room cookies remain valid until their original expiry or
a grant/device revocation; password sign-in cannot issue new ones.

For isolated development, leaving `WEB_AUTH_MODE` unset retains the older
account-password mode with `WEB_USERS`, or the room-password mode without it.
Those paths are not available on a passkey-mode deployment.

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
| `GET /v1/web/rooms` | Public `{rooms:[{id,name,publicKey}],maxPostBytes:151,loginMode,deviceProtocol,accountDeviceProtocol,authOrigin}`; no login required |
| `POST /v1/web/rooms/{alias}/challenge` | Disabled in passkey mode; legacy development account challenge |
| `POST /v1/web/rooms/{alias}/login` | Passkey mode: `{ticket}` from the account authority. Returns `{author,name,username,expires,maxPostBytes}` and a scoped HttpOnly room cookie |
| `GET /v1/web/rooms/{alias}/session` | Returns the current browser author, name and expiry |
| `POST /v1/web/rooms/{alias}/logout` | Ends this session and closes its sockets |
| `GET /v1/web/rooms/{alias}/history` | Latest 100 visible messages, oldest first, and `{more,floor,profiles}` |
| `GET .../history?before={seq}` | Earlier page; `more` means more earlier history |
| `GET .../history?after={seq}` | Catch-up page; `more` means another forward page |
| `POST /v1/web/rooms/{alias}/posts` | `{id:"<UUID v4>",text}`; returns `{message,duplicate}` only after storage has synced |
| `GET /v1/web/rooms/{alias}/socket?since={seq}` | Same-origin WebSocket with `aspen-room.web.v1`; cookie authentication |

Account routes are under `/v1/auth/` at `PASSKEY_ORIGIN` and use the
`aspen_account` cookie scoped to that path. POST bodies are limited to 16 KiB.
An account ownership challenge returns `{id,purpose,origin,publicKey,message,
protocol:"aspen-account.device.v1",expires}`. The signed UTF-8 message is exactly
`protocol + "\n" + origin + "\n" + purpose + "\n" + publicKey + "\n" + id`.
Keys and signatures are lowercase hex; WebAuthn JSON fields use base64url.

| Account endpoint | POST request / result |
| --- | --- |
| `register/options` | `{username,publicKey,enrollment?}`; first registration needs the enrollment token, later registration needs that device's account cookie. Returns a device challenge and WebAuthn `options` |
| `register/verify` | `{id,signature,response}`; verifies the device and WebAuthn registration, then issues an account cookie |
| `authenticate/options` | `{username,publicKey}`; returns a device challenge and WebAuthn `options` |
| `authenticate/verify` | `{id,signature,response}`; verifies both proofs and issues an account cookie |
| `device-challenge` | `{publicKey,purpose:"device-login"|"link"}`; returns a two-minute device challenge |
| `device-login` | `{id,signature}`; an already approved key obtains a fresh account cookie without moving or exporting a passkey |
| `link/start` | `{id,signature,label}` for purpose `link`; returns `{code,claim,publicKey,expires,url}`. The URL contains only the public approval code in its fragment |
| `link/inspect` | `{code}` with approving account cookie; returns `{code,publicKey,label,expires}` but never the private `claim` |
| `link/approve` | `{code,publicKey}` with approving account cookie; approves the exact inspected key |
| `link/status` | `{code,claim}`; requester polls for `{approved,publicKey,expires}` |
| `link/claim` | `{code,claim}`; requester consumes an approved, unexpired link and obtains its account cookie |
| `room-ticket` | `{alias,publicKey}` with account cookie; returns a one-use, 60-second `{ticket,alias,publicKey,expires}` for the same device and a granted alias |
| `devices` | `{}` with account cookie; returns that account's device keys and revocation state |
| `device/revoke` | `{publicKey}` with account cookie; revokes one of that account's keys |
| `logout` | `{}` with account cookie; ends only that account cookie |

`GET /v1/auth/session` returns the account's `{username,publicKey,name,expires}`.
Links, tickets and authentication challenges expire or consume once across
Worker restarts. Codes and claims use random 96-bit and 256-bit values
respectively; knowing a code cannot claim the requesting device's session.
The requesting key is bound by an Ed25519 proof before a link is created.
Grant changes, account removal and device revocation are checked again before
claiming or using a ticket. Global device ownership also checks pre-existing
room device records across all configured backends.

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
