# Configured HTTPS for Lua bots

An ESP32 bot or the production Go `native_lua` bot can run `!weather`,
`!service health` and owner-installed HTTP/RPC commands through an approved
HTTPS service. Local radio, notes and utility commands continue while a
network invocation waits. Start the [reference host service](../../cmd/meshcore-bot-service/README.md),
configure its fixed IP, TLS hostname, CA and bearer token, then enable the
bot's network grant.

Choose the [ESP32 HTTPS image](../esp32/README.md#choose-an-image) or
[Linux native Lua worker](../../HOST_GUIDE.md#host-status) first.
For Lua function signatures and invocation authority, see
[the runtime reference](BOT_RUNTIME.md#approved-http-json-and-rpc);
this guide owns destination configuration and transport behavior.
The [service guide](../../cmd/meshcore-bot-service/README.md) owns the
receiver's operations and deployment.

## Configure a service

ESP32 requires the HTTPS board profile and fresh accepted SNTP time. The native
host requires OpenSSL, the existing cJSON runtime and an NTP-synchronized Linux
system clock. Build it with `make -C firmware/esp32 bot-native-worker`.
It retains the Go-owned bot identity, private state directory and shared radio
connection; HTTPS uses the existing network worker, not another radio scheduler.
On Linux, a plausible wall-clock date is insufficient: the kernel clock must
also report synchronization (`adjtimex` without `STA_UNSYNC`). The native `home`
status command reports `clock=0` until that condition is satisfied.
Native private SPIFFS has 128 KiB aggregate capacity, at most 32 files and a
32 KiB per-file ceiling for configuration records; Lua source remains 4096 bytes.

Use authenticated mast administration for ESP32, or the private native owner
socket (`admin.py --unix-socket /absolute/state/bot/native/admin.sock command ...`).
The ESP32 command prefix is `bot https`; the native owner socket uses `https`.
For example, replacing the IP/hostname/port with your service:

```text
bot https endpoint home 192.0.2.10 service.example 8787 /v1/rpc post
bot https ca home HEX_CHUNK
bot https token home HEX_CHUNK
bot https ops home 7
bot https commit
bot home on
```

On the native socket use `https ...` and `home on`. Append PEM/token bytes
in even-length hex chunks of at most 128 characters; 80-character chunks fit
both administration paths. CA is at most 4096 bytes; token is 32..256 printable
ASCII bytes without whitespace. Replacing an endpoint clears its staged CA and
token. `ops home` is a sum of health=1, echo=2 and weather=4; choose only needed
operations. Changes do not take effect until verified `commit`. `status` reports
counts and epoch, never credentials. Native files remain in the private
state directory; ESP32 double-slot records remain in SPIFFS across restart.
Configuration failures start with `Error:`. Do not enable the grant after a
failed or uncertain commit.
On ESP32, CA/token staging requires encrypted Management RF. Both the HTTP
handler and shared administration dispatcher reject web/Lua staging; the
native host's private owner socket permits provisioning.

To move a configured private Aspen application to a generic build, use
`bot https retain home` followed by `bot https commit`. This stages the initial
home address, CA, token and allowed operations without printing or transporting
them. It refuses an absent initial configuration or an already staged/saved
home endpoint. The saved route takes priority over build defaults and remains
available after a generic application update. This operation requires direct
authenticated administration; it does not enable the bot's network grant.

Stage secrets only through encrypted authenticated RF administration or the
local protected Unix socket. Plaintext web administration rejects CA/token
staging. Do not paste token hex into shell history, log files or source.
TLS always checks CA, hostname and dates; there is no insecure fallback.
The pinned IP avoids DNS rebinding and restricts local-LAN access to the exact
operator-approved destination. Redirects are rejected rather than followed.

## HTTP, JSON and named RPC

Install [`examples/network_api.lua`](examples/network_api.lua) after configuring:

| Native alias | Path and method | Command |
| --- | --- | --- |
| `home_status` | `/v1/example/status get` | `!net_status` |
| `home_echo` | `/v1/example/echo post` | `!net_echo hello` |
| `home_rpc` | `/v1/rpc post`, plus `rpc home_rpc sum /v1/rpc` | `!net_sum 13 29` |
| `home_rpc` | Add `rpc home_rpc digest /v1/rpc`, enable service `-compute` | `!net_digest hello` |

Each endpoint needs its own staged IP/hostname/CA/token and commit. Four
endpoints and eight exact service/operation mappings fit the configuration.
Choose the slots you need; a package endpoint needs one of the four slots.
To keep weather, status, echo and package fetch together, reuse `home` for named
RPC: stage `bot https rpc home sum /v1/rpc` and
`bot https rpc home digest /v1/rpc`, then commit. On the native socket omit `bot`.
Replace `home_rpc` with `home` in the example Lua calls to avoid a fifth endpoint.
The four aliases are then `home`, `home_status`, `home_echo` and `package`;
the named RPC mappings reuse the existing `home` connection destination.
Lua never provides a URL, header, socket or credential.

```lua
local status = http.get("home_status")
local echo = http.post("home_echo", {text = "hello"})
local sum = rpc.call("home_rpc", "sum", {a = 13, b = 29})
local encoded = json.encode({hello = "mesh"})
local decoded = json.decode(encoded)
```

HTTP results have `ok`, `http_status`, `submitted`, and a decoded JSON `body`.
Named RPC results have the same status fields and a decoded `result`.
Failures provide `error.code` and `error.message`. `json.null` preserves JSON
null and `json.array()` represents an empty array. The network grant is
restricted to authenticated private DM invocations, not local/channel input.

There is one HTTPS connection, four Lua network mailboxes and one owner GET
mailbox shared by JSON and package fetch. The native telemetry mailbox remains
separate and uses the same connection. Request waits include queue time and
expire after 15 seconds; unsent expired requests are removed without opening a
socket. Admission is at most two requests per caller and four globally each
minute, plus the invocation's existing eight-I/O budget.
HTTP POST JSON is at most 1024 bytes; responses at most 2048 bytes; headers
total 2048 bytes, a line 511 bytes. Native network JSON parsing accepts at most
four nested levels and 48 structural components. Each decoded Lua JSON string
value or object key is limited to 1024 UTF-8 bytes. A response containing a
larger string returns `ok=false` with `error.code="invalid_response"`, even
when the complete body fits the 2048-byte limit. Shorten or split large string
fields at the service. Lua JSON utilities additionally reject duplicate keys,
cycles, invalid UTF-8, non-finite numbers and oversized values.

Only explicit Content-Length and uncompressed bodies are accepted. Offline,
unconfigured, denied, busy/rate-limited, timeout, TLS, remote and malformed
responses are errors. Cancellation or source/grant/configuration replacement
discards late results. `submitted=true` means the request may have reached the
service, not that its effect was confirmed. Interrupted/post-send failures may
be `unknown`; neither platform retries automatically. ESP32 SDK calls remain
bounded but not preemptible; native connect/handshake takes at most two seconds,
then nonblocking reads/writes check cancellation between attempts.

RPC request IDs contain a native random transport-instance nonce and the
source generation/job/operation token. They do not export the caller key or
persist an in-flight request. A restart does not replay a request or reuse its
identifier. See the service guide for operation-specific ID retention.

## Fetch a package without replacing the running source

Configure exact GET alias `package`, its fixed raw-source path, CA and token;
enable `bot home on` (native: `home on`). Use the expected full source SHA-256:

```sh
make -C firmware/esp32 bot-plugin-fetch PACKAGE_SHA256=LOWERCASE_SHA256 \
  MAST_CLI_ARGS="--unix-socket /absolute/state/bot/native/admin.sock"
```

Use the ordinary authenticated radio/web CLI connection options for ESP32.
The CLI accepts no URL. The endpoint must return HTTP 200, Content-Length
1..4096, and `application/x-lua`, `application/octet-stream` or `text/plain`.
It streams 128-byte chunks into staging and checks the expected SHA-256 before
closing staging. The source manager then checks readback, metadata, runtime/API,
schema rollback compatibility and Lua compilation/initialization through the
same journal and activation lifecycle as RF uploads. A successful network GET
alone cannot activate source. Hash mismatch, interruption or validation failure
leaves the active source/data/keys intact. `source status`, `source hash`,
`source cancel` and `source rollback` use the existing owner backend.
Export the active source/data before filesystem reprovisioning.

## Local validation

```sh
make -C firmware/esp32 bot-network-check
```

This runs bounded Lua/HTTP/configuration checks, ESP32 TLS admission/date/CA-cache
tests, real TLS against the supplied service using the production Linux transport,
and real native-worker streamed fetch/hash failure/cancellation/restart checks.
It also checks the Go attachment's shared-radio and identity behavior.
No hardware is flashed or service restarted. Physical ESP32 mixed-load memory,
socket and RF acceptance remains an operator deployment check.
The public `Xiao_S3_WIO_onchip_https` build for this slice uses 1,744,645 flash
bytes and 137,860 linker RAM bytes; runtime PSRAM/peak TLS measurements are
separate from linker totals.
