# Shared Aspen rooms

Run one Cloudflare Worker with one SQLite Durable Object per backend room.
The Worker owns the room keys, canonical membership, ordered messages,
deduplication and ACK-confirmed cursors. Thin radio frontends connect with
inbound hibernating WebSockets and carry opaque MeshCore packets.

```sh
cd services/shared-room
npm ci
cp .dev.vars.example .dev.vars
npm run dev
```

The example uses **public local fixture keys**. Use it for simulated packets;
never advertise those identities on a real RF network. For a deployment,
configure private `ALIASES`, `FRONTENDS` and `ROOM_KEYS` secrets. Keep
`MODE="opaque"`, the default. [TRUST.md](TRUST.md) describes private setup.

| Configuration | Purpose |
| --- | --- |
| `ALIASES[id] = {backend, publicKey, name, password}` | Advertised room identity and its canonical backend |
| `ROOM_KEYS[id] = "<128 lowercase hex characters>"` | Expanded native 64-byte private key, held only in the Worker |
| `FRONTENDS[id] = {token, aliases:[...], region?:"ab"}` | Restricted frontend credential, aliases and optional public flood region |

Several frontends can serve the same public key/name without receiving its
private key. Distinct aliases can share one backend history; membership,
routes, pending delivery and cursors remain separate for each alias and full
client key. The service pins each alias's first-use public key in SQLite.
Replacing that identity requires a new alias ID. Changing its backend selects
a different history; this does not migrate the old room.

## Small opaque API

Every request uses `Authorization: Bearer <frontend token>`. Tokens go in
headers, including the WSS upgrade. Connect once per served alias:

```text
GET /v1/aliases/{alias}/socket
Sec-WebSocket-Protocol: aspen-room.v2.json
```

The server identifies the configured alias before any operations:

```json
{"type":"ready","version":2,"alias":"A","publicKey":"<64 hex characters>","name":"Room"}
```

Send JSON text frames with a correlation ID:

```json
{"id":"1","operation":{"op":"rf","packet":"<canonical base64 radio packet>"}}
```

| Operation | Fields | Purpose |
| --- | --- | --- |
| `rf` | `packet` | Forward received radio bytes; the Worker authenticates and interprets them |
| `txReceipt` | `dispatchId, outcome` | Report final `sent`, `failed` or `unknown` radio outcome |
| `advertise` | none | Explicitly request a Worker-signed native room advertisement |

Replies are `{"type":"result","id":"1","result":{"accepted":true}}` or
`{"type":"error","id":"1","error":"..."}`. `accepted:false` means the packet
was unrelated, invalid, ambiguous or belonged to another configured alias.
Decoded `login`, `post`, `members`, `prepare` and `ack` claims are rejected.

The Worker pushes opaque radio dispatches:

```json
{"type":"transmit","alias":"A","dispatchId":"<UUID>","packet":"<base64>","delayMs":300,"priority":0}
```

Submit exactly those bytes through the shared physical TX queue. Queue
acceptance is not `sent`: return the final transmission result. Send a receipt
with that dispatch ID. There is no plaintext history or ACK proof in the event.
The Worker commits a post before permitting its success ACK, and stores the
expected history ACK proof before dispatching that history packet.

HTTP callers can submit the same operation to
`POST /v1/aliases/{alias}/operations`. Its result includes an optional
`transmission` object for the immediate response. Keep a socket for pushed
history. [WIRE.md](WIRE.md) lists the bounds and native packet contract.

## Coherence and recovery

The first valid observation of an RF login/post/request selects its responder;
other frontends hearing that same payload do not transmit a second response.
Logical posts deduplicate independently of RF retry bits. Every new committed
post joins one durable backend sequence and is pushed to active readers.

One pending history delivery per alias/full client key prevents a later
max-timestamp cursor from skipping an earlier unconfirmed message. A matching
native client ACK advances the cursor and triggers the next delivery. RF
receipts never advance it. A late receipt after an ACK is harmless.

Login catches up from the authoritative confirmed cursor. A lower client
`since` can request replay; a higher value cannot skip unconfirmed history.
A new member starts at the beginning of the visible window. Native
empty-password ACL login for an existing member preserves its cursor and
pending delivery, including zero/repeated timestamps. A fresh password login
or authenticated REQ refresh with an increasing timestamp selects the frontend
hearing it and replaces uncertain pending work. The new frontend uses flood
until it learns its own PATH; another modem's route is never reused.

After a failed or unknown dispatch, there is no timed takeover or automatic
RF retransmission. The next fresh client request recovers. Reconnecting a
frontend also leaves pending dispatches paused. Attachments restore alias,
frontend and credential fingerprint through hibernation; SQLite restores the
membership, routes, delivery proofs and cursors. Credentials are rechecked.
There are no Worker keepalive timers, periodic alarms or proactive replicas.
Existing clients can use Reset Path/relogin; seamless roaming and exactly-once
client display are not promised.

When all configured aliases share one backend, history uses learned
direct routes. Bare ACKs have only a 32-bit proof; the Worker tries all four
native attempt-bit variants and reserves distinct active proofs atomically
across all frontends, since each modem can hear the others' bare ACKs.
If all four are occupied, that delivery waits for an ACK or fresh request.

When the service configures multiple independent backends, all frontends receive history by
flood, even after learning a direct route. Native clients reply with an
encrypted PATH carrying the ACK, authenticated to that room/client identity.
Bare ACKs cannot advance those deliveries. Identical simultaneous history in
different backends therefore works without a cross-room coordinator. This
costs flood airtime; login/post responses still use normal learned routes.
This also protects modems with separate alias grants that share the RF network.
After adding a second backend to the service, a fresh client login or
refresh replaces any old direct pending delivery with this PATH ACK mode.

## Public regional routing

Set a physical frontend's optional `region` to a public native region name,
such as `ab` or `#ab`. That frontend accepts transport flood/direct routes 0/3
only when their primary transport code matches the configured name. Outgoing
floods, including login replies, history and explicit advertisements, carry
that region's native code. A regional frontend rejects unscoped floods;
unknown or mismatched scoped packets return `accepted:false` with no fallback.
Without `region`, a frontend continues using unscoped flood/direct routes 1/2
and rejects scoped packets. The thin radio adapter carries the bytes unchanged.

Different frontends can serve the same room identity in different public
regions. Names are case-sensitive; `ab` and `AB` differ. An optional leading
`#` is normalized. There is no inherited `can`/`ab`/`edm` permission or region
tree in this service. Each frontend selects one public region explicitly.
Native clients need their outgoing scope configured to match, including their
flooded PATH ACKs; receiving a scoped advert does not automatically select it.
See Aspen's [region guide](../../firmware/shared/REGIONS.md).

Public region names derive publicly known transport keys and control flood
routing. They are not an authorization boundary. Native ordinary direct
route 2 remains accepted and replies with learned direct routes use route 2;
room/password authentication and native ACK rules still apply. Incoming
transport-direct route 3 is validated, and its replies follow native ordinary
direct behavior. The secondary transport code supplies no admission permission
and is zero on outgoing floods. Private `$` regions require an explicit
16-byte key and secure handoff; they are rejected by this configuration.

`HISTORY_LIMIT=0` defaults to unlimited catch-up. A positive value limits the
visible window to that many latest posts. It deletes no messages, attempts or
dedup records. Choose a removal policy before adding irreversible pruning.

## Frontends and checks

[Native Aspen](NATIVE.md) has a direct Wi-Fi/WSS transport and opaque driver.
Its private configuration provider defaults to disabled. The portable driver
uses bounded queues, validates ready metadata, and retains final receipts
before draining best-effort RF reception. Explicit operator requests produce
advertisements; connecting alone does not.

[`examples/opaque-frontend.ts`](examples/opaque-frontend.ts) is a small host or
Birch adapter. Inject a header-capable WebSocket and one shared bounded radio
submitter, such as Birch's `SubmitWithReceipt`. Return `unknown` for an uncertain
TX result and filter local reflections. Forward received bytes to all served
alias sockets; the Worker resolves colliding room/client prefixes against
canonical full-key membership. No frontend member lookup or crypto is needed.
The older TypeScript adapter and Go codec remain decoded-mode development
references; they require explicit `MODE="decoded"` and are not the deployed
central-key path.

```sh
npm run typecheck
npm test
npm run build
```

The focused checks cover two-front reception, one durable post, native login
and PATH replies, pushed encrypted history, cursor/receipt behavior,
hibernation/reconnect, catch-up, shared-alias and independent-backend ACK isolation and unauthorized or
forged decoded requests. The native crypto test uses the committed C++ packet
and ACK fixture; login/post/PATH/REQ and scoped fixtures come from pinned meshcore-go.
Regional checks cover both transport routes, matching scope on replies,
catch-up, hibernation and wrong-region/corrupted-code rejection.
`make -C firmware/shared/cloudroom test` checks the portable driver and parser.
The build is a Wrangler dry run; none of these commands deploy or transmit RF.
Live hardware/RF interoperability still needs an operator test.

Cloudflare's [hibernation API](https://developers.cloudflare.com/durable-objects/best-practices/websockets/)
avoids duration charges while idle and eligible to hibernate. Its
[pricing](https://developers.cloudflare.com/durable-objects/platform/pricing/)
is not per outgoing message/subscriber: outgoing WebSocket messages are free,
incoming messages use a 20:1 request ratio, and compute/storage also count.
SQLite DOs are available on Free. Paid starts at $5/account/month; a small room
with 1,000 posts and 5,000 refreshes per month can fit included allowances.
An existing Paid account with sufficient headroom can have $0 incremental
cost; replay volume, frontend count and other account use still matter.
