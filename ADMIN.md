# Administer a node

Use `tools/admin.py` to run management commands, install Lua/Wasm programs,
save backups and update Aspen firmware. Run it from the repository root:

```sh
python3 -m venv .tmp/admin-venv
. .tmp/admin-venv/bin/activate
python3 -m pip install -r firmware/esp32/requirements-sign.txt
python3 tools/admin.py --help
```

Choose the connection with options before the command:

| Connection | Options |
| --- | --- |
| Aspen web management | `--web http://NODE_IP --password-file /private/admin.password` |
| Willow | `--willow-socket /path/to/state/admin.sock` |
| Birch native bot | `--unix-socket /path/to/bot/native/admin.sock` |
| Encrypted RF through a KISS modem | `--gateway MODEM_IP --seed-file /private/admin.seed --target FULL_MANAGEMENT_PUBLIC_KEY` |

Password and private-key files must have permissions `0600`. Aspen's HTTP
management connection sends the password and session over the LAN; use it
on a trusted network. RF administration uses the full public key of the
destination role. See [RF administration](tools/hardware/RF_ADMIN.md) for setup.

## Commands and programs

```sh
python3 tools/admin.py --web http://NODE_IP --password-file /private/admin.password \
  command 'bot repeaters status'
python3 tools/admin.py --willow-socket /path/to/state/admin.sock \
  source list
python3 tools/admin.py --willow-socket /path/to/state/admin.sock \
  source install station-status station-status.lua
python3 tools/admin.py --willow-socket /path/to/state/admin.sock \
  source export station-status saved-station-status.lua
```

`source install NAME FILE` updates one named Lua file. `source replace FILE`
replaces the complete source set. For a complete Wasm program, select
`--runtime wasm` before `source replace`, `source download` or `source rollback`.

Use `package create`, `package inspect` and `package install` for command
packages. `packet install SLOT FILE` installs a per-packet program;
`packet export SLOT FILE` saves its current source. `data export` and
`data restore` transfer scoped notes, timers or reminders. Each group has
`--help` with its arguments.

## Backups and updates

```sh
python3 tools/admin.py --web http://NODE_IP --password-file /private/admin.password \
  backup download /private/node.mcb --volatile --key-file /private/operator.seed
python3 tools/admin.py backup inspect /private/node.mcb --key-file /private/operator.seed
python3 tools/admin.py --web http://NODE_IP --password-file /private/admin.password \
  firmware status
```

Follow [application updates](tools/hardware/ESP32_MAINTENANCE.md) to install
an image while keeping identities, settings and programs. The same `firmware`
group provides `resume` for an interrupted update and `force-restart` when a
normal restart cannot save state. Forced restart can lose recent unsaved data.
See [node backups](NODE_BACKUP.md) for encrypted downloads and extraction.

## Telemetry

Configure an HTTPS telemetry receiver through encrypted Management RF:

```sh
python3 tools/admin.py --gateway MODEM_IP --seed-file /private/admin.seed \
  --target FULL_MANAGEMENT_PUBLIC_KEY \
  telemetry configure --endpoint https://metrics.example/write \
  --ca-file receiver-ca.pem --token-file /private/receiver.token --interval 60
```

This replaces the node's telemetry endpoint and enables publishing after
the settings commit. Add `--disabled` to save the endpoint with publishing
off. Use `--address IPV4` when the receiver needs a specific address; the
hostname is still used for TLS verification. See [telemetry](firmware/esp32/TELEMETRY.md)
for metrics and receiver setup.
