# Embedding engines in Hew

Run Lua or WebAssembly in a native Hew program with configurable execution limits.
Both packages include their engine sources and build through Hew's native
package support. Install the engine you need in your Hew project.

| Package | Guest code | Host interface |
| --- | --- | --- |
| [hew.lua](hew-lua/README.md) | Lua text | Scalar calls and explicit host requests with resume tokens |
| [hew.wamr](hew-wamr/README.md) | WebAssembly modules | Numeric export calls and checked linear-memory copies |

Copy the package directory into your project, or refer to it in this checkout:

```toml
[dependencies]
"hew.lua" = { path = "vendor/hew-lua" }
```

Run `hew install --offline` in the consuming project, then build normally. Each
package guide gives its compiler requirements, example, execution limits and
supported platforms. Neither package needs manual linker arguments. The packages
are independent; choose the engine your application needs.

Willow and Aspen use the same portable engine cores through their MeshCore
adapters. Radio operations, permissions, packet policies and guest-source
management remain with those adapters. The standalone Hew APIs expose only the
interfaces described in the package guides.
