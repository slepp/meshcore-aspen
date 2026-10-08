# Room keys and private setup

The default `MODE="opaque"` API keeps expanded native room private keys in
`ROOM_KEYS` Worker secrets. Radio frontends hold only their restricted bearer
token, alias public keys/names and endpoint/CA settings. The Worker verifies
native ECDH/MAC ciphertext, resolves short prefixes against full membership,
signs advertisements, encodes history and derives its expected client ACKs.
[Native crypto source](native/README.md) explains the pinned implementation.

A frontend token can forward or replay native packets and request signed
advertisements for its permitted aliases. It cannot assert a client identity,
post plaintext, list members or choose an ACK proof through this API. A
compromised radio frontend can still drop, replay or misreport its radio work.
Native MeshCore has a short two-byte MAC and bare 32-bit ACKs; the service
preserves those protocol limits rather than claiming stronger authentication.
TLS protects the frontend connection. Revoke a lost frontend token; replace the
advertised identity if its Worker room key is compromised.

## Browser access

The same Worker serves Aspen Rooms over HTTPS. Browser login checks the
configured alias's room password and creates a room-scoped, HttpOnly, Secure,
SameSite=Strict session cookie valid for 30 days. Login, posts, logout and
WebSocket upgrades require the service's own Origin. No cross-origin browser
API is enabled, and credentials are never placed in socket URLs. Password
attempts are limited to ten per minute per connecting address and backend.

Anyone knowing a room password can read its visible history and post messages.
Alias names and public radio identities appear in the unauthenticated channel
list; passwords, keys, membership and history do not. Do not use a private
description as an alias name. Changing an alias's password, public key or backend
invalidates its web sessions when they are next used. Leaving a room removes
that browser session and closes its sockets. Radio ACL membership has its
existing separate lifetime; changing a password does not revoke native members.

The browser stores a random device secret in local storage and uses it to
derive a stable web author ID at login. This is not a companion's private key
and grants no radio membership or management permission. Display names are
labels, not verified accounts, and different users can choose the same name.
The author ID distinguishes them in room details and message tooltips.
Room private keys and radio frontend tokens remain outside the browser.

Local storage also retains display-name preferences, drafts and uncertain
post IDs/text. Clearing site data loses that browser identity and any unsent
work. Do not share a browser profile with someone who should not use its joined
rooms. Message history is not saved in local storage. See [WEB.md](WEB.md).

An optional frontend `region` is a public routing name, not a secret or client
authorization boundary. It validates scoped packets and determines outgoing
flood scope. Native direct packets have no transport codes and still use room
authentication. Unknown scopes never fall back to unscoped handling. Private
`$` regions need operator-provided 16-byte transport keys and are unsupported
by this public-name configuration. Existing test setup files omit `region`.

The legacy `MODE="decoded"` development path trusts frontend assertions and
exposes plaintext/member operations. Its adapters need room keys on the host.
Do not enable that mode for the central-key deployment or provision those
reference adapters with the new Worker keys.

## Operator-run handoff

Prepare new test identities without reading existing radio identity storage:

```sh
GOMODCACHE=/tmp/aspen-go-mod GOCACHE=/tmp/aspen-go-build \
  go run services/shared-room/tools/create-test-setup.go --directory PRIVATE_DIRECTORY
```

The generator creates exclusive 0700/0600 files for two new test rooms and two
restricted frontend credentials. It never uploads or advertises them. Its
`service-config.json` contains ALIASES/FRONTENDS; `worker-room-keys.json` holds
expanded keys targeted at the Worker. Preserve those private files. Do not
commit them, paste them into a conversation, or copy room keys to a frontend.

Validate and upload with an operator-run local process:

```sh
node services/shared-room/tools/configure.mjs \
  --file PRIVATE_DIRECTORY/service-config.json \
  --keys PRIVATE_DIRECTORY/worker-room-keys.json --validate

node services/shared-room/tools/configure.mjs \
  --file PRIVATE_DIRECTORY/service-config.json \
  --keys PRIVATE_DIRECTORY/worker-room-keys.json --account YOUR_ACCOUNT_ID
```

Validation checks private file ownership/permissions, grants, expanded-key
format and native public-key pairing without printing values. Upload reads
files locally and pipes ALIASES/FRONTENDS/ROOM_KEYS to Wrangler `secret bulk`.
Before upload it checks live binding names/types without printing values.
It stops if a private name is already a plaintext binding. It reuses the
operator's Cloudflare login, captures preflight output and sanitizes Wrangler's
error-only upload logging. This is an operator secret handoff, not ordinary tool stdin
or a new account-wide credential. Keep account IDs, endpoints and private
configuration in local ignored files or secret-manager mounts.

Deploy the opaque Worker code before uploading these secrets and enabling
frontends. Choose the Cloudflare account privately, then run the normal
Wrangler deployment from this package. Do not add empty `ALIASES`, `FRONTENDS`
or `ROOM_KEYS` entries to `vars`: an absent credential binding rejects requests
with 401, while a plaintext binding blocks creation of a same-name secret.

The checked-in configuration uses `keep_vars=true` to retain existing public
operator settings on future code deployments. Wrangler also
[preserves secrets across deployments](https://developers.cloudflare.com/workers/wrangler/configuration/#source-of-truth).
An older deployment with plaintext credential placeholders needs one code
deployment that omits just those placeholders and temporarily sets
`keep_vars=false` in its private deployment configuration. Preserve the other
bindings and existing DO namespace; do not delete secrets to resolve this
collision. The next normal code deployment uses the checked-in configuration.

The secret handoff updates the existing Worker configuration; another code
deployment is unnecessary after a successful upload. Supply only endpoint, CA, restricted
token and public metadata to `cloudRoomConfiguration()` in the private native
build. The default provider is disabled. No deployment, secret upload, flash
or RF advertisement is performed by a build or the validation command.
