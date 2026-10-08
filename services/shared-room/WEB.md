# Join an Aspen room from the web

Open the room Worker's HTTPS address on your desktop or phone. Select a channel,
enter a display name and that room's password, and choose **Join room**.
The operator's `ALIASES` configuration supplies the channels and passwords.
Aliases pointing to the same backend show the same ordered messages; another
backend has its own conversation.

The interface uses IP only. Web posts are stored in the same SQLite history as
native radio posts, then delivered to connected browsers and eligible radio
members. A radio member's confirmed history cursor advances only on its native
ACK. Viewing a message on the web does not acknowledge it for a companion.
The web identity is separate from any companion or management identity.

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

Joined channels remain connected while you switch between them. Badges count
new messages arriving in another channel during this page session. Reopening
the site restores its selected channel and login cookies, then catches up from
the shared history. Earlier messages load in pages of 100. `HISTORY_LIMIT`
applies to both browser history and radio catch-up.

The browser stores its random device identity, name preference, drafts and
uncertain post IDs/text in local storage. Passwords are not written there.
Cookies grant access for up to 30 days. Leaving a room ends its browser session
but keeps unsent work; clearing site data loses the identity and unsent work.
Separate devices have separate identities. Display names are not unique or
verified user accounts.

## Browser API

These routes use room-scoped session cookies, not `FRONTENDS` bearer tokens.
Requests and responses are JSON; responses are not cached. Mutating requests
use `Content-Type: application/json` and an `Origin` matching the Worker.
Login and sockets require HTTPS, except for local development on localhost.
The browser API does not accept radio operations or expose room private keys.

| Route | Request / result |
| --- | --- |
| `GET /v1/web/rooms` | Public `{rooms:[{id,name,publicKey}],maxPostBytes:151}`; no login required |
| `POST /v1/web/rooms/{alias}/login` | `{identity:"<64 lowercase hex>",name,password}`; returns `{author,name,expires,maxPostBytes}` and sets an HttpOnly session cookie |
| `GET /v1/web/rooms/{alias}/session` | Returns the current browser author, name and expiry |
| `POST /v1/web/rooms/{alias}/logout` | Ends this session and closes its sockets |
| `GET /v1/web/rooms/{alias}/history` | Latest 100 visible messages, oldest first, and `{more,floor}` |
| `GET .../history?before={seq}` | Earlier page; `more` means more earlier history |
| `GET .../history?after={seq}` | Catch-up page; `more` means another forward page |
| `POST /v1/web/rooms/{alias}/posts` | `{id:"<UUID v4>",text}`; returns `{message,duplicate}` only after storage has synced |
| `GET /v1/web/rooms/{alias}/socket?since={seq}` | Same-origin WebSocket with `aspen-room.web.v1`; cookie authentication |

Posts deduplicate by backend, stable web author and UUID. Reusing a UUID for
different content returns 409. A duplicate's original alias, display name,
timestamp and sequence remain unchanged. History includes canonical `seq`,
`timestamp`, `originAlias`, `author`, `clientTimestamp`, `text`, and nullable
`webName`. Radio messages have no `webName`.

The socket starts with `{type:"ready",version:1,alias,publicKey,name}`, followed
by `{type:"message",message}` in sequence order, including the sender's posts.
It replays up to 100 messages after `since`. A further backlog produces
`{type:"catchup",cursor}`; load forward history, then send
`{op:"sync",since:<last sequence>}` to align the connection. Merge history and
socket events by `seq`, because a catch-up response can overlap live messages.
The DO uses hibernating sockets, checks session expiry/configuration after wake,
and retains radio delivery state independently.

Browser history cursors are read positions, not RF delivery proofs. The service
does not promise immediate radio reception, unique display names, attachments,
or a web identity shared with a companion.
