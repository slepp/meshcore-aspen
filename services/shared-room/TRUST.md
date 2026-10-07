# Room crypto and frontend authority

The implemented v1 API trusts radio frontends to verify native MeshCore crypto.
This differs from the original proposal where a Worker owns the room private key
and an opaque radio frontend only carries received/transmitted packet bytes.
The approved target remains **Worker-owned room keys and opaque radio frontends**.
The current decoded API is an intermediate trusted-frontend prototype, not a
change to that target. The deployed alias/frontend maps remain empty.

| Boundary | Current decoded-operation prototype | Original opaque frontend design |
| --- | --- | --- |
| Room expanded private key | Each frontend advertising that identity | Worker/native-compatible codec |
| Native RF MAC/decryption/signing | Frontend codec | Worker codec |
| Frontend receives | Plain durable operations/history and packet keys | Opaque RF bytes and dispatch instructions |
| Worker checks | Frontend bearer grant, alias membership/password, sequence/dedup/cursors | Those checks plus native packet authentication |
| Same room identity at several frontends | Securely provision the same expanded key/name to each | Frontends share the allowed alias, not its private key |

## What the current source enforces

`src/config.ts` restricts each token to configured aliases. Alias-to-backend
mapping and full public keys are server controlled, and an alias's first-use
public key is pinned in SQLite. Distinct aliases keep separate client
membership/password, pending proof, route and cursor namespaces even when they
share a backend's ordered history.

`src/room.ts` owns full-key membership and checks a room password for new
membership. An existing member can use native empty-password ACL login. The
service assumes that frontend-supplied `client`, timestamp, text, attempt and
proof came from authenticated radio activity. The `source:"client"` field is a
frontend assertion; it is not a cryptographic proof. The TypeScript adapter and
Go reference codec use authoritative member lookups to resolve prefixes, but
the HTTP/WebSocket API itself cannot prove that those codec checks occurred.

A compromised allowed token can list existing full member keys, forge decoded
posts/refreshes for them, select itself as their frontend and replay shared
history through its own pending delivery/ACK sequence. It can choose a proof
when preparing its own delivery. It can therefore read or manipulate content in
its allowed aliases' backends, including history originating in another alias
that intentionally shares that backend. Tokens cannot remap an alias or grant
access to an unrelated backend through an alias outside their configured list.

A frontend holding an expanded room key can additionally generate signed room
adverts and valid native packets as that room, and derive the per-client shared
secrets used by the native protocol. Revoke a compromised frontend token to
remove its API grant; a leaked room key also requires a new advertised identity
and coordinated frontend/client replacement. The service's pinned key rule
requires a new alias ID for that replacement. Token rotation alone cannot
remove a stolen key's RF authority.

Existing native identity storage is an NVS blob (`RoleIdentity.cpp`); the current
cloud work does not provision cloud identities there or claim hardware-backed
key protection. Private key duplication and secure handoff are additional
operator responsibilities in the frontend-crypto design.

## Decision and supported handoff

Frontend crypto deliberately reuses the existing native MeshCore primitives and
keeps the durable Worker small. It is **not necessary** to the architecture, nor
should it be treated as implicit approval of the original central-key proposal.
Preserving opaque frontends requires a pinned native-compatible Worker crypto
module and an opaque packet API; the current decoded API is insufficient for
that trust model. Do not advertise the current prototype as that implementation.
Approval to generate two new test identities/credentials does not authorize
duplicating room private keys onto frontends. New test keys are designated for
the Worker; keep their local preparation separate until that codec/key store is
implemented. Only a separate explicit choice of frontend crypto would change
that location.

WSS connection, bounded packet queues and physical radio receipts are independent
of this decision. They can be implemented before either key setup. Leave native
startup disabled and no room key provider installed while the boundary is open.

Credential approval does not make ordinary tool stdin a secret handoff channel.
For setup, use a user-run script reading a private secret-manager mount/file,
or an approved supported secret mechanism. Wrangler's `secret bulk` consumes
stdin/file; force `WRANGLER_LOG_SANITIZE=true`, keep debug logging disabled, and
never include values in tool arguments, stdout or model-visible logs. Native
expanded keys stay in the chosen private store and are never needed by the
current Worker; a central-key implementation would require a separately approved
supported upload to that Worker. A user-run handoff can configure only the
`aspen-shared-room` ALIASES/FRONTENDS secrets and preserve the existing Cloudflare
OAuth login without creating any account-wide API credentials.

`tools/create-test-setup.go` prepares only two fresh test identities, two room
passwords and two frontend tokens in a new private local directory. It never
reads existing radio identities, uploads values or provisions a frontend. The
separate Worker-key file stays local; `tools/configure.mjs` cannot upload it.
That operator-run handoff currently supports only the decoded prototype and
requires `--accept-frontend-crypto`. Do not run it for the approved opaque
frontend target. Extend the handoff for the reviewed central codec before
activating these identities; the current service stays locked.
