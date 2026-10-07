# Native MeshCore crypto for Workers

`src/native-crypto.wasm` is a bounded, import-free build of the native primitives.
The Worker owns each expanded 64-byte Ed25519 scalar/nonce key, derives native
ECDH secrets, encrypts AES-128 ECB packets with the two-byte HMAC-SHA256 prefix,
and signs room advertisements. It uses no WebCrypto key-format substitution.

- `ed25519/`: unchanged files from MeshCore native revision
  `d92964352441e53b93e8667b802e04f6e072b39e`, including its zlib-style license.
- `crypto/`: unchanged AES128/SHA256 and supporting files from rweather's
  Arduino Crypto **0.4.0**, as resolved by the pinned native build. Each file
  retains its MIT license header.
- `sources.sha256`: hashes of these vendored source files.
- `adapter.cpp`: fixed scratch arena and bounded native calls; no allocation,
  filesystem, network or JavaScript imports.
- `compat/`: the few libc declarations needed by those unchanged sources.

The checked-in binary is 62,613 bytes, SHA-256
`60b97ae1299e335876570c611eff9895304bee2a46160b28880378abfec464b0`.
Normal Worker builds use that binary and need no C toolchain. Rebuild with LLVM
22.1.8 (including wasm-ld):

```sh
WASM_CC=/path/to/llvm/bin/clang python3 services/shared-room/native/build.py
cd services/shared-room
npm test
npm run build
```

`native-crypto.ts` checks lengths, calls synchronously and clears the arena after
each operation. The focused test decrypts the committed native C++ ciphertext,
checks its ACK hash, rejects a modified MAC and checks native signing. Public
login/post/PATH/REQ and regional fixtures are generated independently with pinned
`meshcore-go v1.5.0` by `tools/native-fixtures.go`. These fixture keys must never
be used for deployed identities.

Public named regions use `SHA256("#" + name)[:16]` and the existing native
SHA256 HMAC over payload type plus payload. `mc_transport` follows pinned
[`TransportKey::calcTransportCode`](https://github.com/meshcore-dev/MeshCore/blob/d92964352441e53b93e8667b802e04f6e072b39e/src/helpers/TransportKeyStore.cpp),
including its little-endian 16-bit code and reserved-value remapping. The module
still has fixed 128KiB memory and no imports. No private region key is generated
or loaded by the public-name derivation.
