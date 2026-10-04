# Offline fixture interface

This page describes `build/roles`, the explicit-clock fixture process.
For the automatic TCP/KISS service, use
[the service guide](SERVICE.md). Both executables share the Hew role codecs
and state machines; the service can additionally use the native bot extension.

Start `build/roles serve ...` as described in the [README](README.md). Input
and output are line-oriented; all wire bytes are lowercase hexadecimal.
Each input command ends with an `END` output line. `READY PUBLIC_KEY` is emitted
once at startup. The interface is for private host fixtures, not a network API.

## Role events

| Command | Result |
| --- | --- |
| `packet NOW_MS LOCAL HEX` | Decode/process one MeshCore packet; `LOCAL` is `0` for RF reception or `1` for local reflection. Zero or more `TX DELAY_MS HEX` replies. |
| `rx NOW_MS KISS_HEX` | Feed an arbitrary fragment of port-0 KISS DATA into the native decoder, then process complete packets. |
| `tick NOW_MS` | Drive room push deadlines and bot deferred invocations; zero or more `TX` lines. |
| `acl PUBLIC_KEY_HEX PERMISSION` | Provision/update a synthetic contact's full permission byte; `true` or `false`. |
| `advert UNIX_SECONDS` | `WIRE HEX`, containing a signed flood advert. |
| `cursor PUBLIC_KEY_HEX` | Permission, replay stamp, confirmed sync cursor, activity, pending ACK, failures and path-known state. |
| `status` | Member/post/push/command/duplicate/rejection counts and stopped state. |
| `close` | Reject later work and discard deferred invocations; EOF ends the process. |

`NOW_MS` is explicit **fixture time**, including its Unix-seconds component.
Use increasing nonnegative values. Room plaintext timestamps are independent
32-bit replay stamps supplied by the test peer. Received packet timestamps do
not set the fixture clock. The room's unique post clock follows the Go role's
monotonically increasing seconds. Advance beyond six seconds after a post and
tick repeatedly to visit each room member.

`TX` means the role constructed a packet, **not transmission or delivery**.
The harness emits requested delays without sleeping. A caller must separately
submit these packets to the queued-modem adapter to test modem outcomes.

## Queued shared-modem adapter

| Command | Result |
| --- | --- |
| `submit PRIORITY DELAY_MS EXPIRY_MS PACKET_HEX` | `KISS HEX`, a queued-v1 SUBMIT request; empty hex means no admission. |
| `event BODY_HEX` | Apply a 23-byte TX event body; `job=N state=S` or `ignored`. |
| `disconnect` | Number of pending submissions whose final outcome is unknown. Further submissions are rejected; nothing is replayed. |

The fixture adapter uses port 0 and source generation 1. It increments job IDs
from 1 and permits 32 pending jobs. Hardware subcommand `0x21` contains:

```text
version:u8=1
generation:u32le
job_id:u32le
priority:u8
delay_ms:u32le
expiry_ms:u32le
packet:bytes
```

It is carried inside KISS command `0x06` with standard FEND/FESC escaping.
The TX-event body is version, generation, job, state, reason, queue-wait,
RF-airtime and estimated-airtime (23 bytes total). State values are:
`0` rejected, `1` accepted, `2` succeeded, `3` failed, `4` unknown.
Acceptance retains the pending job; a matching terminal event removes it.
Wrong-generation, malformed, duplicate-final, and unknown-job events are ignored.
Success confirms radio transmission, never remote receipt.

HELLO/config readback, real source-generation discovery, role subport
negotiation, socket/serial transport and PHY authority are deliberately absent.
Do not point this harness at a shared modem.

## Persistence

| Command | Result |
| --- | --- |
| `state PATH` | Lock and automatically restore/select a private state file, then commit each subsequent packet/ACL/tick transaction before output. |
| `save PATH` | Explicit snapshot for tests; no implicit lock. Returns `false` and stops the role on commit failure. |
| `load PATH` | Explicit restore for tests; validates identity, role, lengths, duplicate member keys and path encoding. |
| `configure UTF8_HEX` | Fixture-only role settings as hex-encoded `key=value` lines. Production settings belong in the private service file. |
| `signal QUARTER_DB` | Fixture-only SNR for subsequent TRACE forwarding; the service obtains this from measured RX_META. |
| `measurement HEX` | Nine bytes: little-endian float32 RSSI, float32 SNR, then origin (`0` measured RF, `1` reflection, `2` missing metadata). Only measured finite values populate RF management signal fields. |
| `profile HEX` | The fixture's verified 18-byte physical profile for RF getters; does not connect to a modem. |
| `telemetry FLAGS BATTERY NOISE RSSI TEMP_HEX RX TX ERRORS TX_MS RX_MS EVENTS` | Typed synthetic measurement snapshot. `TEMP_HEX` is four-byte little-endian binary32. Flags are documented in `meshproto.management`. |
| `confirmed PACKET_HEX` | Fixture-only successful TX observation for role counters. Actual service counters use matching terminal queued-TX outcomes. |

Use `state`, not raw `save`/`load`, when testing multiple processes.
Keep paths under a private directory. `HEW2` snapshots contain identities,
ACL/replay/cursors/paths/counters and notes, with volatile history. `HEW3`
retains that prefix and appends durable history:
`count:u8 | (author:32, timestamp:u32le, size:u8, text:size)*count`.
The count is bounded at 32; text is 1–151 bytes without NUL. Timestamps increase
strictly and cannot exceed the saved clock. Text remains bytes, including Go
`RawText`; it is never passed through a lossy UTF-8 decoder. Existing `HEW2`
files remain readable. Loading `HEW3` retains history on subsequent commits.
`HEW4` preserves nonzero Go member `Attempt` bytes. After the same notes prefix
it stores `durable_history:u8 | attempt:u8*member_count`, followed by the `HEW3`
history tail only when `durable_history` is 1. The flag must be 0 or 1; attempts
use member order. Those bytes survive future commits without being interpreted
as pending transmissions. A zero-attempt snapshot may use `HEW2`/`HEW3` again.
`HEW5` retains that HEW4 layout and appends supported RF preference overrides:
version byte `1`, four `length:u16le | UTF8:length` strings (name, guest
credential, administrator credential, owner text), nine u32le settings
(width, flags, multi-ACK, loop, flood/unscoped/advert hop limits, local/flood
advert seconds), and two little-endian binary32 retransmit factors.
Flags bit 0 enables repeat and bit 1 enables read-only guests.
The complete extension is validated before state mutation; malformed or
nonfinite records are rejected. HEW5 is written after a supported RF setting
changes and remains readable alongside older snapshots.
Under the per-state lock, `state` removes an owned private abandoned
`PATH.pending` staging file before loading the committed target. It never
promotes staging bytes. The `identity` command does the same under its identity
lock without overwriting an existing seed. Raw `save` has no lock and does not
perform this recovery. Unsafe staging objects fail closed.

Native-retention history, active sessions, pending delivery attempts,
packet/request dedup and deferred jobs are volatile. Durable-replay history
survives. A returning member logs in again; the confirmed
cursor, replay timestamp, permissions and learned route survive. A previously
admitted but unacknowledged post is not marked delivered after restart.

## TRACE forwarding

TRACE payload is `tag:u32le | auth:u32le | flags:u8 | hashes`.
The low two flag bits select hash widths 1, 2, 4 or 8, independently of
ordinary three-byte paths. The packet path-length byte is an SNR count,
0–64; each path byte is signed SNR in quarter-dB units, not a node hash.
At hop `count`, the relay compares `hashes[count*width:(count+1)*width]`
with its public-key prefix. It appends measured SNR and increments count,
leaving tag/auth/flags/hashes unchanged. Forward priority is 5 with the
configured direct airtime delay. Dedup includes the SNR count, so a return
hop can revisit the same relay. Local reflection, flood TRACE, malformed
hash paths, wrong next hops and completed/full paths are not retransmitted.
The retained companion Base originates TRACE and validates its completed
return; the relay does not invent an endpoint response.

## Wire-contract helpers

These commands call native Hew functions directly:

```text
wire PACKET_HEX
path ENCODED_PATH_BYTE
region KEY_HEX PAYLOAD_TYPE PAYLOAD_HEX
ack PLAINTEXT_HEX PROOF_PUBLIC_KEY_HEX
seal SECRET_HEX PLAINTEXT_HEX
open SECRET_HEX MAC_AND_CIPHERTEXT_HEX
kiss KISS_STREAM_FRAGMENT_HEX
```

Responses are `WIRE`, `HEX`, `FRAME`, `INVALID`, a boolean, or an integer.
`ack` hashes exactly the supplied plaintext. Protocol callers remove text
NUL/padding before calculating the ACK, and room delivery uses the recipient
public key. `open` returns an empty byte value on malformed length, MAC failure
or invalid key; callers must not interpret it as authenticated empty plaintext.
