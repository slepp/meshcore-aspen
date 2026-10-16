# Run Willow with Docker

`ghcr.io/slepp/meshcore-willow:0.0.12` runs the Willow host, Base companion,
dashboard or MQTT broker on Linux x86-64. The image includes the release's
native Lua/Wasm worker and WebSocket-capable curl library. No Hew compiler
is needed. Each container runs one service; override its command to select
another service.

## Start a new host

Use a MeshCore shared modem with extended MKISS and queued protocol v1.
Read its configured physical profile first; Willow checks that profile and
does not retune the radio. Stop any previous host using the same identities
before starting Willow.

Copy [compose.yaml](../container/compose.yaml) to an installation directory.
From that directory:

```sh
install -d -m 0700 state
export WILLOW_UID="$(id -u)" WILLOW_GID="$(id -g)"
docker compose pull
```

Keep these UID/GID values in a private `.env` file for subsequent Compose
commands. All services must use the same UID and the same `/state` mount;
Willow requires owned private files and checks the owner of its Unix sockets.
The image defaults to UID/GID 1000. Use a nonroot account.

**Initialize only a new node.** This command creates new relay, room and bot
identities and refuses an existing `/state/host`. Use the migration procedure
below for an existing node.

```sh
MODEM_IPV4=192.168.1.50
PROFILE_HEX=$(python3 -c 'import struct; print(struct.pack("<IIBBBfBh", 912525000, 250000, 7, 5, 2, 1.0, 1, 0).hex())')
docker compose run --rm --no-deps host build/hew-host-release init /state/host \
  --address "$MODEM_IPV4" --port 8001 --profile "$PROFILE_HEX"
docker compose run --rm --no-deps host \
  python3 -B tools/willow.py check --state /state/host
docker compose up -d host
docker compose logs --tail=50 host
docker compose exec host python3 -B tools/willow.py health --state /state/host
```

Replace the example address and profile with your modem's values. The profile
expression encodes 912.525 MHz, 250 kHz bandwidth, SF7, CR5, TX 2 dBm, airtime
factor 1 and CAD enabled with threshold 0. Inside a container, `127.0.0.1`
refers to its network namespace, not the Docker host.
Use the radio's LAN address or another reachable modem endpoint.
`ONLINE` and `BOT_READY` in the logs indicate the modem and native worker
started. The health command reports each role's current state.

## Retain an existing node

For Birch, first import its stopped state using [MIGRATION.md](MIGRATION.md).
For Willow, follow [the frozen-state upgrade guide](MIGRATION.md#upgrade-a-frozen-willow-state).
Put that complete stopped state under `state/frozen`, then use this image’s
launcher to copy it into a new `state/host` directory:

```sh
docker compose run --rm --no-deps host python3 -B tools/willow.py rebind \
  --source /state/frozen --state /state/host
docker compose run --rm --no-deps host \
  python3 -B tools/willow.py check --state /state/host
```

Rebinding sets the worker path to `/opt/willow/hosts/willow/build/native-worker`
while retaining identities and data. Start only the new owner with
`docker compose up -d host`; keep the stopped original state for recovery.

## Add the dashboard or broker

Create `state/dashboard.conf` as an owned mode-0600 file:

```ini
address=0.0.0.0
port=9080
state=/state/host
page=/opt/willow/internal/app/admin_page.html
base_port=0
bot_companion_port=0
password_env=MESHCORE_LAB_ADMIN_PASSWORD
```

Set `MESHCORE_LAB_ADMIN_PASSWORD` in the private Compose `.env` file to a
12–256-character printable password without spaces. Then run:

```sh
docker compose --profile dashboard up -d
```

Open `http://127.0.0.1:9080/` on the Docker host. The example publishes only
to host loopback. For LAN access, explicitly change the published address
and use a trusted network or a TLS reverse proxy; dashboard HTTP carries
login credentials. See [SERVICE.md](SERVICE.md#run-the-host-dashboard) for
authentication and health settings.

For the local anonymous MQTT broker, create owned mode-0600
`state/broker.conf` with:

```ini
host=127.0.0.1
port=1883
```

Run `docker compose --profile broker up -d`. Configure the host observer's
MQTT URL as `tcp://127.0.0.1:1883`. The Compose services share the host
container's network namespace, so these loopback endpoints reach each other.
The broker keeps messages and sessions in memory; they clear on restart.
See [BROKER.md](BROKER.md) for authenticated external listeners.

## Add a Base companion

Stage a stopped Base's retained identity and state using [BASE.md](BASE.md).
Put its prepared directory under `state/base` and its owned mode-0600 JSON
configuration at `state/base.json`. Use container paths such as
`"state_dir":"/state/base"`, the modem's reachable address, and
`"status_listen":"127.0.0.1:9081"`.

To use the example's published companion port, configure
`"companion_listen":"0.0.0.0:5000"` and
`"companion_allow_remote":true`. The mapping still binds only to the Docker
host's loopback address. **Companion TCP has no authentication.** Expose it
to a LAN only when that network's clients may control the companion.
Set dashboard `base_port=9081` when this Base is running.

```sh
docker compose --profile base --profile dashboard up -d
```

The image also runs these services independently with normal command overrides:

| Service | Command after the image name |
| --- | --- |
| Host | `python3 -B tools/willow.py run --release --state /state/host` |
| Base | `build/hew-base-release /state/base.json` |
| Dashboard | `build/hew-dashboard-release /state/dashboard.conf` |
| Broker | `build/hew-broker-release /state/broker.conf` |

Mount the same state at the same paths when services share owner sockets.
Supply `MESHCORE_HOST_ADMIN_HTTP=1` and the configured password environment
variable to a standalone dashboard container. Use `docker run --init` for
standalone containers so Docker forwards stop signals and reaps child processes.

## Upgrade and roll back

Stop the containers cleanly and make a private backup of the entire `state`
directory before an upgrade. Keep that backup and the previous image tag.
Change the image tag in Compose, pull it, run the offline `check` command
above, and start the services again. Use `docker compose down` before
recreating the host network namespace and its attached services.

Recreating containers retains the bind-mounted identities, settings, messages
and native program state. Never run `init` over retained state, delete the
state directory, or run two instances against it. If rolling back after the
new version has written state, follow the frozen-state rebind procedure in
[the frozen-state upgrade guide](MIGRATION.md#upgrade-a-frozen-willow-state). Keep backups, credentials and private
keys out of images and public assets.

## Build and publish the image

Download the Willow Linux x86-64 release bundle, copy it to
`hosts/willow/container/willow-bundle.tar.gz`, and build from that directory:

```sh
docker build --platform linux/amd64 --memory 4g --memory-swap 4g --cpu-period 100000 --cpu-quota 200000 \
  --build-arg VERSION=0.0.12 --build-arg SOURCE_REVISION=FULL_SOURCE_COMMIT \
  -t meshcore-willow:0.0.12 hosts/willow/container
```

The build checks the complete bundle before installing it at `/opt/willow`.
Its source, licences, notices and native relinking files remain in the image.
The repository's **Publish Willow container** workflow downloads a published
`willow-vVERSION` bundle and publishes version and `sha-FULL_SOURCE_COMMIT`
tags to GHCR. It also accepts a manual release-tag dispatch.
