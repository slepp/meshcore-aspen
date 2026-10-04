# Dependencies and licenses

Original project code uses [Apache-2.0](LICENSE). Dependencies and adapted
upstream code retain their own licenses; see [NOTICE](NOTICE).

## Native firmware and application runtimes

| Component | Source used by the build | Licensing materials |
| --- | --- | --- |
| MeshCore | [`d92964352441e53b93e8667b802e04f6e072b39e`](https://github.com/meshcore-dev/MeshCore/tree/d92964352441e53b93e8667b802e04f6e072b39e), `companion-v1.17.1` | [MIT copyright and permission notice](LICENSES/MeshCore-MIT.txt); retain upstream component notices |
| Lua | [5.5.1](https://www.lua.org/ftp/lua-5.5.1.tar.gz) | MIT; copyright and license in the Lua distribution |
| WAMR | [`b124f70345d712bead5c0c2393acb2dc583511de`](https://github.com/bytecodealliance/wasm-micro-runtime/tree/b124f70345d712bead5c0c2393acb2dc583511de), 2.4.1 | Apache-2.0 with LLVM exception; retain `LICENSE` and component notices |
| ESP32 Arduino | [2.0.17](https://github.com/espressif/arduino-esp32/tree/2.0.17), source revision `dcc1105b0cf1322a437b354c336f2abf72b7e512` | LGPL-2.1 and component-specific licenses |
| ESP-IDF | [4.4.7](https://github.com/espressif/esp-idf/tree/v4.4.7), source revision `38eeba213aa695aabfd6d89aa9f5078dbe5a94c3` | Apache-2.0 and component-specific licenses |
| nRF52 Arduino | [MeshCore's Arduino fork at `d541301665b40959682252911e57b11df3ee651a`](https://github.com/meshcore-dev/Adafruit_nRF52_Arduino/tree/d541301665b40959682252911e57b11df3ee651a) | LGPL-2.1 and component-specific licenses, including Bluefruit and Nordic SDK notices |
| CMSIS | [`a65b7c9a3e6502127fdb80eb288d8cbdf251a6f4`](https://github.com/ARM-software/CMSIS_5/tree/a65b7c9a3e6502127fdb80eb288d8cbdf251a6f4) | Apache-2.0 and component-specific notices |

PlatformIO resolves additional board and radio libraries from the selected
configuration. The generated upstream tree includes their license files.
Binary packages must identify the versions actually linked, rather than
assuming every firmware profile uses the same dependencies.

## Go host

[`go.mod`](go.mod) and [`go.sum`](go.sum) record the module versions and
checksums. The executable links these modules:

| Module | License |
| --- | --- |
| `github.com/meshcore-go/meshcore-go` and its companion/hardware transport modules | MIT |
| `github.com/eclipse/paho.mqtt.golang` | Dual Eclipse Public License 2.0 / Eclipse Distribution License 1.0; see its `LICENSE` and `NOTICE.md` |
| `github.com/mochi-mqtt/server/v2` | MIT |
| `github.com/gorilla/websocket` | BSD-2-Clause |
| `github.com/rs/xid` | MIT |
| `go.bug.st/serial` | BSD-3-Clause |
| `filippo.io/edwards25519` | BSD-3-Clause |
| `golang.org/x/crypto`, `x/net`, `x/sync`, `x/sys` | BSD-3-Clause; retain component notices |

When distributing a host executable, include the linked modules' license and
notice files from the module cache, along with the Go toolchain's license.
Source dependencies are downloaded by Go; they are not vendored here.

## Willow native host

Willow uses Hew **0.6.0-rc7** and native libraries in addition to the shared
Lua/Wasm worker dependencies above. See the
[Willow library and compatibility guide](experiments/hew-roles/LIBRARIES.md)
for the required runtime contracts and checks.

| Component | Source | License |
| --- | --- | --- |
| Hew compiler, runtime and standard library | [hew-lang/hew, `v0.6.0-rc7`](https://github.com/hew-lang/hew/tree/v0.6.0-rc7); runtime and stdlib are linked from `libhew.a` | MIT or Apache-2.0; retain the selected license and component notices |
| OpenSSL / libcrypto | [openssl/openssl](https://github.com/openssl/openssl), system OpenSSL 3 | Apache-2.0 and component notices |
| libsodium | [jedisct1/libsodium](https://github.com/jedisct1/libsodium), system `libsodium.so.23` | ISC |
| libcurl | [curl/curl](https://github.com/curl/curl), system headers and library | curl license (MIT-style); TLS and other enabled dependencies retain their own licenses |
| cJSON | [DaveGamble/cJSON](https://github.com/DaveGamble/cJSON), native worker JSON library | MIT |

The source build uses installed system libraries; it does not vendor them.
When distributing native binaries, record the actual linked versions and
include their licenses and transitive notices, plus those for Hew's linked
runtime dependencies. The native worker input manifest records the libraries
used by that build. Source-only distribution does not include compiler or
system-library binaries.

## Python tools

[`requirements.txt`](requirements.txt) declares the monitor and management
dependencies. Firmware signing also uses
[`requirements-sign.txt`](firmware/esp32/requirements-sign.txt).

| Package | License |
| --- | --- |
| `cryptography` | BSD-3-Clause or Apache-2.0, with bundled-library notices |
| `PyNaCl` | Apache-2.0, with bundled libsodium notices |
| `pyserial` | BSD-3-Clause |

## Distributing firmware

Keep the dependency licenses and copyright notices alongside each image.
For statically linked LGPL components, include the application object files,
replaceable library archives, a working relink recipe and the corresponding
source or exact accessible source locations. Include any modified LGPL
source. Check both the original and replacement-library relink paths against
the application before publishing the package.

The [firmware downloads](https://ve6slp.ca/projects/meshcore/#downloads)
include per-package dependency inventories, notices and relink materials.
Their manifests identify the particular build; they do not describe every
possible source configuration.
