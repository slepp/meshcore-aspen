# MeshCore Willow

Run a relay, room and Lua/Wasm command bot on a Linux computer connected to a
WiFi shared modem. Add a Base companion for the MeshCore app, a browser
dashboard and an MQTT broker. An optional observer publishes received radio
packets to your broker.

Download the [Willow 0.0.11 Linux bundle](https://github.com/slepp/meshcore-aspen/releases/tag/willow-v0.0.11)
to install without a compiler. It includes all four services, the program
worker, dashboard page, migration tools and source. Start below, or
[build from source](#build-from-source).

For Docker, use `ghcr.io/slepp/meshcore-willow:0.0.11` with the
[container setup and Compose example](docs/CONTAINERS.md). The same image runs
the host, Base, dashboard or broker with private persistent state.

## Install the Linux bundle

Install the runtime libraries and Python helpers. On Debian 12:

```sh
sudo apt-get install python3 python3-cryptography python3-nacl \
  libssl3 libcjson1 libsodium23 libstdc++6 zlib1g ca-certificates
```

Download `willow-v0.0.11-linux-x86_64.tar.gz` from the release above, extract it,
and select a new versioned installation directory:

```sh
tar -xzf willow-v0.0.11-linux-x86_64.tar.gz
cd willow-v0.0.11-linux-x86_64
python3 install-willow.py --prefix "$HOME/.local/lib/willow/0.0.11"
cd "$HOME/.local/lib/willow/0.0.11/hosts/willow"
```

The installer checks bundle files and never replaces an existing installation
or changes private node state. You can move a complete binary installation to
another prefix; its host verifies the bundled worker before use.
The bundle includes libcurl with HTTP, HTTPS, WS and WSS support and uses the
system's trusted CA certificates for TLS connections.

For a new node, set the modem's IPv4 address and the hexadecimal profile from
[the initialization guide](docs/SERVICE.md#start-with-new-identities), then run:

```sh
./build/hew-host-release init "$HOME/.local/state/willow" \
  --address "$MODEM_IPV4" --port 8001 --profile "$PROFILE_HEX"
python3 -B tools/willow.py check --state "$HOME/.local/state/willow"
python3 -B tools/willow.py run --release --state "$HOME/.local/state/willow"
```

The check reads configuration and verifies the worker without connecting to the
modem. Startup reports `ONLINE` and `BOT_READY native-extension`. The modem owns
radio tuning; Willow checks its profile without retuning it. Use a short private
state path so its Unix socket names fit the operating system's limit.

For an existing node, use [stopped-state migration and rollback](docs/MIGRATION.md)
instead of `init`. Never start two services with the same identity. The
[service updater](../../tools/INSTALL_WILLOW.md) accepts an extracted binary
bundle as its source, so an upgrade can retain the latest state without a compiler.

Run Base, the dashboard and the MQTT broker separately using their guides in
[docs/BASE.md](docs/BASE.md), [docs/SERVICE.md](docs/SERVICE.md#run-the-host-dashboard)
and [docs/BROKER.md](docs/BROKER.md). Their executables are under `build/`.

## Use your node

Send `!ping` to the bot from a MeshCore app. Use the running host's
[owner DM and inbox commands](docs/SERVICE.md#send-a-dm-and-read-the-owners-inbox)
to send as the bot and read authenticated incoming DMs.

For the browser dashboard, follow [the host dashboard setup](docs/SERVICE.md#run-the-host-dashboard).
It defaults to `http://127.0.0.1:9080/`; set `address=0.0.0.0` in its private
configuration for LAN access at `http://HOST_LAN_ADDRESS:9080/`. `/admin` opens
the same dashboard. View live role and repeater signal status without logging
in; connect under Administration to change settings or manage programs.
`/status` shows public state and `/readyz` reports readiness. Restarting only the
dashboard leaves the host roles, identities and companion listeners running.

To track repeater SNR and RSSI, [install the packet sampler and commands](docs/PACKET_GUESTS.md).
The bot replies, dashboard and signal metrics share its report; saved programs
and settings remain after restart, while signal sample rings start empty.
Signal rows combine matching first-byte prefixes by default. Expand a row to
see individual prefixes; means are weighted by sample count and grouped P95
shows the range of the individual P95 values.

## Choose the services you need

| Task | Service and setup |
| --- | --- |
| Forward mesh traffic | The host's relay supports flood and direct forwarding, loop policies and up to eight permitted region keys. Configure its [role settings](docs/MANAGEMENT.md) and [named regions](docs/SERVICE.md#configure-a-roles-named-regions). |
| Host a room | The host's room accepts up to 20 members and keeps 32 posts. Password, administrator and ACL access are available; public access is opt-in. See [room configuration](docs/SERVICE.md). |
| Run commands and packet samplers | The default bot worker runs Lua and Wasm with saved settings and private data. See [program setup](docs/SERVICE.md) and [signal sampling](docs/PACKET_GUESTS.md). |
| Connect the MeshCore app | Run [Base](docs/BASE.md) with its own identity and companion TCP endpoint for chats, contacts and channels. |
| View roles and manage the bot | Run the [browser dashboard](docs/SERVICE.md#run-the-host-dashboard). Keep administration on localhost or behind a trusted TLS tunnel. |
| Publish received packets | Enable the [MQTT observer](docs/SERVICE.md), then connect it to your broker or run the separate [Willow broker](docs/BROKER.md). The observer checkpoints pending publications and resumes them after a restart. |

The modem connection uses numeric IPv4 over TCP. The host needs three logical
ports, or four with the observer; Base opens a separate connection. Check the
modem's connection capacity before adding services. Willow leaves radio tuning
with the modem and pauses when its profile differs from the configured profile.

Keep the state directory private: it contains identities, credentials and
message data without encryption at rest. Use [migration and rollback](docs/MIGRATION.md)
when changing installations. Saved role settings, ACLs, paths and bot data
survive restart. Active sessions, pending delivery ACKs and running commands
start fresh; room history follows its configured retention mode.

For a small bot without Lua or Wasm, `worker=-` selects the built-in Hew bot.
Its command list and note limits are in the
[bot mode reference](docs/LIBRARIES.md#service-implementation-and-protocol-limits).
Use the default worker for installed programs and the full native command set.

## Build from source

Install the [build dependencies and matching Hew toolchain](docs/LIBRARIES.md#build-toolchain),
then run:

```sh
git clone https://github.com/slepp/meshcore-aspen.git
cd meshcore-aspen
make -j2 willow-check
cd hosts/willow
python3 -B tools/willow.py --version
```

Expected: `Willow 0.0.11`. Use the initialization, check and run commands above,
or migrate an existing stopped node. `make willow-check` builds the services
and checks them with local modem fixtures. `make willow-build` builds them
without running those checks. The launcher defaults to four scheduler workers;
set `HEW_WORKERS` to choose another count.

To extend Willow, see the [source layout and interfaces](docs/LIBRARIES.md#source-layout-and-interfaces),
[development commands](docs/LIBRARIES.md#development-commands) and
[service checks](docs/LIBRARIES.md#service-checks). The
[Linux bundle guide](docs/BINARY_RELEASE.md) covers packaging and relinking.
