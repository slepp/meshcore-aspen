# Pine BLE radio bridge

The planned peer mode connects two Pine radios over secured BLE. Each Pine keeps
its own LoRa frequency, bandwidth, spreading factor and transmit settings. A
forwarded packet crosses the BLE link after native routing, then the other radio
transmits those bytes without routing them a second time. The pair uses a new
shared repeater identity; existing repeater and bot identities remain stored.
Local adverts, administration replies and locally generated ACKs stay on their
receiving radio.

**The portable link is implemented; the BLE service and repeater wiring are not
yet available in a firmware profile.** No pairing or radio command enables this
mode today. A BLE UART service does not automatically create an operating-system
serial port: nRF52840 supports BLE GATT, not Bluetooth Classic SPP.

## Peer link version 1

The dedicated GATT adapter must authenticate the configured peer before starting
the link. Each connection uses a fresh nonzero random 32-bit session nonce and
confirms the same virtual repeater public key. The key in `HELLO` identifies the
role; it does not replace BLE authentication. Both sides offer two outstanding
transmit slots. No command changes the other radio's PHY.

Frames use KISS `C0` delimiters, `DB DC` for `C0`, and `DB DD` for `DB`.
The adapter copies a complete encoded frame or rejects it without sending any
bytes. It then fragments accepted frames to the negotiated ATT payload size in
order. Invalid escapes and overlong frames are discarded through the next
delimiter. Maximum native packet length is 255 bytes, decoded frame length
280 bytes and encoded length 562 bytes.

These are peer frames, not the shared modem's KISS control protocol. They reuse
the command numbers and state/reason values in `QueuedTxProtocol.h`; all
multi-byte integers are little-endian. Offsets include the command byte.

| Frame | Fields |
|---|---|
| `HELLO` (`20`, 39 bytes) | version at 1; sender nonce at 2; slots (2) at 6; virtual repeater public key at 7 |
| `SUBMIT` (`21`, 26–280 bytes) | version at 1; sender nonce at 2; receiver nonce at 6; job at 10; delay ms at 14; lifetime ms at 18; packet length at 22; origin flags at 24; native packet at 25 |
| `EVENT` (`FA`, 21 bytes) | version at 1; sender nonce at 2; receiver nonce at 6; original submit job at 10; state at 14; reason at 15; confirmed RF ms at 16; RF-known (0/1) at 20 |

Job IDs increase within a session; duplicate or older submissions never
retransmit. Origin bit 0 marks a packet-program emission and bit 1 marks a local
reflection. The device adapter must retain these flags and exclude the virtual
repeater from peer-egress reflections. Native routing, payload encryption and
regional restrictions belong to the radio integration, not this framing layer.

Lifetime is 1–60000 ms, starts at receiver admission, and must exceed the delay.
The radio adapter enforces expiry in its normal transmit queue. The sender waits
until its submitted lifetime plus five seconds for a terminal result. Handshake
timeout is five seconds. Timeout, changed session or disconnect closes the link;
unsent receiver work must be stopped. A packet already on air may still finish.

`ACCEPTED` means queue admission, not RF transmission. `SUCCEEDED` reports
confirmed RF completion. `FAILED` reports a known failure; `UNKNOWN` means the
radio cannot confirm the outcome. No accepted packet is replayed automatically
after disconnection. Terminal results retain their slots until consumed; accepted
and terminal events remain separately readable even when both arrive together.

## Portable checks

Run `make -C firmware/nrf52840 peer-link-test`. The test exercises one-byte
fragmentation, escaped maximum-size packets, two-slot pressure, admission
rejection, confirmed and uncertain results, duplicate suppression, session
changes, timer wraparound and malformed-frame recovery. The link's static RAM
budget is 1536 bytes; the later GATT adapter and native role need separate budgets.
