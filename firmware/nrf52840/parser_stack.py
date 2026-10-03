#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check the Pine parser bound using its ARM compiler stack-usage files."""
import argparse
import json
from pathlib import Path
import re


def frames(path: Path):
    result = {}
    for line in path.read_text().splitlines():
        declaration, size, kind = line.rsplit("\t", 2)
        name = declaration.split(":", 3)[-1]
        if kind != "static":
            raise ValueError(f"Unbounded stack-usage entry in {path}: {declaration}")
        result[name] = int(size)
    return result


def frame(entries, name, cpp=False):
    matches = [size for key, size in entries.items()
               if (name + "(" in key if cpp else key.split(".", 1)[0] == name)]
    if len(matches) != 1:
        raise ValueError(f"Expected one ARM stack-usage entry for {name}, found {len(matches)}")
    return matches[0]


def measure(build: Path, config: Path, worker_source: Path):
    lua_dirs = list(build.glob("lib*/OnchipLua"))
    if len(lua_dirs) != 1:
        raise ValueError("Expected one compiled OnchipLua library")
    lua = lua_dirs[0]
    parser = frames(lua / "lparser.c.su")
    api = frames(lua / "lapi.c.su")
    auxiliary = frames(lua / "lauxlib.c.su")
    calls = frames(lua / "ldo.c.su")
    vm = frames(build / "examples/nrfmast/onchip/BotVm.cpp.su")
    worker = frames(build / "examples/nrfmast/onchip/BotWorker.cpp.su")
    limits = re.findall(r"^#define LUAI_MAXCCALLS (\d+)$", config.read_text(), re.MULTILINE)
    if not limits:
        raise ValueError("Missing staged Lua C-call limit")
    depth = int(limits[-1])
    stacks = re.findall(
        r'#elif defined\(NRF52_PLATFORM\)\s+if \(xTaskCreate\(entry, "mesh-command", (\d+),',
        worker_source.read_text())
    if len(stacks) != 1:
        raise ValueError("Expected one nRF52 VM task stack reservation")
    stack = int(stacks[0]) * 4  # nRF52840 FreeRTOS uses 32-bit StackType_t words.
    initialize = frame(vm, "onchip::BotSession::Impl::initialize", cpp=True)
    if initialize > 256:
        raise ValueError(f"Compact initializer frame {initialize}B exceeds 256B")

    # Paths between Lua's enterlevel checks; inlined helpers are included in .su frames.
    cycles = {
        "named-function": ("statement", "body", "statlist"),
        "global-function": ("statement", "globalstatfunc", "body", "statlist"),
        "block": ("statement", "block", "statlist"),
        "for-block": ("statement", "forbody", "block", "statlist"),
        "if-block": ("statement", "test_then_block", "block", "statlist"),
        "global-initializer": ("statement", "globalstatfunc", "initglobal"),
        "table-field": ("subexpr", "constructor", "recfield"),
        "call-argument": ("subexpr", "suffixedexp", "funcargs", "explist"),
        "index": ("subexpr", "suffixedexp", "yindex"),
        "anonymous-function": ("subexpr", "body", "statlist"),
        "binary-unary": ("subexpr",),
        "assignment": ("restassign",),
    }
    cycle_bytes = {name: sum(frame(parser, entry) for entry in entries)
                   for name, entries in cycles.items()}
    fixed = (frame(worker, "onchip::BotWorker::entry", cpp=True) +
             frame(worker, "onchip::BotWorker::run", cpp=True) +
             frame(vm, "onchip::BotSession::load", cpp=True) + initialize +
             frame(auxiliary, "luaL_loadbufferx") + frame(api, "lua_load") +
             frame(api, "lua_pcallk") + frame(calls, "luaD_pcall") +
             2 * frame(calls, "luaD_rawrunprotected") +
             frame(calls, "ccall") + frame(calls, "luaD_precall") +
             frame(calls, "precallC") + frame(calls, "luaD_protectedparser") +
             frame(calls, "f_parser") + frame(parser, "luaY_parser"))
    # Upvalue lookup recurses through parent FuncStates without enterlevel.
    lookup = frame(parser, "singlevaraux")
    reserve = 2048  # Lexer/codegen/error/allocator leaf calls and RTOS/FPU context.
    bound = fixed + depth * (max(cycle_bytes.values()) + lookup) + reserve
    if bound > stack:
        raise ValueError(f"Pine parser bound {bound}B exceeds VM task stack {stack}B")
    return {"vm_task_stack_bytes": stack, "initialize_frame_bytes": initialize,
            "parser_c_call_limit": depth, "fixed_parser_frames_bytes": fixed,
            "guarded_parser_cycle_bytes": cycle_bytes,
            "parent_lookup_frame_bytes": lookup, "leaf_and_context_reserve_bytes": reserve,
            "conservative_parser_bound_bytes": bound, "spare_beyond_reserve_bytes": stack - bound,
            "measurement": "ARM .su-derived bound, not a physical task high-water measurement"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", required=True, type=Path)
    parser.add_argument("--lua-config", required=True, type=Path)
    parser.add_argument("--worker-source", type=Path,
                        default=Path(__file__).resolve().parent.parent / "onchip/BotWorker.cpp")
    args = parser.parse_args()
    try:
        result = measure(args.build, args.lua_config, args.worker_source)
    except (OSError, ValueError) as error:
        parser.exit(1, f"Pine parser stack check failed: {error}\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
