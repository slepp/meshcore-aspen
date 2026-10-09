# Desktop RF identities and direct messages: deferred design

Today a browser or TUI can join shared rooms over IP using its own device key.
The Worker stores that key as the message author. It is **not** advertised as
a MeshCore companion, and the UI does not yet send or receive direct messages.
Keep using a companion radio/application for DMs.

The next design lets a desktop choose one or more home towers to reach the
RF mesh while keeping a full MeshCore identity on that desktop. It must not
copy a private key into an Aspen frontend or turn every room alias into that
user's companion. Device linking associates independently generated keys
with one account; it does not merge those identities.

## Identity and tower selection

A desktop's current Ed25519 message-author key is not automatically a native
MeshCore identity. The RF client will use a native-compatible signing/ECDH
implementation and validated MeshCore private-key expansion. Existing desktop
keys need an explicit migration decision: preserve them only if their signing
and shared-secret behavior passes native vectors; otherwise create a separate
RF identity while retaining message history and device state.

An account grants a particular **full device key** access to selected towers.
The operator limits tower credentials, RF profile, route/region, queue and
airtime budget. The UI shows a primary home tower and optional fallback towers;
room/channel selection does not silently change them. Revoking one device
ends its tower access without moving or deleting another device's identity.

The desktop signs adverts and prepares encrypted packets locally. Towers
carry bounded opaque packets and verified ingress metadata; they do not own
the desktop identity or decrypt its DMs. Account authentication authorizes
transport, not an arbitrary sender identity. Validate an outgoing packet's
claimed source/signature where the native format permits, and bind requests
to the approved key, operation and selected tower.

## Sending and the first ACK

Persist an outbox entry before sending: a local operation UUID, full sender
and recipient keys, exact native packet bytes, selected towers, attempt
budget and expiry. Towers return their native queue token and final TX
result. Keep saved, queued, transmitted, uncertain and recipient-ACK states
separate; a tower's HTTP success or SX1262 transmission is not delivery.

Use the primary tower first. An explicitly configured fallback policy can
use another tower after its bounded deadline; it must not immediately flood
all towers. Reuse the same native message identity for permitted retries,
with native ACK correlation and duplicate suppression, rather than creating
a new message per tower. Respect the native ACK token's size and collisions:
match it to an outstanding message's full keys/context and reject ambiguous
matches.

The **first valid recipient ACK wins** the local delivery race. Persist it
before cancelling later attempts. ACKs heard by multiple towers deduplicate
by message context; tower-generation changes cannot resurrect completed
work. Cancellation prevents work not yet physically admitted. It cannot
retract a packet already on air or promise that an uncertain admission never
transmitted. A reconnect/restart never replays such work automatically.
An explicit retry retains the original operation and shows that another RF
copy may result.

## Receiving and routes

All selected towers forward eligible RF receptions with tower, connection
generation, reception time, RSSI/SNR and local-reflection flag. The desktop
verifies sender adverts, decrypts locally, deduplicates by native message
context and sends native ACKs through the appropriate home route. A local
reflection is never reported as remote RF reception.

Persist verified contacts and routes per full RF key, separately from display
names and account/device links. Prefer the tower/path that actually received
the packet; expire stale paths and use a deliberate flood fallback. A room
frontend's backend-authoritative membership/routes are not a desktop DM
contact store.

Offline behavior needs an explicit choice before implementation: initially
fail/pause delivery when no approved desktop is online. Durable encrypted
store-and-forward would need its own bounded inbox, expiry, recipient grants
and acknowledgement contract; do not infer it from room history. The Worker
must not decrypt or fabricate a desktop recipient ACK.

## Implementation gates

| Stage | Required result before enabling |
| --- | --- |
| Native identity library | Signing, expanded-key/ECDH, encryption and advert/DM/ACK vectors agree with the pinned MeshCore implementation |
| Tower transport | Key-bound authorization, bounded opaque operations, native TX receipts, generation invalidation and explicit uncertainty |
| Desktop state | Retained identity/outbox/contacts, safe restart, no key copying through account linking |
| Single-tower DM | RF advert and encrypted DM/ACK exchange with a companion radio; wrong-key and tampered-packet rejection |
| Multiple towers | First valid ACK persistence, cancelled unsent work, duplicate/late ACKs, ACK collisions and uncertain TX across restart |
| UI/operator controls | Explicit tower selection, online/offline behavior, revocation, RF budgets and distinct transmission/delivery status |

Use the existing queued-v1 radio and native packet contracts; this plan adds
no new over-air DM protocol. The initial work is a desktop/native library
and a scoped tower API, not a change to room identities or room history.
See [native service hooks](../../firmware/runtime/NATIVE_SERVICES.md) for the
on-device boundary and [WEB.md](WEB.md) for the deployed room/device flow.
