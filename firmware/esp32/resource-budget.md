# Device flash and memory budgets

Use an ESP32-S3 with **8 MiB flash and PSRAM** for the combined radio.
Choose `ONCHIP_BOT_WASM=0` for a Lua-only bot.
In the recorded full HTTPS/admin comparison, that removes **93,344 BIN bytes**;
the Wasm-enabled image is 1,915,024 bytes, leaving 1,427,312 in its app slot.
On nRF52, choose [production Lua](../nrf52840/PRODUCTION-LUA.md) or the smaller
native-note/BLE image for your workload. The production Lua guide gives its
runtime limits; use `mem` and `bot memory` to check the selected image.

This reference preserves a historical profile comparison made before the
initial source snapshot. These numbers have not been recalculated for the
current tree. For a release's image sizes, read its manifest. All quantities are
**bytes**, unless stated otherwise.
[Machine-readable results](../../test_support/resource_budget/measurements.json)
contain sections, binary/ELF hashes, dependency hashes and compiler-derived
allocation sizes.

## What the existing profiles include

| ESP32 PlatformIO environment | Linked application surface |
| --- | --- |
| `Xiao_S3_WIO_kiss_wifi` | Shared modem and WiFi/KISS/dashboard |
| `Xiao_S3_WIO_onchip` | Shared modem, WiFi/KISS/dashboard and native Repeater, Room, Companion and Observer |
| `Xiao_S3_WIO_onchip_bot`, Wasm `0` | Above plus command bot, Lua 5.5.1, native bot APIs and durable storage |
| `Xiao_S3_WIO_onchip_bot`, Wasm `1` | Same Lua bot plus WAMR interpreter and Wasm native API bridge |
| `Xiao_S3_WIO_onchip_beta` | Lua bot plus mast RF/web administration and source validation/deployment |
| `Xiao_S3_WIO_onchip_https`, Wasm `0` / `1` | Beta plus native HTTPS/RPC/telemetry transport, with optional WAMR |
| `Xiao_S3_WIO_onchip_https_probe` | HTTPS plus `ONCHIP_BOT_HTTPS_METRICS` and an operator header for instrumentation |

`nativeRoleMask` selects **only Repeater=1, Room=2, Companion=4,
Observer=8**. Bot and Admin selection are independent. A mask of `0` or `4`
does not make a smaller BIN: these native roles are compiled together and the
mask is persisted/runtime state. It changes which native role aggregates start,
not which role functions occupy flash. Disabled roles retain their static
wrappers and shared services. Identities and durable files remain.

Bot profiles include Lua; enabling Wasm adds its interpreter alongside it.
Select the native profile for roles without a command bot, or the WiFi KISS
profile for a shared modem with applications on a separate host.
The combined ESP32 companion accepts app connections over TCP.

Compile-time settings still matter: this public comparison uses an empty MQTT
URI, so the Observer identity/status code is present but the MQTT publisher
startup is optimized away. Empty operator/trusted keys and absent native HTTPS
endpoints likewise describe an unprovisioned public build, not a deployable
configured service. Runtime disabling and compile-time constant elimination
are different mechanisms.

## Runtime diagnostics

Use `stats memory`, `stats psram`, `stats vm` and `stats system` through
Management to inspect the running image rather than extrapolating these
historical build sizes. The eight-entry diagnostics queue has 1,296 bytes of
fixed record storage in PSRAM (internal-heap fallback if PSRAM allocation
fails), an internal FreeRTOS control block and the existing 4 KiB internal
worker stack. Its syslog formatter uses a fixed 256-byte worker buffer.
Raw lwIP UDP sends temporarily allocate one PCB and a packet buffer; they
consume no BSD socket slots and create no additional task. See
[remote logs and memory checks](MAST_ADMIN.md#remote-logs-and-memory-checks)
for commands and counter meanings.

## Recorded ESP32 comparison

Every source-size row below comes from the same historical source and
configuration. The JSON retains that comparison's build identifier; it is
not a revision required by the current builder. MeshCore:
`d92964352441e53b93e8667b802e04f6e072b39e` (`companion-v1.17.1`).
All rows use Seeed XIAO ESP32-S3/Wio SX1262 and the same 8-MiB partition table.
Native rows use 350 contacts, 40 channels, 20 room clients, 16 neighbours,
32 unsynced posts, 12 KISS request slots and two companion TCP clients.
The modem-only row has no contact/channel/companion service despite carrying
the same contact/channel capacity definitions. Public inputs are
`resource-build`, `build-only`, `not-a-secret`, empty MQTT/room password/keys,
and build-only native/mast administrator passwords.

PlatformIO Core 6.1.19; espressif32 6.11.0; Xtensa GCC
8.4.0+2021r2-patch5; Arduino package
`3.20017.241212+sha.dcc1105b` (Arduino 2.0.17 / ESP-IDF 4.4.7).
Optimization is `-Os`, C++17, section garbage collection; no compiler LTO flag.
The native profile is also built as C++17 to avoid a language-mode confounder.
The WiFi KISS profile is normalized to four TCP clients, no local sources,
the same RF settings, base64 dependency and C++17.
`SOURCE_DATE_EPOCH=1790798400` fixes build clock/date inputs.
The archive hashes for Lua 5.5.1 and WAMR revision
`b124f70345d712bead5c0c2393acb2dc583511de` are recorded in the JSON.

| Existing profile | Complete BIN | ELF allocated LOAD bytes | DRAM `.data` | DRAM `.bss` | IRAM¹ | App-slot free |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| WiFi KISS modem, no on-device roles | 1,148,944 | 1,148,826 | 91,528 | 75,528 | 74,691 | 2,193,392 |
| Native, no command bot | 1,236,768 | 1,236,654 | 37,892 | 97,840 | 74,751 | 2,105,568 |
| Lua bot, Wasm `0` | 1,495,936 | 1,495,822 | 38,212 | 101,112 | 74,791 | 1,846,400 |
| Lua bot, Wasm `1` | 1,600,048 | 1,599,934 | 38,328 | 101,192 | 74,791 | 1,742,288 |
| Beta/admin, Wasm `0` | 1,648,160 | 1,648,038 | 38,216 | 101,688 | 74,791 | 1,694,176 |
| HTTPS/admin, Wasm `0` | 1,821,680 | 1,821,566 | 38,292 | 99,600 | 74,791 | 1,520,656 |
| HTTPS/admin, Wasm `1` | 1,915,024 | 1,914,898 | 38,404 | 99,664 | 74,791 | 1,427,312 |

**Full-service compile baseline:** the last two rows both use
`Xiao_S3_WIO_onchip_https`, with native roles, command bot, Lua, mast
administration, source deployment, HTTPS and telemetry compiled. Source,
toolchain, dependency hashes, public profile, epoch, capacities, C++ options
and partitions match. Effective compiler definitions differ only in
`ONCHIP_BOT_WASM=0` versus `1`; preparation also includes/excludes WAMR and its
bridge accordingly. These rows do not enable the probe's extra metrics.
The earlier bot-only Wasm pair omits admin/HTTPS and is a separate comparison.

¹ IRAM includes vectors/text and the one-byte non-load IRAM datum. It is
internal instruction RAM, **not** available PSRAM. No static external-RAM
section is allocated in these images; the PSRAM allocations below are dynamic.
RTC loads total 41 bytes, with 16 bytes of RTC no-init state. ELF dummy/alias
sections are address-space reservations, not additional physical allocations.
In particular, do not add `.dram0.dummy`, `.flash_rodata_dummy` or the nRF
`.heap` arena to static application RAM.

**Exact paired-build deltas**, not exclusive component sizes:

| Change | BIN delta | Allocated LOAD delta | `.data` delta | `.bss` delta |
| --- | ---: | ---: | ---: | ---: |
| WiFi KISS modem → Native combined | +87,824 | +87,828 | −53,636 | +22,312 |
| Native → Lua bot | +259,168 | +259,168 | +320 | +3,272 |
| Lua bot Wasm `0` → `1` | +104,112 | +104,112 | +116 | +80 |
| Lua bot → Beta/admin | +152,224 | +152,216 | +4 | +576 |
| Beta/admin → HTTPS/admin | +173,520 | +173,528 | +76 | −2,088 |
| Full HTTPS/admin Wasm `0` → `1` | **+93,344** | **+93,332** | **+112** | **+64** |

Native → bot also changes the supported local-source capacity from four to
five and dashboard identity slots from six to seven, links native bot APIs,
storage and additional SDK/library code. Its delta is **not pure Lua cost**.
Beta/admin likewise brings web/source-management and shared dependencies;
the following HTTPS delta includes shared transport/SDK code, not an exclusive
TLS library budget. HTTPS also changes KISS TCP clients from four to three
to retain the SDK's 16-socket budget. Those are existing profile contracts.
The HTTPS
`.bss` decrease is not evidence that HTTPS uses less runtime memory.
Modem → native also changes the radio-dispatch preparation and moves the
dashboard from static internal RAM to dynamic PSRAM. The modem's smaller
image therefore has **more static DRAM** than native combined
(167,056 versus 135,732 bytes). The 87,824-byte image delta is not a sum of
exclusive per-role code savings, and the static decrease does not include
the native image's new PSRAM requests.
The full HTTPS pair's 93,344-byte increase differs from the bot-only
104,112-byte increase because their shared linked surfaces differ.
Neither number is an SDK-independent interpreter size.

### Actual flash layout

Each OTA application slot is **3,342,336** bytes: `app0` at `0x10000`,
`app1` at `0x340000`. The inactive slot is OTA reserve, not extra space for
the running app. SPIFFS is **1,572,864** bytes at `0x670000`, NVS is
20,480 bytes at `0x9000`, OTA metadata 8,192 bytes at `0xe000`,
and coredump 65,536 bytes at `0x7f0000`.

Bootloader is a separate 15,104-byte artifact at `0x0`; the partition artifact
is 3,072 bytes at `0x8000`. Neither is included in the application BIN.
Remaining gaps/alignment are layout space, not feature cost. SPIFFS content
was not built here and is not part of these BINs.
BIN headers/checksum/hash/alignment exceed allocated ELF LOAD bytes by
114 bytes (118 in the modem row, 122 in the admin row, 126 in full HTTPS/Wasm).
Compare the same
measure on both sides.

### Retained linker-map attribution

These are **estimates of retained input placement**, not independently
removable flash budgets. The parser uses only the allocated output ranges,
counts overlapping merged strings/constants once and puts conflicting owners
in shared attribution. SDK/ROM/library dependencies and linker relaxation can
move costs between rows. The exact deltas above include those effects.

| Map category | Full HTTPS/admin/Wasm `0` LOAD bytes | Full HTTPS/admin/Wasm `1` LOAD bytes |
| --- | ---: | ---: |
| MeshCore/radio/crypto and shared native adapters | 200,905 | 200,901 |
| WiFi/TLS/web/admin selected objects | 502,053 | 503,104 |
| Repeater wrapper/native application | 24,635 | 24,743 |
| Room wrapper/native application | 17,684 | 17,444 |
| Companion wrapper/native application | 33,021 | 33,113 |
| Observer identity/status, MQTT URI empty | 1,896 | 1,904 |
| Shared bot/native APIs | 152,212 | 154,032 |
| Lua interpreter | 73,369 | 73,372 |
| WAMR and Wasm bridge | 0 | 89,421 |
| Storage/identity/profile/timer/reminder objects | 51,915 | 51,975 |
| SDK/Arduino/newlib/other shared objects | 751,161 | 751,806 |
| Cross-category merged ranges | 34 | 34 |
| Alignment/linker-generated/unattributed remainder | 12,681 | 13,049 |

The difference between 89,421 map-attributed Wasm LOAD bytes and the exact
93,332 paired-build LOAD increase is shared dependencies/placement, not missing
bytes to charge to each role. Native role rows exclude shared MeshCore and crypto;
summing a role's wrapper and all shared libraries for every role would count
the same code repeatedly. SDK/Arduino objects also contain networking and
storage support; the named networking category is not all networking code.
Symbols are retained in the local artifacts. `nm` letters alone cannot
identify SRAM: a flash `.rodata` symbol can be classified `D`. Use its section.

## ESP32 RAM: placement and lifecycle

The following sizes are **compiler/DWARF-derived requested allocations** in
this fixed build, not observed heap peaks. Allocator overhead, SDK allocations
and task control blocks are additional. Static DRAM above remains allocated
when a runtime role is off.

| Component | PSRAM request and lifetime | Internal RAM consequence |
| --- | --- | --- |
| Modem-only dashboard | No dynamic PSRAM request | 68,376-byte static dashboard, already included in modem ELF DRAM |
| Dashboard | 72,376 native / 73,016 bot / 72,920 HTTPS; allocated at setup, retained | HTTP task/SDK/socket state is separate |
| Repeater / Room / Companion | 18,472 / 23,696 / 82,600; allocated only when started, released by generated role stop | Static radio wrappers, role control and shared mux remain |
| Management core | 2,792 without mast admin / 5,752 with it; independent of native mask | Management/static state remains |
| Command bot core | 55,256 while enabled; released by stop | Bot-off startup skips this core and its ordinary workers |
| Bot worker copied state | 80,392 Lua / 80,408 bot-only Wasm / 126,368 HTTPS / 126,384 HTTPS+Wasm per worker | Control 108 for bot-only / 140 for HTTPS; task synchronization stays internal |
| Retained Lua session native metadata | **98,808 per state**, separate from Lua allocator | No internal fallback for this aggregate |
| Lua allocator | Up to **98,304 per state**, allocated as needed, freed by `lua_close`; not a preallocated 96-KiB pool | No internal fallback |
| Wasm native session | **52,136 per loaded session**, outside WAMR pool; freed by clear | Small C++ session handle/locking is separate |
| Wasm linear memory | **65,536 per instance**, mapped separately in PSRAM outside the pool; mapping requests eight additional alignment/header bytes; released on deinstantiate | No internal fallback |
| WAMR runtime pool | **1,048,576**, lazy at first valid Wasm load, retained after module removal/worker stop | Small global pool pointer/lock; pool is not internal SRAM |
| KV cache/readback/redo | 19,020 file banks + 3,184 journal + 392 record = **22,596**, lazy, retained until store destruction | NVS/SPIFFS SDK allocations separate |
| Timer / reminder caches | 2,972 / 3,996, lazy; restore adds temporary/lazy workspaces | Durable records remain in flash after caches are freed |
| HTTPS workspaces | 3,585 request workspace; one 4,120 CA-date cache; telemetry workspace 7,920 | TLS contexts and record buffers are internal, not PSRAM |

The Companion allocation includes contacts/channels, mesh tables and its
packet pool: do **not** add those again. The three native aggregates total
124,768 bytes if all three start; a companion-only boot requests only 82,600
of that total. The static companion-session bridge (18,472 bytes), Observer
(5,648 in bot images) and mux (48,240 bot / 45,020 HTTPS) are already counted
in ELF static RAM; do not add them to heap use.

Lua ready/idle normally retains **active and recovery/diagnostic states**.
Each carries its own 98,808-byte native aggregate and separately metered Lua
heap. Four job records, copied events/I/O/results and duplicated manifests
are embedded in that aggregate; their slots are not extra allocations per
invocation. A `BotManifest` is 8,643 bytes; copies are already included.
Working jobs add Lua coroutine/value allocations **within the state's
96-KiB cap**, not four independent 96-KiB heaps.

Source replacement can retain the old active and diagnostic states while a
candidate is initialized. Mast source validation creates another worker with
its own copied state, 16-KiB stack and candidate session, but no storage task
when it has no bot identity. Its validated candidate can coexist with the
live worker's staged candidate until publication/activation retires them.
There is therefore no safe single-heap multiplier for every load phase.
Stopping validators releases their state/tasks; the WAMR global pool remains.
Disabling bot events, home access or shared-state grants only fences work,
not session memory.
An HTTPS image can keep a native network-only worker with the bot disabled;
bot-off is not a promise that every HTTPS worker disappears.

WAMR's **8,192-byte interpreter execution stack and runtime/module metadata
are inside the 1-MiB pool**. `wasmPoolHighWaterBytes` is a subset of the
reserved pool, not an additional allocation. The **65,536-byte linear memory
is outside it**: in this pinned WAMR, `wasm_allocate_linear_memory` uses
`wasm_mmap_linear_memory` and the prepared ESP-IDF `os_mmap` requests PSRAM
directly. The reported pool high-water/`peakBytes` does not include that
mapping or the 52,136-byte native session. Lua states are separate too.
The pool starts only after package/profile
validation reaches runtime initialization; an empty/no-Wasm workload pays no
pool allocation. Initialization failure frees the pool; successful
initialization leaves it reserved even if subsequent module load/init fails.

ESP FreeRTOS task stack requests are bytes: command VM 16,384, storage 6,144,
HTTPS worker 16,384 when present, diagnostics 4,096 plus 1,288 queue payload
bytes and FreeRTOS overhead. These are **internal RAM**, not Lua/WAMR heaps.
An enabled/configured MQTT Observer adds an 8,192-byte task, SDK MQTT buffers
and eight event queue slots; the empty-MQTT public build does not start them.
WiFi, radio/DMA, HTTP, sockets, filesystem caches, SDK tasks and queue control
blocks still require additional runtime memory.

Existing on-device HTTPS observations in [TLS admission](TELEMETRY.md#a-write-fails-without-an-http-response)
reported two internal 16,720-byte records plus a 2,208-byte context. They
are portions of one handshake, not a second reserve to add to the
68-KiB admission work budget. That budget plus 32-KiB radio/network reserve
is an admission policy, **not a measured worst-case peak**.
Historical free 111,948 / later minimum 18,096 samples are not an attributable
TLS pair. Existing transport diagnostics expose before/connected/after
internal free, largest blocks and global minima; VM diagnostics expose
last invocation heap peak, internal/PSRAM free and VM task stack high-water.

| Physical observation for the recorded comparison | Result |
| --- | --- |
| Boot/disabled/ready/idle/load/working internal free, minimum and largest block | **Unmeasured** |
| Same phases: PSRAM free, minimum and largest block | **Unmeasured** |
| VM/storage/HTTPS/observer/HTTP/radio stack high-water | **Unmeasured**; last VM stack field exists, other tasks lack a complete sample |
| WAMR reserved/used/high-water after unload | Retention verified in code; **current-image used/high-water unmeasured** |

## Pine / nRF52 contrast

The historical comparison built `nrfmast_fleet`: native Repeater plus
DM-only native bot and compiled BLE, enabled/configured separately at runtime.
This profile has eight contacts, one native connection and BLE DFU off.
It is a different application, not the ESP32 combined-role image.

| Pine native/BLE result | Bytes |
| --- | ---: |
| Complete application BIN / ELF application LOAD | 383,144 / 383,144 |
| `.text` / `.ARM.exidx` / `.data` | 381,852 / 8 / 1,284 |
| `.bss` / total static RAM | 48,160 / 49,444 |
| Linker heap arena / main stack reserve | 186,076 / 2,048 |
| Application flash limit / free | 811,008 / 427,864 |
| Linker application RAM region | 237,568 |
| UF2 container | 766,464 |

Nordic platform 11.0.0, Arm GCC 7.2.1 (20170904), framework upstream
`d541301665b40959682252911e57b11df3ee651a`. Flash application region is `[0x27000, 0xed000)`;
SoftDevice/MBR are below it, InternalFS/bootloader and settings above.
The app BIN does not contain those regions. There is no ESP-style dual
OTA slot or PSRAM here. The heap arena is available to FreeRTOS tasks,
BLE and application allocations, **not measured free heap**.
This build's device minimum/largest/stack values are unmeasured.

Production Lua has a different runtime surface; see
[its supported profile and limits](../nrf52840/PRODUCTION-LUA.md).
Measure the selected image rather than treating the native/BLE table as its
heap budget. `nrfmast_lua_probe` is an interpreter resource probe, not a
production Lua image. There is no nRF52 Wasm profile.

## Reproduce without changing a radio

Use a clean checkout. The helper verifies firmware/build inputs against
`HEAD` by default; `--baseline REVISION` selects another revision available
in that checkout. It records the resolved source revision and refuses
modified or untracked firmware inputs. These commands produce measurements
for the selected source, not a recreation of the historical table.
The helper uses a dedicated build
tree; it never prepares an operator's upstream working directory. Existing
dependency caches are required. Archive SHA mismatches fail before preparation.
No credentials/environment files are required.

```sh
python3 test_support/resource_budget/build.py \
  --upstream PATH_TO_PINNED_UPSTREAM \
  --libdeps PATH_TO_CACHED_ESP_LIBDEPS \
  --lua-archive PATH_TO_LUA_5_5_1_ARCHIVE \
  --wamr-archive PATH_TO_PINNED_WAMR_ARCHIVE

python3 test_support/resource_budget/inspect.py \
  .tmp/resource-artifacts/https-wasm-firmware.elf \
  --prefix "$HOME/.platformio/packages/toolchain-xtensa-esp32s3/bin/xtensa-esp32s3-elf-" \
  --map .tmp/resource-artifacts/https-wasm-firmware.map \
  --bin .tmp/resource-artifacts/https-wasm-firmware.bin \
  --partitions .tmp/resource-artifacts/https-wasm-partitions.bin

mkdir -p .tmp
TMPDIR="$PWD/.tmp" make -C firmware/nrf52840 prepare SOURCE=PATH_TO_PINNED_UPSTREAM
# Supply the already-cached nrfmast_fleet libdeps in this isolated build.
TMPDIR="$PWD/.tmp" pio run -d firmware/nrf52840/.build/upstream \
  -e nrfmast_fleet -j 2 -t buildprog -t create_uf2
"$HOME/.platformio/packages/toolchain-gccarmnoneeabi/bin/arm-none-eabi-objcopy" \
  -O binary firmware/nrf52840/.build/upstream/.pio/build/nrfmast_fleet/firmware.elf \
  firmware/nrf52840/.build/upstream/.pio/build/nrfmast_fleet/firmware.bin
make -C firmware/nrf52840 size ENV=nrfmast_fleet

PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover \
  -s test_support/resource_budget -v
```

ESP builds run serially with two compiler jobs. The saved artifacts include
BIN, ELF, map, bootloader, partitions, public profile, compilation database,
build log and input hashes in `.tmp/resource-artifacts/`. The inspect helper
needs the matching compiler's `objdump`, `nm` and `readelf`. Comparison tables
use actual LOAD sections, not ELF file length or UF2 transport size.
Changing dependency hashes/configuration makes a new baseline; do not silently
reuse the measured deltas.
For only the full HTTPS comparison, add `--profiles https https-wasm`
to the build command. Both use `Xiao_S3_WIO_onchip_https`; only the prepared
Wasm dependency/runtime selection and `ONCHIP_BOT_WASM` differ.

## Decisions

* **ESP32 flash:** the recorded full HTTPS/Wasm image leaves
  1,427,312 bytes in each app slot. Read the selected release's manifest for
  its current headroom. Runtime role-mask changes cannot improve it.
* **ESP32 internal SRAM: prioritize TLS/stack/concurrency headroom**, not
  interpreter flash cuts. Keep the existing admission/reserve checks; collect
  internal minimum and largest block under the operator's simultaneous load
  before declaring it safe.
* **ESP32 PSRAM: Wasm is already optional.** Its lazy, retained 1-MiB pool is
  the clearest large lifetime cost. Use the existing Wasm-free build if it is
  unused. Multiple 98,808-byte native Lua session records, their independent
  Lua heaps, candidate validation and HTTPS copied buffers deserve budget
  attention, but these measurements alone do not justify new feature gates.
* **Pine: measure the selected production Lua image**, including idle,
  validation/replacement, jobs and BLE/RF/storage load, sampled minimum free
  heap, largest block where available and task stack headroom. The historical
  native/BLE image's 186,076-byte linker arena is not a production Lua measurement.
