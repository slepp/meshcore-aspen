# Downloadable bot commands

Add a named Lua command file to an Aspen or Willow bot while retaining its
bundled commands and other installed sources. The catalog includes a greeting,
private DM notes, received repeater signal reports and an Aspen repeater monitor.
Each download includes its install commands, requirements and licence notices.

Open the [command library](https://ve6slp.ca/projects/meshcore/extend/library/),
choose a command and download its ZIP. Extract it and follow the included
`README.md` to install the named source, then try the example command.

Use `source install NAME FILE` from the included instructions. It preserves
other named sources; do not use whole-set `install` or `source replace` for
these additive files. Existing custom globals or commands can conflict, and
the entire Lua set must still fit eight entries and 4096 bytes. A bundled main
reference uses one entry. Export a named file before updating it and read back
status after an uncertain installation. Saved notes are scoped to the verified
DM caller; code rollback does not undo stored data.

The signal download reads packet slot 0. Inspect and export an occupied slot
before choosing to replace it. If the sampler is already running there, retain
it and add only the Lua file. Its sample rings start empty after restart or
reload. The monitor download is Aspen-only and requires explicit target ACLs,
configuration and event permission; its polling consumes RF airtime. Installation
retains saved enablement and grants, so an already enabled monitor can start
polling immediately when the source activates. Follow
its linked guide before enabling it.

## Build the command downloads

The library metadata lives in `catalog.json`. Lua sources live in
`firmware/runtime/plugins/`; the signal sampler uses
`firmware/runtime/wasm/examples/repeater_snr.c`. The packager copies these
sources into ZIPs and builds the sampler with Willow's C target.

From the repository root, with Python 3, make, clang and wasm-opt installed:

```sh
python3 command-library/package.py --output .tmp/command-library
```

The output contains one `meshcore-command-ID-vVERSION.zip` per entry,
`catalog.json` with download sizes and hashes, and `SHA256SUMS`. Each ZIP
includes its instructions, code, manifest, licence and notices.

## Test changes

Check the ZIP contents and hashes locally:

```sh
python3 -m unittest discover -s command-library -p 'test_*.py'
```

For native encrypted-packet replays, supply the existing bot runner:

```sh
make -C firmware/esp32 bot-host-runner
BOT_HOST_RUNNER="$(make -s -C firmware/esp32 --eval 'print-runner: ; @echo $(BOT_HOST_RUNNER)' print-runner)" \
  python3 -m unittest discover -s command-library -p 'test_*.py'
```

These replays use the established greeting and per-caller note scenarios plus
an additive source set with bundled commands. They do not transmit RF. The
signal sampler's packet workflow and report checks remain in the focused
[signal guide](../firmware/runtime/REPEATER_SNR.md#local-checks); monitor checks
remain in the [repeater guide](../firmware/runtime/REMOTE_REPEATERS.md).
