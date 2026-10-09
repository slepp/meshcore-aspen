# Native packet engines

Build a native packet module against
[`PacketPipeline.h`](../shared/PacketPipeline.h) and attach it with
[`NativePacketHost`](../esp32/NativePacketHost.h) on Aspen's radio dispatch
task. The module can read, replace, modify or drop packets, and stage
up to two transmissions per hook invocation. Nothing is enabled by default.
This interface currently takes compiled C++ modules; it does not install
WASM or Lua packet programs.

The shared modem exposes these raw hooks:

| Stage | Packet and consequence |
| --- | --- |
| `Receive` | RF bytes before the split to on-device roles and the host modem; a replacement reaches both |
| `LocalDelivery` | A separate copy for each on-device role; `destination` identifies its source slot |
| `Admission` | A submitted host or on-device packet before it enters the TX queue |
| `Transmit` | A queued packet immediately before the shared modem estimates airtime and starts RF |
| `Reflection` | A local copy after confirmed RF completion; changing this copy does not change the packet already transmitted |

`local` distinguishes reflection from RF reception. `engineOrigin` identifies
generated packets and native replies derived from them. `reflectionOrigin`
also follows replies derived from a local reflection, even when the reply
itself is an outgoing packet with `local=false`. Native receive and relay
hooks use the packet's stored RSSI/SNR, including after a receive delay.
Raw ciphertext edits do not recompute encryption MACs, signatures or message
ACK hashes.

Aspen's on-device roles also expose these hooks:

| Stage | Packet and consequence |
| --- | --- |
| `Relay` | A complete wire packet after the role's routing edits, before scheduler admission; includes direct and multipart ACK forwarding |
| `PlainReceive` | Decrypted peer, anonymous, path-return or group data after a valid MAC, or advert application data after signature verification |
| `PlainCompose` | Owned-identity peer, anonymous, path-return or group data before encryption, or advert application data before signing |

`payloadType` identifies the native payload and `identity` contains the owning
role's 32-byte public key. Private keys and shared secrets remain in native code.
For group data, `authenticated` records a valid channel MAC, not a verified
sender identity.
Plaintext receive edits change local handling, not the encrypted or signed
packet that another role may forward. The receive length includes AES padding.
An empty advert application body still invokes its hook.

Message ACK hashes use the **original received plaintext** even if a receive
engine changes the message or its length. Extended retries preserve the original
attempt byte separately from the text hash. Outgoing
message and room-post ACKs use the **final composed plaintext** before encryption.
Changing raw ciphertext later cannot update that expected ACK.

A plaintext receive drop prevents local handling. A compose drop prevents packet
creation; its caller reports the normal packet-creation failure. A relay drop
prevents that forwarding operation without claiming a transmission occurred.
At native hooks, malformed wire encodings, invalid plaintext envelopes and
invalid emitted packets fault the engine before any effects enter the scheduler.

## Registration

Keep engines and their name strings alive for the entire host lifetime.
Create one `NativePacketHost` for the shared modem, supplying a monotonic
microsecond clock and a fault reporter. Its `begin()` reserves an internal
scheduler source and attaches its pipeline. It consumes no KISS client or
native-role socket/slot. Do not register or invoke engines from the network
task.

Implement `packet_engine::Engine::process(metadata, call)` and register:

```cpp
using namespace packet_engine;
const auto result = host.pipeline().attach(engine, {
    "my-engine", stageMask(Stage::Receive), 2000, 500});
if (result != Registration::Attached)
  Serial.printf("Packet engine registration failed: %s\n", registrationText(result));
```

Registration accepts two engines, in registration order. Names are unique
1..31 printable ASCII bytes without spaces. Each engine declares its stage
mask, a fuel budget of 1..100000 and a time budget of 1..20000 microseconds.
Register realistic small budgets for a radio fast path. The time budget is
per engine, per invocation, not a claim that the radio can tolerate a 20 ms
callback on every packet.

`Call::read`, `write`, `replace` and `emit` validate lengths and consume fuel.
Native loops must also call `consume()` and return when it reports failure.
These are **cooperative native limits**: arbitrary C++ cannot be preempted
by this interface. Do not block, access flash, sleep, perform network I/O or
call the physical radio from an engine.

## Mutation, faults and transmission

All engines see a candidate buffer. Their writes and emissions remain staged
until the invocation finishes and the entire emission batch fits the scheduler.
`Decision::Drop` stops later engines and drops the current packet; valid
staged emissions can replace it.

An execution error, bad return value, invalid memory range, fuel exhaustion,
deadline overrun, invalid native packet or recursive pipeline entry discards the invocation's edits
and emissions, disables the failing engine and calls the host fault reporter.
The original bytes at that hook continue. Later hooks retain edits already
committed by earlier hooks.

If the shared TX queue cannot accept the whole emission batch, no generated
packet is admitted and the original packet continues. The host records
`EmissionRejected`; the engines remain enabled because queue pressure is
temporary. Read the host's latest fault and counter with `status()` and
individual engine state/counters through `pipeline()`. Recovery requires an
explicit `enable(slot, true)` after correcting the cause.

Generated packets use the same priority/FIFO scheduler, expiry rules, carrier
checks and aggregate airtime cap as host and on-device traffic. Their dedicated
source has its own airtime credit; its default factor is 1. `emit()` copies a
complete raw packet with priority, delay and expiry. It does not send
immediately, bypass the airtime cap or promise RF completion.

Engines may inspect, modify or drop generated packets, but cannot emit from a
generated packet, local reflection or native reply derived from either.
Guard `emit()` with `engineOrigin`, `reflectionOrigin` and `local` to avoid
`EmissionOrigin` faults. Native bot jobs retain these flags across worker
dispatch; room posts and client synchronization retain them until their
deferred transmissions. This prevents local feedback from
creating another transmission. RF packets received from another radio remain
ordinary receive events; an engine must still avoid over-air forwarding loops.

A drop at admission reports queued-TX reason 11 with `REJECTED`; a drop before
RF starts reports reason 11 with `FAILED`. Neither means a transmission occurred.
Only `SUCCEEDED` produces reflection. `UNKNOWN` remains uncertain and is not
automatically replayed.

`stop()` detaches the pipeline and cancels packets queued by its dedicated
source. Native-role replies keep their normal completion and airtime accounting,
including replies carrying an engine-origin flag.
A packet already transmitting completes through the normal radio path;
stopping an engine cannot recall it.

## Local checks

```sh
python3 -m unittest discover -s firmware/esp32/tests -p test_packet_pipeline.py -v
make -C firmware/esp32 arbiter-test BUILD="$PWD/.tmp/onchip-packet-native"
make -C firmware/esp32 packet-bridge-test BUILD="$PWD/.tmp/onchip-packet-native"
make -C firmware/esp32 bot-packet-origin-test BUILD="$PWD/.tmp/onchip-packet-native"
```

The portable check covers registration, mutation/rollback, drop, budgets,
recursion, emission-origin guards and atomic emission rejection. The arbiter
check covers RF/host/native fanout, source-specific drops, transmission,
reflection and generated packets sharing the scheduler and airtime accounting.
The bridge check exercises native encryption/signing boundaries, original and
final plaintext ACKs, extended retries, relay drops, delayed signal metadata,
deferred room synchronization and invalid-edit rollback. The bot check covers
local command filtering and asynchronous reply composition/completion.
These commands do not change a connected radio.
