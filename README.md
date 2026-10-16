# MeshCore Aspen

Run a repeater, room, companion and programmable bot together through one LoRa
radio. Each role keeps its own identity, contacts and settings while sharing
one antenna, radio profile and airtime budget. Connect the MeshCore app,
exchange messages with other radios and add commands without reflashing.

Use **Aspen** for a standalone ESP32 node, or **Willow** to run the roles on a
Linux computer connected to a WiFi shared modem at the antenna.

[Install Aspen](https://ve6slp.ca/projects/meshcore/install/) ·
[Run Willow](hosts/willow/README.md) ·
[Download bot commands](https://ve6slp.ca/projects/meshcore/extend/library/) ·
[Operator guide](https://ve6slp.ca/projects/meshcore/operate/)

## Choose your hardware

| Hardware | Run it as | Start here |
| --- | --- | --- |
| Seeed XIAO ESP32-S3R8 + Wio SX1262 | Aspen: standalone repeater, room, companion and Lua/Wasm bot | [Browser setup and USB installation](https://ve6slp.ca/projects/meshcore/install/), or [offline USB setup](firmware/esp32/PUBLIC_SETUP.md) |
| Linux x86-64 computer + configured WiFi/TCP shared modem | Willow: host roles, Base companion, dashboard, MQTT and Lua/Wasm applications | [Willow installation](hosts/willow/README.md) |
| Original LilyGO T-Beam v1.2 **SX1262 or SX1276**, or Heltec WiFi LoRa 32 **V3** | Shared modem for Willow or another host | [T-Beam modem](firmware/esp32/TBEAM-V12-MODEM.md) · [Heltec V3 modem](firmware/esp32/HELTEC-V3-MODEM.md) |
| Heltec WiFi LoRa 32 **V4 R8**, or LilyGO T-Beam **Supreme SX1262** | Aspen with the board's own wiring and update target | [Board-specific setup](firmware/esp32/BOARDS.md) |
| Seeed XIAO nRF52840 + Wio SX1262 | Pine: compact repeater and separately addressed Lua bot | [Pine installation and updates](firmware/nrf52840/README.md) |
| WisBlock RAK4631 (RAK4630), with optional RAK13800 Ethernet and RAK19018 PoE | Pine: repeater, notes bot and Ethernet companion | [RAK setup](firmware/nrf52840/RAK4631.md) |

Match the exact board and radio variant. The XIAO firmware image is for the
XIAO; use the linked board profile for Heltec and LilyGO hardware. Full Aspen
uses PSRAM and two application slots. Heltec V3 has no PSRAM, and the original
T-Beam's 4 MB flash is better suited to the shared modem with roles on a host.

## Put Aspen on a new board

**First installation erases the board.** On an existing Aspen, use an
[application-only update](firmware/esp32/ESP_FIELD_UPDATES.md) to retain its
keys, settings, programs and data. Save an
[encrypted node backup](NODE_BACKUP.md) before maintenance, and keep the private
setup ZIP and operator seed outside public source and downloads.

1. Fit the antenna and open the [Aspen browser installer](https://ve6slp.ca/projects/meshcore/install/).
2. Download its firmware ZIP, choose your network's frequency, bandwidth,
   spreading factor, coding rate and permitted power, then enter your WiFi and
   administrator settings.
3. Save the private setup ZIP and first-install BIN. Select that BIN under
   **Custom Firmware** in the MeshCore flasher and flash over USB.
4. Open `http://aspen.local/admin`, or the DHCP address shown by your router.
   Sign in and check the radio profile and ready roles.
5. In the MeshCore app, choose **Connect → WiFi**, enter the node's address and
   port **5000**, and skip radio setup. Send an advert, then a private `!ping`
   to the command bot. Expect `Pong`; `!help` lists the available commands.

A separate companion radio on the same PHY can message the bot over RF.
Companion TCP, KISS and HTTP administration belong on a trusted LAN or
protected tunnel. Port **8001** is the shared KISS modem connection; port
**5000** is the companion app connection.

## Run the roles on a computer

Willow keeps role identities and durable state on the host while the shared
modem handles RF. Run a repeater, room, command bot and observer, then add a
Base companion, browser dashboard or MQTT broker as needed.

Follow the [Willow guide](hosts/willow/README.md) to install the host, create
private state, select the modem address and radio profile, check the settings
and start the services. Expect ready roles, a connected modem and a bot that
answers `!ping`. To retain an existing node, use the
[migration procedure](hosts/willow/docs/MIGRATION.md) and stop the previous
owner before starting the same identities on Willow.

[Birch](HOST_GUIDE.md#install-the-birch-host-download) provides the Go host
implementation with a prebuilt Debian 12 x86-64 download. An
[indoor ESP32 role](HOST_GUIDE.md#esp32-roles-with-a-remote-modem) can also use a
separate WiFi modem. Both use the same shared-radio protocol.

## Make the node useful

- **Connect and administer:** use the [operator guide](https://ve6slp.ca/projects/meshcore/operate/)
  for role names, status, app connections, backups and updates.
- **Add commands:** download [greeting, private notes, repeater signal and monitoring modules](https://ve6slp.ca/projects/meshcore/extend/library/).
  Installing a named module keeps bundled commands and other installed sources.
- **Automate the mesh:** [write Lua commands](firmware/runtime/BOT_DEVELOPMENT.md)
  or use the [C/Rust Wasm SDK](firmware/runtime/WASM_RUNTIME.md). Store private
  caller data, use timers and call owner-configured HTTPS services.
- **Watch your network:** collect [MQTT observations](firmware/esp32/OBSERVER.md),
  [monitor repeaters](firmware/runtime/REMOTE_REPEATERS.md) and view
  [Grafana dashboards](dashboards/grafana/README.md).
- **Share room history:** [Aspen Rooms](https://aspen.ve6slp.ca) connects browser
  and [terminal clients](clients/room-tui/README.md) with radio frontends through
  a shared room service.

All roles share one RF channel and its capacity. A dashboard TX completion
means the local radio sent a packet; a recipient ACK or reply confirms that
another radio received it. Local reflections are labelled separately from RF
reception.

Pine's download is an application update for an initialized node. Keep its
existing identities, credential records and Lua storage, and retain USB access
when performing a BLE update. The
[Pine guide](firmware/nrf52840/PRODUCTION-LUA.md) covers initial setup, limits
and recovery.

## Build and contribute

The [contributor guide](CONTRIBUTING.md) covers dependencies, source layout and
local checks. Use the [command reference](COMMANDS.md) for endpoints and
permissions, [architecture guide](ARCHITECTURE.md) for service ownership and
[release guide](release/README.md) for packaging.

| Source | What it contains |
| --- | --- |
| [ESP32 firmware](firmware/esp32/README.md) · [nRF52840 firmware](firmware/nrf52840/README.md) | Aspen and Pine board setup and builds |
| [Willow](hosts/willow/README.md) · [Go host](HOST_GUIDE.md) | Host roles, services and saved state |
| [Shared runtime](firmware/runtime/BOT_DEVELOPMENT.md) · [Hew Lua/Wasm packages](packages/README.md) | Command engines, examples and application APIs |
| [Command library](command-library/README.md) | Downloadable modules built from canonical examples |
| [Shared modem protocol](firmware/shared/QUEUED_TX_PROTOCOL.md) | Queueing, PHY control and radio integration |
| [Administration tool](tools/hardware/admin.py) | Authenticated management and named source operations |

## Licence

Original project code is [Apache-2.0](LICENSE). Upstream MeshCore and other
dependencies retain their own licences and notices.
