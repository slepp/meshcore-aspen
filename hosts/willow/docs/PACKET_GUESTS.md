# Sample repeater signal on Willow

Willow's bot worker can run the same Receive packet guests as Aspen. Install the
repeater sampler in slot 0 and add the named Lua commands to your existing source
set. Run these commands from the source tree, using your host state's admin socket:

```sh
make -C hosts/willow build/repeater-snr.wasm
python3 tools/hardware/admin.py --willow-socket /absolute/state/admin.sock \
  packet install 0 hosts/willow/build/repeater-snr.wasm
python3 tools/hardware/admin.py --willow-socket /absolute/state/admin.sock \
  packet budget 0 1 100000 20000 9
python3 tools/hardware/admin.py --willow-socket /absolute/state/admin.sock \
  packet enable 0 on
python3 tools/hardware/admin.py --willow-socket /absolute/state/admin.sock \
  source install snr firmware/runtime/plugins/examples/snr.lua
```

The named source operation retains the main Lua source and other named modules.
It refuses a conflicting command namespace. Existing channel access rules still
control who can invoke `!snr`, `!rssi` and prefixes such as `!snr d1`.

The sampler counts measured RF reception with a repeater path. Direct packets,
local reflections and packets without signal measurements do not add samples.
Its command replies, dashboard signal table and `/metrics` use the same numeric
report. Metrics are named `meshcore_packet_<field>` with labels including
`device="willow_<first16 bot public key>"`, `slot`, `report` and `label`. The
existing collector and signal Grafana device selector can use these series.

Packet source bytes, selected generations, enable state and budgets remain after
a restart. Sample rings and reports start empty after a worker restart. A modem
reconnect retains the worker's current samples. The host removes the copied
report when its worker ends.

Use `packet api`, `packet 0 status`, `packet 0 hash`, `packet 0 stats`,
`packet 0 timing` and `packet 0 budget` through the same CLI's `command` verb
for readback. `packet export 0 FILE`, `packet rollback 0`, `packet remove 0`
and `packet enable 0 off` use the corresponding CLI packet operations. Rollback
selects the previous packet source; it does not replace command Lua or Wasm.

Willow supports Receive stage mask 1 and capability bits 1 (read system) and 8
(publish report), combined as 9. Guests process private RX copies; transformed
bytes and drop decisions do not change bot reception. Packet guests cannot
transmit or change the shared modem PHY. The worker queues at most eight copies,
runs one per turn and shares a quarter-time packet allowance with up to 100 ms
of burst credit. Each slot retains its instruction and 20 ms maximum execution
budget. Queue overflow discards sampler copies while bot reception continues.

Run the isolated installation, reception, command and restart check without a
radio:

```sh
make -C hosts/willow -j2 packet-workflow-test
```
