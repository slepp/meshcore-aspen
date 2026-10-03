# MeshCore bot host service (v1)

Run this service to provide health, echo, optional weather and approved
host computation to ESP32 or Go-native Lua bots. It does not connect to the
radio. The bot's native owner configuration supplies the fixed endpoint,
credentials and operation allowlist; Lua supplies only approved aliases and
bounded arguments. Local radio, notes and utility commands do not depend on
this process. After starting it, follow [network setup](../../firmware/runtime/NETWORK_API.md)
to authorize the bot's connection.

## Build and run

From the repository root:

```sh
make -C cmd/meshcore-bot-service test
make -C cmd/meshcore-bot-service build
make -C cmd/meshcore-bot-service run
```

The executable is `cmd/meshcore-bot-service/bin/meshcore-bot-service`. The default listen address is `127.0.0.1:8787` (IPv4 loopback), with weather disabled. To enable the real, public, no-token Open-Meteo geocoding/forecast adapter:

```sh
make -C cmd/meshcore-bot-service run WEATHER=true
```

In another terminal, a loopback service without token configuration accepts:

```sh
curl --fail -sS http://127.0.0.1:8787/v1/rpc -H 'Content-Type: application/json' \
  -d '{"operation":"health","request_id":"h1","args":{}}'
```

Expect `{"ok":true,"result":{"status":"ok"}}`. Health checks this process,
not weather-provider reachability. An ESP32 cannot reach your host's loopback:
use the protected LAN/TLS listener below for the bot.

For a LAN listener, configure a secret and a TLS certificate/key **outside this repository**. Generate a unique token using an operator-approved secret manager or `openssl rand -hex 32`; do not put the token in a Make variable, command line, or tracked file.

```sh
export MESHCORE_BOT_SERVICE_TOKEN="$(openssl rand -hex 32)"
make -C cmd/meshcore-bot-service run LISTEN=192.0.2.10:8787 \
  TLS_CERT=/secure/path/service.crt TLS_KEY=/secure/path/service.key WEATHER=true
```

Replace the example IP and certificate paths with operator-owned values. Non-loopback binding (including wildcard addresses) fails unless both a valid bearer token and certificate/key paths are configured. TLS uses a minimum of TLS 1.2. The listen address must use an IP literal, not a hostname. Loopback may use TLS and/or bearer auth too. If a token is configured, **all** calls require it. Restrict access to secrets; firmware must verify the service certificate identity. A reverse proxy or port-forward that makes loopback reachable remotely needs its own TLS/auth protections.

Flags: `-listen`, `-tls-cert`, `-tls-key`, `-weather`, `-compute`, `-geocode-url`, and `-forecast-url`. The provider endpoints default to `https://geocoding-api.open-meteo.com/v1/search` and `https://api.open-meteo.com/v1/forecast`. Override them only for an operator-controlled fixture; they must use HTTPS, except `http://` with an IP loopback hostname for local test servers. URLs cannot contain credentials, query parameters, or fragments. Provider calls connect directly, ignoring ambient `HTTP_PROXY` and `HTTPS_PROXY`; redirects are never followed. No provider call occurs until weather is enabled and requested. No API key is needed. A weather lookup sends only the supplied place name to geocoding, then coordinates to forecasts; RPC IDs, caller headers, local node identities, and bearer credentials are never forwarded. Responses are not cached.

### Persistent local service

`make -C cmd/meshcore-bot-service service-install SERVICE_DIRECTORY=/private/service LISTEN=192.0.2.10:8787`
installs and enables the ordinary `meshcore-bot-service.service` user unit.
The directory must contain `server.crt`, `server.key`, and a mode-0600
`service.env` defining `MESHCORE_BOT_SERVICE_TOKEN`; the token never enters the
unit or command line. Weather is enabled in this unit. Check with
`make -C cmd/meshcore-bot-service service-status`. The user manager must support lingering if the service
must survive logout. Certificate renewal and retained credentials are operator
responsibilities; restarting the unit is required after replacing its files.

## Frozen HTTP/JSON wire contract

Use `POST /v1/rpc` with `Content-Type: application/json` (optional `charset=utf-8`). With authentication enabled, include `Authorization: Bearer <configured-token>`. The request is one UTF-8 JSON object with `operation`, `request_id`, and `args` and no unknown fields:

```json
{"operation":"echo","request_id":"example-1","args":{"text":"hello"}}
```

Use canonical lowercase JSON field names. Go's JSON decoder also accepts case-insensitive spellings of recognized names, including argument fields; unknown fields are rejected. Success uses HTTP 200 `{"ok":true,"result":{...}}`; failure uses a non-2xx HTTP status `{"ok":false,"error":{"code":"...","message":"..."}}`. Errors never include provider response bodies, token values, request arguments, or request IDs. The service does not echo `request_id`. The ID is required, 1-64 ASCII bytes from `A-Z a-z 0-9 _ - .`. Health, echo, sum and weather are read-only and use it for correlation. Opt-in digest uses it to suppress repeated computation as described below; IDs are never persisted.

| Operation | `args` | HTTP 200 `result` |
| --- | --- | --- |
| `health` | `{}` | `{"status":"ok"}` (process health only, not provider reachability) |
| `echo` | `{"text":"hello"}` | `{"text":"hello"}` |
| `weather` | `{"place":"Fixture City"}` | `{"source":"open-meteo","location":"Fixture City","country":"Testland","temperature_c":12.5,"weather_code":3,"observed_at":"2026-09-27T17:15:00Z","source_age_seconds":900}` |
| `sum` | `{"a":13,"b":29}` | `{"sum":42}`; both integer operands required in -1000000..1000000 |
| `digest` (opt-in) | `{"text":"hello","rounds":100000}` | `{"sha256":"70ef...adcadfe3","rounds":100000}`; iterative SHA-256, text 1..512 bytes, rounds 1..1000000 |

`echo.text` is 1-512 UTF-8 **bytes**, without control characters. `weather.place` is 1-80 UTF-8 bytes, without control characters or leading/trailing whitespace. The provider reports Celsius, a numeric Open-Meteo weather code, a UTC RFC 3339 observation timestamp, and nonnegative whole seconds since observation. Data more than 2 hours old or more than 5 minutes in the future returns `provider_stale`; an observation slightly in the future reports age 0. The first geocoding match is used. Weather is off by default; a disabled call returns `weather_disabled`, never a fabricated forecast.

Example local calls (start the service first):

```sh
curl -sS http://127.0.0.1:8787/v1/rpc -H 'Content-Type: application/json' \
  -d '{"operation":"health","request_id":"h1","args":{}}'
curl -sS http://127.0.0.1:8787/v1/rpc -H 'Content-Type: application/json' \
  -d '{"operation":"echo","request_id":"e1","args":{"text":"hello"}}'
curl -sS http://127.0.0.1:8787/v1/rpc -H 'Content-Type: application/json' \
  -d '{"operation":"weather","request_id":"w1","args":{"place":"Paris"}}'
```

For authenticated LAN calls, use HTTPS with certificate verification and supply the token via a protected client credential store (not in the URL).

### Named endpoint examples

The same reference server provides `GET /v1/example/status` with
`{"service":"meshcore-bot-service","status":"ok"}` and
`POST /v1/example/echo` with `{"text":"hello"}`. Both require the
configured bearer token, if present, and return bounded JSON without an RPC
envelope. The echo endpoint accepts 1..512 UTF-8 bytes of non-control text
and has no durable side effect. `sum` above exercises the named RPC mapping
at `POST /v1/rpc`; the request ID is correlation only, not deduplication.

For a TLS-enabled mast, configure native names `home_status` for the status
path with `get`, `home_echo` for the echo path with `post`, and `home_rpc`
for `/v1/rpc` with `post`; then map `rpc home_rpc sum /v1/rpc`. Use
authenticated `bot https endpoint ...`, `bot https ca NAME HEX`,
`bot https token NAME HEX`, and `bot https commit` as documented in
[mast administration](../../firmware/esp32/MAST_ADMIN.md). Stage the
operator's fixed service IP, certificate hostname, CA and token separately
for each endpoint; no HTTP address or credentials appear in Lua.
If weather, status, echo and package fetch must coexist, the bot's four
endpoint slots cannot also hold a separate `home_rpc`. Map `sum` and `digest`
on `home`, and change their Lua calls from `home_rpc` to `home`; the
[network setup guide](../../firmware/runtime/NETWORK_API.md#http-json-and-named-rpc)
shows that shared-slot configuration.
To run the bundled `!weather` and `!service` commands **without a compiled
credential**, also stage `endpoint home IP HOST PORT /v1/rpc post`, its CA/token,
and `ops home 7` (or a narrower health=1, echo=2, weather=4 mask) before
commit. This runtime `home` takes precedence over a compiled `home` profile.
After `bot home on`, install
[`firmware/runtime/examples/network_api.lua`](../../firmware/runtime/examples/network_api.lua)
as an editable plugin to try `!net_status`, `!net_echo hello` and
`!net_sum 13 29`. The legacy `home` health/echo/weather operations remain
available when configured and granted.

### Opt-in host computation

Run `make -C cmd/meshcore-bot-service run COMPUTE=true` to enable `digest`.
It hashes UTF-8 text once, then hashes the previous 32-byte digest for each
remaining round. It uses the same authenticated `/v1/rpc`, five-second context
and sixteen admitted request slots, not another identity or worker scheduler.
No user-provided program, command, file path or shell expression is executed.
The default is `compute_disabled`; a persistent unit enables it only when
installed explicitly with `COMPUTE=true`.

Try this read-only local example:

```sh
curl -sS http://127.0.0.1:8787/v1/rpc -H 'Content-Type: application/json' \
  -d '{"operation":"digest","request_id":"digest-example-1","args":{"text":"hello","rounds":100000}}'
```

Configure `rpc home_rpc digest /v1/rpc` on the existing approved service and
install `examples/network_api.lua` to run `!net_digest hello` on either platform.
See [network configuration](../../firmware/runtime/NETWORK_API.md).

Repeat the same request ID and semantic arguments to receive the retained
terminal result without recomputing. Changing those arguments gives HTTP 409
`request_id_conflict`; a concurrent duplicate gives 409 `request_in_progress`.
There are at most 128 retained or running IDs, with terminal results retained
ten minutes; new work receives 503 `busy` when the ID budget is full.
Cancellation checks run every 1024 rounds and retain a 408 `cancelled` result,
so a same-ID retry does not restart cancelled work. IDs/results are forgotten
on service restart or expiry. Digest is pure computation, so repeating after
expiry is safe, but this is not an exactly-once side-effect contract.
The bots never automatically replay a request after an uncertain transmission.

### Limits and errors for firmware integration

* Request body: **4096 bytes maximum**, including JSON syntax/whitespace; no compression. Response body: **2048 bytes maximum**, including the envelope. Oversize input returns HTTP 413 `request_too_large`; an unexpectedly oversized encoded response returns HTTP 500 `internal_error`. Every valid `health`, `echo`, and `weather` response fits the limit, including JSON-escaped strings.
* Server: **16 concurrent admitted calls after body reads**; excess completed reads get HTTP 503 `busy`. Header limit **8192 bytes**; header read timeout **10 s**, complete request read timeout **13 s** (both include the TLS handshake), response write timeout **8 s**, idle timeout **20 s**. RPC processing context timeout **5 s**; each provider request has a **2 s** client timeout. Provider response bodies are limited to **16384 bytes**.
* `bad_request` (400): invalid JSON, missing/invalid fields or arguments, or unreadable body. `unsupported_operation` (400): unknown operation. `unauthorized` (401): missing/invalid configured bearer token. `not_found` (404): unknown route. `method_not_allowed` (405): not POST. `unsupported_media_type` (415): not UTF-8 JSON.
* `weather_disabled` (503): operator has not enabled weather. `place_not_found` (404): geocoding omitted results or returned none. `provider_unavailable` (503): network/offline failure or upstream 5xx. `provider_rate_limited` (503): upstream 429. `provider_timeout` (504): provider/request timeout or upstream 408. `provider_interrupted` (503): cancelled provider call. `provider_bad_response` (502): upstream redirect, other unexpected status, invalid/oversize response, or invalid source fields. `provider_stale` (502): observation outside the age window. `internal_error` (500): response encoding/size failure. Error `message` is fixed human-readable text; branch on `code`.

Firmware integration separately owns Wi-Fi/offline handling, cancellation, endpoint and DNS/redirect policy, TLS identity verification, credentials, timeout budget, and approval of named operations. This server does not substitute for those on-chip controls.
