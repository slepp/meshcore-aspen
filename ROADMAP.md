# Current capabilities and roadmap

Build a MeshCore node with several independently identified roles sharing one
LoRa radio. Choose a Go host, an ESP32 on-device setup, or a compact nRF52840
repeater/bot in the [setup chooser](README.md#choose-a-setup).
Other MeshCore radios exchange adverts, messages and routes with these roles
using ordinary MeshCore protocols.

## Available now

Choose an image from the
[firmware downloads](https://ve6slp.ca/projects/meshcore/#downloads).
Its manifest identifies the build and supported board. Select the features
and saved configuration for your setup.

| Area | Available behavior | Operator decision |
| --- | --- | --- |
| Native roles | ESP32 and Go repeater, room, companion and observer; independent identities/state; companion protocol 13 | Choose role placement and one shared PHY authority; avoid accidental duplicate repeaters |
| Shared transport | Multi-client KISS, queued final TX results, negotiated four-port MKISS and sender-excluding ESP32 local reflection | Count logical ports and physical connections separately; reflection is not RF reception |
| Lua | Lua 5.5.1 on ESP32, Go `native_lua` and nRF52840; cooperative jobs and caller-scoped authority | Use durable ESP32 beta/HTTPS source lifecycle; Pine has a compact two-job profile |
| Wasm | Optional WAMR 2.4.1 interpreter, C/Rust SDK and one Wasm package alongside Lua on ESP32/native host | Bot builds default to `ONCHIP_BOT_WASM=1`; select `0` to omit it |
| Commands | Typed exports, help, modules/events, diagnostics, notes, utilities, granted channel boards and personal reminders | Use `!help` for permitted commands; grants are explicit |
| Storage | Scoped KV/list/CAS/transactions, file-backed KV/scheduler records and no-rearm backup/restore | Preserve bot/scope/principal ownership; source rollback does not roll back data |
| Administration | ESP32 Management RF identity, trusted-LAN HTTP UI and private same-UID host owner socket | Provision separate mast/role credentials; import secrets over encrypted RF or Unix, not HTTP |
| Networking | Approved HTTPS aliases, JSON/RPC, native-host TLS, expected-hash package fetch and optional metrics | Configure destination, CA, hostname, token and grants |
| Recovery | Source readback, previous-generation rollback, bundled-source recovery and ESP32 WiFi recovery | Inspect unknown outcomes before repeating mutations; do not erase storage as a repair shortcut |
| Admission | Opt-in measured-load bot admission with caller fairness and fixed queue/airtime ceilings | Inspect `bot adaptive`; saved `on`/`off` applies after reboot |
| nRF | Repeater, production Lua or native-note bot, durable notes and optional secured BLE companion | Public Pine is update-only; keep USB recovery accessible for BLE updates |
| Development | `make bot-local` executes Lua source and JSON scenarios in the native runtime | Test locally before uploading; use a companion radio for RF and device recovery checks |

Ordinary bot command cooldown is **zero**. Airtime, queues, jobs and storage
still bound work. Configured network requests allow **two per caller and four
globally per minute**.

## Setup and maintenance

- **Aspen:** the public ESP32 image requires a private
  [offline USB setup record](firmware/esp32/PUBLIC_SETUP.md) before identities,
  RF, administration or OTA start. Private compiled-credential images use
  different provisioning.
- **Birch UART modem:** use a 3.3 V adapter, RX GPIO44/D7,
  TX GPIO43/D6, 115200 8N1. The
  [host guide](HOST_GUIDE.md#connect-the-public-birch-uart-modem) covers wired
  queued-protocol clients. For Go host roles, follow the WiFi modem build and
  host setup in that guide.
- **Pine:** the public update requires existing initialized identity and
  credential records. Application-only updates preserve InternalFS and QSPI
  Lua state. Refresh trusted UTC after restart
  and at least hourly before creating deadlines; see
  [production Lua](firmware/nrf52840/PRODUCTION-LUA.md).
- **Pine BLE updates:** an interrupted Legacy DFU transfer can require USB
  recovery. Keep the device physically accessible; see the
  [recovery procedure](firmware/nrf52840/BLE-FIELD-UPDATE.md#aborted-transfer-and-preservation-checks).
- **Network access:** companion TCP, KISS and dashboards belong on a trusted
  LAN. ESP32 web administration uses plaintext HTTP; credentials and private
  backups are visible to that network.
- **Shared radio:** all roles share one RF profile and capacity. Status
  distinguishes transmission and recipient acknowledgement. Inspect uncertain
  outcomes before submitting another operation. Configure frequency, bandwidth
  and power for your network and regional operating rules.

## Next directions

Potential additions:

- Wireless recovery from interrupted Pine BLE updates before inaccessible
  installations; phone-specific scan, pairing and DFU checks.
- More device-specific mixed RF/network/storage workloads, power-loss cases
  and long-term storage/flash-endurance measurements.
- Broader recurring/channel reminders and conversation-thread scopes.
  Current personal reminders use durable claims and do not replay uncertain
  sends.
- Companion-assisted bulk transfer with coordinated temporary radio settings.
  A single SX1262 can listen on only one setting at a time.
- Additional instances of the same on-device role if demand and measured
  resources justify them. ESP32 currently runs at most one of each built-in
  role; extra instances can run on a host.

Start application work with [Lua development](firmware/runtime/BOT_DEVELOPMENT.md),
the [runtime API](firmware/runtime/BOT_RUNTIME.md#lua-api-by-capability) or the
[Wasm SDK](firmware/runtime/WASM_RUNTIME.md). Wire contracts and validation
procedures live in their focused guides.
