# Native reference tests

Run from the workspace root:

```sh
make native-test-deps
make -C test_support/parity test
make -C test_support/parity spike
make -C test_support/parity abi32
make -C test_support/parity policy-differential
```

`test` builds actual pinned upstream translation units, runs behavioural assertions
and writes `.tmp/parity-oracle/{events.jsonl,results.json,provenance.json,matrix.json}`.
Only a successful run produces `results.json`. `snapshot` explicitly refreshes
the checked-in `testdata/parity` reference corpus; ordinary tests do not change it.
The root `make test`, `firmware-check` and `parity-native` targets prepare native
test dependencies automatically. Run `make native-test-deps` first when invoking
the nested targets directly.

The clean detached reference is `.tmp/parity-upstream`, commit
`d92964352441e53b93e8667b802e04f6e072b39e`. `prepare` creates it from the existing
`.tmp/MeshCore` repository and rejects a dirty/different reference.
`UPSTREAM`, `CRYPTO` and `BUILD` can be overridden. The default `CRYPTO` path uses
**rweather/Crypto 0.4.0**. The root preparation fetches the pinned MeshCore
source and installs Crypto, CayenneLPP 1.6.1 and base64 1.4.0 into ignored
`.tmp` directories via PlatformIO on first use. Compiler-generated dependency
files and SHA-256 hashes record the compiled closure; the broader source
inventory is also hashed.
`LPP` selects the installed CayenneLPP `src` directory beside Crypto by default.
Its unchanged serialization sources also appear in the compiled dependency hashes.
Compiler/ABI flag changes invalidate existing objects through a build stamp.

Artifact paths are repository-relative; compiled closure keys use the
`meshcore/`, `crypto/`, `harness/` and `generated/` source namespaces.
Explicit source directories outside the repository use `meshcore/`, `crypto/`,
`cayennelpp/`, `dependencies/` and `generated/` labels rather than machine paths.
Standard compiler include paths remain part of the ABI record.
Generate SHA-256 lists from the repository root with relative input names, for
example `sha256sum go.mod go.sum internal/roles/*.go`; do not pass absolute
workspace paths. The PHY `provenance` target does this automatically. Consumers
resolve hash-list entries from the repository root, not their working directory.

Source, compiler and dependency hashes identify the inputs to each recorded
result. A changed implementation or harness needs a new run; moving an artifact
without changing its inputs does not change the captured source or binary hashes.

## What executes

The unchanged native Packet, Dispatcher, Mesh, Utils, Identity and static packet
manager execute with explicit clock/RNG/radio inputs. Real upstream ACL, region,
transport-key, identity-storage and base-chat sources are linked. ACL/region
storage uses a small in-memory File implementation with failure injection;
base-chat contact allocation executes with recording/no-op application callbacks.

Real Crypto AES/SHA/HMAC and firmware ed25519 sources are checked against FIPS
197, FIPS 180-4, RFC 4231 case 1 and RFC 8032 section 7.1 test 1. None of the
upstream `test/mocks` headers is included. Unused Crypto random-key-generation
entrypoints are linker-discarded; Mesh RNG bytes are supplied explicitly.

`kernels.py` extracts the exact native role delay, RadioLib SNR and repeater loop
functions/tables, and records fragment hashes. Only class qualifiers are changed; fixtures
provide preferences, radio and RNG. `RecordedRNG` observes requested exclusive
bounds and delegates to the actual upstream `mesh::RNG::nextInt`. These are labelled
`extracted-native-arithmetic`. This keeps hardware-coupled arithmetic usable
without copying formulas into Go.
The runtime-AF case also extracts the two exact CommonCLI assignment statements
and drives the real Dispatcher with AF0/1/9/99/99.25 to capture arithmetic
and budget behaviour.

Events are JSON lines keyed by `scenario` and `event`. They contain packet bytes,
queue priority and eligibility, RX holds, radio start/end, budget and state
observations. Consumers should compare their own outputs with the corresponding
native vectors. Integer fields and wire bytes are exact; printed scores have six
decimal places. The RNG input is a little-endian 32-bit word, not a language PRNG
seed. Synthetic packet identities and seeds in the corpus are public test data.
The ABI event also records the selected float `pow` return width. On this host,
base10/score1.85/airtime1000 returns -899, not the double-precision -900:
native float `pow` rounds before the expression subtracts double `1.0`.
Target-library floating-point equivalence still needs the target build/capture.

Ordinary flood and direct origin vectors cover widths 1/2/3. TRACE origins cover
their separate payload-path encoding at widths 1/2/4/8 and priority 5. A
three-byte ordinary path is not a three-byte TRACE route; callers must reject
that request instead of silently substituting another TRACE width.
BaseChatMesh ACK vectors execute encrypted plain/signed receives with extended
attempt tails; Python
`hashlib` independently checks the observed truncated SHA-256 tags against
sender keys for plain messages and recipient keys for signed messages. These
tags are hashes, not CRCs.
`ack_input` supplies complete synthetic identity seeds, public keys, shared secret
and encoded return path/bytes. Its nonce index is relative to the embedded ACK
payload, not the entire direct, multipart or encrypted PATH frame.

Room boundary vectors execute `StrHelper::strncpy` with the native application's
151-byte buffer argument (150 retained bytes, including a split UTF-8 boundary),
real encrypted response allocation at plaintext lengths 165/167/168/172, and
actual CayenneLPP voltage/temperature serialization.

`client-acl-vectors/acl_case` records capacity, ordered input clients, raw
permissions, explicit activities, admission input and resulting native slot
order. It includes unfiltered save/load, on-disk permission bytes and an
admin-only save result. Equal-activity inputs use descending public keys so
insertion order cannot be confused with lexical key order. `0x80` is nonzero
and survives unfiltered `ClientACL::save`, although it is not an administrator.
Guest admission uses a separate policy. Reloaded activity is zero because that
native field is transient. The all-admin case records the host's safe rejection
of the native overwrite.

## Boundaries and exceptions

The native baseline profile has 32 base-chat contacts and 20 ACL clients.
Board/application overrides select their own capacities.

`abi32` builds the same source as an actual ELF32 executable in
`.tmp/parity-oracle-32-ieee` and executes deadline and packet-queue rollover
assertions. It needs a 32-bit C/C++ toolchain and standard libraries. The
default `ABI32_FLAGS` selects multiarch asm headers and
`-msse2 -mfpmath=sse` for IEEE float evaluation rather than x87 excess
precision (an x87 run produced 35,999 ms startup credit at AF99,
versus 36,000 ms with SSE/64-bit float evaluation).
No upstream types or timer expressions are altered. `ABI32_FLAGS` is overridable.

The target runs the core suite plus 32-bit timer/queue wrap cases.
`abi32-{evidence,reference}.json` and `abi32-events.jsonl` preserve its separate
source/ABI provenance. A normal 64-bit run never supplies rollover evidence;
the matrix links the separate 32-bit result only while its source hashes match.
The added CayenneLPP case executes on host64 only: its unchanged non-Arduino
Polyline source fails ELF32 compilation at `std::max(size_t, unsigned long)`.
The 32-bit runner explicitly records that case as blocked and omits those
library objects; native core timer, packet and room-copy cases still execute
with unchanged 32-bit types. This does not imply a failure in the Arduino target
branch, which uses different library integration.

`spike` bounds each unchanged application/substrate translation-unit probe to
30 seconds and five diagnostics. BaseChatMesh and storage helpers compile;
actual MyMesh/CommonCLI reach RTClib's MCU `Wire.h` dependency, and DataStore
requires a concrete platform format implementation. Diagnostic artifacts and
source hashes are recorded.
`acceptance.json.native_application_execution` records the first compile
failures and the application interactions outside this native reference runner.

Native exceptions are retained visibly: one-byte count63 forwarding carries into
width bits, full all-admin ACL admission overwrites an administrator, and failed
TX occupancy is not charged. The host must follow the explicit safe contract,
including rejecting an all-admin overwrite. Room READ_ONLY posting and
durable-retention differences are separate application behaviours.

## Comparing results

`matrix.json` maps companion commands, role behaviours and wire formats to
the individual native test cases. `behaviours.json` lists those cases;
`acceptance.json` lists application and RF scenarios outside this runner.
Generate current local results with `make parity-native`.

`make -C test_support/parity policy-differential` compares Go policy against
the native vectors and writes `policy-differential.json`. The comparison
includes RNG bounds, loop thresholds and airtime-factor handling. The PHY
tests in `test_support/phy_parity` cover the KISS modem, multiplexer and
queued adapter. For live two-radio checks, follow the root README.
