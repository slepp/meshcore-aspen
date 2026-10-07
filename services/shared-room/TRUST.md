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
It reuses the operator's Cloudflare login and forces log sanitization with
error-only logging. This is an operator secret handoff, not ordinary tool stdin
or a new account-wide credential. Keep account IDs, endpoints and private
configuration in local ignored files or secret-manager mounts.

Deploy the opaque Worker code before uploading these secrets and enabling
frontends. Choose the Cloudflare account privately, then run the normal
Wrangler deployment from this package. Supply only endpoint, CA, restricted
token and public metadata to `cloudRoomConfiguration()` in the private native
build. The default provider is disabled. No deployment, secret upload, flash
or RF advertisement is performed by a build or the validation command.
