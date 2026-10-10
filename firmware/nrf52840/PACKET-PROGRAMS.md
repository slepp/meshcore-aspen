# Packet programs on Pine

Run bounded Lua packet programs on Pine's modem, or extend the native C++
packet hooks in a custom build. Select the packet role instead of the command
Lua worker or Room. The repeater and companion keep their identities and PHY;
command Lua source, notes and settings remain saved while that worker is paused.
No packet program runs by default.

From Pine's authenticated repeater administrator console:

```text
packet role on
reboot
packet api
```

`packet api` should report `role=1 live=1`, two Lua slots and a 4096-byte source
limit. Save a small program on the host:

```lua
return function()
  local info = packet.info()
  return packet.CONTINUE
end
```

Install through the [RF administration client](../../tools/hardware/RF_ADMIN.md)
with an existing authorized companion identity and private administrator password:

```sh
python3 tools/hardware/admin.py --gateway MODEM_HOST \
  --target PINE_RELAY_KEY --seed-file /private/companion.seed \
  --password-file /private/node-password --untagged --no-command-retry \
  packet install 0 inspect.lua
```

Then send `packet 0 budget 1 3000 800 0` and `packet 0 enable on`. Read
`packet 0 status`, `hash`, `stats`, `memory` and `timing`. Installation saves a
new slot disabled; replacement retains its budget and enable state. Use
`packet 0 rollback` to select the previous source, `remove` to clear the
selection, or `retry` to reconcile a storage/load failure.
Do not repeat an uncertain commit blindly: read status and hash first.

`packet role off` followed by `reboot` resumes the saved command Lua source.
Disable the current optional role before selecting Room or peer mode. Saved
role changes take effect on restart, not during a packet callback.

## Bounds and behavior

- Two slots, 4096 bytes of Lua text per source, 48-byte upload chunks, three
  retained source files per slot plus one upload file. Sources and checked
  selection journals live in QSPI and survive application-only updates.
- Each VM has a 12 KiB Lua allocation limit. The loader uses one 16 KiB task
  stack and up to 4096 bytes of source; replacement can retain both the active
  and candidate VM. Allocation must retain an 8 KiB physical-heap reserve.
  A replacement that cannot fit leaves the prior program selected.
- Per-hook budgets are at most 10000 fuel units and 2000 microseconds.
  Capabilities are `0`: no program-requested retuning or owned composition.
  Read, edit, replace, drop and bounded raw emission are available.
- Two pending emitted packets share the normal modem airtime spacing and
  physical transmit watchdog. Each hook stages at most two emissions.
  Unspecified expiry is 60 seconds; delayed packets can expire before sending.
  Firmware-update preparation cancels unsent emissions and drains an on-air
  transmission before handover.
- Production Pine reserves 28 native repeater packets, eight companion/bot
  packets and the existing two receive snapshots per radio port.
  Disabled packet mode allocates no VMs, loader stack or emission queue.

Stages are RF receive (`1`), per-role delivery (`2`), admission (`4`),
transmit (`8`), native relay (`32`), plaintext receive (`64`) and plaintext
compose (`128`). Pine does not locally reflect completed transmissions; a
reflection-stage budget has no calls on this modem. RSSI/SNR describe actual
RF reception. Relay hooks see bytes after native routing, including ACK
forwarding. Plaintext hooks run after successful native authentication or
before owned encryption/signing. Private keys remain in native code.

The shared [packet API reference](../runtime/PACKET_ENGINES.md) describes the
Lua imports, wire validation, transactional edits and original-received/final-
composed ACK hashes. Pine uses the same checked native bridges, with its smaller
budgets and without Wasm or an ESP32 PSRAM pool.
A fault discards that call's edits/emissions, disables the offending slot and
saves its disabled state. Later enabling is an explicit operator action.
Previously accepted transmissions are not undone by disabling a program.

For a compiled engine, implement `packet_engine::Engine` and register it on
the single `Service::pipeline` in `PinePacketPrograms.cpp`, in place of one or
both managed slots. Keep the two-engine registry, lifetime and dispatch-task
rules; do not attach a second host to the modem.

Run `make -C firmware/nrf52840 production-lua-radio-test` for physical-driver
simulation of raw/per-role/native hooks, fault rollback and peer egress.
The shared controller test is
`python3 -m unittest discover -s firmware/esp32/tests -p test_packet_programs.py`.
Application maintenance uses [host-verified updates](HOST-UPDATES.md).
