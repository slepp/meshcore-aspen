# Lua for Hew

Embed Lua 5.5.1 in a native Hew program, pass scalar values and handle requests
from Lua in your host application. Each session owns its interpreter and closes
when it leaves scope. The package includes the interpreter sources; install
Hew, a C11/C++17 compiler and the platform math library to build it.

Use Hew with its matching runtime and standard library. When building Hew from
source, run `make hew` and put its `build/bin` directory on `PATH`; this builds
the compiler and optimized native runtime together.

Add a path dependency and import `hew.lua`:

```toml
[dependencies]
"hew.lua" = { path = "../hew-lua" }
```

```hew
import hew.lua;
fn main() {
    let vm = lua.open(lua.default_limits()).expect("open Lua");
    vm.load("function add(a,b) return a+b end".to_bytes()).expect("load Lua");
    var args: Vec<lua.Value> = Vec.new();
    args.push(lua.Value.Integer(20));
    args.push(lua.Value.Integer(22));
    let result = vm.call("add", args).expect("call Lua");
    match result {
        lua.Step.Complete(values) => {
            match values[0] {
                lua.Value.Integer(value) => println(f"{value}"),
                _ => panic("expected an integer"),
            }
        }
        lua.Step.Host(_) => panic("unexpected host operation"),
    }
}
```

Run `hew install --offline`, then `hew build main.hew --release -o demo` in your
consumer directory. The compiler builds the package's native sources through
`[native]`; no manual linker arguments are needed. A complete example is in
[examples/consumer](examples/consumer/main.hew). Run `python3 tests/run.py` here
for independent O0/O2 consumers and that example.

A `Session` closes at scope exit or with consuming `close()`. Use one thread at a
time. `load(bytes)` permits one text-source load attempt; functions and globals persist
between calls. `call(name, values)` returns `Complete(values)` or `Host(request)`.
Values are `Nil`, `Bool`, 64-bit `Integer`, 64-bit `Number` and `Bytes`. Lua strings
cross as copied bytes, including embedded NULs. Arguments/results are limited to
eight scalars; tables, functions and userdata are rejected at this boundary.

Lua can call `host(operation, ...)` to suspend and return a copied request. The
Hew caller decides which operations to implement and authorize, then supplies up
to eight values with `resume(request.token, values)`. No host callbacks or work
run implicitly. `cancel()` discards the pending continuation while keeping loaded
globals. An old token cannot resume cancelled or later work. Closing a session
also releases suspended work. A failed invocation can be followed by another
call; a failed load should be followed by a new session.

`Limits` bounds the Lua heap, source/value bytes, parser steps, instructions,
active microseconds and host calls. Defaults are a 1 MiB Lua heap, 64 KiB source,
16 KiB per scalar string, 262,144 parser steps, 100,000 instructions, 100 ms
active execution and 32 host calls. Parser, instruction and active-time counts
restart for each load/call and accumulate across its host resumes. Time waiting
for the host is excluded. Host argument copies are separately bounded by eight <!-- copy-check: allow vague-state -- States the allocation limit and accounting boundary. -->
strings of at most `value_bytes`; Hew-owned returned copies are caller-owned.
`peak_bytes()`, `instructions()` and `parser_steps()` report the current session
heap peak and most recent execution counts.

The library surface contains base conveniences, math, string and table functions.
It excludes protected calls, metatable access/finalizers, dynamic loaders, coroutine controls, debug, file,
OS and package libraries. Lua/parser hooks enforce execution limits; native
library operations are cooperative and may finish after the active-time limit,
which is checked before accepting their result. Host operations run outside Lua
and need their own deadlines. This is an embedding API, not process isolation. <!-- copy-check: allow contrast -- The execution limits do not isolate native host operations. -->

The portable heap, parser/instruction dispatch, state construction and interpreter
sources also serve Willow and Aspen's Lua engines. Their adapters retain MeshCore
imports, permission checks, phase limits, radio accounting, coroutine jobs and
platform allocation policy. Their command/packet APIs and source selections are
independent of this standalone scalar API.

Package code is Apache-2.0. Lua's MIT licence is in
[native/lua/LICENSE-LUA.txt](native/lua/LICENSE-LUA.txt). The bundled public Hew byte ABI
header is available under MIT; see [native/HEW-MIT.txt](native/HEW-MIT.txt).
[NOTICE](NOTICE) records the source versions and local interpreter changes.
