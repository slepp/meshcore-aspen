#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent
OUT = ROOT / ".tmp/wasm-examples"


def build():
    OUT.mkdir(parents=True, exist_ok=True)
    common = ["--target=wasm32", "-Oz", "-nostdlib", "-fno-builtin",
              "-mno-bulk-memory", "-mno-reference-types", "-mno-multivalue",
              "-Wl,--no-entry", "-Wl,--strip-all", "-Wl,--initial-memory=65536",
              "-Wl,--max-memory=65536", "-Wl,-z,stack-size=4096"]
    commands = []
    for name in ("arithmetic", "notes", "rpc"):
        commands.append(["clang"] + common + [str(HERE / f"examples/{name}.c"), "-o", str(OUT / f"c-{name}.wasm")])
        commands.append(["rustc", "+1.96.0", "--edition=2024", "--target=wasm32-unknown-unknown",
                         "--crate-type=cdylib", "-C", "opt-level=z", "-C", "panic=abort",
                         "-C", "target-feature=-bulk-memory,-reference-types,-multivalue",
                         "-C", "link-arg=--initial-memory=65536", "-C", "link-arg=--max-memory=65536",
                         "-C", "link-arg=-zstack-size=4096", "-C", "link-arg=--strip-all",
                         "--cfg", name, str(HERE / "examples/plugin.rs"), "-o", str(OUT / f"rust-{name}.wasm")])
    for fault in range(10):
        commands.append(["clang"] + common + [f"-DFAULT={fault}", "-fno-optimize-sibling-calls",
            str(HERE / "examples/fault.c"), "-o", str(OUT / f"fault-{fault}.wasm")])
    commands.append(["clang"] + common + [str(HERE / "examples/events.c"), "-o", str(OUT / "c-events.wasm")])
    commands.append(["clang"] + common + [str(HERE / "examples/dispatch.c"), "-o", str(OUT / "c-dispatch.wasm")])
    commands.append(["clang"] + common + [str(HERE / "examples/suspension.c"), "-o", str(OUT / "c-suspension.wasm")])
    commands.append(["clang"] + common + [str(HERE / "examples/rpc_args.c"), "-o", str(OUT / "c-rpc-args.wasm")])
    commands.append(["clang"] + common + [str(HERE / "examples/owner.c"), "-o", str(OUT / "c-owner.wasm")])
    commands.append(["clang"] + common + [str(HERE / "examples/admin.c"), "-o", str(OUT / "c-admin.wasm")])
    for contract in range(10):
        commands.append(["clang"] + common + [f"-DCONTRACT={contract}",
            str(HERE / "examples/contract.c"), "-o", str(OUT / f"contract-{contract}.wasm")])
    for command in commands:
        subprocess.run(command, check=True)
    (OUT / "commands.json").write_text(json.dumps(commands, indent=2) + "\n")
    for path in sorted(OUT.glob("*.wasm")):
        print(f"{path.name}: {path.stat().st_size} bytes")
        if path.stat().st_size > 3800:
            raise ValueError(f"{path.name} exceeds the 4096-byte package envelope after metadata")


if __name__ == "__main__":
    build()
