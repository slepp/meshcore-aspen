# Shared Aspen rooms

Run one Cloudflare Worker with one SQLite Durable Object per backend room.
Radio frontends connect to it over inbound hibernating WebSockets. The service
stores membership, ordered messages, deduplication, delivery state and confirmed
cursors; a frontend bridges authenticated radio operations to the API.

```sh
cd services/shared-room
npm ci
cp .dev.vars.example .dev.vars
npm run dev
```

The example configuration gives one physical frontend three room identities.
`A` advertises WelcomeYEG and `B` advertises WelcomeYYC; both share the `welcome`
backend. `C` has independent history. A second frontend can advertise the same
configured A/B identities. Set real public keys, names, room passwords and
frontend tokens in `.dev.vars` for local use. Store `ALIASES` and `FRONTENDS` as
Worker secrets for a deployment. Deploying requires an operator to choose the
Cloudflare account and service access policy.

| Layer | Configuration | Responsibility |
| --- | --- | --- |
| Physical frontend/radio | `FRONTENDS[id] = {token, aliases:[...]}` | One physical TX queue; sockets for its allowed identities |
| Advertised identity | `ALIASES[id] = {publicKey, name, password, backend}` | Its key, password, client membership, route, cursor and ACK namespace |
| Backend room | The configured `backend` name | One canonical message sequence and shared history |

An identity's expanded private key stays at its frontends. Frontends serving the
same alias use the same key and name. Distinct aliases use distinct keys, even
when their first RF prefix byte collides. The service pins each alias ID to its
public key on first use: use a new alias ID when replacing that key. Backend
mapping is server configuration. Changing a mapping selects a different history;
existing state is not migrated between Durable Objects.

## API

Every request carries `Authorization: Bearer <frontend token>`. Each credential
has an explicit list of permitted aliases. Tokens go in headers, including the
WebSocket upgrade, so use a WSS client that supports headers.

Connect each advertised identity to:

```text
GET /v1/aliases/{alias}/socket
```

The server sends `{"type":"ready","version":1,"alias":"A","publicKey":"...","name":"WelcomeYEG"}`.
Send JSON text frames with a caller-chosen correlation ID:

```json
{"id":"1","operation":{"op":"login","client":"<64 lowercase hex chars>","timestamp":123,"since":0,"password":"room password","attempt":"<64 hex chars>","route":"<frontend codec route>"}}
```

Replies are `{"type":"result","id":"1","result":{...}}` or
`{"type":"error","id":"1","error":"..."}`. A HTTP client can submit the
same operation directly to `POST /v1/aliases/{alias}/operations`, returning the
result or an HTTP error. Keep a socket connected to receive deliveries.
Both directions are limited to 4096 UTF-8 bytes. Use the `aspen-room.v1.json`
subprotocol; correlation IDs contain 1..32 ASCII letters, digits, `_` or `-`.
See [wire format and encoding measurements](WIRE.md).

| Operation | Fields beyond `op` | Result/use |
| --- | --- | --- |
| `login` | `client, timestamp, since, password, attempt, route` | Checks alias password; selects RF responder and refreshes history |
| `post` | `client, timestamp, text, source:"client", attempt` | Commits one logical post; `respond:true` permits success ACK |
| `refresh` | `client, timestamp, optional since, attempt, route` | New authenticated client activity selects a frontend and restarts pending delivery |
| `path` | `client, attempt, route`, optional `deliveryId, proof` | Stores a learned native route and atomically confirms a bundled PATH ACK |
| `prepare` | `client, deliveryId, proof` | Stores expected RF ACK proof; only `transmit:true` permits submission |
| `receipt` | `client, deliveryId, outcome` | Records `sent`, `failed` or `unknown`; keeps cursor unchanged |
| `ack` | `client, deliveryId, proof` | Confirms the pending message, advances cursor and pushes the next |
| `members` | optional hexadecimal `prefix` | Full-key candidates; owned routes/pending proofs for reconnect |

`client` is always the full 32-byte public key in lowercase hexadecimal.
`timestamp` and `since` are native unsigned 32-bit timestamps. `proof` is the
nonzero native 32-bit ACK value written as eight lowercase hex digits.
`route` is canonical standard base64 containing up to 255 opaque codec bytes,
describing how to send the response to this client; it must exclude keys and
passwords. Posts are 1..151 well-formed UTF-8 bytes with no NUL. `members` replies
fit the frame budget; split the prefix into its next hexadecimal digit when
asked for a longer prefix.

An `attempt` is SHA-256 of the authenticated RF request's payload and payload
type, excluding the mutable route/path. All frontends observing that request
submit the same value. Only the first valid observation gets `respond:true`.
Correlation IDs and local modem job IDs are independent of this RF attempt.
The API trusts configured frontends to verify native MACs/decryption and submit
the correct full client identity; possession of a frontend token is authority to
act for its allowed radio identities.

A delivery event contains:

```json
{"type":"delivery","alias":"B","client":"<full client key>","deliveryId":"<UUID>","route":"...","message":{"seq":1,"timestamp":1800000000,"originAlias":"A","author":"<full author key>","clientTimestamp":123,"text":"hello"}}
```

`seq` is backend ordering. Each committed post receives an increasing native
wire timestamp, including posts committed in the same second. The original
alias, full author key and client timestamp remain attached to shared content.
They describe provenance; every alias encodes the delivery with its own room
identity. A delivered signed room post must never be submitted as a new client
post. Only native original text posts enter the service, preventing relay loops.

## Delivery rules

The first authenticated frontend receiving a login/request wins that RF attempt.
Other copies are suppressed. A logical post is identified by origin alias, full
author key, client timestamp and exact text. Native retries with different RF
attempt bits therefore get an ACK for their own attempt while storing one post.
Success is returned after the durable commit.

There is one pending history message per **advertised identity + full client
key**. The frontend encodes it, commits its proof with `prepare`, then submits
it to the physical radio. `prepare` returns `transmit:false` if the same delivery
was already prepared. A TX receipt describes RF submission; only a matching
client ACK advances the confirmed cursor. A late receipt after an ACK is harmless.
Readers get the next message after confirming the current one.

After a failed or uncertain dispatch, the pending delivery stays paused.
There is no timed takeover or automatic RF retransmission. A new login or
authenticated refresh with an increasing native timestamp selects the frontend
hearing that request and replays from the confirmed cursor. Re-observing the
same RF request suppresses its response; the client needs a fresh request
timestamp to recover. Old delivery IDs cannot acknowledge a replacement.

A normal password login catches up from the service's confirmed cursor. A lower `since`
requests replay; a larger `since` cannot skip unconfirmed messages. A new member
starts at the beginning of available history. A native empty-password login for an existing alias member accepts zero/repeated
timestamps and preserves its cursor, timestamp and pending delivery. It selects
a new frontend only when no delivery is pending; this avoids an implicit RF
takeover. Use a fresh password login or authenticated refresh to recover an
uncertain pending delivery.

A client's own committed posts
are skipped for that same advertised identity. Across aliases they retain their
origin and are eligible for delivery.

WebSocket attachments retain frontend identity, alias and credential fingerprint
through hibernation. Durable storage retains routes, pending proofs and cursors
through disconnection. Reconnect restores member/proof state and pushes newly
available work, leaving already pending deliveries paused until client activity.
The Worker uses `acceptWebSocket` and has no keepalive intervals or periodic
alarms. Existing clients can use Reset Path/relogin to change radio routes;
automatic roaming and exactly-once display are outside this delivery contract.

`HISTORY_LIMIT=0` defaults to unlimited catch-up. A positive value limits the
visible catch-up window to that many latest posts. It does **not** delete stored
history, attempts or deduplication records. No automatic age-based removal is
enabled. Choose a removal policy before adding an irreversible pruning job.

## Frontend integration

[`examples/frontend.ts`](examples/frontend.ts) is a usable decoded-operation
adapter: inject a host WebSocket connector, native codec and radio submission
function. It opens one socket per configured identity, verifies advertised
key/name, restores membership and pending ACK bindings, and uses a single
round-robin physical TX queue. Call `advertNext()` once per global advertisement
slot, for example every 60 seconds plus jitter. Each successive slot advertises
one identity, spreading advertisements instead of flooding every room together.

The intended device frontend is [native Aspen directly over Wi-Fi/WSS](NATIVE.md).
Its portable parser is implemented; WSS connection/runtime wiring remains. The
Worker does not terminate raw MeshCore crypto. The native-compatible reference
[`internal/sharedroom`](../../internal/sharedroom) implements login/PATH responses,
original posts, REQ history, signed deliveries and bare ACK matching using the
repository's pinned `github.com/meshcore-go/meshcore-go v1.5.0` primitives and
[`internal/roles`](../../internal/roles): expanded 64-byte Ed25519 scalar/nonce
keys, ECDH, AES-128 ECB, truncated HMAC and `CalcAckHash`. WebCrypto Ed25519 alone
does not implement that wire contract. The Worker tests use decoded fixtures; two focused Go codec tests reuse the
committed native ciphertext and ACK fixtures. Live RF interoperability remains
to be exercised. The reference codec currently supports unscoped RF.

For Birch, connect raw receive handlers to the codec and route encoded responses
through `Link.SubmitWithReceipt` and its final TX result. Return `unknown` for
`ErrTXOutcomeUnknown`; queue acceptance alone is not confirmed transmission.
Keep one shared physical queue across all advertised aliases. Query authoritative
`members(prefix)` for relevant directed packets, try every colliding advertised
and client prefix, and accept exactly one authenticated decode. Match bare ACKs
against every owned pending proof across identities; reject ambiguous matches.
Only the elected frontend sends the RF response.

The current native Aspen Home/Lua HTTP admission is two calls per caller and
four globally per minute (`BotHttps.cpp`). It needs a different transport bridge
to use this continuously connected API. The backend was built against Aspen
`21e2bf63031d5b16dab79da8830e9f7c6d21a119`, whose recorded native MeshCore revision
is `d92964352441e53b93e8667b802e04f6e072b39e`.

## Checks

```sh
npm run typecheck
npm test
npm run build
```

Nine local workerd tests exercise duplicate reception, one durable logical post,
ordered ACK/cursor behavior, uncertain dispatch recovery, hibernation and
reconnect, authorization, distinct shared aliases, backend isolation, and a
physical frontend exposing A/B/C with rotating advertisements and queue recovery
when reconnect interrupts a prepare call, native PATH/ACL behavior and bounded
JSON/base64 replies. `go test ./internal/sharedroom` checks the native reference
codec; [WIRE.md](WIRE.md) has the small portable parser check. The build is a
Wrangler dry run; these commands do not deploy a service or transmit RF.

Cloudflare documents the [hibernation API](https://developers.cloudflare.com/durable-objects/best-practices/websockets/)
and [SQLite Durable Object pricing](https://developers.cloudflare.com/durable-objects/platform/pricing/).
Free supports SQLite DOs; outgoing WebSocket messages have no request charge.
