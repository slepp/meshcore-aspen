# MeshCore Aspen

Run independent MeshCore roles through one LoRa radio: a repeater, room,
companion/base and command bot can keep separate identities while sharing
one antenna, radio profile and airtime budget. Use a Go or Hew host for the roles,
run them directly on an ESP32-S3, or choose a smaller nRF52840 repeater/bot.
Other MeshCore radios exchange adverts, encrypted messages and routes with
these roles over RF.

Aspen is both the project name and its lead standalone ESP32 firmware.
Birch runs the roles off-chip on a Go host connected over WiFi to the shared
modem. Willow implements that host arrangement in Hew. Pine is the nRF52840
variation of Aspen, with a smaller repeater/bot feature set. Cedar is a lab
companion used for RF checks, not a shipped setup.

The project and community documentation are public at
[ve6slp.ca/projects/meshcore](https://ve6slp.ca/projects/meshcore/).
Get the [published Aspen release](https://github.com/slepp/meshcore-aspen/releases/latest)
for the XIAO ESP32-S3R8 + Wio SX1262, or see
[other firmware downloads](https://ve6slp.ca/projects/meshcore/#downloads).
First-install procedures differ
from application-only updates that retain an existing node's identities and
settings. Flash the selected image to update a node's firmware.

## Choose a setup

| Setup | Supported hardware | Use it when | Next step |
| --- | --- | --- | --- |
| Aspen: standalone ESP32 | XIAO ESP32-S3R8 with PSRAM + Wio SX1262 | You want a self-contained repeater, room, companion and Lua/Wasm bot | [Set up a public Aspen image over USB](firmware/esp32/PUBLIC_SETUP.md), or [build an ESP32 image](firmware/esp32/README.md#choose-an-image) |
| Birch: WiFi modem + Go host | Seeed XIAO ESP32-S3 + Wio SX1262; Linux host for native Lua | You want host services, filesystem state, external automation or native Lua | [Build WiFi and start the host](#start-a-wifi-radio-and-go-host) |
| Willow: WiFi modem + Hew host | The same shared modem; Linux x86-64 host and pinned Hew compiler | You want to develop or trial the experimental Hew host, Base companion, MQTT broker and dashboard | [Willow source and setup guide](experiments/hew-roles/README.md) |
| Pine: standalone nRF | XIAO nRF52840 + Wio SX1262 | You want a constrained native repeater/bot, personal notes and optional BLE companion | [nRF setup and update prerequisites](firmware/nrf52840/README.md), [notes and BLE](firmware/nrf52840/STATE-BLE.md) |
| ESP32 role with a remote modem | XIAO ESP32-S3 indoors; ESP32 + SX1262 at the radio site | You want a repeater, room or companion on an indoor board, connected over WiFi | [Remote-radio roles](HOST_GUIDE.md#esp32-roles-with-a-remote-modem) |

The nRF `nrfmast_fleet_lua` image runs the compact production Lua bot and
repeater; native-note images remain available. The default `nrfmast_rx` build
is receive-only; select the guide's RF-capable image to transmit.
**Aspen uses offline USB setup before its roles start.** The **Birch UART
modem** connects queued-protocol clients on RX GPIO44/D7 and TX GPIO43/D6
at 115200 8N1 with a 3.3 V adapter. The Go host connects to the
WiFi modem built below. **The Pine download updates an initialized node**:
it requires initialized identities and credential records and preserves its
QSPI Lua state. Keep USB recovery accessible if using Pine BLE updates:
an interrupted transfer can require USB. Follow each bundle's installation
guide. See [Birch UART wiring](HOST_GUIDE.md#connect-the-public-birch-uart-modem).
ESP32 network calls need the HTTPS profile and approved service configuration.
The ESP32 and native host bot can compile Wasm alongside Lua; see the
[Wasm runtime and build flags](firmware/runtime/WASM_RUNTIME.md).
For autonomous fleet status and battery voltage through Aspen's existing
metrics receiver, [install a remote-repeater Lua monitor](firmware/runtime/REMOTE_REPEATERS.md).
For MQTT packet collection, choose the host or on-device
[observer output contract](firmware/esp32/OBSERVER.md).
Additional role instances can run on a host, but each enabled built-in ESP32
role runs at most once. A second repeater can duplicate forwarding and consume
extra airtime; choose placement deliberately.

The ESP32 roles share firmware and runtime code, but the documented combined
image requires PSRAM. A port to another board, including Heltec V3 or V4, must
select that board's radio wiring, TCXO and memory layout, then enable roles
within its resources. These boards do not yet have a qualified project image.
Willow sources are included in `experiments/hew-roles`. Its event-driven
runtime is experimental; qualify your selected services before replacing an
existing installation. Aspen, Birch and Pine retain their own setup guides.

### Where the software and radio run

| Arrangement | Runs the roles | Connects to the RF mesh |
| --- | --- | --- |
| Go host | Indoor Linux computer, with several selected roles | ESP32 + SX1262 shared modem over LAN/WiFi TCP |
| Willow host | Linux x86-64 computer running Hew services and a native Lua/Wasm worker | ESP32 + SX1262 shared modem over LAN/WiFi TCP |
| Remote ESP32 role | Indoor ESP32 running a selected repeater, room or companion | Separate ESP32 + SX1262 shared modem over WiFi TCP |
| Aspen | ESP32 runs the selected roles and bot | Its local SX1262 |
| Pine | nRF52840 runs the repeater and Lua bot | Its local SX1262 |

Aspen and Birch use independent product versions. The current source reports
`aspen-0.1.2` or `birch-0.1.0-rc.1`, separately from the MeshCore base.
Show releases as **Aspen 0.1.2 · based on MeshCore 1.17.1** and tag them
`aspen-v0.1.2` or `birch-v0.1.0-rc.1`. Birch releases contain one matching
host, native worker and modem bundle. Pine retains `1.17.1-slp-pine`.
The [release guide](release/README.md) lists compatibility contracts, candidate
build commands and publication steps. Existing website downloads and the
older GitHub draft keep their original identities and source revisions.
Choose enabled roles and features in the build and saved configuration.

To retain a node's identities, settings, programs and saved data, use the
[encrypted node backup workflow](NODE_BACKUP.md). Aspen and Birch can download
over WiFi; Aspen, Birch and Pine also support paced authenticated RF retrieval.
Keep the matching operator seed to inspect or extract an encrypted archive.

## Start a WiFi radio and Go host

With Git, Python 3, PlatformIO and Go installed, run from the repository root:

```sh
make firmware-config
$EDITOR firmware/platformio.local.ini
make firmware
make firmware-upload UPLOAD_PORT='/dev/serial/by-id/<radio-device>'
make host-config
$EDITOR meshcore-host.json
make host-run
```

Set WiFi and the hostname in the private firmware configuration. In the host
configuration, set `radio_address`, enable the roles you need, choose a legal
shared LoRa profile and supply the room password through `MESHCORE_ROOM_PASSWORD`
before starting an enabled room. Use `room_public: true` for a deliberately
password-free room. Match that profile
on the companion radio you will use for RF. If the modem is an administrable
standalone mast, select `"phy_authority": "modem"` rather than trying to retune
it from the host.

Expected: the modem serves KISS at `RADIO_IP:8001` and a read-only dashboard
at `http://RADIO_IP/`. The host serves role status at
`http://127.0.0.1:9080/status`; `/readyz` returns HTTP 200 when its enabled
applications and connections are ready. If mDNS is unavailable, use the
modem's DHCP address. Choose the upload device explicitly, especially when
several radios are attached.

See the [Go host and radio guide](HOST_GUIDE.md) for capacity, configuration,
service installation, backup and rollback. To run the Lua bot on the host,
follow [native Lua setup](HOST_GUIDE.md#host-status). The KISS bot proxy
provides a connection for an external bot application.

## Use a companion or bot

- **Companion application:** connect to the selected Base's companion TCP
  endpoint, normally `127.0.0.1:5000` on the host or `MAST_IP:5000` on a radio.
  For a LAN connection to the host, set `companion_listen` to its LAN address
  and set `companion_allow_remote: true`.
  Applications on this endpoint share one Base identity and contacts.
  For Android, choose **Connect → WiFi** and enable **Auto Reconnect**;
  see [connection settings](firmware/shared/ANDROID.md).
- **Command bot over RF:** let the sending companion advertise so the bot
  learns its contact, then send the bot a private `!ping` DM. Expect `Pong`.
  `!help` lists commands permitted for that request; `!help NAME` gives usage.
  Personal notes use `!remember`, `!recall` and `!notes`. Reminders and
  network commands require separate owner grants; see
  [bot usage and API](firmware/runtime/BOT_RUNTIME.md).
- **External bot:** use a dedicated companion endpoint or KISS connection
  according to the bot's protocol. Python companion bots use the companion
  endpoint. See [external bot setup](HOST_GUIDE.md#bot-connection).

Keep companion TCP, KISS and the dashboard on a trusted LAN or protected tunnel.

## Who owns what?

The MeshCore native roles and over-air protocols are upstream behavior.
This project adds concurrent independent roles sharing one PHY, app runtimes,
administration, scoped storage and configured networking.

| Service | Identity and responsibility |
| --- | --- |
| Shared modem / KISS | Physical tuning, carrier access, queue and aggregate airtime; each role owns its separate crypto identity |
| Repeater / Relay | Its own key and preferences; forwards eligible RF traffic |
| Room | Its own key, login policy, members and bounded history |
| Companion / Base | Its own key, contacts, channels and messages; connected applications share that identity |
| Lua command bot | Its own key, selected source and scoped data; independent of Base and external bots |
| Management | ESP32 mast-wide administration and shared PHY authority; native role ACLs do not grant this authority |
| Observer | Publishes received RF packets to the configured MQTT broker |

All roles share **one RF channel and capacity**. Local reflections carry
completed local transmissions to colocated roles and are labelled separately
from measured RF receptions. Status distinguishes queue admission, confirmed
transmission and recipient acknowledgement. After an uncertain operation,
inspect its outcome before submitting another request.

Role identities and durable state survive ordinary firmware/host updates.
Contacts/routes cached only in the Lua bot's RAM must be learned again after
restart. Source replacement cancels suspended Lua jobs but retains scoped
data; source rollback does not roll back data. See
[ESP32 persistence and recovery](firmware/esp32/README.md#what-survives-a-restart)
and [host state](HOST_GUIDE.md#state-compatibility-and-credentials).

## Administer a node

Start with [ESP32 provisioning and image selection](firmware/esp32/README.md),
then [mast administration](firmware/esp32/MAST_ADMIN.md) for roles, names,
keys, grants, source lifecycle and scoped backups. Use encrypted authenticated
RF, the host bot's private owner Unix socket, or the ESP32
[trusted-LAN web page](firmware/esp32/MAST_WEB_UI.md).
The web page is **plaintext HTTP**: credentials and private backups are visible
to the network. Secret imports use encrypted RF or the protected Unix socket,
not HTTP. Configure [approved HTTPS services](firmware/runtime/NETWORK_API.md)
and [optional metrics](firmware/esp32/TELEMETRY.md) only when needed.
Use the [Grafana fleet dashboard](dashboards/grafana/README.md) to view
existing native and companion-bot metrics in VictoriaMetrics.
For an individual node, use `stats help` and the read-only
[CLI stats API](firmware/shared/STATS.md) for board measurements, radio counters,
memory and running-service diagnostics.

**Before erasing or restoring:** back up identity/state and source. SPIFFS
provisioning replaces role files and bot data; NVS erasure can destroy identities.
Scheduler restores use explicit **no-rearm** and do not replay consumed or
uncertain sends. Read the relevant recovery procedure before acting.

## Develop an application

For a clean Linux checkout, follow [contributor setup](CONTRIBUTING.md), then
run `make check` to build the host tools and run offline Python, Go race and
native smoke tests without opening a radio. Firmware and Lua/Wasm validation
have additional dependencies described in their focused guides.

Test a Lua command in the native runtime without flashing or transmitting:

```sh
make -s -C firmware/esp32 bot-local \
  SOURCE=firmware/runtime/plugins/template.lua \
  SCENARIO=firmware/runtime/plugins/examples/hello.scenario.json
```

The scenario checks the replies and exits nonzero if an assertion fails.
Follow the [Lua package and local replay workflow](firmware/runtime/BOT_DEVELOPMENT.md)
to test your own source, permissions, state and restart behavior. Use the
[capability-organized Lua reference](firmware/runtime/BOT_RUNTIME.md#lua-api-by-capability)
for signatures, results, authorization and asynchronous limits.

The optional [Wasm runtime and C/Rust SDK](firmware/runtime/WASM_RUNTIME.md)
run one Wasm package alongside Lua on ESP32 and the native host worker.
Bot builds default to `ONCHIP_BOT_WASM=1`; choose `ONCHIP_BOT_WASM=0` to omit
the interpreter. The guide covers package installation, shared permissions,
runtime-specific recovery and field validation procedures.

### Host status

The [native-host setup and status reference](HOST_GUIDE.md#host-status) explains
`native_lua`, worker installation, the owner socket and `/status` fields.

For implementation work, use the
[queued TX contract](firmware/shared/QUEUED_TX_PROTOCOL.md),
[remote radio adapter](firmware/shared/RemoteKissRadio.md),
[mast wire contract](firmware/esp32/MAST_BETA_PROTOCOL_V1.md) and
[native host-worker contract](internal/nativebot/PROTOCOL.md).
The [roadmap](ROADMAP.md) separates current code from planned capabilities.

### Find source and tools

Run the root `Makefile` targets from the repository root. For a component's
own targets, use `make -C DIRECTORY` as shown in its guide.

| Entry point | Use it for |
| --- | --- |
| [`firmware/esp32`](firmware/esp32/README.md), [`firmware/nrf52840`](firmware/nrf52840/README.md) | Aspen and Pine board integration, preparation and builds |
| [`firmware/runtime`](firmware/runtime/BOT_DEVELOPMENT.md) | Shared bot engine, Lua examples in `examples/`, package examples in `plugins/`, and Wasm SDK |
| [`firmware/shared`](firmware/shared/QUEUED_TX_PROTOCOL.md), [`firmware/remote-radio/esp32`](firmware/shared/RemoteKissRadio.md) | Shared modem contracts and the ESP32 remote-radio adapter |
| [`cmd`](cmd), [`internal`](internal) | Host commands and libraries; Python chat in [`cmd/meshcore-chat`](cmd/meshcore-chat/README.md) |
| [Willow: `experiments/hew-roles`](experiments/hew-roles/README.md) | Experimental Hew host services, migration and native runtime contracts |
| [`tools/hardware/admin.py`](tools/hardware/admin.py) | Node administration and Lua/Wasm package operations |
| [Directed MeshCore link](tools/meshcore_link/GUIDE.md) | Persistent Base-feed DM send, inbox and session notifications |
| [`meshcore_kiss_monitor.py`](meshcore_kiss_monitor.py) | Standalone serial and TCP packet monitor |

Configuration files such as `firmware/platformio.local.ini` and
`firmware/platformio.onchip.ini` are generated by `make firmware-config` and
`make aspen-config`; edit those private files, not the tracked examples.
The `aspen-*`, `remote-radio-*` and `pine-*` targets select the current setups.
Existing `onchip-*`, `phyless-*` and `nrf52-*` commands remain available;
`nrf52-*` builds the upstream companion, not the Pine on-device role.
See [contributor layout and offline checks](CONTRIBUTING.md#source-layout)
for test runners and build staging.

## Licence

Original project code is [Apache-2.0](LICENSE). Upstream MeshCore and other
dependencies retain their own licences and notices.
