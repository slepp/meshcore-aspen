# Build a Linux binary bundle

Build the downloadable Willow bundle on Linux x86-64 with a current Hew
compiler and its matching standard library. The Debian 12 container supplies
the C/C++ libraries and links the emitted Hew objects against an optimized
runtime built from the same Hew source.

Build the container image from the repository root:

```sh
docker build -f release/Dockerfile.willow-debian12 \
  -t willow-debian12 .
```

Build `libhew.a` from your compiler's Hew source on Debian 12, using its
supported Rust toolchain and `cargo build -p hew-lib --profile release-lib`.
The output is `target/release-lib/libhew.a`. Put the matching Hew `build/bin`
launcher on `PATH`, then package the committed Willow source:

```sh
python3 tools/package_willow.py --output /path/to/new/output \
  --baseline-image willow-debian12 \
  --runtime /path/to/debian12/libhew.a \
  --runtime-source /path/to/matching/hew/source
```

The packager stages the tracked source and pinned native dependencies, builds
the worker, emits four optimized Hew objects, and links them in the container.
It builds the shared libcurl selected by `release/willow-runtime.json` with
HTTP, HTTPS, WS and WSS support. `--curl-source /path/to/archive.tar.xz` reuses a
local archive with the same SHA256. Container build steps have no network
access; public dependency downloads happen during staging.

The output is `willow-vVERSION-linux-x86_64.tar.gz`. Its manifest records source,
file hashes and actual ELF library and symbol requirements. The bundled
libcurl is found relative to each service executable; trusted TLS certificate
authorities come from the installed operating system.

Extract the bundle and run the installed-service scenario from a source tree
with the normal Willow test prerequisites:

```sh
python3 hosts/willow/tests/binary_bundle.py --bundle /path/to/extracted/bundle
```

It installs into two new prefixes, initializes private state, refuses a modified
worker, and retains identities, notes and room history through upgrade and
rollback. The existing combined scenario runs the host, Base, dashboard and
broker together with loopback modem emulators. It does not use a radio.

## Source, notices and relinking

The bundle carries tracked project source in `source.tar.gz`, the original
curl archive in `curl-source.tar.xz`, and Hew licences and dependency notices
under `licenses/hew/`. `dependency-notices.tar.gz` contains system-library
notices and the native dependencies' source with their copyright notices.

`native-relink.tar.gz` contains the native worker's dependency source, C/C++
objects and captured compiler commands, plus the four Hew application objects,
matching `libhew.a`, bridge objects and `relink-commands.json`. Extract these
beside the project source to rebuild or relink. The captured commands use the
container's `/src` and `/runtime/libhew.a` paths; substitute your extracted
source directory and runtime archive, or mount them at those paths in the
build container. Replace the captured curl `-L/src/hosts/willow/build/curl/install/lib`
and `-lcurl` arguments with `-Lbuild/lib -l:libcurl.so.4` from the recorded
`hosts/willow` working directory to
link the shipped library, or rebuild it from `curl-source.tar.xz` with the
configuration in `tools/build_willow_curl.py`. Keep the executable's `$ORIGIN/lib` runtime search path and
ship the matching shared library under `hosts/willow/build/lib/`.
