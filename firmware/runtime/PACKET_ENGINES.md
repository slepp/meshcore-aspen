# Packet engines

Attach a C++, Lua or portable Wasm packet engine to Aspen's shared modem.
Engines can read, replace, modify or drop packets, and stage
up to two transmissions per hook invocation. Register them on the radio
dispatch task; they run on that task, not the command worker.
Nothing is enabled by default.

Build a C++ packet module against
[`PacketPipeline.h`](../shared/PacketPipeline.h) and attach it with
[`NativePacketHost`](../esp32/NativePacketHost.h).
[`PacketLua`](PacketLua.h) and [`PacketWasm`](PacketWasm.h) implement that
same interface for scripts. Load the program before attaching its engine.
These are application-level registration APIs; the management CLI does not
yet install packet programs.

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

## Lua and Wasm programs

Lua and Wasm use the same packet operations. Each invocation meters every
guest instruction against the registered engine's fuel and time budgets.
Imports also consume fuel and have a separate limit of 64 calls. Packet data
and metadata are copied; guests receive no native pointers or private keys.

| Operation | Lua 5.5.1 | Wasm packet ABI v1 |
| --- | --- | --- |
| Inspect metadata and current length | `packet.info()` returns a table | `mp_get_info(out, capacity)` copies an 80-byte `mp_info` |
| Read bytes | `packet.read(offset, length)` returns a binary string | `mp_read(offset, out, length)` |
| Write within the current length | `packet.write(offset, bytes)` | `mp_write(offset, bytes, length)` |
| Replace the current buffer and length | `packet.replace(bytes)` | `mp_replace(bytes, length)` |
| Stage a complete raw transmission | `packet.emit(bytes, priority, delay_ms, expiry_ms)` | `mp_emit(bytes, length, priority, delay_ms, expiry_ms)` |
| Continue or drop | Return `packet.CONTINUE` or `packet.DROP` | Return `MP_CONTINUE` or `MP_DROP` |

Offsets are zero-based. A packet holds at most 255 bytes. Write, replace and
emit return the copied byte count. Lua emission defaults to priority 4,
delay 0 and expiry 0; Wasm callers supply those values explicitly.
Priority is 0..255, and delay/expiry are 0..1073741823 milliseconds.
The pipeline applies the same origin checks and native packet validation
to both runtimes.

Metadata contains the stage, current `length`/`capacity`, source and
destination slots, payload type, role public identity, generation, job,
RSSI in dBm and SNR in quarter-dB. Wasm `flags` uses bit 1 for local,
2 for authenticated, 4 for engine origin and 8 for reflection origin.
Lua exposes those flags and the booleans `local`, `authenticated`,
`engine_origin` and `reflection_origin`. Its `identity` is a 32-byte binary
string, matching Wasm's byte array.

A Lua program is 1..16384 bytes of source text and returns one process function:

```lua
return function()
  local info = packet.info()
  if info.stage == 0 and info.rssi < -120 then
    return packet.DROP
  end
  return packet.CONTINUE
end
```

`PacketLua::load()` accepts a heap limit of 8192..262144 bytes, default 65536.
The Lua heap includes parsed functions, globals, strings and temporary metadata
tables. The sandbox supplies `packet` and `assert`; it does not open Lua's
standard libraries. The loader meters consumed source characters as well as
initialization instructions. Packet imports require an active process call,
so top-level initialization cannot read a packet or stage emissions.

Portable Wasm programs use [`wasm/sdk/packet.h`](wasm/sdk/packet.h), importing
only `info`, `read`, `write`, `replace` and `emit` from
`meshcore_packet_v1`. Export `mp_init()->i32`, returning `MP_ABI_VERSION`,
and `mp_process()->i32`, returning the disposition. A module is at most
16384 bytes, with one fixed 64KiB linear memory and an 8KiB interpreter stack.
Start sections, implicit constructors, WASI, tables and guest threads are
unavailable. Use the portable compiler flags in the
[Wasm guide](WASM_RUNTIME.md#build-and-install-a-package) and export the packet
entry points instead of the command entry points. Command packages and packet
programs use separate import namespaces.

Both loaders default to 100000 initialization instructions and a 20000 us
loading/initialization time budget. Load and clear programs while their engine
is idle; keep the engine object alive for as long as it is registered.
Globals persist across successful calls until reload, clear or restart.
Packet edits and emissions are transactional; guest globals are not.
Reload a faulting program before re-enabling its slot when its globals may
have changed.

Wasm command and packet programs share one WAMR 2.4.1 runtime and its 1MiB
PSRAM pool. Loading, unloading and import registration are serialized;
packet execution does not acquire that loader lock. Each module has an
explicit execution context and its own meter. Lua packet and command programs
share allocator and compiler-budget helpers, but use separate Lua states.
Packet programs currently expose only packet operations; system configuration,
PHY changes and owned-identity packet construction are not SDK calls.

### Resource measurements

Read `PacketLua::stats()` for source/session sizes, live and peak Lua heap,
parser steps, instructions, native calls and load/invocation microseconds.
`PacketWasm::stats()` reports source/session/linear/stack sizes, the shared
pool reservation and pool high-water at load, instructions, native calls and
load/init/invocation microseconds. Pool high-water belongs to all WAMR users,
not one engine. These categories can overlap; do not sum them into a per-module
heap figure.

The local check prints a 10000-invocation host comparison for a metadata
read and three-byte packet edit. One Linux x86-64 run used 16504 bytes of
Wasm session storage, 65536 bytes of linear memory and an 8192-byte interpreter
stack, alongside the shared 1MiB runtime pool. Lua used 104 bytes of session
storage and peaked at 28242 bytes of its 65536-byte heap. Mean host times were
1.00 us for Wasm and 1.16 us for Lua in that fixture. This measures host execution;
ESP32 PSRAM access and radio-task timing need device measurements.

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
make -C firmware/esp32 packet-lua-test
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
`packet-lua-test` also runs the Wasm guests, checks command/packet runtime
coexistence, and exercises script mutation, drops, staged emissions, faults,
memory bounds, fuel, deadlines and mixed-engine rollback. It prints the host
resource comparison above.
