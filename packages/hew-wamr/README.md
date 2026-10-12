# WebAssembly for Hew

Load a portable WebAssembly module, call numeric exports and copy bytes to or
from its linear memory. `hew.wamr` owns the module bytes, instance and execution
environment, and bounds each call with instruction fuel and active time.

The standalone native package currently supports **Linux x86-64**. It includes
WAMR 2.4.1 sources and the upstream native-call assembly; consumers need a C11/
C++17 compiler and Hew with native `.s`/`.S` package-source support. It builds
through `[native]` without MeshCore, PlatformIO, CMake or a prepared archive.
Native assembly package support is currently proposed in
[Hew PR #3689](https://github.com/hew-lang/hew/pull/3689); use a compiler with
that capability and its matching runtime and standard library.

Add a path dependency, then run `hew install --offline`:

```toml
[dependencies]
"hew.wamr" = { path = "../hew-wamr" }
```

```hew
import hew.wamr;
import std.fs;
fn main() {
    let module = fs.read_bytes("arithmetic.wasm").expect("read module");
    let vm = wamr.open(module, wamr.default_limits()).expect("open module");
    var args: Vec<wamr.Value> = Vec.new();
    args.push(wamr.Value.I32(20));
    args.push(wamr.Value.I32(22));
    let result = vm.call("add", args).expect("call add");
    match result[0] {
        wamr.Value.I32(value) => println(f"{value}"),
        _ => panic("expected i32"),
    }
}
```

Build with `hew build main.hew --release -o demo`. The complete
[consumer example](examples/consumer/main.hew) uses the module built from
[tests/fixture.c](tests/fixture.c). `python3 tests/run.py` builds copied,
independent O0/O2 consumers, generates modules with Clang's Wasm target and runs
the example. To build the example module directly:

```sh
cd examples/consumer
clang --target=wasm32 -O1 -nostdlib -fno-builtin \
  -mno-bulk-memory -mno-reference-types -mno-multivalue \
  -Wl,--no-entry -Wl,--strip-all -Wl,--initial-memory=65536 \
  -Wl,--max-memory=65536 -Wl,-z,stack-size=4096 \
  ../../tests/fixture.c -o fixture.wasm
hew install --offline
hew build main.hew --release -o demo
./demo
```

Set `HEW` to a compatible compiler executable if needed; it should
use its matching runtime and standard library.

`open(bytes, limits)` copies and retains the module bytes. An owned `Instance`
closes automatically at scope exit, or explicitly with consuming `close()`.
Each instance has independent memory and globals. `call(name, values)` accepts
up to eight `I32`, `I64`, `F32` or `F64` arguments and returns up to four numeric
results. Counts and exact types must match the export. Missing exports, traps,
bounds failures and budget exhaustion return errors. Calls after a trap may
continue; guest writes made before a trap remain in that instance.

`read(offset, size)` returns a fresh byte copy; `write(offset, bytes)` copies into
the instance. Offsets and lengths are checked without truncation, including
empty ranges at the end of memory. `memory_size()` reports the current bytes.
Modules may grow memory up to their declared maximum within `memory_pages`.

Default per-instance limits are 64 KiB source, one 64 KiB memory page, an 8 KiB
interpreter stack, 100,000 instructions and 100 ms active time per call. Set
`Limits.memory_pages` for separately compiled modules with larger declared
memory; the supported ceiling is 256 pages. The module must declare an explicit
maximum. The bounded profile permits up to 16 exports, with export names of at
most 63 bytes. `instructions()` reports the most recent call's opcode count.
Loading/validation/instantiation are cooperative work bounded by source and
pool capacity; the execution time limit does not preempt them.

One process-wide WAMR pool holds runtime metadata, loaded modules and instance
metadata. Linux linear memory uses separately page-bounded mappings, and the
owned source-byte copy is separately bounded by `source_bytes`; the pool is not
a total instance-memory cap. Its default size is 1 MiB. `configure_pool(bytes)` can select
128 KiB..64 MiB before first use; the selection is immutable and changing it
returns an error. Repeating the same selection is allowed. Larger instances or
more simultaneous instances need a suitable pool. Closing instances releases
their allocations; the shared pool remains reserved until process exit.

The initial standalone profile permits **no imports**, tables, start section or
implicit initialization exports (`_start`, `_initialize`, `__post_instantiate`,
`__wasm_call_ctors`). It has no WASI, file, network, thread or callback API.
Prepare data with memory writes, call an explicit initialization export if your
module needs one, and exchange results through numeric calls and memory copies.

`cancel()` permanently retires execution while allowing memory inspection until
close. Hew calls are synchronous, so cancellation from the same Hew owner affects
later calls. The native C API permits another thread to set the owned atomic
cancel flag while keeping the instance alive; it must not close the instance
concurrently. Other native operations require one owner at a time.

The shared backend also serves Willow and Aspen's command and packet Wasm
engines. Native C/C++ adapters may register at most two explicitly allowed
import namespaces with process-lifetime symbol storage. They select the shared
pool allocator before initialization. MeshCore adapters retain their 1 MiB
PSRAM pool, exact one-page profile, imports, permissions, async guest protocol,
packet accounting and event-epoch cancellation. They use the same interpreter,
profile parser, runtime lock, registration and per-opcode meter as this package.
Native imports are cooperative and need their own work limits.

Package code is Apache-2.0. WAMR and its source notices are retained under
[Apache-2.0 with the LLVM exception](native/vendor/LICENSE). The public Hew byte
ABI header is distributed under [MIT](native/HEW-MIT.txt).
[NOTICE](NOTICE) records the pinned source and interpreter/platform changes.
