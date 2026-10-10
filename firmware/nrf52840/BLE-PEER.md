# Pine BLE radio bridge

Peer mode connects two Pine radios over secured BLE. Each Pine keeps
its own LoRa frequency, bandwidth, spreading factor and transmit settings. A
forwarded packet crosses the BLE link after native routing, then the other radio
transmits those bytes without routing them a second time. The pair uses a new
shared repeater identity; existing repeater and bot identities remain stored.
Local adverts, administration replies and locally generated ACKs stay on their
receiving radio.

The production XIAO and Solar profiles include a BLE central/peripheral adapter
and native post-relay egress. Peer mode replaces the command Lua worker, Room,
packet-program role and phone companion while active; their saved state remains.
A BLE UART service does not create an operating-system serial port: nRF52840
supports BLE GATT, not Bluetooth Classic SPP.

## Connect a pair

Use two explicitly identified Pine radios with initialized physical identities
and protected [node backups](../../NODE_BACKUP.md). Keep their independent local
PHY settings: use the same PHY for antenna zones, or choose compatible radio
settings independently on each side for separate frequency zones.
No peer command retunes either radio.

1. On each radio, read `peer address` over USB while its existing companion BLE
   is running. Record the address type and twelve hex digits. The radios must
   be within BLE range; directional LoRa antennas do not extend BLE range.
2. In a private, ignored project directory, create **one additional virtual
   repeater identity**, shared by this pair:

   ```sh
   install -d -m 700 firmware/nrf52840/.build/peer-local
   python3 firmware/nrf52840/peer_setup.py generate \
     --output firmware/nrf52840/.build/peer-local/identity.json
   ```

   Keep this `0600` file and a protected six-digit PIN file out of source and
   release archives. Existing repeater, bot and Room identities are not replaced.
   The same virtual identity and PIN must be installed on both radios.
3. Select the first radio in its private operator inventory
   (`MESHCORE_OPERATOR_CONFIG`, `nrf_serial`; see [host updates](HOST-UPDATES.md)).
   Configure it as central, pinning the second radio's recorded address:

   ```sh
   python3 firmware/nrf52840/peer_setup.py configure \
     --port /dev/serial/by-id/FIRST_PINE \
     --identity firmware/nrf52840/.build/peer-local/identity.json \
     --pin-file firmware/nrf52840/.build/peer-local/pin \
     --role central --peer-address TYPE:SECOND_ADDRESS12
   ```

   Use the second radio's own inventory and USB port to run the same command
   with `--role peripheral --peer-address TYPE:FIRST_ADDRESS12`.
   Configuration is local USB only; neither private keys nor the PIN are printed
   or sent over LoRa. The helper currently selects inventoried XIAO devices.
   Solar uses the same local `peer key`/`peer configure` commands on its console.
4. If another optional role is selected, save it off and restart first.
   On both radios send `peer on`, then `reboot`. Read `peer status`: expect
   `saved=1 live=1 ready=1`, with one central and one peripheral. BLE traffic
   starts only after address pinning, encrypted MITM pairing and notification
   subscription. Each boot clears prior bonds and requires the saved PIN again.
5. Send an ordinary mesh message into one LoRa zone. Check reception on a
   companion radio in the other zone. `offered` counts submissions to BLE;
   `ok` counts confirmed RF on this receiver, not a remote human read receipt.
   Local RF completion alone does not show that another radio received it.

Send `peer off` over USB and restart to resume the physical repeater identity
and saved command Lua/companion operation. A running pair continues using its
current role until that restart. To change credentials, stop peer mode and
restart before configuring again. An existing different virtual identity is
not overwritten: restore its protected backup instead.
Pair mode has no BLE DFU service; use [host-verified USB updates](HOST-UPDATES.md).

## Device budgets

One BLE connection, two outstanding link jobs, two raw RF jobs, two encoded
frames and 512 receive FIFO bytes in the selected UART adapter are bounded.
The device object is capped at 8 KiB, excluding SDK FIFO/SoftDevice allocations;
the peer RF job object is capped at 640 bytes. Allocation retains an 8 KiB
physical heap reserve. No Lua VM, Wasm pool or second Room is started.

The radio pass reads at most 64 BLE bytes and writes at most one 20-byte
fragment. GATT discovery runs on the BLE callback task, not the radio task;
SDK writes can wait for controller credit. Pairing/subscription has a
ten-second deadline. A central tries its pinned address at most once per five
seconds. Changed connection handles and link timeouts end the old session;
accepted packets are not replayed across reconnects. Unsent RF work is cancelled
on disconnect; an already on-air packet may finish or report uncertainty.

## Peer link version 1

The exclusive Nordic UART GATT adapter authenticates the configured peer before starting
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
reflection. The device adapter rejects generated/reflected submissions and excludes the virtual
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

Run `make -C firmware/nrf52840 peer-link-test production-lua-radio-test`.
The device egress simulation connects two independently timed physical drivers,
checks that routed bytes are not routed twice, and exercises RF certainty and
disconnect cancellation. The portable link test exercises one-byte
fragmentation, escaped maximum-size packets, two-slot pressure, admission
rejection, confirmed and uncertain results, duplicate suppression, session
changes, timer wraparound and malformed-frame recovery. The link's static RAM
budget is 1536 bytes, within the separate device budget above.
