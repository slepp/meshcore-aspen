#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Prepare the pinned source-only Lua interpreter in a disposable build tree."""
import argparse
import json
from pathlib import Path
import shutil
from prepare import check_target

CORE = """lapi lcode lctype ldebug ldo ldump lfunc lgc llex lmem lobject
lopcodes lparser lstate lstring ltable ltm lundump lvm lzio lauxlib lstrlib""".split()


def generate(target: Path, c_stack_limit: int = 40):
    root = Path(__file__).resolve().parents[2]
    if target.resolve().parent != (root / "firmware/nrf52840/.build").resolve():
        check_target(root / ".tmp/onchip-upstream", target)
    stage_lua(target, c_stack_limit=c_stack_limit)


def stage_lua(target: Path, *, c_stack_limit: int = 40):
    """Stage the common parser hooks after the caller verifies its build tree."""
    dest = target / "lib/OnchipLua"
    dest.mkdir(parents=True, exist_ok=True)
    for stamp in dest.glob(".staged-*"):
        stamp.unlink()
    package = Path(__file__).resolve().parents[2] / "packages/hew-lua/native"
    for path in (package / "lua").glob("*.h"):
        shutil.copy2(path, dest / path.name)
    for stem in CORE:
        shutil.copy2(package / "lua" / (stem + ".c"), dest / (stem + ".c"))
    for name in ("hlua_core.hpp", "hlua_core.cpp"):
        shutil.copy2(package / name, dest / name)
    config = dest / "luaconf.h"
    config.write_text(config.read_text() +
                      f"\n#undef HLUA_C_STACK_LIMIT\n#define HLUA_C_STACK_LIMIT {c_stack_limit}\n")
    (dest / "library.json").write_text(json.dumps({
        "name": "OnchipLua", "version": "5.5.1",
        "build": {"srcDir": ".", "includeDir": ".",
                  "srcFilter": ["+<" + stem + ".c>" for stem in CORE] + ["+<hlua_core.cpp>"]}
    }, indent=2) + "\n")
    shutil.copy2(package / "lua/LUA-README", dest / "LUA-README")
    shutil.copy2(package / "lua/LICENSE-LUA.txt", dest / "LICENSE-LUA.txt")
    (dest / f".staged-{c_stack_limit}").write_text("Lua 5.5.1\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--target", required=True, type=Path)
    parser.add_argument("--c-stack-limit", type=int, choices=(32, 40), default=40)
    args = parser.parse_args()
    generate(args.target, args.c_stack_limit)
