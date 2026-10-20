# Host your own shared rooms

Run the shared-room service on your own Cloudflare account. Your Aspen nodes
create, join and convert rooms against it with the same `cloudroom` commands
as the hosted service, and you keep every room key. Setup takes one command
and about ten minutes.

## Hosted service or your own server

| | Hosted (`rooms.ve6slp.ca`) | Your own Worker |
|---|---|---|
| Setup | Request a create code, then run `cloudroom create` | `npm run setup`, then mint your own codes |
| Who can read and speak as the room | The hosted service operator | You |
| Cost | None | Cloudflare plan limits below |
| Room limits | Operator quotas | Yours |
| Node commands | `cloudroom create CODE NAME` | `cloudroom create CODE@HOST NAME` |

The hosted service takes new rooms by invitation. Open a "Hosted room request"
issue on [slepp/meshcore-aspen](https://github.com/slepp/meshcore-aspen/issues/new/choose);
the operator replies with a one-time create code, typically within a few days.
Joining an existing room needs no request: the room's owner sends you an
invite code.

Whichever server holds a room can read it and send as it. The room key lives on
that server, sealed in storage. Choose your own Worker when that server must
be one you operate.

## Deploy the Worker

You need a Cloudflare account, Node 20 or newer, and a checkout of this
repository.

```sh
cd services/shared-room
npm ci
npx wrangler login
npm run setup
```

`setup` asks for a Worker name (default `aspen-rooms`) and an optional custom
domain, then:

1. writes `wrangler.local.jsonc`, a git-ignored copy of `wrangler.jsonc` with
   your name, no project route, `SIGNUP_MODE=code` and Workers Logs off;
2. generates `ROOM_KEK` and an operator token, uploads their secrets to
   Cloudflare through standard input, and saves them under `.deploy/` with
   mode 0600;
3. deploys the Worker and checks `GET /v1/info`;
4. mints one create code and prints the command to use it:

```text
Worker aspen-rooms runs at https://aspen-rooms.example.workers.dev (version 0.1.21, signup code).
Copy .deploy/room-kek.json to your password manager. Without it, hosted rooms cannot be recovered.

Create your first room from the companion app's Management console on an Aspen node:
  cloudroom create ABCD-EFGH-JKMN-PQRS@aspen-rooms.example.workers.dev My Room
```

Pass `--name NAME` and `--route rooms.example.org` to skip the questions. A
custom domain must be a zone on your Cloudflare account. A node accepts a
host of up to 36 bytes in a direct message, so choose a short domain or Worker
name.

Run `npm run setup` again at any time. It redeploys, keeps the existing
`ROOM_KEK` and token, and uploads a secret only when Cloudflare lacks it.

### Back up ROOM_KEK

`.deploy/room-kek.json` holds the key that unseals every hosted room key.
Copy it to your password manager now. If it is lost, the rooms on that Worker
cannot be opened and must be created again. Restore it on a new checkout with:

```sh
npm run setup -- --restore-kek /path/to/room-kek.json
```

To change the key, run `npm run setup -- --rotate-kek`. New rooms use the new
key; existing rooms keep opening with theirs, so the file always keeps every
key it has had. Back it up again after each rotation.

## Create the first room

On an Aspen node whose clock is set, open the companion app's Management
console and send the printed command:

```text
cloudroom create ABCD-EFGH-JKMN-PQRS@aspen-rooms.example.workers.dev My Room
cloudroom result
```

The node contacts your Worker over TLS, the Worker generates the room key, and
the node restarts after ten seconds. The room is then a normal MeshCore room.
Add it in the companion app, then read the password:

```text
cloudroom advertise 1
cloudroom password
```

The [node guide](NATIVE.md) covers every stage: invite more nodes, join a room
from another operator, convert an existing local room, and leave.

## Operate the service

`admin.mjs` reads `.deploy/admin-token` (or the token on standard input with
`--token-stdin`) and talks to the Worker's operator API. The API exists only
when `ADMIN_TOKEN_HASH` is set.

```sh
npm run admin -- mint --count 3 --ttl 86400
npm run admin -- rooms
npm run admin -- disable ROOM_ID
npm run admin -- enable ROOM_ID
npm run admin -- delete ROOM_ID --yes
npm run admin -- codes
npm run admin -- revoke-code HASH_PREFIX
npm run admin -- ban NODE_KEY
npm run admin -- unban NODE_KEY
npm run admin -- invite ROOM_ID
npm run admin -- audit
```

| Command | Use |
|---|---|
| `mint` | Create codes for `cloudroom create` (1 to 20 per call, valid 1 minute to 30 days, default 7 days). Shown once. |
| `rooms` | List rooms with state, owner node and last activity. |
| `disable`, `enable` | Stop a room, or resume it. A disabled room closes its connections and keeps its data for 30 days. |
| `delete` | Remove a room, its history and its key for every node. |
| `codes`, `revoke-code` | List codes by hash prefix, or revoke one. |
| `ban`, `unban` | Block a node key. A ban revokes that node's tokens in every room. |
| `invite` | Mint an owner code (15 minutes) for a room whose only owner node is lost. |
| `audit` | Operator actions, redemptions and revocations. Entries hold node ids, never addresses. |

`npm run doctor` checks the deployment: `/v1/info`, the Worker version against
this checkout, `ROOM_KEK` and `ADMIN_TOKEN_HASH`, the Durable Object migration,
the operator API and Workers Logs.

## Who can create rooms

`SIGNUP_MODE` in `wrangler.local.jsonc` controls room creation:

| Mode | Behaviour |
|---|---|
| `code` (default) | Creating a room needs a code you minted. |
| `closed` | No new rooms. Owner invites still let nodes join existing rooms. |
| `open` | Anyone can create a room without a code. |

Leave the mode on `code` for any Worker reachable from the Internet. `open`
accepts a room from every caller within the per-address and service-wide
limits below, and each room uses your Cloudflare quota. To change the mode,
edit `vars.SIGNUP_MODE` in `wrangler.local.jsonc` and run `npm run setup`.

### Limits and abuse protection

The Worker applies these limits without extra configuration:

| Limit | Value |
|---|---|
| Redeem requests per client address | 10 per minute, 40 per hour |
| Wrong or unknown codes per client address | 5 per minute, 30 per hour |
| Wrong tries on one code | 5, then the code is deleted |
| Rooms created per node | 2 per day, 4 live |
| Open-signup creates | 3 per day per address, 20 per hour in total |
| Rooms on the Worker | 2000 (`MAX_ROOMS`) |
| Nodes per room | 16 (`MAX_FRONTENDS`) |
| Live invite codes per room | 4 |
| Room writes per day | 20000 (`ROOM_WRITES_PER_DAY`); a room that passes it is disabled |

Client addresses are reduced to a network prefix and hashed with a daily key;
those counters expire within 48 hours. Add a
[Cloudflare WAF rate-limiting rule](https://developers.cloudflare.com/waf/rate-limiting-rules/)
on `/v1/node/*` to stop floods before they reach the Worker. A room that no
node has connected to for 180 days is disabled, and one idle for 365 days is
deleted.

## Cloudflare plan limits

Limits as of October 2026. Check the linked pages before relying on them.

| Plan | Worker requests | Durable Object row writes | CPU per request |
|---|---|---|---|
| Workers Free | 100,000 per day | 100,000 per day | 10 ms |
| Workers Paid ($5 per month) | 10 million per month included | 50 million per month included | 30 s default |

See [Workers pricing](https://developers.cloudflare.com/workers/platform/pricing/),
[Durable Objects pricing](https://developers.cloudflare.com/durable-objects/platform/pricing/)
and [Workers limits](https://developers.cloudflare.com/workers/platform/limits/).
A room with a few nodes and light traffic fits the Free plan. A busy room,
many nodes or a long history needs Workers Paid; once the Free daily limit is
reached, requests fail until the limit resets and the room goes quiet.

## Update the Worker

Check out the release that matches your nodes and run `npm ci` and
`npm run setup`. The release notes name the minimum Worker for each firmware
version, and `cloudroom probe HOST` on a node shows the Worker's version.
Rooms, tokens and secrets stay in place.

## Recover from loss

**A node is lost or replaced.** On a remaining owner node, send
`cloudroom nodes` and `cloudroom revoke ID` for the lost node. The new node
joins with `cloudroom invite` and `cloudroom join`. Restoring a backup onto
new hardware copies the node key, so retire the old node first.

**The only owner node is lost.** Mint an owner code and join from the
replacement node:

```sh
npm run admin -- rooms
npm run admin -- invite ROOM_ID
```

```text
cloudroom join WXYZ-0123-4567-89AB@aspen-rooms.example.workers.dev
```

The owner code lasts 15 minutes and works once. The invite appears in the
audit log.

**`ROOM_KEK` is lost.** The sealed room keys cannot be opened. Restore it from
your password manager with `--restore-kek`. Without a copy, delete the rooms
with `admin.mjs delete`, create them again and have members rejoin each new room.

## Use a private certificate authority

A node verifies a Cloudflare `workers.dev` or custom-domain certificate with
its built-in public roots, so a Worker on Cloudflare needs no certificate
setting. A server behind a private certificate authority needs a profile
upload, because a direct message cannot carry a certificate. Put the authority's PEM in the alias `ca` field
and set `ca_mode` to `pem` in a schema 2 profile; see
[the profile format](NATIVE.md#configure-the-generic-image).

## Troubleshooting

| Symptom | Check |
|---|---|
| `Clock not set. Wait for SNTP` | The node needs the time before it can verify TLS. Send `get sntp.server` and confirm the node reaches an NTP server over WiFi. |
| `TLS verify failed` | The host in the command must be the host the certificate names. Run `cloudroom probe HOST`. |
| `server unavailable` | Run `cloudroom probe HOST`; it reports DNS, TLS or HTTP failures. Then `npm run doctor`. |
| `Server is not accepting new rooms` | `SIGNUP_MODE` is `closed`, or `ROOM_KEK` is missing. Run `npm run doctor`. |
| `code not valid` | Codes work once and expire. Mint another and check the `@HOST`. |
| `Server limit reached` | A room, node or code limit above applies. |
| Hosted routes answer 503 | `ROOM_KEK` is not set on the Worker. Run `npm run setup`. |
