# Contributing

This is a hobby project for building and running MeshCore nodes. Bug fixes,
clearer instructions, useful examples and reports from real deployments are
welcome.

Start with the [README](README.md) for supported setups. For a Lua or Wasm
application, use the [application development guide](firmware/runtime/BOT_DEVELOPMENT.md)
or [Wasm SDK guide](firmware/runtime/WASM_RUNTIME.md).

## Work without a radio

Use Go **1.26.7 or newer**, Python **3.13**, GNU make and a C/C++17 compiler
on Linux. Start with a Python virtual environment:

```sh
python3 -m venv .venv
. .venv/bin/activate
python -m pip install --require-hashes -r requirements-linux.lock
make check
```

This runs Python tests, Go race tests, host builds and native C++ smoke checks
without flashing, connecting to a radio or sending RF traffic. Physical-node
checks are opt-in. Extended firmware and browser checks need additional
dependencies, Node.js and Chrome; consult the relevant Makefile and guide.

`requirements-linux.lock` pins the Python 3.13 Linux x86_64 dependencies used
by CI and release checks. For another platform, install from `requirements.txt`.

To replay an application locally:

```sh
make -s -C firmware/esp32 bot-local \
  SOURCE=firmware/runtime/plugins/template.lua \
  SCENARIO=firmware/runtime/plugins/examples/hello.scenario.json
```

This builds the native runtime and may download pinned dependencies. It uses
an in-memory radio and network fixture, not a connected node.

## Source layout

| Directory | Responsibility |
| --- | --- |
| `firmware/esp32`, `firmware/nrf52840` | Board integration and firmware build staging |
| `firmware/runtime` | Shared C++ bot engine, Lua/Wasm bindings, package tools and examples |
| `firmware/shared` | Modem, queue and wire contracts used across builds |
| `firmware/remote-radio/esp32` | ESP32 roles connected to a separate WiFi modem |
| `cmd`, `internal` | Go host services, protocol implementations and checks; the Python chat CLI also lives under `cmd` |
| `tools/hardware` | Operator administration, selected-device checks and update workflows |
| `test_support` | Native seams, protocol fixtures and hardware test runners |

The native Go host worker builds the same C++ bot engine used on the radios.
Python stages firmware sources, drives the existing hardware toolchains and
runs integration checks. The monitor, broker and chat are standalone operator
tools; chat uses the shared standard-library TCP client in `tools/companion.py`.
Lua programs and optional Wasm packages use the runtime's application API.
JavaScript implements the browser administration pages.

## Propose a change

Describe the affected setup, the behavior you want and how someone can
reproduce it. For a bug, include the firmware/profile version, selected roles,
commands and relevant error text. Remove credentials, private keys and
private message contents from reports.

Keep pull requests focused. Add or update a test for changed behavior, use
the smallest existing runner covering the change, and update the related
guide when commands or configuration change. Preserve native protocol
contracts, authorization checks and the distinction between queued,
transmitted, acknowledged and uncertain operations.

Write documentation for someone building, running or extending a node.
Lead with the device, command and expected result. Put wiring and protocol
details in their focused guides; keep development logs and private rollout
notes out of user documentation and commit messages.

Use a short commit subject describing the change. Explain a meaningful
compatibility decision in the body when needed; do not include local
deployment transcripts, machine paths, credentials or private node data.

## Hardware checks

Use a legal local radio profile and an explicitly selected device. Back up
identity and state before destructive operations. Never assume that
`/dev/ttyACM0` is the intended board.

Hardware workflows read device identifiers, addresses and deployment policy
from a private operator inventory. Copy
[`tools/hardware/operator-inventory.json.example`](tools/hardware/operator-inventory.json.example)
outside tracked source, replace its synthetic values with your selected
devices and restrict the file to mode `0600`. Set `MESHCORE_OPERATOR_CONFIG`
to that file. Missing configuration or a mismatched hardware identifier
refuses the operation.

Use the documented public setup and application-update tools for a new node.
Hardware tests that change source, identities, settings or firmware require a
maintenance window and the corresponding private backup.

## Licensing

Contributions are provided under the project's [Apache-2.0 license](LICENSE),
except material retaining an upstream license. Preserve copyright and
license notices when adapting dependencies; see [THIRD_PARTY.md](THIRD_PARTY.md).
No contributor agreement or separate assignment is required.

Report security-sensitive problems privately using [SECURITY.md](SECURITY.md),
not a public issue containing exploit details or node credentials.
